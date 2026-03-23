#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>

namespace l3kv {
class Engine;
}

namespace l3kvg {

class Node;
class Query;

struct SREMetrics {
  std::atomic<uint64_t> hop_latency_us{0};
  std::atomic<uint64_t> serialization_time_us{0};
  std::atomic<uint64_t> cache_hits{0};
  std::atomic<uint64_t> cache_misses{0};

  double get_hit_ratio() const {
    uint64_t hits = cache_hits.load();
    uint64_t total = hits + cache_misses.load();
    return total == 0 ? 0.0 : static_cast<double>(hits) / total;
  }
};

class Engine {
public:
  Engine(const std::string &db_path, uint32_t node_id = 1);
  ~Engine();

  Engine(const Engine &) = delete;
  Engine &operator=(const Engine &) = delete;

  Query query();

  std::shared_ptr<Node> get_node(std::string_view uuid);
  void put_node(std::string_view uuid, const std::string &json_payload);

  static std::string format_weight(double weight);

  void add_edge(std::string_view src_uuid, std::string_view label,
                double weight, std::string_view dst_uuid);

  // Mechanical Sympathy & HPC APIs
  SREMetrics &get_metrics() { return metrics_; }

  // Pointer Swizzling Registry
  void swizzle_node(std::string_view uuid, std::shared_ptr<Node> ptr);
  std::shared_ptr<Node> get_swizzled(std::string_view uuid);

  l3kv::Engine *get_store() const { return store_.get(); }

private:
  std::unique_ptr<l3kv::Engine> store_;
  std::mutex cache_mutex_;
  std::unordered_map<std::string, std::shared_ptr<Node>> node_cache_;
  SREMetrics metrics_;
};

} // namespace l3kvg
