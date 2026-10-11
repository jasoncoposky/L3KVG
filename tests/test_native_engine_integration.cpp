#include <gtest/gtest.h>
#include "L3KVG/Engine.hpp"
#include "L3KVG/Node.hpp"
#include "L3KVG/Query.hpp"
#include "L3KVG/MutationBatch.hpp"
#include <filesystem>
#include <fstream>
#include <algorithm>

struct TempDir {
    std::string path;
    explicit TempDir(std::string p) : path(std::move(p)) {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
};

TEST(NativeEngineIntegrationTest, AddAndTraverseEdges) {
    TempDir temp_dir("test_native_engine_add_traverse_db");
    const std::string& db_dir = temp_dir.path;

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
}

TEST(NativeEngineIntegrationTest, DeleteEdgeAndVerify) {
    TempDir temp_dir("test_native_engine_del_db");
    const std::string& db_dir = temp_dir.path;

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
}

TEST(NativeEngineIntegrationTest, FluentQueryExecution) {
    TempDir temp_dir("test_native_engine_fluent_db");
    const std::string& db_dir = temp_dir.path;

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
}

TEST(NativeEngineIntegrationTest, RecoveryAfterRestart) {
    TempDir temp_dir("test_native_engine_recovery_db");
    const std::string& db_dir = temp_dir.path;

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
}

TEST(NativeEngineIntegrationTest, BatchMutationIntegration) {
    TempDir temp_dir("test_native_engine_batch_db");
    const std::string& db_dir = temp_dir.path;

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
}

TEST(NativeEngineIntegrationTest, ReadOnlyLabelLookup) {
    TempDir temp_dir("test_native_engine_lookup_db");
    const std::string& db_dir = temp_dir.path;

    l3kvg::Settings settings;
    l3kvg::Engine engine(db_dir, 1, nullptr, 4, settings);

    // Initial label registry is empty
    EXPECT_EQ(engine.get_label_registry().size(), 0);
    EXPECT_EQ(engine.get_label_registry().lookup("UNKNOWN_LABEL"), 0);

    uint64_t n1 = 3001;
    uint64_t n2 = 3002;
    engine.add_edge(n1, "KNOWN_LABEL", 1.0, n2);

    EXPECT_EQ(engine.get_label_registry().size(), 1);
    uint16_t known_lid = engine.get_label_registry().lookup("KNOWN_LABEL");
    EXPECT_GT(known_lid, 0);

    auto node = engine.get_node(n1);
    ASSERT_NE(node, nullptr);

    // Query with unregistered label - should return empty and NOT register new label!
    auto unknown_neighbors = node->get_neighbors("NON_EXISTENT_LABEL");
    EXPECT_TRUE(unknown_neighbors.empty());
    EXPECT_EQ(engine.get_label_registry().size(), 1);
    EXPECT_EQ(engine.get_label_registry().lookup("NON_EXISTENT_LABEL"), 0);

    auto node2 = engine.get_node(n2);
    ASSERT_NE(node2, nullptr);
    auto unknown_in = node2->get_in_neighbors("NON_EXISTENT_LABEL");
    EXPECT_TRUE(unknown_in.empty());
    EXPECT_EQ(engine.get_label_registry().size(), 1);
}

TEST(NativeEngineIntegrationTest, WeightSensitiveEdgeDeletion) {
    TempDir temp_dir("test_native_engine_weight_del_db");
    const std::string& db_dir = temp_dir.path;

    l3kvg::Settings settings;
    l3kvg::Engine engine(db_dir, 1, nullptr, 4, settings);

    uint64_t src = 4001;
    uint64_t dst = 4002;

    engine.add_edge(src, "CONNECTS", 1.5, dst);
    engine.add_edge(src, "CONNECTS", 3.5, dst);

    EXPECT_EQ(engine.get_adjacency_store().get_out_degree(src), 2);

    // Try deleting with wrong weight (e.g. 2.0) -> should not delete anything
    engine.del_edge(src, "CONNECTS", 2.0, dst);
    EXPECT_EQ(engine.get_adjacency_store().get_out_degree(src), 2);

    // Delete edge with weight 1.5
    engine.del_edge(src, "CONNECTS", 1.5, dst);
    EXPECT_EQ(engine.get_adjacency_store().get_out_degree(src), 1);

    // Check remaining edge has weight 3.5:
    auto node = engine.get_node(src);
    ASSERT_NE(node, nullptr);
    auto neigh_high = node->get_neighbors("CONNECTS", 3.0);
    ASSERT_EQ(neigh_high.size(), 1);
    EXPECT_EQ(neigh_high[0], dst);

    // Delete edge with weight 3.5
    engine.del_edge(src, "CONNECTS", 3.5, dst);
    EXPECT_EQ(engine.get_adjacency_store().get_out_degree(src), 0);
    EXPECT_TRUE(node->get_neighbors("CONNECTS", 0.0).empty());
}

TEST(NativeEngineIntegrationTest, BatchMutationAllOrNothingStaging) {
    TempDir temp_dir("test_native_engine_staged_batch_db");
    const std::string& db_dir = temp_dir.path;

    l3kvg::Settings settings;
    l3kvg::Engine engine(db_dir, 1, nullptr, 4, settings);

    uint64_t src = 5001;
    uint64_t dst = 5002;

    l3kvg::MutationBatch batch;
    batch.add_edge(src, "TEST_STAGED", 1.0, dst, "{}");

    // Pass an unauthorized principal ID (non-admin, e.g. 8888) to trigger failure during add_put
    bool ok = engine.apply_batch(batch.get_buffer(), 8888);
    EXPECT_FALSE(ok);

    // All-or-nothing: the staged edge should NOT be present in AdjacencyStore!
    EXPECT_EQ(engine.get_adjacency_store().get_out_degree(src), 0);
    EXPECT_EQ(engine.get_adjacency_store().get_in_degree(dst), 0);

    auto node = engine.get_node(src);
    ASSERT_NE(node, nullptr);
    EXPECT_TRUE(node->get_neighbors("TEST_STAGED").empty());
}

TEST(NativeEngineIntegrationTest, LabelsDictCRLFAndAtomicWrite) {
    TempDir temp_dir("test_native_engine_crlf_db");
    const std::string& db_dir = temp_dir.path;

    std::filesystem::create_directories(db_dir);
    std::string dict_path = db_dir + "/labels.dict";
    {
        std::ofstream ofs(dict_path, std::ios::binary);
        // Write CRLF separated labels
        ofs << "ALPHA\r\nBETA\r\nGAMMA\r\n";
    }

    {
        l3kvg::Settings settings;
        l3kvg::Engine engine(db_dir, 1, nullptr, 4, settings);

        // Verify CRLF was stripped and labels were loaded
        EXPECT_EQ(engine.get_label_registry().size(), 3);
        EXPECT_EQ(engine.get_label_registry().get_label(1), "ALPHA");
        EXPECT_EQ(engine.get_label_registry().get_label(2), "BETA");
        EXPECT_EQ(engine.get_label_registry().get_label(3), "GAMMA");

        engine.add_edge(6001, "DELTA", 1.0, 6002);
        EXPECT_EQ(engine.get_label_registry().size(), 4);

        engine.flush();

        // Verify .tmp file does not exist after flush
        EXPECT_FALSE(std::filesystem::exists(dict_path + ".tmp"));
        EXPECT_TRUE(std::filesystem::exists(dict_path));
    }

    // Reopen and verify DELTA is present
    {
        l3kvg::Settings settings;
        l3kvg::Engine engine(db_dir, 1, nullptr, 4, settings);
        EXPECT_EQ(engine.get_label_registry().size(), 4);
        EXPECT_EQ(engine.get_label_registry().get_label(4), "DELTA");
        EXPECT_FALSE(std::filesystem::exists(dict_path + ".tmp"));
    }
}

TEST(NativeEngineIntegrationTest, WildcardAndSynthesizedEdgeTraversal) {
    TempDir temp_dir("test_wildcard_synthesized_edges_db");
    const std::string& db_dir = temp_dir.path;

    {
        l3kvg::Settings settings;
        l3kvg::Engine engine(db_dir, 1, nullptr, 4, settings);

        uint64_t parent = 9001;
        uint64_t child1 = 9002;
        uint64_t child2 = 9003;
        uint64_t friend1 = 9004;

        engine.add_edge(parent, "CONTAINS", 1.0, child1);
        engine.add_edge(parent, "CONTAINS", 2.5, child2);
        engine.add_edge(parent, "KNOWS", 0.5, friend1);

        auto node = engine.get_node(parent);
        ASSERT_NE(node, nullptr);

        // 1. Wildcard node->get_neighbors("") returning native edges
        auto all_neighbors = node->get_neighbors("");
        ASSERT_EQ(all_neighbors.size(), 3);
        std::sort(all_neighbors.begin(), all_neighbors.end());
        EXPECT_EQ(all_neighbors[0], child1);
        EXPECT_EQ(all_neighbors[1], child2);
        EXPECT_EQ(all_neighbors[2], friend1);

        // 2. node->get_edges("CONTAINS") returning synthesized Edge objects
        auto contains_edges = node->get_edges("CONTAINS");
        ASSERT_EQ(contains_edges.size(), 2);
        for (const auto& e : contains_edges) {
            EXPECT_EQ(e->get_src(), parent);
            EXPECT_EQ(e->get_label(), "CONTAINS");
            EXPECT_TRUE(e->get_dst() == child1 || e->get_dst() == child2);
            if (e->get_dst() == child1) {
                EXPECT_FLOAT_EQ(static_cast<float>(e->get_weight()), 1.0f);
            } else if (e->get_dst() == child2) {
                EXPECT_FLOAT_EQ(static_cast<float>(e->get_weight()), 2.5f);
            }
        }

        // 3. node->get_edges("") returning all synthesized Edge objects
        auto all_edges = node->get_edges("");
        ASSERT_EQ(all_edges.size(), 3);
        size_t contains_count = 0;
        size_t knows_count = 0;
        for (const auto& e : all_edges) {
            EXPECT_EQ(e->get_src(), parent);
            if (e->get_label() == "CONTAINS") {
                contains_count++;
            } else if (e->get_label() == "KNOWS") {
                knows_count++;
                EXPECT_EQ(e->get_dst(), friend1);
                EXPECT_FLOAT_EQ(static_cast<float>(e->get_weight()), 0.5f);
            }
        }
        EXPECT_EQ(contains_count, 2);
        EXPECT_EQ(knows_count, 1);
    }
}
