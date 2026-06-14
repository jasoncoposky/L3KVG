#include "L3KVG/Engine.hpp"
#include "L3KVG/Node.hpp"
#include "L3KVG/Query.hpp"
#include <chrono>
#include <iostream>
#include <vector>
#include <filesystem>

using namespace l3kvg;

int main() {
    std::string db_path = "bench_core.db";
    std::filesystem::remove_all(db_path);
    
    Settings settings;
    Engine engine(db_path, 1, nullptr, 4, settings);

    const int num_nodes = 10000;
    std::cout << "Benchmarking raw Engine::put_node performance (" << num_nodes << " nodes)...\n";
    
    auto start_put = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < num_nodes; ++i) {
        engine.put_node(0x1000 + i, "{\"n\":\"test\", \"v\":" + std::to_string(i) + "}");
    }
    auto end_put = std::chrono::high_resolution_clock::now();
    double put_ms = std::chrono::duration<double, std::milli>(end_put - start_put).count();
    std::cout << "Total time: " << put_ms << " ms\n";
    std::cout << "Throughput: " << (num_nodes * 1000.0 / put_ms) << " nodes/sec\n";

    std::cout << "Benchmarking raw Engine::get_node performance (" << num_nodes << " nodes)...\n";
    auto start_get = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < num_nodes; ++i) {
        auto node = engine.get_node(0x1000 + i);
        if (!node) std::cerr << "Get failed for " << i << "\n";
    }
    auto end_get = std::chrono::high_resolution_clock::now();
    double get_ms = std::chrono::duration<double, std::milli>(end_get - start_get).count();
    std::cout << "Total time: " << get_ms << " ms\n";
    std::cout << "Avg Latency: " << (get_ms * 1000.0 / num_nodes) << " us\n";

    std::filesystem::remove_all(db_path);
    return 0;
}
