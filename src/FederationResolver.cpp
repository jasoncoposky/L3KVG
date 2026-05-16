#include "L3KVG/FederationResolver.hpp"

namespace l3kvg {

FederationResolver::FederationResolver(std::shared_ptr<lite3::ConsistentHash> ring, lite3::NodeID local_node_id)
    : ring_(std::move(ring)), local_node_id_(local_node_id) {}

lite3::NodeID FederationResolver::get_node_owner(const std::string& vertex_id) const {
  if (!ring_ || ring_->size() == 0) {
    return local_node_id_;
  }
  return ring_->get_node(vertex_id);
}

bool FederationResolver::is_local(const std::string& vertex_id) const {
  return get_node_owner(vertex_id) == local_node_id_;
}

void FederationResolver::register_local_cluster(std::string name, uint16_t id) {
    local_cluster_id_ = id;
    federation_map_[id] = {std::move(name), {}};
}

void FederationResolver::register_federation(std::string name, uint16_t id, std::vector<std::string> endpoints) {
    federation_map_[id] = {std::move(name), std::move(endpoints)};
}

bool FederationResolver::is_local_cluster(uint16_t cluster_id) const {
    return cluster_id == local_cluster_id_;
}

std::vector<std::string> FederationResolver::get_federation_endpoints(uint16_t cluster_id) const {
    auto it = federation_map_.find(cluster_id);
    if (it != federation_map_.end()) {
        return it->second.endpoints;
    }
    return {};
}

} // namespace l3kvg
