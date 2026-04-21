#include "L3KVG/EdgeCoordinator.hpp"
#include "engine/store.hpp"
#include "L3KVG/Engine.hpp" 
#include "L3KVG/KeyBuilder.hpp"
#include <cstdio>
#include <stdexcept>
#include <vector>
#include <future>

namespace l3kvg {

EdgeCoordinator::EdgeCoordinator(l3kv::Engine* store, ClusterResolver& resolver, RemoteL3KVClient& remote_client, uint32_t node_id)
    : store_(store), resolver_(resolver), remote_client_(remote_client), hlc_(node_id) {
    shards_ = std::make_unique<BatchShard[]>(NUM_SHARDS);
    task_pool_ = std::make_unique<ThreadPool>(16); // Managed completion pool
    flush_thread_ = std::thread(&EdgeCoordinator::flush_loop, this);
}

EdgeCoordinator::~EdgeCoordinator() {
    stop_flusher_ = true;
    cv_.notify_all();
    if (flush_thread_.joinable()) {
        flush_thread_.join();
    }
}

std::future<void> EdgeCoordinator::atomic_put_edge(const std::string& src_uuid, const std::string& label, double weight, const std::string& dst_uuid, const std::string& payload) {
    auto ts = hlc_.now();
    
    // Create a pure lite3-cpp buffer for the payload
    lite3cpp::Buffer buf;
    buf.init_object();
    size_t root = 0;
    size_t ts_obj = buf.set_obj(root, "ts");
    buf.set_i64(ts_obj, "wall_time", static_cast<int64_t>(ts.wall_time));
    buf.set_i64(ts_obj, "logical", static_cast<int64_t>(ts.logical));
    buf.set_i64(ts_obj, "node_id", static_cast<int64_t>(ts.node_id));
    
    if (!payload.empty()) {
        // If payload is already a JSON string (for legacy/props), store as string
        // but if we want "pure lite3", we should ideally accept props as a buffer.
        // For now, we'll store props as raw bytes if it looks like a buffer data
        // or just a string.
        buf.set_str(root, "props", payload);
    }

    // Capture raw buffer data
    std::vector<uint8_t> final_payload_data(buf.data(), buf.data() + buf.size());

    lite3::NodeID src_owner = resolver_.get_node_owner(src_uuid);
    lite3::NodeID dst_owner = resolver_.get_node_owner(dst_uuid);
    lite3::NodeID local_id = resolver_.get_local_node_id();

    std::string out_key = std::string(KeyBuilder::edge_out_key(src_uuid, label, weight, dst_uuid));
    std::string in_key = std::string(KeyBuilder::edge_in_key(dst_uuid, label, src_uuid));

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
            
            size_t shard_idx = owner % NUM_SHARDS;
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

void EdgeCoordinator::flush_loop() {
    while (!stop_flusher_) {
        {
            std::unique_lock<std::mutex> lock(cv_mu_);
            cv_.wait_for(lock, std::chrono::milliseconds(2), [this] { 
                if (stop_flusher_) return true;
                for (size_t i = 0; i < NUM_SHARDS; ++i) {
                    std::lock_guard<std::mutex> s_lock(shards_[i].mu);
                    if (!shards_[i].buffer.empty()) return true;
                }
                return false;
            });
        }

        if (stop_flusher_) {
            for (size_t i = 0; i < NUM_SHARDS; ++i) flush_shard(i);
            break;
        }

        for (size_t i = 0; i < NUM_SHARDS; ++i) {
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

    // Identify targets in this shard (could be multiple NodeIDs hashing to same shard)
    std::unordered_map<lite3::NodeID, lite3cpp::Buffer> node_batches;
    std::unordered_map<lite3::NodeID, std::vector<std::shared_ptr<std::promise<void>>>> node_promises;

    for (size_t i = 0; i < to_flush.size(); ++i) {
        auto const& entry = to_flush[i];
        // We need to know which owner this key belongs to.
        // For simplicity in this shard model, we can re-resolve or store owner in BatchEntry.
        // Let's re-resolve for now (fast consistent hash lookup)
        lite3::NodeID owner;
        if (entry.key.starts_with("e:out:")) {
            // parse src_uuid
            size_t start = 6;
            size_t end = entry.key.find(':', start);
            owner = resolver_.get_node_owner(entry.key.substr(start, end - start));
        } else {
            // parse dst_uuid
            size_t start = 5;
            size_t end = entry.key.find(':', start);
            owner = resolver_.get_node_owner(entry.key.substr(start, end - start));
        }

        if (!node_batches.contains(owner)) {
            node_batches[owner].init_object();
        }
        node_batches[owner].set_bytes(0, entry.key, {reinterpret_cast<const std::byte*>(entry.val.data()), entry.val.size()});
        node_promises[owner].push_back(promises[i]);
    }

    for (auto& [owner, batch_buf] : node_batches) {
        // ZeroMQ handles the asynchronous delivery in its own I/O threads.
        // We no longer need to enqueue a task to wait for a future.
        remote_client_.put_batch_binary_async(owner, batch_buf);
        auto p_list = std::move(node_promises[owner]);
        for (auto& p : p_list) p->set_value();
    }
}


} // namespace l3kvg
