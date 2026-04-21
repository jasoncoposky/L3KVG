#pragma once

#include <string>
#include <vector>
#include <future>
#include <unordered_map>
#include <mutex>
#include <memory>
#include <zmq.hpp>
#include <lite3/ring.hpp>
#include "buffer.hpp"


namespace l3kvg {

class RemoteL3KVClient {
public:
    RemoteL3KVClient();
    ~RemoteL3KVClient();

    void add_peer(lite3::NodeID node_id, const std::string& endpoint_url);

    std::future<std::vector<std::string>> get_neighbors_async(
        lite3::NodeID owner_id,
        const std::string& target_node_id, 
        const std::string& label,
        double min_weight
    );

    std::future<bool> put_edge_async(
        lite3::NodeID owner_id,
        const std::string& edge_key, 
        const std::string& json_payload
    );

    std::future<std::string> get_node_payload_async(
        lite3::NodeID owner_id,
        const std::string& target_node_id
    );

    // Batch Fetch: Reduces network roundtrips by coalescing requests to the same peer.
    std::future<std::unordered_map<std::string, std::string>> get_nodes_batch_async(
        lite3::NodeID owner_id,
        const std::vector<std::string>& node_uuids
    );

    std::future<bool> put_node_async(
        lite3::NodeID owner_id,
        const std::string& target_node_id, 
        const std::string& json_payload
    );
    
    std::future<bool> put_batch_async(
        lite3::NodeID owner_id,
        const std::unordered_map<std::string, std::string>& batch
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
    
    zmq::context_t zmq_ctx_;
};


} // namespace l3kvg
