#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <vector>
#include <unordered_map>
#include <shared_mutex>
#include <lite3/ring.hpp>
#include <cstdint>

namespace l3kvg {

class FederationResolver {
public:
  FederationResolver(std::shared_ptr<lite3::ConsistentHash> ring, lite3::NodeID local_node_id);

  // Maps a Vertex ID to a specific physical Peer ID
  [[nodiscard]] lite3::NodeID get_node_owner(uint64_t vertex_id) const;
  [[nodiscard]] lite3::NodeID get_local_shard_owner(uint64_t vertex_id) const;

  // Identifies if the data is local or requires an RPC
  [[nodiscard]] bool is_local(uint64_t vertex_id) const;

  // Gets the local node ID
  [[nodiscard]] lite3::NodeID get_local_node_id() const noexcept { return local_node_id_; }

  // Federation registration
  void register_local_cluster(std::string name, uint16_t id);
  void register_federation(std::string name, uint16_t id, std::vector<std::string> endpoints);
  
  [[nodiscard]] bool is_local_cluster(uint16_t cluster_id) const noexcept;
  [[nodiscard]] uint16_t get_local_cluster_id() const noexcept { return local_cluster_id_; }
  [[nodiscard]] std::vector<uint16_t> get_remote_cluster_ids() const;
  [[nodiscard]] std::vector<lite3::NodeID> get_all_node_ids() const;
  [[nodiscard]] std::shared_ptr<const std::vector<std::string>> get_federation_endpoints(uint16_t cluster_id) const noexcept;

  // Edge-Hashed String Mapping
  [[nodiscard]] uint64_t parse_uuid(std::string_view uuid_str) const;

private:
  struct ClusterInfo {
    std::string name;
    std::shared_ptr<const std::vector<std::string>> endpoints;
  };

  [[nodiscard]] lite3::NodeID get_node_owner_impl(uint64_t vertex_id) const noexcept;

  mutable std::shared_mutex mutex_;
  std::shared_ptr<lite3::ConsistentHash> ring_;
  lite3::NodeID local_node_id_;
  uint16_t local_cluster_id_ = 0;
  std::unordered_map<uint16_t, ClusterInfo> federation_map_;
  std::unordered_map<std::string, uint16_t> cluster_name_to_id_;
};

} // namespace l3kvg
