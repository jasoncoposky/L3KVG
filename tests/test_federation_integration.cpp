#include "L3KVG/Engine.hpp"
#include "L3KVG/Query.hpp"
#include "L3KVG/FederationID.hpp"
#include "L3KVG/QueryResult.hpp"
#include <filesystem>
#include <gtest/gtest.h>
#include <string>
#include <future>
#include <vector>
#include <thread>
#include <zmq.hpp>
#include <zmq_addon.hpp>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

void run_mock_server(uint16_t port, uint16_t cluster_id, const std::string& db_path) {
    std::filesystem::remove_all(db_path);
    auto engine = std::make_unique<l3kvg::Engine>(db_path, 1);
    engine->get_resolver().register_local_cluster("remote", cluster_id);
    
    // Put remote node B
    uint64_t node_b_id = engine->get_resolver().parse_uuid("node_b");
    engine->put_node(node_b_id, R"json({"id":"node_b","name":"Node B (Remote)"})json");

    zmq::context_t ctx(1);
    zmq::socket_t sock(ctx, ZMQ_ROUTER);
    std::string zmq_endpoint = "tcp://127.0.0.1:" + std::to_string(port);
    sock.bind(zmq_endpoint);

    bool running = true;
    while (running) {
        std::vector<zmq::message_t> recv_msgs;
        auto result = zmq::recv_multipart(sock, std::back_inserter(recv_msgs), zmq::recv_flags::dontwait);
        if (!result) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        if (recv_msgs.size() < 5) {
            continue;
        }

        auto& identity = recv_msgs[0];
        auto opcode = recv_msgs[3].to_string();

        if (opcode == "R") {
            try {
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
                running = false;
            } catch (...) {
            }
        } else if (opcode == "H") {
            sock.send(identity, zmq::send_flags::sndmore);
            sock.send(zmq::message_t(), zmq::send_flags::sndmore);
            sock.send(zmq::message_t("OK", 2), zmq::send_flags::none);
        }
    }
}

TEST(FederationIntegrationTest, ClientPing) {
    uint16_t remote_port = 5558;
    uint16_t remote_cluster_id = 101;
    std::string remote_db = "test_remote_db_ping";
    
    std::thread remote_thread(run_mock_server, remote_port, remote_cluster_id, remote_db);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    l3kvg::Settings settings;
    settings.fed_timeout_ms = 500;
    l3kvg::RemoteL3KVClient client(settings);
    auto pool = std::make_shared<l3kvg::ThreadPool>(1);
    client.set_thread_pool(pool);
    client.add_peer(remote_cluster_id, "tcp://127.0.0.1:" + std::to_string(remote_port));

    auto future = client.ping_peer(remote_cluster_id);
    EXPECT_TRUE(future.get());

    // Clean up mock server (it will exit on next query, but for ping we might need another way or just let it time out/detach)
    // For this test, let's trigger the "R" opcode to let it join cleanly.
    (void)client.resume_query_async(remote_cluster_id, {1}, "{}").get();
    
    remote_thread.join();
    std::filesystem::remove_all(remote_db);
}

TEST(FederationIntegrationTest, EndToEndZmqQuery) {
    uint16_t remote_port = 5557;
    uint16_t remote_cluster_id = 100;
    std::string remote_db = "test_remote_db";
    std::string local_db = "test_local_db";

    std::thread remote_thread(run_mock_server, remote_port, remote_cluster_id, remote_db);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    std::filesystem::remove_all(local_db);
    
    auto engine = std::make_unique<l3kvg::Engine>(local_db, 1);
    engine->get_resolver().register_local_cluster("local", 1);
    engine->get_resolver().register_federation("remote", remote_cluster_id, {"tcp://127.0.0.1:" + std::to_string(remote_port)});
    
    engine->get_remote_client().add_peer(remote_cluster_id, "tcp://127.0.0.1:" + std::to_string(remote_port));

    uint64_t node_a_id = engine->get_resolver().parse_uuid("node_a");
    engine->put_node(node_a_id, R"json({"id":"node_a","name":"Node A"})json");

    uint64_t node_b_id = engine->get_resolver().parse_uuid("remote:node_b");
    engine->add_edge(node_a_id, "link", 1.0, node_b_id);

    auto results = engine->query()
                         .match("a")
                         .where_eq("a", "id", "node_a")
                         .out("link")
                         .as("b")
                         .return_("b", "name")
                         .execute();

    ASSERT_EQ(results.size(), 1);
    EXPECT_EQ(results[0].fields.at("b.name"), "Node B (Remote)");

    remote_thread.join();
    std::filesystem::remove_all(local_db);
    std::filesystem::remove_all(remote_db);
}

int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
