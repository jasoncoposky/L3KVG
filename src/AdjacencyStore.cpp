#include "L3KVG/AdjacencyStore.hpp"
#include <algorithm>

namespace l3kvg {

namespace {

constexpr uint32_t encode_page_id(size_t shard_idx, uint32_t local_idx) noexcept {
    return (static_cast<uint32_t>(shard_idx) << 24) | (local_idx & 0x00FFFFFF);
}

constexpr size_t decode_shard(uint32_t page_id) noexcept {
    return static_cast<size_t>((page_id >> 24) & 0xFF);
}

constexpr uint32_t decode_local(uint32_t page_id) noexcept {
    return page_id & 0x00FFFFFF;
}

} // anonymous namespace

AdjacencyStore::AdjacencyStore() = default;

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

    size_t s_src = shard_index(src_id);
    size_t s_dst = shard_index(dst_id);

    auto update_src = [s_src, src_id, &edge](Shard& shard) {
        VertexRecord& v = shard.directory[src_id];
        if (v.out_degree == 0 && v.in_degree == 0) {
            // Zero-allocation inline
            v.inline_edge = edge;
            v.out_degree = 1;
        } else if (v.is_inlined()) {
            // Promotion from inlined edge to slotted page
            PackedEdge old_inline = v.inline_edge;
            uint32_t local_id = static_cast<uint32_t>(shard.page_pool.size());
            shard.page_pool.emplace_back();
            auto& page = shard.page_pool.back();
            page.edges[0] = old_inline;
            page.edges[1] = edge;
            page.count = 2;
            page.next_page_id = 0;

            uint32_t page_id = encode_page_id(s_src, local_id);
            v.ext.out_block_id = page_id;
            v.ext.in_block_id = 0;
            v.ext.page_offset = 0;
            v.out_degree = 2;
        } else if (v.out_degree == 0) {
            // in_degree > 0, so ext is active. Allocate outgoing page chain.
            uint32_t local_id = static_cast<uint32_t>(shard.page_pool.size());
            shard.page_pool.emplace_back();
            auto& page = shard.page_pool.back();
            page.edges[0] = edge;
            page.count = 1;
            page.next_page_id = 0;

            uint32_t page_id = encode_page_id(s_src, local_id);
            v.ext.out_block_id = page_id;
            v.out_degree = 1;
        } else {
            // Already has page chain in ext.out_block_id
            uint32_t curr_id = v.ext.out_block_id;
            uint32_t local_id = decode_local(curr_id);
            while (shard.page_pool[local_id].next_page_id != 0) {
                curr_id = shard.page_pool[local_id].next_page_id;
                local_id = decode_local(curr_id);
            }
            auto* curr_page = &shard.page_pool[local_id];
            if (curr_page->count < AdjacencyPage::CAPACITY) {
                curr_page->edges[curr_page->count++] = edge;
            } else {
                uint32_t new_local_id = static_cast<uint32_t>(shard.page_pool.size());
                shard.page_pool.emplace_back();
                uint32_t new_page_id = encode_page_id(s_src, new_local_id);
                curr_page->next_page_id = new_page_id;
                auto& new_page = shard.page_pool.back();
                new_page.edges[0] = edge;
                new_page.count = 1;
                new_page.next_page_id = 0;
            }
            v.out_degree++;
        }
    };

    auto update_dst = [s_dst, dst_id, src_id, &edge](Shard& shard) {
        PackedEdge in_edge = edge;
        in_edge.target_id = src_id; // For incoming edge, target is source

        VertexRecord& v = shard.directory[dst_id];
        // If dst had an inlined outgoing edge, promote it before ext is written
        if (v.is_inlined()) {
            PackedEdge old_inline = v.inline_edge;
            uint32_t out_local_id = static_cast<uint32_t>(shard.page_pool.size());
            shard.page_pool.emplace_back();
            auto& out_page = shard.page_pool.back();
            out_page.edges[0] = old_inline;
            out_page.count = 1;
            out_page.next_page_id = 0;

            uint32_t out_pid = encode_page_id(s_dst, out_local_id);
            v.ext.out_block_id = out_pid;
            v.ext.in_block_id = 0;
            v.ext.page_offset = 0;
        }

        if (v.in_degree == 0) {
            uint32_t in_local_id = static_cast<uint32_t>(shard.page_pool.size());
            shard.page_pool.emplace_back();
            auto& in_page = shard.page_pool.back();
            in_page.edges[0] = in_edge;
            in_page.count = 1;
            in_page.next_page_id = 0;

            uint32_t in_pid = encode_page_id(s_dst, in_local_id);
            v.ext.in_block_id = in_pid;
            v.in_degree = 1;
        } else {
            uint32_t curr_id = v.ext.in_block_id;
            uint32_t local_id = decode_local(curr_id);
            while (shard.page_pool[local_id].next_page_id != 0) {
                curr_id = shard.page_pool[local_id].next_page_id;
                local_id = decode_local(curr_id);
            }
            auto* curr_page = &shard.page_pool[local_id];
            if (curr_page->count < AdjacencyPage::CAPACITY) {
                curr_page->edges[curr_page->count++] = in_edge;
            } else {
                uint32_t new_local_id = static_cast<uint32_t>(shard.page_pool.size());
                shard.page_pool.emplace_back();
                uint32_t new_page_id = encode_page_id(s_dst, new_local_id);
                curr_page->next_page_id = new_page_id;
                auto& new_page = shard.page_pool.back();
                new_page.edges[0] = in_edge;
                new_page.count = 1;
                new_page.next_page_id = 0;
            }
            v.in_degree++;
        }
    };

    if (s_src == s_dst) {
        std::unique_lock lock(shards_[s_src].mutex);
        update_src(shards_[s_src]);
        update_dst(shards_[s_src]);
    } else {
        size_t first = std::min(s_src, s_dst);
        size_t second = std::max(s_src, s_dst);
        std::unique_lock lock1(shards_[first].mutex);
        std::unique_lock lock2(shards_[second].mutex);
        update_src(shards_[s_src]);
        update_dst(shards_[s_dst]);
    }
}

bool AdjacencyStore::del_edge(uint64_t src_id, uint64_t dst_id, uint16_t label_id) {
    size_t s_src = shard_index(src_id);
    size_t s_dst = shard_index(dst_id);
    bool found = false;

    auto do_del = [&](Shard& shard_src, Shard& shard_dst) {
        // 1. Tombstone in outgoing
        if (auto it = shard_src.directory.find(src_id); it != shard_src.directory.end()) {
            auto& v = it->second;
            if (v.is_inlined()) {
                if (v.inline_edge.target_id == dst_id &&
                    v.inline_edge.label_id == label_id &&
                    v.inline_edge.is_active()) {
                    v.inline_edge.set_active(false);
                    found = true;
                }
            } else if (v.ext.out_block_id > 0) {
                uint32_t curr_id = v.ext.out_block_id;
                while (curr_id != 0) {
                    uint32_t local_id = decode_local(curr_id);
                    if (local_id >= shard_src.page_pool.size()) break;
                    auto& page = shard_src.page_pool[local_id];
                    for (uint32_t i = 0; i < page.count; ++i) {
                        if (page.edges[i].target_id == dst_id &&
                            page.edges[i].label_id == label_id &&
                            page.edges[i].is_active()) {
                            page.edges[i].set_active(false);
                            found = true;
                            break;
                        }
                    }
                    if (found) break;
                    curr_id = page.next_page_id;
                }
            }
        }

        // 2. Tombstone in incoming
        if (auto it = shard_dst.directory.find(dst_id); it != shard_dst.directory.end()) {
            auto& v = it->second;
            if (v.ext.in_block_id > 0) {
                uint32_t curr_id = v.ext.in_block_id;
                while (curr_id != 0) {
                    uint32_t local_id = decode_local(curr_id);
                    if (local_id >= shard_dst.page_pool.size()) break;
                    auto& page = shard_dst.page_pool[local_id];
                    for (uint32_t i = 0; i < page.count; ++i) {
                        if (page.edges[i].target_id == src_id &&
                            page.edges[i].label_id == label_id &&
                            page.edges[i].is_active()) {
                            page.edges[i].set_active(false);
                            break;
                        }
                    }
                    curr_id = page.next_page_id;
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

std::span<const PackedEdge> AdjacencyStore::get_outgoing_edges(uint64_t src_id) const noexcept {
    const auto& shard = shards_[shard_index(src_id)];
    std::shared_lock lock(shard.mutex);
    auto it = shard.directory.find(src_id);
    if (it == shard.directory.end()) {
        return {};
    }
    const auto& v = it->second;
    if (v.out_degree == 0) {
        return {};
    }
    if (v.is_inlined()) {
        return std::span<const PackedEdge>(&v.inline_edge, 1);
    }
    if (v.ext.out_block_id > 0) {
        uint32_t local_id = decode_local(v.ext.out_block_id);
        if (local_id < shard.page_pool.size()) {
            return std::span<const PackedEdge>(shard.page_pool[local_id].edges,
                                               shard.page_pool[local_id].count);
        }
    }
    return {};
}

std::span<const PackedEdge> AdjacencyStore::get_incoming_edges(uint64_t dst_id) const noexcept {
    const auto& shard = shards_[shard_index(dst_id)];
    std::shared_lock lock(shard.mutex);
    auto it = shard.directory.find(dst_id);
    if (it == shard.directory.end()) {
        return {};
    }
    const auto& v = it->second;
    if (v.in_degree == 0 || v.ext.in_block_id == 0) {
        return {};
    }
    uint32_t local_id = decode_local(v.ext.in_block_id);
    if (local_id < shard.page_pool.size()) {
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
    return it->second.out_degree;
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
    if (v.out_degree <= 1 && v.in_degree == 0) {
        return 0; // Inlined
    }
    return v.ext.out_block_id;
}

const AdjacencyPage* AdjacencyStore::get_page(uint32_t page_id) const noexcept {
    if (page_id == 0) return nullptr;
    size_t s_idx = decode_shard(page_id);
    uint32_t local_id = decode_local(page_id);
    if (s_idx < NUM_SHARDS) {
        const auto& shard = shards_[s_idx];
        std::shared_lock lock(shard.mutex);
        if (local_id < shard.page_pool.size()) {
            return &shard.page_pool[local_id];
        }
    }
    // Fallback if s_idx was 0 and not found in shard 0
    if (s_idx == 0) {
        for (size_t i = 0; i < NUM_SHARDS; ++i) {
            const auto& shard = shards_[i];
            std::shared_lock lock(shard.mutex);
            if (local_id < shard.page_pool.size()) {
                return &shard.page_pool[local_id];
            }
        }
    }
    return nullptr;
}

} // namespace l3kvg
