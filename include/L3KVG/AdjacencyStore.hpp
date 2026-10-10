#pragma once
#include <cstdint>
#include <cstddef>
#include <span>
#include <vector>
#include <memory>
#include <shared_mutex>
#include <unordered_map>
#include <array>
#include <type_traits>
#include "L3KVG/PackedEdge.hpp"
#include "L3KVG/LabelRegistry.hpp"

namespace l3kvg {

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#endif

struct alignas(8) VertexRecord {
    uint32_t out_degree{0};
    uint32_t in_degree{0};

    union {
        PackedEdge inline_edge;
        struct {
            uint32_t flags;
            uint32_t reserved;
            struct {
                uint32_t out_block_id;
                uint32_t in_block_id;
                uint64_t page_offset;
            } ext;
        };
    };

    VertexRecord() noexcept : out_degree(0), in_degree(0), flags(0), reserved(0) {
        ext.out_block_id = 0;
        ext.in_block_id = 0;
        ext.page_offset = 0;
    }
};

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif

static_assert(sizeof(VertexRecord) == 32, "VertexRecord must be exactly 32 bytes");
static_assert(alignof(VertexRecord) == 8, "VertexRecord must be 8-byte aligned");
static_assert(std::is_trivially_copyable_v<VertexRecord>, "VertexRecord must be trivially copyable");
static_assert(std::is_standard_layout_v<VertexRecord>, "VertexRecord must be standard layout");

struct alignas(64) AdjacencyPage {
    static constexpr uint32_t CAPACITY = 170;
    uint32_t count{0};
    uint32_t next_page_id{0};
    uint32_t version{0};
    uint32_t reserved{0};
    PackedEdge edges[CAPACITY];
};

static_assert(sizeof(AdjacencyPage) == 4096, "AdjacencyPage must be exactly 4096 bytes");
static_assert(alignof(AdjacencyPage) == 64, "AdjacencyPage must be 64-byte aligned");

class AdjacencyStore {
public:
    AdjacencyStore();
    ~AdjacencyStore() = default;

    AdjacencyStore(const AdjacencyStore&) = delete;
    AdjacencyStore& operator=(const AdjacencyStore&) = delete;
    AdjacencyStore(AdjacencyStore&&) = delete;
    AdjacencyStore& operator=(AdjacencyStore&&) = delete;

    void add_edge(uint64_t src_id, uint64_t dst_id, uint16_t label_id,
                  float weight = 1.0f, uint16_t flags = PackedEdge::FLAG_ACTIVE,
                  uint32_t prop_index = 0, uint32_t hlc = 0);

    bool del_edge(uint64_t src_id, uint64_t dst_id, uint16_t label_id);

    [[nodiscard]] std::span<const PackedEdge> get_outgoing_edges(uint64_t src_id) const noexcept;
    [[nodiscard]] std::span<const PackedEdge> get_incoming_edges(uint64_t dst_id) const noexcept;

    [[nodiscard]] size_t get_out_degree(uint64_t src_id) const noexcept;
    [[nodiscard]] size_t get_in_degree(uint64_t dst_id) const noexcept;

private:
    struct NodeAdjacency {
        mutable std::shared_mutex mutex;
        VertexRecord out_record;
        VertexRecord in_record;
        std::vector<PackedEdge> out_edges; // Dynamic page buffer
        std::vector<PackedEdge> in_edges;  // Dynamic page buffer
    };

    static constexpr size_t NUM_SHARDS = 64;

    struct Shard {
        mutable std::shared_mutex mutex;
        std::unordered_map<uint64_t, std::shared_ptr<NodeAdjacency>> nodes;
    };

    std::array<Shard, NUM_SHARDS> shards_;

    [[nodiscard]] static size_t shard_idx(uint64_t id) noexcept {
        id ^= id >> 33;
        id *= 0xff51afd7ed558ccdULL;
        id ^= id >> 33;
        id *= 0xc4ceb9fe1a85ec53ULL;
        id ^= id >> 33;
        return static_cast<size_t>(id % NUM_SHARDS);
    }

    std::shared_ptr<NodeAdjacency> get_or_create_node(uint64_t id);
    [[nodiscard]] std::shared_ptr<NodeAdjacency> get_node(uint64_t id) const noexcept;
};

} // namespace l3kvg
