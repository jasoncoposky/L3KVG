#include "L3KVG/Engine.hpp"
#include "L3KVG/Node.hpp"
#include "L3KVG/Query.hpp"
#include "L3KVG/KeyBuilder.hpp"
#include "L3KVG/MutationBatch.hpp"
#include "engine/store.hpp"
#include <bit>
#include <charconv>
#include <iomanip>
#include <sstream>
#include <iostream>
#include <filesystem>
#include <fstream>


namespace l3kvg {

namespace {

void copy_batch_edge_array(const lite3cpp::Buffer& src, size_t src_arr_ofs, lite3cpp::Buffer& dst, size_t dst_arr_ofs);

void copy_batch_edge_props(const lite3cpp::Buffer& src, size_t src_obj_ofs, lite3cpp::Buffer& dst, size_t dst_obj_ofs) {
    for (auto it = src.begin(src_obj_ofs), end = src.end(src_obj_ofs); it != end; ++it) {
        lite3cpp::HashedKey hk(it->key);
        switch (it->value_type) {
            case lite3cpp::Type::Int64:
                dst.set_i64(dst_obj_ofs, hk, src.get_i64(src_obj_ofs, hk));
                break;
            case lite3cpp::Type::Float64:
                dst.set_f64(dst_obj_ofs, hk, src.get_f64(src_obj_ofs, hk));
                break;
            case lite3cpp::Type::String:
                dst.set_str(dst_obj_ofs, hk, src.get_str(src_obj_ofs, hk));
                break;
            case lite3cpp::Type::Bool:
                dst.set_bool(dst_obj_ofs, hk, src.get_bool(src_obj_ofs, hk));
                break;
            case lite3cpp::Type::Null:
                dst.set_null(dst_obj_ofs, hk);
                break;
            case lite3cpp::Type::Bytes:
                dst.set_bytes(dst_obj_ofs, hk, src.get_bytes(src_obj_ofs, hk));
                break;
            case lite3cpp::Type::Object: {
                size_t child_src = src.get_obj(src_obj_ofs, hk);
                size_t child_dst = dst.set_obj(dst_obj_ofs, hk);
                copy_batch_edge_props(src, child_src, dst, child_dst);
                break;
            }
            case lite3cpp::Type::Array: {
                size_t child_src = src.get_arr(src_obj_ofs, hk);
                size_t child_dst = dst.set_arr(dst_obj_ofs, hk);
                copy_batch_edge_array(src, child_src, dst, child_dst);
                break;
            }
            default:
                break;
        }
    }
}

void copy_batch_edge_array(const lite3cpp::Buffer& src, size_t src_arr_ofs, lite3cpp::Buffer& dst, size_t dst_arr_ofs) {
    lite3cpp::NodeView node(reinterpret_cast<const lite3cpp::PackedNodeLayout*>(src.data() + src_arr_ofs));
    for (uint32_t i = 0; i < node.size(); ++i) {
        switch (src.arr_get_type(src_arr_ofs, i)) {
            case lite3cpp::Type::Int64:
                dst.arr_append_i64(dst_arr_ofs, src.arr_get_i64(src_arr_ofs, i));
                break;
            case lite3cpp::Type::Float64:
                dst.arr_append_f64(dst_arr_ofs, src.arr_get_f64(src_arr_ofs, i));
                break;
            case lite3cpp::Type::String:
                dst.arr_append_str(dst_arr_ofs, src.arr_get_str(src_arr_ofs, i));
                break;
            case lite3cpp::Type::Bool:
                dst.arr_append_bool(dst_arr_ofs, src.arr_get_bool(src_arr_ofs, i));
                break;
            case lite3cpp::Type::Null:
                dst.arr_append_null(dst_arr_ofs);
                break;
            case lite3cpp::Type::Bytes:
                dst.arr_append_bytes(dst_arr_ofs, src.arr_get_bytes(src_arr_ofs, i));
                break;
            case lite3cpp::Type::Object: {
                size_t child_src = src.arr_get_obj(src_arr_ofs, i);
                size_t child_dst = dst.arr_append_obj(dst_arr_ofs);
                copy_batch_edge_props(src, child_src, dst, child_dst);
                break;
            }
            case lite3cpp::Type::Array: {
                size_t child_src = src.arr_get_arr(src_arr_ofs, i);
                size_t child_dst = dst.arr_append_arr(dst_arr_ofs);
                copy_batch_edge_array(src, child_src, dst, child_dst);
                break;
            }
            default:
                break;
        }
    }
}

void apply_inc_node_int_to_buffer(lite3cpp::Buffer& nbuf, std::string_view key, int64_t delta) {
    using namespace lite3cpp::literals;
    if (nbuf.size() < sizeof(lite3cpp::PackedNodeLayout)) {
        nbuf.init_object();
    }
    lite3cpp::HashedKey hk(key);

    auto try_inc_at = [&](lite3cpp::Buffer& b, size_t obj_ofs) -> bool {
        auto t = b.get_type_noexcept(obj_ofs, hk);
        if (t == lite3cpp::Type::Int64) {
            int64_t cur = b.get_i64(obj_ofs, hk);
            b.set_i64(obj_ofs, hk, cur + delta);
            return true;
        }
        if (t == lite3cpp::Type::String) {
            std::string_view sv = b.get_str(obj_ofs, hk);
            int64_t cur = 0;
            if (!sv.empty()) {
                auto [ptr, ec] = std::from_chars(sv.data(), sv.data() + sv.size(), cur);
                if (ec != std::errc{}) {
                    try { cur = std::stoll(std::string(sv)); } catch (...) { cur = 0; }
                }
            }
            std::string updated = std::to_string(cur + delta);
            b.set_str(obj_ofs, hk, updated);
            return true;
        }
        if (t == lite3cpp::Type::Float64) {
            int64_t cur = static_cast<int64_t>(b.get_f64(obj_ofs, hk));
            b.set_i64(obj_ofs, hk, cur + delta);
            return true;
        }
        return false;
    };

    if (try_inc_at(nbuf, 0)) {
        return;
    }
    if (nbuf.get_type_noexcept(0, "properties"_hk) == lite3cpp::Type::Object) {
        size_t p_ofs = nbuf.get_obj(0, "properties"_hk);
        if (try_inc_at(nbuf, p_ofs)) {
            return;
        }
    }
    if (nbuf.get_type_noexcept(0, "_binary"_hk) == lite3cpp::Type::Bytes) {
        auto bin = nbuf.get_bytes(0, "_binary"_hk);
        if (bin.size() >= sizeof(lite3cpp::PackedNodeLayout)) {
            lite3cpp::Buffer nested(reinterpret_cast<const uint8_t*>(bin.data()), bin.size());
            if (try_inc_at(nested, 0)) {
                nbuf.set_bytes(0, "_binary"_hk,
                               std::span<const std::byte>(reinterpret_cast<const std::byte*>(nested.data()), nested.size()));
                return;
            }
        }
    }
    nbuf.set_i64(0, hk, delta);
}

} // namespace

static std::atomic<uint64_t> s_next_engine_instance_id{1};

Engine::Engine(const std::string &db_path, uint32_t node_id, std::shared_ptr<lite3::ConsistentHash> ring, size_t thread_pool_size, Settings settings)
    : resolver_(std::move(ring), node_id), settings_(std::move(settings)), engine_instance_id_(s_next_engine_instance_id.fetch_add(1, std::memory_order_relaxed)), hlc_(node_id), db_path_(db_path) {
  settings_.node_id = node_id;
  std::string store_wal_path = db_path;
  std::string wal_path;
  if (!db_path.empty() && db_path != ":memory:") {
      std::error_code ec;
      if (std::filesystem::is_regular_file(db_path, ec)) {
          store_wal_path = db_path;
          wal_path = db_path + ".edges.wal";
          dict_path_ = db_path + ".labels.dict";
      } else if (std::filesystem::is_directory(db_path, ec)) {
          store_wal_path = db_path + "/kv.wal";
          wal_path = db_path + "/edges.wal";
          dict_path_ = db_path + "/labels.dict";
      } else {
          std::error_code dir_ec;
          if (std::filesystem::create_directories(db_path, dir_ec) || std::filesystem::is_directory(db_path, dir_ec)) {
              store_wal_path = db_path + "/kv.wal";
              wal_path = db_path + "/edges.wal";
              dict_path_ = db_path + "/labels.dict";
          } else {
              store_wal_path = db_path;
              wal_path = db_path + ".edges.wal";
              dict_path_ = db_path + ".labels.dict";
          }
      }
  }
  store_ = std::make_unique<l3kv::Engine>(store_wal_path, node_id);
  pool_ = std::make_shared<ThreadPool>(thread_pool_size);
  remote_client_ = std::make_unique<RemoteL3KVClient>(settings_);
  remote_client_->set_thread_pool(pool_);
  
  auto replication_cb = [this](const std::string& key, const std::string& payload) {
      this->broadcast_replication(key, payload, this->resolver_.get_local_cluster_id());
  };

  edge_coordinator_ = std::make_unique<EdgeCoordinator>(store_.get(), resolver_, *remote_client_, node_id, pool_, settings_, replication_cb);
  
  for (size_t i = 0; i < settings_.node_cache_shards; ++i) {
    cache_shards_.push_back(std::make_unique<CacheShard>());
  }

  if (!dict_path_.empty()) {
      if (std::filesystem::exists(dict_path_)) {
          try {
              std::ifstream ifs(dict_path_);
              std::string line;
              while (std::getline(ifs, line)) {
                  if (!line.empty() && line.back() == '\r') {
                      line.pop_back();
                  }
                  if (!line.empty()) {
                      label_registry_.get_or_register(line);
                  }
              }
          } catch (...) {
          }
      }
      label_registry_.set_dict_path(dict_path_);
  }

  if (!wal_path.empty()) {
      try {
          std::error_code ec;
          std::filesystem::create_directories(std::filesystem::path(wal_path).parent_path(), ec);
          edge_wal_ = std::make_unique<BinaryEdgeWAL>(wal_path);
          size_t recovered_edges = edge_wal_->recover(adjacency_store_);
          size_t active_edges = adjacency_store_.total_active_edges();
          uint64_t edge_wal_bytes = edge_wal_->file_size();
          if ((edge_wal_bytes > 16ULL * 1024 * 1024 && recovered_edges > active_edges * 2) ||
              (edge_wal_bytes > 128ULL * 1024 * 1024 && recovered_edges > active_edges + 50000)) {
              edge_wal_->compact(adjacency_store_);
          }
      } catch (...) {
          edge_wal_.reset();
      }
  }

  if (adjacency_store_.total_active_edges() == 0 &&
      !store_->get_prefix_keys_all_shards("e:", "", 1).empty()) {
      has_legacy_kv_edges_.store(true, std::memory_order_relaxed);
  }
}

Engine::~Engine() {
  if (edge_wal_) {
      edge_wal_->flush();
      edge_wal_->close();
  }
  if (!dict_path_.empty()) {
      label_registry_.persist();
  }

  // 1. Stop the EdgeCoordinator (which may use the pool)
  edge_coordinator_.reset();
  
  // 2. Stop the ThreadPool and wait for all background tasks to finish.
  // This must happen while the RemoteL3KVClient (and its ZMQ context) is still alive.
  pool_.reset();

  // 3. Finally, clean up the remote client and local store.
  remote_client_.reset();
  store_.reset();
}

void Engine::set_remote_client(std::unique_ptr<RemoteL3KVClient> client) {
  remote_client_ = std::move(client);
  auto replication_cb = [this](const std::string& key, const std::string& payload) {
      this->broadcast_replication(key, payload, this->resolver_.get_local_cluster_id());
  };
  edge_coordinator_ = std::make_unique<EdgeCoordinator>(store_.get(), resolver_, *remote_client_, settings_.node_id, pool_, settings_, replication_cb);
}

size_t Engine::get_cache_shard(uint64_t id) {
    return std::hash<uint64_t>{}(id) % settings_.node_cache_shards;
}

void Engine::invalidate_node_cache(uint64_t id) {
    cache_epoch_.fetch_add(1, std::memory_order_relaxed);
    if (!has_cached_nodes_.load(std::memory_order_relaxed)) {
        return;
    }
    size_t h = get_cache_shard(id);
    auto& shard = *cache_shards_[h];
    if (shard.count.load(std::memory_order_relaxed) == 0) {
        return;
    }
    std::lock_guard<std::mutex> lock(shard.mutex);
    if (auto it = shard.map.find(id); it != shard.map.end()) {
        shard.lru.erase(it->second.lru_it);
        shard.map.erase(it);
        shard.count.store(shard.map.size(), std::memory_order_relaxed);
    }
}

Query Engine::query() { return Query(this); }

const std::shared_ptr<Node>& Engine::get_node(uint64_t id) {
  thread_local const Engine* tl_last_engine = nullptr;
  thread_local uint64_t tl_engine_instance_id = 0;
  thread_local uint64_t tl_epoch = 0;
  thread_local uint64_t tl_last_id = UINT64_MAX;
  thread_local std::shared_ptr<Node> tl_last_node;

  uint64_t curr_epoch = cache_epoch_.load(std::memory_order_relaxed);
  if (tl_last_engine == this && tl_engine_instance_id == engine_instance_id_ &&
      tl_last_node && tl_last_id == id && tl_epoch == curr_epoch) {
      return tl_last_node;
  }

  size_t h = get_cache_shard(id);
  auto& shard = *cache_shards_[h];

  std::lock_guard<std::mutex> lock(shard.mutex);
  if (auto it = shard.map.find(id); it != shard.map.end()) {
    // O(1) zero-allocation LRU update
    if (shard.lru.begin() != it->second.lru_it) {
        shard.lru.splice(shard.lru.begin(), shard.lru, it->second.lru_it);
    }
    tl_last_engine = this;
    tl_engine_instance_id = engine_instance_id_;
    tl_epoch = curr_epoch;
    tl_last_id = id;
    tl_last_node = it->second.node;
    return tl_last_node;
  }

  if (shard.map.size() >= settings_.node_cache_size_per_shard && !shard.lru.empty()) {
      uint64_t victim = shard.lru.back();
      shard.map.erase(victim);
      shard.lru.pop_back();
  }
  shard.lru.push_front(id);
  auto node = std::make_shared<Node>(this, id);
  shard.map[id] = {node, shard.lru.begin()};
  shard.count.store(shard.map.size(), std::memory_order_relaxed);
  has_cached_nodes_.store(true, std::memory_order_relaxed);
  metrics_.cache_misses.fetch_add(1, std::memory_order_relaxed);
  tl_last_engine = this;
  tl_engine_instance_id = engine_instance_id_;
  tl_epoch = curr_epoch;
  tl_last_id = id;
  tl_last_node = node;
  return tl_last_node;
}

const std::shared_ptr<Node>& Engine::get_node(std::string_view uuid) {
    return get_node(resolver_.parse_uuid(uuid));
}

void Engine::swizzle_node(uint64_t id, std::shared_ptr<Node> ptr) {
  size_t h = get_cache_shard(id);
  auto& shard = *cache_shards_[h];

  std::lock_guard<std::mutex> lock(shard.mutex);
  if (auto it = shard.map.find(id); it != shard.map.end()) {
      it->second.node = ptr;
      if (shard.lru.begin() != it->second.lru_it) {
          shard.lru.splice(shard.lru.begin(), shard.lru, it->second.lru_it);
      }
      metrics_.cache_hits.fetch_add(1, std::memory_order_relaxed);
  } else {
      if (shard.map.size() >= settings_.node_cache_size_per_shard && !shard.lru.empty()) {
          uint64_t victim = shard.lru.back();
          shard.map.erase(victim);
          shard.lru.pop_back();
      }
      shard.lru.push_front(id);
      shard.map[id] = {ptr, shard.lru.begin()};
      shard.count.store(shard.map.size(), std::memory_order_relaxed);
  }
}

std::shared_ptr<Node> Engine::get_swizzled(uint64_t id) {
  size_t h = get_cache_shard(id);
  auto& shard = *cache_shards_[h];

  std::lock_guard<std::mutex> lock(shard.mutex);
  if (auto it = shard.map.find(id); it != shard.map.end()) {
    shard.lru.splice(shard.lru.begin(), shard.lru, it->second.lru_it);
    metrics_.cache_hits.fetch_add(1, std::memory_order_relaxed);
    return it->second.node;
  }
  metrics_.cache_misses.fetch_add(1, std::memory_order_relaxed);
  return nullptr;
}

void Engine::swizzle_node(uint64_t id, const std::string& payload) {
  auto node = get_node(id);
  if (node) {
    node->hydrate(payload);
  }
}

std::vector<std::shared_ptr<Node>> Engine::fetch_nodes(const std::vector<uint64_t>& ids, uint32_t principal_id) {
  if (ids.empty()) return {};

  std::vector<std::shared_ptr<Node>> result(ids.size());

  get_thread_pool().parallel_for(0, ids.size(), [&](size_t first, size_t last) {
    for (size_t i = first; i < last; ++i) {
      uint64_t id = ids[i];
      auto node = get_node(id);
      if (node) {
        if (node->try_load_local(principal_id)) {
          result[i] = std::move(node);
        } else if (!resolver_.is_local(id)) {
          result[i] = std::move(node);
        }
      }
    }
  });

  std::unordered_map<lite3::NodeID, std::vector<uint64_t>> remote_requests;
  for (size_t i = 0; i < ids.size(); ++i) {
    auto& node = result[i];
    if (node && !node->is_loaded()) {
      lite3::NodeID owner = resolver_.get_node_owner(ids[i]);
      if (owner != resolver_.get_local_node_id()) {
        remote_requests[owner].push_back(ids[i]);
      }
    }
  }

  if (remote_requests.empty()) return result;

  std::vector<std::pair<lite3::NodeID, std::future<std::unordered_map<uint64_t, std::string>>>> futures;
  for (auto& [owner, batch] : remote_requests) {
    futures.push_back({owner, remote_client_->get_nodes_batch_async(owner, batch, principal_id)});
  }

  for (auto& pair : futures) {
    try {
      auto batch_results = pair.second.get();
      for (auto& [id, payload] : batch_results) {
        swizzle_node(id, payload);
      }
    } catch (...) {}
  }

  for (size_t i = 0; i < ids.size(); ++i) {
    if (result[i] && !result[i]->is_loaded()) {
      auto refreshed = get_node(ids[i]);
      result[i] = (refreshed && refreshed->is_loaded()) ? std::move(refreshed) : nullptr;
    }
  }

  return result;
}

std::vector<std::shared_ptr<Node>> Engine::get_nodes_by_prefix(const std::string& prefix, uint32_t principal_id) {
  auto keys = store_->get_prefix_keys_all_shards(prefix, "", settings_.prefix_scan_limit);
  std::vector<uint64_t> ids;
  ids.reserve(keys.size());
  for (auto& k : keys) {
      if (k.starts_with("n:{") && k.ends_with("}") && k.size() > 4) {
          std::string_view hex_sv(k.data() + 3, k.size() - 4);
          uint64_t parsed_id = 0;
          auto [ptr, ec] = std::from_chars(hex_sv.data(), hex_sv.data() + hex_sv.size(), parsed_id, 16);
          if (ec == std::errc{} && ptr == hex_sv.data() + hex_sv.size()) {
              ids.push_back(parsed_id);
          }
      }
  }
  return fetch_nodes(ids, principal_id);
}

std::vector<std::pair<std::string, lite3cpp::Buffer>> Engine::scan_kv_prefix(
    const std::string& prefix,
    const std::string& start_key,
    size_t limit,
    uint32_t principal_id) {
    return store_->scan_prefix_kv(prefix, start_key, limit, principal_id);
}

void Engine::put_node(uint64_t id, std::string payload) {
  // if(0) std::fprintf(stderr, "[Engine] put_node %016llx: payload_size=%zu\n", (unsigned long long)id, payload.size());
  lite3::NodeID owner = resolver_.get_node_owner(id);
  
  if (owner != resolver_.get_local_node_id()) {
    invalidate_node_cache(id);
    try {
        remote_client_->put_node_async(owner, id, payload);
        return;
    } catch (const std::exception& e) {
        if(0) std::fprintf(stderr, "[Engine::put_node] Remote RPC Failed: %s\n", e.what()); //std::fflush(stderr);
    }
  }

  const uint8_t* ptr = reinterpret_cast<const uint8_t*>(payload.data());
  std::string binary_payload;
  auto ts = hlc_.now();

  if (payload.size() >= sizeof(lite3cpp::PackedNodeLayout) && (ptr[0] == 0x06 || ptr[0] == 0x07)) {
      try {
          lite3cpp::Buffer buf(ptr, payload.size());
          ts.write_to_buffer(buf, 0, "_hlc");
          binary_payload = std::string(reinterpret_cast<const char*>(buf.data()), buf.size());
      } catch (...) {
          binary_payload = std::move(payload);
      }
  } else {
      try {
          lite3cpp::Buffer buf = lite3cpp::lite3_json::from_json_string(payload.empty() ? "{}" : payload);
          ts.write_to_buffer(buf, 0, "_hlc");
          binary_payload = std::string(reinterpret_cast<const char*>(buf.data()), buf.size());
      } catch (...) {
          binary_payload = std::move(payload);
      }
  }

  std::string key = std::string(KeyBuilder::node_key(id));
  broadcast_replication(key, binary_payload, resolver_.get_local_cluster_id());
  
  {
      std::lock_guard<std::mutex> rmw_lock(node_rmw_locks_[id % node_rmw_locks_.size()]);
      store_->put(std::move(key), std::move(binary_payload));
  }

  invalidate_node_cache(id);
}

void Engine::put_node(std::string_view uuid, std::string payload) {
    put_node(resolver_.parse_uuid(uuid), std::move(payload));
}

void Engine::replicate_key(const std::string& key, std::string payload, uint16_t origin_cluster_id) {
    if (key.starts_with("e:out:") || key.starts_with("e:in:")) {
        has_legacy_kv_edges_.store(true, std::memory_order_relaxed);
    }
    if (key.starts_with("sys:")) {
        store_->put(key, std::move(payload));
        return;
    }

    try {
        // Extract node ID from key format: n:{id} or e:out:{id}:... or e:in:{id}:...
        size_t start = key.find('{');
        size_t end = key.find('}', start);
        
        if (start == std::string::npos || end == std::string::npos) {
          store_->put(key, payload);
          return;
        }

        uint64_t id = 0;
        lite3::NodeID owner;
        {
          std::string_view id_sv(key.data() + start + 1, end - start - 1);
          auto [ptr, ec] = std::from_chars(id_sv.data(), id_sv.data() + id_sv.size(), id, 16);
          if (ec != std::errc{} || ptr != id_sv.data() + id_sv.size()) {
            store_->put(key, payload);
            return;
          }
          owner = resolver_.get_local_shard_owner(id);

          if (owner != resolver_.get_local_node_id()) {
            remote_client_->replicate_async(owner, key, payload, origin_cluster_id);
            return;
          }
        }

        // Conflict Resolution: Last-Writer-Wins using HLC
        std::string binary_payload;
        try {
            lite3cpp::Buffer in_buf;
            bool is_binary = false;
            const uint8_t* in_ptr = reinterpret_cast<const uint8_t*>(payload.data());
            if (payload.size() >= sizeof(lite3cpp::PackedNodeLayout) && (in_ptr[0] == 0x06 || in_ptr[0] == 0x07)) {
                in_buf = lite3cpp::Buffer::from_view(in_ptr, payload.size());
                is_binary = true;
                binary_payload = payload;
            }
            if (!is_binary) {
                try {
                    in_buf = lite3cpp::lite3_json::from_json_string(payload);
                    binary_payload = std::string(reinterpret_cast<const char*>(in_buf.data()), in_buf.size());
                } catch (...) {
                    binary_payload = payload;
                }
            }

            HLCTimestamp remote_ts = HLCTimestamp::read_from_buffer(in_buf, 0, "_hlc");
            if (remote_ts.wall_time == 0) {
                remote_ts = HLCTimestamp::read_from_buffer(in_buf, 0, "ts");
            }
            if (remote_ts.wall_time > 0) {
                hlc_.update(remote_ts);

                auto local_blob = store_->get_blob(key);
                if (local_blob && local_blob->view().size() >= sizeof(lite3cpp::PackedNodeLayout)) {
                    auto lv = local_blob->view();
                    auto local_data = lite3cpp::Buffer::from_view(lv.data(), lv.size());
                    HLCTimestamp local_ts = HLCTimestamp::read_from_buffer(local_data, 0, "_hlc");
                    if (local_ts.wall_time == 0) {
                        local_ts = HLCTimestamp::read_from_buffer(local_data, 0, "ts");
                    }
                    if (local_ts.wall_time > 0 && !(remote_ts > local_ts)) {
                        // Stale update, ignore
                        return;
                    }
                }
            }
        } catch (...) {
            binary_payload = payload;
        }

        store_->put(key, std::move(binary_payload));

        if (key.starts_with("n:{")) {
            invalidate_node_cache(id);
        }
    } catch (...) {
        store_->put(key, std::move(payload));
    }
}

void Engine::broadcast_replication(const std::string& key, const std::string& payload, uint16_t origin_cluster_id) {
    // Loop Prevention: Only nodes in the cluster that originated the write should broadcast to remote clusters.
    if (origin_cluster_id != resolver_.get_local_cluster_id()) {
        return;
    }

    auto remote_clusters = resolver_.get_remote_cluster_ids();
    for (auto cluster_id : remote_clusters) {
        remote_client_->replicate_async(cluster_id, key, payload, origin_cluster_id);
    }
}

void Engine::put_system_key(const std::string& key, const std::string& payload, uint32_t principal_id) {
    // Authorization: only ADMIN can write system keys
    if (principal_id != ADMIN_UID) {
        throw std::runtime_error("Unauthorized: Only ADMIN can modify system metadata");
    }
    if (key.starts_with("e:out:") || key.starts_with("e:in:")) {
        has_legacy_kv_edges_.store(true, std::memory_order_relaxed);
    }

    // System keys (sys:) are special. We want them on ALL nodes eventually.
    // We broadcast to all known peers (local and remote)
    auto peers = resolver_.get_all_node_ids();
    for (auto node_id : peers) {
        if (node_id != resolver_.get_local_node_id()) {
            remote_client_->replicate_async(node_id, key, payload, resolver_.get_local_cluster_id());
        }
    }
    
    store_->put(key, payload);
}

void Engine::del_node(uint64_t id) {
  lite3::NodeID owner = resolver_.get_node_owner(id);
  
  if (owner != resolver_.get_local_node_id()) {
    // Phase 5 Pending: Remote del_node RPC
    invalidate_node_cache(id);
    return;
  }

  std::string key = std::string(KeyBuilder::node_key(id));
  {
      std::lock_guard<std::mutex> rmw_lock(node_rmw_locks_[id % node_rmw_locks_.size()]);
      store_->del(key);
  }

  auto hlc_ts = hlc_.now();
  std::vector<AdjacencyStore::IncidentEdgeRecord> removed;
  {
    std::unique_lock<std::mutex> edge_lock(edge_wal_ ? edge_wal_->write_mutex() : local_edge_mu_);
    removed = adjacency_store_.remove_vertex(id);
    if (edge_wal_ && !removed.empty()) {
      std::vector<EdgeMutationOp> del_ops;
      del_ops.reserve(removed.size());
      for (const auto& rec : removed) {
        del_ops.push_back(EdgeMutationOp{
            .src_id = rec.src_id,
            .dst_id = rec.dst_id,
            .hlc_timestamp = hlc_ts.wall_time,
            .label_id = rec.label_id,
            .flags = EdgeMutationOp::OP_DELETE,
            .weight = rec.weight
        });
      }
      edge_wal_->append_batch_unlocked(del_ops);
    }
  }

  for (const auto& rec : removed) {
    if ((rec.flags & PackedEdge::FLAG_HAS_PROPERTIES) != 0 ||
        !resolver_.is_local(rec.src_id) ||
        !resolver_.is_local(rec.dst_id) ||
        resolver_.has_remote_clusters()) {
      std::string lbl(label_registry_.get_label(rec.label_id));
      if (!lbl.empty()) {
        edge_coordinator_->atomic_del_edge(rec.src_id, lbl, static_cast<double>(rec.weight), rec.dst_id).get();
      }
    }
    if (rec.src_id != id) invalidate_node_cache(rec.src_id);
    if (rec.dst_id != id) invalidate_node_cache(rec.dst_id);
  }

  invalidate_node_cache(id);
}

void Engine::flush() {
  if (edge_wal_) {
      edge_wal_->flush();
  }
  if (!dict_path_.empty()) {
      label_registry_.persist();
  }
  store_->wait_all_shards();
  store_->flush();
}

void Engine::compact_wal() {
  if (store_) {
      store_->compact_wal();
  }
  if (edge_wal_) {
      edge_wal_->compact(adjacency_store_);
  }
  if (!dict_path_.empty()) {
      label_registry_.persist();
  }
}

std::string Engine::format_weight(double weight) {
  return std::string(KeyBuilder::format_weight(weight));
}

void Engine::add_edge(uint64_t src_id, std::string_view label,
                      double weight, uint64_t dst_id,
                      std::string_view payload) {
  uint16_t lid = label_registry_.get_or_register(label);
  auto hlc_ts = hlc_.now();
  const bool has_props = !payload.empty() && payload != "{}";
  uint16_t flags = PackedEdge::FLAG_ACTIVE | (has_props ? PackedEdge::FLAG_HAS_PROPERTIES : 0);
  {
      std::unique_lock<std::mutex> edge_lock(edge_wal_ ? edge_wal_->write_mutex() : local_edge_mu_);
      bool inserted = adjacency_store_.add_edge(src_id, dst_id, lid, static_cast<float>(weight), flags, 0, hlc_ts.wall_time);
      if (edge_wal_ && (inserted || has_props)) {
          EdgeMutationOp op{src_id, dst_id, hlc_ts.wall_time, lid, flags, static_cast<float>(weight)};
          edge_wal_->append_unlocked(op);
      }
  }
  if (__builtin_expect(has_props || !resolver_.is_local(src_id) || !resolver_.is_local(dst_id) || resolver_.has_remote_clusters(), 0)) {
      edge_coordinator_->atomic_put_edge(src_id, std::string(label), weight, dst_id, std::string(payload)).get();
  }
  
  // Cache Invalidation
  if (has_cached_nodes_.load(std::memory_order_relaxed)) {
      invalidate_node_cache(src_id);
      invalidate_node_cache(dst_id);
  }
}

void Engine::add_edge(std::string_view src_uuid, std::string_view label,
                      double weight, std::string_view dst_uuid,
                      std::string_view payload) {
    add_edge(resolver_.parse_uuid(src_uuid), label, weight, resolver_.parse_uuid(dst_uuid), payload);
}

void Engine::del_edge(uint64_t src_id, std::string_view label,
                      double weight, uint64_t dst_id) {
  uint16_t lid = label_registry_.get_or_register(label);
  auto hlc_ts = hlc_.now();
  uint16_t removed_flags = 0;
  float removed_weight = static_cast<float>(weight);
  bool deleted = false;
  {
      std::unique_lock<std::mutex> edge_lock(edge_wal_ ? edge_wal_->write_mutex() : local_edge_mu_);
      deleted = adjacency_store_.del_edge(src_id, dst_id, lid, static_cast<float>(weight), &removed_flags, &removed_weight);
      if (edge_wal_ && deleted) {
          EdgeMutationOp op{src_id, dst_id, hlc_ts.wall_time, lid, EdgeMutationOp::OP_DELETE, removed_weight};
          edge_wal_->append_unlocked(op);
      }
  }
  if ((removed_flags & PackedEdge::FLAG_HAS_PROPERTIES) != 0 ||
      !deleted ||
      !resolver_.is_local(src_id) ||
      !resolver_.is_local(dst_id) ||
      resolver_.has_remote_clusters()) {
      edge_coordinator_->atomic_del_edge(src_id, std::string(label), weight, dst_id).get();
  }

  // Cache Invalidation
  if (has_cached_nodes_.load(std::memory_order_relaxed)) {
      invalidate_node_cache(src_id);
      invalidate_node_cache(dst_id);
  }
}

bool Engine::apply_batch(const lite3cpp::Buffer& buffer, uint32_t principal_id) {
    using namespace lite3cpp::literals;
    if (buffer.size() < sizeof(lite3cpp::PackedNodeLayout)) return true;
    auto arr_ofs_opt = buffer.try_get_arr(0, "m"_hk);
    if (!arr_ofs_opt) return true;
    const size_t arr_ofs = *arr_ofs_opt;
    const size_t count = MutationBatch::item_count(buffer);
    if (count == 0) return true;

    try {
        std::vector<MutationItem> items;
        items.reserve(count);
        uint64_t node_stripe_mask = 0;

        for (auto it = buffer.begin(arr_ofs), end = buffer.end(arr_ofs); it != end; ++it) {
            if (it->value_type != lite3cpp::Type::Object) continue;
            const size_t item_ofs = it->value_offset + 1;
            if (item_ofs + sizeof(lite3cpp::PackedNodeLayout) > buffer.size()) continue;

            MutationItem item = MutationBatch::read_item_at_offset(buffer, item_ofs);
            if (item.op == MutationOp::PutNode || item.op == MutationOp::DelNode || item.op == MutationOp::IncNodeInt) {
                node_stripe_mask |= (1ULL << (item.src % node_rmw_locks_.size()));
            }
            items.push_back(item);
        }
        if (items.empty()) return true;

        std::array<std::unique_lock<std::mutex>, 64> rmw_locks;
        size_t num_rmw_locks = 0;
        {
            uint64_t mask = node_stripe_mask;
            while (mask != 0) {
                const unsigned s = static_cast<unsigned>(std::countr_zero(mask));
                rmw_locks[num_rmw_locks++] = std::unique_lock<std::mutex>(node_rmw_locks_[s]);
                mask &= (mask - 1);
            }
        }

        std::vector<uint64_t> nodes_to_invalidate;
        nodes_to_invalidate.reserve(items.size());

        const auto now = store_->get_clock().now();
        const auto edge_hlc = hlc_.now();

        // Pre-build metadata blobs once per batch
        std::string meta_put_bytes;
        std::string meta_del_bytes;
        {
            lite3cpp::Buffer mbuf(160);
            mbuf.init_object();
            mbuf.set_i64(0, "ts"_hk, now.wall_time);
            mbuf.set_i64(0, "l"_hk, now.logical);
            mbuf.set_i64(0, "n"_hk, now.node_id);
            meta_put_bytes.assign(reinterpret_cast<const char*>(mbuf.data()), mbuf.size());
            mbuf.set_bool(0, "tombstone"_hk, true);
            meta_del_bytes.assign(reinterpret_cast<const char*>(mbuf.data()), mbuf.size());
        }
        const std::string_view meta_put_sv(meta_put_bytes);
        const std::string_view meta_del_sv(meta_del_bytes);

        auto has_write_perm = [&](std::string_view key) -> bool {
            if (principal_id == INTERNAL_UID || principal_id == ADMIN_UID || principal_id == 0) {
                return true;
            }
            auto perm = store_->credentials().check_permission(principal_id, key);
            return (perm & l3kv::Permission::WRITE) || (perm & l3kv::Permission::ADMIN);
        };

        auto require_write_perm = [&](std::string_view key) {
            if (!has_write_perm(key)) {
                throw std::runtime_error("Unauthorized: Access Denied for key " + std::string(key));
            }
        };

        auto store_has_live_key = [&](std::string_view key) -> bool {
            auto v = store_->get_view(key);
            return v != nullptr && v->buf_.size() > 0;
        };

        // Unified per-key final state staging (resolves same-batch Put->Del & Del->Put ordering cleanly)
        struct StagedKvEntry {
            bool is_delete{false};
            std::string value;
        };
        std::vector<std::string> staged_kv_keys;
        staged_kv_keys.reserve(items.size() * 2);
        std::vector<StagedKvEntry> staged_kv_entries;
        staged_kv_entries.reserve(items.size() * 2);
        std::unordered_map<std::string, size_t> staged_kv_index;
        staged_kv_index.reserve(items.size() * 2);

        auto stage_kv_put = [&](std::string_view key, std::string_view val) {
            require_write_perm(key);
            std::string kstr(key);
            if (auto it = staged_kv_index.find(kstr); it != staged_kv_index.end()) {
                auto& entry = staged_kv_entries[it->second];
                entry.is_delete = false;
                entry.value.assign(val.data(), val.size());
            } else {
                const size_t idx = staged_kv_entries.size();
                staged_kv_keys.push_back(kstr);
                staged_kv_index.emplace(std::move(kstr), idx);
                staged_kv_entries.push_back({false, std::string(val)});
            }
        };

        auto stage_kv_del = [&](std::string_view key) {
            require_write_perm(key);
            std::string kstr(key);
            if (auto it = staged_kv_index.find(kstr); it != staged_kv_index.end()) {
                auto& entry = staged_kv_entries[it->second];
                entry.is_delete = true;
                entry.value.clear();
            } else {
                const size_t idx = staged_kv_entries.size();
                staged_kv_keys.push_back(kstr);
                staged_kv_index.emplace(std::move(kstr), idx);
                staged_kv_entries.push_back({true, {}});
            }
        };

        enum class StagedEdgeKind : uint8_t { AddEdge, DelEdge, DelNode };
        struct StagedEdgeOp {
            StagedEdgeKind kind;
            uint64_t src;
            uint64_t dst;
            uint16_t label_id;
            float weight;
            uint64_t timestamp;
            bool has_props;
        };
        std::vector<StagedEdgeOp> staged_edge_ops;
        staged_edge_ops.reserve(items.size());

        for (const MutationItem& item : items) {
            switch (item.op) {
                case MutationOp::PutRaw: {
                    if (item.key.starts_with("e:out:") || item.key.starts_with("e:in:")) {
                        has_legacy_kv_edges_.store(true, std::memory_order_relaxed);
                    }
                    stage_kv_put(item.key, item.value);
                    break;
                }
                case MutationOp::CasRaw: {
                    require_write_perm(item.key);
                    auto res = store_->cas_put(std::string(item.key), std::string(item.value),
                                               item.expected_hash, principal_id, item.ttl_ms);
                    if (!res.ok) {
                        return false;
                    }
                    break;
                }
                case MutationOp::DelRaw: {
                    stage_kv_del(item.key);
                    break;
                }
                case MutationOp::PutNode: {
                    nodes_to_invalidate.push_back(item.src);
                    std::string_view nkey = KeyBuilder::node_key(item.src);
                    stage_kv_put(nkey, item.value);
                    break;
                }
                case MutationOp::IncNodeInt: {
                    nodes_to_invalidate.push_back(item.src);
                    std::string nkey(KeyBuilder::node_key(item.src));
                    require_write_perm(nkey);

                    lite3cpp::Buffer nbuf;
                    auto nit = staged_kv_index.find(nkey);
                    if (nit != staged_kv_index.end()) {
                        const auto& staged = staged_kv_entries[nit->second];
                        if (staged.is_delete) {
                            // Node was deleted earlier in this batch: start fresh from empty object
                            nbuf.init_object();
                        } else if (l3kv::is_json_payload(staged.value)) {
                            try {
                                nbuf = lite3cpp::lite3_json::from_json_string(staged.value);
                            } catch (...) {
                                nbuf.init_object();
                            }
                        } else if (staged.value.size() >= sizeof(lite3cpp::PackedNodeLayout)) {
                            nbuf = lite3cpp::Buffer(reinterpret_cast<const uint8_t*>(staged.value.data()), staged.value.size());
                        } else {
                            nbuf.init_object();
                        }
                    } else {
                        auto blob = store_->get_blob(nkey, principal_id);
                        if (blob && blob->buf_.size() >= sizeof(lite3cpp::PackedNodeLayout)) {
                            nbuf = blob->buf_;
                        } else {
                            nbuf.init_object();
                        }
                    }

                    apply_inc_node_int_to_buffer(nbuf, item.key, item.int_delta);
                    stage_kv_put(nkey, std::string_view(reinterpret_cast<const char*>(nbuf.data()), nbuf.size()));
                    break;
                }
                case MutationOp::DelNode: {
                    std::string nkey(KeyBuilder::node_key(item.src));
                    require_write_perm(nkey);
                    nodes_to_invalidate.push_back(item.src);
                    stage_kv_del(nkey);

                    // Clean up any existing property-bearing incident edge keys in KV batch
                    auto out_snap = adjacency_store_.get_outgoing_edges_snapshot(item.src);
                    for (const auto& pe : out_snap) {
                        if (pe.has_properties() || resolver_.has_remote_clusters()) {
                            std::string_view lbl = label_registry_.get_label(pe.label_id);
                            if (!lbl.empty()) {
                                stage_kv_del(KeyBuilder::edge_out_key(item.src, lbl, pe.weight, pe.target_id));
                                stage_kv_del(KeyBuilder::edge_in_key(pe.target_id, lbl, item.src));
                            }
                        }
                    }
                    auto in_snap = adjacency_store_.get_incoming_edges_snapshot(item.src);
                    for (const auto& pe : in_snap) {
                        if (pe.has_properties() || resolver_.has_remote_clusters()) {
                            std::string_view lbl = label_registry_.get_label(pe.label_id);
                            if (!lbl.empty()) {
                                stage_kv_del(KeyBuilder::edge_out_key(pe.target_id, lbl, pe.weight, item.src));
                                stage_kv_del(KeyBuilder::edge_in_key(item.src, lbl, pe.target_id));
                            }
                        }
                    }
                    // Also clean up any property-bearing edges incident to item.src staged earlier in this batch
                    for (const auto& prev_op : staged_edge_ops) {
                        if (prev_op.kind == StagedEdgeKind::AddEdge &&
                            (prev_op.has_props || resolver_.has_remote_clusters()) &&
                            (prev_op.src == item.src || prev_op.dst == item.src)) {
                            std::string_view lbl = label_registry_.get_label(prev_op.label_id);
                            if (!lbl.empty()) {
                                stage_kv_del(KeyBuilder::edge_out_key(prev_op.src, lbl, prev_op.weight, prev_op.dst));
                                stage_kv_del(KeyBuilder::edge_in_key(prev_op.dst, lbl, prev_op.src));
                            }
                        }
                    }

                    staged_edge_ops.push_back({StagedEdgeKind::DelNode, item.src, 0, 0, 0.0f, edge_hlc.wall_time, false});
                    break;
                }
                case MutationOp::AddEdge: {
                    const bool is_privileged = (principal_id == INTERNAL_UID || principal_id == ADMIN_UID || principal_id == 0);
                    const bool has_props = !item.value.empty() && item.value != "{}";
                    if (!is_privileged) {
                        require_write_perm(KeyBuilder::edge_out_key(item.src, item.label, item.weight, item.dst));
                        require_write_perm(KeyBuilder::edge_in_key(item.dst, item.label, item.src));
                    }

                    nodes_to_invalidate.push_back(item.src);
                    nodes_to_invalidate.push_back(item.dst);
                    uint16_t lid = label_registry_.get_or_register(item.label);
                    staged_edge_ops.push_back({StagedEdgeKind::AddEdge, item.src, item.dst, lid, static_cast<float>(item.weight), edge_hlc.wall_time, has_props});

                    if (has_props || resolver_.has_remote_clusters()) {
                        std::string out_key(KeyBuilder::edge_out_key(item.src, item.label, item.weight, item.dst));
                        std::string in_key(KeyBuilder::edge_in_key(item.dst, item.label, item.src));

                        lite3cpp::Buffer ebuf;
                        ebuf.init_object();
                        if (has_props) {
                            const uint8_t* ptr = reinterpret_cast<const uint8_t*>(item.value.data());
                            if (item.value.size() >= sizeof(lite3cpp::PackedNodeLayout) && (ptr[0] == 0x06 || ptr[0] == 0x07)) {
                                try {
                                    lite3cpp::Buffer props_buf = lite3cpp::Buffer::from_view(ptr, item.value.size());
                                    size_t props_ofs = ebuf.set_obj(0, "props"_hk);
                                    copy_batch_edge_props(props_buf, 0, ebuf, props_ofs);
                                } catch (...) {
                                    ebuf.set_str(0, "props"_hk, item.value);
                                }
                            } else {
                                try {
                                    lite3cpp::Buffer props_buf = lite3cpp::lite3_json::from_json_string(item.value);
                                    size_t props_ofs = ebuf.set_obj(0, "props"_hk);
                                    copy_batch_edge_props(props_buf, 0, ebuf, props_ofs);
                                } catch (...) {
                                    ebuf.set_str(0, "props"_hk, item.value);
                                }
                            }
                        }
                        std::string_view epayload_sv(reinterpret_cast<const char*>(ebuf.data()), ebuf.size());
                        stage_kv_put(out_key, epayload_sv);
                        stage_kv_put(in_key, epayload_sv);
                    }
                    break;
                }
                case MutationOp::DelEdge: {
                    std::string_view out_key_sv = KeyBuilder::edge_out_key(item.src, item.label, item.weight, item.dst);
                    std::string_view in_key_sv = KeyBuilder::edge_in_key(item.dst, item.label, item.src);
                    require_write_perm(out_key_sv);
                    require_write_perm(in_key_sv);

                    nodes_to_invalidate.push_back(item.src);
                    nodes_to_invalidate.push_back(item.dst);
                    uint16_t lid = label_registry_.get_or_register(item.label);
                    staged_edge_ops.push_back({StagedEdgeKind::DelEdge, item.src, item.dst, lid, static_cast<float>(item.weight), edge_hlc.wall_time, false});

                    if (resolver_.has_remote_clusters() ||
                        (!staged_kv_index.empty() && (staged_kv_index.contains(std::string(out_key_sv)) || staged_kv_index.contains(std::string(in_key_sv)))) ||
                        store_has_live_key(out_key_sv) ||
                        store_has_live_key(in_key_sv)) {
                        stage_kv_del(out_key_sv);
                        stage_kv_del(in_key_sv);
                    }
                    break;
                }
            }
        }

        if (!staged_kv_entries.empty()) {
            std::vector<std::string> meta_keys;
            meta_keys.reserve(staged_kv_entries.size());
            for (const auto& k : staged_kv_keys) {
                meta_keys.emplace_back(KeyBuilder::meta_key(k));
            }

            std::vector<l3kv::BatchOp> wal_batch;
            wal_batch.reserve(staged_kv_entries.size() * 2);
            std::map<size_t, l3kv::Engine::ShardMutation> shard_works;

            for (size_t i = 0; i < staged_kv_entries.size(); ++i) {
                const std::string& key = staged_kv_keys[i];
                const std::string& mkey = meta_keys[i];
                const auto& entry = staged_kv_entries[i];

                const size_t k_shard = store_->get_routing_shard(key);
                const size_t m_shard = store_->get_routing_shard(mkey);

                if (entry.is_delete) {
                    wal_batch.push_back({l3kv::WalOp::DELETE_, key, ""});
                    wal_batch.push_back({l3kv::WalOp::PUT, mkey, meta_del_sv});
                    shard_works[k_shard].dels.push_back(key);
                    shard_works[m_shard].puts.emplace_back(mkey, meta_del_bytes);
                } else {
                    wal_batch.push_back({l3kv::WalOp::PUT, key, entry.value});
                    wal_batch.push_back({l3kv::WalOp::PUT, mkey, meta_put_sv});
                    shard_works[k_shard].puts.emplace_back(key, entry.value);
                    shard_works[m_shard].puts.emplace_back(mkey, meta_put_bytes);
                }
            }

            store_->apply_batch_mutations(wal_batch, std::move(shard_works));
        }

        // Release striped node RMW locks now that KV read-modify-write is committed
        for (size_t i = 0; i < num_rmw_locks; ++i) {
            rmw_locks[num_rmw_locks - 1 - i].unlock();
        }
        num_rmw_locks = 0;

        std::vector<AdjacencyStore::IncidentEdgeRecord> extra_prop_dels;
        if (!staged_edge_ops.empty()) {
            std::unique_lock<std::mutex> edge_lock(edge_wal_ ? edge_wal_->write_mutex() : local_edge_mu_);
            std::vector<EdgeMutationOp> wal_ops;
            if (edge_wal_) {
                wal_ops.reserve(staged_edge_ops.size());
            }

            for (const auto& op : staged_edge_ops) {
                if (op.kind == StagedEdgeKind::AddEdge) {
                    uint16_t flags = PackedEdge::FLAG_ACTIVE | (op.has_props ? PackedEdge::FLAG_HAS_PROPERTIES : 0);
                    bool inserted = adjacency_store_.add_edge(op.src, op.dst, op.label_id, op.weight, flags, 0, op.timestamp);
                    if (edge_wal_ && (inserted || op.has_props)) {
                        wal_ops.push_back(EdgeMutationOp{op.src, op.dst, op.timestamp, op.label_id, flags, op.weight});
                    }
                } else if (op.kind == StagedEdgeKind::DelEdge) {
                    uint16_t rflags = 0;
                    float rweight = op.weight;
                    bool deleted = adjacency_store_.del_edge(op.src, op.dst, op.label_id, op.weight, &rflags, &rweight);
                    if (edge_wal_ && deleted) {
                        wal_ops.push_back(EdgeMutationOp{op.src, op.dst, op.timestamp, op.label_id, EdgeMutationOp::OP_DELETE, rweight});
                    }
                } else if (op.kind == StagedEdgeKind::DelNode) {
                    auto removed = adjacency_store_.remove_vertex(op.src);
                    for (const auto& rec : removed) {
                        nodes_to_invalidate.push_back(rec.src_id);
                        nodes_to_invalidate.push_back(rec.dst_id);
                        if (edge_wal_) {
                            wal_ops.push_back(EdgeMutationOp{rec.src_id, rec.dst_id, op.timestamp, rec.label_id, EdgeMutationOp::OP_DELETE, rec.weight});
                        }
                        if ((rec.flags & PackedEdge::FLAG_HAS_PROPERTIES) != 0) {
                            extra_prop_dels.push_back(rec);
                        }
                    }
                }
            }

            if (edge_wal_ && !wal_ops.empty()) {
                edge_wal_->append_batch_unlocked(wal_ops);
            }
        }

        for (const auto& rec : extra_prop_dels) {
            std::string_view lbl = label_registry_.get_label(rec.label_id);
            if (!lbl.empty()) {
                std::string out_key(KeyBuilder::edge_out_key(rec.src_id, lbl, rec.weight, rec.dst_id));
                std::string in_key(KeyBuilder::edge_in_key(rec.dst_id, lbl, rec.src_id));
                auto is_readded = [&](const std::string& k) {
                    auto it = staged_kv_index.find(k);
                    return it != staged_kv_index.end() && !staged_kv_entries[it->second].is_delete;
                };
                if (!is_readded(out_key) && store_has_live_key(out_key)) store_->del(out_key, principal_id);
                if (!is_readded(in_key) && store_has_live_key(in_key)) store_->del(in_key, principal_id);
            }
        }

        for (uint64_t nid : nodes_to_invalidate) {
            invalidate_node_cache(nid);
        }
        return true;
    } catch (const std::exception& e) {
        return false;
    }
}

} // namespace l3kvg
