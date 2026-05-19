#include "L3KVG/Engine.hpp"
#include "L3KVG/Query.hpp"
#include "L3KVG/FederationID.hpp"
#include "L3KVG/QueryResult.hpp"
#include <filesystem>
#include <gtest/gtest.h>
#include <string>
#include <future>
#include <vector>

// Mock RemoteL3KVClient to intercept resume_query_async
namespace l3kvg {
class MockRemoteClient : public RemoteL3KVClient {
public:
    std::future<std::vector<ResultRow>> resume_query_async(
        uint16_t cluster_id,
        const std::vector<uint64_t>& starting_nodes,
        const std::string& query_json,
        uint32_t principal_id = l3kv::INTERNAL_UID) override {
        std::promise<std::vector<ResultRow>> p;
        std::vector<ResultRow> results;
        
        // Mock result for Node B
        // In a real scenario, this would come from another cluster's Engine::execute
        ResultRow row;
        row.fields["b.name"] = "Node B (Remote)";
        row.fields["0"] = "Node B (Remote)";
        results.push_back(std::move(row));
        
        p.set_value(std::move(results));
        return p.get_future();
    }
};
}

TEST(FederationQueryTest, CrossClusterTraversal) {
  // 1. Setup Local Cluster (ID: 1)
  std::string db_path = "test_l3kvg_fed_db";
  std::filesystem::remove_all(db_path);
  
  uint16_t local_cluster_id = 1;
  uint16_t remote_cluster_id = 2;

  auto engine = std::make_unique<l3kvg::Engine>(db_path, 1);
  engine->get_resolver().register_local_cluster("local", local_cluster_id);
  engine->get_resolver().register_federation("remote", remote_cluster_id, {"tcp://localhost:5556"});

  // Inject Mock Client
  engine->set_remote_client(std::make_unique<l3kvg::MockRemoteClient>());

  // 2. Put Local Node A
  uint64_t node_a_id = engine->get_resolver().parse_uuid("node_a");
  engine->put_node(node_a_id, R"({"id":"node_a","name":"Node A"})");

  // 3. Define Remote Node B ID (but don't put it locally)
  uint64_t node_b_id = engine->get_resolver().parse_uuid("remote:node_b");

  // 4. Add edge A -> B
  engine->add_edge(node_a_id, "link", 1.0, node_b_id);

  // 5. Query: Match A, traverse to B
  // This triggers the suspension logic in Query::execute because B is remote.
  auto results = engine->query()
                     .match("a")
                     .where_eq("a", "id", "node_a")
                     .out("link")
                     .as("b")
                     .return_("b", "name")
                     .execute();

  // 6. Verify results
  // Should contain Node B's name as returned by the mock.
  ASSERT_EQ(results.size(), 1);
  EXPECT_EQ(results[0].fields.at("b.name"), "Node B (Remote)");
  EXPECT_EQ(results[0].fields.at("0"), "Node B (Remote)");
}

int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
