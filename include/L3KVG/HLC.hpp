#pragma once

#include <string>
#include <chrono>
#include <mutex>
#include <cstdint>
#include <nlohmann/json.hpp>

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

    std::string to_json_string() const {
        return "{\"wall_time\": " + std::to_string(wall_time) + 
               ", \"logical\": " + std::to_string(logical) + 
               ", \"node_id\": " + std::to_string(node_id) + "}";
    }

    static HLCTimestamp from_json(const nlohmann::json& j) {
        HLCTimestamp ts;
        ts.wall_time = j.value("wall_time", 0ULL);
        ts.logical = j.value("logical", (uint16_t)0);
        ts.node_id = j.value("node_id", 0U);
        return ts;
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
