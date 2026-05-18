#include "L3KVG/FederationResolver.hpp"
#include "L3KVG/FederationID.hpp"
#include "L3KVG/KeyBuilder.hpp"
#include <mutex>
#include <stdexcept>
#include <xxhash.h>
#include <cstdio>

namespace l3kvg {

FederationResolver::FederationResolver(std::shared_ptr<lite3::ConsistentHash> ring, lite3::NodeID local_node_id)
    : ring_(std::move(ring)), local_node_id_(local_node_id) {}

lite3::NodeID FederationResolver::get_node_owner(uint64_t vertex_id) const {
  std::shared_lock lock(mutex_);
  return get_node_owner_impl(vertex_id);
}

bool FederationResolver::is_local(uint64_t vertex_id) const {
  std::shared_lock lock(mutex_);
  return get_node_owner_impl(vertex_id) == local_node_id_;
}

lite3::NodeID FederationResolver::get_node_owner_impl(uint64_t vertex_id) const noexcept {
  if (!ring_ || ring_->size() == 0) {
    return local_node_id_;
  }
  return ring_->get_node(KeyBuilder::node_key(vertex_id));
}

void FederationResolver::register_local_cluster(std::string name, uint16_t id) {
    std::unique_lock lock(mutex_);
    local_cluster_id_ = id;
    cluster_name_to_id_[name] = id;
    federation_map_[id] = {std::move(name), nullptr};
}

void FederationResolver::register_federation(std::string name, uint16_t id, std::vector<std::string> endpoints) {
    std::unique_lock lock(mutex_);
    cluster_name_to_id_[name] = id;
    federation_map_[id] = {std::move(name), std::make_shared<const std::vector<std::string>>(std::move(endpoints))};
}

bool FederationResolver::is_local_cluster(uint16_t cluster_id) const noexcept {
    std::shared_lock lock(mutex_);
    return cluster_id == local_cluster_id_;
}

std::vector<uint16_t> FederationResolver::get_remote_cluster_ids() const {
    std::shared_lock lock(mutex_);
    std::vector<uint16_t> ids;
    for (auto& [id, info] : federation_map_) {
        if (id != local_cluster_id_) {
            ids.push_back(id);
        }
    }
    return ids;
}

std::shared_ptr<const std::vector<std::string>> FederationResolver::get_federation_endpoints(uint16_t cluster_id) const noexcept {
    std::shared_lock lock(mutex_);
    auto it = federation_map_.find(cluster_id);
    if (it != federation_map_.end()) {
        return it->second.endpoints;
    }
    return nullptr;
}

uint64_t FederationResolver::parse_uuid(std::string_view uuid_str) const {
    std::string_view cluster_name;
    std::string_view local_uuid;

    auto colon_pos = uuid_str.find(':');
    uint16_t cluster_id;

    if (colon_pos != std::string_view::npos) {
        cluster_name = uuid_str.substr(0, colon_pos);
        local_uuid = uuid_str.substr(colon_pos + 1);

        std::shared_lock lock(mutex_);
        auto it = cluster_name_to_id_.find(std::string(cluster_name));
        if (it == cluster_name_to_id_.end()) {
            throw std::runtime_error("Unknown cluster name: " + std::string(cluster_name));
        }
        cluster_id = it->second;
    } else {
        local_uuid = uuid_str;
        std::shared_lock lock(mutex_);
        cluster_id = local_cluster_id_;
    }

    uint64_t hash = XXH3_64bits(local_uuid.data(), local_uuid.size());
    return FederationID::pack(cluster_id, hash);
}

} // namespace l3kvg
