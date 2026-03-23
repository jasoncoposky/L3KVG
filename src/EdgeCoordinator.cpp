#include "L3KVG/EdgeCoordinator.hpp"
#include "engine/store.hpp"
#include "L3KVG/Engine.hpp" 
#include <iostream>

namespace l3kvg {

EdgeCoordinator::EdgeCoordinator(l3kv::Engine* store, ClusterResolver& resolver, RemoteL3KVClient& remote_client, uint32_t node_id)
    : store_(store), resolver_(resolver), remote_client_(remote_client), hlc_(node_id) {}

void EdgeCoordinator::atomic_put_edge(const std::string& src_uuid, const std::string& label, double weight, const std::string& dst_uuid) {
    auto ts = hlc_.now();
    std::string ts_json = "{\"ts\": " + ts.to_json_string() + "}";

    std::string w_str = Engine::format_weight(weight);

    std::string out_key = "e:out:{" + src_uuid + "}:" + label + ":" + w_str + ":" + dst_uuid;
    std::string in_key = "e:in:{" + dst_uuid + "}:" + label + ":" + src_uuid;

    lite3::NodeID src_owner = resolver_.get_node_owner(src_uuid);
    lite3::NodeID dst_owner = resolver_.get_node_owner(dst_uuid);
    lite3::NodeID local_id = resolver_.get_local_node_id();

    std::future<bool> out_future;
    if (src_owner == local_id) {
        out_future = std::async(std::launch::deferred, [this, out_key, ts_json]() {
            store_->put(out_key, ts_json);
            return true;
        });
    } else {
        out_future = remote_client_.put_edge_async(src_owner, out_key, ts_json);
    }

    std::future<bool> in_future;
    if (dst_owner == local_id) {
        in_future = std::async(std::launch::deferred, [this, in_key, ts_json]() {
            store_->put(in_key, ts_json);
            return true;
        });
    } else {
        in_future = remote_client_.put_edge_async(dst_owner, in_key, ts_json);
    }

    try {
        out_future.get();
    } catch(const std::exception& e) {
        std::cerr << "[EdgeCoordinator] Pending Sync required for OUT edge on peer " << src_owner << ": " << e.what() << "\n";
    }

    try {
        in_future.get();
    } catch(const std::exception& e) {
        std::cerr << "[EdgeCoordinator] Pending Sync required for IN edge on peer " << dst_owner << ": " << e.what() << "\n";
    }
}

} // namespace l3kvg
