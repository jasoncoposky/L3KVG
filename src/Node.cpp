#include "L3KVG/Node.hpp"
#include "L3KVG/Engine.hpp"
#include "L3KVG/RemoteL3KVClient.hpp"
#include "engine/store.hpp"

namespace l3kvg {

Node::Node(Engine *engine, std::string uuid)
    : engine_(engine), uuid_(std::move(uuid)) {}

void Node::ensure_loaded() {
  if (loaded_)
    return;

  auto& resolver = engine_->get_resolver();
  std::string key = "n:{" + uuid_ + "}";

  if (!resolver.is_local(uuid_)) {
      lite3::NodeID owner = resolver.get_node_owner(uuid_);
      auto& client = engine_->get_remote_client();
      try {
          std::string raw_data = client.get_node_payload_async(owner, uuid_).get();
          if (!raw_data.empty()) {
              std::cerr << "[Node::ensure_loaded] DEBUG: Remote data received for " << uuid_ << ": " << raw_data << "\n";
              payload_ = lite3cpp::lite3_json::from_json_string(raw_data);
              std::cerr << "[Node::ensure_loaded] DEBUG: Payload size after hydration: " << (payload_ ? payload_->size() : 0) << "\n";
              
              if (payload_->get_type(0, "bloom") == lite3cpp::Type::Int64) {
                  bloom_filter_ = payload_->get_i64(0, "bloom");
              } else {
                  bloom_filter_ = 0xFFFFFFFFFFFFFFFF;
              }
          }
      } catch (const std::exception& e) {
          std::cerr << "[Node::ensure_loaded] Remote Fetch Failed: " << e.what() << "\n";
      }
  } else {
      auto buf = engine_->get_store()->get(key);
      if (buf.size() > 0) {
          payload_ = std::move(buf);
          if (payload_->get_type(0, "bloom") == lite3cpp::Type::Int64) {
              bloom_filter_ = payload_->get_i64(0, "bloom");
          } else {
              bloom_filter_ = 0xFFFFFFFFFFFFFFFF;
          }
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

  auto& resolver = engine_->get_resolver();
  if (!resolver.is_local(uuid_)) {
    lite3::NodeID owner = resolver.get_node_owner(uuid_);
    auto& client = engine_->get_remote_client();
    try {
      return client.get_neighbors_async(owner, uuid_, std::string(label), min_weight).get();
    } catch (const std::exception& e) {
      std::cerr << "[Node::get_neighbors] Remote RPC Failed: " << e.what() << "\n";
      return {};
    }
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

std::vector<std::shared_ptr<Edge>> Node::get_edges(std::string_view label,
                                                  double min_weight) {
  if (!might_have_edge(label)) {
    return {};
  }

  auto& resolver = engine_->get_resolver();
  if (!resolver.is_local(uuid_)) {
    // Phase 5 Pending: Remote get_edges RPC
    // For now, we fall back to get_neighbors (UUIDs only) or empty
    return {};
  }

  std::vector<std::shared_ptr<Edge>> edges;
  std::string prefix = "e:out:{" + uuid_ + "}:" + std::string(label) + ":";

  std::string min_w_str = Engine::format_weight(min_weight);
  std::string start_key = prefix + min_w_str;

  auto *store = engine_->get_store();
  size_t target_shard = store->get_routing_shard(prefix);

  // We need a method in l3kv::Engine that returns pairs of {key, value}
  // Let's assume get_prefix_entries exists or iterate through keys and get values.
  auto chunk = store->get_prefix_keys(prefix, target_shard, start_key, 1000);
  for (const auto &key : chunk) {
    if (key.ends_with(":meta"))
      continue;
    
    lite3cpp::Buffer buf = store->get(key); // Fetch property payload
    
    // Parse key: e:out:{src}:label:weight:dst
    // We already know src (uuid_), label, weight (from key), and dst (from key)
    
    size_t last_colon = key.find_last_of(':');
    if (last_colon != std::string::npos) {
        std::string dst_uuid = key.substr(last_colon + 1);
        
        // Extract weight from key
        size_t weight_colon = key.find_last_of(':', last_colon - 1);
        double weight = 0.0;
        if (weight_colon != std::string::npos) {
            std::string w_str = key.substr(weight_colon + 1, last_colon - weight_colon - 1);
            weight = std::stod(w_str);
        }
        
        edges.push_back(std::make_shared<Edge>(engine_, uuid_, std::string(label), weight, dst_uuid, 
                                               buf.size() > 0 ? std::make_optional(std::move(buf)) : std::nullopt));
    }
  }
  return edges;
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
