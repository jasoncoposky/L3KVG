#include "L3KVG/Engine.hpp"
#include "L3KVG/Node.hpp"
#include "L3KVG/Query.hpp"
#include "L3KVG/KeyBuilder.hpp"
#include "engine/store.hpp"
#include <iomanip>
#include <sstream>
#include <iostream>

namespace l3kvg {

Engine::Engine(const std::string &db_path, uint32_t node_id, std::shared_ptr<lite3::ConsistentHash> ring, size_t thread_pool_size, Settings settings)
    : resolver_(std::move(ring), node_id), settings_(std::move(settings)) {
  store_ = std::make_unique<l3kv::Engine>(db_path, node_id);
  remote_client_ = std::make_unique<RemoteL3KVClient>(settings_);
  pool_ = std::make_shared<ThreadPool>(thread_pool_size);
  remote_client_->set_thread_pool(pool_);
  edge_coordinator_ = std::make_unique<EdgeCoordinator>(store_.get(), resolver_, *remote_client_, node_id, pool_, settings_);
  
  for (size_t i = 0; i < settings_.node_cache_shards; ++i) {
    cache_shards_.push_back(std::make_unique<CacheShard>());
  }
}

Engine::~Engine() = default;

size_t Engine::get_cache_shard(uint64_t id) {
    return std::hash<uint64_t>{}(id) % settings_.node_cache_shards;
}

Query Engine::query() { return Query(this); }

std::shared_ptr<Node> Engine::get_node(uint64_t id) {
  size_t h = get_cache_shard(id);
  auto& shard = *cache_shards_[h];

  {
    std::lock_guard<std::mutex> lock(shard.mutex);
    if (auto it = shard.map.find(id); it != shard.map.end()) {
      // LRU update
      shard.lru.remove(id);
      shard.lru.push_front(id);
      return it->second;
    }
  }

  auto node = std::make_shared<Node>(this, id);
  {
    std::lock_guard<std::mutex> lock(shard.mutex);
    if (shard.map.size() >= settings_.node_cache_size_per_shard) {
        uint64_t victim = shard.lru.back();
        shard.map.erase(victim);
        shard.lru.pop_back();
    }
    shard.map[id] = node;
    shard.lru.push_front(id);
  }
  metrics_.cache_misses.fetch_add(1, std::memory_order_relaxed);
  return node;
}

std::shared_ptr<Node> Engine::get_node(std::string_view uuid) {
    return get_node(resolver_.parse_uuid(uuid));
}

void Engine::swizzle_node(uint64_t id, std::shared_ptr<Node> ptr) {
  size_t h = get_cache_shard(id);
  auto& shard = *cache_shards_[h];

  std::lock_guard<std::mutex> lock(shard.mutex);
  if (shard.map.contains(id)) {
      shard.lru.remove(id);
  } else if (shard.map.size() >= settings_.node_cache_size_per_shard) {
      uint64_t victim = shard.lru.back();
      shard.map.erase(victim);
      shard.lru.pop_back();
  }
  shard.map[id] = ptr;
  shard.lru.push_front(id);
  metrics_.cache_hits.fetch_add(1, std::memory_order_relaxed);
}

std::shared_ptr<Node> Engine::get_swizzled(uint64_t id) {
  size_t h = get_cache_shard(id);
  auto& shard = *cache_shards_[h];

  std::lock_guard<std::mutex> lock(shard.mutex);
  if (auto it = shard.map.find(id); it != shard.map.end()) {
    shard.lru.remove(id);
    shard.lru.push_front(id);
    metrics_.cache_hits.fetch_add(1, std::memory_order_relaxed);
    return it->second;
  }
  metrics_.cache_misses.fetch_add(1, std::memory_order_relaxed);
  return nullptr;
}

std::vector<std::shared_ptr<Node>> Engine::fetch_nodes(const std::vector<uint64_t>& ids) {
  std::unordered_map<lite3::NodeID, std::vector<uint64_t>> remote_requests;
  std::vector<std::shared_ptr<Node>> result;
  result.reserve(ids.size());

  for (const auto& id : ids) {
    auto node = get_node(id);
    if (node && !node->is_loaded()) {
      // Locality of Reference: Try local store even if we are not the primary owner.
      std::string key = std::string(KeyBuilder::node_key(id));
      auto buf = store_->get(key);
      if (buf.size() > 0) {
          node->hydrate(std::string(reinterpret_cast<const char*>(buf.data()), buf.size()));
      } else {
          lite3::NodeID owner = resolver_.get_node_owner(id);
          if (owner != resolver_.get_local_node_id()) {
            remote_requests[owner].push_back(id);
          }
      }
    }
    result.push_back(node);
  }

  if (remote_requests.empty()) return result;

  std::vector<std::pair<lite3::NodeID, std::future<std::unordered_map<uint64_t, std::string>>>> futures;
  for (auto& [owner, batch] : remote_requests) {
    futures.push_back({owner, remote_client_->get_nodes_batch_async(owner, batch)});
  }

  for (auto& pair : futures) {
    try {
      auto batch_results = pair.second.get();
      for (auto& [id, payload] : batch_results) {
        auto node = get_node(id);
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

std::vector<std::shared_ptr<Node>> Engine::get_nodes_by_prefix(const std::string& prefix) {
  auto keys = store_->get_prefix_keys_all_shards(prefix, "", settings_.prefix_scan_limit);
  std::vector<uint64_t> ids;
  for (auto& k : keys) {
      // Keys are n:{%016llx}
      if (k.starts_with("n:{") && k.ends_with("}")) {
          std::string id_str = k.substr(3, k.size() - 4);
          ids.push_back(std::stoull(id_str, nullptr, 16));
      }
  }
  return fetch_nodes(ids);
}

void Engine::put_node(uint64_t id, std::string payload) {
  lite3::NodeID owner = resolver_.get_node_owner(id);
  
  if (owner != resolver_.get_local_node_id()) {
    try {
        remote_client_->put_node_async(owner, id, payload);
        return;
    } catch (const std::exception& e) {
        std::cerr << "[Engine::put_node] Remote RPC Failed: " << e.what() << "\n";
    }
  }

  std::string key = std::string(KeyBuilder::node_key(id));
  store_->put(std::move(key), std::move(payload));
  store_->wait_all_shards();
}

void Engine::put_node(std::string_view uuid, std::string payload) {
    put_node(resolver_.parse_uuid(uuid), std::move(payload));
}

void Engine::del_node(uint64_t id) {
  lite3::NodeID owner = resolver_.get_node_owner(id);
  
  if (owner != resolver_.get_local_node_id()) {
    // Phase 5 Pending: Remote del_node RPC
    return;
  }

  std::string key = std::string(KeyBuilder::node_key(id));
  store_->del(key);
  store_->wait_all_shards();
}

void Engine::flush() {
  store_->wait_all_shards();
}

std::string Engine::format_weight(double weight) {
  return std::string(KeyBuilder::format_weight(weight));
}

void Engine::add_edge(uint64_t src_id, std::string label,
                      double weight, uint64_t dst_id,
                      std::string payload) {
  edge_coordinator_->atomic_put_edge(src_id, std::move(label), weight, dst_id, std::move(payload)).get();
}

void Engine::add_edge(std::string_view src_uuid, std::string label,
                      double weight, std::string_view dst_uuid,
                      std::string payload) {
    add_edge(resolver_.parse_uuid(src_uuid), std::move(label), weight, resolver_.parse_uuid(dst_uuid), std::move(payload));
}

void Engine::del_edge(uint64_t src_id, std::string label,
                      double weight, uint64_t dst_id) {
  edge_coordinator_->atomic_del_edge(src_id, std::move(label), weight, dst_id).get();
}

} // namespace l3kvg
