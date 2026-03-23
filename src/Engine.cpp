#include "L3KVG/Engine.hpp"
#include "L3KVG/Node.hpp"
#include "L3KVG/Query.hpp"
#include "engine/store.hpp"
#include <iomanip>
#include <sstream>

namespace l3kvg {

Engine::Engine(const std::string &db_path, uint32_t node_id) {
  store_ = std::make_unique<l3kv::Engine>(db_path, node_id);
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
  std::string key = "n:{" + std::string(uuid) + "}";
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
  std::string w_str = format_weight(weight);

  std::string out_key = "e:out:{" + std::string(src_uuid) +
                        "}:" + std::string(label) + ":" + w_str + ":" +
                        std::string(dst_uuid);
  std::string in_key = "e:in:{" + std::string(dst_uuid) +
                       "}:" + std::string(label) + ":" + std::string(src_uuid);

  store_->put(out_key, "{}");
  store_->put(in_key, "{}");
}

} // namespace l3kvg
