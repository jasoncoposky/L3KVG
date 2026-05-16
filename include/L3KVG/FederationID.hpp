#pragma once
#include <cstdint>

namespace l3kvg {
class FederationID {
public:
    static constexpr uint64_t LOCAL_HASH_MASK = 0xFFFFFFFFFFFFULL;
    static uint64_t pack(uint16_t cluster_id, uint64_t node_hash) {
        return (static_cast<uint64_t>(cluster_id) << 48) | (node_hash & LOCAL_HASH_MASK);
    }
    static uint16_t get_cluster(uint64_t id) { return static_cast<uint16_t>(id >> 48); }
    static uint64_t get_local_hash(uint64_t id) { return id & LOCAL_HASH_MASK; }
};
}
