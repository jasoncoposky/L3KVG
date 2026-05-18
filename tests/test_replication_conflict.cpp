#include "L3KVG/Engine.hpp"
#include "L3KVG/Node.hpp"
#include "L3KVG/Query.hpp"
#include "L3KVG/QueryResult.hpp"
#include "L3KVG/KeyBuilder.hpp"
#include "engine/store.hpp"
#include <filesystem>
#include <gtest/gtest.h>
#include <thread>
#include <zmq.hpp>
#include <zmq_addon.hpp>
#include <nlohmann/json.hpp>
#include <atomic>

using json = nlohmann::json;

class ReplicationConflictTest : public ::testing::Test {
protected:
    std::string db_local = "test_conflict_local";
    
    void SetUp() override {
        std::filesystem::remove_all(db_local);
    }

    void TearDown() override {
        std::filesystem::remove_all(db_local);
    }
};

TEST_F(ReplicationConflictTest, LastWriterWins) {
    l3kvg::Settings settings;
    auto engine = std::make_unique<l3kvg::Engine>(db_local, 1, nullptr, 1, settings);
    
    uint64_t node_id = 100;
    std::string key = std::string(l3kvg::KeyBuilder::node_key(node_id));
    
    // 1. Cluster A writes at T1
    l3kvg::HLCTimestamp t1;
    t1.wall_time = 1000000;
    t1.logical = 1;
    t1.node_id = 1;
    
    std::string payload1 = "{\"name\":\"Older\", \"_hlc\":" + t1.to_json_string() + "}";
    
    // 2. Cluster B writes at T2 (Newer)
    l3kvg::HLCTimestamp t2;
    t2.wall_time = 1000000;
    t2.logical = 2; // Higher logical counter
    t2.node_id = 2;
    
    std::string payload2 = "{\"name\":\"Newer\", \"_hlc\":" + t2.to_json_string() + "}";

    // Simulate T2 arriving first
    std::cout << "[Test] Applying Newer write (T2)..." << std::endl;
    engine->replicate_key(key, payload2, 2);
    
    // Wait for async sharded put
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    
    auto val = engine->get_store()->get(key);
    ASSERT_GT(val.size(), 0);
    EXPECT_TRUE(std::string(reinterpret_cast<const char*>(val.data()), val.size()).find("Newer") != std::string::npos);

    // Simulate T1 (Older) arriving LATER
    std::cout << "[Test] Applying Older write (T1) arriving LATER..." << std::endl;
    engine->replicate_key(key, payload1, 1);
    
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // Verify Cluster A KEPT the Newer write
    bool resolved = false;
    std::string final_name;
    for (int i = 0; i < 20; ++i) {
        // Force sync fetch
        auto nodes = engine->fetch_nodes({node_id});
        if (!nodes.empty() && nodes[0]->is_loaded()) {
            final_name = nodes[0]->get_attribute<std::string>("name");
            if (final_name == "Newer") {
                resolved = true;
                break;
            }
            if (final_name == "Older") {
                // We found the wrong one, stop early
                break;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    
    EXPECT_TRUE(resolved) << "Older write overwrote Newer write! Final Name Attribute: " << final_name;
}

int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
