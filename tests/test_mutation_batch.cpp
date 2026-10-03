#include <gtest/gtest.h>
#include <L3KVG/MutationBatch.hpp>
#include <L3KVG/Engine.hpp>
#include <L3KVG/Node.hpp>
#include "engine/store.hpp"
#include <filesystem>

TEST(MutationBatchTest, EmptyBatch) {
    l3kvg::MutationBatch batch;
    EXPECT_TRUE(batch.empty());
    EXPECT_EQ(batch.size(), 0);
}

TEST(MutationBatchTest, PackHeterogeneousMutations) {
    l3kvg::MutationBatch batch;
    batch.put_node(0x1000, "{\"n\":\"file1\"}");
    batch.add_index("idx:DataObject:n:file1", "1000");
    batch.add_edge(0x2000, "CONTAINS", 1.0, 0x1000, "{}");
    batch.del_raw("tmp:marker");

    EXPECT_FALSE(batch.empty());
    EXPECT_EQ(batch.size(), 4);

    auto item0 = batch.get(0);
    EXPECT_EQ(item0.op, l3kvg::MutationOp::PutNode);
    EXPECT_EQ(item0.src, 0x1000);
    EXPECT_EQ(item0.value, "{\"n\":\"file1\"}");

    auto item1 = batch.get(1);
    EXPECT_EQ(item1.op, l3kvg::MutationOp::PutRaw);
    EXPECT_EQ(item1.key, "idx:DataObject:n:file1");
    EXPECT_EQ(item1.value, "1000");

    auto item2 = batch.get(2);
    EXPECT_EQ(item2.op, l3kvg::MutationOp::AddEdge);
    EXPECT_EQ(item2.src, 0x2000);
    EXPECT_EQ(item2.dst, 0x1000);
    EXPECT_EQ(item2.label, "CONTAINS");
    EXPECT_DOUBLE_EQ(item2.weight, 1.0);
    EXPECT_EQ(item2.value, "{}");

    auto item3 = batch.get(3);
    EXPECT_EQ(item3.op, l3kvg::MutationOp::DelRaw);
    EXPECT_EQ(item3.key, "tmp:marker");
}

TEST(MutationBatchTest, EngineApplyBatch) {
    std::string db_path = "test_apply_batch_db";
    std::filesystem::remove_all(db_path);

    {
        l3kvg::Engine engine(db_path, 1);

        l3kvg::MutationBatch batch;
        batch.put_node(0x5001, "{\"n\":\"batch_file\"}");
        batch.put_raw("idx:test:key", "val123");
        batch.add_edge(0x6001, "CONTAINS", 1.0, 0x5001, "{}");

        bool ok = engine.apply_batch(batch.get_buffer());
        EXPECT_TRUE(ok);

        auto node = engine.get_node(0x5001);
        ASSERT_NE(node, nullptr);
        EXPECT_EQ(node->get_attribute_as_string("n"), "batch_file");

        auto raw_val = engine.get_store()->get("idx:test:key");
        EXPECT_EQ(std::string(reinterpret_cast<const char*>(raw_val.data()), raw_val.size()), "val123");

        auto node6001 = engine.get_node(0x6001);
        ASSERT_NE(node6001, nullptr);
        auto neighbors = node6001->get_neighbors("CONTAINS");
        ASSERT_EQ(neighbors.size(), size_t(1));
        EXPECT_EQ(neighbors[0], 0x5001);
    }

    std::filesystem::remove_all(db_path);
}
