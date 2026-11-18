#pragma once

#include <array>
#include <atomic>
#include <memory>
#include <mutex>
#include <shared_mutex>
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
#include "L3KVG/LabelRegistry.hpp"
#include "L3KVG/AdjacencyStore.hpp"
#include "L3KVG/BinaryEdgeWAL.hpp"
#include "engine/credential_manager.hpp"

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

  const std::shared_ptr<Node>& get_node(uint64_t id);
  const std::shared_ptr<Node>& get_node(std::string_view uuid);
  
  std::vector<std::shared_ptr<Node>> fetch_nodes(const std::vector<uint64_t>& ids, uint32_t principal_id = l3kv::INTERNAL_UID);
  std::vector<std::shared_ptr<Node>> get_nodes_by_prefix(const std::string& prefix, uint32_t principal_id = l3kv::INTERNAL_UID);
  std::vector<std::pair<std::string, lite3cpp::Buffer>> scan_kv_prefix(
      const std::string& prefix,
      const std::string& start_key = "",
      size_t limit = 1000,
      uint32_t principal_id = l3kv::INTERNAL_UID);
  
  void put_node(uint64_t id, std::string payload);
  void put_node(std::string_view uuid, std::string payload);
  
  // Replication & Security Interface
  void replicate_key(const std::string& key, std::string payload, uint16_t origin_cluster_id);
  void broadcast_replication(const std::string& key, const std::string& payload, uint16_t origin_cluster_id = 0);
  void put_system_key(const std::string& key, const std::string& payload, uint32_t principal_id = ADMIN_UID);

  void del_node(uint64_t id);
  void flush();
  void compact_wal();

  static std::string format_weight(double weight);

  void add_edge(uint64_t src_id, std::string_view label,
                double weight, uint64_t dst_id, 
                std::string_view payload = "");
  void add_edge(std::string_view src_uuid, std::string_view label,
                double weight, std::string_view dst_uuid,
                std::string_view payload = "");

  void del_edge(uint64_t src_id, std::string_view label,
                double weight, uint64_t dst_id);

  bool apply_batch(const lite3cpp::Buffer& buffer, uint32_t principal_id = l3kv::INTERNAL_UID);

  // Mechanical Sympathy & HPC APIs
  SREMetrics &get_metrics() { return metrics_; }
  const Settings& get_settings() const { return settings_; }

  // Pointer Swizzling Registry
  void swizzle_node(uint64_t id, std::shared_ptr<Node> ptr);
  void swizzle_node(uint64_t id, const std::string& payload);
  std::shared_ptr<Node> get_swizzled(uint64_t id);

  l3kv::Engine *get_store() const { return store_.get(); }
  
  FederationResolver& get_resolver() { return resolver_; }
  RemoteL3KVClient& get_remote_client() { return *remote_client_; }
  void set_remote_client(std::unique_ptr<RemoteL3KVClient> client);
  void set_auth_secret(const std::string& secret) { remote_client_->set_auth_secret(secret); }
  EdgeCoordinator& get_edge_coordinator() { return *edge_coordinator_; }
  HLCProvider& get_hlc() { return hlc_; }
  ThreadPool& get_thread_pool() { return *pool_; }
  std::shared_ptr<ThreadPool> get_thread_pool_ptr() { return pool_; }

  // Native Graph Substrate Accessors
  AdjacencyStore& get_adjacency_store() { return adjacency_store_; }
  const AdjacencyStore& get_adjacency_store() const { return adjacency_store_; }
  LabelRegistry& get_label_registry() { return label_registry_; }
  const LabelRegistry& get_label_registry() const { return label_registry_; }
  BinaryEdgeWAL* get_edge_wal() { return edge_wal_.get(); }
  const BinaryEdgeWAL* get_edge_wal() const { return edge_wal_.get(); }
  [[nodiscard]] uint64_t get_cache_epoch() const noexcept { return cache_epoch_.load(std::memory_order_relaxed); }
  [[nodiscard]] bool has_legacy_kv_edges() const noexcept { return has_legacy_kv_edges_.load(std::memory_order_relaxed); }
  void mark_legacy_kv_edges() noexcept { has_legacy_kv_edges_.store(true, std::memory_order_relaxed); }

private:
  struct CacheShard {
    mutable std::mutex mutex;
    using LruList = std::list<uint64_t>;
    struct CacheEntry {
      std::shared_ptr<Node> node;
      LruList::iterator lru_it;
    };
    std::unordered_map<uint64_t, CacheEntry> map;
    LruList lru;
    std::atomic<size_t> count{0};
  };

  size_t get_cache_shard(uint64_t id);
  void invalidate_node_cache(uint64_t id);

  std::string db_path_;
  std::string dict_path_;
  LabelRegistry label_registry_;
  AdjacencyStore adjacency_store_;
  std::unique_ptr<BinaryEdgeWAL> edge_wal_;
  std::shared_mutex edge_global_mu_;
  std::array<std::mutex, 256> edge_op_locks_;
  std::array<std::mutex, 64> node_rmw_locks_;

  static constexpr size_t edge_stripe(uint64_t src, uint64_t dst) noexcept {
    uint64_t h = (src * 0x9E3779B97F4A7C15ULL) ^ (dst * 0xBF58476D1CE4E5B9ULL);
    return static_cast<size_t>((h ^ (h >> 32)) & 255ULL);
  }

  std::unique_ptr<l3kv::Engine> store_;
  FederationResolver resolver_;
  std::unique_ptr<RemoteL3KVClient> remote_client_;
  std::unique_ptr<EdgeCoordinator> edge_coordinator_;
  std::shared_ptr<ThreadPool> pool_;
  Settings settings_;
  
  std::vector<std::unique_ptr<CacheShard>> cache_shards_;
  std::atomic<uint64_t> cache_epoch_{1};
  std::atomic<bool> has_cached_nodes_{false};
  std::atomic<bool> has_legacy_kv_edges_{false};
  uint64_t engine_instance_id_{0};
  
  HLCProvider hlc_;
  SREMetrics metrics_;
};

} // namespace l3kvg
