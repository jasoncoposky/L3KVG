#pragma once

#include <string>
#include <chrono>
#include <mutex>
#include <cstdint>
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
    explicit HLCProvider(uint32_t node_id) : node_id_(node_id), last_wall_time_(0), logical_counter_(0) {}

    HLCTimestamp now() {
        std::lock_guard<std::mutex> lock(mu_);
        uint64_t current_wall = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        
        if (current_wall == last_wall_time_) {
            logical_counter_++;
        } else if (current_wall > last_wall_time_) {
            last_wall_time_ = current_wall;
            logical_counter_ = 0;
        } else {
            current_wall = last_wall_time_;
            logical_counter_++;
        }

        return {current_wall, logical_counter_, node_id_};
    }

    void update(const HLCTimestamp& remote) {
        std::lock_guard<std::mutex> lock(mu_);
        uint64_t current_wall = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        
        uint64_t prev_wall = last_wall_time_;
        last_wall_time_ = std::max({prev_wall, remote.wall_time, current_wall});
        if (last_wall_time_ == prev_wall && last_wall_time_ == remote.wall_time) {
            logical_counter_ = std::max(static_cast<uint16_t>(logical_counter_), remote.logical) + 1;
        } else if (last_wall_time_ == prev_wall) {
            logical_counter_++;
        } else if (last_wall_time_ == remote.wall_time) {
            logical_counter_ = remote.logical + 1;
        } else {
            logical_counter_ = 0;
        }
    }

private:
    uint32_t node_id_;
    uint64_t last_wall_time_;
    uint16_t logical_counter_;
    std::mutex mu_;
};

} // namespace l3kvg
