#pragma once

#include <string>
#include <vector>
#include <future>
#include <unordered_map>
#include <lite3/ring.hpp>

namespace l3kvg {

class RemoteL3KVClient {
public:
    RemoteL3KVClient();

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

    std::future<bool> put_node_async(
        lite3::NodeID owner_id,
        const std::string& target_node_id, 
        const std::string& json_payload
    );

private:
    std::unordered_map<lite3::NodeID, std::string> peer_endpoints_;
};

} // namespace l3kvg
