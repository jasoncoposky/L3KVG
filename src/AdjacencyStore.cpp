#include "L3KVG/AdjacencyStore.hpp"
#include "L3KVG/SIMDFilter.hpp"
#include <algorithm>
#include <cmath>

namespace l3kvg {

namespace {

[[nodiscard]] inline bool same_edge_weight(float w1, float w2) noexcept {
    if (w1 == w2) return true;
    if (std::isnan(w1) && std::isnan(w2)) return true;
    return std::abs(w1 - w2) < 1e-5f;
}

inline void update_existing_edge_in_place(PackedEdge& existing, const PackedEdge& incoming) noexcept {
    existing.weight = incoming.weight;
    if (incoming.has_properties()) {
        existing.hlc_timestamp = incoming.hlc_timestamp;
        existing.flags = static_cast<uint16_t>(
            existing.flags | PackedEdge::FLAG_HAS_PROPERTIES |
            (incoming.flags & PackedEdge::FLAG_DIRECTED) |
            PackedEdge::FLAG_ACTIVE);
    } else {
        existing.flags = static_cast<uint16_t>(
            existing.flags | (incoming.flags & PackedEdge::FLAG_DIRECTED) |
            PackedEdge::FLAG_ACTIVE);
    }
}

} // namespace

AdjacencyStore::AdjacencyStore() {
    for (auto& shard : shards_) {
        shard.directory.reserve(4096);
    }
}

bool AdjacencyStore::add_edge(uint64_t src_id, uint64_t dst_id, uint16_t label_id,
                              float weight, uint16_t flags,
                              uint32_t prop_index, uint64_t hlc) {
    (void)prop_index;
    PackedEdge edge;
    edge.target_id = dst_id;
    edge.label_id = label_id;
    edge.flags = flags | PackedEdge::FLAG_ACTIVE;
    edge.weight = weight;
    edge.hlc_timestamp = hlc;

    size_t s_src = shard_index(src_id);
    size_t s_dst = shard_index(dst_id);

    auto update_src = [s_src, src_id, &edge](Shard& shard) -> bool {
        VertexRecord& v = shard.directory[src_id];
        if (!v.is_promoted() && (v.out_degree & VertexRecord::DEGREE_MASK) == 0 && v.in_degree == 0) {
            // Zero-allocation inline
            v.inline_edge = edge;
            v.out_degree = 1;
            return true;
        } else if (v.is_inlined()) {
            if (v.inline_edge.is_active() &&
                v.inline_edge.target_id == edge.target_id &&
                v.inline_edge.label_id == edge.label_id &&
                same_edge_weight(v.inline_edge.weight, edge.weight)) {
                update_existing_edge_in_place(v.inline_edge, edge);
                return false;
            }
            // Promotion from inlined edge to slotted page
            PackedEdge old_inline = v.inline_edge;
            uint32_t local_id = shard.page_pool.allocate_page();
            auto& page = shard.page_pool[local_id];
            page.edges[0] = old_inline;
            page.edges[1] = edge;
            page.count = 2;
            page.next_page_id = 0;

            uint32_t page_id = encode_page_id(s_src, local_id);
            v.ext.out_block_id = page_id;
            v.ext.in_block_id = 0;
            v.ext.page_offset = 0;
            v.out_degree = 2 | VertexRecord::FLAG_PROMOTED;
            return true;
        } else {
            if (!v.is_promoted()) {
                v.ext.out_block_id = 0;
                v.ext.in_block_id = 0;
                v.ext.page_offset = 0;
                v.out_degree |= VertexRecord::FLAG_PROMOTED;
            }
            if (v.ext.out_block_id == 0) {
                uint32_t local_id = shard.page_pool.allocate_page();
                auto& page = shard.page_pool[local_id];
                page.edges[0] = edge;
                page.count = 1;
                page.next_page_id = 0;

                uint32_t page_id = encode_page_id(s_src, local_id);
                v.ext.out_block_id = page_id;
                v.out_degree = 1 | VertexRecord::FLAG_PROMOTED;
                return true;
            } else {
                PackedEdge* first_tombstone = nullptr;
                uint32_t curr_id = v.ext.out_block_id;
                uint32_t last_local_id = decode_local_page_id(curr_id);
                while (curr_id != 0) {
                    uint32_t p_shard = decode_shard_index(curr_id);
                    uint32_t local_id = decode_local_page_id(curr_id);
                    if (p_shard != s_src || local_id >= shard.page_pool.size()) break;
                    last_local_id = local_id;
                    auto& page = shard.page_pool[local_id];
                    for (uint32_t i = 0; i < page.count; ++i) {
                        if (page.edges[i].is_active()) {
                            if (page.edges[i].target_id == edge.target_id &&
                                page.edges[i].label_id == edge.label_id &&
                                same_edge_weight(page.edges[i].weight, edge.weight)) {
                                update_existing_edge_in_place(page.edges[i], edge);
                                return false;
                            }
                        } else if (first_tombstone == nullptr) {
                            first_tombstone = &page.edges[i];
                        }
                    }
                    if (page.next_page_id == 0 || page.next_page_id == curr_id) {
                        break;
                    }
                    curr_id = page.next_page_id;
                }
                if (first_tombstone != nullptr) {
                    *first_tombstone = edge;
                } else {
                    auto* curr_page = &shard.page_pool[last_local_id];
                    if (curr_page->count < AdjacencyPage::CAPACITY) {
                        curr_page->edges[curr_page->count++] = edge;
                    } else {
                        uint32_t new_local_id = shard.page_pool.allocate_page();
                        uint32_t new_page_id = encode_page_id(s_src, new_local_id);
                        shard.page_pool[last_local_id].next_page_id = new_page_id;
                        auto& new_page = shard.page_pool[new_local_id];
                        new_page.edges[0] = edge;
                        new_page.count = 1;
                        new_page.next_page_id = 0;
                    }
                }
                v.out_degree = ((v.out_degree & VertexRecord::DEGREE_MASK) + 1) | VertexRecord::FLAG_PROMOTED;
                return true;
            }
        }
    };

    auto update_dst = [s_dst, dst_id, src_id, &edge](Shard& shard) {
        PackedEdge in_edge = edge;
        in_edge.target_id = src_id; // For incoming edge, target is source

        VertexRecord& v = shard.directory[dst_id];
        // If dst had an inlined outgoing edge, promote it before ext is written
        if (v.is_inlined()) {
            PackedEdge old_inline = v.inline_edge;
            uint32_t out_local_id = shard.page_pool.allocate_page();
            auto& out_page = shard.page_pool[out_local_id];
            out_page.edges[0] = old_inline;
            out_page.count = 1;
            out_page.next_page_id = 0;

            uint32_t out_pid = encode_page_id(s_dst, out_local_id);
            v.ext.out_block_id = out_pid;
            v.ext.in_block_id = 0;
            v.ext.page_offset = 0;
            v.out_degree = (v.out_degree & VertexRecord::DEGREE_MASK) | VertexRecord::FLAG_PROMOTED;
        } else {
            if (!v.is_promoted()) {
                v.ext.out_block_id = 0;
                v.ext.in_block_id = 0;
                v.ext.page_offset = 0;
            }
            v.out_degree |= VertexRecord::FLAG_PROMOTED;
        }

        if (v.ext.in_block_id == 0) {
            uint32_t in_local_id = shard.page_pool.allocate_page();
            auto& in_page = shard.page_pool[in_local_id];
            in_page.edges[0] = in_edge;
            in_page.count = 1;
            in_page.next_page_id = 0;

            uint32_t in_pid = encode_page_id(s_dst, in_local_id);
            v.ext.in_block_id = in_pid;
            v.in_degree = 1;
        } else {
            PackedEdge* first_tombstone = nullptr;
            uint32_t curr_id = v.ext.in_block_id;
            uint32_t last_local_id = decode_local_page_id(curr_id);
            while (curr_id != 0) {
                uint32_t p_shard = decode_shard_index(curr_id);
                uint32_t local_id = decode_local_page_id(curr_id);
                if (p_shard != s_dst || local_id >= shard.page_pool.size()) break;
                last_local_id = local_id;
                auto& page = shard.page_pool[local_id];
                for (uint32_t i = 0; i < page.count; ++i) {
                    if (page.edges[i].is_active()) {
                        if (page.edges[i].target_id == in_edge.target_id &&
                            page.edges[i].label_id == in_edge.label_id &&
                            same_edge_weight(page.edges[i].weight, in_edge.weight)) {
                            update_existing_edge_in_place(page.edges[i], in_edge);
                            return;
                        }
                    } else if (first_tombstone == nullptr) {
                        first_tombstone = &page.edges[i];
                    }
                }
                if (page.next_page_id == 0 || page.next_page_id == curr_id) {
                    break;
                }
                curr_id = page.next_page_id;
            }
            if (first_tombstone != nullptr) {
                *first_tombstone = in_edge;
            } else {
                auto* curr_page = &shard.page_pool[last_local_id];
                if (curr_page->count < AdjacencyPage::CAPACITY) {
                    curr_page->edges[curr_page->count++] = in_edge;
                } else {
                    uint32_t new_local_id = shard.page_pool.allocate_page();
                    uint32_t new_page_id = encode_page_id(s_dst, new_local_id);
                    shard.page_pool[last_local_id].next_page_id = new_page_id;
                    auto& new_page = shard.page_pool[new_local_id];
                    new_page.edges[0] = in_edge;
                    new_page.count = 1;
                    new_page.next_page_id = 0;
                }
            }
            v.in_degree++;
        }
    };

    bool inserted = false;
    if (s_src == s_dst) {
        std::unique_lock lock(shards_[s_src].mutex);
        inserted = update_src(shards_[s_src]);
        update_dst(shards_[s_src]);
    } else {
        size_t first = std::min(s_src, s_dst);
        size_t second = std::max(s_src, s_dst);
        std::unique_lock lock1(shards_[first].mutex);
        std::unique_lock lock2(shards_[second].mutex);
        inserted = update_src(shards_[s_src]);
        update_dst(shards_[s_dst]);
    }
    return inserted;
}

bool AdjacencyStore::del_edge(uint64_t src_id, uint64_t dst_id, uint16_t label_id,
                              float weight,
                              uint16_t* out_removed_flags,
                              float* out_removed_weight) {
    size_t s_src = shard_index(src_id);
    size_t s_dst = shard_index(dst_id);
    bool found = false;
    float matched_weight = weight;

    auto do_del = [&](Shard& shard_src, Shard& shard_dst) {
        // 1. Tombstone in outgoing
        if (auto it = shard_src.directory.find(src_id); it != shard_src.directory.end()) {
            auto& v = it->second;
            if (v.is_inlined()) {
                if (v.inline_edge.target_id == dst_id &&
                    v.inline_edge.label_id == label_id &&
                    (weight < 0.0f || same_edge_weight(v.inline_edge.weight, weight)) &&
                    v.inline_edge.is_active()) {
                    if (out_removed_flags) *out_removed_flags = v.inline_edge.flags;
                    matched_weight = v.inline_edge.weight;
                    if (out_removed_weight) *out_removed_weight = matched_weight;
                    v.inline_edge.set_active(false);
                    found = true;
                    v.out_degree = 0;
                    v.ext.out_block_id = 0;
                    v.ext.in_block_id = 0;
                    v.ext.page_offset = 0;
                }
            } else if (v.is_promoted() && v.ext.out_block_id > 0) {
                uint32_t curr_id = v.ext.out_block_id;
                while (curr_id != 0) {
                    uint32_t p_shard = decode_shard_index(curr_id);
                    uint32_t local_id = decode_local_page_id(curr_id);
                    if (p_shard != s_src || local_id >= shard_src.page_pool.size()) break;
                    auto& page = shard_src.page_pool[local_id];
                    if (page.next_page_id == curr_id) break; // cycle guard
                    for (uint32_t i = 0; i < page.count; ++i) {
                        if (page.edges[i].target_id == dst_id &&
                            page.edges[i].label_id == label_id &&
                            (weight < 0.0f || same_edge_weight(page.edges[i].weight, weight)) &&
                            page.edges[i].is_active()) {
                            if (out_removed_flags) *out_removed_flags = page.edges[i].flags;
                            matched_weight = page.edges[i].weight;
                            if (out_removed_weight) *out_removed_weight = matched_weight;
                            page.edges[i].set_active(false);
                            found = true;
                            uint32_t current_deg = v.out_degree & VertexRecord::DEGREE_MASK;
                            if (current_deg > 0) {
                                v.out_degree = (current_deg - 1) | (v.out_degree & VertexRecord::FLAG_PROMOTED);
                            }
                            break;
                        }
                    }
                    if (found) break;
                    curr_id = page.next_page_id;
                }
            }
        }

        // 2. Tombstone in incoming (only if found in outgoing)
        if (found) {
            if (auto it = shard_dst.directory.find(dst_id); it != shard_dst.directory.end()) {
                auto& v = it->second;
                if (v.is_promoted() && v.ext.in_block_id > 0) {
                    uint32_t curr_id = v.ext.in_block_id;
                    bool in_found = false;
                    while (curr_id != 0) {
                        uint32_t p_shard = decode_shard_index(curr_id);
                        uint32_t local_id = decode_local_page_id(curr_id);
                        if (p_shard != s_dst || local_id >= shard_dst.page_pool.size()) break;
                        auto& page = shard_dst.page_pool[local_id];
                        if (page.next_page_id == curr_id) break; // cycle guard
                        for (uint32_t i = 0; i < page.count; ++i) {
                            if (page.edges[i].target_id == src_id &&
                                page.edges[i].label_id == label_id &&
                                same_edge_weight(page.edges[i].weight, matched_weight) &&
                                page.edges[i].is_active()) {
                                page.edges[i].set_active(false);
                                if (v.in_degree > 0) {
                                    v.in_degree--;
                                }
                                in_found = true;
                                break;
                            }
                        }
                        if (in_found) break;
                        curr_id = page.next_page_id;
                    }
                }
            }
        }
    };

    if (s_src == s_dst) {
        std::unique_lock lock(shards_[s_src].mutex);
        do_del(shards_[s_src], shards_[s_src]);
    } else {
        size_t first = std::min(s_src, s_dst);
        size_t second = std::max(s_src, s_dst);
        std::unique_lock lock1(shards_[first].mutex);
        std::unique_lock lock2(shards_[second].mutex);
        do_del(shards_[s_src], shards_[s_dst]);
    }

    return found;
}

std::vector<AdjacencyStore::IncidentEdgeRecord> AdjacencyStore::remove_vertex(uint64_t id) {
    std::vector<IncidentEdgeRecord> removed;
    for (int pass = 0; pass < 4; ++pass) {
        auto out_edges = get_outgoing_edges_snapshot(id);
        auto in_edges = get_incoming_edges_snapshot(id);
        if (out_edges.empty() && in_edges.empty()) {
            break;
        }
        if (removed.empty()) {
            removed.reserve(out_edges.size() + in_edges.size());
        }
        bool any = false;
        for (const auto& pe : out_edges) {
            uint16_t rflags = 0;
            float rweight = pe.weight;
            if (del_edge(id, pe.target_id, pe.label_id, pe.weight, &rflags, &rweight)) {
                removed.push_back(IncidentEdgeRecord{id, pe.target_id, pe.label_id, rflags, rweight});
                any = true;
            }
        }
        for (const auto& pe : in_edges) {
            uint16_t rflags = 0;
            float rweight = pe.weight;
            if (del_edge(pe.target_id, id, pe.label_id, pe.weight, &rflags, &rweight)) {
                removed.push_back(IncidentEdgeRecord{pe.target_id, id, pe.label_id, rflags, rweight});
                any = true;
            }
        }
        if (!any) {
            break;
        }
    }
    return removed;
}

std::vector<PackedEdge> AdjacencyStore::get_outgoing_edges_snapshot(uint64_t src_id) const {
    std::vector<PackedEdge> result;
    size_t s_idx = shard_index(src_id);
    const auto& shard = shards_[s_idx];
    std::shared_lock lock(shard.mutex);
    auto it = shard.directory.find(src_id);
    if (it == shard.directory.end()) return result;
    const auto& v = it->second;
    result.reserve(v.get_out_degree());
    if (v.is_inlined()) {
        if (v.inline_edge.is_active()) {
            result.push_back(v.inline_edge);
        }
        return result;
    }
    if (!v.is_promoted()) return result;
    uint32_t curr_id = v.ext.out_block_id;
    while (curr_id != 0) {
        uint32_t p_shard = decode_shard_index(curr_id);
        uint32_t p_local = decode_local_page_id(curr_id);
        if (p_shard != s_idx || p_local >= shard.page_pool.size()) break;
        const auto& page = shard.page_pool[p_local];
        if (page.next_page_id == curr_id) break; // cycle guard
        for (uint32_t i = 0; i < page.count; ++i) {
            if (page.edges[i].is_active()) {
                result.push_back(page.edges[i]);
            }
        }
        curr_id = page.next_page_id;
    }
    return result;
}

void AdjacencyStore::filter_outgoing_edges(uint64_t src_id, uint16_t target_label, float min_weight, std::vector<uint64_t>& out_neighbors) const {
    size_t s_idx = shard_index(src_id);
    const auto& shard = shards_[s_idx];
    std::shared_lock lock(shard.mutex);
    auto it = shard.directory.find(src_id);
    if (it == shard.directory.end()) return;
    const auto& v = it->second;
    if (v.is_inlined()) {
        if (v.inline_edge.is_active()) {
            if ((target_label == 0 || v.inline_edge.label_id == target_label) && (v.inline_edge.weight >= min_weight)) {
                out_neighbors.push_back(v.inline_edge.target_id);
            }
        }
        return;
    }
    if (!v.is_promoted()) return;
    uint32_t curr_id = v.ext.out_block_id;
    while (curr_id != 0) {
        uint32_t p_shard = decode_shard_index(curr_id);
        uint32_t p_local = decode_local_page_id(curr_id);
        if (p_shard != s_idx || p_local >= shard.page_pool.size()) break;
        const auto& page = shard.page_pool[p_local];
        if (page.next_page_id == curr_id) break; // cycle guard
        if (page.count == 1) {
            const auto& e = page.edges[0];
            if (e.is_active() && (target_label == 0 || e.label_id == target_label) && (e.weight >= min_weight)) {
                out_neighbors.push_back(e.target_id);
            }
        } else {
            SIMDFilter::filter_edges(std::span<const PackedEdge>(page.edges, page.count), target_label, min_weight, out_neighbors);
        }
        curr_id = page.next_page_id;
    }
}

std::vector<PackedEdge> AdjacencyStore::get_incoming_edges_snapshot(uint64_t dst_id) const {
    std::vector<PackedEdge> result;
    size_t s_idx = shard_index(dst_id);
    const auto& shard = shards_[s_idx];
    std::shared_lock lock(shard.mutex);
    auto it = shard.directory.find(dst_id);
    if (it == shard.directory.end()) return result;
    const auto& v = it->second;
    result.reserve(v.in_degree);
    if (!v.is_promoted()) return result;
    uint32_t curr_id = v.ext.in_block_id;
    while (curr_id != 0) {
        uint32_t p_shard = decode_shard_index(curr_id);
        uint32_t p_local = decode_local_page_id(curr_id);
        if (p_shard != s_idx || p_local >= shard.page_pool.size()) break;
        const auto& page = shard.page_pool[p_local];
        if (page.next_page_id == curr_id) break; // cycle guard
        for (uint32_t i = 0; i < page.count; ++i) {
            if (page.edges[i].is_active()) {
                result.push_back(page.edges[i]);
            }
        }
        curr_id = page.next_page_id;
    }
    return result;
}

std::span<const PackedEdge> AdjacencyStore::get_outgoing_edges(uint64_t src_id) const noexcept {
    size_t s_idx = shard_index(src_id);
    const auto& shard = shards_[s_idx];
    std::shared_lock lock(shard.mutex);
    auto it = shard.directory.find(src_id);
    if (it == shard.directory.end()) {
        return {};
    }
    const auto& v = it->second;
    if (v.is_inlined()) {
        return std::span<const PackedEdge>(&v.inline_edge, 1);
    }
    if (v.is_promoted() && v.ext.out_block_id > 0) {
        uint32_t p_shard = decode_shard_index(v.ext.out_block_id);
        uint32_t local_id = decode_local_page_id(v.ext.out_block_id);
        if (p_shard == s_idx && local_id < shard.page_pool.size()) {
            return std::span<const PackedEdge>(shard.page_pool[local_id].edges,
                                               shard.page_pool[local_id].count);
        }
    }
    return {};
}

std::span<const PackedEdge> AdjacencyStore::get_incoming_edges(uint64_t dst_id) const noexcept {
    size_t s_idx = shard_index(dst_id);
    const auto& shard = shards_[s_idx];
    std::shared_lock lock(shard.mutex);
    auto it = shard.directory.find(dst_id);
    if (it == shard.directory.end()) {
        return {};
    }
    const auto& v = it->second;
    if (!v.is_promoted() || v.ext.in_block_id == 0) {
        return {};
    }
    uint32_t p_shard = decode_shard_index(v.ext.in_block_id);
    uint32_t local_id = decode_local_page_id(v.ext.in_block_id);
    if (p_shard == s_idx && local_id < shard.page_pool.size()) {
        return std::span<const PackedEdge>(shard.page_pool[local_id].edges,
                                           shard.page_pool[local_id].count);
    }
    return {};
}

size_t AdjacencyStore::get_out_degree(uint64_t src_id) const noexcept {
    const auto& shard = shards_[shard_index(src_id)];
    std::shared_lock lock(shard.mutex);
    auto it = shard.directory.find(src_id);
    if (it == shard.directory.end()) {
        return 0;
    }
    return it->second.get_out_degree();
}

size_t AdjacencyStore::get_in_degree(uint64_t dst_id) const noexcept {
    const auto& shard = shards_[shard_index(dst_id)];
    std::shared_lock lock(shard.mutex);
    auto it = shard.directory.find(dst_id);
    if (it == shard.directory.end()) {
        return 0;
    }
    return it->second.in_degree;
}

uint32_t AdjacencyStore::get_out_page_id(uint64_t src_id) const noexcept {
    const auto& shard = shards_[shard_index(src_id)];
    std::shared_lock lock(shard.mutex);
    auto it = shard.directory.find(src_id);
    if (it == shard.directory.end()) {
        return 0;
    }
    const auto& v = it->second;
    if (!v.is_promoted()) {
        return 0;
    }
    return v.ext.out_block_id;
}

const AdjacencyPage* AdjacencyStore::get_page(uint32_t page_id) const noexcept {
    if (page_id == 0) return nullptr;
    size_t s_idx = decode_shard_index(page_id);
    uint32_t local_id = decode_local_page_id(page_id);
    if (s_idx < NUM_SHARDS) {
        const auto& shard = shards_[s_idx];
        std::shared_lock lock(shard.mutex);
        if (local_id < shard.page_pool.size()) {
            return &shard.page_pool[local_id];
        }
    }
    return nullptr;
}

} // namespace l3kvg
