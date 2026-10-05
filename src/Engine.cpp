#include "L3KVG/Engine.hpp"
#include "L3KVG/Node.hpp"
#include "L3KVG/Query.hpp"
#include "L3KVG/KeyBuilder.hpp"
#include "L3KVG/MutationBatch.hpp"
#include "engine/store.hpp"
#include <iomanip>
#include <sstream>
#include <iostream>


namespace l3kvg {

Engine::Engine(const std::string &db_path, uint32_t node_id, std::shared_ptr<lite3::ConsistentHash> ring, size_t thread_pool_size, Settings settings)
    : resolver_(std::move(ring), node_id), settings_(std::move(settings)), hlc_(node_id) {
  settings_.node_id = node_id;
  store_ = std::make_unique<l3kv::Engine>(db_path, node_id);
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
}

Engine::~Engine() {
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
    size_t h = get_cache_shard(id);
    auto& shard = *cache_shards_[h];
    std::lock_guard<std::mutex> lock(shard.mutex);
    if (auto it = shard.map.find(id); it != shard.map.end()) {
        shard.lru.erase(it->second.lru_it);
        shard.map.erase(it);
    }
}

Query Engine::query() { return Query(this); }

std::shared_ptr<Node> Engine::get_node(uint64_t id) {
  size_t h = get_cache_shard(id);
  auto& shard = *cache_shards_[h];

  std::lock_guard<std::mutex> lock(shard.mutex);
  if (auto it = shard.map.find(id); it != shard.map.end()) {
    // O(1) zero-allocation LRU update
    shard.lru.splice(shard.lru.begin(), shard.lru, it->second.lru_it);
    return it->second.node;
  }

  if (shard.map.size() >= settings_.node_cache_size_per_shard && !shard.lru.empty()) {
      uint64_t victim = shard.lru.back();
      shard.map.erase(victim);
      shard.lru.pop_back();
  }
  shard.lru.push_front(id);
  auto node = std::make_shared<Node>(this, id);
  shard.map[id] = {node, shard.lru.begin()};
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
  if (auto it = shard.map.find(id); it != shard.map.end()) {
      it->second.node = ptr;
      shard.lru.splice(shard.lru.begin(), shard.lru, it->second.lru_it);
      metrics_.cache_hits.fetch_add(1, std::memory_order_relaxed);
  } else {
      if (shard.map.size() >= settings_.node_cache_size_per_shard && !shard.lru.empty()) {
          uint64_t victim = shard.lru.back();
          shard.map.erase(victim);
          shard.lru.pop_back();
      }
      shard.lru.push_front(id);
      shard.map[id] = {ptr, shard.lru.begin()};
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

std::vector<std::shared_ptr<Node>> Engine::fetch_nodes(const std::vector<uint64_t>& ids, uint32_t principal_id) {
  if(0) std::fprintf(stderr, "  [Engine] fetch_nodes: requested %zu nodes\n", ids.size()); //std::fflush(stderr);
  std::unordered_map<lite3::NodeID, std::vector<uint64_t>> remote_requests;
  std::vector<std::shared_ptr<Node>> result;
  result.reserve(ids.size());

  for (const auto& id : ids) {
    auto node = get_node(id);
    if (node && !node->is_loaded()) {
      node->ensure_loaded();
      if (!node->is_loaded()) {
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
    futures.push_back({owner, remote_client_->get_nodes_batch_async(owner, batch, principal_id)});
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

std::vector<std::shared_ptr<Node>> Engine::get_nodes_by_prefix(const std::string& prefix, uint32_t principal_id) {
  auto keys = store_->get_prefix_keys_all_shards(prefix, "", settings_.prefix_scan_limit);
  std::vector<uint64_t> ids;
  for (auto& k : keys) {
      // Keys are n:{%016llx}
      if (k.starts_with("n:{") && k.ends_with("}")) {
          std::string id_str = k.substr(3, k.size() - 4);
          ids.push_back(std::stoull(id_str, nullptr, 16));
      }
  }
  return fetch_nodes(ids, principal_id);
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
  
  store_->put(std::move(key), std::move(binary_payload));

  invalidate_node_cache(id);
}

void Engine::put_node(std::string_view uuid, std::string payload) {
    put_node(resolver_.parse_uuid(uuid), std::move(payload));
}

void Engine::replicate_key(const std::string& key, std::string payload, uint16_t origin_cluster_id) {
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

        uint64_t id;
        lite3::NodeID owner;
        try {
          std::string id_str = key.substr(start + 1, end - start - 1);
          id = std::stoull(id_str, nullptr, 16);
          owner = resolver_.get_local_shard_owner(id);

          if (owner != resolver_.get_local_node_id()) {
            remote_client_->replicate_async(owner, key, payload, origin_cluster_id);
            return;
          }
        } catch (...) {
          store_->put(key, payload);
          return;
        }

        // Conflict Resolution: Last-Writer-Wins using HLC
        std::string binary_payload;
        try {
            lite3cpp::Buffer in_buf;
            bool is_binary = false;
            const uint8_t* in_ptr = reinterpret_cast<const uint8_t*>(payload.data());
            if (payload.size() >= sizeof(lite3cpp::PackedNodeLayout) && (in_ptr[0] == 0x06 || in_ptr[0] == 0x07)) {
                try {
                    in_buf = lite3cpp::Buffer(in_ptr, payload.size());
                    is_binary = true;
                    binary_payload = payload;
                } catch (...) {}
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

                auto local_data = store_->get(key);
                if (local_data.size() > 0) {
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
  store_->del(key);
  invalidate_node_cache(id);
}

void Engine::flush() {
  store_->wait_all_shards();
  store_->flush();
}

std::string Engine::format_weight(double weight) {
  return std::string(KeyBuilder::format_weight(weight));
}

void Engine::add_edge(uint64_t src_id, std::string label,
                      double weight, uint64_t dst_id,
                      std::string payload) {
  edge_coordinator_->atomic_put_edge(src_id, std::move(label), weight, dst_id, std::move(payload)).get();
  
  // Cache Invalidation
  invalidate_node_cache(src_id);
  invalidate_node_cache(dst_id);
}

void Engine::add_edge(std::string_view src_uuid, std::string label,
                      double weight, std::string_view dst_uuid,
                      std::string payload) {
    add_edge(resolver_.parse_uuid(src_uuid), std::move(label), weight, resolver_.parse_uuid(dst_uuid), std::move(payload));
}

void Engine::del_edge(uint64_t src_id, std::string label,
                      double weight, uint64_t dst_id) {
  edge_coordinator_->atomic_del_edge(src_id, std::move(label), weight, dst_id).get();

  // Cache Invalidation
  invalidate_node_cache(src_id);
  invalidate_node_cache(dst_id);
}

bool Engine::apply_batch(const lite3cpp::Buffer& buffer, uint32_t principal_id) {
    size_t count = MutationBatch::item_count(buffer);
    if (count == 0) return true;

    try {
        std::vector<l3kv::BatchOp> wal_batch;
        std::map<size_t, l3kv::Engine::ShardMutation> shard_works;
        std::vector<uint64_t> nodes_to_invalidate;
        nodes_to_invalidate.reserve(count);

        auto now = store_->get_clock().now();

        auto add_put = [&](const std::string& key, const std::string& val) {
            auto perm = (principal_id == INTERNAL_UID || principal_id == 0) ? l3kv::Permission::ADMIN : store_->credentials().check_permission(principal_id, key);
            if (!(perm & l3kv::Permission::WRITE) && !(perm & l3kv::Permission::ADMIN)) {
                throw std::runtime_error("Unauthorized: Access Denied for key " + key);
            }
            std::string mkey_s(KeyBuilder::meta_key(key));
            lite3cpp::Buffer mbuf; mbuf.init_object();
            mbuf.set_i64(0, "ts", now.wall_time); mbuf.set_i64(0, "l", now.logical); mbuf.set_i64(0, "n", now.node_id);
            std::string meta_val = mbuf.move_to_string();

            wal_batch.push_back({l3kv::WalOp::PUT, key, val});
            wal_batch.push_back({l3kv::WalOp::PUT, mkey_s, meta_val});

            shard_works[store_->get_routing_shard(key)].puts.emplace_back(key, val);
            shard_works[store_->get_routing_shard(mkey_s)].puts.emplace_back(mkey_s, meta_val);
        };

        auto add_del = [&](const std::string& key) {
            auto perm = (principal_id == INTERNAL_UID || principal_id == 0) ? l3kv::Permission::ADMIN : store_->credentials().check_permission(principal_id, key);
            if (!(perm & l3kv::Permission::WRITE) && !(perm & l3kv::Permission::ADMIN)) {
                return; // skip or unauthorized
            }
            std::string mkey_s(KeyBuilder::meta_key(key));
            lite3cpp::Buffer mbuf; mbuf.init_object();
            mbuf.set_i64(0, "ts", now.wall_time); mbuf.set_i64(0, "l", now.logical); mbuf.set_i64(0, "n", now.node_id);
            mbuf.set_bool(0, "tombstone", true);
            std::string meta_val = mbuf.move_to_string();

            wal_batch.push_back({l3kv::WalOp::DELETE_, key, ""});
            wal_batch.push_back({l3kv::WalOp::PUT, mkey_s, meta_val});

            shard_works[store_->get_routing_shard(key)].dels.push_back(key);
            shard_works[store_->get_routing_shard(mkey_s)].puts.emplace_back(mkey_s, meta_val);
        };

        for (size_t i = 0; i < count; ++i) {
            MutationItem item = MutationBatch::read_item(buffer, i);
            switch (item.op) {
                case MutationOp::PutRaw: {
                    add_put(std::string(item.key), std::string(item.value));
                    break;
                }
                case MutationOp::DelRaw: {
                    add_del(std::string(item.key));
                    break;
                }
                case MutationOp::PutNode: {
                    nodes_to_invalidate.push_back(item.src);
                    std::string key = std::string(KeyBuilder::node_key(item.src));
                    add_put(key, std::string(item.value));
                    break;
                }
                case MutationOp::DelNode: {
                    nodes_to_invalidate.push_back(item.src);
                    std::string key = std::string(KeyBuilder::node_key(item.src));
                    add_del(key);
                    break;
                }
                case MutationOp::AddEdge: {
                    nodes_to_invalidate.push_back(item.src);
                    nodes_to_invalidate.push_back(item.dst);
                    std::string out_key = std::string(KeyBuilder::edge_out_key(item.src, std::string(item.label), item.weight, item.dst));
                    std::string in_key = std::string(KeyBuilder::edge_in_key(item.dst, std::string(item.label), item.src));
                    
                    lite3cpp::Buffer ebuf; ebuf.init_object();
                    if (!item.value.empty()) {
                        ebuf.set_str(0, "props", std::string(item.value));
                    }
                    std::string epayload(reinterpret_cast<const char*>(ebuf.data()), ebuf.size());
                    add_put(out_key, epayload);
                    add_put(in_key, epayload);
                    break;
                }
                case MutationOp::DelEdge: {
                    nodes_to_invalidate.push_back(item.src);
                    nodes_to_invalidate.push_back(item.dst);
                    std::string out_key = std::string(KeyBuilder::edge_out_key(item.src, std::string(item.label), item.weight, item.dst));
                    std::string in_key = std::string(KeyBuilder::edge_in_key(item.dst, std::string(item.label), item.src));
                    add_del(out_key);
                    add_del(in_key);
                    break;
                }
            }
        }

        store_->apply_batch_mutations(wal_batch, std::move(shard_works));

        for (uint64_t nid : nodes_to_invalidate) {
            invalidate_node_cache(nid);
        }
        return true;
    } catch (const std::exception& e) {
        return false;
    }
}

} // namespace l3kvg
