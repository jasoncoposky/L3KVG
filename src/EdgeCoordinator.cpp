#include "L3KVG/EdgeCoordinator.hpp"
#include "engine/store.hpp"
#include "L3KVG/Engine.hpp" 
#include "L3KVG/KeyBuilder.hpp"

namespace l3kvg {

EdgeCoordinator::EdgeCoordinator(l3kv::Engine* store, ClusterResolver& resolver, RemoteL3KVClient& remote_client, uint32_t node_id)
    : store_(store), resolver_(resolver), remote_client_(remote_client), hlc_(node_id) {}

void EdgeCoordinator::atomic_put_edge(const std::string& src_uuid, const std::string& label, double weight, const std::string& dst_uuid, const std::string& payload) {
    auto ts = hlc_.now();
    char payload_buf[2048];
    int p_len;
    if (payload.empty()) {
        p_len = std::snprintf(payload_buf, sizeof(payload_buf), 
                              "{\"ts\": {\"wall_time\": %llu, \"logical\": %u, \"node_id\": %u}}",
                              ts.wall_time, ts.logical, ts.node_id);
    } else {
        p_len = std::snprintf(payload_buf, sizeof(payload_buf), 
                              "{\"ts\": {\"wall_time\": %llu, \"logical\": %u, \"node_id\": %u}, \"props\": %.*s}",
                              ts.wall_time, ts.logical, ts.node_id, 
                              static_cast<int>(payload.size()), payload.data());
    }
    std::string final_payload(payload_buf, p_len);

    std::string_view out_key = KeyBuilder::edge_out_key(src_uuid, label, weight, dst_uuid);
    std::string_view in_key = KeyBuilder::edge_in_key(dst_uuid, label, src_uuid);

    lite3::NodeID src_owner = resolver_.get_node_owner(src_uuid);
    lite3::NodeID dst_owner = resolver_.get_node_owner(dst_uuid);
    lite3::NodeID local_id = resolver_.get_local_node_id();

    if (src_owner == local_id) {
        store_->put(std::string(out_key), final_payload);
    } else {
        remote_client_.put_edge_async(src_owner, std::string(out_key), final_payload).get();
    }

    if (dst_owner == local_id) {
        store_->put(std::string(in_key), final_payload);
    } else {
        remote_client_.put_edge_async(dst_owner, std::string(in_key), final_payload).get();
    }
}

} // namespace l3kvg
