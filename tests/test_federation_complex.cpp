#include "L3KVG/Engine.hpp"
#include "L3KVG/Query.hpp"
#include "L3KVG/FederationID.hpp"
#include "L3KVG/QueryResult.hpp"
#include "L3KVG/KeyBuilder.hpp"
#include "engine/store.hpp"
#include <filesystem>
#include <gtest/gtest.h>
#include <string>
#include <future>
#include <vector>
#include <thread>
#include <atomic>
#include <set>
#include <zmq.hpp>
#include <zmq_addon.hpp>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

struct MockServerStats {
    std::atomic<int> r_requests{0};
};

struct PeerConfig {
    std::string name;
    uint16_t id;
    std::string endpoint;
};

void run_mock_cluster_flexible(uint16_t port, uint16_t cluster_id, const std::string& db_path, MockServerStats& stats, std::atomic<bool>& stop_signal, std::vector<PeerConfig> peers, bool populate_default = true) {
    try {
        if (std::filesystem::exists(db_path)) {
            std::filesystem::remove_all(db_path);
        }
        auto engine = std::make_unique<l3kvg::Engine>(db_path, 1);
        engine->get_resolver().register_local_cluster("this_cluster", cluster_id);
        
        for (const auto& peer : peers) {
            engine->get_resolver().register_federation(peer.name, peer.id, {peer.endpoint});
            engine->get_remote_client().add_peer(peer.id, peer.endpoint);
        }

        if (populate_default) {
            for (int i = 1; i <= 10; ++i) {
                std::string id_str = "node_b" + std::to_string(i);
                uint64_t node_b_id = engine->get_resolver().parse_uuid(id_str);
                json node_data = {
                    {"id", id_str},
                    {"name", "Name" + std::to_string(i)},
                    {"age", 20 + i},
                    {"status", "active"}
                };
                lite3cpp::Buffer buf = lite3cpp::lite3_json::from_json_string(node_data.dump());
                engine->put_node(node_b_id, std::string(reinterpret_cast<const char*>(buf.data()), buf.size()));
            }
        }

        zmq::context_t ctx(1);
        zmq::socket_t sock(ctx, ZMQ_ROUTER);
        std::string zmq_endpoint = "tcp://127.0.0.1:" + std::to_string(port);
        sock.bind(zmq_endpoint);
        printf("[Mock %d] Listening on %s\n", cluster_id, zmq_endpoint.c_str()); fflush(stdout);

        while (!stop_signal) {
            std::vector<zmq::message_t> recv_msgs;
            auto result = zmq::recv_multipart(sock, std::back_inserter(recv_msgs), zmq::recv_flags::dontwait);
            if (!result) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }

            if (recv_msgs.size() < 4) continue;

            auto& identity = recv_msgs[0];
            auto opcode = recv_msgs[3].to_string();

            if (opcode == "R") {
                if (recv_msgs.size() < 6) continue;
                stats.r_requests++;
                std::vector<uint64_t> nodes = json::parse(recv_msgs[4].to_string());
                std::string query_json = recv_msgs[5].to_string();

                auto results = engine->query().resume(nodes, query_json).execute();

                json j_res = json::array();
                for (const auto& row : results) {
                    json jr = json::object();
                    for (const auto& [k, v] : row.fields) jr[k] = v;
                    j_res.push_back(jr);
                }

                std::string resp_json = j_res.dump();
                sock.send(identity, zmq::send_flags::sndmore);
                sock.send(zmq::message_t(), zmq::send_flags::sndmore);
                sock.send(zmq::message_t(resp_json.data(), resp_json.size()), zmq::send_flags::none);
            } else if (opcode == "S") {
                if (recv_msgs.size() < 7) continue;
                std::string key = recv_msgs[4].to_string();
                std::string payload = recv_msgs[5].to_string();
                engine->get_store()->put(key, payload);
                sock.send(identity, zmq::send_flags::sndmore);
                sock.send(zmq::message_t(), zmq::send_flags::sndmore);
                sock.send(zmq::message_t("OK", 2), zmq::send_flags::none);
            } else if (opcode == "P") {
                if (recv_msgs.size() < 6) continue;
                std::string key = recv_msgs[4].to_string();
                std::string payload = recv_msgs[5].to_string();
                engine->get_store()->put(key, payload);
                sock.send(identity, zmq::send_flags::sndmore);
                sock.send(zmq::message_t(), zmq::send_flags::sndmore);
                sock.send(zmq::message_t("OK", 2), zmq::send_flags::none);
            } else if (opcode == "E") {
                if (recv_msgs.size() < 8) continue;
                try {
                    uint64_t src = std::stoull(recv_msgs[4].to_string(), nullptr, 16);
                    std::string label = recv_msgs[5].to_string();
                    double weight = std::stod(recv_msgs[6].to_string());
                    uint64_t dst = std::stoull(recv_msgs[7].to_string(), nullptr, 16);
                    engine->add_edge(src, label, weight, dst);
                } catch (...) {}
                sock.send(identity, zmq::send_flags::sndmore);
                sock.send(zmq::message_t(), zmq::send_flags::sndmore);
                sock.send(zmq::message_t("OK", 2), zmq::send_flags::none);
            }
        }
    } catch (const std::exception& e) {
        printf("[Mock %d] CRITICAL ERROR: %s\n", cluster_id, e.what()); fflush(stdout);
    }
}

void run_mock_cluster(uint16_t port, uint16_t cluster_id, const std::string& db_path, MockServerStats& stats, std::atomic<bool>& stop_signal) {
    run_mock_cluster_flexible(port, cluster_id, db_path, stats, stop_signal, {});
}

TEST(FederationComplexTest, MultiNodeBatching) {
    uint16_t remote_port = 5558;
    uint16_t remote_cluster_id = 200;
    std::string remote_db = "test_remote_db_complex";
    std::string local_db = "test_local_db_complex";

    MockServerStats stats;
    std::atomic<bool> stop_signal{false};

    std::thread remote_thread(run_mock_cluster, remote_port, remote_cluster_id, remote_db, std::ref(stats), std::ref(stop_signal));
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    if (std::filesystem::exists(local_db)) {
        std::filesystem::remove_all(local_db);
    }
    
    auto engine = std::make_unique<l3kvg::Engine>(local_db, 1);
    engine->get_resolver().register_local_cluster("local", 1);
    engine->get_resolver().register_federation("remote", remote_cluster_id, {"tcp://127.0.0.1:" + std::to_string(remote_port)});
    
    engine->get_remote_client().add_peer(remote_cluster_id, "tcp://127.0.0.1:" + std::to_string(remote_port));

    uint64_t node_a_id = engine->get_resolver().parse_uuid("node_a");
    engine->put_node(node_a_id, R"json({"id":"node_a"})json");

    for (int i = 1; i <= 10; ++i) {
        std::string remote_id = "remote:node_b" + std::to_string(i);
        uint64_t node_b_id = engine->get_resolver().parse_uuid(remote_id);
        engine->add_edge(node_a_id, "link", 1.0, node_b_id);
    }

    auto results = engine->query()
                         .match("a")
                         .where_eq("a", "id", "node_a")
                         .out("link")
                         .as("b")
                         .return_("b", "id")
                         .execute();

    EXPECT_EQ(results.size(), 10);
    EXPECT_EQ(stats.r_requests.load(), 1);

    stop_signal = true;
    remote_thread.join();
    
    std::filesystem::remove_all(local_db);
    std::filesystem::remove_all(remote_db);
}

TEST(FederationComplexTest, MultiProjectionConsistency) {
    uint16_t remote_port = 5559;
    uint16_t remote_cluster_id = 201;
    std::string remote_db = "test_remote_db_proj";
    std::string local_db = "test_local_db_proj";

    MockServerStats stats;
    std::atomic<bool> stop_signal{false};

    std::thread remote_thread(run_mock_cluster, remote_port, remote_cluster_id, remote_db, std::ref(stats), std::ref(stop_signal));
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    if (std::filesystem::exists(local_db)) {
        std::filesystem::remove_all(local_db);
    }
    
    auto engine = std::make_unique<l3kvg::Engine>(local_db, 1);
    engine->get_resolver().register_local_cluster("local", 1);
    engine->get_resolver().register_federation("remote", remote_cluster_id, {"tcp://127.0.0.1:" + std::to_string(remote_port)});
    
    engine->get_remote_client().add_peer(remote_cluster_id, "tcp://127.0.0.1:" + std::to_string(remote_port));

    uint64_t node_a_id = engine->get_resolver().parse_uuid("node_a");
    engine->put_node(node_a_id, R"json({"id":"node_a"})json");

    uint64_t node_b_id = engine->get_resolver().parse_uuid("remote:node_b1");
    engine->add_edge(node_a_id, "link", 1.0, node_b_id);

    auto results = engine->query()
                         .match("a")
                         .where_eq("a", "id", "node_a")
                         .out("link")
                         .as("b")
                         .return_("b", "name")
                         .return_("b", "age")
                         .return_("b", "status")
                         .execute();

    EXPECT_EQ(results.size(), 1);
    if (results.size() > 0) {
        EXPECT_EQ(results[0].fields.at("b.name"), "Name1");
        EXPECT_EQ(results[0].fields.at("b.age"), "21");
        EXPECT_EQ(results[0].fields.at("b.status"), "active");
    }

    stop_signal = true;
    remote_thread.join();
    
    std::filesystem::remove_all(local_db);
    std::filesystem::remove_all(remote_db);
}

TEST(FederationComplexTest, RecursiveMultiHop) {
    uint16_t port2 = 5560; // Cluster 2
    uint16_t id2 = 202;
    uint16_t port3 = 5561; // Cluster 3
    uint16_t id3 = 203;

    std::string db2 = "test_db_cluster2";
    std::string db3 = "test_db_cluster3";
    std::string db1 = "test_db_cluster1";

    MockServerStats stats2, stats3;
    std::atomic<bool> stop_signal{false};

    std::thread thread3(run_mock_cluster_flexible, port3, id3, db3, std::ref(stats3), std::ref(stop_signal), std::vector<PeerConfig>{}, false);
    std::thread thread2(run_mock_cluster_flexible, port2, id2, db2, std::ref(stats2), std::ref(stop_signal), 
                        std::vector<PeerConfig>{{"cluster3", id3, "tcp://127.0.0.1:" + std::to_string(port3)}}, false);

    std::this_thread::sleep_for(std::chrono::milliseconds(1000));

    if (std::filesystem::exists(db1)) {
        std::filesystem::remove_all(db1);
    }
    
    auto engine1 = std::make_unique<l3kvg::Engine>(db1, 1);
    engine1->get_resolver().register_local_cluster("cluster1", 1);
    engine1->get_resolver().register_federation("cluster2", id2, {"tcp://127.0.0.1:" + std::to_string(port2)});
    engine1->get_resolver().register_federation("cluster3", id3, {"tcp://127.0.0.1:" + std::to_string(port3)});
    engine1->get_remote_client().add_peer(id2, "tcp://127.0.0.1:" + std::to_string(port2));
    engine1->get_remote_client().add_peer(id3, "tcp://127.0.0.1:" + std::to_string(port3));

    uint64_t id_a = engine1->get_resolver().parse_uuid("node_a");
    engine1->put_node(id_a, R"json({"id":"node_a"})json");

    uint64_t id_b = engine1->get_resolver().parse_uuid("cluster2:node_b");
    uint64_t id_c = engine1->get_resolver().parse_uuid("cluster3:node_c");

    std::string key_b = std::string(l3kvg::KeyBuilder::node_key(id_b));
    std::string key_c = std::string(l3kvg::KeyBuilder::node_key(id_c));

    zmq::context_t ctx(1);
    auto send_cmd = [&](uint16_t port, const std::string& opcode, const std::vector<std::string>& args) {
        zmq::socket_t client(ctx, ZMQ_DEALER);
        client.connect("tcp://127.0.0.1:" + std::to_string(port));
        client.send(zmq::message_t(), zmq::send_flags::sndmore);
        uint32_t dummy_uid = 0;
        client.send(zmq::message_t(&dummy_uid, 4), zmq::send_flags::sndmore);
        client.send(zmq::message_t(opcode.data(), opcode.size()), zmq::send_flags::sndmore);
        for (size_t i = 0; i < args.size(); ++i) {
            client.send(zmq::message_t(args[i].data(), args[i].size()), (i == args.size() - 1) ? zmq::send_flags::none : zmq::send_flags::sndmore);
        }
        zmq::message_t reply_del, reply_msg;
        (void)client.recv(reply_del); 
        (void)client.recv(reply_msg);
    };

    try {
        json node_c_data = {{"id", "node_c"}, {"val", "Success"}};
        lite3cpp::Buffer buf_c = lite3cpp::lite3_json::from_json_string(node_c_data.dump());
        send_cmd(port3, "P", {key_c, std::string(reinterpret_cast<const char*>(buf_c.data()), buf_c.size())});

        json node_b_data = {{"id", "node_b"}};
        lite3cpp::Buffer buf_b = lite3cpp::lite3_json::from_json_string(node_b_data.dump());
        send_cmd(port2, "P", {key_b, std::string(reinterpret_cast<const char*>(buf_b.data()), buf_b.size())});
        
        char b_id_buf[17], c_id_buf[17];
        std::snprintf(b_id_buf, sizeof(b_id_buf), "%016llx", (unsigned long long)id_b);
        std::snprintf(c_id_buf, sizeof(c_id_buf), "%016llx", (unsigned long long)id_c);
        send_cmd(port2, "E", {b_id_buf, "link", "1.0", c_id_buf});

        engine1->add_edge(id_a, "link", 1.0, id_b);

        auto results = engine1->query()
                              .match("a")
                              .where_eq("a", "id", "node_a")
                              .out("link").as("b")
                              .out("link")
                              .as("target")
                              .return_("target", "val")
                              .execute();

        EXPECT_EQ(results.size(), 1);
        if (results.size() > 0) {
            EXPECT_EQ(results[0].fields.at("target.val"), "Success");
        }
    } catch (const std::exception& e) {
        printf("[C1] ERROR: %s\n", e.what()); fflush(stdout);
    }

    stop_signal = true;
    thread2.join();
    thread3.join();

    std::filesystem::remove_all(db1);
    std::filesystem::remove_all(db2);
    std::filesystem::remove_all(db3);
}

int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
