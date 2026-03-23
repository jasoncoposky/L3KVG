#include "L3KVG/ClusterResolver.hpp"

namespace l3kvg {

ClusterResolver::ClusterResolver(std::shared_ptr<lite3::ConsistentHash> ring, lite3::NodeID local_node_id)
    : ring_(std::move(ring)), local_node_id_(local_node_id) {}

lite3::NodeID ClusterResolver::get_node_owner(const std::string& vertex_id) const {
  if (!ring_ || ring_->size() == 0) {
    return local_node_id_;
  }
  return ring_->get_node(vertex_id);
}

bool ClusterResolver::is_local(const std::string& vertex_id) const {
  return get_node_owner(vertex_id) == local_node_id_;
}

} // namespace l3kvg
