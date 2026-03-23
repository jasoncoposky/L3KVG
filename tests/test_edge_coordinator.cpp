#include <gtest/gtest.h>
#include "L3KVG/Engine.hpp"
#include "L3KVG/Node.hpp"
#include <filesystem>
#include <memory>
#include <string>
#include <lite3/ring.hpp>

TEST(EdgeCoordinatorTest, BasicLocalDualWrite) {
    std::string db_path = "test_coord_db";
    std::filesystem::remove_all(db_path);

    auto ring = std::make_shared<lite3::ConsistentHash>(100);
    ring->add_node(42);

    {
        l3kvg::Engine engine(db_path, 42, ring);

        engine.put_node("node_A", "{\"name\": \"Alice\"}");
        engine.put_node("node_B", "{\"name\": \"Bob\"}");

        engine.add_edge("node_A", "knows", 0.9, "node_B");

        auto nodeA = engine.get_node("node_A");
        auto neighbors_A = nodeA->get_neighbors("knows", 0.5);
        ASSERT_EQ(neighbors_A.size(), size_t(1));
        EXPECT_EQ(neighbors_A[0], std::string("node_B"));
    }
    
    std::filesystem::remove_all(db_path);
}
