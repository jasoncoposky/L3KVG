#include <gtest/gtest.h>
#include "L3KVG/Engine.hpp"
#include "L3KVG/Node.hpp"
#include "L3KVG/Query.hpp"
#include "L3KVG/MutationBatch.hpp"
#include <filesystem>
#include <algorithm>

TEST(NativeEngineIntegrationTest, AddAndTraverseEdges) {
    std::string db_dir = "test_native_engine_add_traverse_db";
    std::filesystem::remove_all(db_dir);

    {
        l3kvg::Settings settings;
        l3kvg::Engine engine(db_dir, 1, nullptr, 4, settings);

        uint64_t n1 = 1001;
        uint64_t n2 = 1002;
        uint64_t n3 = 1003;
        uint64_t n4 = 1004;

        engine.add_edge(n1, "KNOWS", 0.9, n2);
        engine.add_edge(n1, "KNOWS", 0.4, n3);
        engine.add_edge(n1, "WORKS_WITH", 1.0, n4);
        engine.add_edge(n3, "KNOWS", 0.8, n2);

        EXPECT_EQ(engine.get_adjacency_store().get_out_degree(n1), 3);
        EXPECT_EQ(engine.get_adjacency_store().get_in_degree(n2), 2);

        auto node1 = engine.get_node(n1);
        ASSERT_NE(node1, nullptr);

        // Filter by label and min_weight
        auto neigh_high = node1->get_neighbors("KNOWS", 0.5);
        ASSERT_EQ(neigh_high.size(), 1);
        EXPECT_EQ(neigh_high[0], n2);

        auto neigh_all_knows = node1->get_neighbors("KNOWS", 0.0);
        ASSERT_EQ(neigh_all_knows.size(), 2);
        std::sort(neigh_all_knows.begin(), neigh_all_knows.end());
        EXPECT_EQ(neigh_all_knows[0], n2);
        EXPECT_EQ(neigh_all_knows[1], n3);

        auto neigh_works = node1->get_neighbors("WORKS_WITH", 0.0);
        ASSERT_EQ(neigh_works.size(), 1);
        EXPECT_EQ(neigh_works[0], n4);

        // Wildcard label ("") matches any active edge
        auto neigh_wildcard = node1->get_neighbors("", 0.0);
        ASSERT_EQ(neigh_wildcard.size(), 3);
        std::sort(neigh_wildcard.begin(), neigh_wildcard.end());
        EXPECT_EQ(neigh_wildcard[0], n2);
        EXPECT_EQ(neigh_wildcard[1], n3);
        EXPECT_EQ(neigh_wildcard[2], n4);

        // In-neighbors
        auto node2 = engine.get_node(n2);
        ASSERT_NE(node2, nullptr);
        auto in_n2 = node2->get_in_neighbors("KNOWS");
        ASSERT_EQ(in_n2.size(), 2);
        std::sort(in_n2.begin(), in_n2.end());
        EXPECT_EQ(in_n2[0], n1);
        EXPECT_EQ(in_n2[1], n3);

        auto node4 = engine.get_node(n4);
        ASSERT_NE(node4, nullptr);
        auto in_n4 = node4->get_in_neighbors("WORKS_WITH");
        ASSERT_EQ(in_n4.size(), 1);
        EXPECT_EQ(in_n4[0], n1);

        auto in_n3_works = engine.get_node(n3)->get_in_neighbors("WORKS_WITH");
        EXPECT_TRUE(in_n3_works.empty());
    }

    std::filesystem::remove_all(db_dir);
}

TEST(NativeEngineIntegrationTest, DeleteEdgeAndVerify) {
    std::string db_dir = "test_native_engine_del_db";
    std::filesystem::remove_all(db_dir);

    {
        l3kvg::Settings settings;
        l3kvg::Engine engine(db_dir, 1, nullptr, 4, settings);

        uint64_t src = 2001;
        uint64_t dst1 = 2002;
        uint64_t dst2 = 2003;

        engine.add_edge(src, "FRIEND", 1.0, dst1);
        engine.add_edge(src, "FRIEND", 1.0, dst2);

        EXPECT_EQ(engine.get_adjacency_store().get_out_degree(src), 2);
        EXPECT_EQ(engine.get_adjacency_store().get_in_degree(dst1), 1);
        EXPECT_EQ(engine.get_adjacency_store().get_in_degree(dst2), 1);

        auto n_src = engine.get_node(src);
        ASSERT_NE(n_src, nullptr);
        EXPECT_EQ(n_src->get_neighbors("FRIEND").size(), 2);
        EXPECT_EQ(engine.get_node(dst1)->get_in_neighbors("FRIEND").size(), 1);

        // Delete first edge
        engine.del_edge(src, "FRIEND", 1.0, dst1);

        EXPECT_EQ(engine.get_adjacency_store().get_out_degree(src), 1);
        EXPECT_EQ(engine.get_adjacency_store().get_in_degree(dst1), 0);
        EXPECT_EQ(engine.get_adjacency_store().get_in_degree(dst2), 1);

        auto neigh1 = n_src->get_neighbors("FRIEND");
        ASSERT_EQ(neigh1.size(), 1);
        EXPECT_EQ(neigh1[0], dst2);
        EXPECT_TRUE(engine.get_node(dst1)->get_in_neighbors("FRIEND").empty());

        // Delete second edge
        engine.del_edge(src, "FRIEND", 1.0, dst2);

        EXPECT_EQ(engine.get_adjacency_store().get_out_degree(src), 0);
        EXPECT_EQ(engine.get_adjacency_store().get_in_degree(dst2), 0);
        EXPECT_TRUE(n_src->get_neighbors("FRIEND").empty());
        EXPECT_TRUE(engine.get_node(dst2)->get_in_neighbors("FRIEND").empty());
    }

    std::filesystem::remove_all(db_dir);
}

TEST(NativeEngineIntegrationTest, FluentQueryExecution) {
    std::string db_dir = "test_native_engine_fluent_db";
    std::filesystem::remove_all(db_dir);

    {
        l3kvg::Settings settings;
        l3kvg::Engine engine(db_dir, 1, nullptr, 4, settings);

        uint64_t coll = 100;
        uint64_t data = 200;
        uint64_t repl = 300;

        engine.put_node(coll, R"({"id":"100","name":"Collection1","entity_type":"collection"})");
        engine.put_node(data, R"({"id":"200","name":"DataObj1","entity_type":"data_object"})");
        engine.put_node(repl, R"({"id":"300","name":"Replica1","entity_type":"replica"})");

        engine.add_edge(coll, "CONTAINS", 1.0, data);
        engine.add_edge(data, "HAS_REPLICA", 1.0, repl);

        l3kvg::Query q(&engine);
        q.match_id(data, "DataObject")
         .out("HAS_REPLICA").as("Replica")
         .from("DataObject").in("CONTAINS").as("Collection");

        auto res = q.execute();
        ASSERT_EQ(res.size(), 1);
        EXPECT_EQ(res[0].nodes.size(), 3);
        bool has_data = false, has_coll = false, has_repl = false;
        for (const auto& n : res[0].nodes) {
            if (n->get_id() == data) has_data = true;
            if (n->get_id() == coll) has_coll = true;
            if (n->get_id() == repl) has_repl = true;
        }
        EXPECT_TRUE(has_data);
        EXPECT_TRUE(has_coll);
        EXPECT_TRUE(has_repl);

        // Multi-hop outbound
        l3kvg::Query q2(&engine);
        q2.match_id(coll, "Collection")
          .out("CONTAINS").as("DataObject")
          .out("HAS_REPLICA").as("Replica");

        auto res2 = q2.execute();
        ASSERT_EQ(res2.size(), 1);
        EXPECT_EQ(res2[0].nodes.size(), 3);
    }

    std::filesystem::remove_all(db_dir);
}

TEST(NativeEngineIntegrationTest, RecoveryAfterRestart) {
    std::string db_dir = "test_native_engine_recovery_db";
    std::filesystem::remove_all(db_dir);

    uint64_t n501 = 501;
    uint64_t n502 = 502;
    uint64_t n503 = 503;
    uint64_t n504 = 504;

    // Scope 1: Create engine, add edges, flush, destroy
    {
        l3kvg::Settings settings;
        l3kvg::Engine engine(db_dir, 1, nullptr, 4, settings);

        engine.add_edge(n501, "FOLLOWS", 1.0, n502);
        engine.add_edge(n501, "FOLLOWS", 2.0, n503);
        engine.add_edge(n502, "LIKES", 1.5, n504);

        engine.flush();
    }

    // Scope 2: Open new engine on same db_dir, verify edges recovered from WAL
    {
        l3kvg::Settings settings;
        l3kvg::Engine engine2(db_dir, 1, nullptr, 4, settings);

        EXPECT_EQ(engine2.get_adjacency_store().get_out_degree(n501), 2);
        EXPECT_EQ(engine2.get_adjacency_store().get_out_degree(n502), 1);
        EXPECT_EQ(engine2.get_adjacency_store().get_in_degree(n502), 1);
        EXPECT_EQ(engine2.get_adjacency_store().get_in_degree(n504), 1);

        auto node501 = engine2.get_node(n501);
        ASSERT_NE(node501, nullptr);

        auto follows = node501->get_neighbors("FOLLOWS", 1.5);
        ASSERT_EQ(follows.size(), 1);
        EXPECT_EQ(follows[0], n503);

        auto all_follows = node501->get_neighbors("FOLLOWS", 0.0);
        ASSERT_EQ(all_follows.size(), 2);
        std::sort(all_follows.begin(), all_follows.end());
        EXPECT_EQ(all_follows[0], n502);
        EXPECT_EQ(all_follows[1], n503);

        auto node502 = engine2.get_node(n502);
        ASSERT_NE(node502, nullptr);
        auto in502 = node502->get_in_neighbors("FOLLOWS");
        ASSERT_EQ(in502.size(), 1);
        EXPECT_EQ(in502[0], n501);

        auto likes502 = node502->get_neighbors("LIKES");
        ASSERT_EQ(likes502.size(), 1);
        EXPECT_EQ(likes502[0], n504);

        // Delete an edge in recovered engine and verify persistence of deletion
        engine2.del_edge(n501, "FOLLOWS", 1.0, n502);
        engine2.flush();
    }

    // Scope 3: Reopen again, verify deletion was persisted
    {
        l3kvg::Settings settings;
        l3kvg::Engine engine3(db_dir, 1, nullptr, 4, settings);

        EXPECT_EQ(engine3.get_adjacency_store().get_out_degree(n501), 1);
        auto node501 = engine3.get_node(n501);
        ASSERT_NE(node501, nullptr);
        auto follows = node501->get_neighbors("FOLLOWS");
        ASSERT_EQ(follows.size(), 1);
        EXPECT_EQ(follows[0], n503);

        auto node502 = engine3.get_node(n502);
        ASSERT_NE(node502, nullptr);
        EXPECT_TRUE(node502->get_in_neighbors("FOLLOWS").empty());
    }

    std::filesystem::remove_all(db_dir);
}

TEST(NativeEngineIntegrationTest, BatchMutationIntegration) {
    std::string db_dir = "test_native_engine_batch_db";
    std::filesystem::remove_all(db_dir);

    {
        l3kvg::Settings settings;
        l3kvg::Engine engine(db_dir, 1, nullptr, 4, settings);

        uint64_t n7001 = 7001;
        uint64_t n8001 = 8001;
        uint64_t n8002 = 8002;
        uint64_t n8003 = 8003;

        l3kvg::MutationBatch batch;
        batch.add_edge(n7001, "LINKED_TO", 1.0, n8001, "{}");
        batch.add_edge(n7001, "LINKED_TO", 2.0, n8002, "{}");
        batch.add_edge(n7001, "OTHER", 3.0, n8003, "{}");

        bool ok = engine.apply_batch(batch.get_buffer());
        EXPECT_TRUE(ok);

        EXPECT_EQ(engine.get_adjacency_store().get_out_degree(n7001), 3);
        EXPECT_EQ(engine.get_adjacency_store().get_in_degree(n8001), 1);
        EXPECT_EQ(engine.get_adjacency_store().get_in_degree(n8002), 1);

        auto node7001 = engine.get_node(n7001);
        ASSERT_NE(node7001, nullptr);

        auto linked = node7001->get_neighbors("LINKED_TO");
        ASSERT_EQ(linked.size(), 2);
        std::sort(linked.begin(), linked.end());
        EXPECT_EQ(linked[0], n8001);
        EXPECT_EQ(linked[1], n8002);

        auto in8001 = engine.get_node(n8001)->get_in_neighbors("LINKED_TO");
        ASSERT_EQ(in8001.size(), 1);
        EXPECT_EQ(in8001[0], n7001);

        // Delete via batch
        l3kvg::MutationBatch del_batch;
        del_batch.del_edge(n7001, "LINKED_TO", 1.0, n8001);
        bool del_ok = engine.apply_batch(del_batch.get_buffer());
        EXPECT_TRUE(del_ok);

        EXPECT_EQ(engine.get_adjacency_store().get_out_degree(n7001), 2);
        EXPECT_EQ(engine.get_adjacency_store().get_in_degree(n8001), 0);

        auto linked_after = node7001->get_neighbors("LINKED_TO");
        ASSERT_EQ(linked_after.size(), 1);
        EXPECT_EQ(linked_after[0], n8002);

        EXPECT_TRUE(engine.get_node(n8001)->get_in_neighbors("LINKED_TO").empty());
    }

    std::filesystem::remove_all(db_dir);
}
