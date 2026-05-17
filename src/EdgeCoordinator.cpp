#include "L3KVG/EdgeCoordinator.hpp"
#include "engine/store.hpp"
#include "L3KVG/Engine.hpp" 
#include "L3KVG/KeyBuilder.hpp"
#include <cstdio>
#include <stdexcept>
#include <vector>
#include <future>

namespace l3kvg {

EdgeCoordinator::EdgeCoordinator(l3kv::Engine* store, FederationResolver& resolver, RemoteL3KVClient& remote_client, uint32_t node_id, std::shared_ptr<ThreadPool> pool, const Settings& settings)
    : store_(store), resolver_(resolver), remote_client_(remote_client), hlc_(node_id), 
      num_shards_(settings.edge_write_shards), 
      edge_flush_interval_ms_(settings.edge_flush_interval_ms),
      task_pool_(std::move(pool)) {
    shards_ = std::make_unique<BatchShard[]>(num_shards_);
    flush_thread_ = std::thread(&EdgeCoordinator::flush_loop, this);
}


EdgeCoordinator::~EdgeCoordinator() {
    stop_flusher_ = true;
    cv_.notify_all();
    if (flush_thread_.joinable()) {
        flush_thread_.join();
    }
}

std::future<void> EdgeCoordinator::atomic_put_edge(uint64_t src_id, const std::string& label, double weight, uint64_t dst_id, const std::string& payload) {
    auto ts = hlc_.now();
    
    // Create a pure lite3-cpp buffer for the payload
    std::string full_json = "{\"ts\":" + ts.to_json_string();
    if (!payload.empty()) {
        full_json += ",\"props\":" + payload;
    }
    full_json += "}";
    
    lite3cpp::Buffer buf = lite3cpp::lite3_json::from_json_string(full_json);

    // Capture raw buffer data
    std::vector<uint8_t> final_payload_data(buf.data(), buf.data() + buf.size());

    lite3::NodeID src_owner = resolver_.get_node_owner(src_id);
    lite3::NodeID dst_owner = resolver_.get_node_owner(dst_id);
    lite3::NodeID local_id = resolver_.get_local_node_id();

    std::string out_key = std::string(KeyBuilder::edge_out_key(src_id, label, weight, dst_id));
    std::string in_key = std::string(KeyBuilder::edge_in_key(dst_id, label, src_id));

    std::vector<std::future<void>> futures;

    auto handle_write = [&](lite3::NodeID owner, const std::string& key) {
        if (owner == local_id) {
            size_t shard_idx = store_->get_routing_shard(key);
            // Engine::put takes a string, we'll cast the data
            std::string binary_str(reinterpret_cast<const char*>(final_payload_data.data()), final_payload_data.size());
            futures.push_back(store_->submit_to_shard_idx(shard_idx, [this, key, binary_str]() {
                store_->put(key, binary_str);
            }));
        } else {
            auto prom = std::make_shared<std::promise<void>>();
            futures.push_back(prom->get_future());
            
            size_t shard_idx = owner % num_shards_;
            auto& shard = shards_[shard_idx];
            {
                std::lock_guard<std::mutex> lock(shard.mu);
                shard.buffer.push_back({key, final_payload_data});
                shard.promises.push_back(prom);
            }
            
            cv_.notify_all();
        }
    };

    handle_write(src_owner, out_key);
    handle_write(dst_owner, in_key);

    if (futures.size() == 1) return std::move(futures[0]);
    if (futures.empty()) {
        std::promise<void> p;
        p.set_value();
        return p.get_future();
    }

    return std::async(std::launch::deferred, [futs = std::move(futures)]() mutable {
        for (auto& f : futs) f.get();
    });
}

std::future<void> EdgeCoordinator::atomic_del_edge(uint64_t src_id, const std::string& label, double weight, uint64_t dst_id) {
    lite3::NodeID src_owner = resolver_.get_node_owner(src_id);
    lite3::NodeID dst_owner = resolver_.get_node_owner(dst_id);
    lite3::NodeID local_id = resolver_.get_local_node_id();

    std::string out_key = std::string(KeyBuilder::edge_out_key(src_id, label, weight, dst_id));
    std::string in_key = std::string(KeyBuilder::edge_in_key(dst_id, label, src_id));

    std::vector<std::future<void>> futures;

    auto handle_del = [&](lite3::NodeID owner, const std::string& key) {
        if (owner == local_id) {
            size_t shard_idx = store_->get_routing_shard(key);
            futures.push_back(store_->submit_to_shard_idx(shard_idx, [this, key]() {
                store_->del(key);
            }));
        } else {
            // Phase 5 Pending: Remote del_edge batching/RPC
        }
    };

    handle_del(src_owner, out_key);
    handle_del(dst_owner, in_key);

    if (futures.size() == 1) return std::move(futures[0]);
    if (futures.empty()) {
        std::promise<void> p;
        p.set_value();
        return p.get_future();
    }

    return std::async(std::launch::deferred, [futs = std::move(futures)]() mutable {
        for (auto& f : futs) f.get();
    });
}

void EdgeCoordinator::flush_loop() {
    while (!stop_flusher_) {
        {
            std::unique_lock<std::mutex> lock(cv_mu_);
            cv_.wait_for(lock, std::chrono::milliseconds(edge_flush_interval_ms_), [this] { 
                if (stop_flusher_) return true;
                for (size_t i = 0; i < num_shards_; ++i) {
                    std::lock_guard<std::mutex> s_lock(shards_[i].mu);
                    if (!shards_[i].buffer.empty()) return true;
                }
                return false;
            });
        }

        if (stop_flusher_) {
            for (size_t i = 0; i < num_shards_; ++i) flush_shard(i);
            break;
        }

        for (size_t i = 0; i < num_shards_; ++i) {
            flush_shard(i);
        }
    }
}

void EdgeCoordinator::flush_shard(size_t shard_idx) {
    std::vector<BatchEntry> to_flush;
    std::vector<std::shared_ptr<std::promise<void>>> promises;
    
    auto& shard = shards_[shard_idx];
    {
        std::lock_guard<std::mutex> lock(shard.mu);
        if (shard.buffer.empty()) return;
        to_flush.swap(shard.buffer);
        promises.swap(shard.promises);
    }

    std::unordered_map<lite3::NodeID, lite3cpp::Buffer> node_batches;
    std::unordered_map<lite3::NodeID, std::vector<std::shared_ptr<std::promise<void>>>> node_promises;

    for (size_t i = 0; i < to_flush.size(); ++i) {
        auto const& entry = to_flush[i];
        lite3::NodeID owner;
        if (entry.key.starts_with("e:out:")) {
            size_t start = entry.key.find('{');
            size_t end = entry.key.find('}', start);
            std::string id_str = entry.key.substr(start + 1, end - start - 1);
            owner = resolver_.get_node_owner(std::stoull(id_str, nullptr, 16));
        } else {
            size_t start = entry.key.find('{');
            size_t end = entry.key.find('}', start);
            std::string id_str = entry.key.substr(start + 1, end - start - 1);
            owner = resolver_.get_node_owner(std::stoull(id_str, nullptr, 16));
        }

        if (!node_batches.contains(owner)) {
            node_batches[owner].init_object();
        }
        node_batches[owner].set_bytes(0, entry.key, {reinterpret_cast<const std::byte*>(entry.val.data()), entry.val.size()});
        node_promises[owner].push_back(promises[i]);
    }

    for (auto& [owner, batch_buf] : node_batches) {
        remote_client_.put_batch_binary_async(owner, batch_buf);
        auto p_list = std::move(node_promises[owner]);
        for (auto& p : p_list) p->set_value();
    }
}

} // namespace l3kvg
