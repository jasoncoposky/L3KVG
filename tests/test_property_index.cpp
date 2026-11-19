#include <gtest/gtest.h>

#include "L3KVG/Engine.hpp"
#include "L3KVG/PropertyIndex.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <unistd.h>
#include <vector>

using namespace l3kvg;

namespace {
std::string make_temp_dir(const char* name) {
    auto p = std::filesystem::temp_directory_path() / (std::string(name) + "_" + std::to_string(::getpid()));
    std::filesystem::remove_all(p);
    std::filesystem::create_directories(p);
    return p.string();
}
} // namespace

TEST(PropertyIndexTest, SortableDoubleOrderPreservation) {
    std::vector<double> vals = {
        -1e12, -1000.5, -1.0, -0.0001, -0.0, 0.0, 0.0001, 1.0, 42.0, 1024.0, 1e9, 1e15
    };
    std::vector<std::string> encoded;
    for (double v : vals) {
        std::string h = PropertyIndex::encode_sortable_double(v);
        ASSERT_EQ(h.size(), 16U);
        EXPECT_DOUBLE_EQ(PropertyIndex::decode_sortable_double(h), v == 0.0 ? 0.0 : v);
        encoded.push_back(std::move(h));
    }
    for (size_t i = 1; i < encoded.size(); ++i) {
        if (vals[i - 1] == 0.0 && vals[i] == 0.0) {
            EXPECT_EQ(encoded[i - 1], encoded[i]);
        } else {
            EXPECT_LT(encoded[i - 1], encoded[i])
                << "Order mismatch between " << vals[i - 1] << " and " << vals[i];
        }
    }
}

TEST(PropertyIndexTest, ExactPrefixUnitNumericRangeAndMergedScopes) {
    std::string dir = make_temp_dir("test_l3kvg_pidx");
    {
        Engine engine(dir);
        auto idx = engine.property_index();

        // Populate 2,000 entities in base_scope with numeric `score`, string `tag`, and unit `mg`
        for (int i = 0; i < 2000; ++i) {
            std::string ent = "doc_" + std::to_string(i);
            idx.put("commit_a", "score", std::to_string(i - 500), ent, (i % 2 == 0) ? "pts" : "mg");
            idx.put("commit_a", "tag", (i < 10) ? "alpha_v1" : "beta_v2", ent);
        }

        // 1. Exact string match
        auto eq = idx.query_eq("commit_a", "tag", "alpha_v1", 100);
        EXPECT_EQ(eq.size(), 10U);

        // 2. Prefix match
        auto pfx = idx.query_prefix("commit_a", "tag", "alpha_", 100);
        EXPECT_EQ(pfx.size(), 10U);

        // 3. Unit match
        auto u = idx.query_unit("commit_a", "score", "mg", 50);
        EXPECT_EQ(u.size(), 50U);

        // 4. O(log N + K) numeric range query: -10 <= score <= 10 (21 docs: i=490..510)
        auto t0 = std::chrono::steady_clock::now();
        auto rng = idx.query_numeric_range("commit_a", "score", -10.0, true, 10.0, true, 100);
        auto t1 = std::chrono::steady_clock::now();
        EXPECT_EQ(rng.size(), 21U);
        auto us = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
        std::cout << "[PropertyIndex] Range [-10, 10] out of 2,000 docs: " << rng.size()
                  << " hits in " << us << " us\n";

        // Exclusive bounds: -10 < score < 10 -> 19 docs
        auto rng_ex = idx.query_numeric_range("commit_a", "score", -10.0, false, 10.0, false, 100);
        EXPECT_EQ(rng_ex.size(), 19U);

        // 5. Merged base + overlay - tombstones
        idx.put_tombstone("ovl_7", "score", "0", "doc_500");
        idx.put("ovl_7", "score", "5", "doc_new", "pts");

        PropertyQuery pq;
        pq.attr = "score";
        pq.op = PropertyOp::Between;
        pq.num_min = 0.0;
        pq.num_max = 5.0;
        auto merged = idx.query_merged("commit_a", "ovl_7", pq, 100);
        // Base had 6 docs (500..505), minus doc_500, plus doc_new => 6 docs
        EXPECT_EQ(merged.size(), 6U);
        EXPECT_EQ(std::find(merged.begin(), merged.end(), "doc_500"), merged.end());
        EXPECT_NE(std::find(merged.begin(), merged.end(), "doc_new"), merged.end());
    }
    std::filesystem::remove_all(dir);
}
