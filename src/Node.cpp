#include "L3KVG/Node.hpp"
#include "L3KVG/Engine.hpp"
#include "engine/store.hpp"

namespace l3kvg {

Node::Node(Engine *engine, std::string uuid)
    : engine_(engine), uuid_(std::move(uuid)) {}

void Node::ensure_loaded() {
  if (loaded_)
    return;

  std::string key = "n:{" + uuid_ + "}";
  auto buf = engine_->get_store()->get(key);

  if (buf.size() > 0) {
    payload_ = std::move(buf);

    if (payload_->get_type(0, "bloom") == lite3cpp::Type::Int64) {
      bloom_filter_ = payload_->get_i64(0, "bloom");
    } else {
      bloom_filter_ =
          0xFFFFFFFFFFFFFFFF; // Missing filter implies everything might exist
    }
  }
  loaded_ = true;
}

std::string_view Node::get_raw_view(std::string_view key) {
  ensure_loaded();
  if (!payload_ || payload_->size() == 0)
    return {};
  if (payload_->get_type(0, key) == lite3cpp::Type::String) {
    return payload_->get_str(0, key);
  }
  return {};
}

static uint64_t bloom_hash(std::string_view label) {
  size_t hash = std::hash<std::string_view>{}(label);
  return 1ULL << (hash % 64);
}

bool Node::might_have_edge(std::string_view label) const {
  if (bloom_filter_ == 0)
    return true;
  return (bloom_filter_ & bloom_hash(label)) != 0;
}

void Node::register_edge_bloom(std::string_view label) {
  bloom_filter_ |= bloom_hash(label);
}

std::vector<std::string> Node::get_neighbors(std::string_view label,
                                             double min_weight) {
  if (!might_have_edge(label)) {
    return {};
  }

  std::vector<std::string> neighbors;
  std::string prefix = "e:out:{" + uuid_ + "}:" + std::string(label) + ":";

  std::string min_w_str = Engine::format_weight(min_weight);
  std::string start_key = prefix + min_w_str;

  auto *store = engine_->get_store();
  size_t target_shard = store->get_routing_shard(prefix);

  auto chunk = store->get_prefix_keys(prefix, target_shard, start_key, 1000);
  for (const auto &key : chunk) {
    if (key.ends_with(":meta"))
      continue;
    size_t last_colon = key.find_last_of(':');
    if (last_colon != std::string::npos) {
      neighbors.push_back(key.substr(last_colon + 1));
    }
  }
  return neighbors;
}

std::vector<std::shared_ptr<Node>>
Node::get_hot_neighbors(std::string_view label, double min_weight) {
  std::vector<std::shared_ptr<Node>> hot_nodes;
  auto neighbors = get_neighbors(label, min_weight);

  for (const auto &neighbor_uuid : neighbors) {
    auto swizzled = engine_->get_swizzled(neighbor_uuid);
    if (swizzled) {
      hot_nodes.push_back(swizzled);
    } else {
      hot_nodes.push_back(engine_->get_node(neighbor_uuid));
    }
  }
  return hot_nodes;
}

bool Node::has_attribute(const std::string &key) {
  ensure_loaded();
  if (!payload_ || payload_->size() == 0)
    return false;
  return payload_->get_type(0, key) != lite3cpp::Type::Null &&
         payload_->get_type(0, key) != lite3cpp::Type::Invalid;
}

} // namespace l3kvg
