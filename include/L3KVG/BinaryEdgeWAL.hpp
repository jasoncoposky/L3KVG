#pragma once

#include <cstdint>
#include <cstddef>
#include <span>
#include <string>
#include <mutex>
#include <type_traits>
#include "L3KVG/AdjacencyStore.hpp"

namespace l3kvg {

/**
 * @brief Fixed 32-byte representation of an edge mutation operation for the WAL.
 * Naturally 8-byte aligned with no padding.
 */
struct alignas(8) EdgeMutationOp {
    static constexpr uint16_t OP_DELETE   = 1 << 15;
    static constexpr uint16_t FLAG_ACTIVE = 1 << 0;

    uint64_t src_id{0};
    uint64_t dst_id{0};
    uint64_t hlc_timestamp{0};
    uint16_t label_id{0};
    uint16_t flags{0};
    float    weight{1.0f};

    [[nodiscard]] constexpr bool is_delete() const noexcept {
        return (flags & OP_DELETE) != 0;
    }

    [[nodiscard]] constexpr bool is_active() const noexcept {
        return (flags & FLAG_ACTIVE) != 0;
    }

    constexpr void set_delete(bool del = true) noexcept {
        if (del) {
            flags |= OP_DELETE;
        } else {
            flags &= ~OP_DELETE;
        }
    }

    constexpr void set_active(bool active = true) noexcept {
        if (active) {
            flags |= FLAG_ACTIVE;
        } else {
            flags &= ~FLAG_ACTIVE;
        }
    }

    bool operator==(const EdgeMutationOp&) const noexcept = default;
};

static_assert(sizeof(EdgeMutationOp) == 32, "EdgeMutationOp must be exactly 32 bytes");
static_assert(alignof(EdgeMutationOp) == 8, "EdgeMutationOp must be 8-byte aligned");
static_assert(std::is_trivially_copyable_v<EdgeMutationOp>, "EdgeMutationOp must be trivially copyable");
static_assert(std::is_standard_layout_v<EdgeMutationOp>, "EdgeMutationOp must be standard layout");

/**
 * @brief High-throughput binary Write-Ahead Log for edge mutations.
 * Appends raw 32-byte EdgeMutationOp records and performs fast 64KB chunked sequential recovery.
 */
class BinaryEdgeWAL {
public:
    explicit BinaryEdgeWAL(const std::string& path);
    ~BinaryEdgeWAL();

    BinaryEdgeWAL(const BinaryEdgeWAL&) = delete;
    BinaryEdgeWAL& operator=(const BinaryEdgeWAL&) = delete;

    BinaryEdgeWAL(BinaryEdgeWAL&&) noexcept;
    BinaryEdgeWAL& operator=(BinaryEdgeWAL&&) noexcept;

    void append(const EdgeMutationOp& op);
    void append_batch(std::span<const EdgeMutationOp> ops);
    size_t recover(AdjacencyStore& store);
    void flush();
    void close();
    [[nodiscard]] bool is_open() const noexcept;
    [[nodiscard]] const std::string& path() const noexcept { return path_; }

private:
    void flush_unlocked();
    void close_unlocked();
    void write_all_unlocked(const char* data, size_t total_bytes);

    std::string path_;
    int fd_{-1};
    mutable std::mutex write_mutex_;
    std::vector<char> buffer_;
};

} // namespace l3kvg
