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
        size_t hlc_ofs = buf.set_obj(parent_ofs, key);
        buf.set_i64(hlc_ofs, "wall_time", static_cast<int64_t>(wall_time));
        buf.set_i64(hlc_ofs, "logical", static_cast<int64_t>(logical));
        buf.set_i64(hlc_ofs, "node_id", static_cast<int64_t>(node_id));
    }

    static HLCTimestamp read_from_buffer(const lite3cpp::Buffer& buf, size_t parent_ofs = 0, std::string_view key = "_hlc") {
        HLCTimestamp ts;
        try {
            size_t hlc_ofs = buf.get_obj(parent_ofs, key);
            if (hlc_ofs != static_cast<size_t>(-1)) {
                ts.wall_time = static_cast<uint64_t>(buf.get_i64(hlc_ofs, "wall_time"));
                ts.logical = static_cast<uint16_t>(buf.get_i64(hlc_ofs, "logical"));
                ts.node_id = static_cast<uint32_t>(buf.get_i64(hlc_ofs, "node_id"));
            }
        } catch (...) {}
        return ts;
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
        
        last_wall_time_ = std::max({last_wall_time_, remote.wall_time, current_wall});
        if (last_wall_time_ == remote.wall_time && last_wall_time_ == current_wall) {
            logical_counter_ = std::max((uint16_t)logical_counter_, remote.logical) + 1;
        } else if (last_wall_time_ == remote.wall_time) {
            logical_counter_ = remote.logical + 1;
        } else if (last_wall_time_ == current_wall) {
            logical_counter_++;
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
