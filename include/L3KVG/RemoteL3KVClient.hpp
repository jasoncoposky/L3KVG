#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <future>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>
#include <zmq.hpp>
#include <lite3/ring.hpp>

#include "buffer.hpp"
#include "L3KVG/QueryResult.hpp"
#include "L3KVG/ThreadPool.hpp"
#include "L3KVG/Settings.hpp"
#include "engine/credential_manager.hpp"

namespace l3kvg {

class MutationBatch;

enum class CircuitState { CLOSED, OPEN, HALF_OPEN };

class CircuitBreakerOpenException : public std::runtime_error {
public:
    explicit CircuitBreakerOpenException(const std::string& msg) : std::runtime_error(msg) {}
};

class FederationTimeoutException : public std::runtime_error {
public:
    explicit FederationTimeoutException(const std::string& msg) : std::runtime_error(msg) {}
};

enum class GraphReadKind : uint8_t {
    NodePayload = 0,
    OutNeighbors = 1,
    InNeighbors = 2,
    RawKey = 3
};

struct GraphReadOp {
    GraphReadKind kind = GraphReadKind::NodePayload;
    uint64_t node_id = 0;
    std::string label_or_key;
    double min_weight = -999999.0;
};

struct GraphReadResult {
    bool found = false;
    std::string payload;              // populated for NodePayload / RawKey
    std::vector<uint64_t> neighbors;  // populated for OutNeighbors / InNeighbors
};

class RemoteL3KVClient {
public:
    using GraphReadKind = l3kvg::GraphReadKind;
    using GraphReadOp = l3kvg::GraphReadOp;
    using GraphReadResult = l3kvg::GraphReadResult;

    RemoteL3KVClient(const Settings& settings = {});
    virtual ~RemoteL3KVClient();

    void set_thread_pool(std::shared_ptr<ThreadPool> pool) { task_pool_ = std::move(pool); }
    void set_auth_secret(const std::string& secret);

    void add_peer(lite3::NodeID node_id, const std::string& endpoint_url);

    // Synchronous zero-overhead RPC methods (thread_local ZMQ_DEALER sockets, 0 ThreadPool hops)
    std::string get_node_payload(
        lite3::NodeID owner_id,
        uint64_t target_node_id,
        uint32_t principal_id = l3kv::INTERNAL_UID);

    lite3cpp::Buffer get_node_buffer(
        lite3::NodeID owner_id,
        uint64_t target_node_id,
        uint32_t principal_id = l3kv::INTERNAL_UID);

    std::vector<uint64_t> get_neighbors(
        lite3::NodeID owner_id,
        uint64_t target_node_id,
        std::string_view label,
        double min_weight = -999999.0,
        uint32_t principal_id = l3kv::INTERNAL_UID);

    std::vector<uint64_t> get_in_neighbors(
        lite3::NodeID owner_id,
        uint64_t target_node_id,
        std::string_view label,
        uint32_t principal_id = l3kv::INTERNAL_UID);

    std::string get_raw_key(
        lite3::NodeID owner_id,
        std::string_view key,
        uint32_t principal_id = l3kv::INTERNAL_UID);

    virtual std::vector<std::pair<std::string, std::string>> get_prefix_entries(
        lite3::NodeID owner_id,
        std::string_view prefix,
        uint32_t principal_id = l3kv::INTERNAL_UID);

    virtual std::vector<std::string> get_prefix_keys(
        lite3::NodeID owner_id,
        std::string_view prefix,
        uint32_t principal_id = l3kv::INTERNAL_UID);

    virtual bool execute_batch(
        lite3::NodeID owner_id,
        const lite3cpp::Buffer& batch_buffer,
        uint32_t principal_id = l3kv::INTERNAL_UID);

    bool execute_batch(
        lite3::NodeID owner_id,
        const MutationBatch& batch,
        uint32_t principal_id = l3kv::INTERNAL_UID);

    virtual lite3cpp::Buffer resume_query_buffer(
        uint16_t cluster_id,
        const std::vector<uint64_t>& starting_nodes,
        const lite3cpp::Buffer& query_buf,
        uint32_t principal_id = l3kv::INTERNAL_UID);

    virtual std::vector<GraphReadResult> execute_multi_read(
        lite3::NodeID owner_id,
        std::span<const GraphReadOp> ops,
        uint32_t principal_id = l3kv::INTERNAL_UID);

    // Asynchronous RPC wrappers
    std::future<std::vector<uint64_t>> get_neighbors_async(
        lite3::NodeID owner_id,
        uint64_t target_node_id,
        const std::string& label,
        double min_weight,
        uint32_t principal_id = l3kv::INTERNAL_UID);

    std::future<std::vector<uint64_t>> get_in_neighbors_async(
        lite3::NodeID owner_id,
        uint64_t target_node_id,
        const std::string& label,
        uint32_t principal_id = l3kv::INTERNAL_UID);

    virtual std::future<std::vector<ResultRow>> resume_query_async(
        uint16_t cluster_id,
        const std::vector<uint64_t>& starting_nodes,
        const lite3cpp::Buffer& query_buf,
        uint32_t principal_id = l3kv::INTERNAL_UID);

    virtual std::future<lite3cpp::Buffer> resume_query_buffer_async(
        uint16_t cluster_id,
        const std::vector<uint64_t>& starting_nodes,
        const lite3cpp::Buffer& query_buf,
        uint32_t principal_id = l3kv::INTERNAL_UID);

    virtual std::future<std::vector<ResultRow>> resume_query_async(
        uint16_t cluster_id,
        const std::vector<uint64_t>& starting_nodes,
        const std::string& query_payload,
        uint32_t principal_id = l3kv::INTERNAL_UID);

    std::future<bool> put_edge_async(
        lite3::NodeID owner_id,
        const std::string& edge_key,
        const std::string& json_payload,
        uint32_t principal_id = l3kv::INTERNAL_UID);

    std::future<std::string> get_node_payload_async(
        lite3::NodeID owner_id,
        uint64_t target_node_id,
        uint32_t principal_id = l3kv::INTERNAL_UID);

    std::future<lite3cpp::Buffer> get_node_buffer_async(
        lite3::NodeID owner_id,
        uint64_t target_node_id,
        uint32_t principal_id = l3kv::INTERNAL_UID);

    std::future<std::string> get_raw_key_async(
        lite3::NodeID owner_id,
        const std::string& key,
        uint32_t principal_id = l3kv::INTERNAL_UID);

    std::future<bool> put_raw_async(
        lite3::NodeID owner_id,
        const std::string& key,
        const std::string& value,
        uint32_t principal_id = l3kv::INTERNAL_UID) {
        return put_edge_async(owner_id, key, value, principal_id);
    }

    std::future<bool> del_raw_async(
        lite3::NodeID owner_id,
        const std::string& key,
        uint32_t principal_id = l3kv::INTERNAL_UID) {
        return del_edge_async(owner_id, key, principal_id);
    }

    virtual std::future<std::vector<std::pair<std::string, std::string>>> get_prefix_entries_async(
        lite3::NodeID owner_id,
        const std::string& prefix,
        uint32_t principal_id = l3kv::INTERNAL_UID);

    virtual std::future<std::vector<std::string>> get_prefix_keys_async(
        lite3::NodeID owner_id,
        const std::string& prefix,
        uint32_t principal_id = l3kv::INTERNAL_UID);

    std::future<uint64_t> atomic_incr_async(
        lite3::NodeID owner_id,
        const std::string& key,
        int64_t delta,
        uint32_t principal_id = l3kv::INTERNAL_UID);

    std::future<std::unordered_map<uint64_t, std::string>> get_nodes_batch_async(
        lite3::NodeID owner_id,
        const std::vector<uint64_t>& node_ids,
        uint32_t principal_id = l3kv::INTERNAL_UID);

    std::future<bool> put_node_async(
        lite3::NodeID owner_id,
        uint64_t target_node_id,
        const std::string& json_payload,
        uint32_t principal_id = l3kv::INTERNAL_UID);

    std::future<bool> put_node_async(
        lite3::NodeID owner_id,
        uint64_t target_node_id,
        const lite3cpp::Buffer& payload,
        uint32_t principal_id = l3kv::INTERNAL_UID);

    std::future<bool> del_node_async(
        lite3::NodeID owner_id,
        uint64_t target_node_id,
        uint32_t principal_id = l3kv::INTERNAL_UID);

    std::future<bool> del_edge_async(
        lite3::NodeID owner_id,
        const std::string& edge_key,
        uint32_t principal_id = l3kv::INTERNAL_UID);

    std::future<bool> put_batch_async(
        lite3::NodeID owner_id,
        const std::unordered_map<uint64_t, std::string>& batch,
        uint32_t principal_id = l3kv::INTERNAL_UID);

    virtual std::future<bool> put_batch_binary_async(
        lite3::NodeID owner_id,
        const lite3cpp::Buffer& batch_buffer,
        uint32_t principal_id = l3kv::INTERNAL_UID);

    virtual std::future<bool> execute_batch_async(
        lite3::NodeID owner_id,
        const MutationBatch& batch,
        uint32_t principal_id = l3kv::INTERNAL_UID);

    std::future<bool> replicate_async(
        uint16_t cluster_id,
        const std::string& key,
        const std::string& payload,
        uint16_t origin_cluster_id,
        uint32_t principal_id = l3kv::INTERNAL_UID);

    std::future<bool> ping_peer(lite3::NodeID node_id);

    // Circuit Breaker API
    CircuitState get_circuit_state(lite3::NodeID node_id);
    void set_circuit_state(lite3::NodeID node_id, CircuitState state);
    void report_failure(lite3::NodeID node_id);
    void report_success(lite3::NodeID node_id);

    struct TlSocketBox {
        std::mutex close_mu;
        std::unique_ptr<zmq::socket_t> socket;
        std::string endpoint;
        uint64_t config_epoch{0};
        bool authenticated{false};

        void close_socket() noexcept {
            std::lock_guard<std::mutex> lk(close_mu);
            if (socket) {
                try {
                    socket->set(zmq::sockopt::linger, 0);
                    socket->close();
                } catch (...) {}
                socket.reset();
            }
            authenticated = false;
        }
    };

    struct SocketRegistry {
        std::mutex mu;
        bool client_closed{false};
        std::unordered_set<std::shared_ptr<TlSocketBox>> active;
    };

private:
    struct PeerState {
        std::string endpoint;
        std::mutex state_mu;
        std::atomic<CircuitState> state{CircuitState::CLOSED};
        std::atomic<int> consecutive_failures{0};
        std::atomic<int64_t> last_failure_ns{0};
    };

    bool has_auth_secret();
    std::shared_ptr<PeerState> get_peer_state(lite3::NodeID node_id);
    TlSocketBox* get_or_create_tl_socket(lite3::NodeID node_id, std::shared_ptr<PeerState>* out_peer = nullptr, bool require_auth = true);
    void invalidate_tl_socket(lite3::NodeID node_id, TlSocketBox* box) noexcept;
    void ensure_socket_authenticated(TlSocketBox& box, lite3::NodeID node_id);
    void check_circuit(PeerState& peer);
    void run_health_check_loop();

    template <typename SendFn>
    std::vector<zmq::message_t>& send_and_recv_rpc(lite3::NodeID peer_id, SendFn&& send_fn, const char* timeout_msg);

    bool put_key_bytes_sync(lite3::NodeID owner_id, std::string_view key, const void* data, size_t len, uint32_t principal_id);
    bool del_key_sync(lite3::NodeID owner_id, std::string_view key, uint32_t principal_id);
    bool ping_peer_sync(lite3::NodeID node_id);
    std::vector<ResultRow> resume_query_rows_sync(uint16_t cluster_id, const std::vector<uint64_t>& starting_nodes, const lite3cpp::Buffer& query_buf, uint32_t principal_id);

    const uint64_t client_instance_id_;
    std::atomic<uint64_t> config_epoch_{1};

    Settings settings_;
    std::string auth_secret_;
    std::unordered_map<lite3::NodeID, std::shared_ptr<PeerState>> peers_;
    std::shared_ptr<SocketRegistry> socket_registry_;
    std::mutex endpoints_mutex_;
    int zmq_sndhwm_;
    zmq::context_t zmq_ctx_;
    std::shared_ptr<ThreadPool> task_pool_;

    std::atomic<bool> stop_health_check_{false};
    std::mutex health_check_cv_mu_;
    std::condition_variable health_check_cv_;
    std::thread health_check_thread_;
    pid_t creator_pid_{0};
};

} // namespace l3kvg
