#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>

#include "L3KVG/ClusterResolver.hpp"
#include "L3KVG/RemoteL3KVClient.hpp"
#include "L3KVG/EdgeCoordinator.hpp"

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
  Engine(const std::string &db_path, uint32_t node_id = 1, std::shared_ptr<lite3::ConsistentHash> ring = nullptr);
  ~Engine();

  Engine(const Engine &) = delete;
  Engine &operator=(const Engine &) = delete;

  Query query();

  std::shared_ptr<Node> get_node(std::string_view uuid);
  std::vector<std::shared_ptr<Node>> fetch_nodes(const std::vector<std::string>& uuids);
  void put_node(std::string_view uuid, const std::string &json_payload);

  static std::string format_weight(double weight);

  void add_edge(std::string_view src_uuid, std::string_view label,
                double weight, std::string_view dst_uuid, 
                const std::string &payload = "");

  // Mechanical Sympathy & HPC APIs
  SREMetrics &get_metrics() { return metrics_; }

  // Pointer Swizzling Registry
  void swizzle_node(std::string_view uuid, std::shared_ptr<Node> ptr);
  std::shared_ptr<Node> get_swizzled(std::string_view uuid);

  l3kv::Engine *get_store() const { return store_.get(); }
  
  ClusterResolver& get_resolver() { return resolver_; }
  RemoteL3KVClient& get_remote_client() { return remote_client_; }
  EdgeCoordinator& get_edge_coordinator() { return *edge_coordinator_; }

private:
  std::unique_ptr<l3kv::Engine> store_;
  ClusterResolver resolver_;
  RemoteL3KVClient remote_client_;
  std::unique_ptr<EdgeCoordinator> edge_coordinator_;
  std::mutex cache_mutex_;
  std::unordered_map<std::string, std::shared_ptr<Node>> node_cache_;
  SREMetrics metrics_;
};

} // namespace l3kvg
