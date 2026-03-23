#include "L3KVG/Engine.hpp"
#include "L3KVG/Node.hpp"
#include "L3KVG/Query.hpp"
#include "engine/store.hpp"
#include <iomanip>
#include <sstream>
#include <iostream>

namespace l3kvg {

Engine::Engine(const std::string &db_path, uint32_t node_id, std::shared_ptr<lite3::ConsistentHash> ring)
    : resolver_(std::move(ring), node_id) {
  store_ = std::make_unique<l3kv::Engine>(db_path, node_id);
  edge_coordinator_ = std::make_unique<EdgeCoordinator>(store_.get(), resolver_, remote_client_, node_id);
}

Engine::~Engine() = default;

Query Engine::query() { return Query(this); }

std::shared_ptr<Node> Engine::get_node(std::string_view uuid) {
  std::string suuid(uuid);
  {
    std::lock_guard<std::mutex> lock(cache_mutex_);
    if (auto it = node_cache_.find(suuid); it != node_cache_.end()) {
      return it->second;
    }
  }
  auto node = std::make_shared<Node>(this, suuid);
  {
    std::lock_guard<std::mutex> lock(cache_mutex_);
    if (node_cache_.size() > 10000) {
      node_cache_.clear(); // Hard eviction bound
    }
    node_cache_[suuid] = node;
  }
  metrics_.cache_misses.fetch_add(1, std::memory_order_relaxed);
  return node;
}

void Engine::swizzle_node(std::string_view uuid, std::shared_ptr<Node> ptr) {
  std::string suuid(uuid);
  std::lock_guard<std::mutex> lock(cache_mutex_);
  node_cache_[suuid] = ptr;
  metrics_.cache_hits.fetch_add(1, std::memory_order_relaxed);
}

std::shared_ptr<Node> Engine::get_swizzled(std::string_view uuid) {
  std::string suuid(uuid);
  std::lock_guard<std::mutex> lock(cache_mutex_);
  if (auto it = node_cache_.find(suuid); it != node_cache_.end()) {
    metrics_.cache_hits.fetch_add(1, std::memory_order_relaxed);
    return it->second;
  }
  metrics_.cache_misses.fetch_add(1, std::memory_order_relaxed);
  return nullptr;
}

void Engine::put_node(std::string_view uuid, const std::string &json_payload) {
  std::string suuid(uuid);
  lite3::NodeID owner = resolver_.get_node_owner(suuid);
  
  if (owner != resolver_.get_local_node_id()) {
    try {
        remote_client_.put_node_async(owner, suuid, json_payload).get();
        return;
    } catch (const std::exception& e) {
        std::cerr << "[Engine::put_node] Remote RPC Failed: " << e.what() << "\n";
        // Fallback to local write if RPC fails? In a real system we might retry or error.
        // For this MVP, we will attempt local write as a safety net.
    }
  }

  std::string key = "n:{" + suuid + "}";
  store_->put(key, json_payload);
}

std::string Engine::format_weight(double weight) {
  std::ostringstream ss;
  ss << std::setw(8) << std::setfill('0') << std::fixed << std::setprecision(4)
     << weight;
  return ss.str();
}

void Engine::add_edge(std::string_view src_uuid, std::string_view label,
                      double weight, std::string_view dst_uuid) {
  edge_coordinator_->atomic_put_edge(std::string(src_uuid), std::string(label), weight, std::string(dst_uuid));
}

} // namespace l3kvg
