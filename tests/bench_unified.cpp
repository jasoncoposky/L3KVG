#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
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
#include "engine/store.hpp"
#include <nlohmann/json.hpp>

using json = nlohmann::json;

void run_kv_bench(l3kvg::EdgeCoordinator& coordinator, int num_ops, int threads) {
    std::cout << "--- Phase 1: L3KV Unified KV Updates (" << num_ops << " ops, " << threads << " threads) ---\n";
    std::atomic<int> completed{0};
    auto start = std::chrono::high_resolution_clock::now();

    std::vector<std::thread> workers;
    for (int t = 0; t < threads; ++t) {
        workers.emplace_back([&, t]() {
            for (int i = 0; i < num_ops / threads; ++i) {
                std::string key = "kv_key_" + std::to_string(t * (num_ops / threads) + i);
                std::string val = "{\"value\": \"data_" + std::to_string(i) + "\"}";
                
                // We use EdgeCoordinator's store to test the underlying KV performance
                // Note: EdgeCoordinator doesn't have a direct raw put yet, so we use the store directly
                // but since it's a distributed client, we'd normally use RemoteL3KVClient.
                // For this unified bench, we'll just test the Graph Edges as the proxy for distributed load.
                completed++;
            }
        });
    }
    for (auto& w : workers) w.join();
    auto end = std::chrono::high_resolution_clock::now();
    double duration = std::chrono::duration<double>(end - start).count();
    std::cout << "KV Ops Phase Finished (Internal only in this proxy mode)\n";
}

void run_graph_bench(l3kvg::EdgeCoordinator& coordinator, int num_edges, int threads) {
    std::cout << "--- Phase 2: L3KVG Unified Edge Creation (" << num_edges << " edges, " << threads << " threads) ---\n";
    std::atomic<int> completed{0};
    auto start = std::chrono::high_resolution_clock::now();

    std::vector<std::thread> workers;
    for (int t = 0; t < threads; ++t) {
        workers.emplace_back([&, t]() {
            try {
                for (int i = 0; i < num_edges / threads; ++i) {
                    int src_idx = (t * (num_edges / threads) + i) % 10000;
                    int dst_idx = (src_idx + 1) % 10000;
                    
                    std::string src = "u_" + std::to_string(src_idx);
                    std::string dst = "u_" + std::to_string(dst_idx);
                    
                    auto fut = coordinator.atomic_put_edge(src, "knows", 1.0, dst, "{\"meta\": \"bench\"}");
                    if (i == (num_edges / threads) - 1) fut.get();
                    completed++;
                }
            } catch (const std::exception& e) {
                std::cerr << "Worker error: " << e.what() << "\n";
            }
        });
    }
    for (auto& w : workers) w.join();
    
    auto end = std::chrono::high_resolution_clock::now();
    double duration = std::chrono::duration<double>(end - start).count();
    double tps = completed.load() / duration;
    
    std::cout << "Completed " << completed.load() << " edges in " << std::fixed << std::setprecision(2) << duration << "s (" << tps << " edges/sec)\n";
    std::cout << "Throughput: " << (int)tps << " edges/sec\n";
}

int main() {
#ifdef _WIN32
    WSADATA wsaData;
    WSAStartup(MAKEWORD(2, 2), &wsaData);
#endif

    try {
        auto ring = std::make_shared<lite3::ConsistentHash>();
        ring->add_node(1);
        ring->add_node(2);
        ring->add_node(3);

        l3kvg::ClusterResolver resolver(ring, 1);
        l3kvg::RemoteL3KVClient remote_client;
        remote_client.add_peer(1, "tcp://127.0.0.1:8081");
        remote_client.add_peer(2, "tcp://127.0.0.1:8082");
        remote_client.add_peer(3, "tcp://127.0.0.1:8083");


        auto engine_kv = std::make_unique<l3kv::Engine>("bench_unified_db", 1);
        l3kvg::EdgeCoordinator coordinator(engine_kv.get(), resolver, remote_client, 1);

        run_graph_bench(coordinator, 100000, 16);

    } catch (const std::exception& e) {
        std::cerr << "Critical Error: " << e.what() << "\n";
        return 1;
    }

#ifdef _WIN32
    WSACleanup();
#endif
    return 0;
}
