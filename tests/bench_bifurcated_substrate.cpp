#include "L3KVG/Engine.hpp"
#include "L3KVG/Node.hpp"
#include "L3KVG/Query.hpp"
#include "L3KVG/Settings.hpp"
#include <chrono>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <string>
#include <vector>

using namespace l3kvg;

struct TempDbDir {
    std::string path;
    explicit TempDbDir(std::string p) : path(std::move(p)) {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
    ~TempDbDir() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
};

int main() {
    std::cout << "===============================================================\n";
    std::cout << "  L3KVG Native Bifurcated Substrate Benchmark & Qualification  \n";
    std::cout << "===============================================================\n\n";

    TempDbDir temp_dir("bench_bifurcated_substrate_db");
    Settings settings;
    // Set cache size large enough to hold benchmark working set
    settings.node_cache_size_per_shard = 20000;
    settings.node_cache_shards = 16;

    Engine engine(temp_dir.path, 1, nullptr, 4, settings);

    // =========================================================================
    // Measure 1: Single-thread Edge Ingestion Throughput
    // Seed 100,000 edges into l3kvg::Engine with add_edge(i, "CONTAINS", 1.0, i + 1).
    // Measure total seconds and edges/sec. SLA: > 150,000 edges/sec.
    // =========================================================================
    std::cout << "--- Measure 1: Single-thread Edge Ingestion Throughput ---\n";
    constexpr uint64_t NUM_INGEST_EDGES = 100000;
    std::cout << "Ingesting " << NUM_INGEST_EDGES << " edges into Engine...\n";

    // Isolated component timing
    {
        auto t0 = std::chrono::high_resolution_clock::now();
        for (uint64_t i = 0; i < 10000; ++i) {
            engine.get_label_registry().get_or_register("CONTAINS");
        }
        auto t1 = std::chrono::high_resolution_clock::now();
        for (uint64_t i = 0; i < 10000; ++i) {
            engine.get_hlc().now();
        }
        auto t2 = std::chrono::high_resolution_clock::now();
        if (engine.get_edge_wal()) {
            for (uint64_t i = 0; i < 10000; ++i) {
                EdgeMutationOp op{i, i + 1, 1000, 1, PackedEdge::FLAG_ACTIVE, 1.0f};
                engine.get_edge_wal()->append(op);
            }
        }
        auto t3 = std::chrono::high_resolution_clock::now();
        for (uint64_t i = 0; i < 10000; ++i) {
            engine.get_adjacency_store().add_edge(1000000 + i, 1000000 + i + 1, 1, 1.0f);
        }
        auto t4 = std::chrono::high_resolution_clock::now();
        
        std::cout << "[Probe] 10k label: " << std::chrono::duration<double, std::milli>(t1 - t0).count() << " ms, "
                  << "hlc: " << std::chrono::duration<double, std::milli>(t2 - t1).count() << " ms, "
                  << "wal: " << std::chrono::duration<double, std::milli>(t3 - t2).count() << " ms, "
                  << "adj: " << std::chrono::duration<double, std::milli>(t4 - t3).count() << " ms\n";
    }

    auto start_ingest = std::chrono::high_resolution_clock::now();
    for (uint64_t i = 0; i < NUM_INGEST_EDGES; ++i) {
        engine.add_edge(i, "CONTAINS", 1.0, i + 1);
    }
    auto end_ingest = std::chrono::high_resolution_clock::now();

    double ingest_sec = std::chrono::duration<double>(end_ingest - start_ingest).count();
    double edges_per_sec = static_cast<double>(NUM_INGEST_EDGES) / ingest_sec;

    std::cout << "Total time: " << std::fixed << std::setprecision(4) << ingest_sec << " s\n";
    std::cout << "Throughput: " << std::fixed << std::setprecision(2) << edges_per_sec << " edges/sec\n";
    std::cout << "SLA Requirement: > 150,000 edges/sec\n";
    if (edges_per_sec >= 150000.0) {
        std::cout << "Result: [PASSED] Throughput SLA met (" << edges_per_sec << " edges/sec)\n\n";
    } else {
        std::cout << "Result: [WARNING/FAIL] Throughput SLA not met (" << edges_per_sec << " edges/sec)\n\n";
    }

    // =========================================================================
    // Measure 2: Point Neighbor Traversal Latency (The Core < 50ns SLA)
    // Perform 1,000,000 neighbor traversals with engine.get_node(id)->get_neighbors("CONTAINS").
    // Measure total wall-clock time in nanoseconds and compute average nanoseconds per hop.
    // SLA: < 50 ns per hop.
    // =========================================================================
    std::cout << "--- Measure 2: Point Neighbor Traversal Latency (Core < 50ns SLA) ---\n";
    constexpr uint64_t NUM_TRAVERSALS = 1000000;
    std::cout << "Performing " << NUM_TRAVERSALS << " neighbor traversals (warm cache)...\n";

    // Warm-up cache for queried node(s)
    uint64_t test_node_id = 42;
    auto warm_node = engine.get_node(test_node_id);
    auto warm_res = warm_node->get_neighbors("CONTAINS");
    (void)warm_res;

    uint64_t total_hops_visited = 0;
    auto start_traversal = std::chrono::high_resolution_clock::now();
    for (uint64_t i = 0; i < NUM_TRAVERSALS; ++i) {
        auto neighbors = engine.get_node(test_node_id)->get_neighbors("CONTAINS");
        total_hops_visited += neighbors.size();
    }
    auto end_traversal = std::chrono::high_resolution_clock::now();

    // Isolated breakdown measurements:
    auto start_gn = std::chrono::high_resolution_clock::now();
    for (uint64_t i = 0; i < NUM_TRAVERSALS; ++i) {
        const auto& node = engine.get_node(test_node_id);
        (void)node;
    }
    auto end_gn = std::chrono::high_resolution_clock::now();
    double gn_ns = static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(end_gn - start_gn).count()) / NUM_TRAVERSALS;

    auto test_node_ptr = engine.get_node(test_node_id);
    auto start_nb = std::chrono::high_resolution_clock::now();
    for (uint64_t i = 0; i < NUM_TRAVERSALS; ++i) {
        auto neighbors = test_node_ptr->get_neighbors("CONTAINS");
        total_hops_visited += neighbors.size();
    }
    auto end_nb = std::chrono::high_resolution_clock::now();
    double nb_ns = static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(end_nb - start_nb).count()) / NUM_TRAVERSALS;

    auto total_traversal_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(end_traversal - start_traversal).count();
    double avg_ns_per_hop = static_cast<double>(total_traversal_ns) / static_cast<double>(NUM_TRAVERSALS);

    std::cout << "Total Traversal Wall-Clock: " << total_traversal_ns << " ns\n";
    std::cout << "Breakdown: get_node() avg: " << gn_ns << " ns, get_neighbors() avg: " << nb_ns << " ns\n";
    std::cout << "Combined avg: " << std::fixed << std::setprecision(2) << avg_ns_per_hop << " ns per hop\n";
    std::cout << "SLA Requirement: < 50 ns per hop\n";
    if (avg_ns_per_hop < 50.0) {
        std::cout << "Result: [PASSED] Sub-50ns traversal SLA met (" << avg_ns_per_hop << " ns/hop)\n\n";
    } else {
        std::cout << "Result: [FAIL] Sub-50ns traversal SLA not met (" << avg_ns_per_hop << " ns/hop)\n\n";
    }

    // =========================================================================
    // Measure 3: SIMD Vectorized Predicate Filter Latency
    // Seed nodes with 1,000 alternating edges with varying weights.
    // Perform 100,000 filtered traversals with min_weight threshold.
    // Measure average nanoseconds per traversal.
    // =========================================================================
    std::cout << "--- Measure 3: SIMD Vectorized Predicate Filter Latency ---\n";
    uint64_t simd_node_id = 9999999;
    constexpr uint64_t SIMD_EDGES_COUNT = 1000;
    std::cout << "Seeding node " << simd_node_id << " with " << SIMD_EDGES_COUNT << " alternating edges & weights...\n";

    for (uint64_t i = 0; i < SIMD_EDGES_COUNT; ++i) {
        std::string label = (i % 2 == 0) ? "SIMD_A" : "SIMD_B";
        double weight = static_cast<double>(i) * 0.1;
        engine.add_edge(simd_node_id, label, weight, 10000000 + i);
    }

    constexpr uint64_t NUM_FILTER_TRAVERSALS = 100000;
    std::cout << "Performing " << NUM_FILTER_TRAVERSALS << " SIMD-filtered traversals (min_weight = 50.0)...\n";

    auto simd_node = engine.get_node(simd_node_id);
    // Warm up
    auto warm_filter = simd_node->get_neighbors("SIMD_A", 50.0);
    uint64_t filter_matches = 0;

    auto start_filter = std::chrono::high_resolution_clock::now();
    for (uint64_t i = 0; i < NUM_FILTER_TRAVERSALS; ++i) {
        auto node = engine.get_node(simd_node_id);
        auto neighbors = node->get_neighbors("SIMD_A", 50.0);
        filter_matches += neighbors.size();
    }
    auto end_filter = std::chrono::high_resolution_clock::now();

    auto total_filter_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(end_filter - start_filter).count();
    double avg_filter_ns = static_cast<double>(total_filter_ns) / static_cast<double>(NUM_FILTER_TRAVERSALS);

    std::cout << "Total Filter Wall-Clock: " << total_filter_ns << " ns\n";
    std::cout << "Traversals: " << NUM_FILTER_TRAVERSALS << ", Total Matches: " << filter_matches 
              << " (" << (filter_matches / NUM_FILTER_TRAVERSALS) << " per query)\n";
    std::cout << "Avg Latency: " << std::fixed << std::setprecision(2) << avg_filter_ns << " ns per traversal\n\n";

    // =========================================================================
    // Measure 4: High-Fanout Chained Page Traversal
    // Traverse supernode with degree > 500 across chained 4KB AdjacencyPages.
    // Measure latency and confirm all edges reached.
    // =========================================================================
    std::cout << "--- Measure 4: High-Fanout Chained Page Traversal ---\n";
    uint64_t supernode_id = 8888888;
    constexpr uint64_t SUPERNODE_DEGREE = 600; // > 500 across ~4 AdjacencyPages
    std::cout << "Seeding supernode " << supernode_id << " with degree " << SUPERNODE_DEGREE << " (> 500)...\n";

    for (uint64_t i = 0; i < SUPERNODE_DEGREE; ++i) {
        engine.add_edge(supernode_id, "SUPER_EDGE", 1.0, 20000000 + i);
    }

    auto supernode = engine.get_node(supernode_id);
    constexpr uint64_t NUM_SUPERNODE_TRAVERSALS = 10000;

    auto start_super = std::chrono::high_resolution_clock::now();
    uint64_t super_edges_counted = 0;
    for (uint64_t i = 0; i < NUM_SUPERNODE_TRAVERSALS; ++i) {
        auto node = engine.get_node(supernode_id);
        auto neighbors = node->get_neighbors("SUPER_EDGE");
        super_edges_counted += neighbors.size();
    }
    auto end_super = std::chrono::high_resolution_clock::now();

    auto total_super_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(end_super - start_super).count();
    double avg_super_ns = static_cast<double>(total_super_ns) / static_cast<double>(NUM_SUPERNODE_TRAVERSALS);
    uint64_t edges_per_traversal = super_edges_counted / NUM_SUPERNODE_TRAVERSALS;

    std::cout << "Traversed " << NUM_SUPERNODE_TRAVERSALS << " times across chained 4KB pages.\n";
    std::cout << "Edges reached per traversal: " << edges_per_traversal << " (Expected: " << SUPERNODE_DEGREE << ")\n";
    std::cout << "Avg Latency: " << std::fixed << std::setprecision(2) << avg_super_ns << " ns (" 
              << std::setprecision(3) << (avg_super_ns / 1000.0) << " us)\n";

    if (edges_per_traversal == SUPERNODE_DEGREE) {
        std::cout << "Result: [PASSED] All " << SUPERNODE_DEGREE << " chained edges confirmed reached.\n\n";
    } else {
        std::cout << "Result: [FAIL] Expected " << SUPERNODE_DEGREE << " edges but got " << edges_per_traversal << "\n\n";
        return 1;
    }

    std::cout << "===============================================================\n";
    std::cout << "  All Bifurcated Substrate Benchmark Tests Completed!         \n";
    std::cout << "===============================================================\n";

    return 0;
}
