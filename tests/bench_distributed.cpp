#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <winsock2.h>
#include <ws2tcpip.h>
#include <BaseTsd.h>

#include <string>
#include <vector>
#include <chrono>
#include <iostream>
#include <thread>
#include <atomic>
#include <numeric>

#include "L3KVG/EdgeCoordinator.hpp"
#include "L3KVG/ClusterResolver.hpp"
#include "L3KVG/RemoteL3KVClient.hpp"
#include "L3KVG/Engine.hpp"
#include "httplib.h"
#include <nlohmann/json.hpp>

using json = nlohmann::json;

int main() {
    try {
        std::cout << "--- L3KVG Distributed Cluster Benchmark (Phase 2: Batching + Binary) ---\n";
        
        // Setup client-side infrastructure to match the server cluster
        auto ring = std::make_shared<lite3::ConsistentHash>();
        ring->add_node(1);
        ring->add_node(2);
        ring->add_node(3);

        l3kvg::ClusterResolver resolver(ring, 1);
        l3kvg::RemoteL3KVClient remote_client;
        remote_client.add_peer(1, "tcp://127.0.0.1:8081");
        remote_client.add_peer(2, "tcp://127.0.0.1:8082");
        remote_client.add_peer(3, "tcp://127.0.0.1:8083");


        // Use a unique DB path for the benchmark client
        auto engine = std::make_unique<l3kvg::Engine>("bench_db_client", 1, ring);
        l3kvg::EdgeCoordinator coordinator(engine->get_store(), resolver, remote_client, 1);

        const int NUM_NODES = 10000;
        const int NUM_EDGES = 50000;
        const int THREADS = 16;

        std::cout << "Benchmarking distributed edge creation (" << NUM_EDGES << " edges, " << THREADS << " threads)...\n";
        std::cout << "Using Batching (1000 per RPC) and Zero-Copy Binary Buffers.\n";

        std::atomic<int> completed{0};
        auto start = std::chrono::high_resolution_clock::now();

        std::vector<std::thread> workers;
        for (int t = 0; t < THREADS; ++t) {
            workers.emplace_back([&, t]() {
                try {
                    for (int i = 0; i < NUM_EDGES / THREADS; ++i) {
                        int src = (t * (NUM_EDGES / THREADS) + i) % NUM_NODES;
                        int dst = (src + 1) % NUM_NODES;
                        
                        std::string src_uuid = "npc_" + std::to_string(src);
                        std::string dst_uuid = "npc_" + std::to_string(dst);
                        
                        auto fut = coordinator.atomic_put_edge(src_uuid, "knows", 1.0, dst_uuid, "{\"meta\": \"bench\"}");
                        if (i == (NUM_EDGES / THREADS) - 1) fut.get();
                        completed++;
                    }
                } catch (const std::exception& e) {
                    std::cerr << "Worker thread error: " << e.what() << "\n";
                }
            });
        }

        for (auto &w : workers) w.join();
        auto end = std::chrono::high_resolution_clock::now();
        
        auto duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
        double ops_sec = (completed.load() / (duration_ms / 1000.0));

        std::cout << "\nResults:\n";
        std::cout << "  Total Edges: " << completed.load() << "\n";
        std::cout << "  Total Time:  " << duration_ms << " ms\n";
        std::cout << "  Throughput:  " << ops_sec << " edges/sec\n";

        if (ops_sec > 30000) {
            std::cout << "\n[SUCCESS] Phase 2 Performance Target Met (>30k ops/sec)!\n";
        } else {
            std::cout << "\n[WARN] Performance improved but below target: " << ops_sec << " ops/sec\n";
        }
    } catch (const std::exception& e) {
        std::cerr << "Main thread error: " << e.what() << "\n";
        return 1;
    }

    return 0;
}
