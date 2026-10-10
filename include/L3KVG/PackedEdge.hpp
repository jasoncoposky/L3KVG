#pragma once
#include <cstdint>

namespace l3kvg {

#pragma pack(push, 4)
struct alignas(8) PackedEdge {
    static constexpr uint16_t FLAG_ACTIVE         = 1 << 0;
    static constexpr uint16_t FLAG_HAS_PROPERTIES = 1 << 1;
    static constexpr uint16_t FLAG_DIRECTED       = 1 << 2;

    uint64_t target_id{0};   // 8 bytes: Target vertex ID (or source for incoming)
    uint16_t label_id{0};    // 2 bytes: Dictionary-encoded relationship label
    uint16_t flags{0};       // 2 bytes: Status flags
    float    weight{1.0f};   // 4 bytes: 32-bit float weight
    uint32_t prop_index{0};  // 4 bytes: Offset into PropertyStore (0 = none)
    uint32_t hlc_logical{0}; // 4 bytes: HLC logical clock for Last-Writer-Wins

    [[nodiscard]] constexpr bool is_active() const noexcept {
        return (flags & FLAG_ACTIVE) != 0;
    }

    [[nodiscard]] constexpr bool has_properties() const noexcept {
        return (flags & FLAG_HAS_PROPERTIES) != 0;
    }

    constexpr void set_active(bool active) noexcept {
        if (active) flags |= FLAG_ACTIVE;
        else flags &= ~FLAG_ACTIVE;
    }

    bool operator==(const PackedEdge&) const noexcept = default;
};
#pragma pack(pop)

static_assert(sizeof(PackedEdge) == 24, "PackedEdge must be exactly 24 bytes");
static_assert(alignof(PackedEdge) == 8, "PackedEdge must be 8-byte aligned");

} // namespace l3kvg
