#pragma once
#include <cstdint>
#include <cstddef>
#include <type_traits>

namespace l3kvg {

struct alignas(8) PackedEdge {
    static constexpr uint16_t FLAG_ACTIVE         = 1 << 0;
    static constexpr uint16_t FLAG_HAS_PROPERTIES = 1 << 1;
    static constexpr uint16_t FLAG_DIRECTED       = 1 << 2;

    uint64_t target_id{0};      // 8 bytes (offset 0): Target vertex ID (or source for incoming)
    uint16_t label_id{0};       // 2 bytes (offset 8): Dictionary-encoded relationship label
    uint16_t flags{0};          // 2 bytes (offset 10): Status flags
    float    weight{1.0f};      // 4 bytes (offset 12): 32-bit float weight
    uint64_t hlc_timestamp{0};  // 8 bytes (offset 16): Full 64-bit HLC timestamp

    [[nodiscard]] constexpr bool is_active() const noexcept {
        return (flags & FLAG_ACTIVE) != 0;
    }

    [[nodiscard]] constexpr bool has_properties() const noexcept {
        return (flags & FLAG_HAS_PROPERTIES) != 0;
    }

    [[nodiscard]] constexpr bool is_directed() const noexcept {
        return (flags & FLAG_DIRECTED) != 0;
    }

    constexpr void set_active(bool active) noexcept {
        if (active) flags |= FLAG_ACTIVE;
        else flags &= ~FLAG_ACTIVE;
    }

    constexpr void set_directed(bool directed) noexcept {
        if (directed) flags |= FLAG_DIRECTED;
        else flags &= ~FLAG_DIRECTED;
    }

    constexpr void set_has_properties(bool has_props) noexcept {
        if (has_props) flags |= FLAG_HAS_PROPERTIES;
        else flags &= ~FLAG_HAS_PROPERTIES;
    }

    bool operator==(const PackedEdge&) const noexcept = default;
};

static_assert(sizeof(PackedEdge) == 24, "PackedEdge must be exactly 24 bytes");
static_assert(alignof(PackedEdge) == 8, "PackedEdge must be 8-byte aligned");
static_assert(offsetof(PackedEdge, label_id) == 8, "PackedEdge::label_id must be at offset 8");
static_assert(offsetof(PackedEdge, flags) == 10, "PackedEdge::flags must be at offset 10");
static_assert(offsetof(PackedEdge, weight) == 12, "PackedEdge::weight must be at offset 12");
static_assert(offsetof(PackedEdge, hlc_timestamp) == 16, "PackedEdge::hlc_timestamp must be at offset 16");
static_assert(std::is_trivially_copyable_v<PackedEdge>, "PackedEdge must be trivially copyable for binary serialization");
static_assert(std::is_standard_layout_v<PackedEdge>, "PackedEdge must be standard layout");

} // namespace l3kvg
