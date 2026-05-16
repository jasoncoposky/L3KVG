#pragma once

#include <memory>
#include <string>
#include <vector>
#include <unordered_map>
#include <lite3/ring.hpp>

namespace l3kvg {

class FederationResolver {
public:
  FederationResolver(std::shared_ptr<lite3::ConsistentHash> ring, lite3::NodeID local_node_id);

  // Maps a Vertex UUID to a specific physical Peer ID
  lite3::NodeID get_node_owner(const std::string& vertex_id) const;

  // Identifies if the data is local or requires an RPC
  bool is_local(const std::string& vertex_id) const;

  // Gets the local node ID
  lite3::NodeID get_local_node_id() const { return local_node_id_; }

  // Federation registration
  void register_local_cluster(std::string name, uint16_t id);
  void register_federation(std::string name, uint16_t id, std::vector<std::string> endpoints);
  
  bool is_local_cluster(uint16_t cluster_id) const;
  std::vector<std::string> get_federation_endpoints(uint16_t cluster_id) const;

private:
  struct ClusterInfo {
    std::string name;
    std::vector<std::string> endpoints;
  };

  std::shared_ptr<lite3::ConsistentHash> ring_;
  lite3::NodeID local_node_id_;
  uint16_t local_cluster_id_ = 0;
  std::unordered_map<uint16_t, ClusterInfo> federation_map_;
};

} // namespace l3kvg
