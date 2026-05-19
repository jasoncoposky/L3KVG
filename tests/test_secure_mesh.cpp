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
                    auto opcode = recv_msgs[2].to_string();

                    if (opcode == "A" && recv_msgs.size() >= 5) {
                        uint32_t uid = std::stoul(recv_msgs[3].to_string());
                        std::string secret = recv_msgs[4].to_string();
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

                    if (opcode == "G") {
                        uint64_t id = std::stoull(recv_msgs[3].to_string(), nullptr, 16);
                        std::string key = std::string(KeyBuilder::node_key(id));
                        auto buf = engine->get_store()->get(key);
                        sock.send(identity, zmq::send_flags::sndmore);
                        sock.send(zmq::message_t(), zmq::send_flags::sndmore);
                        if (buf.size() > 0) sock.send(zmq::message_t(buf.data(), buf.size()), zmq::send_flags::none);
                        else sock.send(zmq::message_t("", 0), zmq::send_flags::none);
                    } else if (opcode == "P") {
                        std::string key = recv_msgs[3].to_string();
                        std::string payload = recv_msgs[4].to_string();
                        
                        // Enforce ACL in harness
                        uint32_t current_uid = sessions[identity_str];
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

} // namespace l3kvg

int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
