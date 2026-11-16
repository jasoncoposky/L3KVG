#include <gtest/gtest.h>
#include "L3KVG/PackedEdge.hpp"
#include "L3KVG/SIMDFilter.hpp"
#include <vector>
#include <random>
#include <algorithm>
#include <limits>
#include <cmath>

using namespace l3kvg;

// Helper to construct a PackedEdge concisely
static PackedEdge make_edge(uint64_t target_id, uint16_t label_id, float weight, bool active = true) {
    PackedEdge e{};
    e.target_id = target_id;
    e.label_id = label_id;
    e.weight = weight;
    e.flags = active ? PackedEdge::FLAG_ACTIVE : 0;
    return e;
}

TEST(SIMDFilterTest, FilterByLabelAndWeight) {
    // 64 edges, alternating labels, weight threshold
    constexpr size_t N = 64;
    std::vector<PackedEdge> edges(N);
    for (size_t i = 0; i < N; ++i) {
        uint16_t label = (i % 2 == 0) ? 1 : 2;
        float weight = static_cast<float>(i) * 0.1f; // 0.0 to 6.3
        edges[i] = make_edge(1000 + i, label, weight, true);
    }

    // Filter by label 1 and weight >= 3.0f
    // Even i: 0, 2, 4, ..., 62.
    // i * 0.1 >= 3.0 => i >= 30. Even i in [30..62] => 17 matches.
    std::vector<uint64_t> matched;
    SIMDFilter::filter_edges(edges, 1, 3.0f, matched);

    std::vector<uint64_t> expected;
    for (size_t i = 30; i < N; i += 2) {
        expected.push_back(1000 + i);
    }

    EXPECT_EQ(matched, expected);
    EXPECT_EQ(SIMDFilter::count_matching_edges(edges, 1, 3.0f), expected.size());

    // Filter by label 2 and weight >= 4.0f
    // Odd i: 1, 3, ..., 63.
    // i * 0.1 >= 4.0 => i >= 40. Odd i in [41..63] => 12 matches.
    matched.clear();
    expected.clear();
    SIMDFilter::filter_edges(edges, 2, 4.0f, matched);
    for (size_t i = 41; i < N; i += 2) {
        expected.push_back(1000 + i);
    }
    EXPECT_EQ(matched, expected);
    EXPECT_EQ(SIMDFilter::count_matching_edges(edges, 2, 4.0f), expected.size());
}

TEST(SIMDFilterTest, TombstoneFiltering) {
    // 64 edges: every 3rd edge is inactive (tombstone)
    constexpr size_t N = 64;
    std::vector<PackedEdge> edges(N);
    for (size_t i = 0; i < N; ++i) {
        bool active = (i % 3 != 0);
        edges[i] = make_edge(2000 + i, 1, 10.0f, active);
    }

    std::vector<uint64_t> matched;
    SIMDFilter::filter_edges(edges, 1, 1.0f, matched);

    std::vector<uint64_t> expected;
    for (size_t i = 0; i < N; ++i) {
        if (i % 3 != 0) {
            expected.push_back(2000 + i);
        }
    }
    EXPECT_EQ(matched, expected);
    EXPECT_EQ(SIMDFilter::count_matching_edges(edges, 1, 1.0f), expected.size());

    // All inactive edges
    std::vector<PackedEdge> all_inactive(32);
    for (size_t i = 0; i < 32; ++i) {
        all_inactive[i] = make_edge(3000 + i, 1, 100.0f, false);
    }
    matched.clear();
    SIMDFilter::filter_edges(all_inactive, 1, 0.0f, matched);
    EXPECT_TRUE(matched.empty());
    EXPECT_EQ(SIMDFilter::count_matching_edges(all_inactive, 1, 0.0f), 0);
}

TEST(SIMDFilterTest, WildcardLabelMatching) {
    // When target_label == 0, match all active edges regardless of label
    std::vector<PackedEdge> edges = {
        make_edge(1, 1, 5.0f, true),
        make_edge(2, 2, 10.0f, true),
        make_edge(3, 42, 1.0f, true),   // below min_weight (3.0f)
        make_edge(4, 99, 8.0f, false),  // tombstone, high weight
        make_edge(5, 500, 3.0f, true),  // exact min_weight boundary
        make_edge(6, 0, 7.0f, true),    // edge with label_id = 0
        make_edge(7, 10, 2.99f, true),  // just below threshold
        make_edge(8, 20, 15.0f, true)
    };

    std::vector<uint64_t> matched;
    SIMDFilter::filter_edges(edges, 0, 3.0f, matched);

    std::vector<uint64_t> expected = {1, 2, 5, 6, 8};
    EXPECT_EQ(matched, expected);
    EXPECT_EQ(SIMDFilter::count_matching_edges(edges, 0, 3.0f), expected.size());
}

TEST(SIMDFilterTest, EmptyAndSingleEdge) {
    // Empty span
    std::vector<PackedEdge> empty_vec;
    std::vector<uint64_t> matched;
    SIMDFilter::filter_edges(empty_vec, 1, 0.0f, matched);
    EXPECT_TRUE(matched.empty());
    EXPECT_EQ(SIMDFilter::count_matching_edges(empty_vec, 1, 0.0f), 0);

    bool cb_called = false;
    SIMDFilter::filter_edges_cb(empty_vec, 1, 0.0f, [&](const PackedEdge&) {
        cb_called = true;
    });
    EXPECT_FALSE(cb_called);

    // Single matching edge
    std::vector<PackedEdge> single_edge = { make_edge(42, 5, 2.5f, true) };
    SIMDFilter::filter_edges(single_edge, 5, 2.0f, matched);
    EXPECT_EQ(matched, std::vector<uint64_t>{42});
    EXPECT_EQ(SIMDFilter::count_matching_edges(single_edge, 5, 2.0f), 1);

    // Single edge: label mismatch
    matched.clear();
    SIMDFilter::filter_edges(single_edge, 6, 2.0f, matched);
    EXPECT_TRUE(matched.empty());
    EXPECT_EQ(SIMDFilter::count_matching_edges(single_edge, 6, 2.0f), 0);

    // Single edge: weight too low
    matched.clear();
    SIMDFilter::filter_edges(single_edge, 5, 3.0f, matched);
    EXPECT_TRUE(matched.empty());
    EXPECT_EQ(SIMDFilter::count_matching_edges(single_edge, 5, 3.0f), 0);

    // Single edge: tombstone
    single_edge[0].set_active(false);
    matched.clear();
    SIMDFilter::filter_edges(single_edge, 5, 2.0f, matched);
    EXPECT_TRUE(matched.empty());
    EXPECT_EQ(SIMDFilter::count_matching_edges(single_edge, 5, 2.0f), 0);

    // Spans of size 2 through 7 (scalar tail coverage)
    for (size_t len = 2; len <= 7; ++len) {
        std::vector<PackedEdge> sub(len);
        for (size_t j = 0; j < len; ++j) {
            sub[j] = make_edge(500 + j, 1, static_cast<float>(j), true);
        }
        matched.clear();
        SIMDFilter::filter_edges(sub, 1, 1.0f, matched);
        EXPECT_EQ(matched.size(), len - 1);
        EXPECT_EQ(SIMDFilter::count_matching_edges(sub, 1, 1.0f), len - 1);
    }
}

TEST(SIMDFilterTest, LargeBatchVectorization) {
    // 1,500 edges to test multiple vectorized chunks and non-multiple-of-8 tails
    constexpr size_t N = 1500;
    std::vector<PackedEdge> edges(N);
    std::mt19937 rng(42);
    std::uniform_int_distribution<uint16_t> label_dist(0, 5);
    std::uniform_real_distribution<float> weight_dist(-10.0f, 50.0f);
    std::bernoulli_distribution active_dist(0.8); // 80% active

    for (size_t i = 0; i < N; ++i) {
        edges[i] = make_edge(
            10000 + i,
            label_dist(rng),
            weight_dist(rng),
            active_dist(rng)
        );
    }

    // Test multiple query combinations
    struct TestCase {
        uint16_t label;
        float min_weight;
    };
    std::vector<TestCase> test_cases = {
        {0, 10.0f},   // wildcard label, positive weight
        {1, 5.0f},    // specific label, moderate weight
        {3, -5.0f},   // specific label, negative weight threshold
        {4, 100.0f},  // no edge should match
        {2, -20.0f},  // all active label-2 edges should match
        {0, -50.0f}   // all active edges match
    };

    for (const auto& tc : test_cases) {
        std::vector<uint64_t> ref_matched;
        for (const auto& e : edges) {
            bool act = e.is_active();
            bool lbl = (tc.label == 0) | (e.label_id == tc.label);
            bool wt = (e.weight >= tc.min_weight);
            if (act & lbl & wt) {
                ref_matched.push_back(e.target_id);
            }
        }

        std::vector<uint64_t> simd_matched;
        SIMDFilter::filter_edges(edges, tc.label, tc.min_weight, simd_matched);
        EXPECT_EQ(simd_matched, ref_matched)
            << "Mismatch for label=" << tc.label << " min_weight=" << tc.min_weight;

        size_t count = SIMDFilter::count_matching_edges(edges, tc.label, tc.min_weight);
        EXPECT_EQ(count, ref_matched.size())
            << "Count mismatch for label=" << tc.label << " min_weight=" << tc.min_weight;

        std::vector<uint64_t> cb_matched;
        SIMDFilter::filter_edges_cb(edges, tc.label, tc.min_weight, [&](const PackedEdge& e) {
            cb_matched.push_back(e.target_id);
        });
        EXPECT_EQ(cb_matched, ref_matched);
    }
}

TEST(SIMDFilterTest, CountMatchingEdges) {
    // Test count across various buffer sizes to check vector chunk and tail boundaries
    const std::vector<size_t> sizes = {0, 1, 7, 8, 9, 15, 16, 17, 31, 32, 33, 63, 64, 65, 128, 257, 1024};
    for (size_t sz : sizes) {
        std::vector<PackedEdge> edges(sz);
        for (size_t i = 0; i < sz; ++i) {
            edges[i] = make_edge(i, static_cast<uint16_t>(i % 3), static_cast<float>(i), (i % 5 != 0));
        }

        std::vector<uint64_t> matched;
        SIMDFilter::filter_edges(edges, 1, 10.0f, matched);
        size_t count = SIMDFilter::count_matching_edges(edges, 1, 10.0f);
        EXPECT_EQ(count, matched.size()) << "Size failed: " << sz;
    }
}

TEST(SIMDFilterTest, FilterEdgesCallback) {
    std::vector<PackedEdge> edges = {
        make_edge(101, 1, 2.0f, true),
        make_edge(102, 2, 4.0f, true),
        make_edge(103, 1, 6.0f, true),
        make_edge(104, 1, 8.0f, false) // tombstone
    };
    edges[0].hlc_timestamp = (1ULL << 40) + 42;
    edges[2].hlc_timestamp = (1ULL << 40) + 84;

    // 1. Callback taking `const PackedEdge&`
    std::vector<uint64_t> hlc_timestamps;
    SIMDFilter::filter_edges_cb(edges, 1, 1.0f, [&](const PackedEdge& edge) {
        hlc_timestamps.push_back(edge.hlc_timestamp);
    });
    EXPECT_EQ(hlc_timestamps, (std::vector<uint64_t>{(1ULL << 40) + 42, (1ULL << 40) + 84}));

    // 2. Callback taking `uint64_t`
    std::vector<uint64_t> target_ids;
    SIMDFilter::filter_edges_cb(edges, 1, 1.0f, [&](uint64_t tid) {
        target_ids.push_back(tid);
    });
    EXPECT_EQ(target_ids, (std::vector<uint64_t>{101, 103}));

    // 3. Append to non-empty matched_targets vector in filter_edges
    std::vector<uint64_t> existing = {999};
    SIMDFilter::filter_edges(edges, 1, 1.0f, existing);
    EXPECT_EQ(existing, (std::vector<uint64_t>{999, 101, 103}));
}

TEST(SIMDFilterTest, NegativeAndZeroWeights) {
    std::vector<PackedEdge> edges = {
        make_edge(1, 1, -10.0f, true),
        make_edge(2, 1, -5.0f, true),
        make_edge(3, 1, -2.5f, true),
        make_edge(4, 1, 0.0f, true),
        make_edge(5, 1, 5.0f, true)
    };

    // Filter min_weight = -5.0f: should match -5.0f, -2.5f, 0.0f, 5.0f
    std::vector<uint64_t> matched;
    SIMDFilter::filter_edges(edges, 1, -5.0f, matched);
    EXPECT_EQ(matched, (std::vector<uint64_t>{2, 3, 4, 5}));
    EXPECT_EQ(SIMDFilter::count_matching_edges(edges, 1, -5.0f), 4);

    // Filter min_weight = 0.0f: should match 0.0f, 5.0f
    matched.clear();
    SIMDFilter::filter_edges(edges, 1, 0.0f, matched);
    EXPECT_EQ(matched, (std::vector<uint64_t>{4, 5}));
    EXPECT_EQ(SIMDFilter::count_matching_edges(edges, 1, 0.0f), 2);
}

TEST(SIMDFilterTest, SpecialFloatingPointValues) {
    const float nan_val = std::numeric_limits<float>::quiet_NaN();
    const float inf_pos = std::numeric_limits<float>::infinity();
    const float inf_neg = -std::numeric_limits<float>::infinity();

    // 1. Scalar tail test (< 8 elements)
    std::vector<PackedEdge> small_edges = {
        make_edge(1, 1, nan_val, true),
        make_edge(2, 1, inf_pos, true),
        make_edge(3, 1, inf_neg, true),
        make_edge(4, 1, 10.0f, true),
        make_edge(5, 1, -10.0f, true),
        make_edge(6, 1, nan_val, false) // inactive NaN
    };

    // Filter min_weight = 0.0f:
    // NaN >= 0.0f -> false (ordered comparison returns false for NaN)
    // +Inf >= 0.0f -> true (edge 2)
    // -Inf >= 0.0f -> false
    // 10.0f >= 0.0f -> true (edge 4)
    // -10.0f >= 0.0f -> false
    std::vector<uint64_t> matched;
    SIMDFilter::filter_edges(small_edges, 1, 0.0f, matched);
    EXPECT_EQ(matched, (std::vector<uint64_t>{2, 4}));
    EXPECT_EQ(SIMDFilter::count_matching_edges(small_edges, 1, 0.0f), 2);

    // 2. Vectorized batch test (>= 16 elements to exercise 16-unroll and 8-unroll SIMD loops)
    // Construct 24 edges with alternating NaN, +Inf, -Inf, and finite numbers
    std::vector<PackedEdge> batch_edges(24);
    for (size_t i = 0; i < 24; ++i) {
        float wt = 0.0f;
        switch (i % 6) {
            case 0: wt = nan_val; break;
            case 1: wt = inf_pos; break;
            case 2: wt = inf_neg; break;
            case 3: wt = 100.0f; break;
            case 4: wt = -50.0f; break;
            case 5: wt = 0.0f; break;
        }
        // Inactive tombstone every 7th element
        bool active = (i % 7 != 0);
        batch_edges[i] = make_edge(100 + i, 1, wt, active);
    }

    // Verify _CMP_GE_OQ and scalar >= treat NaN as non-matching (ordered quiet comparison returns false for NaN)
    // and +Inf matching properly for min_weight = 0.0f
    std::vector<uint64_t> ref_matched;
    for (const auto& e : batch_edges) {
        if (e.is_active() && (e.label_id == 1) && (e.weight >= 0.0f)) {
            ref_matched.push_back(e.target_id);
        }
    }
    // Explicitly verify ref_matched does not include any NaN edges
    for (uint64_t id : ref_matched) {
        const auto& e = batch_edges[id - 100];
        EXPECT_FALSE(std::isnan(e.weight));
    }

    matched.clear();
    SIMDFilter::filter_edges(batch_edges, 1, 0.0f, matched);
    EXPECT_EQ(matched, ref_matched);
    EXPECT_EQ(SIMDFilter::count_matching_edges(batch_edges, 1, 0.0f), ref_matched.size());

    std::vector<uint64_t> cb_matched;
    SIMDFilter::filter_edges_cb(batch_edges, 1, 0.0f, [&](const PackedEdge& e) {
        cb_matched.push_back(e.target_id);
    });
    EXPECT_EQ(cb_matched, ref_matched);

    // 3. Test min_weight = -inf:
    // NaN >= -inf is false (NaN is unordered with respect to any float)
    // +inf >= -inf is true
    // -inf >= -inf is true
    // finite >= -inf is true
    std::vector<uint64_t> ref_min_inf;
    for (const auto& e : batch_edges) {
        if (e.is_active() && (e.label_id == 1) && (e.weight >= inf_neg)) {
            ref_min_inf.push_back(e.target_id);
        }
    }
    matched.clear();
    SIMDFilter::filter_edges(batch_edges, 1, inf_neg, matched);
    EXPECT_EQ(matched, ref_min_inf);
    EXPECT_EQ(SIMDFilter::count_matching_edges(batch_edges, 1, inf_neg), ref_min_inf.size());

    // 4. Test min_weight = +inf:
    // Only active edges with weight == +inf should match
    std::vector<uint64_t> ref_pos_inf;
    for (const auto& e : batch_edges) {
        if (e.is_active() && (e.label_id == 1) && (e.weight >= inf_pos)) {
            ref_pos_inf.push_back(e.target_id);
        }
    }
    matched.clear();
    SIMDFilter::filter_edges(batch_edges, 1, inf_pos, matched);
    EXPECT_EQ(matched, ref_pos_inf);
    EXPECT_EQ(SIMDFilter::count_matching_edges(batch_edges, 1, inf_pos), ref_pos_inf.size());

    // 5. Test min_weight = NaN:
    // Ordered comparison with NaN always produces false, so no edge matches
    matched.clear();
    SIMDFilter::filter_edges(batch_edges, 1, nan_val, matched);
    EXPECT_TRUE(matched.empty());
    EXPECT_EQ(SIMDFilter::count_matching_edges(batch_edges, 1, nan_val), 0);
}
