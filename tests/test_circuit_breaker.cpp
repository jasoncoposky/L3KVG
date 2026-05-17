#include "L3KVG/RemoteL3KVClient.hpp"
#include "L3KVG/Settings.hpp"
#include <gtest/gtest.h>
#include <thread>
#include <chrono>
#include <zmq.hpp>

using namespace l3kvg;

TEST(CircuitBreakerTest, TimeoutAndTrip) {
    Settings settings;
    settings.fed_timeout_ms = 100; // Fast timeout
    settings.breaker_failure_threshold = 2;
    
    RemoteL3KVClient client(settings);
    auto pool = std::make_shared<ThreadPool>(1);
    client.set_thread_pool(pool);

    uint16_t cluster_id = 200;
    // Connect to a port where nothing is listening to induce timeout
    client.add_peer(cluster_id, "tcp://127.0.0.1:9999");

    // First attempt should timeout and report failure
    auto future1 = client.resume_query_async(cluster_id, {1, 2}, "{}");
    auto res1 = future1.get();
    EXPECT_TRUE(res1.empty());
    EXPECT_EQ(client.get_circuit_state(cluster_id), CircuitState::CLOSED);

    // Second attempt should timeout and trip to OPEN
    auto future2 = client.resume_query_async(cluster_id, {1, 2}, "{}");
    auto res2 = future2.get();
    EXPECT_TRUE(res2.empty());
    EXPECT_EQ(client.get_circuit_state(cluster_id), CircuitState::OPEN);

    // Third attempt should fail fast
    auto start = std::chrono::steady_clock::now();
    auto future3 = client.resume_query_async(cluster_id, {1, 2}, "{}");
    auto res3 = future3.get();
    auto end = std::chrono::steady_clock::now();
    
    EXPECT_TRUE(res3.empty());
    EXPECT_LT(std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count(), 50); // Fail fast
}

int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
