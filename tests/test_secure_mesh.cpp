#include "L3KVG/Engine.hpp"
#include "L3KVG/Node.hpp"
#include "L3KVG/Query.hpp"
#include "L3KVG/KeyBuilder.hpp"
#include "L3KVG/FederationID.hpp"
#include "L3KVG/QueryResult.hpp"
#include "engine/store.hpp"
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
            // Allow itself and peers
            engine->get_store()->credentials().register_user(id, "node", "key"); 
            engine->get_store()->credentials().set_acl(id, "*", l3kv::Permission::ADMIN);
        }
    }

    ~NodeHandle() {
        stop();
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
                    uint32_t session_uid = sessions.contains(identity_str) ? sessions[identity_str] : 0;
                    uint32_t current_uid = session_uid;

                    // If session has ADMIN perms, allow Principal Propagation
                    auto session_perm = engine->get_store()->credentials().check_permission(session_uid, "");
                    if (session_perm & l3kv::Permission::ADMIN) {
                        current_uid = effective_uid;
                    }

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

                    if (!auth_secret.empty() && !sessions.contains(identity_str) && opcode != "A") {
                        sock.send(identity, zmq::send_flags::sndmore);
                        sock.send(zmq::message_t(), zmq::send_flags::sndmore);
                        sock.send(zmq::message_t("ERR_UNAUTH", 10), zmq::send_flags::none);
                        continue;
                    }

                    if (opcode == "G") {
                        try {
                            uint64_t id = std::stoull(recv_msgs[4].to_string(), nullptr, 16);
                            std::string key = std::string(KeyBuilder::node_key(id));

                            // Authorization Check
                            auto perm = engine->get_store()->credentials().check_permission(current_uid, key);
                            if (!(perm & l3kv::Permission::READ) && !(perm & l3kv::Permission::ADMIN)) {
                                sock.send(identity, zmq::send_flags::sndmore);
                                sock.send(zmq::message_t(), zmq::send_flags::sndmore);
                                sock.send(zmq::message_t("", 0), zmq::send_flags::none);
                                continue;
                            }

                            auto buf = engine->get_store()->get(key);
                            sock.send(identity, zmq::send_flags::sndmore);
                            sock.send(zmq::message_t(), zmq::send_flags::sndmore);
                            if (buf.size() > 0) sock.send(zmq::message_t(buf.data(), buf.size()), zmq::send_flags::none);
                            else sock.send(zmq::message_t("", 0), zmq::send_flags::none);
                        } catch (...) {
                            sock.send(identity, zmq::send_flags::sndmore);
                            sock.send(zmq::message_t(), zmq::send_flags::sndmore);
                            sock.send(zmq::message_t("", 0), zmq::send_flags::none);
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
                    } else if (opcode == "S") {
                        try {
                            std::string key = recv_msgs[4].to_string();
                            std::string payload = recv_msgs[5].to_string();
                            uint16_t origin = 0;
                            if (recv_msgs.size() >= 7) origin = std::stoi(recv_msgs[6].to_string());
                            
                            engine->replicate_key(key, payload, origin);
                            sock.send(identity, zmq::send_flags::sndmore);
                            sock.send(zmq::message_t(), zmq::send_flags::sndmore);
                            sock.send(zmq::message_t("OK", 2), zmq::send_flags::none);
                        } catch (...) {
                            sock.send(identity, zmq::send_flags::sndmore);
                            sock.send(zmq::message_t(), zmq::send_flags::sndmore);
                            sock.send(zmq::message_t("ERR", 3), zmq::send_flags::none);
                        }
                    } else if (opcode == "P") {

                        try {
                            std::string key = recv_msgs[4].to_string();
                            std::string payload = recv_msgs[5].to_string();

                            // Enforce ACL in harness
                            auto perm = engine->get_store()->credentials().check_permission(current_uid, key);
                            if (!(perm & l3kv::Permission::WRITE) && !(perm & l3kv::Permission::ADMIN)) {
                                sock.send(identity, zmq::send_flags::sndmore);
                                sock.send(zmq::message_t(), zmq::send_flags::sndmore);
                                sock.send(zmq::message_t("ERR_FORBIDDEN", 13), zmq::send_flags::none);
                            } else {
                                engine->get_store()->put(key, payload);
                                sock.send(identity, zmq::send_flags::sndmore);
                                sock.send(zmq::message_t(), zmq::send_flags::sndmore);
                                sock.send(zmq::message_t("OK", 2), zmq::send_flags::none);
                            }
                        } catch (...) {
                            sock.send(identity, zmq::send_flags::sndmore);
                            sock.send(zmq::message_t(), zmq::send_flags::sndmore);
                            sock.send(zmq::message_t("ERR", 3), zmq::send_flags::none);
                        }
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

TEST(SecureMeshTest, UnauthorizedNodeRejected) {
    std::string secret = "top-secret-123";
    std::string db1 = "secure_db_1";
    std::string db2 = "secure_db_2";
    std::filesystem::remove_all(db1);
    std::filesystem::remove_all(db2);

    Settings settings;
    settings.fed_timeout_ms = 500;
    
    NodeHandle node1(1, 1, 9501, db1, nullptr, settings, secret);
    node1.start();
    
    settings.node_id = 2;
    NodeHandle node2(2, 2, 9502, db2, nullptr, settings, "");
    node2.start();

    node2.engine->get_remote_client().add_peer(1, "tcp://127.0.0.1:9501");
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    auto future = node2.engine->get_remote_client().put_node_async(1, 100, "{\"hacked\":true}");
    EXPECT_FALSE(future.get());
    
    auto payload_future = node2.engine->get_remote_client().get_node_payload_async(1, 100);
    EXPECT_TRUE(payload_future.get().empty());

    node1.stop();
    node2.stop();
    std::filesystem::remove_all(db1);
    std::filesystem::remove_all(db2);
}

TEST(SecureMeshTest, AuthorizedNodeSucceeds) {
    std::string secret = "top-secret-123";
    std::string db1 = "secure_db_1";
    std::string db2 = "secure_db_2";
    std::filesystem::remove_all(db1);
    std::filesystem::remove_all(db2);

    Settings settings;
    settings.fed_timeout_ms = 500;
    
    NodeHandle node1(1, 1, 9501, db1, nullptr, settings, secret);
    node1.engine->get_store()->credentials().register_user(2, "node2", "key2");
    node1.engine->get_store()->credentials().set_acl(2, "*", l3kv::Permission::ADMIN);
    node1.start();
    
    settings.node_id = 2;
    NodeHandle node2(2, 2, 9502, db2, nullptr, settings, secret);
    node2.start();

    node2.engine->get_remote_client().add_peer(1, "tcp://127.0.0.1:9501");
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    auto future = node2.engine->get_remote_client().put_node_async(1, 100, "{\"name\":\"Bob\"}");
    EXPECT_TRUE(future.get());
    
    auto payload_future = node2.engine->get_remote_client().get_node_payload_async(1, 100);
    std::string payload = payload_future.get();
    EXPECT_FALSE(payload.empty());
    EXPECT_TRUE(payload.find("Bob") != std::string::npos);

    node1.stop();
    node2.stop();
    std::filesystem::remove_all(db1);
    std::filesystem::remove_all(db2);
}

TEST(SecureMeshTest, NamespaceSandboxing) {
    std::string secret = "mesh-secret";
    std::string db1 = "sandbox_db_1";
    std::filesystem::remove_all(db1);

    Settings settings;
    NodeHandle node1(1, 1, 9601, db1, nullptr, settings, secret);
    
    node1.engine->get_store()->credentials().register_user(2, "cluster2", "key2");
    node1.engine->get_store()->credentials().set_acl(2, "n:{cluster2:", l3kv::Permission::WRITE | l3kv::Permission::READ);
    
    node1.start();

    settings.node_id = 2;
    NodeHandle node2(2, 2, 9602, "sandbox_db_2", nullptr, settings, secret);
    node2.start();
    node2.engine->get_remote_client().add_peer(1, "tcp://127.0.0.1:9601");

    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    EXPECT_TRUE(node2.engine->get_remote_client().put_edge_async(1, "n:{cluster2:node1}", "{\"val\":1}").get());
    EXPECT_FALSE(node2.engine->get_remote_client().put_edge_async(1, "n:{cluster1:node1}", "{\"val\":2}").get());

    node1.stop();
    node2.stop();
    std::filesystem::remove_all(db1);
    std::filesystem::remove_all("sandbox_db_2");
}

TEST(SecureMeshTest, PrincipalPropagation) {
    std::string secret = "mesh-secret";
    std::string db1 = "prop_db_1";
    std::string db2 = "prop_db_2";
    std::string db3 = "prop_db_3";
    std::filesystem::remove_all(db1);
    std::filesystem::remove_all(db2);
    std::filesystem::remove_all(db3);

    Settings settings;
    uint32_t user_id = 500;
    
    // 1. Setup Node 1 (Bridge)
    settings.node_id = 1;
    NodeHandle node1(1, 1, 9701, db1, nullptr, settings, secret);
    // User 500 must be known by the bridge node for local authorization
    node1.engine->get_store()->credentials().register_user(user_id, "User500", "key500");
    node1.engine->get_store()->credentials().set_acl(user_id, "n:{", l3kv::Permission::READ);
    node1.start();

    // 2. Setup Node 3 (Destination)
    settings.node_id = 3;
    NodeHandle node3(3, 3, 9703, db3, nullptr, settings, secret);
    node3.engine->get_store()->credentials().register_user(user_id, "User500", "key500");
    node3.engine->get_store()->credentials().set_acl(user_id, "n:{", l3kv::Permission::READ);
    
    // Node 1 (Bridge) must be a TRUSTED PEER in Node 3
    node3.engine->get_store()->credentials().register_user(1, "node1", "key1");
    node3.engine->get_store()->credentials().set_acl(1, "", l3kv::Permission::ADMIN);
    node3.start();

    // 3. Setup Node 2 (Initiator)
    settings.node_id = 2;
    NodeHandle node2(2, 2, 9702, db2, nullptr, settings, secret);
    node2.start();

    // Connectivity: Node 2 -> Node 1 -> Node 3
    node2.engine->get_remote_client().add_peer(1, "tcp://127.0.0.1:9701");
    
    node1.engine->get_remote_client().add_peer(3, "tcp://127.0.0.1:9703");
    node1.engine->get_resolver().register_federation("cloud", 3, {"tcp://127.0.0.1:9703"});

    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // 4. Node 2 initiates request as User 500
    std::cout << "[Test] Node 2 requesting Node 3 data via Node 1 as UID 500..." << std::endl;
    
    // Refined test: Node 3 has node with id=999 (Cluster 3)
    uint64_t target_id = FederationID::pack(3, 999);
    std::string target_key = std::string(KeyBuilder::node_key(target_id));
    node3.engine->get_store()->put(target_key, "{\"name\":\"Secret\"}");
    node3.engine->get_store()->credentials().set_acl(user_id, "n:{", l3kv::Permission::READ);

    std::cout << "[Test] Node 1 (Admin Session) fetching from Node 3 as UID 500 via Query..." << std::endl;
    auto results = node1.engine->query()
        .set_principal_id(user_id)
        .match_id(target_id, "s")
        .return_("s", "name")
        .execute();
    
    ASSERT_EQ(results.size(), 1);
    EXPECT_EQ(results[0].fields["s.name"], "Secret");
    std::cout << "[PASS] Principal ID propagated and authorized via Query" << std::endl;

    // 5. Test rejection: UID 666 (no perms)
    std::cout << "[Test] Node 1 (Admin Session) fetching from Node 3 as UID 666 via Query..." << std::endl;
    auto results_bad = node1.engine->query()
        .set_principal_id(666)
        .match_id(target_id, "s")
        .execute();
    EXPECT_TRUE(results_bad.empty()) << "Unauthorized Principal ID was accepted!";
    std::cout << "[PASS] Unauthorized Principal ID rejected" << std::endl;

    node1.stop();
    node2.stop();
    node3.stop();
    std::filesystem::remove_all(db1);
    std::filesystem::remove_all(db2);
    std::filesystem::remove_all(db3);
}

TEST(SecureMeshTest, GlobalSecurityReplication) {
    std::string secret = "mesh-secret";
    std::string db1 = "sec_rep_db_1";
    std::string db2 = "sec_rep_db_2";
    std::filesystem::remove_all(db1);
    std::filesystem::remove_all(db2);

    Settings settings;
    
    // 1. Setup Node 1 (US Cluster)
    settings.node_id = 1;
    NodeHandle node1(1, 1, 9801, db1, nullptr, settings, secret);
    node1.start();

    // 2. Setup Node 2 (EU Cluster)
    settings.node_id = 2;
    NodeHandle node2(2, 2, 9802, db2, nullptr, settings, secret);
    node2.start();

    // Connectivity: Node 1 <-> Node 2
    node1.engine->get_remote_client().add_peer(2, "tcp://127.0.0.1:9802");
    node1.engine->get_resolver().register_federation("EU", 2, {"tcp://127.0.0.1:9802"});

    node2.engine->get_remote_client().add_peer(1, "tcp://127.0.0.1:9801");
    node2.engine->get_resolver().register_federation("US", 1, {"tcp://127.0.0.1:9801"});

    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // 3. Register Global User 777 on Node 1 (Admin)
    uint32_t user_id = 777;
    std::string user_payload = "{\"uid\":777, \"name\":\"GlobalUser\"}";
    node1.engine->put_system_key("sys:u:777", user_payload, ADMIN_UID);
    
    // Set ACL for User 777 on Node 1
    node1.engine->put_system_key("sys:acl:777:n:{", "READ", ADMIN_UID);

    std::cout << "[Test] Global user 777 registered on Node 1. Waiting for broadcast..." << std::endl;
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    // 4. Verify Node 2 has the metadata locally
    auto node2_perm = node2.engine->get_store()->credentials().check_permission(user_id, "n:{any}");
    EXPECT_TRUE(node2_perm & l3kv::Permission::READ) << "Security metadata was not replicated to Node 2!";

    if (node2_perm & l3kv::Permission::READ) {
        std::cout << "[PASS] Security metadata replicated globally!" << std::endl;
    }

    node1.stop();
    node2.stop();
    std::filesystem::remove_all(db1);
    std::filesystem::remove_all(db2);
}

} // namespace l3kvg

int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
