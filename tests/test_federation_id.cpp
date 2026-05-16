#include <gtest/gtest.h>
#include "L3KVG/FederationID.hpp"

TEST(FederationIDTest, PackUnpack) {
    uint16_t cluster_id = 100;
    uint64_t node_hash = 0x1A2B3C4D5E6FULL;
    uint64_t id = l3kvg::FederationID::pack(cluster_id, node_hash);
    
    EXPECT_EQ(l3kvg::FederationID::get_cluster(id), cluster_id);
    EXPECT_EQ(l3kvg::FederationID::get_local_hash(id), node_hash & 0xFFFFFFFFFFFFULL);
}

TEST(FederationIDTest, EdgeValues) {
    uint16_t cluster_id = 0xFFFF;
    uint64_t node_hash = 0xFFFFFFFFFFFFFFFFULL;
    uint64_t id = l3kvg::FederationID::pack(cluster_id, node_hash);
    
    EXPECT_EQ(l3kvg::FederationID::get_cluster(id), 0xFFFF);
    EXPECT_EQ(l3kvg::FederationID::get_local_hash(id), 0xFFFFFFFFFFFFULL);
}
