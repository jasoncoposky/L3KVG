#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <BaseTsd.h>

#include "httplib.h"
#include <nlohmann/json.hpp>
#include <chrono>
#include <iostream>
#include <thread>
#include <vector>
#include <atomic>
#include <numeric>

using json = nlohmann::json;

int main() {
    std::cout << "--- L3KVG Distributed Cluster Benchmark (3 Nodes) ---\n";
    std::string master_url = "http://127.0.0.1:8081";
    httplib::Client cli("127.0.0.1", 8081);
    cli.set_read_timeout(10, 0);

    const int NUM_NODES = 1000;
    const int NUM_EDGES = 5000;
    const int THREADS = 10;

    std::cout << "Seeding " << NUM_NODES << " nodes across the cluster...\n";
    for (int i = 0; i < NUM_NODES; ++i) {
        json payload = {
            {"target", "npc_" + std::to_string(i)},
            {"payload", {{"id", "npc_" + std::to_string(i)}, {"type", "npc"}, {"index", i}}}
        };
        auto res = cli.Post("/api/internal/put_node", payload.dump(), "application/json");
        if (!res || res->status != 200) {
            std::cerr << "Failed to seed node " << i << "\n";
            return 1;
        }
    }

    std::cout << "Benchmarking distributed edge creation (" << NUM_EDGES << " edges, " << THREADS << " threads)...\n";
    std::atomic<int> completed{0};
    auto start = std::chrono::high_resolution_clock::now();

    std::vector<std::thread> workers;
    for (int t = 0; t < THREADS; ++t) {
        workers.emplace_back([&, t]() {
            httplib::Client worker_cli("127.0.0.1", 8081 + (t % 3)); // Balance requests across 3 nodes
            for (int i = 0; i < NUM_EDGES / THREADS; ++i) {
                int src = 0; // High fan-out on npc_0
                int dst = (t * (NUM_EDGES / THREADS) + i) % NUM_NODES;
                
                // Construct edge key similar to EdgeCoordinator
                std::string key = "e:npc_" + std::to_string(src) + ":knows:npc_" + std::to_string(dst);
                json payload = {
                    {"key", key},
                    {"payload", {{"w", 1.0}, {"t", 123456789}}}
                };
                
                auto res = worker_cli.Post("/api/internal/put_edge", payload.dump(), "application/json");
                if (res && res->status == 200) completed++;
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

    std::cout << "\n--- Distributed Multi-hop Query Benchmark ---\n";
    auto start_q = std::chrono::high_resolution_clock::now();
    int read_count = 100;
    for (int i = 0; i < read_count; ++i) {
        json q_payload = {
            {"target", "npc_0"},
            {"label", "knows"},
            {"min_weight", 0.0}
        };
        auto res = cli.Post("/api/internal/neighbors", q_payload.dump(), "application/json");
        if (res && res->status == 200) {
            // Success
        }
    }
    auto end_q = std::chrono::high_resolution_clock::now();
    auto q_duration_us = std::chrono::duration_cast<std::chrono::microseconds>(end_q - start_q).count() / read_count;

    std::cout << "  Avg Sharded Neighbor Lookup: " << q_duration_us << " us\n";

    if (ops_sec > 1000 && q_duration_us < 5000) {
        std::cout << "\n[SUCCESS] Distributed Cluster Performance Verified.\n";
    } else {
        std::cout << "\n[WARN] Cluster performance below theoretical peak.\n";
    }

    return 0;
}
