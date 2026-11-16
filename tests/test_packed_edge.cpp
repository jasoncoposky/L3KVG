#include <gtest/gtest.h>
#include "L3KVG/PackedEdge.hpp"
#include "L3KVG/LabelRegistry.hpp"
#include <thread>
#include <vector>

TEST(PackedEdgeTest, StructSizeAndAlignment) {
    EXPECT_EQ(sizeof(l3kvg::PackedEdge), 24);
    EXPECT_EQ(alignof(l3kvg::PackedEdge), 8);
    l3kvg::PackedEdge edge{};
    edge.target_id = 0x123456789ABCDEF0ULL;
    edge.label_id = 42;
    edge.flags = l3kvg::PackedEdge::FLAG_ACTIVE | l3kvg::PackedEdge::FLAG_DIRECTED;
    edge.weight = 1.5f;
    edge.hlc_timestamp = 0x0011223344556677ULL;

    EXPECT_TRUE(edge.is_active());
    EXPECT_FALSE(edge.has_properties());
    EXPECT_TRUE(edge.is_directed());

    edge.set_has_properties(true);
    EXPECT_TRUE(edge.has_properties());
    edge.set_has_properties(false);
    EXPECT_FALSE(edge.has_properties());

    edge.set_directed(false);
    EXPECT_FALSE(edge.is_directed());
    edge.set_directed(true);
    EXPECT_TRUE(edge.is_directed());

    edge.set_active(false);
    EXPECT_FALSE(edge.is_active());
    edge.set_active(true);
    EXPECT_TRUE(edge.is_active());
}

TEST(PackedEdgeTest, EqualityOperator) {
    l3kvg::PackedEdge e1{
        .target_id = 12345,
        .label_id = 1,
        .flags = l3kvg::PackedEdge::FLAG_ACTIVE | l3kvg::PackedEdge::FLAG_DIRECTED,
        .weight = 2.5f,
        .hlc_timestamp = 0x123456789ABCDEF0ULL
    };
    l3kvg::PackedEdge e2 = e1;
    EXPECT_EQ(e1, e2);

    e2.target_id = 12346;
    EXPECT_NE(e1, e2);
    e2 = e1;
    e2.label_id = 2;
    EXPECT_NE(e1, e2);
    e2 = e1;
    e2.flags = 0;
    EXPECT_NE(e1, e2);
    e2 = e1;
    e2.weight = 3.0f;
    EXPECT_NE(e1, e2);
    e2 = e1;
    e2.hlc_timestamp = 0x123456789ABCDEF1ULL;
    EXPECT_NE(e1, e2);
}

TEST(LabelRegistryTest, BidirectionalMapping) {
    l3kvg::LabelRegistry registry;
    EXPECT_EQ(registry.size(), 0);
    EXPECT_EQ(registry.get_label(0), "");
    EXPECT_EQ(registry.get_or_register(""), 0);

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
    EXPECT_EQ(registry.size(), 100);
}

TEST(LabelRegistryTest, CapacityOverflow) {
    l3kvg::LabelRegistry registry;
    // Register up to maximum 65535 labels
    for (uint32_t i = 1; i <= 65535; ++i) {
        // Fast generated label
        char buf[16];
        snprintf(buf, sizeof(buf), "L%x", i);
        uint16_t id = registry.get_or_register(buf);
        ASSERT_EQ(id, i);
    }
    EXPECT_EQ(registry.size(), 65535);

    // 65536th label must throw std::overflow_error
    EXPECT_THROW(registry.get_or_register("OVERFLOW_LABEL"), std::overflow_error);
}
