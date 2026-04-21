#include "L3KVG/Engine.hpp"
#include "L3KVG/Node.hpp"
#include "L3KVG/Query.hpp"
#include "L3KVG/KeyBuilder.hpp"
#include "engine/store.hpp"
#include <iomanip>
#include <sstream>
#include <iostream>

namespace l3kvg {

Engine::Engine(const std::string &db_path, uint32_t node_id, std::shared_ptr<lite3::ConsistentHash> ring)
    : resolver_(std::move(ring), node_id) {
  store_ = std::make_unique<l3kv::Engine>(db_path, node_id);
  edge_coordinator_ = std::make_unique<EdgeCoordinator>(store_.get(), resolver_, remote_client_, node_id);
  
  for (size_t i = 0; i < CACHE_SHARDS; ++i) {
    cache_shards_.push_back(std::make_unique<CacheShard>());
  }
}

Engine::~Engine() = default;

size_t Engine::get_cache_shard(std::string_view uuid) {
    return std::hash<std::string_view>{}(uuid) % CACHE_SHARDS;
}

Query Engine::query() { return Query(this); }

std::shared_ptr<Node> Engine::get_node(std::string_view uuid) {
  std::string suuid(uuid);
  size_t h = get_cache_shard(uuid);
  auto& shard = *cache_shards_[h];

  {
    std::lock_guard<std::mutex> lock(shard.mutex);
    if (auto it = shard.map.find(suuid); it != shard.map.end()) {
      // LRU update
      shard.lru.remove(suuid);
      shard.lru.push_front(suuid);
      return it->second;
    }
  }

  auto node = std::make_shared<Node>(this, suuid);
  {
    std::lock_guard<std::mutex> lock(shard.mutex);
    if (shard.map.size() >= CacheShard::MAX_SHARD_SIZE) {
        std::string victim = shard.lru.back();
        shard.map.erase(victim);
        shard.lru.pop_back();
    }
    shard.map[suuid] = node;
    shard.lru.push_front(suuid);
  }
  metrics_.cache_misses.fetch_add(1, std::memory_order_relaxed);
  return node;
}

void Engine::swizzle_node(std::string_view uuid, std::shared_ptr<Node> ptr) {
  std::string suuid(uuid);
  size_t h = get_cache_shard(uuid);
  auto& shard = *cache_shards_[h];

  std::lock_guard<std::mutex> lock(shard.mutex);
  if (shard.map.contains(suuid)) {
      shard.lru.remove(suuid);
  } else if (shard.map.size() >= CacheShard::MAX_SHARD_SIZE) {
      std::string victim = shard.lru.back();
      shard.map.erase(victim);
      shard.lru.pop_back();
  }
  shard.map[suuid] = ptr;
  shard.lru.push_front(suuid);
  metrics_.cache_hits.fetch_add(1, std::memory_order_relaxed);
}

std::shared_ptr<Node> Engine::get_swizzled(std::string_view uuid) {
  std::string suuid(uuid);
  size_t h = get_cache_shard(uuid);
  auto& shard = *cache_shards_[h];

  std::lock_guard<std::mutex> lock(shard.mutex);
  if (auto it = shard.map.find(suuid); it != shard.map.end()) {
    shard.lru.remove(suuid);
    shard.lru.push_front(suuid);
    metrics_.cache_hits.fetch_add(1, std::memory_order_relaxed);
    return it->second;
  }
  metrics_.cache_misses.fetch_add(1, std::memory_order_relaxed);
  return nullptr;
}

std::vector<std::shared_ptr<Node>> Engine::fetch_nodes(const std::vector<std::string>& uuids) {
  std::unordered_map<lite3::NodeID, std::vector<std::string>> remote_requests;
  std::vector<std::shared_ptr<Node>> result;
  result.reserve(uuids.size());

  for (const auto& uuid : uuids) {
    auto node = get_node(uuid);
    if (node && !node->is_loaded()) {
      // Locality of Reference: Try local store even if we are not the primary owner.
      std::string key = std::string(KeyBuilder::node_key(uuid));
      auto buf = store_->get(key);
      if (buf.size() > 0) {
          node->hydrate(std::string(reinterpret_cast<const char*>(buf.data()), buf.size()));
      } else {
          lite3::NodeID owner = resolver_.get_node_owner(uuid);
          if (owner != resolver_.get_local_node_id()) {
            remote_requests[owner].push_back(uuid);
          }
      }
    }
    result.push_back(node);
  }

  if (remote_requests.empty()) return result;

  std::vector<std::pair<lite3::NodeID, std::future<std::unordered_map<std::string, std::string>>>> futures;
  for (auto& [owner, batch] : remote_requests) {
    futures.push_back({owner, remote_client_.get_nodes_batch_async(owner, batch)});
  }

  for (auto& pair : futures) {
    try {
      auto batch_results = pair.second.get();
      for (auto& [uuid, payload] : batch_results) {
        auto node = get_node(uuid);
        if (node) {
            node->hydrate(payload);
        }
      }
    } catch (const std::exception& e) {
      std::cerr << "[Engine::fetch_nodes] Batch RPC to node " << pair.first << " failed: " << e.what() << "\n";
    }
  }

  return result;
}

void Engine::put_node(std::string_view uuid, const std::string &json_payload) {
  lite3::NodeID owner = resolver_.get_node_owner(std::string(uuid));
  
  if (owner != resolver_.get_local_node_id()) {
    try {
        remote_client_.put_node_async(owner, std::string(uuid), json_payload);
        return;
    } catch (const std::exception& e) {
        std::cerr << "[Engine::put_node] Remote RPC Failed: " << e.what() << "\n";
    }
  }

  std::string key = std::string(KeyBuilder::node_key(uuid));
  store_->put(key, json_payload);
}

std::string Engine::format_weight(double weight) {
  return std::string(KeyBuilder::format_weight(weight));
}

void Engine::add_edge(std::string_view src_uuid, std::string_view label,
                      double weight, std::string_view dst_uuid,
                      const std::string &payload) {
  edge_coordinator_->atomic_put_edge(std::string(src_uuid), std::string(label), weight, std::string(dst_uuid), payload);
}

} // namespace l3kvg
