#include <gtest/gtest.h>
#include "L3KVG/Engine.hpp"
#include "L3KVG/Node.hpp"
#include <thread>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <string>
#include <zmq.hpp>
#include <zmq_addon.hpp>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

void run_zmq_mock_node_server(uint16_t port, std::atomic<bool>& stop_signal) {
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

        if (recv_msgs.size() < 3) continue;

        auto& identity = recv_msgs[0];
        auto opcode = recv_msgs[2].to_string();

        if (opcode == "G") {
            // Respond with Remote Bob
            json res = {{"name", "Remote Bob"}, {"bloom", 0}};
            std::string resp_json = res.dump();
            
            sock.send(identity, zmq::send_flags::sndmore);
            sock.send(zmq::message_t(), zmq::send_flags::sndmore);
            sock.send(zmq::message_t(resp_json.data(), resp_json.size()), zmq::send_flags::none);
        }
    }
}

TEST(AsyncTraversalTest, DistributedNodeFetch) {
    uint16_t zmq_port = 9095;
    std::atomic<bool> stop_signal{false};
    
    std::thread server_thread(run_zmq_mock_node_server, zmq_port, std::ref(stop_signal));
    
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    std::string db_path = "test_async_db";
    std::filesystem::remove_all(db_path);

    auto ring = std::make_shared<lite3::ConsistentHash>(100);
    ring->add_node(1); // Local
    ring->add_node(2); // Remote (mapped to port 9095)

    {
        l3kvg::Engine engine(db_path, 1, ring);
        // add_peer expects URL, and it will append +1 if it starts with http://
        // But here we want to connect DIRECTLY to 9095.
        // If we provide tcp://, it won't touch the port.
        engine.get_remote_client().add_peer(2, "tcp://127.0.0.1:9095");

        auto& resolver = engine.get_resolver();
        
        std::string remote_uuid;
        for (int i = 0; i < 1000; ++i) {
            std::string candidate = "remote_" + std::to_string(i);
            if (resolver.get_node_owner(resolver.parse_uuid(candidate)) == 2) {
                remote_uuid = candidate;
                break;
            }
        }

        ASSERT_FALSE(remote_uuid.empty()) << "Could not find a UUID that hashes to node 2";

        std::cerr << "[AsyncTraversalTest] DEBUG: Testing with remote UUID: " << remote_uuid << "\n";

        auto node = engine.get_node(remote_uuid);
        std::string name = node->get_attribute<std::string>("name");
        std::cerr << "[AsyncTraversalTest] DEBUG: Got name property: '" << name << "'\n";
        
        EXPECT_EQ(name, "Remote Bob");
    }

    stop_signal = true;
    server_thread.join();
    std::filesystem::remove_all(db_path);
}

int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
