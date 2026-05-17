#include <gtest/gtest.h>
#include "L3KVG/FederationResolver.hpp"
#include <lite3/ring.hpp>
#include <memory>

TEST(FederationResolverTest, BasicMapping) {
  auto ring = std::make_shared<lite3::ConsistentHash>(100);
  ring->add_node(1);
  ring->add_node(2);
  ring->add_node(3);

  l3kvg::FederationResolver resolver(ring, 1);

  EXPECT_EQ(resolver.get_local_node_id(), 1);

  // Check some vertex assignments using the new uint64_t ID system
  uint64_t id1 = resolver.parse_uuid("vertex_A");
  uint64_t id2 = resolver.parse_uuid("vertex_B");
  
  lite3::NodeID owner1 = resolver.get_node_owner(id1);
  lite3::NodeID owner2 = resolver.get_node_owner(id2);
  
  EXPECT_TRUE(owner1 >= 1 && owner1 <= 3);
  EXPECT_TRUE(owner2 >= 1 && owner2 <= 3);

  // Same vertex should always return same owner
  EXPECT_EQ(resolver.get_node_owner(id1), owner1);
}

TEST(FederationResolverTest, UnshardedFallback) {
  auto empty_ring = std::make_shared<lite3::ConsistentHash>(100);
  l3kvg::FederationResolver resolver(empty_ring, 42);

  // Should fallback to local node id
  uint64_t id = resolver.parse_uuid("any_vertex");
  EXPECT_EQ(resolver.get_node_owner(id), 42);
  EXPECT_TRUE(resolver.is_local(id));
}
