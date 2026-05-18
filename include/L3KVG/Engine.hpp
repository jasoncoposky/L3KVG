#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <list>
#include <vector>
#include <cstdint>

#include "L3KVG/FederationResolver.hpp"
#include "L3KVG/RemoteL3KVClient.hpp"
#include "L3KVG/EdgeCoordinator.hpp"
#include "L3KVG/ThreadPool.hpp"
#include "L3KVG/Settings.hpp"
#include "L3KVG/HLC.hpp"

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
  Engine(const std::string &db_path, uint32_t node_id = 1, std::shared_ptr<lite3::ConsistentHash> ring = nullptr, size_t thread_pool_size = 4, Settings settings = {});
  ~Engine();

  Engine(const Engine &) = delete;
  Engine &operator=(const Engine &) = delete;

  Query query();

  std::shared_ptr<Node> get_node(uint64_t id);
  std::shared_ptr<Node> get_node(std::string_view uuid);
  
  std::vector<std::shared_ptr<Node>> fetch_nodes(const std::vector<uint64_t>& ids);
  std::vector<std::shared_ptr<Node>> get_nodes_by_prefix(const std::string& prefix);
  
  void put_node(uint64_t id, std::string payload);
  void put_node(std::string_view uuid, std::string payload);
  
  // Replication Interface
  void replicate_key(const std::string& key, std::string payload, uint16_t origin_cluster_id);
  void broadcast_replication(const std::string& key, const std::string& payload, uint16_t origin_cluster_id = 0);

  void del_node(uint64_t id);
  void flush();

  static std::string format_weight(double weight);

  void add_edge(uint64_t src_id, std::string label,
                double weight, uint64_t dst_id, 
                std::string payload = "");
  void add_edge(std::string_view src_uuid, std::string label,
                double weight, std::string_view dst_uuid,
                std::string payload = "");

  void del_edge(uint64_t src_id, std::string label,
                double weight, uint64_t dst_id);

  // Mechanical Sympathy & HPC APIs
  SREMetrics &get_metrics() { return metrics_; }
  const Settings& get_settings() const { return settings_; }

  // Pointer Swizzling Registry
  void swizzle_node(uint64_t id, std::shared_ptr<Node> ptr);
  std::shared_ptr<Node> get_swizzled(uint64_t id);

  l3kv::Engine *get_store() const { return store_.get(); }
  
  FederationResolver& get_resolver() { return resolver_; }
  RemoteL3KVClient& get_remote_client() { return *remote_client_; }
  void set_remote_client(std::unique_ptr<RemoteL3KVClient> client) { remote_client_ = std::move(client); }
  EdgeCoordinator& get_edge_coordinator() { return *edge_coordinator_; }
  HLCProvider& get_hlc() { return hlc_; }
  ThreadPool& get_thread_pool() { return *pool_; }
  std::shared_ptr<ThreadPool> get_thread_pool_ptr() { return pool_; }

private:
  struct CacheShard {
    std::mutex mutex;
    std::unordered_map<uint64_t, std::shared_ptr<Node>> map;
    std::list<uint64_t> lru;
  };

  size_t get_cache_shard(uint64_t id);

  std::unique_ptr<l3kv::Engine> store_;
  FederationResolver resolver_;
  std::unique_ptr<RemoteL3KVClient> remote_client_;
  std::unique_ptr<EdgeCoordinator> edge_coordinator_;
  std::shared_ptr<ThreadPool> pool_;
  Settings settings_;
  
  std::vector<std::unique_ptr<CacheShard>> cache_shards_;
  
  HLCProvider hlc_;
  SREMetrics metrics_;
};

} // namespace l3kvg
