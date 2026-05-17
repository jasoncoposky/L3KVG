#include <gtest/gtest.h>
#include "L3KVG/FederationResolver.hpp"
#include "L3KVG/FederationID.hpp"
#include <lite3/ring.hpp>

TEST(FederationResolverTest, RegistrationAndLookup) {
    auto ring = std::make_shared<lite3::ConsistentHash>();
    l3kvg::FederationResolver resolver(ring, 1);
    
    resolver.register_local_cluster("us-east", 101);
    resolver.register_federation("eu-west", 100, {"tcp://eu-west:8080"});
    
    EXPECT_TRUE(resolver.is_local_cluster(101));
    EXPECT_FALSE(resolver.is_local_cluster(100));
    
    auto endpoints = resolver.get_federation_endpoints(100);
    ASSERT_NE(endpoints, nullptr);
    ASSERT_EQ(endpoints->size(), 1);
    EXPECT_EQ((*endpoints)[0], "tcp://eu-west:8080");
}

TEST(FederationResolverTest, ParseUUID) {
    auto ring = std::make_shared<lite3::ConsistentHash>();
    l3kvg::FederationResolver resolver(ring, 1);
    resolver.register_local_cluster("us-east", 101);
    resolver.register_federation("eu-west", 100, {"tcp://eu-west:8080"});
    
    uint64_t id1 = resolver.parse_uuid("eu-west:user_1");
    EXPECT_EQ(l3kvg::FederationID::get_cluster(id1), 100);
    
    uint64_t id2 = resolver.parse_uuid("user_1");
    EXPECT_EQ(l3kvg::FederationID::get_cluster(id2), 101);

    // Consistency check
    EXPECT_EQ(resolver.parse_uuid("user_1"), id2);
    
    // Different UUIDs should have different hashes (mostly)
    uint64_t id3 = resolver.parse_uuid("user_2");
    EXPECT_NE(id2, id3);
    EXPECT_EQ(l3kvg::FederationID::get_cluster(id3), 101);

    // Unknown cluster should throw
    EXPECT_THROW({ (void)resolver.parse_uuid("unknown:user_1"); }, std::runtime_error);
}
