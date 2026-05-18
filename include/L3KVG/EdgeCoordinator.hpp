#pragma once

#include <string>
#include <chrono>
#include <mutex>
#include <cstdint>
#include <future>
#include <unordered_map>
#include <vector>
#include <thread>
#include <condition_variable>
#include <atomic>
#include <memory>
#include <functional>
#include "L3KVG/FederationResolver.hpp"
#include "L3KVG/RemoteL3KVClient.hpp"
#include "L3KVG/ThreadPool.hpp"
#include "L3KVG/Settings.hpp"
#include "L3KVG/HLC.hpp"
#include "buffer.hpp"

namespace l3kv { class Engine; }

namespace l3kvg {

class EdgeCoordinator {
public:
    EdgeCoordinator(l3kv::Engine* store, FederationResolver& resolver, RemoteL3KVClient& remote_client, uint32_t node_id, std::shared_ptr<ThreadPool> pool, const Settings& settings = {}, 
                    std::function<void(const std::string&, const std::string&)> replication_cb = nullptr);
    ~EdgeCoordinator();

    std::future<void> atomic_put_edge(uint64_t src_id, const std::string& label, double weight, uint64_t dst_id, const std::string& payload = "");
    std::future<void> atomic_del_edge(uint64_t src_id, const std::string& label, double weight, uint64_t dst_id);

private:
    struct BatchEntry {
        std::string key;
        std::vector<uint8_t> val;
    };

    struct BatchShard {
        std::mutex mu;
        std::vector<BatchEntry> buffer;
        std::vector<std::shared_ptr<std::promise<void>>> promises;
    };

    void flush_loop();
    void flush_shard(size_t shard_idx);

    l3kv::Engine* store_;
    FederationResolver& resolver_;
    RemoteL3KVClient& remote_client_;
    HLCProvider hlc_;

    size_t num_shards_;
    int edge_flush_interval_ms_;
    std::unique_ptr<BatchShard[]> shards_;
    
    std::thread flush_thread_;
    std::shared_ptr<ThreadPool> task_pool_;
    std::function<void(const std::string&, const std::string&)> replication_cb_;
    std::atomic<bool> stop_flusher_{false};
    std::condition_variable cv_;
    std::mutex cv_mu_; // Dedicated mutex for the condition variable
};

} // namespace l3kvg
