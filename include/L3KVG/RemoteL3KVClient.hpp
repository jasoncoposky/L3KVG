#pragma once

#include <string>
#include <vector>
#include <future>
#include <unordered_map>
#include <mutex>
#include <memory>
#include <zmq.hpp>
#include <lite3/ring.hpp>
#include <cstdint>
#include "buffer.hpp"
#include "L3KVG/QueryResult.hpp"
#include "L3KVG/ThreadPool.hpp"
#include "L3KVG/Settings.hpp"


namespace l3kvg {

class RemoteL3KVClient {
public:
    RemoteL3KVClient(const Settings& settings = {});
    virtual ~RemoteL3KVClient();

    void set_thread_pool(std::shared_ptr<ThreadPool> pool) { task_pool_ = std::move(pool); }

    void add_peer(lite3::NodeID node_id, const std::string& endpoint_url);

    std::future<std::vector<uint64_t>> get_neighbors_async(
        lite3::NodeID owner_id,
        uint64_t target_node_id, 
        const std::string& label,
        double min_weight
    );

    virtual std::future<std::vector<ResultRow>> resume_query_async(
        uint16_t cluster_id,
        const std::vector<uint64_t>& starting_nodes,
        const std::string& query_json
    );

    std::future<bool> put_edge_async(
        lite3::NodeID owner_id,
        const std::string& edge_key, 
        const std::string& json_payload
    );

    std::future<std::string> get_node_payload_async(
        lite3::NodeID owner_id,
        uint64_t target_node_id
    );

    // Batch Fetch: Reduces network roundtrips by coalescing requests to the same peer.
    std::future<std::unordered_map<uint64_t, std::string>> get_nodes_batch_async(
        lite3::NodeID owner_id,
        const std::vector<uint64_t>& node_ids
    );

    std::future<bool> put_node_async(
        lite3::NodeID owner_id,
        uint64_t target_node_id, 
        const std::string& json_payload
    );
    
    std::future<bool> put_batch_async(
        lite3::NodeID owner_id,
        const std::unordered_map<uint64_t, std::string>& batch
    );

    std::future<bool> put_batch_binary_async(
        lite3::NodeID owner_id,
        const lite3cpp::Buffer& batch_buffer
    );

private:
    struct Session {
        std::mutex mu;
        std::unique_ptr<zmq::socket_t> socket;
        bool connected = false;
    };

    std::shared_ptr<Session> get_session(lite3::NodeID node_id);

    std::unordered_map<lite3::NodeID, std::string> peer_endpoints_;
    std::unordered_map<lite3::NodeID, std::shared_ptr<Session>> peer_sessions_;
    std::mutex endpoints_mutex_;
    std::shared_ptr<ThreadPool> task_pool_;
    
    int zmq_sndhwm_;
    zmq::context_t zmq_ctx_;
};


} // namespace l3kvg
