#include <gtest/gtest.h>
#include "L3KVG/AdjacencyStore.hpp"
#include <thread>
#include <vector>
#include <atomic>

TEST(AdjacencyStoreTest, StructSizeAndInlining) {
    EXPECT_EQ(sizeof(l3kvg::VertexRecord), 32);
    EXPECT_EQ(alignof(l3kvg::VertexRecord), 8);
    EXPECT_EQ(sizeof(l3kvg::AdjacencyPage), 4096);
    EXPECT_EQ(alignof(l3kvg::AdjacencyPage), 64);

    l3kvg::AdjacencyStore store;

    // Single edge should be inlined without page allocation
    store.add_edge(10, 20, 1, 1.0f, l3kvg::PackedEdge::FLAG_ACTIVE, 0, 1);
    EXPECT_EQ(store.get_out_page_id(10), 0);
    EXPECT_EQ(store.get_out_degree(10), 1);
    EXPECT_EQ(store.get_in_degree(20), 1);

    auto out_span = store.get_outgoing_edges(10);
    ASSERT_EQ(out_span.size(), 1);
    EXPECT_EQ(out_span[0].target_id, 20);
    EXPECT_EQ(out_span[0].label_id, 1);
    EXPECT_TRUE(out_span[0].is_active());

    auto in_span = store.get_incoming_edges(20);
    ASSERT_EQ(in_span.size(), 1);
    EXPECT_EQ(in_span[0].target_id, 10);
    EXPECT_TRUE(in_span[0].is_active());
}

TEST(AdjacencyStoreTest, PromotionToSlottedPage) {
    l3kvg::AdjacencyStore store;
    uint64_t src = 10;

    store.add_edge(src, 20, 1);
    EXPECT_EQ(store.get_out_page_id(src), 0);

    // 2nd edge causes promotion to slotted page
    store.add_edge(src, 30, 2);
    uint32_t pid = store.get_out_page_id(src);
    EXPECT_GT(pid, 0);

    const auto* page = store.get_page(pid);
    ASSERT_NE(page, nullptr);
    EXPECT_EQ(page->count, 2);
    EXPECT_EQ(store.get_out_degree(src), 2);

    auto edges = store.get_outgoing_edges(src);
    ASSERT_EQ(edges.size(), 2);
    EXPECT_EQ(edges[0].target_id, 20);
    EXPECT_EQ(edges[0].label_id, 1);
    EXPECT_EQ(edges[1].target_id, 30);
    EXPECT_EQ(edges[1].label_id, 2);
}

TEST(AdjacencyStoreTest, CapacityAndChaining) {
    l3kvg::AdjacencyStore store;
    uint64_t src = 100;

    // Add 200 edges to node 100
    for (uint64_t i = 1; i <= 200; ++i) {
        store.add_edge(src, 1000 + i, 1, static_cast<float>(i));
    }

    EXPECT_EQ(store.get_out_degree(src), 200);

    uint32_t first_pid = store.get_out_page_id(src);
    EXPECT_GT(first_pid, 0);

    const auto* page1 = store.get_page(first_pid);
    ASSERT_NE(page1, nullptr);
    EXPECT_EQ(page1->count, 170);
    EXPECT_GT(page1->next_page_id, 0);

    const auto* page2 = store.get_page(page1->next_page_id);
    ASSERT_NE(page2, nullptr);
    EXPECT_EQ(page2->count, 30);
    EXPECT_EQ(page2->next_page_id, 0);

    EXPECT_EQ(page1->edges[0].target_id, 1001);
    EXPECT_EQ(page1->edges[169].target_id, 1170);
    EXPECT_EQ(page2->edges[0].target_id, 1171);
    EXPECT_EQ(page2->edges[29].target_id, 1200);
}

TEST(AdjacencyStoreTest, DeleteEdgeTombstone) {
    l3kvg::AdjacencyStore store;
    store.add_edge(10, 20, 1, 1.0f, l3kvg::PackedEdge::FLAG_ACTIVE, 0, 1);
    store.add_edge(10, 30, 2, 2.0f, l3kvg::PackedEdge::FLAG_ACTIVE, 0, 2);

    EXPECT_EQ(store.get_out_degree(10), 2);
    bool deleted = store.del_edge(10, 20, 1);
    EXPECT_TRUE(deleted);
    EXPECT_EQ(store.get_out_degree(10), 1);
    EXPECT_EQ(store.get_in_degree(20), 0);

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

TEST(AdjacencyStoreTest, ConcurrentReadersAndWritersSimultaneous) {
    l3kvg::AdjacencyStore store;
    std::atomic<bool> stop{false};
    std::atomic<size_t> read_ops{0};

    // 4 writers
    std::vector<std::thread> writers;
    for (int t = 0; t < 4; ++t) {
        writers.emplace_back([&, t]() {
            for (uint64_t i = 0; i < 200; ++i) {
                uint64_t src = 5000 + t;
                uint64_t dst = 6000 + i;
                store.add_edge(src, dst, 1, static_cast<float>(i));
            }
        });
    }

    // 4 readers running simultaneously while writers write
    std::vector<std::thread> readers;
    for (int r = 0; r < 4; ++r) {
        readers.emplace_back([&, r]() {
            while (!stop.load(std::memory_order_relaxed)) {
                for (int t = 0; t < 4; ++t) {
                    uint64_t src = 5000 + t;
                    auto edges = store.get_outgoing_edges(src);
                    if (!edges.empty()) {
                        EXPECT_GT(edges[0].target_id, 0);
                    }
                    size_t out_deg = store.get_out_degree(src);
                    (void)out_deg;
                    uint64_t dst = 6000 + (r * 10);
                    size_t in_deg = store.get_in_degree(dst);
                    (void)in_deg;
                    read_ops.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }

    for (auto& w : writers) w.join();
    stop.store(true, std::memory_order_relaxed);
    for (auto& r : readers) r.join();

    EXPECT_GT(read_ops.load(), 0);
    for (int t = 0; t < 4; ++t) {
        uint64_t src = 5000 + t;
        EXPECT_EQ(store.get_out_degree(src), 200);
        uint32_t p1_id = store.get_out_page_id(src);
        EXPECT_GT(p1_id, 0);
        const auto* p1 = store.get_page(p1_id);
        ASSERT_NE(p1, nullptr);
        EXPECT_EQ(p1->count, 170);
        EXPECT_GT(p1->next_page_id, 0);
        const auto* p2 = store.get_page(p1->next_page_id);
        ASSERT_NE(p2, nullptr);
        EXPECT_EQ(p2->count, 30);
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
    EXPECT_EQ(store.get_out_degree(1), 0);
    EXPECT_EQ(store.get_in_degree(2), 0);

    auto out = store.get_outgoing_edges(1);
    EXPECT_TRUE(out.empty());

    auto in = store.get_incoming_edges(2);
    ASSERT_EQ(in.size(), 1);
    EXPECT_FALSE(in[0].is_active());

    // Readdition of an edge to vertex 1
    store.add_edge(1, 3, 20, 7.5f);
    EXPECT_EQ(store.get_out_degree(1), 1);
    out = store.get_outgoing_edges(1);
    ASSERT_EQ(out.size(), 1);
    EXPECT_TRUE(out[0].is_active());
    EXPECT_EQ(out[0].target_id, 3);
}

TEST(AdjacencyStoreTest, ForEachOutgoingEdgeChained) {
    l3kvg::AdjacencyStore store;
    uint64_t src = 700;
    for (uint64_t i = 1; i <= 200; ++i) {
        store.add_edge(src, 2000 + i, 1, static_cast<float>(i));
    }
    EXPECT_EQ(store.get_out_degree(src), 200);

    size_t count = 0;
    std::vector<uint64_t> visited_targets;
    store.for_each_outgoing_edge(src, [&](const l3kvg::PackedEdge& edge) {
        visited_targets.push_back(edge.target_id);
        count++;
    });

    EXPECT_EQ(count, 200);
    ASSERT_EQ(visited_targets.size(), 200);
    for (size_t i = 0; i < 200; ++i) {
        EXPECT_EQ(visited_targets[i], 2000 + i + 1);
    }
}

TEST(AdjacencyStoreTest, OutgoingAndIncomingEdgesSnapshot) {
    l3kvg::AdjacencyStore store;
    uint64_t src = 800;
    for (uint64_t i = 1; i <= 200; ++i) {
        store.add_edge(src, 3000 + i, 1, static_cast<float>(i));
    }

    auto snapshot = store.get_outgoing_edges_snapshot(src);
    EXPECT_EQ(snapshot.size(), 200);
    for (size_t i = 0; i < 200; ++i) {
        EXPECT_EQ(snapshot[i].target_id, 3000 + i + 1);
        EXPECT_TRUE(snapshot[i].is_active());
    }

    // Verify incoming edges snapshot for a destination node
    auto in_snapshot = store.get_incoming_edges_snapshot(3001);
    ASSERT_EQ(in_snapshot.size(), 1);
    EXPECT_EQ(in_snapshot[0].target_id, src);
    EXPECT_TRUE(in_snapshot[0].is_active());
}

TEST(AdjacencyStoreTest, DeleteEdgeInSecondPageDecrementsDegree) {
    l3kvg::AdjacencyStore store;
    uint64_t src = 900;
    for (uint64_t i = 1; i <= 200; ++i) {
        store.add_edge(src, 4000 + i, 1, static_cast<float>(i));
    }

    EXPECT_EQ(store.get_out_degree(src), 200);

    // Edge 180 is in the second page of the chain (since page capacity is 170)
    uint64_t target_180 = 4180;
    bool deleted = store.del_edge(src, target_180, 1);
    EXPECT_TRUE(deleted);
    EXPECT_EQ(store.get_out_degree(src), 199);
    EXPECT_EQ(store.get_in_degree(target_180), 0);

    // Verify snapshot reflects only active edges (199 edges)
    auto snapshot = store.get_outgoing_edges_snapshot(src);
    EXPECT_EQ(snapshot.size(), 199);
    for (const auto& e : snapshot) {
        EXPECT_NE(e.target_id, target_180);
    }

    // Verify for_each_outgoing_edge visits all 200 edges including the tombstoned one
    size_t active_count = 0;
    size_t total_count = 0;
    store.for_each_outgoing_edge(src, [&](const l3kvg::PackedEdge& edge) {
        total_count++;
        if (edge.is_active()) {
            active_count++;
        } else {
            EXPECT_EQ(edge.target_id, target_180);
        }
    });
    EXPECT_EQ(total_count, 200);
    EXPECT_EQ(active_count, 199);

    // Verify for_each_incoming_edge on target_180 visits the tombstoned incoming edge
    size_t in_count = 0;
    store.for_each_incoming_edge(target_180, [&](const l3kvg::PackedEdge& edge) {
        in_count++;
        EXPECT_EQ(edge.target_id, src);
        EXPECT_FALSE(edge.is_active());
    });
    EXPECT_EQ(in_count, 1);
}
