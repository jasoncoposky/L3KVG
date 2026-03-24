#include <gtest/gtest.h>
#include "L3KVG/Engine.hpp"
#include "L3KVG/Node.hpp"
#include "L3KVG/Edge.hpp"
#include <filesystem>

TEST(EdgePropertiesTest, StoreAndRetrieveProperties) {
    std::string db_path = "test_edge_props_db";
    std::filesystem::remove_all(db_path);

    {
        l3kvg::Engine engine(db_path, 1);
        
        engine.put_node("a", "{\"name\": \"Alice\"}");
        engine.put_node("b", "{\"name\": \"Bob\"}");
        
        // Add edge with properties
        std::string edge_props = "{\"since\": 2021, \"type\": \"contractor\", \"rate\": 125.5}";
        engine.add_edge("a", "works_with", 1.0, "b", edge_props);
        
        auto node_a = engine.get_node("a");
        auto edges = node_a->get_edges("works_with");
        
        ASSERT_EQ(edges.size(), 1);
        auto edge = edges[0];
        
        EXPECT_EQ(edge->get_src(), "a");
        EXPECT_EQ(edge->get_dst(), "b");
        EXPECT_EQ(edge->get_label(), "works_with");
        EXPECT_DOUBLE_EQ(edge->get_weight(), 1.0);
        
        // Retrieve properties
        EXPECT_EQ(edge->get_attribute<int>("since"), 2021);
        EXPECT_EQ(edge->get_attribute<std::string>("type"), "contractor");
        EXPECT_DOUBLE_EQ(edge->get_attribute<double>("rate"), 125.5);
    }

    std::filesystem::remove_all(db_path);
}
