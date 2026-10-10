#include "L3KVG/AdjacencyStore.hpp"

namespace l3kvg {

AdjacencyStore::AdjacencyStore() = default;

std::shared_ptr<AdjacencyStore::NodeAdjacency> AdjacencyStore::get_or_create_node(uint64_t id) {
    auto& shard = shards_[shard_idx(id)];
    {
        std::shared_lock read_lock(shard.mutex);
        if (auto it = shard.nodes.find(id); it != shard.nodes.end()) {
            return it->second;
        }
    }
    std::unique_lock write_lock(shard.mutex);
    if (auto it = shard.nodes.find(id); it != shard.nodes.end()) {
        return it->second;
    }
    auto entry = std::make_shared<NodeAdjacency>();
    shard.nodes[id] = entry;
    return entry;
}

std::shared_ptr<AdjacencyStore::NodeAdjacency> AdjacencyStore::get_node(uint64_t id) const noexcept {
    const auto& shard = shards_[shard_idx(id)];
    std::shared_lock read_lock(shard.mutex);
    if (auto it = shard.nodes.find(id); it != shard.nodes.end()) {
        return it->second;
    }
    return nullptr;
}

void AdjacencyStore::add_edge(uint64_t src_id, uint64_t dst_id, uint16_t label_id,
                              float weight, uint16_t flags,
                              uint32_t prop_index, uint32_t hlc) {
    PackedEdge edge;
    edge.target_id = dst_id;
    edge.label_id = label_id;
    edge.flags = flags | PackedEdge::FLAG_ACTIVE;
    edge.weight = weight;
    edge.prop_index = prop_index;
    edge.hlc_logical = hlc;

    // 1. Update Outgoing Edge for src_id
    {
        auto src_node = get_or_create_node(src_id);
        std::unique_lock lock(src_node->mutex);
        if (src_node->out_record.out_degree == 0) {
            src_node->out_record.inline_edge = edge;
            src_node->out_record.out_degree = 1;
        } else if (src_node->out_record.out_degree == 1) {
            src_node->out_edges.reserve(8);
            src_node->out_edges.push_back(src_node->out_record.inline_edge);
            src_node->out_edges.push_back(edge);
            src_node->out_record.out_degree = 2;
        } else {
            src_node->out_edges.push_back(edge);
            src_node->out_record.out_degree = static_cast<uint32_t>(src_node->out_edges.size());
        }
    }

    // 2. Update Incoming Edge for dst_id
    {
        PackedEdge in_edge = edge;
        in_edge.target_id = src_id; // For incoming edge, target is the source
        auto dst_node = get_or_create_node(dst_id);
        std::unique_lock lock(dst_node->mutex);
        if (dst_node->in_record.in_degree == 0) {
            dst_node->in_record.inline_edge = in_edge;
            dst_node->in_record.in_degree = 1;
        } else if (dst_node->in_record.in_degree == 1) {
            dst_node->in_edges.reserve(8);
            dst_node->in_edges.push_back(dst_node->in_record.inline_edge);
            dst_node->in_edges.push_back(in_edge);
            dst_node->in_record.in_degree = 2;
        } else {
            dst_node->in_edges.push_back(in_edge);
            dst_node->in_record.in_degree = static_cast<uint32_t>(dst_node->in_edges.size());
        }
    }
}

bool AdjacencyStore::del_edge(uint64_t src_id, uint64_t dst_id, uint16_t label_id) {
    bool found = false;

    // 1. Tombstone in outgoing
    if (auto src_node = get_node(src_id)) {
        std::unique_lock lock(src_node->mutex);
        if (src_node->out_record.out_degree == 1) {
            if (src_node->out_record.inline_edge.target_id == dst_id &&
                src_node->out_record.inline_edge.label_id == label_id &&
                src_node->out_record.inline_edge.is_active()) {
                src_node->out_record.inline_edge.set_active(false);
                found = true;
            }
        } else {
            for (auto& e : src_node->out_edges) {
                if (e.target_id == dst_id && e.label_id == label_id && e.is_active()) {
                    e.set_active(false);
                    found = true;
                    break;
                }
            }
        }
    }

    // 2. Tombstone in incoming
    if (auto dst_node = get_node(dst_id)) {
        std::unique_lock lock(dst_node->mutex);
        if (dst_node->in_record.in_degree == 1) {
            if (dst_node->in_record.inline_edge.target_id == src_id &&
                dst_node->in_record.inline_edge.label_id == label_id &&
                dst_node->in_record.inline_edge.is_active()) {
                dst_node->in_record.inline_edge.set_active(false);
            }
        } else {
            for (auto& e : dst_node->in_edges) {
                if (e.target_id == src_id && e.label_id == label_id && e.is_active()) {
                    e.set_active(false);
                    break;
                }
            }
        }
    }

    return found;
}

std::span<const PackedEdge> AdjacencyStore::get_outgoing_edges(uint64_t src_id) const noexcept {
    auto node = get_node(src_id);
    if (!node) return {};
    std::shared_lock lock(node->mutex);
    if (node->out_record.out_degree == 0) {
        return {};
    }
    if (node->out_record.out_degree == 1) {
        return std::span<const PackedEdge>(&node->out_record.inline_edge, 1);
    }
    return std::span<const PackedEdge>(node->out_edges.data(), node->out_edges.size());
}

std::span<const PackedEdge> AdjacencyStore::get_incoming_edges(uint64_t dst_id) const noexcept {
    auto node = get_node(dst_id);
    if (!node) return {};
    std::shared_lock lock(node->mutex);
    if (node->in_record.in_degree == 0) {
        return {};
    }
    if (node->in_record.in_degree == 1) {
        return std::span<const PackedEdge>(&node->in_record.inline_edge, 1);
    }
    return std::span<const PackedEdge>(node->in_edges.data(), node->in_edges.size());
}

size_t AdjacencyStore::get_out_degree(uint64_t src_id) const noexcept {
    auto node = get_node(src_id);
    if (!node) return 0;
    std::shared_lock lock(node->mutex);
    return node->out_record.out_degree;
}

size_t AdjacencyStore::get_in_degree(uint64_t dst_id) const noexcept {
    auto node = get_node(dst_id);
    if (!node) return 0;
    std::shared_lock lock(node->mutex);
    return node->in_record.in_degree;
}

} // namespace l3kvg
