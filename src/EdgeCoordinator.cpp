#include "L3KVG/EdgeCoordinator.hpp"
#include "engine/store.hpp"
#include "L3KVG/Engine.hpp" 
#include "L3KVG/KeyBuilder.hpp"
#include "L3KVG/MutationBatch.hpp"
#include <cstdio>
#include <stdexcept>
#include <vector>
#include <future>

namespace l3kvg {

namespace {

void copy_array(const lite3cpp::Buffer& src, size_t src_arr_ofs, lite3cpp::Buffer& dst, size_t dst_arr_ofs);

void copy_properties(const lite3cpp::Buffer& src, size_t src_obj_ofs, lite3cpp::Buffer& dst, size_t dst_obj_ofs) {
    for (auto it = src.begin(src_obj_ofs); it != src.end(src_obj_ofs); ++it) {
        switch (it->value_type) {
            case lite3cpp::Type::Int64:
                dst.set_i64(dst_obj_ofs, it->key, src.get_i64(src_obj_ofs, it->key));
                break;
            case lite3cpp::Type::Float64:
                dst.set_f64(dst_obj_ofs, it->key, src.get_f64(src_obj_ofs, it->key));
                break;
            case lite3cpp::Type::String:
                dst.set_str(dst_obj_ofs, it->key, src.get_str(src_obj_ofs, it->key));
                break;
            case lite3cpp::Type::Bool:
                dst.set_bool(dst_obj_ofs, it->key, src.get_bool(src_obj_ofs, it->key));
                break;
            case lite3cpp::Type::Null:
                dst.set_null(dst_obj_ofs, it->key);
                break;
            case lite3cpp::Type::Bytes:
                dst.set_bytes(dst_obj_ofs, it->key, src.get_bytes(src_obj_ofs, it->key));
                break;
            case lite3cpp::Type::Object: {
                size_t child_src_obj = src.get_obj(src_obj_ofs, it->key);
                size_t child_dst_obj = dst.set_obj(dst_obj_ofs, it->key);
                copy_properties(src, child_src_obj, dst, child_dst_obj);
                break;
            }
            case lite3cpp::Type::Array: {
                size_t child_src_arr = src.get_arr(src_obj_ofs, it->key);
                size_t child_dst_arr = dst.set_arr(dst_obj_ofs, it->key);
                copy_array(src, child_src_arr, dst, child_dst_arr);
                break;
            }
            default:
                break;
        }
    }
}

void copy_array(const lite3cpp::Buffer& src, size_t src_arr_ofs, lite3cpp::Buffer& dst, size_t dst_arr_ofs) {
    lite3cpp::NodeView node(reinterpret_cast<const lite3cpp::PackedNodeLayout*>(src.data() + src_arr_ofs));
    for (uint32_t i = 0; i < node.size(); ++i) {
        lite3cpp::Type val_type = src.arr_get_type(src_arr_ofs, i);
        switch (val_type) {
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
                size_t child_src_obj = src.arr_get_obj(src_arr_ofs, i);
                size_t child_dst_obj = dst.arr_append_obj(dst_arr_ofs);
                copy_properties(src, child_src_obj, dst, child_dst_obj);
                break;
            }
            case lite3cpp::Type::Array: {
                size_t child_src_arr = src.arr_get_arr(src_arr_ofs, i);
                size_t child_dst_arr = dst.arr_append_arr(dst_arr_ofs);
                copy_array(src, child_src_arr, dst, child_dst_arr);
                break;
            }
            default:
                break;
        }
    }
}

} // anonymous namespace

EdgeCoordinator::EdgeCoordinator(l3kv::Engine* store, FederationResolver& resolver, RemoteL3KVClient& remote_client, uint32_t node_id, std::shared_ptr<ThreadPool> pool, const Settings& settings, 
                                 std::function<void(const std::string&, const std::string&)> replication_cb)
    : store_(store), resolver_(resolver), remote_client_(remote_client), hlc_(node_id), 
      num_shards_(settings.edge_write_shards), 
      edge_flush_interval_ms_(settings.edge_flush_interval_ms),
      task_pool_(std::move(pool)),
      replication_cb_(std::move(replication_cb)) {
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
    
    lite3cpp::Buffer buf;
    buf.init_object();
    ts.write_to_buffer(buf, 0, "ts");
    if (!payload.empty()) {
        const uint8_t* ptr = reinterpret_cast<const uint8_t*>(payload.data());
        if (payload.size() >= sizeof(lite3cpp::PackedNodeLayout) && (ptr[0] == 0x06 || ptr[0] == 0x07)) {
            try {
                lite3cpp::Buffer props_buf(ptr, payload.size());
                size_t props_ofs = buf.set_obj(0, "props");
                copy_properties(props_buf, 0, buf, props_ofs);
            } catch (...) {
                buf.set_str(0, "props", payload);
            }
        } else if (payload.front() == '{') {
            try {
                lite3cpp::Buffer props_buf = lite3cpp::lite3_json::from_json_string(payload);
                size_t props_ofs = buf.set_obj(0, "props");
                copy_properties(props_buf, 0, buf, props_ofs);
            } catch (...) {
                buf.set_str(0, "props", payload);
            }
        } else {
            buf.set_str(0, "props", payload);
        }
    }
    std::string binary_str(reinterpret_cast<const char*>(buf.data()), buf.size());

    lite3::NodeID src_owner = resolver_.get_node_owner(src_id);
    lite3::NodeID dst_owner = resolver_.get_node_owner(dst_id);
    lite3::NodeID local_id = resolver_.get_local_node_id();

    std::string out_key = std::string(KeyBuilder::edge_out_key(src_id, label, weight, dst_id));
    std::string in_key = std::string(KeyBuilder::edge_in_key(dst_id, label, src_id));

    if(0) std::fprintf(stderr, "[EdgeCoordinator] atomic_del_edge: out_key=[%s] in_key=[%s]\n", out_key.c_str(), in_key.c_str());

    std::vector<std::future<void>> futures;

    auto handle_write = [&](lite3::NodeID owner, const std::string& key) {
        static const bool s_l3_debug = (std::getenv("L3_DEBUG") != nullptr);
        if (owner == local_id) {
            size_t shard_idx = store_->get_routing_shard(key);
            // Engine::put takes a string, we'll cast the data
            
            if (s_l3_debug) {
                std::fprintf(stderr, "[EdgeCoordinator] Local Write: key=%s shard=%zu data_len=%zu\n", key.c_str(), shard_idx, binary_str.size());
                std::fflush(stderr);
            }

            if (replication_cb_) {
                replication_cb_(key, binary_str);
            }

            store_->put(key, binary_str);
            auto prom = std::make_shared<std::promise<void>>();
            prom->set_value();
            futures.push_back(prom->get_future());
        } else {
            if (s_l3_debug) {
                std::fprintf(stderr, "[EdgeCoordinator] Remote Write: key=%s owner=%u (local=%u)\n", key.c_str(), (uint32_t)owner, (uint32_t)local_id);
                std::fflush(stderr);
            }
            auto prom = std::make_shared<std::promise<void>>();
            futures.push_back(prom->get_future());
            
            size_t shard_idx = owner % num_shards_;
            auto& shard = shards_[shard_idx];
            {
                std::lock_guard<std::mutex> lock(shard.mu);
                shard.buffer.push_back({key, binary_str});
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

    if(0) std::fprintf(stderr, "[EdgeCoordinator] atomic_del_edge: out_key=[%s] in_key=[%s]\n", out_key.c_str(), in_key.c_str());

    std::vector<std::future<void>> futures;

    auto handle_del = [&](lite3::NodeID owner, const std::string& key) {
        if (owner == local_id) {
            size_t shard_idx = store_->get_routing_shard(key);
            if (replication_cb_) {
                replication_cb_(key, "");
            }
            store_->del(key);
            auto prom = std::make_shared<std::promise<void>>();
            prom->set_value();
            futures.push_back(prom->get_future());
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
    try {
        std::vector<BatchEntry> to_flush;
        std::vector<std::shared_ptr<std::promise<void>>> promises;
        
        auto& shard = shards_[shard_idx];
        {
            std::lock_guard<std::mutex> lock(shard.mu);
            if (shard.buffer.empty()) return;
            to_flush.swap(shard.buffer);
            promises.swap(shard.promises);
        }

        std::unordered_map<lite3::NodeID, MutationBatch> node_batches;
        std::unordered_map<lite3::NodeID, std::vector<std::shared_ptr<std::promise<void>>>> node_promises;

        for (size_t i = 0; i < to_flush.size(); ++i) {
            auto const& entry = to_flush[i];
            size_t start = entry.key.find('{');
            size_t end = (start != std::string::npos) ? entry.key.find('}', start) : std::string::npos;
            if (start == std::string::npos || end == std::string::npos || end <= start + 1) {
                try { promises[i]->set_exception(std::make_exception_ptr(std::runtime_error("Invalid edge key format"))); } catch (...) {}
                continue;
            }
            std::string id_str = entry.key.substr(start + 1, end - start - 1);
            uint64_t node_id = 0;
            try { node_id = std::stoull(id_str, nullptr, 16); } catch (...) {
                try { promises[i]->set_exception(std::make_exception_ptr(std::runtime_error("Invalid node id in key"))); } catch (...) {}
                continue;
            }
            lite3::NodeID owner = resolver_.get_node_owner(node_id);

            node_batches[owner].put_raw(entry.key, entry.val);
            node_promises[owner].push_back(promises[i]);
        }

        std::vector<std::pair<std::future<bool>, std::vector<std::shared_ptr<std::promise<void>>>>> pending_batches;
        for (auto& [owner, batch] : node_batches) {
            auto fut = remote_client_.execute_batch_async(owner, batch);
            pending_batches.emplace_back(std::move(fut), std::move(node_promises[owner]));
        }

        for (auto& [fut, p_list] : pending_batches) {
            std::exception_ptr ep = nullptr;
            try {
                if (!fut.get()) {
                    ep = std::make_exception_ptr(std::runtime_error("Remote batch mutation failed"));
                }
            } catch (...) {
                ep = std::current_exception();
            }
            for (auto& p : p_list) {
                try {
                    if (ep) p->set_exception(ep);
                    else p->set_value();
                } catch (...) {}
            }
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "[EdgeCoordinator::flush_shard] Uncaught exception: %s\n", e.what());
    } catch (...) {
        std::fprintf(stderr, "[EdgeCoordinator::flush_shard] Unknown uncaught exception\n");
    }
}

} // namespace l3kvg
