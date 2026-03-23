#include "L3KVG/Engine.hpp"
#include "L3KVG/Query.hpp"
#include <filesystem>
#include <gtest/gtest.h>
#include <string>


TEST(FluentAPITest, BasicQueryExecution) {
  // 1. Setup DB
  std::string db_path = "test_l3kvg_fluent_db";
  std::filesystem::remove_all(db_path);
  auto engine = std::make_unique<l3kvg::Engine>(db_path, 1);

  // 2. Put Nodes
  engine->put_node("npc_tuly",
                   R"({"id":"npc_tuly","type":"npc","name":"Hoster Tuly"})");
  engine->put_node(
      "npc_robb",
      R"({"id":"npc_robb","type":"npc","name":"Robb Stark","faction":"stark"})");
  engine->put_node(
      "npc_jons",
      R"({"id":"npc_jons","type":"npc","name":"Jon Snow","faction":"stark"})");
  engine->put_node(
      "npc_lannister",
      R"({"id":"npc_lannister","type":"npc","name":"Tywin Lannister","faction":"lannister"})");

  // 3. Put Edges (Tuly loyalty -> Robb, Jon, Tywin)
  engine->add_edge("npc_tuly", "loyalty", 0.9, "npc_robb");
  engine->add_edge("npc_tuly", "loyalty", 0.6, "npc_jons");
  engine->add_edge("npc_tuly", "loyalty", 0.1, "npc_lannister");

  // 4. Query: Match Tuly, find loyal allies (weight >= 0.5) that belong to
  // Stark faction.
  auto results = engine->query()
                     .match("n")
                     .where_eq("n", "id", "npc_tuly")
                     .out("loyalty", 0.5)
                     .as("ally")
                     .where_eq("ally", "faction", "stark")
                     .return_("ally", "name")
                     .execute();

  // 5. Verify
  EXPECT_EQ(results.size(), 2);

  bool found_robb = false;
  bool found_jon = false;
  for (const auto &row : results) {
    auto it = row.fields.find("ally.name");
    ASSERT_NE(it, row.fields.end());
    if (it->second == "Robb Stark")
      found_robb = true;
    if (it->second == "Jon Snow")
      found_jon = true;
  }

  EXPECT_TRUE(found_robb);
  EXPECT_TRUE(found_jon);
}

int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
