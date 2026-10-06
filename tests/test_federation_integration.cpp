#include "L3KVG/Engine.hpp"
#include "L3KVG/Node.hpp"
#include "engine/store.hpp"
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
#include "buffer.hpp"

void run_mock_server(uint16_t port, uint16_t cluster_id, const std::string& db_path, std::atomic<bool>& stop_signal) {
    std::filesystem::remove_all(db_path);
    auto engine = std::make_unique<l3kvg::Engine>(db_path, 1);
    engine->get_resolver().register_local_cluster("remote", cluster_id);
    
    // Put remote node B
    uint64_t node_b_id = engine->get_resolver().parse_uuid("node_b");
    lite3cpp::Buffer b_buf;
    b_buf.init_object();
    b_buf.set_str(0, "id", "node_b");
    b_buf.set_str(0, "name", "Node B (Remote)");
    engine->put_node(node_b_id, std::string(reinterpret_cast<const char*>(b_buf.data()), b_buf.size()));

    zmq::context_t ctx(1);
    zmq::socket_t sock(ctx, ZMQ_ROUTER);
    std::string zmq_endpoint = "tcp://127.0.0.1:" + std::to_string(port);
    sock.bind(zmq_endpoint);

    while (!stop_signal) {
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
                const auto& nodes_msg = recv_msgs[4];
                std::vector<uint64_t> nodes;
                bool parsed_as_json = false;
                std::string_view sv(static_cast<const char*>(nodes_msg.data()), nodes_msg.size());
                size_t first = sv.find_first_not_of(" \t\r\n");
                size_t last = sv.find_last_not_of(" \t\r\n");
                if (first != std::string_view::npos && last != std::string_view::npos && last >= first + 1 && sv[first] == '[' && sv[last] == ']') {
                    try {
                        lite3cpp::Buffer buf = lite3cpp::lite3_json::from_json_string(std::string(sv.substr(first, last - first + 1)));
                        if (buf.size() >= sizeof(lite3cpp::PackedNodeLayout)) {
                            lite3cpp::NodeView nv(reinterpret_cast<const lite3cpp::PackedNodeLayout*>(buf.data()));
                            if (nv.type() == lite3cpp::Type::Array) {
                                parsed_as_json = true;
                                for (uint32_t i = 0; i < nv.size(); ++i) {
                                    auto t = buf.arr_get_type(0, i);
                                    if (t == lite3cpp::Type::Int64) {
                                        nodes.push_back(static_cast<uint64_t>(buf.arr_get_i64(0, i)));
                                    } else if (t == lite3cpp::Type::String) {
                                        nodes.push_back(std::stoull(std::string(buf.arr_get_str(0, i)), nullptr, 16));
                                    }
                                }
                            }
                        }
                    } catch (...) {
                        parsed_as_json = false;
                    }
                }
                if (!parsed_as_json && nodes_msg.size() % sizeof(uint64_t) == 0) {
                    size_t count = nodes_msg.size() / sizeof(uint64_t);
                    nodes.resize(count);
                    if (count > 0) {
                        std::memcpy(nodes.data(), nodes_msg.data(), count * sizeof(uint64_t));
                    }
                }

                const auto& qmsg = recv_msgs[5];
                lite3cpp::Buffer qbuf;
                const uint8_t* qptr = static_cast<const uint8_t*>(qmsg.data());
                if (qmsg.size() >= sizeof(lite3cpp::PackedNodeLayout) && (qptr[0] == 0x06 || qptr[0] == 0x07)) {
                    qbuf = lite3cpp::Buffer(std::vector<uint8_t>(qptr, qptr + qmsg.size()));
                } else {
                    std::string qstr = qmsg.to_string();
                    qbuf = lite3cpp::lite3_json::from_json_string(qstr.empty() ? "{}" : qstr);
                }

                auto results = engine->query().resume(nodes, qbuf).execute();

                lite3cpp::Buffer resp_buf = l3kvg::Query::serialize_results(results);
                sock.send(identity, zmq::send_flags::sndmore);
                sock.send(zmq::message_t(), zmq::send_flags::sndmore);
                sock.send(zmq::message_t(resp_buf.data(), resp_buf.size()), zmq::send_flags::none);
            } catch (...) {
                lite3cpp::Buffer empty_buf;
                empty_buf.init_array();
                sock.send(identity, zmq::send_flags::sndmore);
                sock.send(zmq::message_t(), zmq::send_flags::sndmore);
                sock.send(zmq::message_t(empty_buf.data(), empty_buf.size()), zmq::send_flags::none);
            }
        } else if (opcode == "N") {
            try {
                std::string id_str = recv_msgs[4].to_string();
                std::string label = recv_msgs[5].to_string();
                double min_weight = (recv_msgs.size() >= 7) ? std::stod(recv_msgs[6].to_string()) : 0.0;
                uint64_t target_node_id = std::stoull(id_str, nullptr, 16);
                auto node = engine->get_node(target_node_id);
                std::vector<uint64_t> neighs;
                if (node) neighs = node->get_neighbors(label, min_weight);
                sock.send(identity, zmq::send_flags::sndmore);
                sock.send(zmq::message_t(), zmq::send_flags::sndmore);
                sock.send(zmq::message_t(neighs.data(), neighs.size() * sizeof(uint64_t)), zmq::send_flags::none);
            } catch (...) {
                sock.send(identity, zmq::send_flags::sndmore);
                sock.send(zmq::message_t(), zmq::send_flags::sndmore);
                sock.send(zmq::message_t(), zmq::send_flags::none);
            }
        } else if (opcode == "I") {
            try {
                std::string id_str = recv_msgs[4].to_string();
                std::string label = recv_msgs[5].to_string();
                uint64_t target_node_id = std::stoull(id_str, nullptr, 16);
                auto node = engine->get_node(target_node_id);
                std::vector<uint64_t> neighs;
                if (node) neighs = node->get_in_neighbors(label);
                sock.send(identity, zmq::send_flags::sndmore);
                sock.send(zmq::message_t(), zmq::send_flags::sndmore);
                sock.send(zmq::message_t(neighs.data(), neighs.size() * sizeof(uint64_t)), zmq::send_flags::none);
            } catch (...) {
                sock.send(identity, zmq::send_flags::sndmore);
                sock.send(zmq::message_t(), zmq::send_flags::sndmore);
                sock.send(zmq::message_t(), zmq::send_flags::none);
            }
        } else if (opcode == "G") {
            try {
                std::string key = recv_msgs[4].to_string();
                auto val = engine->get_store()->get(key);
                sock.send(identity, zmq::send_flags::sndmore);
                sock.send(zmq::message_t(), zmq::send_flags::sndmore);
                sock.send(zmq::message_t(val.data(), val.size()), zmq::send_flags::none);
            } catch (...) {
                sock.send(identity, zmq::send_flags::sndmore);
                sock.send(zmq::message_t(), zmq::send_flags::sndmore);
                sock.send(zmq::message_t(), zmq::send_flags::none);
            }
        } else if (opcode == "H") {
            sock.send(identity, zmq::send_flags::sndmore);
            sock.send(zmq::message_t(), zmq::send_flags::sndmore);
            sock.send(zmq::message_t("OK", 2), zmq::send_flags::none);
        } else if (opcode == "B") {
            try {
                if (recv_msgs.size() >= 5) {
                    const auto& msg = recv_msgs[4];
                    lite3cpp::Buffer batch_buf(std::vector<uint8_t>(
                        static_cast<const uint8_t*>(msg.data()),
                        static_cast<const uint8_t*>(msg.data()) + msg.size()
                    ));
                    engine->apply_batch(batch_buf);
                }
            } catch (...) {}
            sock.send(identity, zmq::send_flags::sndmore);
            sock.send(zmq::message_t(), zmq::send_flags::sndmore);
            sock.send(zmq::message_t("OK", 2), zmq::send_flags::none);
        } else {
            sock.send(identity, zmq::send_flags::sndmore);
            sock.send(zmq::message_t(), zmq::send_flags::sndmore);
            sock.send(zmq::message_t("ERR", 3), zmq::send_flags::none);
        }
    }
}

TEST(FederationIntegrationTest, ClientPing) {
    uint16_t remote_port = 5558;
    uint16_t remote_cluster_id = 101;
    std::string remote_db = "test_remote_db_ping";
    
    std::atomic<bool> stop_signal{false};
    std::thread remote_thread(run_mock_server, remote_port, remote_cluster_id, remote_db, std::ref(stop_signal));
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    l3kvg::Settings settings;
    settings.fed_timeout_ms = 500;
    l3kvg::RemoteL3KVClient client(settings);
    auto pool = std::make_shared<l3kvg::ThreadPool>(1);
    client.set_thread_pool(pool);
    client.add_peer(remote_cluster_id, "tcp://127.0.0.1:" + std::to_string(remote_port));

    auto future = client.ping_peer(remote_cluster_id);
    EXPECT_TRUE(future.get());

    stop_signal = true;
    remote_thread.join();
    std::filesystem::remove_all(remote_db);
}

TEST(FederationIntegrationTest, EndToEndZmqQuery) {
    uint16_t remote_port = 5557;
    uint16_t remote_cluster_id = 100;
    std::string remote_db = "test_remote_db";
    std::string local_db = "test_local_db";

    std::atomic<bool> stop_signal{false};
    std::thread remote_thread(run_mock_server, remote_port, remote_cluster_id, remote_db, std::ref(stop_signal));
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    std::filesystem::remove_all(local_db);
    
    auto engine = std::make_unique<l3kvg::Engine>(local_db, 1);
    engine->get_resolver().register_local_cluster("local", 1);
    engine->get_resolver().register_federation("remote", remote_cluster_id, {"tcp://127.0.0.1:" + std::to_string(remote_port)});
    
    engine->get_remote_client().add_peer(remote_cluster_id, "tcp://127.0.0.1:" + std::to_string(remote_port));

    uint64_t node_a_id = engine->get_resolver().parse_uuid("node_a");
    lite3cpp::Buffer a_buf;
    a_buf.init_object();
    a_buf.set_str(0, "id", "node_a");
    a_buf.set_str(0, "name", "Node A");
    engine->put_node(node_a_id, std::string(reinterpret_cast<const char*>(a_buf.data()), a_buf.size()));

    uint64_t node_b_id = engine->get_resolver().parse_uuid("remote:node_b");
    engine->add_edge(node_a_id, "link", 1.0, node_b_id);
    engine->get_store()->wait_all_shards();

    auto results = engine->query()
                         .match("a")
                         .where_eq("a", "id", "node_a")
                         .out("link")
                         .as("b")
                         .return_("b", "name")
                         .execute();

    ASSERT_EQ(results.size(), 1);
    std::string b_name = !results[0].projected_values.empty() ? results[0].projected_values[0] : (results[0].fields.count("b.name") ? results[0].fields.at("b.name") : "");
    EXPECT_EQ(b_name, "Node B (Remote)");

    stop_signal = true;
    remote_thread.join();
    std::filesystem::remove_all(local_db);
    std::filesystem::remove_all(remote_db);
}

int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
