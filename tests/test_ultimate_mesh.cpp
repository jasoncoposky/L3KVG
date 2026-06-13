#include "L3KVG/Engine.hpp"
#include "L3KVG/Node.hpp"
#include "L3KVG/Query.hpp"
#include "L3KVG/KeyBuilder.hpp"
#include "L3KVG/FederationID.hpp"
#include "L3KVG/QueryResult.hpp"
#include "engine/store.hpp" // L3KV Store
#include <gtest/gtest.h>
#include <thread>
#include <zmq.hpp>
#include <zmq_addon.hpp>
#include <nlohmann/json.hpp>
#include <filesystem>
#include <vector>
#include <unordered_map>
#include <memory>
#include <atomic>

using json = nlohmann::json;

namespace l3kvg {

class GlobalMeshHarness {
public:
    struct NodeHandle {
        uint32_t node_id;
        uint16_t cluster_id;
        uint16_t zmq_port;
        std::string db_path;
        std::shared_ptr<lite3::ConsistentHash> ring;
        std::unique_ptr<Engine> engine;
        std::thread server_thread;
        std::atomic<bool> running{true};
        std::string auth_secret;
        std::unordered_map<std::string, uint32_t> sessions;
        zmq::context_t ctx{1};
NodeHandle(uint32_t id, uint16_t cid, uint16_t port, std::string db, std::shared_ptr<lite3::ConsistentHash> r, Settings settings, std::string secret = "")
    : node_id(id), cluster_id(cid), zmq_port(port), db_path(db), ring(r), auth_secret(secret) {
    engine = std::make_unique<Engine>(db_path, node_id, ring, 4, settings);
    engine->get_resolver().register_local_cluster("cluster_" + std::to_string(cluster_id), cluster_id);
    if (!auth_secret.empty()) {
        engine->set_auth_secret(auth_secret);
        // Self-trust
        engine->get_store()->credentials().register_user(id, "node", "key"); 
        engine->get_store()->credentials().set_acl(id, "", l3kv::Permission::ADMIN);
    }
}


        void start() {
            server_thread = std::thread([this]() {
                try {
                    zmq::socket_t sock(ctx, ZMQ_ROUTER);
                    sock.set(zmq::sockopt::linger, 0);
                    sock.bind("tcp://127.0.0.1:" + std::to_string(zmq_port));

                    while (running) {
                        std::vector<zmq::message_t> recv_msgs;
                        auto result = zmq::recv_multipart(sock, std::back_inserter(recv_msgs), zmq::recv_flags::dontwait);
                        if (!result || recv_msgs.size() < 4) {
                            std::this_thread::sleep_for(std::chrono::milliseconds(10));
                            continue;
                        }

                        auto identity = std::move(recv_msgs[0]);
                        auto identity_str = identity.to_string();
                        
                        uint32_t effective_uid = 0;
                        if (recv_msgs[2].size() == 4) {
                            effective_uid = *static_cast<uint32_t*>(recv_msgs[2].data());
                        }
                        
                        auto opcode = recv_msgs[3].to_string();

                        if (opcode == "A" && recv_msgs.size() >= 6) {
                            uint32_t uid = std::stoul(recv_msgs[4].to_string());
                            std::string secret = recv_msgs[5].to_string();
                            if (auth_secret.empty() || secret == auth_secret) {
                                sessions[identity_str] = uid;
                                sock.send(identity, zmq::send_flags::sndmore);
                                sock.send(zmq::message_t(), zmq::send_flags::sndmore);
                                sock.send(zmq::message_t("OK", 2), zmq::send_flags::none);
                            } else {
                                sock.send(identity, zmq::send_flags::sndmore);
                                sock.send(zmq::message_t(), zmq::send_flags::sndmore);
                                sock.send(zmq::message_t("ERR_AUTH", 8), zmq::send_flags::none);
                            }
                            continue;
                        }

                        if (!auth_secret.empty() && !sessions.contains(identity_str)) {
                            sock.send(identity, zmq::send_flags::sndmore);
                            sock.send(zmq::message_t(), zmq::send_flags::sndmore);
                            sock.send(zmq::message_t("ERR_UNAUTH", 10), zmq::send_flags::none);
                            continue;
                        }

                        uint32_t session_uid = sessions.contains(identity_str) ? sessions[identity_str] : 0;
                        uint32_t current_uid = session_uid;

                        // If session has ADMIN perms, allow Principal Propagation
                        auto session_perm = engine->get_store()->credentials().check_permission(session_uid, "");
                        if (session_perm & l3kv::Permission::ADMIN) {
                            current_uid = effective_uid;
                        }

                        if (opcode == "S") {
                            std::string key = recv_msgs[4].to_string();
                            std::string payload = recv_msgs[5].to_string();
                            uint16_t origin = 0;
                            if (recv_msgs.size() >= 7) {
                                try { origin = static_cast<uint16_t>(std::stoi(recv_msgs[6].to_string())); } catch (...) {}
                            }
                            engine->replicate_key(key, payload, origin);
                            sock.send(identity, zmq::send_flags::sndmore);
                            sock.send(zmq::message_t(), zmq::send_flags::sndmore);
                            sock.send(zmq::message_t("OK", 2), zmq::send_flags::none);
                        } else if (opcode == "N") {
                            try {
                                uint64_t target_id = std::stoull(recv_msgs[4].to_string(), nullptr, 16);
                                std::string label = recv_msgs[5].to_string();
                                double min_weight = 0.0;
                                if (recv_msgs.size() >= 7) min_weight = std::stod(recv_msgs[6].to_string());
                                
                                // Authorization Check
                                auto perm = engine->get_store()->credentials().check_permission(current_uid, std::string(KeyBuilder::node_key(target_id)));
                                if (!(perm & l3kv::Permission::READ) && !(perm & l3kv::Permission::ADMIN)) {
                                    sock.send(identity, zmq::send_flags::sndmore);
                                    sock.send(zmq::message_t(), zmq::send_flags::sndmore);
                                    sock.send(zmq::message_t("[]", 2), zmq::send_flags::none);
                                    continue;
                                }

                                auto node = engine->get_node(target_id);
                                auto neighbors = node->get_neighbors(label, min_weight);
                                json j_neighs = json::array();
                                for (auto id : neighbors) {
                                    char id_buf[17];
                                    std::snprintf(id_buf, sizeof(id_buf), "%016llx", (unsigned long long)id);
                                    j_neighs.push_back(std::string(id_buf));
                                }
                                std::string resp_json = j_neighs.dump();
                                sock.send(identity, zmq::send_flags::sndmore);
                                sock.send(zmq::message_t(), zmq::send_flags::sndmore);
                                sock.send(zmq::message_t(resp_json.data(), resp_json.size()), zmq::send_flags::none);
                            } catch (...) {
                                sock.send(identity, zmq::send_flags::sndmore);
                                sock.send(zmq::message_t(), zmq::send_flags::sndmore);
                                sock.send(zmq::message_t("[]", 2), zmq::send_flags::none);
                            }
                        } else if (opcode == "M") {
                            // MULTI-GET [Key1] [Key2] ...
                            lite3cpp::Buffer res_buf;
                            res_buf.init_object();
                            for (size_t i = 4; i < recv_msgs.size(); ++i) {
                                std::string key = recv_msgs[i].to_string();
                                auto perm = engine->get_store()->credentials().check_permission(current_uid, key);
                                if ((perm & l3kv::Permission::READ) || (perm & l3kv::Permission::ADMIN)) {
                                    auto val = engine->get_store()->get(key);
                                    if (val.size() > 0) {
                                        res_buf.set_bytes(0, key, std::span<const std::byte>(reinterpret_cast<const std::byte*>(val.data()), val.size()));
                                    }
                                }
                            }
                            sock.send(identity, zmq::send_flags::sndmore);
                            sock.send(zmq::message_t(), zmq::send_flags::sndmore);
                            sock.send(zmq::message_t(res_buf.data(), res_buf.size()), zmq::send_flags::none);
                        } else if (opcode == "R") {
                            try {
                                std::vector<uint64_t> nodes = json::parse(recv_msgs[4].to_string());
                                std::string query_json = recv_msgs[5].to_string();
                                auto results = engine->query().resume(nodes, query_json).execute();
                                json j_res = json::array();
                                for (const auto& row : results) {
                                    json jr = json::object();
                                    for (const auto& [k, v] : row.fields) jr[k] = v;
                                    j_res.push_back(jr);
                                }
                                std::string resp_json = j_res.dump();
                                sock.send(identity, zmq::send_flags::sndmore);
                                sock.send(zmq::message_t(), zmq::send_flags::sndmore);
                                sock.send(zmq::message_t(resp_json.data(), resp_json.size()), zmq::send_flags::none);
                            } catch (...) {
                                sock.send(identity, zmq::send_flags::sndmore);
                                sock.send(zmq::message_t(), zmq::send_flags::sndmore);
                                sock.send(zmq::message_t("[]", 2), zmq::send_flags::none);
                            }
                        } else if (opcode == "G") {
                            try {
                                std::string key = recv_msgs[4].to_string();
                                uint64_t id = 0;
                                if (key.starts_with("n:{") && key.size() >= 19) {
                                    id = std::stoull(key.substr(3, 16), nullptr, 16);
                                } else if (key.size() == 16) {
                                    id = std::stoull(key, nullptr, 16);
                                } else {
                                    // Assume it's a raw key
                                    auto buf = engine->get_store()->get(key);
                                    sock.send(identity, zmq::send_flags::sndmore);
                                    sock.send(zmq::message_t(), zmq::send_flags::sndmore);
                                    if (buf.size() > 0) sock.send(zmq::message_t(buf.data(), buf.size()), zmq::send_flags::none);
                                    else sock.send(zmq::message_t("", 0), zmq::send_flags::none);
                                    continue;
                                }

                                std::string node_key = std::string(KeyBuilder::node_key(id));
                                
                                // Authorization Check
                                auto perm = engine->get_store()->credentials().check_permission(current_uid, node_key);
                                if (!(perm & l3kv::Permission::READ) && !(perm & l3kv::Permission::ADMIN)) {
                                    sock.send(identity, zmq::send_flags::sndmore);
                                    sock.send(zmq::message_t(), zmq::send_flags::sndmore);
                                    sock.send(zmq::message_t("", 0), zmq::send_flags::none);
                                    continue;
                                }

                                auto buf = engine->get_store()->get(node_key);
                                sock.send(identity, zmq::send_flags::sndmore);
                                sock.send(zmq::message_t(), zmq::send_flags::sndmore);
                                if (buf.size() > 0) sock.send(zmq::message_t(buf.data(), buf.size()), zmq::send_flags::none);
                                else sock.send(zmq::message_t("", 0), zmq::send_flags::none);
                            } catch (...) {
                                sock.send(identity, zmq::send_flags::sndmore);
                                sock.send(zmq::message_t(), zmq::send_flags::sndmore);
                                sock.send(zmq::message_t("", 0), zmq::send_flags::none);
                            }
                        } else if (opcode == "P") {
                            std::string key = recv_msgs[4].to_string();
                            std::string payload = recv_msgs[5].to_string();
                            
                            // Authorization Check
                            auto perm = engine->get_store()->credentials().check_permission(current_uid, key);
                            if (!(perm & l3kv::Permission::WRITE) && !(perm & l3kv::Permission::ADMIN)) {
                                sock.send(identity, zmq::send_flags::sndmore);
                                sock.send(zmq::message_t(), zmq::send_flags::sndmore);
                                sock.send(zmq::message_t("ERR_FORBIDDEN", 13), zmq::send_flags::none);
                                continue;
                            }

                            engine->get_store()->put(key, payload);
                            sock.send(identity, zmq::send_flags::sndmore);
                            sock.send(zmq::message_t(), zmq::send_flags::sndmore);
                            sock.send(zmq::message_t("OK", 2), zmq::send_flags::none);
                        } else if (opcode == "H") {
                            sock.send(identity, zmq::send_flags::sndmore);
                            sock.send(zmq::message_t(), zmq::send_flags::sndmore);
                            sock.send(zmq::message_t("OK", 2), zmq::send_flags::none);
                        }
                    }
                } catch (...) {}
            });
        }

        void stop() {
            running = false;
            if (server_thread.joinable()) server_thread.join();
        }
    };

    std::unordered_map<uint32_t, std::shared_ptr<NodeHandle>> nodes;
    std::unordered_map<uint16_t, std::vector<uint32_t>> clusters;

    void add_node(uint32_t id, uint16_t cluster_id, uint16_t port, std::shared_ptr<lite3::ConsistentHash> ring, Settings settings = {}, std::string secret = "") {
        std::string db = "mesh_db_" + std::to_string(id);
        std::filesystem::remove_all(db);
        auto handle = std::make_shared<NodeHandle>(id, cluster_id, port, db, ring, settings, secret);
        nodes[id] = handle;
        clusters[cluster_id].push_back(id);
    }

    void start_all() {
        for (auto& [id, handle] : nodes) handle->start();
    }

    void stop_all() {
        for (auto& [id, handle] : nodes) {
            handle->stop();
        }
    }

    Engine* get_engine(uint32_t node_id) { return nodes.at(node_id)->engine.get(); }
};

class UltimateMeshTest : public ::testing::Test {
protected:
    GlobalMeshHarness harness;
    void TearDown() override {
        harness.stop_all();
    }
};

TEST_F(UltimateMeshTest, ComprehensiveScenario) {
    std::string secret = "global-mesh-secret-123";
    Settings settings;
    settings.fed_timeout_ms = 200;
    settings.breaker_failure_threshold = 2;
    settings.breaker_reset_timeout_ms = 1000;
    settings.health_check_interval_ms = 500;

    // Cluster 1 (US): Nodes 101, 102
    auto us_ring = std::make_shared<lite3::ConsistentHash>();
    us_ring->add_node(101);
    us_ring->add_node(102);
    harness.add_node(101, 1, 9101, us_ring, settings, secret);
    harness.add_node(102, 1, 9102, us_ring, settings, secret);

    // Cluster 2 (EU): Nodes 201, 202
    auto eu_ring = std::make_shared<lite3::ConsistentHash>();
    eu_ring->add_node(201);
    eu_ring->add_node(202);
    harness.add_node(201, 2, 9201, eu_ring, settings, secret);
    harness.add_node(202, 2, 9202, eu_ring, settings, secret);

    // Cluster 3 (CLOUD): Node 301
    auto cloud_ring = std::make_shared<lite3::ConsistentHash>();
    cloud_ring->add_node(301);
    harness.add_node(301, 3, 9301, cloud_ring, settings, secret);

    // Networking: Intra-cluster ZMQ
    harness.get_engine(101)->set_auth_secret(secret);
    harness.get_engine(101)->get_remote_client().add_peer(102, "tcp://127.0.0.1:9102");
    harness.get_engine(102)->set_auth_secret(secret);
    harness.get_engine(102)->get_remote_client().add_peer(101, "tcp://127.0.0.1:9101");
    harness.get_engine(201)->set_auth_secret(secret);
    harness.get_engine(201)->get_remote_client().add_peer(202, "tcp://127.0.0.1:9202");
    harness.get_engine(202)->set_auth_secret(secret);
    harness.get_engine(202)->get_remote_client().add_peer(201, "tcp://127.0.0.1:9201");

    // Networking: Inter-cluster Replication (US <-> EU)
    // Map ClusterIDs to entry-point NodeIDs for replication
    harness.get_engine(101)->get_remote_client().add_peer(2, "tcp://127.0.0.1:9201");
    harness.get_engine(102)->get_remote_client().add_peer(2, "tcp://127.0.0.1:9201");
    harness.get_engine(201)->get_remote_client().add_peer(1, "tcp://127.0.0.1:9101");
    harness.get_engine(202)->get_remote_client().add_peer(1, "tcp://127.0.0.1:9101");

    // TRUST DELEGATION: Register all nodes (including Cluster IDs) as ADMIN peers in all nodes
    for (auto const& [src_id, src_n] : harness.nodes) {
        for (auto const& [dst_id, dst_n] : harness.nodes) {
            if (src_id != dst_id) {
                dst_n->engine->get_store()->credentials().register_user(src_id, "peer", "key");
                dst_n->engine->get_store()->credentials().set_acl(src_id, "", l3kv::Permission::ADMIN);
            }
        }
        // Also trust Cluster IDs for replication
        for (uint16_t cid : {1, 2, 3}) {
            src_n->engine->get_store()->credentials().register_user(cid, "cluster", "key");
            src_n->engine->get_store()->credentials().set_acl(cid, "", l3kv::Permission::ADMIN);
        }
    }

    harness.get_engine(101)->get_resolver().register_federation("eu", 2, {"tcp://127.0.0.1:9201"});
    harness.get_engine(102)->get_resolver().register_federation("eu", 2, {"tcp://127.0.0.1:9201"});
    harness.get_engine(201)->get_resolver().register_federation("us", 1, {"tcp://127.0.0.1:9101"});
    harness.get_engine(202)->get_resolver().register_federation("us", 1, {"tcp://127.0.0.1:9101"});

    // Networking: Federation (US -> CLOUD)
    harness.get_engine(301)->set_auth_secret(secret);
    harness.get_engine(101)->get_remote_client().add_peer(3, "tcp://127.0.0.1:9301");
    harness.get_engine(101)->get_resolver().register_federation("cloud", 3, {"tcp://127.0.0.1:9301"});
    harness.get_engine(102)->get_remote_client().add_peer(3, "tcp://127.0.0.1:9301");
    harness.get_engine(102)->get_resolver().register_federation("cloud", 3, {"tcp://127.0.0.1:9301"});

    harness.start_all();
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));

    // --- FOUNDATIONAL SECURITY: Register Global User ---
    uint32_t user_id = 888;
    std::cout << "[Test] Registering global user 888 on Node 101..." << std::endl;
    harness.get_engine(101)->put_system_key("sys:u:888", "{\"name\":\"MeshUser\"}", ADMIN_UID);
    harness.get_engine(101)->put_system_key("sys:acl:888:", "READ", ADMIN_UID); // Global Read

    std::cout << "[Test] Waiting for global security replication..." << std::endl;
    std::this_thread::sleep_for(std::chrono::milliseconds(2000));

    // --- PHASE 1: Sharding & Local Consistency ---
    std::cout << "--- PHASE 1: Sharding ---" << std::endl;
    uint64_t nodeA_id = 0, nodeB_id = 0;
    for(uint64_t i=1; i<1000; ++i) {
        uint64_t id = FederationID::pack(1, i);
        if(us_ring->get_node(KeyBuilder::node_key(id)) == 101 && nodeA_id == 0) nodeA_id = id;
        if(us_ring->get_node(KeyBuilder::node_key(id)) == 102 && nodeB_id == 0) nodeB_id = id;
    }
    std::cout << "[Test] Chosen nodeA_id=" << std::hex << nodeA_id << " (owner 101), nodeB_id=" << nodeB_id << std::dec << " (owner 102)" << std::endl;

    harness.get_engine(101)->put_node(nodeA_id, R"({"name":"Alice"})");
    harness.get_engine(101)->put_node(nodeB_id, R"({"name":"Bob"})");

    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    EXPECT_GT(harness.get_engine(101)->get_store()->get(std::string(KeyBuilder::node_key(nodeA_id))).size(), 0);
    EXPECT_GT(harness.get_engine(102)->get_store()->get(std::string(KeyBuilder::node_key(nodeB_id))).size(), 0);

    // --- PHASE 2: Active-Active Replication ---
    std::cout << "--- PHASE 2: Replication ---" << std::endl;
    lite3::NodeID eu_owner = eu_ring->get_node(KeyBuilder::node_key(nodeA_id));
    bool replicated = false;
    for(int i=0; i<20; ++i) {
        if(harness.get_engine(eu_owner)->get_store()->get(std::string(KeyBuilder::node_key(nodeA_id))).size() > 0) {
            replicated = true; break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    EXPECT_TRUE(replicated);

    // --- PHASE 2.5: Conflict Resolution (LWW) ---
    std::cout << "--- PHASE 2.5: Conflict Resolution ---" << std::endl;
    harness.get_engine(101)->put_node(nodeA_id, R"({"name":"Alice_Older"})");
    auto ts_newer = harness.get_engine(eu_owner)->get_hlc().now();
    ts_newer.wall_time += 1000000;
    json j_newer; j_newer["name"] = "Alice_Newer"; j_newer["_hlc"] = json::parse(ts_newer.to_json_string());
    harness.get_engine(101)->replicate_key(std::string(KeyBuilder::node_key(nodeA_id)), j_newer.dump(), 2);
    
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    auto nodes_alice = harness.get_engine(101)->fetch_nodes({nodeA_id});
    ASSERT_FALSE(nodes_alice.empty());
    EXPECT_EQ(nodes_alice[0]->get_attribute<std::string>("name"), "Alice_Newer");

    // --- PHASE 3: Global Federation Traversal ---
    std::cout << "--- PHASE 3: Federation ---" << std::endl;
    uint64_t nodeC_id = FederationID::pack(3, 3000);
    harness.get_engine(301)->put_node(nodeC_id, R"({"name":"GlobalTruth"})");
    harness.get_engine(102)->add_edge(nodeB_id, "depends", 1.0, nodeC_id);
    harness.get_engine(101)->add_edge(nodeA_id, "knows", 1.0, nodeB_id);

    std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    auto results = harness.get_engine(101)->query()
        .set_principal_id(user_id)
        .match_id(nodeA_id, "a")
        .out("knows").as("b")
        .out("depends").as("target")
        .return_("target", "name")
        .execute();

    ASSERT_EQ(results.size(), 1);
    EXPECT_EQ(results[0].fields["target.name"], "GlobalTruth");

    // --- PHASE 4: Resilience ---
    std::cout << "--- PHASE 4: Resilience ---" << std::endl;
    harness.nodes[301]->stop(); // Kill CLOUD node

    auto start_res = std::chrono::steady_clock::now();
    bool threw = false;
    try {
        harness.get_engine(101)->query()
            .match_id(nodeA_id, "a")
            .out("knows").as("b")
            .out("depends").as("target")
            .execute();
    } catch (...) {
        threw = true;
    }

    auto end_res = std::chrono::steady_clock::now();
    EXPECT_TRUE(threw);
    EXPECT_LT(std::chrono::duration_cast<std::chrono::milliseconds>(end_res-start_res).count(), 1000);
}

} // namespace l3kvg

int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
