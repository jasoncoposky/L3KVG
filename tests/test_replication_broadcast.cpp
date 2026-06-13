#include "L3KVG/Engine.hpp"
#include "L3KVG/Query.hpp"
#include "L3KVG/QueryResult.hpp"
#include "L3KVG/KeyBuilder.hpp"
#include "engine/store.hpp"
#include <filesystem>
#include <gtest/gtest.h>
#include <thread>
#include <zmq.hpp>
#include <zmq_addon.hpp>
#include <nlohmann/json.hpp>
#include <atomic>

using json = nlohmann::json;

class ReplicationBroadcastTest : public ::testing::Test {
protected:
    uint16_t cluster2_port = 7772;
    uint16_t cluster3_port = 7773;
    std::string db_local = "test_broadcast_local";
    
    std::atomic<bool> running{true};
    std::atomic<int> cluster2_msgs{0};
    std::atomic<int> cluster3_msgs{0};
    std::thread t2, t3;

    void SetUp() override {
        std::filesystem::remove_all(db_local);
        
        auto start_mock = [this](uint16_t port, std::atomic<int>& counter) {
            zmq::context_t ctx(1);
            zmq::socket_t sock(ctx, ZMQ_ROUTER);
            sock.set(zmq::sockopt::linger, 0);
            sock.bind("tcp://127.0.0.1:" + std::to_string(port));
            while (running) {
                std::vector<zmq::message_t> msgs;
                if (zmq::recv_multipart(sock, std::back_inserter(msgs), zmq::recv_flags::dontwait)) {
                    if (msgs.size() >= 4) {
                        auto identity = std::move(msgs[0]);
                        auto opcode = msgs[3].to_string();
                        if (opcode == "S") {
                            counter++;
                            sock.send(identity, zmq::send_flags::sndmore);
                            sock.send(zmq::message_t(), zmq::send_flags::sndmore);
                            sock.send(zmq::message_t("OK", 2), zmq::send_flags::none);
                        }
                    }
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        };

        t2 = std::thread(start_mock, cluster2_port, std::ref(cluster2_msgs));
        t3 = std::thread(start_mock, cluster3_port, std::ref(cluster3_msgs));
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    void TearDown() override {
        running = false;
        if (t2.joinable()) t2.join();
        if (t3.joinable()) t3.join();
        std::filesystem::remove_all(db_local);
    }
};

TEST_F(ReplicationBroadcastTest, BroadcastToMultipleClusters) {
    l3kvg::Settings settings;
    // Fast settings for test
    settings.fed_timeout_ms = 100;
    
    auto engine = std::make_unique<l3kvg::Engine>(db_local, 1, nullptr, 1, settings);
    
    // Register Cluster 2 and 3 as federations
    engine->get_resolver().register_local_cluster("local", 1);
    engine->get_resolver().register_federation("remote2", 2, {"tcp://127.0.0.1:" + std::to_string(cluster2_port)});
    engine->get_resolver().register_federation("remote3", 3, {"tcp://127.0.0.1:" + std::to_string(cluster3_port)});
    
    // Tell the remote client about these peers
    engine->get_remote_client().add_peer(2, "tcp://127.0.0.1:" + std::to_string(cluster2_port));
    engine->get_remote_client().add_peer(3, "tcp://127.0.0.1:" + std::to_string(cluster3_port));

    // Perform a local write
    std::cout << "[Test] Performing local write..." << std::endl;
    uint64_t node_id = 100;
    engine->put_node(node_id, R"({"name":"BroadcastNode"})");

    // Verify both mock clusters received the "S" (Sync) opcode
    bool cluster2_ok = false;
    bool cluster3_ok = false;
    
    for (int i = 0; i < 50; ++i) {
        if (cluster2_msgs > 0) cluster2_ok = true;
        if (cluster3_msgs > 0) cluster3_ok = true;
        if (cluster2_ok && cluster3_ok) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    EXPECT_TRUE(cluster2_ok) << "Cluster 2 did not receive replication";
    EXPECT_TRUE(cluster3_ok) << "Cluster 3 did not receive replication";
}

TEST_F(ReplicationBroadcastTest, BroadcastEdgeToMultipleClusters) {
    l3kvg::Settings settings;
    settings.fed_timeout_ms = 100;
    
    auto engine = std::make_unique<l3kvg::Engine>(db_local, 1, nullptr, 1, settings);
    
    engine->get_resolver().register_local_cluster("local", 1);
    engine->get_resolver().register_federation("remote2", 2, {"tcp://127.0.0.1:" + std::to_string(cluster2_port)});
    engine->get_resolver().register_federation("remote3", 3, {"tcp://127.0.0.1:" + std::to_string(cluster3_port)});
    
    engine->get_remote_client().add_peer(2, "tcp://127.0.0.1:" + std::to_string(cluster2_port));
    engine->get_remote_client().add_peer(3, "tcp://127.0.0.1:" + std::to_string(cluster3_port));

    // Reset counters
    cluster2_msgs = 0;
    cluster3_msgs = 0;

    // Perform a local edge write
    std::cout << "[Test] Performing local edge write..." << std::endl;
    engine->add_edge(1, "knows", 1.0, 2);

    // Verify both mock clusters received the "S" (Sync) opcode
    // Note: add_edge generates TWO writes (out and in). 
    // Since node 1 and 2 are local in this engine setup (shardless), 
    // they might both be local.
    
    bool cluster2_ok = false;
    bool cluster3_ok = false;
    
    for (int i = 0; i < 50; ++i) {
        if (cluster2_msgs >= 2) cluster2_ok = true; // Expecting 2 writes (out and in)
        if (cluster3_msgs >= 2) cluster3_ok = true;
        if (cluster2_ok && cluster3_ok) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    EXPECT_TRUE(cluster2_ok) << "Cluster 2 did not receive edge replication (got " << cluster2_msgs.load() << ")";
    EXPECT_TRUE(cluster3_ok) << "Cluster 3 did not receive edge replication (got " << cluster3_msgs.load() << ")";
}

int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
