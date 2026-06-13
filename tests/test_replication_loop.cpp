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

class ReplicationLoopTest : public ::testing::Test {
protected:
    uint16_t cluster3_port = 8883;
    std::string db_local = "test_loop_local";
    
    std::atomic<bool> running{true};
    std::atomic<int> cluster3_msgs{0};
    std::thread t3;

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

        t3 = std::thread(start_mock, cluster3_port, std::ref(cluster3_msgs));
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    void TearDown() override {
        running = false;
        if (t3.joinable()) t3.join();
        std::filesystem::remove_all(db_local);
    }
};

TEST_F(ReplicationLoopTest, IncomingSyncDoesNotRebroadcast) {
    l3kvg::Settings settings;
    settings.fed_timeout_ms = 100;
    
    auto engine = std::make_unique<l3kvg::Engine>(db_local, 1, nullptr, 1, settings);
    
    engine->get_resolver().register_local_cluster("cluster1", 1);
    engine->get_resolver().register_federation("cluster3", 3, {"tcp://127.0.0.1:" + std::to_string(cluster3_port)});
    engine->get_remote_client().add_peer(3, "tcp://127.0.0.1:" + std::to_string(cluster3_port));

    // Simulate an incoming replication from Cluster 2
    uint64_t node_id = 100;
    std::string key = std::string(l3kvg::KeyBuilder::node_key(node_id));
    std::string payload = R"({"name":"FromCluster2"})";
    
    std::cout << "[Test] Simulating incoming sync from Cluster 2 (origin=2)..." << std::endl;
    // Calling replicate_key with origin=2. It should write locally but NOT broadcast to cluster 3.
    engine->replicate_key(key, payload, 2);

    // Verify local write succeeded (must wait for async sharded put)
    bool local_success = false;
    for (int i = 0; i < 20; ++i) {
        if (engine->get_store()->get(key).size() > 0) {
            local_success = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    EXPECT_TRUE(local_success);

    // Verify Cluster 3 did NOT receive a replication
    std::cout << "[Test] Verifying Cluster 3 did not receive broadcast..." << std::endl;
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    EXPECT_EQ(cluster3_msgs.load(), 0) << "Cluster 3 received a loop replication!";
}

int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
