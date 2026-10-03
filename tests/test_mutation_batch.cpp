#include <gtest/gtest.h>
#include <L3KVG/MutationBatch.hpp>

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
