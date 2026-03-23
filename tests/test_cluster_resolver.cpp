#include <gtest/gtest.h>
#include "L3KVG/ClusterResolver.hpp"
#include <lite3/ring.hpp>
#include <memory>

TEST(ClusterResolverTest, BasicMapping) {
  auto ring = std::make_shared<lite3::ConsistentHash>(100);
  ring->add_node(1);
  ring->add_node(2);
  ring->add_node(3);

  l3kvg::ClusterResolver resolver(ring, 1);

  EXPECT_EQ(resolver.get_local_node_id(), 1);

  // Check some vertex assignments
  lite3::NodeID owner1 = resolver.get_node_owner("vertex_A");
  lite3::NodeID owner2 = resolver.get_node_owner("vertex_B");
  
  EXPECT_TRUE(owner1 >= 1 && owner1 <= 3);
  EXPECT_TRUE(owner2 >= 1 && owner2 <= 3);

  // Same vertex should always return same owner
  EXPECT_EQ(resolver.get_node_owner("vertex_A"), owner1);
}

TEST(ClusterResolverTest, UnshardedFallback) {
  auto empty_ring = std::make_shared<lite3::ConsistentHash>(100);
  l3kvg::ClusterResolver resolver(empty_ring, 42);

  // Should fallback to local node id
  EXPECT_EQ(resolver.get_node_owner("any_vertex"), 42);
  EXPECT_TRUE(resolver.is_local("any_vertex"));
}
