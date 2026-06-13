#include "L3KVG/Engine.hpp"
#include "L3KVG/Query.hpp"
#include "L3KVG/QueryResult.hpp"
#include "L3KVG/KeyBuilder.hpp"
#include "engine/store.hpp" // L3KV Store
#include <filesystem>
#include <gtest/gtest.h>
#include <thread>
#include <zmq.hpp>
#include <zmq_addon.hpp>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

class ReplicationRoutingTest : public ::testing::Test {
protected:
    uint16_t node2_zmq_port = 6663;
    std::string db1 = "test_rep_db1";
    std::string db2 = "test_rep_db2";
    std::shared_ptr<lite3::ConsistentHash> ring;
    std::unique_ptr<l3kvg::Engine> engine1;
    std::unique_ptr<l3kvg::Engine> engine2;
    std::atomic<bool> node2_running{true};
    std::atomic<int> messages_received{0};
    std::thread node2_thread;

    void SetUp() override {
        std::filesystem::remove_all(db1);
        std::filesystem::remove_all(db2);

        ring = std::make_shared<lite3::ConsistentHash>();
        ring->add_node(1);
        ring->add_node(2);

        l3kvg::Settings settings;
        engine1 = std::make_unique<l3kvg::Engine>(db1, 1, ring, 1, settings);
        engine2 = std::make_unique<l3kvg::Engine>(db2, 2, ring, 1, settings);

        engine1->get_remote_client().add_peer(2, "tcp://127.0.0.1:" + std::to_string(node2_zmq_port));

        node2_thread = std::thread([&]() {
            zmq::context_t ctx(1);
            zmq::socket_t sock(ctx, ZMQ_ROUTER);
            sock.set(zmq::sockopt::linger, 0);
            sock.bind("tcp://127.0.0.1:" + std::to_string(node2_zmq_port));
            while (node2_running) {
                std::vector<zmq::message_t> msgs;
                if (zmq::recv_multipart(sock, std::back_inserter(msgs), zmq::recv_flags::dontwait)) {
                    if (msgs.size() >= 6) {
                        messages_received++;
                        auto identity = std::move(msgs[0]);
                        auto opcode = msgs[3].to_string();
                        if (opcode == "P") { 
                            std::string key = msgs[4].to_string();
                            std::string val = msgs[5].to_string();
                            engine2->get_store()->put(key, val);
                            sock.send(identity, zmq::send_flags::sndmore);
                            sock.send(zmq::message_t(), zmq::send_flags::sndmore);
                            sock.send(zmq::message_t("OK", 2), zmq::send_flags::none);
                        }
                    }
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        });
    }

    void TearDown() override {
        node2_running = false;
        if (node2_thread.joinable()) node2_thread.join();
        std::filesystem::remove_all(db1);
        std::filesystem::remove_all(db2);
    }
};

TEST_F(ReplicationRoutingTest, NodeKeyRouting) {
    uint64_t node2_id = 0;
    std::string key;
    for (uint64_t i = 0; i < 1000; ++i) {
        std::string k = std::string(l3kvg::KeyBuilder::node_key(i));
        if (ring->get_node(k) == 2) {
            node2_id = i;
            key = k;
            break;
        }
    }
    
    std::string payload = R"({"id":"test","data":"replicated"})";
    engine1->replicate_key(key, payload, 1);
    
    bool success = false;
    for (int i = 0; i < 50; ++i) {
        if (messages_received > 0 && engine2->get_store()->get(key).size() > 0) {
            success = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    
    EXPECT_TRUE(success);
}

TEST_F(ReplicationRoutingTest, EdgeKeyRouting) {
    uint64_t node2_id = 0;
    for (uint64_t i = 0; i < 1000; ++i) {
        if (ring->get_node(l3kvg::KeyBuilder::node_key(i)) == 2) {
            node2_id = i;
            break;
        }
    }
    
    // Create an edge out key for this node
    std::string key = std::string(l3kvg::KeyBuilder::edge_out_key(node2_id, "friend", 1.0, 999));
    std::string payload = R"({"since":"2024"})";
    
    engine1->replicate_key(key, payload, 1);
    
    bool success = false;
    for (int i = 0; i < 50; ++i) {
        if (messages_received > 0 && engine2->get_store()->get(key).size() > 0) {
            success = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    
    EXPECT_TRUE(success);
}

int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
