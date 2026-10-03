#include <gtest/gtest.h>
#include "L3KVG/RemoteL3KVClient.hpp"
#include "L3KVG/MutationBatch.hpp"
#include <thread>
#include <chrono>
#include <atomic>
#include <zmq.hpp>
#include <zmq_addon.hpp>

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

TEST(RemoteClientTest, ExecuteBatchAsyncApi) {
    l3kvg::RemoteL3KVClient client;
    l3kvg::MutationBatch batch;
    batch.put_raw("key1", "val1");
    auto fut = client.execute_batch_async(9999, batch);
    EXPECT_FALSE(fut.get());
}

TEST(RemoteClientTest, ExecuteBatchOverWire) {
    const uint16_t port = 9996;
    const uint16_t cluster_id = 555;
    std::atomic<bool> server_running{true};
    std::atomic<bool> batch_received{false};

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
                size_t data_idx = 1;
                if (recv_msgs[data_idx].size() == 0) data_idx++;
                if (data_idx < recv_msgs.size() && recv_msgs[data_idx].size() == 4) data_idx++; // skip pid
                if (data_idx < recv_msgs.size()) {
                    auto opcode = recv_msgs[data_idx].to_string(); data_idx++;
                    if (opcode == "B" && data_idx < recv_msgs.size()) {
                        lite3cpp::Buffer buf(std::vector<uint8_t>(
                            static_cast<const uint8_t*>(recv_msgs[data_idx].data()),
                            static_cast<const uint8_t*>(recv_msgs[data_idx].data()) + recv_msgs[data_idx].size()
                        ));
                        if (l3kvg::MutationBatch::item_count(buf) == 2) {
                            batch_received = true;
                        }
                        sock.send(identity, zmq::send_flags::sndmore);
                        sock.send(zmq::message_t(), zmq::send_flags::sndmore);
                        sock.send(zmq::message_t("OK", 2), zmq::send_flags::none);
                    }
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    });

    // Client setup
    l3kvg::Settings settings;
    settings.fed_timeout_ms = 1000;
    l3kvg::RemoteL3KVClient client(settings);
    auto pool = std::make_shared<l3kvg::ThreadPool>(1);
    client.set_thread_pool(pool);
    client.add_peer(cluster_id, "tcp://127.0.0.1:" + std::to_string(port));

    l3kvg::MutationBatch batch;
    batch.put_node(1001, "{\"name\":\"fileA\"}");
    batch.put_raw("idx:test", "1001");

    auto fut = client.execute_batch_async(cluster_id, batch);
    bool ok = fut.get();
    EXPECT_TRUE(ok);
    EXPECT_TRUE(batch_received.load());

    server_running = false;
    server_thread.join();
}
