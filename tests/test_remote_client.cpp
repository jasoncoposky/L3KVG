#include <gtest/gtest.h>
#include "L3KVG/RemoteL3KVClient.hpp"
#include "httplib.h"
#include <thread>
#include <chrono>

TEST(RemoteClientTest, HandlesGraphRequest) {
    httplib::Server svr;
    
    svr.Post("/api/internal/neighbors", [](const httplib::Request &req, httplib::Response &res) {
        // Simple string matching instead of json parsing
        EXPECT_NE(req.body.find("\"target\": \"node_1\""), std::string::npos);
        EXPECT_NE(req.body.find("\"label\": \"friends\""), std::string::npos);
        
        std::string res_json = R"(["node_2", "node_3"])";
        res.set_content(res_json, "application/json");
    });

    std::thread t([&svr]() {
        svr.listen("127.0.0.1", 9091);
    });

    // Wait for server to start
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    l3kvg::RemoteL3KVClient client;
    client.add_peer(42, "http://127.0.0.1:9091");

    auto future = client.get_neighbors_async(42, "node_1", "friends", 0.5);
    auto neighbors = future.get();

    EXPECT_EQ(neighbors.size(), 2);
    EXPECT_EQ(neighbors[0], "node_2");
    EXPECT_EQ(neighbors[1], "node_3");

    svr.stop();
    t.join();
}
