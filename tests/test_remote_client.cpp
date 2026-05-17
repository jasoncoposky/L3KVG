#include <gtest/gtest.h>
#include "L3KVG/RemoteL3KVClient.hpp"
#include <thread>
#include <chrono>

TEST(RemoteClientTest, HandlesGraphRequest) {
    // Note: RemoteL3KVClient now uses ZMQ instead of HTTP.
    // The current implementation has stubs for most methods.

    l3kvg::RemoteL3KVClient client;
    client.add_peer(42, "127.0.0.1:9091");

    uint64_t target_node_id = 0x123456789ABCDEF0ULL;
    auto future = client.get_neighbors_async(42, target_node_id, "friends", 0.5);
    auto neighbors = future.get();

    // The current stub returns an empty vector
    EXPECT_TRUE(neighbors.empty());
}
