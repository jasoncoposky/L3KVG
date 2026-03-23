#include <gtest/gtest.h>
#include "L3KVG/Engine.hpp"
#include "L3KVG/Node.hpp"
#include "httplib.h"
#include <thread>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <string>

TEST(AsyncTraversalTest, DistributedNodeFetch) {
    httplib::Server svr;
    
    svr.Get("/api/internal/node/remote_1", [](const httplib::Request&, httplib::Response& res) {
        res.set_content("{\"name\": \"Remote Bob\", \"bloom\": 0}", "application/json");
    });

    std::thread server_thread([&svr]() {
        svr.listen("127.0.0.1", 9091);
    });
    
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    std::string db_path = "test_async_db";
    std::filesystem::remove_all(db_path);

    auto ring = std::make_shared<lite3::ConsistentHash>(100);
    ring->add_node(1); // Local
    ring->add_node(2); // Remote (mapped to port 9091)

    {
        l3kvg::Engine engine(db_path, 1, ring);
        engine.get_remote_client().add_peer(2, "127.0.0.1:9091");

        auto& resolver = engine.get_resolver();
        
        std::string remote_uuid;
        for (int i = 0; i < 1000; ++i) {
            std::string candidate = "remote_" + std::to_string(i);
            if (resolver.get_node_owner(candidate) == 2) {
                remote_uuid = candidate;
                break;
            }
        }

        if (remote_uuid.empty()) {
            std::cerr << "Could not find a UUID that hashes to node 2.\n";
            exit(1);
        }

        std::cerr << "[AsyncTraversalTest] DEBUG: Testing with remote UUID: " << remote_uuid << "\n";

        // Setup the mock server to respond to this specific UUID
        svr.Get("/api/internal/node/" + remote_uuid, [](const httplib::Request&, httplib::Response& res) {
            res.set_content("{\"name\": \"Remote Bob\", \"bloom\": 0}", "application/json");
        });

        auto node = engine.get_node(remote_uuid);
        std::string name = node->get_attribute<std::string>("name");
        std::cerr << "[AsyncTraversalTest] DEBUG: Got name property: '" << name << "'\n";
        
        EXPECT_EQ(name, "Remote Bob");
    }

    svr.stop();
    server_thread.join();
    std::filesystem::remove_all(db_path);
}
