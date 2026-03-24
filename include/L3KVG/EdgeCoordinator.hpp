#pragma once

#include <string>
#include <chrono>
#include <mutex>
#include <cstdint>
#include <future>
#include "L3KVG/ClusterResolver.hpp"
#include "L3KVG/RemoteL3KVClient.hpp"

namespace l3kv { class Engine; }

namespace l3kvg {

struct HLCTimestamp {
    uint64_t wall_time;
    uint16_t logical;
    uint32_t node_id;

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

private:
    uint32_t node_id_;
    uint64_t last_wall_time_;
    uint16_t logical_counter_;
    std::mutex mu_;
};

class EdgeCoordinator {
public:
    EdgeCoordinator(l3kv::Engine* store, ClusterResolver& resolver, RemoteL3KVClient& remote_client, uint32_t node_id);

    void atomic_put_edge(const std::string& src_uuid, const std::string& label, double weight, const std::string& dst_uuid, const std::string& payload = "");

private:
    l3kv::Engine* store_;
    ClusterResolver& resolver_;
    RemoteL3KVClient& remote_client_;
    HLCProvider hlc_;
};

} // namespace l3kvg
