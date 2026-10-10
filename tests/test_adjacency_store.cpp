#include <gtest/gtest.h>
#include "L3KVG/AdjacencyStore.hpp"
#include <thread>
#include <vector>

TEST(AdjacencyStoreTest, StructSizeAndInlining) {
    EXPECT_EQ(sizeof(l3kvg::VertexRecord), 32);
    l3kvg::AdjacencyStore store;

    // Single edge should be inlined without page allocation
    store.add_edge(100, 200, 1, 1.0f, l3kvg::PackedEdge::FLAG_ACTIVE, 0, 1);
    EXPECT_EQ(store.get_out_degree(100), 1);
    EXPECT_EQ(store.get_in_degree(200), 1);

    auto out_span = store.get_outgoing_edges(100);
    ASSERT_EQ(out_span.size(), 1);
    EXPECT_EQ(out_span[0].target_id, 200);
    EXPECT_EQ(out_span[0].label_id, 1);
    EXPECT_TRUE(out_span[0].is_active());

    auto in_span = store.get_incoming_edges(200);
    ASSERT_EQ(in_span.size(), 1);
    EXPECT_EQ(in_span[0].target_id, 100);
}

TEST(AdjacencyStoreTest, PromotionToSlottedPage) {
    l3kvg::AdjacencyStore store;
    uint64_t src = 500;

    // Add 10 edges to trigger slotted page allocation
    for (uint64_t dst = 1; dst <= 10; ++dst) {
        store.add_edge(src, dst, 1, static_cast<float>(dst), l3kvg::PackedEdge::FLAG_ACTIVE, 0, static_cast<uint32_t>(dst));
    }

    EXPECT_EQ(store.get_out_degree(src), 10);
    auto edges = store.get_outgoing_edges(src);
    ASSERT_EQ(edges.size(), 10);
    for (size_t i = 0; i < 10; ++i) {
        EXPECT_EQ(edges[i].target_id, i + 1);
        EXPECT_FLOAT_EQ(edges[i].weight, static_cast<float>(i + 1));
    }
}

TEST(AdjacencyStoreTest, DeleteEdgeTombstone) {
    l3kvg::AdjacencyStore store;
    store.add_edge(10, 20, 1, 1.0f, l3kvg::PackedEdge::FLAG_ACTIVE, 0, 1);
    store.add_edge(10, 30, 2, 2.0f, l3kvg::PackedEdge::FLAG_ACTIVE, 0, 2);

    EXPECT_EQ(store.get_out_degree(10), 2);
    bool deleted = store.del_edge(10, 20, 1);
    EXPECT_TRUE(deleted);

    auto edges = store.get_outgoing_edges(10);
    int active_count = 0;
    for (const auto& e : edges) {
        if (e.is_active()) active_count++;
    }
    EXPECT_EQ(active_count, 1);

    // Verify incoming edge on dst 20 was also tombstoned
    auto in_edges = store.get_incoming_edges(20);
    ASSERT_EQ(in_edges.size(), 1);
    EXPECT_FALSE(in_edges[0].is_active());

    // Deleting again should return false (already deleted)
    EXPECT_FALSE(store.del_edge(10, 20, 1));
}

TEST(AdjacencyStoreTest, ConcurrentAddAndRead) {
    l3kvg::AdjacencyStore store;
    std::vector<std::thread> writers;
    for (int t = 0; t < 4; ++t) {
        writers.emplace_back([&, t]() {
            for (uint64_t i = 0; i < 100; ++i) {
                uint64_t src = 1000 + t;
                uint64_t dst = 2000 + i;
                store.add_edge(src, dst, 1, 1.0f);
            }
        });
    }
    for (auto& w : writers) w.join();

    for (int t = 0; t < 4; ++t) {
        uint64_t src = 1000 + t;
        EXPECT_EQ(store.get_out_degree(src), 100);
        auto edges = store.get_outgoing_edges(src);
        EXPECT_EQ(edges.size(), 100);
    }
}

TEST(AdjacencyStoreTest, NonexistentVertexQueries) {
    l3kvg::AdjacencyStore store;
    EXPECT_EQ(store.get_out_degree(9999), 0);
    EXPECT_EQ(store.get_in_degree(9999), 0);
    EXPECT_TRUE(store.get_outgoing_edges(9999).empty());
    EXPECT_TRUE(store.get_incoming_edges(9999).empty());
    EXPECT_FALSE(store.del_edge(9999, 8888, 1));
}

TEST(AdjacencyStoreTest, InlinedEdgeTombstoneAndReaddition) {
    l3kvg::AdjacencyStore store;
    store.add_edge(1, 2, 10, 5.0f);
    EXPECT_EQ(store.get_out_degree(1), 1);
    EXPECT_EQ(store.get_in_degree(2), 1);

    EXPECT_TRUE(store.del_edge(1, 2, 10));
    auto out = store.get_outgoing_edges(1);
    ASSERT_EQ(out.size(), 1);
    EXPECT_FALSE(out[0].is_active());

    auto in = store.get_incoming_edges(2);
    ASSERT_EQ(in.size(), 1);
    EXPECT_FALSE(in[0].is_active());

    // Promotion after tombstone
    store.add_edge(1, 3, 20, 7.5f);
    EXPECT_EQ(store.get_out_degree(1), 2);
    out = store.get_outgoing_edges(1);
    ASSERT_EQ(out.size(), 2);
    EXPECT_FALSE(out[0].is_active());
    EXPECT_TRUE(out[1].is_active());
    EXPECT_EQ(out[1].target_id, 3);
}
