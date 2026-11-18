#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>
#include <algorithm>
#include "buffer.hpp"

namespace l3kvg {

struct HLCTimestamp {
    uint64_t wall_time = 0;
    uint16_t logical = 0;
    uint32_t node_id = 0;

    bool operator>(const HLCTimestamp& other) const {
        if (wall_time != other.wall_time) return wall_time > other.wall_time;
        if (logical != other.logical) return logical > other.logical;
        return node_id > other.node_id;
    }

    void write_to_buffer(lite3cpp::Buffer& buf, size_t parent_ofs = 0, std::string_view key = "_hlc") const {
        using namespace lite3cpp::literals;
        if (parent_ofs == 0 && buf.size() == 0) {
            buf.init_object();
        }
        const lite3cpp::HashedKey hk = (key == "_hlc") ? "_hlc"_hk : lite3cpp::HashedKey(key);
        size_t hlc_ofs = buf.set_obj(parent_ofs, hk);
        buf.set_i64(hlc_ofs, "wall_time"_hk, static_cast<int64_t>(wall_time));
        buf.set_i64(hlc_ofs, "logical"_hk, static_cast<int64_t>(logical));
        buf.set_i64(hlc_ofs, "node_id"_hk, static_cast<int64_t>(node_id));
    }

    static HLCTimestamp read_from_buffer(const lite3cpp::Buffer& buf, size_t parent_ofs = 0, std::string_view key = "_hlc") noexcept {
        using namespace lite3cpp::literals;
        HLCTimestamp ts;
        if (buf.size() < sizeof(lite3cpp::PackedNodeLayout)) {
            return ts;
        }
        const lite3cpp::HashedKey hk = (key == "_hlc") ? "_hlc"_hk : lite3cpp::HashedKey(key);
        auto meta_ofs = buf.try_get_obj(parent_ofs, hk);
        if (!meta_ofs) {
            return ts;
        }
        if (auto w = buf.try_get_i64(*meta_ofs, "wall_time"_hk)) {
            ts.wall_time = static_cast<uint64_t>(*w);
        } else if (auto w2 = buf.try_get_i64(*meta_ofs, "w"_hk)) {
            ts.wall_time = static_cast<uint64_t>(*w2);
        } else if (auto wf = buf.try_get_f64(*meta_ofs, "wall_time"_hk)) {
            ts.wall_time = static_cast<uint64_t>(*wf);
        }

        if (auto l = buf.try_get_i64(*meta_ofs, "logical"_hk)) {
            ts.logical = static_cast<uint16_t>(*l);
        } else if (auto l2 = buf.try_get_i64(*meta_ofs, "l"_hk)) {
            ts.logical = static_cast<uint16_t>(*l2);
        } else if (auto lf = buf.try_get_f64(*meta_ofs, "logical"_hk)) {
            ts.logical = static_cast<uint16_t>(*lf);
        }

        if (auto n = buf.try_get_i64(*meta_ofs, "node_id"_hk)) {
            ts.node_id = static_cast<uint32_t>(*n);
        } else if (auto c = buf.try_get_i64(*meta_ofs, "c"_hk)) {
            ts.node_id = static_cast<uint32_t>(*c);
        } else if (auto nf = buf.try_get_f64(*meta_ofs, "node_id"_hk)) {
            ts.node_id = static_cast<uint32_t>(*nf);
        }
        return ts;
    }

    static HLCTimestamp extract_from_lite3(const lite3cpp::Buffer& buf) noexcept {
        return read_from_buffer(buf, 0, "_hlc");
    }

    std::string to_json_string() const {
        return "{\"wall_time\": " + std::to_string(wall_time) + 
               ", \"logical\": " + std::to_string(logical) + 
               ", \"node_id\": " + std::to_string(node_id) + "}";
    }
};

class HLCProvider {
public:
    explicit HLCProvider(uint32_t node_id) : node_id_(node_id) {}

    HLCTimestamp now() {
        uint64_t current_wall = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count()) & kWallMask;

        uint64_t expected = state_.load(std::memory_order_relaxed);
        for (;;) {
            uint64_t last_wall = expected >> 16;
            uint16_t last_logical = static_cast<uint16_t>(expected & 0xFFFFu);
            uint64_t next_wall = last_wall;
            uint16_t next_logical = 0;

            if (current_wall > last_wall) {
                next_wall = current_wall;
                next_logical = 0;
            } else {
                next_logical = static_cast<uint16_t>(last_logical + 1u);
                if (next_logical == 0) {
                    next_wall = last_wall + 1u;
                }
            }

            uint64_t desired = (next_wall << 16) | next_logical;
            if (state_.compare_exchange_weak(expected, desired,
                                             std::memory_order_acq_rel,
                                             std::memory_order_relaxed)) {
                return {next_wall, next_logical, node_id_};
            }
        }
    }

    void update(const HLCTimestamp& remote) {
        uint64_t current_wall = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count()) & kWallMask;
        uint64_t remote_wall = remote.wall_time & kWallMask;

        uint64_t expected = state_.load(std::memory_order_relaxed);
        for (;;) {
            uint64_t prev_wall = expected >> 16;
            uint16_t prev_logical = static_cast<uint16_t>(expected & 0xFFFFu);

            uint64_t next_wall = std::max({prev_wall, remote_wall, current_wall});
            uint16_t next_logical = 0;
            if (next_wall == prev_wall && next_wall == remote_wall) {
                next_logical = static_cast<uint16_t>(std::max(prev_logical, remote.logical) + 1u);
            } else if (next_wall == prev_wall) {
                next_logical = static_cast<uint16_t>(prev_logical + 1u);
            } else if (next_wall == remote_wall) {
                next_logical = static_cast<uint16_t>(remote.logical + 1u);
            }

            uint64_t desired = (next_wall << 16) | next_logical;
            if (state_.compare_exchange_weak(expected, desired,
                                             std::memory_order_acq_rel,
                                             std::memory_order_relaxed)) {
                return;
            }
        }
    }

private:
    static constexpr uint64_t kWallMask = (1ULL << 48) - 1ULL;
    uint32_t node_id_;
    std::atomic<uint64_t> state_{0};
};

} // namespace l3kvg
