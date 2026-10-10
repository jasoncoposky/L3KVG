#pragma once
#include <cstdint>
#include <cstddef>
#include <span>
#include <deque>
#include <shared_mutex>
#include <unordered_map>
#include <array>
#include <type_traits>
#include "L3KVG/PackedEdge.hpp"
#include "L3KVG/LabelRegistry.hpp"

namespace l3kvg {

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpacked-not-aligned"
#pragma GCC diagnostic ignored "-Wpedantic"
#endif

#pragma pack(push, 4)
struct alignas(8) VertexRecord {
    uint32_t out_degree{0};
    uint32_t in_degree{0};
    union {
        PackedEdge inline_edge; // Inlined edge when out_degree == 1 && ext.out_block_id == 0
        struct {
            uint32_t out_block_id; // 1-indexed into page_pool (0 = inlined/none)
            uint32_t in_block_id;  // 1-indexed into page_pool (0 = none)
            uint64_t page_offset;
        } ext;
    };

    VertexRecord() noexcept : out_degree(0), in_degree(0) {
        ext.out_block_id = 0;
        ext.in_block_id = 0;
        ext.page_offset = 0;
    }

    [[nodiscard]] bool is_inlined() const noexcept {
        return out_degree == 1 && in_degree == 0;
    }
};
#pragma pack(pop)

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif

static_assert(sizeof(VertexRecord) == 32, "VertexRecord must be exactly 32 bytes");
static_assert(alignof(VertexRecord) == 8, "VertexRecord must be 8-byte aligned");
static_assert(std::is_trivially_copyable_v<VertexRecord>, "VertexRecord must be trivially copyable");

struct alignas(64) AdjacencyPage {
    static constexpr uint32_t CAPACITY = 170; // 170 * 24 = 4080 bytes. 4080 + 16 header = 4096 bytes (4KB)
    uint32_t count{0};
    uint32_t next_page_id{0}; // 0 = none, or 1-indexed into page_pool
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

    // Helper for testing page internals
    [[nodiscard]] uint32_t get_out_page_id(uint64_t src_id) const noexcept;
    [[nodiscard]] const AdjacencyPage* get_page(uint32_t page_id) const noexcept;

private:
    struct alignas(64) Shard {
        mutable std::shared_mutex mutex;
        std::unordered_map<uint64_t, VertexRecord> directory;
        // std::deque guarantees pointer stability on emplace_back!
        std::deque<AdjacencyPage> page_pool;

        Shard() {
            // Index 0 reserved as null page ID
            page_pool.emplace_back();
        }
    };

    static constexpr size_t NUM_SHARDS = 64;
    std::array<Shard, NUM_SHARDS> shards_;

    [[nodiscard]] static size_t shard_index(uint64_t id) noexcept {
        return (id ^ (id >> 16)) % NUM_SHARDS;
    }
};

} // namespace l3kvg
