#include "L3KVG/Cypher.hpp"
#include <filesystem>
#include <gtest/gtest.h>


TEST(CypherParserTest, MatchWhereReturn) {
  std::string db_path = "test_cypher_db";
  std::filesystem::remove_all(db_path);
  auto engine = std::make_unique<l3kvg::Engine>(db_path, 1);

  // Seed Data
  engine->put_node("npc_1", R"({"id": "npc_1", "type": "npc"})");
  engine->put_node(
      "npc_2", R"({"id": "npc_2", "faction": "stark", "name": "Arya Stark"})");

  // Add Edge
  engine->add_edge("npc_1", "loyalty", 0.85, "npc_2");

  // Execute OpenCypher Subset
  l3kvg::CypherParser parser(engine.get());
  std::string query = "MATCH (n {id: 'npc_1', type: 'npc'})-[e:loyalty]->(ally "
                      "{faction: 'stark'}) "
                      "WHERE e.weight >= 0.5 "
                      "RETURN ally.name";

  auto rows = parser.execute(query);

  ASSERT_EQ(rows.size(), 1);
  EXPECT_EQ(rows[0].fields["ally.name"], "Arya Stark");
}
