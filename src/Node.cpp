#include "L3KVG/Node.hpp"
#include "L3KVG/Engine.hpp"
#include "L3KVG/RemoteL3KVClient.hpp"
#include "L3KVG/KeyBuilder.hpp"
#include "L3KVG/SIMDFilter.hpp"
#include "engine/store.hpp"
#include <charconv>
#include <cstdio>
#include <iostream>

#define L3_LOG(level, ...) do { \
    static const bool s_l3_debug = (std::getenv("L3_DEBUG") != nullptr); \
    if (s_l3_debug) { \
        std::fprintf(stderr, "[L3KVG] " __VA_ARGS__); \
        std::fprintf(stderr, "\n"); \
        std::fflush(stderr); \
    } \
} while(0)

namespace l3kvg {

namespace {

Node::AttrValue extract_attr_at(const lite3cpp::Buffer& buf, size_t ofs, lite3cpp::HashedKey hk) noexcept {
    auto t = buf.get_type_noexcept(ofs, hk);
    switch (t) {
        case lite3cpp::Type::String:
            if (auto sv = buf.try_get_str(ofs, hk)) return *sv;
            break;
        case lite3cpp::Type::Int64:
            if (auto iv = buf.try_get_i64(ofs, hk)) return *iv;
            break;
        case lite3cpp::Type::Float64:
            if (auto fv = buf.try_get_f64(ofs, hk)) return *fv;
            break;
        case lite3cpp::Type::Bool:
            if (auto bv = buf.try_get_bool(ofs, hk)) return *bv;
            break;
        default:
            break;
    }
    return std::monostate{};
}

bool parse_hex_tail_id(std::string_view key, uint64_t& out_id) noexcept {
    size_t start_brace = key.find_last_of('{');
    size_t end_brace = key.find_last_of('}');
    if (start_brace == std::string_view::npos || end_brace == std::string_view::npos || end_brace <= start_brace + 1) {
        return false;
    }
    std::string_view hex_sv = key.substr(start_brace + 1, end_brace - start_brace - 1);
    auto [ptr, ec] = std::from_chars(hex_sv.data(), hex_sv.data() + hex_sv.size(), out_id, 16);
    return ec == std::errc{} && ptr == hex_sv.data() + hex_sv.size();
}

} // namespace

Node::Node(Engine *engine, uint64_t id)
    : engine_(engine), id_(id) {}

Node::~Node() = default;

bool Node::try_load_local(uint32_t principal_id) {
  using namespace lite3cpp::literals;
  std::string_view key = KeyBuilder::node_key(id_);
  if (principal_id != INTERNAL_UID && principal_id != ADMIN_UID) {
    auto perm = engine_->get_store()->credentials().check_permission(principal_id, key);
    if (!(perm & l3kv::Permission::READ) && !(perm & l3kv::Permission::ADMIN)) {
      return false;
    }
  }

  if (loaded_.load(std::memory_order_acquire))
    return true;

  std::lock_guard<std::mutex> lock(loading_mutex_);
  if (loaded_.load(std::memory_order_relaxed))
    return true;

  auto blob = engine_->get_store()->get_blob(key, INTERNAL_UID);
  if (blob && blob->view().size() > 0) {
    blob_pin_ = std::move(blob);
    auto v = blob_pin_->view();
    payload_ = lite3cpp::Buffer::from_view(v.data(), v.size());
    if (payload_->size() >= sizeof(lite3cpp::PackedNodeLayout)) {
      if (auto b = payload_->try_get_i64(0, "bloom"_hk)) {
        bloom_filter_.store(static_cast<uint64_t>(*b), std::memory_order_relaxed);
      } else {
        bloom_filter_.store(0xFFFFFFFFFFFFFFFFULL, std::memory_order_relaxed);
      }
    } else {
      bloom_filter_.store(0xFFFFFFFFFFFFFFFFULL, std::memory_order_relaxed);
    }
    loaded_.store(true, std::memory_order_release);
    return true;
  }

  if (engine_->get_resolver().is_local(id_)) {
    loaded_.store(true, std::memory_order_release);
    return true;
  }
  return false;
}

void Node::ensure_loaded() {
  using namespace lite3cpp::literals;
  if (loaded_.load(std::memory_order_acquire))
    return;

  std::lock_guard<std::mutex> lock(loading_mutex_);
  if (loaded_.load(std::memory_order_relaxed))
    return;

  auto& resolver = engine_->get_resolver();
  std::string_view key = KeyBuilder::node_key(id_);

  // Zero-copy local pin via L3KV Blob
  auto blob = engine_->get_store()->get_blob(key);
  if (blob && blob->view().size() > 0) {
      blob_pin_ = std::move(blob);
      auto v = blob_pin_->view();
      payload_ = lite3cpp::Buffer::from_view(v.data(), v.size());
      if (payload_->size() >= sizeof(lite3cpp::PackedNodeLayout)) {
          if (auto b = payload_->try_get_i64(0, "bloom"_hk)) {
              bloom_filter_.store(static_cast<uint64_t>(*b), std::memory_order_relaxed);
          } else {
              bloom_filter_.store(0xFFFFFFFFFFFFFFFFULL, std::memory_order_relaxed);
          }
      } else {
          bloom_filter_.store(0xFFFFFFFFFFFFFFFFULL, std::memory_order_relaxed);
      }
      loaded_.store(true, std::memory_order_release);
      return;
  }

  if (!resolver.is_local(id_)) {
      lite3::NodeID owner = resolver.get_node_owner(id_);
      auto& client = engine_->get_remote_client();
      try {
          std::string raw_data = client.get_node_payload(owner, id_);
          if (!raw_data.empty()) {
              blob_pin_.reset();
              try {
                  if (l3kv::is_json_payload(raw_data)) {
                      payload_ = lite3cpp::lite3_json::from_json_string(raw_data);
                  } else {
                      payload_ = lite3cpp::Buffer(raw_data);
                  }
              } catch (...) {
                  payload_ = lite3cpp::Buffer(raw_data);
              }

              if (payload_ && payload_->size() >= sizeof(lite3cpp::PackedNodeLayout)) {
                  if (auto b = payload_->try_get_i64(0, "bloom"_hk)) {
                      bloom_filter_.store(static_cast<uint64_t>(*b), std::memory_order_relaxed);
                  } else {
                      bloom_filter_.store(0xFFFFFFFFFFFFFFFFULL, std::memory_order_relaxed);
                  }
              } else {
                  bloom_filter_.store(0xFFFFFFFFFFFFFFFFULL, std::memory_order_relaxed);
              }
          }
      } catch (const std::exception& e) {
          std::cerr << "[Node::ensure_loaded] Remote Fetch Failed: " << e.what() << "\n";
          return;
      } catch (...) {
          return;
      }
  }
  loaded_.store(true, std::memory_order_release);
}

Node::AttrValue Node::try_get_attr(lite3cpp::HashedKey hk) {
  using namespace lite3cpp::literals;
  ensure_loaded();
  if (!payload_ || payload_->size() < sizeof(lite3cpp::PackedNodeLayout)) {
    return std::monostate{};
  }

  AttrValue v = extract_attr_at(*payload_, 0, hk);
  if (!std::holds_alternative<std::monostate>(v)) {
    return v;
  }

  if (auto bin = payload_->try_get_bytes(0, "_binary"_hk);
      bin && bin->size() >= sizeof(lite3cpp::PackedNodeLayout)) {
    lite3cpp::Buffer nested = lite3cpp::Buffer::from_view(*bin);
    v = extract_attr_at(nested, 0, hk);
    if (!std::holds_alternative<std::monostate>(v)) {
      return v;
    }
    if (auto p_ofs = nested.try_get_obj(0, "properties"_hk)) {
      v = extract_attr_at(nested, *p_ofs, hk);
      if (!std::holds_alternative<std::monostate>(v)) {
        return v;
      }
    }
  }

  if (auto p_ofs = payload_->try_get_obj(0, "properties"_hk)) {
    v = extract_attr_at(*payload_, *p_ofs, hk);
    if (!std::holds_alternative<std::monostate>(v)) {
      return v;
    }
  }

  return std::monostate{};
}

std::string_view Node::get_attribute_view(std::string_view key) {
  auto v = try_get_attr(lite3cpp::HashedKey(key));
  if (const auto* sv = std::get_if<std::string_view>(&v)) {
    return *sv;
  }
  return {};
}

std::string_view Node::get_raw_view(std::string_view key) {
  ensure_loaded();
  if (!payload_ || payload_->size() < sizeof(lite3cpp::PackedNodeLayout))
    return {};
  if (auto sv = payload_->try_get_str(0, lite3cpp::HashedKey(key))) {
    return *sv;
  }
  return {};
}

static uint64_t bloom_hash(std::string_view label) {
  size_t hash = std::hash<std::string_view>{}(label);
  return 1ULL << (hash % 64);
}

bool Node::might_have_edge(std::string_view label) const {
  uint64_t bf = bloom_filter_.load(std::memory_order_relaxed);
  if (bf == 0)
    return true;
  return (bf & bloom_hash(label)) != 0;
}

void Node::register_edge_bloom(std::string_view label) {
  bloom_filter_.fetch_or(bloom_hash(label), std::memory_order_relaxed);
}

std::vector<uint64_t> Node::get_neighbors(std::string_view label,
                                             double min_weight,
                                             uint32_t principal_id) {
  uint16_t lid = 0;
  if (!label.empty()) {
      lid = engine_->get_label_registry().lookup(label);
  }

  const bool allow_kv_fallback = engine_->has_legacy_kv_edges() || !engine_->get_settings().cluster_endpoints.empty();
  auto& resolver = engine_->get_resolver();

  std::vector<uint64_t> neighbors;
  if (lid != 0 || label.empty()) {
      engine_->get_adjacency_store().filter_outgoing_edges(id_, lid, static_cast<float>(min_weight), neighbors);
      if (!neighbors.empty() || (!allow_kv_fallback && resolver.is_local(id_))) {
          return neighbors;
      }
  }

  if (allow_kv_fallback && might_have_edge(label)) {
      std::string_view prefix = KeyBuilder::edge_prefix(id_, label);
      std::string_view min_w_str = KeyBuilder::format_weight(min_weight);

      std::string start_key;
      start_key.reserve(prefix.size() + min_w_str.size());
      start_key.append(prefix);
      start_key.append(min_w_str);

      auto *store = engine_->get_store();
      std::string prefix_str(prefix);
      size_t target_shard = store->get_routing_shard(prefix_str);

      std::string current_start = std::move(start_key);
      size_t limit = engine_->get_settings().prefix_scan_limit;
      while (true) {
          auto chunk = store->get_prefix_chunk(prefix_str, target_shard, current_start, limit);
          if (chunk.empty()) {
              break;
          }
          for (const auto &[key, val] : chunk) {
              if (key.ends_with(":meta"))
                  continue;
              uint64_t nid = 0;
              if (parse_hex_tail_id(key, nid)) {
                  neighbors.push_back(nid);
              }
          }
          if (chunk.size() < limit) {
              break;
          }
          current_start = chunk.back().first + '\0';
      }
      if (!neighbors.empty()) {
          return neighbors;
      }
  }

  if (!resolver.is_local(id_)) {
      lite3::NodeID owner = resolver.get_node_owner(id_);
      auto& client = engine_->get_remote_client();
      try {
          return client.get_neighbors(owner, id_, label, min_weight, principal_id);
      } catch (const std::exception& e) {
          std::cerr << "[Node::get_neighbors] Remote RPC Failed: " << e.what() << "\n";
          return {};
      }
  }
  return neighbors;
}

std::vector<uint64_t> Node::get_in_neighbors(std::string_view label, uint32_t principal_id) {
  uint16_t lid = 0;
  if (!label.empty()) {
      lid = engine_->get_label_registry().lookup(label);
  }

  const bool allow_kv_fallback = engine_->has_legacy_kv_edges() || !engine_->get_settings().cluster_endpoints.empty();
  auto& resolver = engine_->get_resolver();

  std::vector<uint64_t> neighbors;
  if (lid != 0 || label.empty()) {
      engine_->get_adjacency_store().for_each_incoming_edge(id_, [&](const PackedEdge& pe) {
          if (pe.is_active() && (lid == 0 || pe.label_id == lid)) {
              neighbors.push_back(pe.target_id);
          }
      });
      if (!neighbors.empty() || (!allow_kv_fallback && resolver.is_local(id_))) {
          return neighbors;
      }
  }

  if (allow_kv_fallback) {
      std::string prefix_str(KeyBuilder::edge_in_prefix(id_, label));
      auto *store = engine_->get_store();
      size_t target_shard = store->get_routing_shard(prefix_str);

      std::string current_start = prefix_str;
      size_t limit = engine_->get_settings().prefix_scan_limit;
      while (true) {
          auto chunk = store->get_prefix_chunk(prefix_str, target_shard, current_start, limit);
          if (chunk.empty()) {
              break;
          }
          for (const auto &[key, val] : chunk) {
              if (key.ends_with(":meta"))
                  continue;
              uint64_t nid = 0;
              if (parse_hex_tail_id(key, nid)) {
                  neighbors.push_back(nid);
              }
          }
          if (chunk.size() < limit) {
              break;
          }
          current_start = chunk.back().first + '\0';
      }
      if (!neighbors.empty()) {
          return neighbors;
      }
  }

  if (!resolver.is_local(id_)) {
      lite3::NodeID owner = resolver.get_node_owner(id_);
      auto& client = engine_->get_remote_client();
      try {
          return client.get_in_neighbors(owner, id_, label, principal_id);
      } catch (const std::exception& e) {
          std::cerr << "[Node::get_in_neighbors] Remote RPC Failed: " << e.what() << "\n";
          return {};
      }
  }
  return neighbors;
}

std::vector<std::shared_ptr<Edge>> Node::get_edges(std::string_view label,
                                                 double min_weight,
                                                 uint32_t principal_id) {
  uint16_t lid = 0;
  if (!label.empty()) {
      lid = engine_->get_label_registry().lookup(label);
  }
  if (lid != 0 || label.empty()) {
      std::vector<PackedEdge> snap = engine_->get_adjacency_store().get_outgoing_edges_snapshot(id_);
      if (!snap.empty()) {
          std::vector<std::shared_ptr<Edge>> edges;
          for (const auto& pe : snap) {
              if (pe.is_active() && (lid == 0 || pe.label_id == lid) && pe.weight >= min_weight) {
                  std::string edge_label(engine_->get_label_registry().get_label(pe.label_id));
                  std::optional<lite3cpp::Buffer> payload_buf = std::nullopt;
                  if (pe.has_properties()) {
                      std::string_view key = KeyBuilder::edge_out_key(id_, edge_label, pe.weight, pe.target_id);
                      lite3cpp::Buffer buf = engine_->get_store()->get(key, principal_id);
                      if (buf.size() > 0) {
                          payload_buf = std::move(buf);
                      }
                  }
                  edges.push_back(std::make_shared<Edge>(engine_, id_, std::move(edge_label), pe.weight, pe.target_id, std::move(payload_buf)));
              }
          }
          if (!edges.empty()) {
              return edges;
          }
      }
  }

  const bool allow_kv_fallback = engine_->has_legacy_kv_edges() || !engine_->get_settings().cluster_endpoints.empty();
  if (!allow_kv_fallback || !might_have_edge(label)) {
    return {};
  }

  auto& resolver = engine_->get_resolver();
  if (!resolver.is_local(id_)) {
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
  std::string prefix_str(prefix);
  size_t target_shard = store->get_routing_shard(prefix_str);

  std::string current_start = std::move(start_key);
  size_t limit = engine_->get_settings().prefix_scan_limit;
  while (true) {
    auto chunk = store->get_prefix_keys(prefix_str, target_shard, current_start, limit);
    if (chunk.empty()) {
      break;
    }
    for (const auto &key : chunk) {
      if (key.ends_with(":meta"))
        continue;
      
      lite3cpp::Buffer buf = store->get(key, principal_id);
      uint64_t dst_id = 0;
      if (parse_hex_tail_id(key, dst_id)) {
          size_t start_brace_dst = key.find_last_of('{');
          size_t weight_end = start_brace_dst - 1;
          size_t weight_start = key.find_last_of(':', weight_end - 1);

          double weight = 0.0;
          if (weight_start != std::string::npos) {
              std::string_view w_sv(key.data() + weight_start + 1, weight_end - weight_start - 1);
              std::from_chars(w_sv.data(), w_sv.data() + w_sv.size(), weight);
          }

          edges.push_back(std::make_shared<Edge>(engine_, id_, std::string(label), weight, dst_id, 
                                                 buf.size() > 0 ? std::make_optional(std::move(buf)) : std::nullopt));
      }
    }
    if (chunk.size() < limit) {
      break;
    }
    current_start = chunk.back() + '\0';
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
  
  try {
      blob_pin_.reset();
      if (data.empty()) {
        payload_ = lite3cpp::Buffer(engine_->get_settings().node_initial_buffer_size);
        payload_->init_object();
      } else {
        try {
            if (l3kv::is_json_payload(data)) {
                payload_ = lite3cpp::lite3_json::from_json_string(data);
            } else {
                payload_ = lite3cpp::Buffer(data);
            }
        } catch (...) {
            payload_ = lite3cpp::Buffer(data);
        }
      }
      loaded_.store(true, std::memory_order_release);
  } catch (...) {
  }
}

bool Node::has_attribute(std::string_view key) {
  using namespace lite3cpp::literals;
  ensure_loaded();
  if (!payload_ || payload_->size() < sizeof(lite3cpp::PackedNodeLayout)) {
    return false;
  }
  const lite3cpp::HashedKey hk(key);
  auto t = payload_->get_type_noexcept(0, hk);
  if (t != lite3cpp::Type::Null && t != lite3cpp::Type::Invalid) return true;

  if (auto bin = payload_->try_get_bytes(0, "_binary"_hk);
      bin && bin->size() >= sizeof(lite3cpp::PackedNodeLayout)) {
      lite3cpp::Buffer nested = lite3cpp::Buffer::from_view(*bin);
      auto t2 = nested.get_type_noexcept(0, hk);
      if (t2 != lite3cpp::Type::Null && t2 != lite3cpp::Type::Invalid) return true;
      if (auto p_ofs = nested.try_get_obj(0, "properties"_hk)) {
          auto tp = nested.get_type_noexcept(*p_ofs, hk);
          if (tp != lite3cpp::Type::Null && tp != lite3cpp::Type::Invalid) return true;
      }
  }

  if (auto p_ofs = payload_->try_get_obj(0, "properties"_hk)) {
      auto tp = payload_->get_type_noexcept(*p_ofs, hk);
      if (tp != lite3cpp::Type::Null && tp != lite3cpp::Type::Invalid) return true;
  }
  return false;
}

lite3cpp::Type Node::get_attribute_type(std::string_view key) {
  using namespace lite3cpp::literals;
  ensure_loaded();
  if (!payload_ || payload_->size() < sizeof(lite3cpp::PackedNodeLayout))
    return lite3cpp::Type::Invalid;
  
  const lite3cpp::HashedKey hk(key);
  auto type = payload_->get_type_noexcept(0, hk);
  if (type != lite3cpp::Type::Null && type != lite3cpp::Type::Invalid) {
      return type;
  }
  if (auto bin = payload_->try_get_bytes(0, "_binary"_hk);
      bin && bin->size() >= sizeof(lite3cpp::PackedNodeLayout)) {
      lite3cpp::Buffer nested = lite3cpp::Buffer::from_view(*bin);
      auto t2 = nested.get_type_noexcept(0, hk);
      if (t2 != lite3cpp::Type::Null && t2 != lite3cpp::Type::Invalid) {
          return t2;
      }
      if (auto p_ofs = nested.try_get_obj(0, "properties"_hk)) {
          auto tp = nested.get_type_noexcept(*p_ofs, hk);
          if (tp != lite3cpp::Type::Null && tp != lite3cpp::Type::Invalid) {
              return tp;
          }
      }
  }
  if (auto p_ofs = payload_->try_get_obj(0, "properties"_hk)) {
      auto tp = payload_->get_type_noexcept(*p_ofs, hk);
      if (tp != lite3cpp::Type::Null && tp != lite3cpp::Type::Invalid) {
          return tp;
      }
  }
  return type;
}

std::string Node::get_attribute_as_string(std::string_view key) {
  using namespace lite3cpp::literals;
  const lite3cpp::HashedKey hk(key);
  AttrValue v = try_get_attr(hk);
  if (const auto* sv = std::get_if<std::string_view>(&v)) {
      return std::string(*sv);
  }
  if (const auto* iv = std::get_if<int64_t>(&v)) {
      char buf[32];
      auto [ptr, ec] = std::to_chars(buf, buf + sizeof(buf), *iv);
      return (ec == std::errc{}) ? std::string(buf, ptr) : std::to_string(*iv);
  }
  if (const auto* fv = std::get_if<double>(&v)) {
      return std::to_string(*fv);
  }
  if (const auto* bv = std::get_if<bool>(&v)) {
      return *bv ? "true" : "false";
  }

  // Fallback for Type::Bytes hex formatting
  if (!payload_ || payload_->size() < sizeof(lite3cpp::PackedNodeLayout)) {
      return "";
  }
  auto format_bytes_hex = [](std::span<const std::byte> span) {
      static const char hex_chars[] = "0123456789abcdef";
      std::string res;
      res.reserve(span.size() * 2);
      for (auto byte : span) {
          uint8_t byte_val = static_cast<uint8_t>(byte);
          res.push_back(hex_chars[(byte_val >> 4) & 0x0F]);
          res.push_back(hex_chars[byte_val & 0x0F]);
      }
      return res;
  };
  if (auto span = payload_->try_get_bytes(0, hk)) {
      return format_bytes_hex(*span);
  }
  if (auto bin = payload_->try_get_bytes(0, "_binary"_hk);
      bin && bin->size() >= sizeof(lite3cpp::PackedNodeLayout)) {
      lite3cpp::Buffer nested = lite3cpp::Buffer::from_view(*bin);
      if (auto span = nested.try_get_bytes(0, hk)) {
          return format_bytes_hex(*span);
      }
      if (auto p_ofs = nested.try_get_obj(0, "properties"_hk)) {
          if (auto span = nested.try_get_bytes(*p_ofs, hk)) {
              return format_bytes_hex(*span);
          }
      }
  }
  if (auto p_ofs = payload_->try_get_obj(0, "properties"_hk)) {
      if (auto span = payload_->try_get_bytes(*p_ofs, hk)) {
          return format_bytes_hex(*span);
      }
  }
  return "";
}

} // namespace l3kvg
