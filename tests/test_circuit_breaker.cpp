#include "L3KVG/RemoteL3KVClient.hpp"
#include "L3KVG/Settings.hpp"
#include <gtest/gtest.h>
#include <thread>
#include <chrono>
#include <zmq.hpp>
#include <zmq_addon.hpp>

using namespace l3kvg;

TEST(CircuitBreakerTest, TimeoutAndTrip) {
    auto test_logic = []() {
        Settings settings;
        settings.fed_timeout_ms = 100; // Fast timeout
        settings.breaker_failure_threshold = 2;
        
        RemoteL3KVClient client(settings);
        auto pool = std::make_shared<ThreadPool>(1);
        client.set_thread_pool(pool);

        uint16_t cluster_id = 200;
        // Connect to a port where nothing is listening to induce timeout
        client.add_peer(cluster_id, "tcp://127.0.0.1:9999");

        // First attempt should timeout and throw FederationTimeoutException
        auto future1 = client.resume_query_async(cluster_id, {1, 2}, "{}");
        EXPECT_THROW({ (void)future1.get(); }, FederationTimeoutException);
        EXPECT_EQ(client.get_circuit_state(cluster_id), CircuitState::CLOSED);

        // Second attempt should timeout and trip to OPEN
        auto future2 = client.resume_query_async(cluster_id, {1, 2}, "{}");
        EXPECT_THROW({ (void)future2.get(); }, FederationTimeoutException);
        EXPECT_EQ(client.get_circuit_state(cluster_id), CircuitState::OPEN);

        // Third attempt should fail fast with CircuitBreakerOpenException
        auto start = std::chrono::steady_clock::now();
        auto future3 = client.resume_query_async(cluster_id, {1, 2}, "{}");
        EXPECT_THROW({ (void)future3.get(); }, CircuitBreakerOpenException);
        auto end = std::chrono::steady_clock::now();
        
        EXPECT_LT(std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count(), 50); // Fail fast
    };

    std::packaged_task<void()> task(test_logic);
    auto future = task.get_future();
    std::thread t(std::move(task));
    
    if (future.wait_for(std::chrono::seconds(10)) == std::future_status::timeout) {
        std::cerr << "[Test] CRITICAL: Test timed out after 10 seconds!" << std::endl;
        t.detach();
        FAIL() << "Test timed out after 10 seconds";
    } else {
        t.join();
        future.get(); // Propagate any exceptions
    }
}

TEST(CircuitBreakerTest, RecoveryViaHeartbeat) {
    Settings settings;
    settings.fed_timeout_ms = 100;
    settings.breaker_failure_threshold = 1;
    settings.breaker_reset_timeout_ms = 500;
    settings.health_check_interval_ms = 200;
    
    RemoteL3KVClient client(settings);
    auto pool = std::make_shared<ThreadPool>(2); // 2 threads: one for query, one for ping
    client.set_thread_pool(pool);

    uint16_t cluster_id = 300;
    uint16_t port = 9998;
    client.add_peer(cluster_id, "tcp://127.0.0.1:" + std::to_string(port));

    // 1. Trip the circuit
    std::cout << "[Test] Tripping circuit..." << std::endl;
    auto future1 = client.resume_query_async(cluster_id, {1}, "{}");
    EXPECT_THROW({ (void)future1.get(); }, FederationTimeoutException);
    EXPECT_EQ(client.get_circuit_state(cluster_id), CircuitState::OPEN);

    // 2. Start a mock server to allow recovery
    std::cout << "[Test] Starting mock server..." << std::endl;
    std::atomic<bool> server_running{true};
    std::thread server_thread([&]() {
        zmq::context_t ctx(1);
        zmq::socket_t sock(ctx, ZMQ_ROUTER);
        sock.set(zmq::sockopt::linger, 0);
        sock.bind("tcp://127.0.0.1:" + std::to_string(port));

        while (server_running) {
            std::vector<zmq::message_t> recv_msgs;
            auto res = zmq::recv_multipart(sock, std::back_inserter(recv_msgs), zmq::recv_flags::dontwait);
            if (res && recv_msgs.size() >= 4) {
                auto identity = std::move(recv_msgs[0]);
                auto opcode = recv_msgs[3].to_string();
                if (opcode == "H") {
                    std::cout << "[MockServer] Received Heartbeat, replying OK" << std::endl;
                    sock.send(identity, zmq::send_flags::sndmore);
                    sock.send(zmq::message_t(), zmq::send_flags::sndmore);
                    sock.send(zmq::message_t("OK", 2), zmq::send_flags::none);
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    });

    // 3. Wait for background thread to recover the circuit
    std::cout << "[Test] Waiting for recovery..." << std::endl;
    bool recovered = false;
    for (int i = 0; i < 30; ++i) { // Wait up to 3 seconds
        if (client.get_circuit_state(cluster_id) == CircuitState::CLOSED) {
            recovered = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    EXPECT_TRUE(recovered) << "Circuit did not recover after 3 seconds. Current state: " << (int)client.get_circuit_state(cluster_id);
    EXPECT_EQ(client.get_circuit_state(cluster_id), CircuitState::CLOSED);

    server_running = false;
    server_thread.join();
}

int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
