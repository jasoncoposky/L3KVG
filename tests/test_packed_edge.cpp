#include <gtest/gtest.h>
#include "L3KVG/PackedEdge.hpp"
#include "L3KVG/LabelRegistry.hpp"
#include <thread>
#include <vector>

TEST(PackedEdgeTest, StructSizeAndAlignment) {
    EXPECT_EQ(sizeof(l3kvg::PackedEdge), 24);
    l3kvg::PackedEdge edge{};
    edge.target_id = 0x123456789ABCDEF0ULL;
    edge.label_id = 42;
    edge.flags = l3kvg::PackedEdge::FLAG_ACTIVE | l3kvg::PackedEdge::FLAG_DIRECTED;
    edge.weight = 1.5f;
    edge.prop_index = 100;
    edge.hlc_logical = 7;

    EXPECT_TRUE(edge.is_active());
    EXPECT_FALSE(edge.has_properties());
    edge.flags |= l3kvg::PackedEdge::FLAG_HAS_PROPERTIES;
    EXPECT_TRUE(edge.has_properties());

    edge.set_active(false);
    EXPECT_FALSE(edge.is_active());
    edge.set_active(true);
    EXPECT_TRUE(edge.is_active());
}

TEST(LabelRegistryTest, BidirectionalMapping) {
    l3kvg::LabelRegistry registry;
    EXPECT_EQ(registry.size(), 0);
    EXPECT_EQ(registry.get_label(0), "");

    uint16_t id_contains = registry.get_or_register("CONTAINS");
    uint16_t id_replica = registry.get_or_register("HAS_REPLICA");
    uint16_t id_contains_again = registry.get_or_register("CONTAINS");

    EXPECT_EQ(id_contains, id_contains_again);
    EXPECT_NE(id_contains, id_replica);
    EXPECT_EQ(registry.get_label(id_contains), "CONTAINS");
    EXPECT_EQ(registry.get_label(id_replica), "HAS_REPLICA");
    EXPECT_EQ(registry.get_label(999), ""); // Unknown ID returns empty
    EXPECT_EQ(registry.size(), 2);
}

TEST(LabelRegistryTest, ConcurrentRegistration) {
    l3kvg::LabelRegistry registry;
    std::vector<std::thread> threads;
    for (int t = 0; t < 8; ++t) {
        threads.emplace_back([&, t]() {
            for (int i = 0; i < 100; ++i) {
                std::string label = "LABEL_" + std::to_string(i);
                uint16_t id = registry.get_or_register(label);
                EXPECT_GT(id, 0);
                EXPECT_EQ(registry.get_label(id), label);
            }
        });
    }
    for (auto& th : threads) th.join();
    EXPECT_GE(registry.size(), 100);
}
