#pragma once

#include <memory>
#include <string>
#include <lite3/ring.hpp>

namespace l3kvg {

class ClusterResolver {
public:
  ClusterResolver(std::shared_ptr<lite3::ConsistentHash> ring, lite3::NodeID local_node_id);

  // Maps a Vertex UUID to a specific physical Peer ID
  lite3::NodeID get_node_owner(const std::string& vertex_id) const;

  // Identifies if the data is local or requires an RPC
  bool is_local(const std::string& vertex_id) const;

  // Gets the local node ID
  lite3::NodeID get_local_node_id() const { return local_node_id_; }

private:
  std::shared_ptr<lite3::ConsistentHash> ring_;
  lite3::NodeID local_node_id_;
};

} // namespace l3kvg
