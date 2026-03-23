#include "L3KVG/Engine.hpp"
#include "L3KVG/Node.hpp"
#include "L3KVG/Query.hpp"
#include <chrono>
#include <filesystem>
#include <iostream>
#include <thread>
#include <vector>

int main() {
  std::string db_path =
      "test_l3kvg_bench_db_" +
      std::to_string(
          std::chrono::system_clock::now().time_since_epoch().count());
  std::filesystem::remove_all(db_path);
  auto engine = std::make_unique<l3kvg::Engine>(db_path, 2);

  std::cout << "Initializing 10,000 node test set...\n";
  engine->put_node("npc_root", R"({"id":"npc_root","type":"npc"})");
  for (int i = 0; i < 10000; ++i) {
    engine->put_node("npc_" + std::to_string(i), R"({"type":"npc"})");
  }

  std::cout << "--- Write Throughput Benchmark (Concurrent) ---\n";
  const int NUM_EDGES = 10000;
  const int NUM_THREADS = 100;
  const int EDGES_PER_THREAD = NUM_EDGES / NUM_THREADS;
  auto start_write = std::chrono::high_resolution_clock::now();

  std::vector<std::thread> threads;
  for (int t = 0; t < NUM_THREADS; ++t) {
    threads.emplace_back([&, t]() {
      for (int i = 0; i < EDGES_PER_THREAD; ++i) {
        int e_idx = t * EDGES_PER_THREAD + i;
        engine->add_edge("npc_root", "knows", 1.0,
                         "npc_" + std::to_string(e_idx));
      }
    });
  }
  for (auto &th : threads) {
    th.join();
  }

  auto end_write = std::chrono::high_resolution_clock::now();
  double write_ms =
      std::chrono::duration<double, std::milli>(end_write - start_write)
          .count();
  double ops_sec = (NUM_EDGES / write_ms) * 1000.0;

  std::cout << "Added " << NUM_EDGES << " atomic edges in " << write_ms
            << " ms.\n";
  std::cout << "Throughput: " << ops_sec << " ops/sec (Target: > 5,000)\n\n";

  std::cout << "--- Traversal Latency Benchmark ---\n";
  auto root = engine->get_node("npc_root");

  // Warm up cache
  auto warm = root->get_neighbors("knows", 0.0);
  std::cout << "Fan-out size on root: " << warm.size() << " edges.\n";

  const int NUM_READS = 10;
  auto start_read = std::chrono::high_resolution_clock::now();
  for (int i = 0; i < NUM_READS; ++i) {
    auto neighbors = root->get_neighbors("knows", 0.0);
    if (neighbors.empty()) {
      std::cerr << "Failure missing edges\n";
    }
  }
  auto end_read = std::chrono::high_resolution_clock::now();
  double read_us =
      std::chrono::duration<double, std::micro>(end_read - start_read).count() /
      NUM_READS;

  std::cout << "Avg Single-hop Traversal Latency (10k fan-out): " << read_us
            << " us\n";

  // Test typical fan-out (e.g. 50 edges)
  engine->put_node("npc_typical", R"({"type":"npc"})");
  for (int i = 0; i < 50; ++i) {
    engine->add_edge("npc_typical", "knows", 1.0, "npc_" + std::to_string(i));
  }
  auto typical_node = engine->get_node("npc_typical");
  auto warm2 = typical_node->get_neighbors("knows", 0.0);

  const int TYPICAL_READS = 100;
  auto t_start_read = std::chrono::high_resolution_clock::now();
  for (int i = 0; i < TYPICAL_READS; ++i) {
    auto neighbors = typical_node->get_neighbors("knows", 0.0);
  }
  auto t_end_read = std::chrono::high_resolution_clock::now();
  double t_read_us =
      std::chrono::duration<double, std::micro>(t_end_read - t_start_read)
          .count() /
      TYPICAL_READS;

  std::cout << "Avg Single-hop Traversal Latency (50 fan-out): " << t_read_us
            << " us (Target: < 500 us)\n";

  if (ops_sec >= 10000.0 && t_read_us <= 500.0) {
    std::cout << "\n[SUCCESS] SRE Metrics Achieved!\n";
    return 0;
  } else {
    std::cout << "\n[WARNING] Performance limits falling short of strict "
                 "constraints.\n";
    return 1;
  }
}
