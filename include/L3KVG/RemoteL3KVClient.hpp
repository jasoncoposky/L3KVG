#pragma once

#include <string>
#include <vector>
#include <future>
#include <unordered_map>
#include <mutex>
#include <memory>
#include <zmq.hpp>
#include <lite3/ring.hpp>
#include <cstdint>
#include <atomic>
#include <chrono>
#include "buffer.hpp"
#include "L3KVG/QueryResult.hpp"
#include "L3KVG/ThreadPool.hpp"
#include "L3KVG/Settings.hpp"
#include "engine/credential_manager.hpp"


namespace l3kvg {

enum class CircuitState { CLOSED, OPEN, HALF_OPEN };

class CircuitBreakerOpenException : public std::runtime_error {
public:
    explicit CircuitBreakerOpenException(const std::string& msg) : std::runtime_error(msg) {}
};

class FederationTimeoutException : public std::runtime_error {
public:
    explicit FederationTimeoutException(const std::string& msg) : std::runtime_error(msg) {}
};

class RemoteL3KVClient {
public:
    RemoteL3KVClient(const Settings& settings = {});
    virtual ~RemoteL3KVClient();

    void set_thread_pool(std::shared_ptr<ThreadPool> pool) { task_pool_ = std::move(pool); }
    void set_auth_secret(const std::string& secret) { auth_secret_ = secret; }

    void add_peer(lite3::NodeID node_id, const std::string& endpoint_url);

    std::future<std::vector<uint64_t>> get_neighbors_async(
        lite3::NodeID owner_id,
        uint64_t target_node_id, 
        const std::string& label,
        double min_weight,
        uint32_t principal_id = l3kv::INTERNAL_UID
    );

    std::future<std::vector<uint64_t>> get_in_neighbors_async(
        lite3::NodeID owner_id,
        uint64_t target_node_id, 
        const std::string& label,
        uint32_t principal_id = l3kv::INTERNAL_UID
    );

    virtual std::future<std::vector<ResultRow>> resume_query_async(
        uint16_t cluster_id,
        const std::vector<uint64_t>& starting_nodes,
        const std::string& query_json,
        uint32_t principal_id = l3kv::INTERNAL_UID
    );

    std::future<bool> put_edge_async(
        lite3::NodeID owner_id,
        const std::string& edge_key, 
        const std::string& json_payload,
        uint32_t principal_id = l3kv::INTERNAL_UID
    );

    std::future<std::string> get_node_payload_async(
        lite3::NodeID owner_id,
        uint64_t target_node_id,
        uint32_t principal_id = l3kv::INTERNAL_UID
    );

    // Batch Fetch: Reduces network roundtrips by coalescing requests to the same peer.
    std::future<std::unordered_map<uint64_t, std::string>> get_nodes_batch_async(
        lite3::NodeID owner_id,
        const std::vector<uint64_t>& node_ids,
        uint32_t principal_id = l3kv::INTERNAL_UID
    );

    std::future<bool> put_node_async(
        lite3::NodeID owner_id,
        uint64_t target_node_id, 
        const std::string& json_payload,
        uint32_t principal_id = l3kv::INTERNAL_UID
    );

    std::future<bool> del_node_async(
        lite3::NodeID owner_id,
        uint64_t target_node_id,
        uint32_t principal_id = l3kv::INTERNAL_UID
    );

    std::future<bool> del_edge_async(
        lite3::NodeID owner_id,
        const std::string& edge_key,
        uint32_t principal_id = l3kv::INTERNAL_UID
    );
    
    std::future<bool> put_batch_async(
        lite3::NodeID owner_id,
        const std::unordered_map<uint64_t, std::string>& batch,
        uint32_t principal_id = l3kv::INTERNAL_UID
    );

    std::future<bool> put_batch_binary_async(
        lite3::NodeID owner_id,
        const lite3cpp::Buffer& batch_buffer,
        uint32_t principal_id = l3kv::INTERNAL_UID
    );

    // Replication API
    std::future<bool> replicate_async(
        uint16_t cluster_id,
        const std::string& key,
        const std::string& payload,
        uint16_t origin_cluster_id,
        uint32_t principal_id = l3kv::INTERNAL_UID
    );

    // Diagnostics
    std::future<bool> ping_peer(lite3::NodeID node_id);

    // Circuit Breaker API
    CircuitState get_circuit_state(lite3::NodeID node_id);
    void set_circuit_state(lite3::NodeID node_id, CircuitState state);
    void report_failure(lite3::NodeID node_id);
    void report_success(lite3::NodeID node_id);

private:
    struct Session {
        std::recursive_mutex mu;
        std::unique_ptr<zmq::socket_t> socket;
        bool connected = false;
        std::atomic<bool> authenticated{false};
        std::atomic<CircuitState> state{CircuitState::CLOSED};
        std::atomic<int> consecutive_failures{0};
        std::chrono::steady_clock::time_point last_failure_time;
    };

    std::shared_ptr<Session> get_session(lite3::NodeID node_id);
    void ensure_authenticated(std::shared_ptr<Session> session, lite3::NodeID node_id);
    void check_circuit(std::shared_ptr<Session> session);
    void run_health_check_loop();

    Settings settings_;
    std::string auth_secret_;
    std::unordered_map<lite3::NodeID, std::string> peer_endpoints_;
    std::unordered_map<lite3::NodeID, std::shared_ptr<Session>> peer_sessions_;
    std::mutex endpoints_mutex_;
    std::shared_ptr<ThreadPool> task_pool_;
    
    int zmq_sndhwm_;
    zmq::context_t zmq_ctx_;

    std::atomic<bool> stop_health_check_{false};
    std::thread health_check_thread_;
};


} // namespace l3kvg
