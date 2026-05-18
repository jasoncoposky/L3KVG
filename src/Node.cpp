#include "L3KVG/Node.hpp"
#include "L3KVG/Engine.hpp"
#include "L3KVG/RemoteL3KVClient.hpp"
#include "L3KVG/KeyBuilder.hpp"
#include "engine/store.hpp"
#include <iostream>

namespace l3kvg {

Node::Node(Engine *engine, uint64_t id)
    : engine_(engine), id_(id) {}

void Node::ensure_loaded() {
  if (loaded_.load(std::memory_order_acquire))
    return;

  std::lock_guard<std::mutex> lock(loading_mutex_);
  if (loaded_.load(std::memory_order_relaxed))
    return;

  auto& resolver = engine_->get_resolver();
  std::string key = std::string(KeyBuilder::node_key(id_));

  // Locality of Reference: Check local store first even if we are not the primary owner.
  // L3KV replication may have placed a local copy here.
  auto buf = engine_->get_store()->get(key);
  if (buf.size() > 0) {
      payload_ = std::move(buf);
      if (payload_->get_type(0, "bloom") == lite3cpp::Type::Int64) {
          bloom_filter_ = payload_->get_i64(0, "bloom");
      } else {
          bloom_filter_ = 0xFFFFFFFFFFFFFFFF;
      }
      loaded_.store(true, std::memory_order_release);
      return;
  }

  if (!resolver.is_local(id_)) {
      lite3::NodeID owner = resolver.get_node_owner(id_);
      auto& client = engine_->get_remote_client();
      try {
          // TODO: Update client to take uint64_t
          std::string raw_data = client.get_node_payload_async(owner, id_).get();
          if (!raw_data.empty()) {
              payload_ = lite3cpp::lite3_json::from_json_string(raw_data);
              
              if (payload_->get_type(0, "bloom") == lite3cpp::Type::Int64) {
                  bloom_filter_ = payload_->get_i64(0, "bloom");
              } else {
                  bloom_filter_ = 0xFFFFFFFFFFFFFFFF;
              }
          }
      } catch (const std::exception& e) {
          std::cerr << "[Node::ensure_loaded] Remote Fetch Failed: " << e.what() << "\n";
      }
  }
  loaded_.store(true, std::memory_order_release);
}

std::string_view Node::get_attribute_view(std::string_view key) {
  ensure_loaded();
  if (!payload_ || payload_->size() == 0)
    return {};
  if (payload_->get_type(0, key) == lite3cpp::Type::String) {
    return payload_->get_str(0, key);
  }
  return {};
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

std::vector<uint64_t> Node::get_neighbors(std::string_view label,
                                             double min_weight) {
  if (!might_have_edge(label)) {
    return {};
  }

  std::vector<uint64_t> neighbors;
  std::string_view prefix = KeyBuilder::edge_prefix(id_, label);
  std::string_view min_w_str = KeyBuilder::format_weight(min_weight);
  
  std::string start_key;
  start_key.reserve(prefix.size() + min_w_str.size());
  start_key.append(prefix);
  start_key.append(min_w_str);

  auto *store = engine_->get_store();
  size_t target_shard = store->get_routing_shard(std::string(prefix));

  // Locality of Reference: Check local store first.
  auto chunk = store->get_prefix_keys(std::string(prefix), target_shard, start_key, engine_->get_settings().prefix_scan_limit);
  if (!chunk.empty()) {
      for (const auto &key : chunk) {
          if (key.ends_with(":meta"))
              continue;
          size_t start_brace = key.find_last_of('{');
          size_t end_brace = key.find_last_of('}');
          if (start_brace != std::string::npos && end_brace != std::string::npos && end_brace > start_brace) {
              std::string id_str = key.substr(start_brace + 1, end_brace - start_brace - 1);
              uint64_t nid = std::stoull(id_str, nullptr, 16);
              // std::cout << "  [Node " << id_ << "] Found neighbor " << nid << " via key " << key << std::endl;
              neighbors.push_back(nid);
          }
      }
      return neighbors;
  }

  auto& resolver = engine_->get_resolver();
  if (!resolver.is_local(id_)) {
    lite3::NodeID owner = resolver.get_node_owner(id_);
    auto& client = engine_->get_remote_client();
    try {
      return client.get_neighbors_async(owner, id_, std::string(label), min_weight).get();
    } catch (const std::exception& e) {
      std::cerr << "[Node::get_neighbors] Remote RPC Failed: " << e.what() << "\n";
      return {};
    }
  }
  return neighbors;
}

std::vector<uint64_t> Node::get_in_neighbors(std::string_view label) {
  std::vector<uint64_t> neighbors;
  std::string_view prefix = KeyBuilder::edge_in_prefix(id_, label);
  
  auto *store = engine_->get_store();
  size_t target_shard = store->get_routing_shard(std::string(prefix));

  auto chunk = store->get_prefix_keys(std::string(prefix), target_shard, std::string(prefix), engine_->get_settings().prefix_scan_limit);
  for (const auto &key : chunk) {
      if (key.ends_with(":meta"))
          continue;
      size_t start_brace = key.find_last_of('{');
      size_t end_brace = key.find_last_of('}');
      if (start_brace != std::string::npos && end_brace != std::string::npos && end_brace > start_brace) {
          std::string id_str = key.substr(start_brace + 1, end_brace - start_brace - 1);
          neighbors.push_back(std::stoull(id_str, nullptr, 16));
      }
  }
  
  return neighbors;
}

std::vector<std::shared_ptr<Edge>> Node::get_edges(std::string_view label,
                                                  double min_weight) {
  if (!might_have_edge(label)) {
    return {};
  }

  auto& resolver = engine_->get_resolver();
  if (!resolver.is_local(id_)) {
    // Phase 5 Pending: Remote get_edges RPC
    // For now, we fall back to get_neighbors (UUIDs only) or empty
    return {};
  }

  std::vector<std::shared_ptr<Edge>> edges;
  std::string_view prefix = KeyBuilder::edge_prefix(id_, label);
  std::string_view min_w_str = KeyBuilder::format_weight(min_weight);
  
  std::string start_key;
  start_key.reserve(prefix.size() + min_w_str.size());
  start_key.append(prefix);
  start_key.append(min_w_str);

  auto *store = engine_->get_store();
  size_t target_shard = store->get_routing_shard(std::string(prefix));

  // We need a method in l3kv::Engine that returns pairs of {key, value}
  // Let's assume get_prefix_entries exists or iterate through keys and get values.
  auto chunk = store->get_prefix_keys(std::string(prefix), target_shard, start_key, engine_->get_settings().prefix_scan_limit);
  for (const auto &key : chunk) {
    if (key.ends_with(":meta"))
      continue;
    
    lite3cpp::Buffer buf = store->get(key); // Fetch property payload
    
    // Parse key: e:out:{src}:{label}:{weight}:{dst}
    size_t start_brace_dst = key.find_last_of('{');
    size_t end_brace_dst = key.find_last_of('}');

    if (start_brace_dst != std::string::npos && end_brace_dst != std::string::npos && end_brace_dst > start_brace_dst) {
        std::string dst_id_str = key.substr(start_brace_dst + 1, end_brace_dst - start_brace_dst - 1);
        uint64_t dst_id = std::stoull(dst_id_str, nullptr, 16);

        // Extract weight from key: it's between the second-to-last colon and the last open-brace
        size_t weight_end = start_brace_dst - 1; // The colon before {dst}
        size_t weight_start = key.find_last_of(':', weight_end - 1);

        double weight = 0.0;
        if (weight_start != std::string::npos) {
            std::string w_str = key.substr(weight_start + 1, weight_end - weight_start - 1);
            weight = std::stod(w_str);
        }

        edges.push_back(std::make_shared<Edge>(engine_, id_, std::string(label), weight, dst_id, 
                                               buf.size() > 0 ? std::make_optional(std::move(buf)) : std::nullopt));
    }

  }
  return edges;
}

std::vector<std::shared_ptr<Node>>
Node::get_hot_neighbors(std::string_view label, double min_weight) {
  std::vector<std::shared_ptr<Node>> hot_nodes;
  auto neighbors = get_neighbors(label, min_weight);

  for (const auto &neighbor_id : neighbors) {
    auto swizzled = engine_->get_swizzled(neighbor_id);
    if (swizzled) {
      hot_nodes.push_back(swizzled);
    } else {
      hot_nodes.push_back(engine_->get_node(neighbor_id));
    }
  }
  return hot_nodes;
}

void Node::hydrate(const std::string &data) {
  std::lock_guard<std::mutex> lock(loading_mutex_);
  if (loaded_.load(std::memory_order_relaxed)) return;
  
  if (data.empty()) {
    payload_ = lite3cpp::Buffer(engine_->get_settings().node_initial_buffer_size);
    payload_->init_object();
  } else {
    try {
        if (data.starts_with("{")) {
            payload_ = lite3cpp::lite3_json::from_json_string(data);
        } else {
            // It's binary BSON, reconstruct Buffer directly
            payload_ = lite3cpp::Buffer(std::vector<uint8_t>(data.begin(), data.end()));
        }
    } catch (...) {
        payload_ = lite3cpp::Buffer(std::vector<uint8_t>(data.begin(), data.end()));
    }
  }
  loaded_.store(true, std::memory_order_release);
}

bool Node::has_attribute(const std::string &key) {
  ensure_loaded();
  if (!payload_ || payload_->size() == 0)
    return false;
  return payload_->get_type(0, key) != lite3cpp::Type::Null &&
         payload_->get_type(0, key) != lite3cpp::Type::Invalid;
}

lite3cpp::Type Node::get_attribute_type(std::string_view key) {
  ensure_loaded();
  if (!payload_ || payload_->size() == 0)
    return lite3cpp::Type::Invalid;
  return payload_->get_type(0, key);
}

std::string Node::get_attribute_as_string(std::string_view key) {
  ensure_loaded();
  if (!payload_ || payload_->size() == 0)
    return "";

  auto type = payload_->get_type(0, key);
  switch (type) {
    case lite3cpp::Type::String:
      return std::string(payload_->get_str(0, key));
    case lite3cpp::Type::Int64:
      return std::to_string(payload_->get_i64(0, key));
    case lite3cpp::Type::Float64:
      return std::to_string(payload_->get_f64(0, key));
    case lite3cpp::Type::Bool:
      return payload_->get_bool(0, key) ? "true" : "false";
    default:
      return "";
  }
}

} // namespace l3kvg
