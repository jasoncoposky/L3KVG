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

TEST(MutationBatchTest, PutNodeBufferZeroCopy) {
    lite3cpp::Buffer node_buf(512);
    node_buf.init_object();
    node_buf.set_str(0, "n", "zero_copy_node");
    node_buf.set_i64(0, "id", 42);

    l3kvg::MutationBatch batch;
    batch.put_node(0x7001, node_buf);
    ASSERT_EQ(batch.size(), 1u);

    auto item = batch.get(0);
    EXPECT_EQ(item.op, l3kvg::MutationOp::PutNode);
    EXPECT_EQ(item.src, 0x7001u);
    EXPECT_EQ(item.value.size(), node_buf.size());

    lite3cpp::Buffer roundtrip(reinterpret_cast<const uint8_t*>(item.value.data()), item.value.size());
    EXPECT_EQ(roundtrip.get_str(0, "n"), "zero_copy_node");
    EXPECT_EQ(roundtrip.get_i64(0, "id"), 42);
}

TEST(MutationBatchTest, FeaturelessAddEdgeSkipsKVWrites) {
    std::string db_path = "test_batch_featureless_edge_db";
    std::filesystem::remove_all(db_path);

    {
        l3kvg::Engine engine(db_path, 1);

        l3kvg::MutationBatch batch;
        // Default payload ("") and explicit "{}" must both take featureless fast-path
        batch.add_edge(10, "CONTAINS", 1.0, 20);
        batch.add_edge(10, "CONTAINS_EMPTY_JSON", 1.0, 21, "{}");

        ASSERT_TRUE(engine.apply_batch(batch.get_buffer()));

        // Graph topology traversal works cleanly via AdjacencyStore
        auto n10 = engine.get_node(10);
        ASSERT_NE(n10, nullptr);
        auto out_contains = n10->get_neighbors("CONTAINS");
        ASSERT_EQ(out_contains.size(), 1u);
        EXPECT_EQ(out_contains[0], 20u);

        auto out_empty_json = n10->get_neighbors("CONTAINS_EMPTY_JSON");
        ASSERT_EQ(out_empty_json.size(), 1u);
        EXPECT_EQ(out_empty_json[0], 21u);

        // Zero e:out: or e:in: keys written in L3KV store
        auto e_out_keys = engine.get_store()->get_prefix_keys_all_shards("e:out:", "", 100);
        auto e_in_keys  = engine.get_store()->get_prefix_keys_all_shards("e:in:", "", 100);
        EXPECT_TRUE(e_out_keys.empty());
        EXPECT_TRUE(e_in_keys.empty());
    }

    std::filesystem::remove_all(db_path);
}

TEST(MutationBatchTest, PropertyBearingAddEdgeGetAttributeParity) {
    std::string db_path = "test_batch_prop_edge_parity_db";
    std::filesystem::remove_all(db_path);

    {
        l3kvg::Engine engine(db_path, 1);

        l3kvg::MutationBatch batch;
        batch.add_edge(10, "REL", 1.0, 30, R"({"role":"admin","rank":7})");

        ASSERT_TRUE(engine.apply_batch(batch.get_buffer()));

        auto node10 = engine.get_node(10);
        ASSERT_NE(node10, nullptr);
        auto edges = node10->get_edges("REL");
        ASSERT_EQ(edges.size(), 1u);

        EXPECT_NO_THROW({
            EXPECT_EQ(edges[0]->get_attribute<std::string>("role"), "admin");
            EXPECT_EQ(edges[0]->get_attribute<int64_t>("rank"), 7);
        });
    }

    std::filesystem::remove_all(db_path);
}

TEST(MutationBatchTest, IncNodeIntAtomicAndSameBatchAccumulation) {
    std::string db_path = "test_batch_inc_node_int_db";
    std::filesystem::remove_all(db_path);

    const uint64_t resc_id = 0x9001;
    const uint64_t str_resc_id = 0x9002;
    const uint64_t staged_resc_id = 0x9003;

    {
        l3kvg::Engine engine(db_path, 1);
        engine.put_node(resc_id, R"({"n":"demoResc"})");
        engine.put_node(str_resc_id, R"({"n":"strResc","obj_count":"100"})");

        // Multiple inc_node_int ops on the same node within a single MutationBatch
        l3kvg::MutationBatch batch;
        batch.inc_node_int(resc_id, "obj_count", 5);
        batch.inc_node_int(resc_id, "obj_count", -2);

        // Stringified integer field preserves string type while updating value
        batch.inc_node_int(str_resc_id, "obj_count", -30);
        batch.inc_node_int(str_resc_id, "obj_count", 12);

        // Staged put_node + inc_node_int in the same batch
        batch.put_node(staged_resc_id, R"({"n":"stagedResc","obj_count":10})");
        batch.inc_node_int(staged_resc_id, "obj_count", 4);

        ASSERT_TRUE(engine.apply_batch(batch.get_buffer()));

        auto n_resc = engine.get_node(resc_id);
        ASSERT_NE(n_resc, nullptr);
        EXPECT_EQ(n_resc->get_attribute<std::string>("n"), "demoResc");
        EXPECT_EQ(n_resc->get_attribute_type("obj_count"), lite3cpp::Type::Int64);
        EXPECT_EQ(n_resc->get_attribute<int64_t>("obj_count"), 3);

        auto n_str = engine.get_node(str_resc_id);
        ASSERT_NE(n_str, nullptr);
        EXPECT_EQ(n_str->get_attribute_type("obj_count"), lite3cpp::Type::String);
        EXPECT_EQ(n_str->get_attribute_as_string("obj_count"), "82");
        EXPECT_EQ(n_str->get_attribute<int64_t>("obj_count"), 82);

        auto n_staged = engine.get_node(staged_resc_id);
        ASSERT_NE(n_staged, nullptr);
        EXPECT_EQ(n_staged->get_attribute<int64_t>("obj_count"), 14);

        // Subsequent batch increments persisted value
        batch.clear();
        batch.inc_node_int(resc_id, "obj_count", 7);
        ASSERT_TRUE(engine.apply_batch(batch.get_buffer()));
        EXPECT_EQ(engine.get_node(resc_id)->get_attribute<int64_t>("obj_count"), 10);
    }

    std::filesystem::remove_all(db_path);
}
