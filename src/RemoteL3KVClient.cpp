#include "L3KVG/RemoteL3KVClient.hpp"
#include "L3KVG/MutationBatch.hpp"
#include "L3KVG/KeyBuilder.hpp"
#include "json.hpp"

#include <charconv>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <thread>
#include <unistd.h>
#include <zmq_addon.hpp>

#ifdef IRODS_SERVER
#include "irods/rodsLog.h"
#endif

namespace l3kvg {

namespace {

std::atomic<uint64_t> s_next_client_instance_id{1};

int64_t steady_now_ns() noexcept {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

struct TlSocketKey {
    uint64_t client_id{0};
    lite3::NodeID node_id{0};
    bool operator==(const TlSocketKey& o) const noexcept {
        return client_id == o.client_id && node_id == o.node_id;
    }
};

struct TlSocketKeyHash {
    size_t operator()(const TlSocketKey& k) const noexcept {
        uint64_t x = k.client_id ^ (static_cast<uint64_t>(k.node_id) * 0x9E3779B97F4A7C15ULL);
        x ^= x >> 33;
        x *= 0xff51afd7ed558ccdULL;
        x ^= x >> 33;
        return static_cast<size_t>(x);
    }
};

struct TlSocketEntry {
    std::weak_ptr<RemoteL3KVClient::SocketRegistry> weak_registry;
    std::shared_ptr<RemoteL3KVClient::TlSocketBox> box;

    TlSocketEntry() = default;
    TlSocketEntry(std::weak_ptr<RemoteL3KVClient::SocketRegistry> reg,
                  std::shared_ptr<RemoteL3KVClient::TlSocketBox> b)
        : weak_registry(std::move(reg)), box(std::move(b)) {}

    TlSocketEntry(const TlSocketEntry&) = delete;
    TlSocketEntry& operator=(const TlSocketEntry&) = delete;

    TlSocketEntry(TlSocketEntry&& o) noexcept
        : weak_registry(std::move(o.weak_registry)), box(std::move(o.box)) {}

    TlSocketEntry& operator=(TlSocketEntry&& o) noexcept {
        if (this != &o) {
            cleanup();
            weak_registry = std::move(o.weak_registry);
            box = std::move(o.box);
        }
        return *this;
    }

    ~TlSocketEntry() {
        cleanup();
    }

    void cleanup() noexcept {
        if (!box) return;
        if (auto reg = weak_registry.lock()) {
            bool should_close = false;
            {
                std::lock_guard<std::mutex> lk(reg->mu);
                if (!reg->client_closed) {
                    reg->active.erase(box);
                    should_close = true;
                }
            }
            if (should_close) {
                box->close_socket();
            }
        }
        box.reset();
        weak_registry.reset();
    }
};

using TlSocketMap = std::unordered_map<TlSocketKey, TlSocketEntry, TlSocketKeyHash>;

TlSocketMap& get_tl_socket_map() {
    thread_local TlSocketMap s_tl_map;
    return s_tl_map;
}

std::vector<zmq::message_t>& get_tl_recv_msgs() {
    thread_local std::vector<zmq::message_t> s_recv_msgs;
    s_recv_msgs.clear();
    return s_recv_msgs;
}

lite3cpp::Buffer clone_or_copy_buffer(const lite3cpp::Buffer& src) {
    if (src.is_view()) {
        return lite3cpp::Buffer(src.data(), src.size());
    }
    return src;
}

bool is_unauth_frame(const zmq::message_t& msg) noexcept {
    return msg.size() == 10 && std::memcmp(msg.data(), "ERR_UNAUTH", 10) == 0;
}

bool is_acl_err_frame(const zmq::message_t& msg) noexcept {
    return (msg.size() == 13 && std::memcmp(msg.data(), "ERR_FORBIDDEN", 13) == 0) ||
           (msg.size() == 8 && std::memcmp(msg.data(), "ERR_AUTH", 8) == 0) ||
           is_unauth_frame(msg);
}

bool is_server_err_frame(const zmq::message_t& msg) noexcept {
    return msg.size() == 3 && std::memcmp(msg.data(), "ERR", 3) == 0;
}

bool is_err_frame(const zmq::message_t& msg) noexcept {
    return is_server_err_frame(msg) ||
           (msg.size() >= 4 && std::memcmp(msg.data(), "ERR_", 4) == 0);
}

bool is_json_sv(std::string_view data) noexcept {
    size_t first_non_ws = data.find_first_not_of(" \t\r\n");
    return first_non_ws != std::string_view::npos &&
           (data[first_non_ws] == '{' || data[first_non_ws] == '[');
}

std::string_view read_iter_string(const lite3cpp::Buffer& buf, const lite3cpp::Iterator::value_type& val) noexcept {
    if (val.value_type != lite3cpp::Type::String) return {};
    if (val.value_offset + 5 > buf.size()) return {};
    uint32_t sz = 0;
    std::memcpy(&sz, buf.data() + val.value_offset + 1, 4);
    if (val.value_offset + 5 + sz > buf.size()) return {};
    return std::string_view(reinterpret_cast<const char*>(buf.data() + val.value_offset + 5), sz);
}

std::vector<uint64_t> parse_neighbors_message(const zmq::message_t& msg) {
    std::vector<uint64_t> results;
    if (msg.size() == 0) return results;

    std::string_view sv(static_cast<const char*>(msg.data()), msg.size());
    if (msg.size() % sizeof(uint64_t) != 0 || (!sv.empty() && sv.front() == '[')) {
        size_t first = sv.find_first_not_of(" \t\r\n");
        size_t last = sv.find_last_not_of(" \t\r\n");
        if (first != std::string_view::npos && last != std::string_view::npos &&
            last >= first + 1 && sv[first] == '[' && sv[last] == ']') {
            try {
                lite3cpp::Buffer buf = lite3cpp::lite3_json::from_json_string(sv.substr(first, last - first + 1));
                if (buf.size() >= sizeof(lite3cpp::PackedNodeLayout)) {
                    lite3cpp::NodeView nv(reinterpret_cast<const lite3cpp::PackedNodeLayout*>(buf.data()));
                    if (nv.type() == lite3cpp::Type::Array) {
                        results.reserve(nv.size());
                        for (auto it = buf.begin(0), end = buf.end(0); it != end; ++it) {
                            if (it->value_type == lite3cpp::Type::Int64 && it->value_offset + 9 <= buf.size()) {
                                int64_t iv = 0;
                                std::memcpy(&iv, buf.data() + it->value_offset + 1, 8);
                                results.push_back(static_cast<uint64_t>(iv));
                            } else if (it->value_type == lite3cpp::Type::String) {
                                std::string_view hex_sv = read_iter_string(buf, *it);
                                uint64_t uid = 0;
                                auto [ptr, ec] = std::from_chars(hex_sv.data(), hex_sv.data() + hex_sv.size(), uid, 16);
                                if (ec == std::errc{} && ptr == hex_sv.data() + hex_sv.size()) {
                                    results.push_back(uid);
                                }
                            }
                        }
                        return results;
                    }
                }
            } catch (...) {}
        }
    }

    if (msg.size() % sizeof(uint64_t) == 0) {
        size_t count = msg.size() / sizeof(uint64_t);
        results.resize(count);
        if (count > 0) {
            std::memcpy(results.data(), msg.data(), count * sizeof(uint64_t));
        }
    }
    return results;
}

} // namespace

RemoteL3KVClient::RemoteL3KVClient(const Settings& settings)
    : client_instance_id_(s_next_client_instance_id.fetch_add(1, std::memory_order_relaxed)),
      settings_(settings),
      socket_registry_(std::make_shared<SocketRegistry>()),
      zmq_sndhwm_(settings.zmq_sndhwm),
      zmq_ctx_(1),
      creator_pid_(getpid()) {
    health_check_thread_ = std::thread(&RemoteL3KVClient::run_health_check_loop, this);
}

RemoteL3KVClient::~RemoteL3KVClient() {
    if (getpid() != creator_pid_) {
        if (health_check_thread_.joinable()) {
            health_check_thread_.detach();
        }
        return;
    }

    stop_health_check_.store(true, std::memory_order_release);
    health_check_cv_.notify_all();
    if (health_check_thread_.joinable()) {
        health_check_thread_.join();
    }

    // 1. Join async worker pool before closing ZMQ sockets
    task_pool_.reset();

    // 2. Close all active per-thread ZMQ sockets registered with this client
    std::unordered_set<std::shared_ptr<TlSocketBox>> to_close;
    if (socket_registry_) {
        std::lock_guard<std::mutex> reg_lk(socket_registry_->mu);
        socket_registry_->client_closed = true;
        to_close.swap(socket_registry_->active);
    }
    for (auto& box : to_close) {
        if (box) {
            box->close_socket();
        }
    }
    {
        std::lock_guard<std::mutex> lock(endpoints_mutex_);
        peers_.clear();
    }

    // 3. Shutdown zmq_ctx_ cleanly
    try {
        zmq_ctx_.shutdown();
        zmq_ctx_.close();
    } catch (...) {}
}

void RemoteL3KVClient::set_auth_secret(const std::string& secret) {
    std::lock_guard<std::mutex> lock(endpoints_mutex_);
    if (auth_secret_ != secret) {
        auth_secret_ = secret;
        config_epoch_.fetch_add(1, std::memory_order_release);
    }
}

bool RemoteL3KVClient::has_auth_secret() {
    std::lock_guard<std::mutex> lock(endpoints_mutex_);
    return !auth_secret_.empty();
}

void RemoteL3KVClient::add_peer(lite3::NodeID node_id, const std::string& endpoint_url) {
    std::string url = endpoint_url;
    if (url.starts_with("http://")) {
        url = url.substr(7);
        size_t colon = url.find(':');
        if (colon != std::string::npos) {
            int port = std::stoi(url.substr(colon + 1));
            url = url.substr(0, colon + 1) + std::to_string(port + 1);
        }
    }
    if (url.find("tcp://") == std::string::npos) {
        url = "tcp://" + url;
    }

    std::lock_guard<std::mutex> lock(endpoints_mutex_);
    auto peer = std::make_shared<PeerState>();
    peer->endpoint = std::move(url);
    peers_[node_id] = std::move(peer);
    config_epoch_.fetch_add(1, std::memory_order_release);
}

std::shared_ptr<RemoteL3KVClient::PeerState> RemoteL3KVClient::get_peer_state(lite3::NodeID node_id) {
    std::lock_guard<std::mutex> lock(endpoints_mutex_);
    auto it = peers_.find(node_id);
    return (it != peers_.end()) ? it->second : nullptr;
}

void RemoteL3KVClient::check_circuit(PeerState& peer) {
    if (peer.state.load(std::memory_order_acquire) == CircuitState::OPEN) {
        const int64_t elapsed_ms = (steady_now_ns() - peer.last_failure_ns.load(std::memory_order_acquire)) / 1000000LL;
        if (elapsed_ms > settings_.breaker_reset_timeout_ms) {
            std::lock_guard<std::mutex> lk(peer.state_mu);
            if (peer.state.load(std::memory_order_relaxed) == CircuitState::OPEN) {
                peer.state.store(CircuitState::HALF_OPEN, std::memory_order_release);
            }
        } else {
            throw CircuitBreakerOpenException("Circuit breaker is OPEN for this peer");
        }
    }
}

void RemoteL3KVClient::invalidate_tl_socket(lite3::NodeID node_id, TlSocketBox* box) noexcept {
    if (box) {
        box->close_socket();
    }
    auto& tl_map = get_tl_socket_map();
    tl_map.erase(TlSocketKey{client_instance_id_, node_id});
}

void RemoteL3KVClient::ensure_socket_authenticated(TlSocketBox& box, lite3::NodeID node_id) {
    if (box.authenticated) return;

    std::string secret_copy;
    {
        std::lock_guard<std::mutex> lock(endpoints_mutex_);
        secret_copy = auth_secret_;
    }
    if (secret_copy.empty()) {
        box.authenticated = true;
        return;
    }

    try {
        char uid_buf[16];
        auto [uptr, uec] = std::to_chars(uid_buf, uid_buf + sizeof(uid_buf), settings_.node_id);
        const size_t uid_len = (uec == std::errc{}) ? static_cast<size_t>(uptr - uid_buf) : 0;

        box.socket->send(zmq::message_t(), zmq::send_flags::sndmore);
        uint32_t dummy_uid = 0;
        box.socket->send(zmq::message_t(&dummy_uid, 4), zmq::send_flags::sndmore);
        box.socket->send(zmq::message_t("A", 1), zmq::send_flags::sndmore);
        box.socket->send(zmq::message_t(uid_buf, uid_len), zmq::send_flags::sndmore);
        box.socket->send(zmq::message_t(secret_copy.data(), secret_copy.size()), zmq::send_flags::none);

        auto& recv_msgs = get_tl_recv_msgs();
        auto res = zmq::recv_multipart(*box.socket, std::back_inserter(recv_msgs));
        if (res && recv_msgs.size() >= 2 &&
            recv_msgs[1].size() == 2 && std::memcmp(recv_msgs[1].data(), "OK", 2) == 0) {
            box.authenticated = true;
            return;
        }
        std::string err = (res && recv_msgs.size() >= 2) ? recv_msgs[1].to_string() : "TIMEOUT";
        throw std::runtime_error("Authentication failed: " + err);
    } catch (...) {
        invalidate_tl_socket(node_id, &box);
        report_failure(node_id);
        throw;
    }
}

RemoteL3KVClient::TlSocketBox* RemoteL3KVClient::get_or_create_tl_socket(
    lite3::NodeID node_id,
    std::shared_ptr<PeerState>* out_peer,
    bool require_auth) {
    auto& tl_map = get_tl_socket_map();
    const uint64_t curr_epoch = config_epoch_.load(std::memory_order_acquire);
    const TlSocketKey key{client_instance_id_, node_id};

    // Fast path: thread_local lookup without locking endpoints_mutex_
    if (auto it = tl_map.find(key); it != tl_map.end()) {
        auto* box = it->second.box.get();
        if (box && box->socket && box->config_epoch == curr_epoch && !it->second.weak_registry.expired()) {
            auto peer = get_peer_state(node_id);
            if (!peer) {
                tl_map.erase(it);
                return nullptr;
            }
            if (out_peer) *out_peer = peer;
            check_circuit(*peer);
            if (require_auth && !box->authenticated) {
                ensure_socket_authenticated(*box, node_id);
            }
            return box;
        }
        tl_map.erase(it);
    }

    // Prune any entries belonging to destroyed clients
    if (tl_map.size() > 64) {
        for (auto it = tl_map.begin(); it != tl_map.end();) {
            if (it->second.weak_registry.expired() || !it->second.box || !it->second.box->socket) {
                it = tl_map.erase(it);
            } else {
                ++it;
            }
        }
    }

    std::shared_ptr<PeerState> peer;
    {
        std::lock_guard<std::mutex> lock(endpoints_mutex_);
        auto pit = peers_.find(node_id);
        if (pit == peers_.end()) {
            return nullptr;
        }
        peer = pit->second;
    }

    if (out_peer) *out_peer = peer;
    check_circuit(*peer);

    auto new_box = std::make_shared<TlSocketBox>();
    new_box->endpoint = peer->endpoint;
    new_box->config_epoch = config_epoch_.load(std::memory_order_acquire);
    new_box->socket = std::make_unique<zmq::socket_t>(zmq_ctx_, ZMQ_DEALER);
    new_box->socket->set(zmq::sockopt::sndhwm, zmq_sndhwm_);
    new_box->socket->set(zmq::sockopt::linger, 0);
    new_box->socket->set(zmq::sockopt::rcvtimeo, settings_.fed_timeout_ms);
    new_box->socket->set(zmq::sockopt::sndtimeo, settings_.fed_timeout_ms);
    new_box->socket->connect(new_box->endpoint);

    {
        std::lock_guard<std::mutex> reg_lk(socket_registry_->mu);
        if (socket_registry_->client_closed) {
            new_box->close_socket();
            return nullptr;
        }
        socket_registry_->active.insert(new_box);
    }

    TlSocketBox* raw_box = new_box.get();
    tl_map.emplace(key, TlSocketEntry(socket_registry_, std::move(new_box)));

    if (require_auth) {
        ensure_socket_authenticated(*raw_box, node_id);
    }
    return raw_box;
}

template <typename SendFn>
std::vector<zmq::message_t>& RemoteL3KVClient::send_and_recv_rpc(
    lite3::NodeID peer_id,
    SendFn&& send_fn,
    const char* timeout_msg) {
    const bool can_reauth = has_auth_secret();
    for (int attempt = 0; attempt < 2; ++attempt) {
        TlSocketBox* box = get_or_create_tl_socket(peer_id);
        if (!box || !box->socket) {
            throw std::runtime_error("Peer not connected");
        }
        auto& recv_msgs = get_tl_recv_msgs();
        bool io_ok = false;
        try {
            send_fn(*box->socket);
            auto res = zmq::recv_multipart(*box->socket, std::back_inserter(recv_msgs));
            io_ok = (res.has_value() && recv_msgs.size() >= 2);
        } catch (...) {
            invalidate_tl_socket(peer_id, box);
            report_failure(peer_id);
            throw;
        }
        if (!io_ok) {
            invalidate_tl_socket(peer_id, box);
            report_failure(peer_id);
            throw FederationTimeoutException(timeout_msg);
        }
        if (attempt == 0 && can_reauth && is_unauth_frame(recv_msgs[1])) {
            box->authenticated = false;
            ensure_socket_authenticated(*box, peer_id);
            continue;
        }
        return recv_msgs;
    }
    throw FederationTimeoutException(timeout_msg);
}

void RemoteL3KVClient::run_health_check_loop() {
    while (!stop_health_check_.load(std::memory_order_acquire)) {
        {
            std::unique_lock<std::mutex> lk(health_check_cv_mu_);
            health_check_cv_.wait_for(
                lk,
                std::chrono::milliseconds(settings_.health_check_interval_ms),
                [this] { return stop_health_check_.load(std::memory_order_acquire); });
        }
        if (stop_health_check_.load(std::memory_order_acquire)) break;

        std::vector<std::pair<lite3::NodeID, std::shared_ptr<PeerState>>> to_check;
        {
            std::lock_guard<std::mutex> lock(endpoints_mutex_);
            for (auto& [id, peer] : peers_) {
                CircuitState st = peer->state.load(std::memory_order_acquire);
                if (st == CircuitState::HALF_OPEN) {
                    to_check.emplace_back(id, peer);
                } else if (st == CircuitState::OPEN) {
                    const int64_t elapsed_ms = (steady_now_ns() - peer->last_failure_ns.load(std::memory_order_acquire)) / 1000000LL;
                    if (elapsed_ms > settings_.breaker_reset_timeout_ms) {
                        to_check.emplace_back(id, peer);
                    }
                }
            }
        }

        for (auto& [id, peer] : to_check) {
            if (stop_health_check_.load(std::memory_order_acquire)) break;
            if (peer->state.load(std::memory_order_acquire) == CircuitState::OPEN) {
                std::lock_guard<std::mutex> lk(peer->state_mu);
                if (peer->state.load(std::memory_order_relaxed) == CircuitState::OPEN) {
                    peer->state.store(CircuitState::HALF_OPEN, std::memory_order_release);
                }
            }
            if (peer->state.load(std::memory_order_acquire) == CircuitState::HALF_OPEN) {
                if (ping_peer_sync(id)) {
                    report_success(id);
                } else {
                    report_failure(id);
                }
            }
        }
    }
}

// ============================================================================
// Synchronous Zero-Overhead RPC Implementations
// ============================================================================

std::string RemoteL3KVClient::get_raw_key(lite3::NodeID owner_id, std::string_view key, uint32_t principal_id) {
    if (!get_peer_state(owner_id)) return {};

    auto& recv_msgs = send_and_recv_rpc(
        owner_id,
        [&](zmq::socket_t& sock) {
            sock.send(zmq::message_t(), zmq::send_flags::sndmore);
            uint32_t pid = principal_id;
            sock.send(zmq::message_t(&pid, 4), zmq::send_flags::sndmore);
            sock.send(zmq::message_t("G", 1), zmq::send_flags::sndmore);
            sock.send(zmq::message_t(key.data(), key.size()), zmq::send_flags::none);
        },
        "Raw key fetch timed out");

    const auto& msg = recv_msgs[1];
    if (is_err_frame(msg)) {
        if (is_server_err_frame(msg)) report_failure(owner_id);
        return {};
    }
    report_success(owner_id);
    return std::string(static_cast<const char*>(msg.data()), msg.size());
}

std::string RemoteL3KVClient::get_node_payload(lite3::NodeID owner_id, uint64_t target_node_id, uint32_t principal_id) {
    return get_raw_key(owner_id, KeyBuilder::node_key(target_node_id), principal_id);
}

lite3cpp::Buffer RemoteL3KVClient::get_node_buffer(lite3::NodeID owner_id, uint64_t target_node_id, uint32_t principal_id) {
    if (!get_peer_state(owner_id)) return {};
    std::string_view key = KeyBuilder::node_key(target_node_id);

    auto& recv_msgs = send_and_recv_rpc(
        owner_id,
        [&](zmq::socket_t& sock) {
            sock.send(zmq::message_t(), zmq::send_flags::sndmore);
            uint32_t pid = principal_id;
            sock.send(zmq::message_t(&pid, 4), zmq::send_flags::sndmore);
            sock.send(zmq::message_t("G", 1), zmq::send_flags::sndmore);
            sock.send(zmq::message_t(key.data(), key.size()), zmq::send_flags::none);
        },
        "Node buffer fetch timed out");

    const auto& msg = recv_msgs[1];
    if (msg.size() == 0) {
        report_success(owner_id);
        return {};
    }
    if (is_err_frame(msg)) {
        if (is_server_err_frame(msg)) report_failure(owner_id);
        return {};
    }
    report_success(owner_id);
    const uint8_t* ptr = static_cast<const uint8_t*>(msg.data());
    if (msg.size() >= sizeof(lite3cpp::PackedNodeLayout) && (ptr[0] == 0x06 || ptr[0] == 0x07)) {
        return lite3cpp::Buffer(ptr, msg.size());
    }
    std::string_view sv(reinterpret_cast<const char*>(ptr), msg.size());
    if (is_json_sv(sv)) {
        return lite3cpp::lite3_json::from_json_string(sv);
    }
    return lite3cpp::Buffer(ptr, msg.size());
}

std::vector<uint64_t> RemoteL3KVClient::get_neighbors(
    lite3::NodeID owner_id,
    uint64_t target_node_id,
    std::string_view label,
    double min_weight,
    uint32_t principal_id) {
    if (!get_peer_state(owner_id)) return {};

    char id_buf[17];
    std::snprintf(id_buf, sizeof(id_buf), "%016llx", static_cast<unsigned long long>(target_node_id));
    char w_buf[32];
    int w_len = std::snprintf(w_buf, sizeof(w_buf), "%.6f", min_weight);
    if (w_len < 0) w_len = 0;

    auto& recv_msgs = send_and_recv_rpc(
        owner_id,
        [&](zmq::socket_t& sock) {
            sock.send(zmq::message_t(), zmq::send_flags::sndmore);
            uint32_t pid = principal_id;
            sock.send(zmq::message_t(&pid, 4), zmq::send_flags::sndmore);
            sock.send(zmq::message_t("N", 1), zmq::send_flags::sndmore);
            sock.send(zmq::message_t(id_buf, 16), zmq::send_flags::sndmore);
            sock.send(zmq::message_t(label.data(), label.size()), zmq::send_flags::sndmore);
            sock.send(zmq::message_t(w_buf, static_cast<size_t>(w_len)), zmq::send_flags::none);
        },
        "Neighbor fetch timed out");

    const auto& msg = recv_msgs[1];
    if (is_err_frame(msg)) {
        if (!is_acl_err_frame(msg)) report_failure(owner_id);
        return {};
    }
    report_success(owner_id);
    return parse_neighbors_message(msg);
}

std::vector<uint64_t> RemoteL3KVClient::get_in_neighbors(
    lite3::NodeID owner_id,
    uint64_t target_node_id,
    std::string_view label,
    uint32_t principal_id) {
    if (!get_peer_state(owner_id)) return {};

    char id_buf[17];
    std::snprintf(id_buf, sizeof(id_buf), "%016llx", static_cast<unsigned long long>(target_node_id));

    auto& recv_msgs = send_and_recv_rpc(
        owner_id,
        [&](zmq::socket_t& sock) {
            sock.send(zmq::message_t(), zmq::send_flags::sndmore);
            uint32_t pid = principal_id;
            sock.send(zmq::message_t(&pid, 4), zmq::send_flags::sndmore);
            sock.send(zmq::message_t("I", 1), zmq::send_flags::sndmore);
            sock.send(zmq::message_t(id_buf, 16), zmq::send_flags::sndmore);
            sock.send(zmq::message_t(label.data(), label.size()), zmq::send_flags::none);
        },
        "In-Neighbor fetch timed out");

    const auto& msg = recv_msgs[1];
    if (is_err_frame(msg)) {
        if (!is_acl_err_frame(msg)) report_failure(owner_id);
        return {};
    }
    report_success(owner_id);
    return parse_neighbors_message(msg);
}

std::vector<std::pair<std::string, std::string>> RemoteL3KVClient::get_prefix_entries(
    lite3::NodeID owner_id,
    std::string_view prefix,
    uint32_t principal_id) {
    using namespace lite3cpp::literals;
    if (!get_peer_state(owner_id)) return {};

    auto& recv_msgs = send_and_recv_rpc(
        owner_id,
        [&](zmq::socket_t& sock) {
            sock.send(zmq::message_t(), zmq::send_flags::sndmore);
            uint32_t pid = principal_id;
            sock.send(zmq::message_t(&pid, 4), zmq::send_flags::sndmore);
            sock.send(zmq::message_t("K", 1), zmq::send_flags::sndmore);
            sock.send(zmq::message_t(prefix.data(), prefix.size()), zmq::send_flags::none);
        },
        "Prefix scan timed out");

    const auto& msg = recv_msgs[1];
    if (is_err_frame(msg)) {
        if (is_server_err_frame(msg)) report_failure(owner_id);
        return {};
    }
    report_success(owner_id);

    std::vector<std::pair<std::string, std::string>> entries;
    if (msg.size() >= sizeof(lite3cpp::PackedNodeLayout)) {
        auto buf = lite3cpp::Buffer::from_view(static_cast<const uint8_t*>(msg.data()), msg.size());
        lite3cpp::NodeView nv(reinterpret_cast<const lite3cpp::PackedNodeLayout*>(buf.data()));
        if (nv.type() == lite3cpp::Type::Array) {
            entries.reserve(nv.size());
            for (auto it = buf.begin(0), end = buf.end(0); it != end; ++it) {
                if (it->value_type == lite3cpp::Type::Object) {
                    const size_t obj_ofs = it->value_offset + 1;
                    if (obj_ofs + sizeof(lite3cpp::PackedNodeLayout) > buf.size()) continue;
                    std::string k(buf.try_get_str(obj_ofs, "k"_hk).value_or(""));
                    std::string v(buf.try_get_str(obj_ofs, "v"_hk).value_or(""));
                    entries.emplace_back(std::move(k), std::move(v));
                } else if (it->value_type == lite3cpp::Type::String) {
                    entries.emplace_back(std::string(read_iter_string(buf, *it)), std::string{});
                }
            }
        }
    }
    return entries;
}

std::vector<std::string> RemoteL3KVClient::get_prefix_keys(
    lite3::NodeID owner_id,
    std::string_view prefix,
    uint32_t principal_id) {
    using namespace lite3cpp::literals;
    if (!get_peer_state(owner_id)) return {};

    auto& recv_msgs = send_and_recv_rpc(
        owner_id,
        [&](zmq::socket_t& sock) {
            sock.send(zmq::message_t(), zmq::send_flags::sndmore);
            uint32_t pid = principal_id;
            sock.send(zmq::message_t(&pid, 4), zmq::send_flags::sndmore);
            sock.send(zmq::message_t("K", 1), zmq::send_flags::sndmore);
            sock.send(zmq::message_t(prefix.data(), prefix.size()), zmq::send_flags::none);
        },
        "Prefix scan timed out");

    const auto& msg = recv_msgs[1];
    if (is_err_frame(msg)) {
        if (is_server_err_frame(msg)) report_failure(owner_id);
        return {};
    }
    report_success(owner_id);

    std::vector<std::string> keys;
    if (msg.size() >= sizeof(lite3cpp::PackedNodeLayout)) {
        auto buf = lite3cpp::Buffer::from_view(static_cast<const uint8_t*>(msg.data()), msg.size());
        lite3cpp::NodeView nv(reinterpret_cast<const lite3cpp::PackedNodeLayout*>(buf.data()));
        if (nv.type() == lite3cpp::Type::Array) {
            keys.reserve(nv.size());
            for (auto it = buf.begin(0), end = buf.end(0); it != end; ++it) {
                if (it->value_type == lite3cpp::Type::Object) {
                    const size_t obj_ofs = it->value_offset + 1;
                    if (obj_ofs + sizeof(lite3cpp::PackedNodeLayout) > buf.size()) continue;
                    if (auto k = buf.try_get_str(obj_ofs, "k"_hk)) {
                        keys.emplace_back(*k);
                    }
                } else if (it->value_type == lite3cpp::Type::String) {
                    keys.emplace_back(read_iter_string(buf, *it));
                }
            }
        }
    }
    return keys;
}

bool RemoteL3KVClient::execute_batch(
    lite3::NodeID owner_id,
    const lite3cpp::Buffer& batch_buffer,
    uint32_t principal_id) {
    if (!get_peer_state(owner_id)) return false;

    try {
        auto& recv_msgs = send_and_recv_rpc(
            owner_id,
            [&](zmq::socket_t& sock) {
                sock.send(zmq::message_t(), zmq::send_flags::sndmore);
                uint32_t pid = principal_id;
                sock.send(zmq::message_t(&pid, 4), zmq::send_flags::sndmore);
                sock.send(zmq::message_t("B", 1), zmq::send_flags::sndmore);
                sock.send(zmq::message_t(batch_buffer.data(), batch_buffer.size()), zmq::send_flags::none);
            },
            "Batch execution timed out");

        if (recv_msgs[1].size() == 2 && std::memcmp(recv_msgs[1].data(), "OK", 2) == 0) {
            report_success(owner_id);
            return true;
        }
        return false;
    } catch (...) {
        return false;
    }
}

bool RemoteL3KVClient::execute_batch(
    lite3::NodeID owner_id,
    const MutationBatch& batch,
    uint32_t principal_id) {
    return execute_batch(owner_id, batch.get_buffer(), principal_id);
}

lite3cpp::Buffer RemoteL3KVClient::resume_query_buffer(
    uint16_t cluster_id,
    const std::vector<uint64_t>& starting_nodes,
    const lite3cpp::Buffer& query_buf,
    uint32_t principal_id) {
    if (!get_peer_state(cluster_id)) return {};

    auto& recv_msgs = send_and_recv_rpc(
        cluster_id,
        [&](zmq::socket_t& sock) {
            sock.send(zmq::message_t(), zmq::send_flags::sndmore);
            uint32_t pid = principal_id;
            sock.send(zmq::message_t(&pid, 4), zmq::send_flags::sndmore);
            sock.send(zmq::message_t("R", 1), zmq::send_flags::sndmore);
            sock.send(zmq::message_t(starting_nodes.data(), starting_nodes.size() * sizeof(uint64_t)), zmq::send_flags::sndmore);
            sock.send(zmq::message_t(query_buf.data(), query_buf.size()), zmq::send_flags::none);
        },
        "Remote query timed out");

    const auto& resp_msg = recv_msgs[1];
    if (is_err_frame(resp_msg)) {
        if (!is_acl_err_frame(resp_msg)) report_failure(cluster_id);
        return {};
    }
    report_success(cluster_id);
    const uint8_t* ptr = static_cast<const uint8_t*>(resp_msg.data());
    if (resp_msg.size() >= sizeof(lite3cpp::PackedNodeLayout) && (ptr[0] == 0x06 || ptr[0] == 0x07)) {
        return lite3cpp::Buffer(ptr, resp_msg.size());
    }
    std::string_view raw_json(reinterpret_cast<const char*>(ptr), resp_msg.size());
    return lite3cpp::lite3_json::from_json_string(raw_json.empty() ? "[]" : raw_json);
}

std::vector<GraphReadResult> RemoteL3KVClient::execute_multi_read(
    lite3::NodeID owner_id,
    std::span<const GraphReadOp> ops,
    uint32_t principal_id) {
    using namespace lite3cpp::literals;
    if (ops.empty() || !get_peer_state(owner_id)) return {};

    thread_local lite3cpp::Buffer tl_req_buf;
    tl_req_buf.clear();
    tl_req_buf.ensure_capacity(ops.size() * (lite3cpp::config::node_size + 64) + 256);
    tl_req_buf.init_object();
    size_t ops_ofs = tl_req_buf.set_arr(0, "ops"_hk);
    for (const auto& op : ops) {
        size_t o_ofs = tl_req_buf.arr_append_obj(ops_ofs);
        tl_req_buf.set_i64(o_ofs, "k"_hk, static_cast<int64_t>(op.kind));
        if (op.kind != GraphReadKind::RawKey) {
            tl_req_buf.set_i64(o_ofs, "id"_hk, static_cast<int64_t>(op.node_id));
        }
        if (!op.label_or_key.empty()) {
            tl_req_buf.set_str(o_ofs, "s"_hk, op.label_or_key);
        }
        if (op.kind == GraphReadKind::OutNeighbors) {
            tl_req_buf.set_f64(o_ofs, "w"_hk, op.min_weight);
        }
    }

    auto& recv_msgs = send_and_recv_rpc(
        owner_id,
        [&](zmq::socket_t& sock) {
            sock.send(zmq::message_t(), zmq::send_flags::sndmore);
            uint32_t pid = principal_id;
            sock.send(zmq::message_t(&pid, 4), zmq::send_flags::sndmore);
            sock.send(zmq::message_t("G", 1), zmq::send_flags::sndmore);
            sock.send(zmq::message_t(tl_req_buf.data(), tl_req_buf.size()), zmq::send_flags::none);
        },
        "Graph multi-read timed out");

    const auto& msg = recv_msgs[1];
    if (is_err_frame(msg)) {
        if (!is_acl_err_frame(msg)) report_failure(owner_id);
        return {};
    }
    report_success(owner_id);

    std::vector<GraphReadResult> out;
    if (msg.size() >= sizeof(lite3cpp::PackedNodeLayout)) {
        auto resp_buf = lite3cpp::Buffer::from_view(static_cast<const uint8_t*>(msg.data()), msg.size());
        lite3cpp::NodeView nv(reinterpret_cast<const lite3cpp::PackedNodeLayout*>(resp_buf.data()));
        if (nv.type() == lite3cpp::Type::Array) {
            out.reserve(nv.size());
            for (auto it = resp_buf.begin(0), end = resp_buf.end(0); it != end; ++it) {
                GraphReadResult gr;
                if (it->value_type == lite3cpp::Type::Object) {
                    const size_t r_ofs = it->value_offset + 1;
                    if (r_ofs + sizeof(lite3cpp::PackedNodeLayout) <= resp_buf.size()) {
                        gr.found = resp_buf.try_get_bool(r_ofs, "f"_hk).value_or(false);
                        if (auto p = resp_buf.try_get_bytes(r_ofs, "p"_hk)) {
                            gr.payload.assign(reinterpret_cast<const char*>(p->data()), p->size());
                        } else if (auto ps = resp_buf.try_get_str(r_ofs, "p"_hk)) {
                            gr.payload.assign(ps->data(), ps->size());
                        }
                        if (auto n = resp_buf.try_get_bytes(r_ofs, "n"_hk);
                            n && n->size() % sizeof(uint64_t) == 0) {
                            size_t cnt = n->size() / sizeof(uint64_t);
                            gr.neighbors.resize(cnt);
                            if (cnt > 0) {
                                std::memcpy(gr.neighbors.data(), n->data(), n->size());
                            }
                        }
                    }
                }
                out.push_back(std::move(gr));
            }
        }
    }
    return out;
}

bool RemoteL3KVClient::put_key_bytes_sync(
    lite3::NodeID owner_id,
    std::string_view key,
    const void* data,
    size_t len,
    uint32_t principal_id) {
    if (!get_peer_state(owner_id)) return false;

    try {
        auto& recv_msgs = send_and_recv_rpc(
            owner_id,
            [&](zmq::socket_t& sock) {
                sock.send(zmq::message_t(), zmq::send_flags::sndmore);
                uint32_t pid = principal_id;
                sock.send(zmq::message_t(&pid, 4), zmq::send_flags::sndmore);
                sock.send(zmq::message_t("P", 1), zmq::send_flags::sndmore);
                sock.send(zmq::message_t(key.data(), key.size()), zmq::send_flags::sndmore);
                sock.send(zmq::message_t(data, len), zmq::send_flags::none);
            },
            "Put key timed out");

        if (recv_msgs[1].size() == 2 && std::memcmp(recv_msgs[1].data(), "OK", 2) == 0) {
            report_success(owner_id);
            return true;
        }
        return false;
    } catch (...) {
        return false;
    }
}

bool RemoteL3KVClient::del_key_sync(lite3::NodeID owner_id, std::string_view key, uint32_t principal_id) {
    if (!get_peer_state(owner_id)) return false;

    try {
        auto& recv_msgs = send_and_recv_rpc(
            owner_id,
            [&](zmq::socket_t& sock) {
                sock.send(zmq::message_t(), zmq::send_flags::sndmore);
                uint32_t pid = principal_id;
                sock.send(zmq::message_t(&pid, 4), zmq::send_flags::sndmore);
                sock.send(zmq::message_t("D", 1), zmq::send_flags::sndmore);
                sock.send(zmq::message_t(key.data(), key.size()), zmq::send_flags::none);
            },
            "Delete key timed out");

        if (recv_msgs[1].size() == 2 && std::memcmp(recv_msgs[1].data(), "OK", 2) == 0) {
            report_success(owner_id);
            return true;
        }
        return false;
    } catch (...) {
        return false;
    }
}

bool RemoteL3KVClient::ping_peer_sync(lite3::NodeID node_id) {
    try {
        auto peer = get_peer_state(node_id);
        if (!peer) return false;

        const bool can_reauth = has_auth_secret();
        for (int attempt = 0; attempt < 2; ++attempt) {
            auto& tl_map = get_tl_socket_map();
            const uint64_t curr_epoch = config_epoch_.load(std::memory_order_acquire);
            const TlSocketKey key{client_instance_id_, node_id};
            TlSocketBox* box = nullptr;

            if (auto it = tl_map.find(key); it != tl_map.end()) {
                auto* cand = it->second.box.get();
                if (cand && cand->socket && cand->config_epoch == curr_epoch && !it->second.weak_registry.expired()) {
                    box = cand;
                } else {
                    tl_map.erase(it);
                }
            }
            if (!box) {
                auto new_box = std::make_shared<TlSocketBox>();
                new_box->endpoint = peer->endpoint;
                new_box->config_epoch = curr_epoch;
                new_box->socket = std::make_unique<zmq::socket_t>(zmq_ctx_, ZMQ_DEALER);
                new_box->socket->set(zmq::sockopt::sndhwm, zmq_sndhwm_);
                new_box->socket->set(zmq::sockopt::linger, 0);
                new_box->socket->set(zmq::sockopt::rcvtimeo, settings_.fed_timeout_ms);
                new_box->socket->set(zmq::sockopt::sndtimeo, settings_.fed_timeout_ms);
                new_box->socket->connect(new_box->endpoint);
                {
                    std::lock_guard<std::mutex> reg_lk(socket_registry_->mu);
                    if (socket_registry_->client_closed) {
                        new_box->close_socket();
                        return false;
                    }
                    socket_registry_->active.insert(new_box);
                }
                box = new_box.get();
                tl_map.emplace(key, TlSocketEntry(socket_registry_, std::move(new_box)));
            }

            ensure_socket_authenticated(*box, node_id);

            auto& recv_msgs = get_tl_recv_msgs();
            bool io_ok = false;
            try {
                box->socket->send(zmq::message_t(), zmq::send_flags::sndmore);
                uint32_t internal_uid = INTERNAL_UID;
                box->socket->send(zmq::message_t(&internal_uid, 4), zmq::send_flags::sndmore);
                box->socket->send(zmq::message_t("H", 1), zmq::send_flags::sndmore);
                box->socket->send(zmq::message_t(), zmq::send_flags::none);

                auto res = zmq::recv_multipart(*box->socket, std::back_inserter(recv_msgs));
                io_ok = (res.has_value() && recv_msgs.size() >= 2);
            } catch (...) {
                invalidate_tl_socket(node_id, box);
                return false;
            }
            if (!io_ok) {
                invalidate_tl_socket(node_id, box);
                return false;
            }
            if (attempt == 0 && can_reauth && is_unauth_frame(recv_msgs[1])) {
                box->authenticated = false;
                continue;
            }
            std::string_view sv(static_cast<const char*>(recv_msgs[1].data()), recv_msgs[1].size());
            return sv == "PONG" || sv == "OK";
        }
        return false;
    } catch (...) {
        return false;
    }
}

std::vector<ResultRow> RemoteL3KVClient::resume_query_rows_sync(
    uint16_t cluster_id,
    const std::vector<uint64_t>& starting_nodes,
    const lite3cpp::Buffer& query_buf,
    uint32_t principal_id) {
    using namespace lite3cpp::literals;
    if (!get_peer_state(cluster_id)) return {};

    auto& recv_msgs = send_and_recv_rpc(
        cluster_id,
        [&](zmq::socket_t& sock) {
            sock.send(zmq::message_t(), zmq::send_flags::sndmore);
            uint32_t pid = principal_id;
            sock.send(zmq::message_t(&pid, 4), zmq::send_flags::sndmore);
            sock.send(zmq::message_t("R", 1), zmq::send_flags::sndmore);
            sock.send(zmq::message_t(starting_nodes.data(), starting_nodes.size() * sizeof(uint64_t)), zmq::send_flags::sndmore);
            sock.send(zmq::message_t(query_buf.data(), query_buf.size()), zmq::send_flags::none);
        },
        "Remote query timed out");

    const auto& resp_msg = recv_msgs[1];
    if (is_err_frame(resp_msg)) {
        if (!is_acl_err_frame(resp_msg)) report_failure(cluster_id);
        return {};
    }
    report_success(cluster_id);

    lite3cpp::Buffer owned_json_buf;
    lite3cpp::Buffer resp_buf;
    const uint8_t* ptr = static_cast<const uint8_t*>(resp_msg.data());
    if (resp_msg.size() >= sizeof(lite3cpp::PackedNodeLayout) && (ptr[0] == 0x06 || ptr[0] == 0x07)) {
        resp_buf = lite3cpp::Buffer::from_view(ptr, resp_msg.size());
    } else {
        std::string_view raw_json(reinterpret_cast<const char*>(ptr), resp_msg.size());
        owned_json_buf = lite3cpp::lite3_json::from_json_string(raw_json.empty() ? "[]" : raw_json);
        resp_buf = lite3cpp::Buffer::from_view(owned_json_buf.data(), owned_json_buf.size());
    }

    std::vector<ResultRow> results;
    if (resp_buf.size() >= sizeof(lite3cpp::PackedNodeLayout)) {
        lite3cpp::NodeView root_nv(reinterpret_cast<const lite3cpp::PackedNodeLayout*>(resp_buf.data()));
        if (root_nv.type() == lite3cpp::Type::Array) {
            results.reserve(root_nv.size());
            std::vector<std::string> cols;
            uint32_t r = 0;
            for (auto rit = resp_buf.begin(0), rend = resp_buf.end(0); rit != rend; ++rit, ++r) {
                if (rit->value_type == lite3cpp::Type::Array) {
                    const size_t arr_ofs = rit->value_offset + 1;
                    if (arr_ofs + sizeof(lite3cpp::PackedNodeLayout) > resp_buf.size()) continue;
                    ResultRow row;
                    for (auto cit = resp_buf.begin(arr_ofs), cend = resp_buf.end(arr_ofs); cit != cend; ++cit) {
                        row.projected_values.emplace_back(read_iter_string(resp_buf, *cit));
                    }
                    results.push_back(std::move(row));
                    continue;
                }
                if (rit->value_type != lite3cpp::Type::Object) continue;
                const size_t row_ofs = rit->value_offset + 1;
                if (row_ofs + sizeof(lite3cpp::PackedNodeLayout) > resp_buf.size()) continue;

                ResultRow row;
                if (auto cols_ofs_opt = resp_buf.try_get_arr(row_ofs, "cols"_hk)) {
                    const size_t cols_ofs = *cols_ofs_opt;
                    cols.clear();
                    for (auto cit = resp_buf.begin(cols_ofs), cend = resp_buf.end(cols_ofs); cit != cend; ++cit) {
                        cols.emplace_back(read_iter_string(resp_buf, *cit));
                    }
                }
                if (r == 0 && !cols.empty()) {
                    row.projected_names = cols;
                }
                if (auto proj_ofs_opt = resp_buf.try_get_arr(row_ofs, "proj"_hk)) {
                    const size_t proj_ofs = *proj_ofs_opt;
                    size_t pi = 0;
                    for (auto pit = resp_buf.begin(proj_ofs), pend = resp_buf.end(proj_ofs); pit != pend; ++pit, ++pi) {
                        std::string val(read_iter_string(resp_buf, *pit));
                        if (pi < cols.size() && !cols[pi].empty()) {
                            row.fields[cols[pi]] = val;
                        }
                        row.projected_values.push_back(std::move(val));
                    }
                } else if (auto fields_ofs_opt = resp_buf.try_get_obj(row_ofs, "fields"_hk)) {
                    const size_t fields_ofs = *fields_ofs_opt;
                    for (auto fit = resp_buf.begin(fields_ofs), fend = resp_buf.end(fields_ofs); fit != fend; ++fit) {
                        std::string_view k = fit->key;
                        if (fit->value_type == lite3cpp::Type::String) {
                            row.fields.emplace(k, read_iter_string(resp_buf, *fit));
                        } else if (fit->value_type == lite3cpp::Type::Int64 && fit->value_offset + 9 <= resp_buf.size()) {
                            int64_t iv = 0;
                            std::memcpy(&iv, resp_buf.data() + fit->value_offset + 1, 8);
                            row.fields.emplace(k, std::to_string(iv));
                        } else if (fit->value_type == lite3cpp::Type::Float64 && fit->value_offset + 9 <= resp_buf.size()) {
                            double fv = 0.0;
                            std::memcpy(&fv, resp_buf.data() + fit->value_offset + 1, 8);
                            row.fields.emplace(k, std::to_string(fv));
                        } else if (fit->value_type == lite3cpp::Type::Bool && fit->value_offset + 2 <= resp_buf.size()) {
                            bool bv = resp_buf.data()[fit->value_offset + 1] != 0;
                            row.fields.emplace(k, bv ? "true" : "false");
                        } else {
                            row.fields.emplace(k, "");
                        }
                    }
                }
                results.push_back(std::move(row));
            }
        }
    }
    return results;
}

// ============================================================================
// Asynchronous RPC Wrappers (moving Buffers, using thread_local sockets)
// ============================================================================

std::future<bool> RemoteL3KVClient::put_batch_binary_async(
    lite3::NodeID owner_id,
    const lite3cpp::Buffer& batch_buffer,
    uint32_t principal_id) {
    auto peer = get_peer_state(owner_id);
    if (!peer || !task_pool_) {
        std::promise<bool> p;
        p.set_value(false);
        return p.get_future();
    }
    try {
        check_circuit(*peer);
    } catch (...) {
        std::promise<bool> p;
        p.set_exception(std::current_exception());
        return p.get_future();
    }

    lite3cpp::Buffer owned = clone_or_copy_buffer(batch_buffer);
    return task_pool_->enqueue([this, owner_id, buf = std::move(owned), principal_id]() -> bool {
        return execute_batch(owner_id, buf, principal_id);
    });
}

std::future<bool> RemoteL3KVClient::execute_batch_async(
    lite3::NodeID owner_id,
    const MutationBatch& batch,
    uint32_t principal_id) {
    return put_batch_binary_async(owner_id, batch.get_buffer(), principal_id);
}

std::future<bool> RemoteL3KVClient::replicate_async(
    uint16_t cluster_id,
    const std::string& key,
    const std::string& payload,
    uint16_t origin_cluster_id,
    uint32_t principal_id) {
    if (!get_peer_state(cluster_id) || !task_pool_) {
        std::promise<bool> p;
        p.set_value(false);
        return p.get_future();
    }

    return task_pool_->enqueue([this, cluster_id, key, payload, origin_cluster_id, principal_id]() -> bool {
        try {
            char orig_buf[16];
            auto [optr, oec] = std::to_chars(orig_buf, orig_buf + sizeof(orig_buf), origin_cluster_id);
            size_t orig_len = (oec == std::errc{}) ? static_cast<size_t>(optr - orig_buf) : 0;

            auto& recv_msgs = send_and_recv_rpc(
                cluster_id,
                [&](zmq::socket_t& sock) {
                    sock.send(zmq::message_t(), zmq::send_flags::sndmore);
                    uint32_t pid = principal_id;
                    sock.send(zmq::message_t(&pid, 4), zmq::send_flags::sndmore);
                    sock.send(zmq::message_t("S", 1), zmq::send_flags::sndmore);
                    sock.send(zmq::message_t(key.data(), key.size()), zmq::send_flags::sndmore);
                    sock.send(zmq::message_t(payload.data(), payload.size()), zmq::send_flags::sndmore);
                    sock.send(zmq::message_t(orig_buf, orig_len), zmq::send_flags::none);
                },
                "Replication sync timed out");

            if (!is_err_frame(recv_msgs[1])) {
                report_success(cluster_id);
                return true;
            }
            if (is_server_err_frame(recv_msgs[1])) report_failure(cluster_id);
            return false;
        } catch (...) {
            return false;
        }
    });
}

std::future<bool> RemoteL3KVClient::ping_peer(lite3::NodeID node_id) {
    if (!task_pool_) {
        std::promise<bool> p;
        p.set_value(false);
        return p.get_future();
    }
    return task_pool_->enqueue([this, node_id]() -> bool {
        return ping_peer_sync(node_id);
    });
}

std::future<std::vector<uint64_t>> RemoteL3KVClient::get_neighbors_async(
    lite3::NodeID owner_id,
    uint64_t target_node_id,
    const std::string& label,
    double min_weight,
    uint32_t principal_id) {
    if (!task_pool_) {
        std::promise<std::vector<uint64_t>> p;
        p.set_value({});
        return p.get_future();
    }
    return task_pool_->enqueue([this, owner_id, target_node_id, label, min_weight, principal_id]() -> std::vector<uint64_t> {
        try {
            return get_neighbors(owner_id, target_node_id, label, min_weight, principal_id);
        } catch (const CircuitBreakerOpenException&) {
            return {};
        } catch (const FederationTimeoutException&) {
            throw;
        } catch (...) {
            return {};
        }
    });
}

std::future<std::vector<uint64_t>> RemoteL3KVClient::get_in_neighbors_async(
    lite3::NodeID owner_id,
    uint64_t target_node_id,
    const std::string& label,
    uint32_t principal_id) {
    if (!task_pool_) {
        std::promise<std::vector<uint64_t>> p;
        p.set_value({});
        return p.get_future();
    }
    return task_pool_->enqueue([this, owner_id, target_node_id, label, principal_id]() -> std::vector<uint64_t> {
        try {
            return get_in_neighbors(owner_id, target_node_id, label, principal_id);
        } catch (const CircuitBreakerOpenException&) {
            return {};
        } catch (const FederationTimeoutException&) {
            throw;
        } catch (...) {
            return {};
        }
    });
}

std::future<lite3cpp::Buffer> RemoteL3KVClient::resume_query_buffer_async(
    uint16_t cluster_id,
    const std::vector<uint64_t>& starting_nodes,
    const lite3cpp::Buffer& query_buf,
    uint32_t principal_id) {
    if (!task_pool_) {
        std::promise<lite3cpp::Buffer> p;
        p.set_value(lite3cpp::Buffer());
        return p.get_future();
    }
    lite3cpp::Buffer owned = clone_or_copy_buffer(query_buf);
    return task_pool_->enqueue([this, cluster_id, starting_nodes, buf = std::move(owned), principal_id]() -> lite3cpp::Buffer {
        return resume_query_buffer(cluster_id, starting_nodes, buf, principal_id);
    });
}

std::future<std::vector<ResultRow>> RemoteL3KVClient::resume_query_async(
    uint16_t cluster_id,
    const std::vector<uint64_t>& starting_nodes,
    const lite3cpp::Buffer& query_buf,
    uint32_t principal_id) {
#ifdef IRODS_SERVER
    rodsLog(LOG_NOTICE, "L3KV_CLIENT: resume_query_async for cluster %u", cluster_id);
#endif
    if (!task_pool_) {
        std::promise<std::vector<ResultRow>> p;
        p.set_value({});
        return p.get_future();
    }
    lite3cpp::Buffer owned = clone_or_copy_buffer(query_buf);
    return task_pool_->enqueue([this, cluster_id, starting_nodes, buf = std::move(owned), principal_id]() -> std::vector<ResultRow> {
        return resume_query_rows_sync(cluster_id, starting_nodes, buf, principal_id);
    });
}

std::future<std::vector<ResultRow>> RemoteL3KVClient::resume_query_async(
    uint16_t cluster_id,
    const std::vector<uint64_t>& starting_nodes,
    const std::string& query_payload,
    uint32_t principal_id) {
    lite3cpp::Buffer buf;
    if (query_payload.size() >= sizeof(lite3cpp::PackedNodeLayout) &&
        (static_cast<uint8_t>(query_payload[0]) == 0x06 || static_cast<uint8_t>(query_payload[0]) == 0x07)) {
        const uint8_t* ptr = reinterpret_cast<const uint8_t*>(query_payload.data());
        buf = lite3cpp::Buffer(ptr, query_payload.size());
    } else {
        buf = lite3cpp::lite3_json::from_json_string(query_payload.empty() ? "{}" : query_payload);
    }
    if (!task_pool_) {
        std::promise<std::vector<ResultRow>> p;
        p.set_value({});
        return p.get_future();
    }
    return task_pool_->enqueue([this, cluster_id, starting_nodes, qbuf = std::move(buf), principal_id]() -> std::vector<ResultRow> {
        return resume_query_rows_sync(cluster_id, starting_nodes, qbuf, principal_id);
    });
}

std::future<bool> RemoteL3KVClient::put_edge_async(
    lite3::NodeID owner_id,
    const std::string& edge_key,
    const std::string& json_payload,
    uint32_t principal_id) {
    auto peer = get_peer_state(owner_id);
    if (!peer || !task_pool_) {
        std::promise<bool> p;
        p.set_value(false);
        return p.get_future();
    }
    try {
        check_circuit(*peer);
    } catch (...) {
        std::promise<bool> p;
        p.set_exception(std::current_exception());
        return p.get_future();
    }

    return task_pool_->enqueue([this, owner_id, edge_key, json_payload, principal_id]() -> bool {
        return put_key_bytes_sync(owner_id, edge_key, json_payload.data(), json_payload.size(), principal_id);
    });
}

std::future<uint64_t> RemoteL3KVClient::atomic_incr_async(
    lite3::NodeID owner_id,
    const std::string& key,
    int64_t delta,
    uint32_t principal_id) {
    if (!task_pool_) {
        std::promise<uint64_t> p;
        p.set_value(0);
        return p.get_future();
    }

    return task_pool_->enqueue([this, owner_id, key, delta, principal_id]() -> uint64_t {
        if (!get_peer_state(owner_id)) return 0;
        try {
            char d_buf[32];
            auto [dptr, dec] = std::to_chars(d_buf, d_buf + sizeof(d_buf), delta);
            size_t d_len = (dec == std::errc{}) ? static_cast<size_t>(dptr - d_buf) : 0;

            auto& recv_msgs = send_and_recv_rpc(
                owner_id,
                [&](zmq::socket_t& sock) {
                    sock.send(zmq::message_t(), zmq::send_flags::sndmore);
                    uint32_t pid = principal_id;
                    sock.send(zmq::message_t(&pid, 4), zmq::send_flags::sndmore);
                    sock.send(zmq::message_t("+", 1), zmq::send_flags::sndmore);
                    sock.send(zmq::message_t(key.data(), key.size()), zmq::send_flags::sndmore);
                    sock.send(zmq::message_t(d_buf, d_len), zmq::send_flags::none);
                },
                "Atomic increment timed out");

            if (!is_err_frame(recv_msgs[1])) {
                std::string_view sv(static_cast<const char*>(recv_msgs[1].data()), recv_msgs[1].size());
                uint64_t parsed = 0;
                auto [p, ec] = std::from_chars(sv.data(), sv.data() + sv.size(), parsed);
                if (ec == std::errc{}) {
                    report_success(owner_id);
                    return parsed;
                }
            }
            return 0;
        } catch (...) {
            return 0;
        }
    });
}

std::future<std::string> RemoteL3KVClient::get_raw_key_async(
    lite3::NodeID owner_id,
    const std::string& key,
    uint32_t principal_id) {
    if (!task_pool_) {
        std::promise<std::string> p;
        p.set_value("");
        return p.get_future();
    }
    return task_pool_->enqueue([this, owner_id, key, principal_id]() -> std::string {
        return get_raw_key(owner_id, key, principal_id);
    });
}

std::future<std::vector<std::pair<std::string, std::string>>> RemoteL3KVClient::get_prefix_entries_async(
    lite3::NodeID owner_id,
    const std::string& prefix,
    uint32_t principal_id) {
    if (!task_pool_) {
        std::promise<std::vector<std::pair<std::string, std::string>>> p;
        p.set_value({});
        return p.get_future();
    }
    return task_pool_->enqueue([this, owner_id, prefix, principal_id]() -> std::vector<std::pair<std::string, std::string>> {
        try {
            return get_prefix_entries(owner_id, prefix, principal_id);
        } catch (const CircuitBreakerOpenException&) {
            return {};
        }
    });
}

std::future<std::vector<std::string>> RemoteL3KVClient::get_prefix_keys_async(
    lite3::NodeID owner_id,
    const std::string& prefix,
    uint32_t principal_id) {
    if (!task_pool_) {
        std::promise<std::vector<std::string>> p;
        p.set_value({});
        return p.get_future();
    }
    return task_pool_->enqueue([this, owner_id, prefix, principal_id]() -> std::vector<std::string> {
        try {
            return get_prefix_keys(owner_id, prefix, principal_id);
        } catch (const CircuitBreakerOpenException&) {
            return {};
        }
    });
}

std::future<lite3cpp::Buffer> RemoteL3KVClient::get_node_buffer_async(
    lite3::NodeID owner_id,
    uint64_t target_node_id,
    uint32_t principal_id) {
    if (!task_pool_) {
        std::promise<lite3cpp::Buffer> p;
        p.set_value({});
        return p.get_future();
    }
    return task_pool_->enqueue([this, owner_id, target_node_id, principal_id]() -> lite3cpp::Buffer {
        return get_node_buffer(owner_id, target_node_id, principal_id);
    });
}

std::future<bool> RemoteL3KVClient::put_node_async(
    lite3::NodeID owner_id,
    uint64_t target_node_id,
    const lite3cpp::Buffer& payload,
    uint32_t principal_id) {
    auto peer = get_peer_state(owner_id);
    if (!peer || !task_pool_) {
        std::promise<bool> p;
        p.set_value(false);
        return p.get_future();
    }
    try {
        check_circuit(*peer);
    } catch (...) {
        std::promise<bool> p;
        p.set_exception(std::current_exception());
        return p.get_future();
    }

    lite3cpp::Buffer owned = clone_or_copy_buffer(payload);
    return task_pool_->enqueue([this, owner_id, target_node_id, buf = std::move(owned), principal_id]() -> bool {
        return put_key_bytes_sync(owner_id, KeyBuilder::node_key(target_node_id), buf.data(), buf.size(), principal_id);
    });
}

std::future<std::string> RemoteL3KVClient::get_node_payload_async(
    lite3::NodeID owner_id,
    uint64_t target_node_id,
    uint32_t principal_id) {
    if (!task_pool_) {
        std::promise<std::string> p;
        p.set_value("");
        return p.get_future();
    }
    return task_pool_->enqueue([this, owner_id, target_node_id, principal_id]() -> std::string {
        return get_node_payload(owner_id, target_node_id, principal_id);
    });
}

std::future<std::unordered_map<uint64_t, std::string>> RemoteL3KVClient::get_nodes_batch_async(
    lite3::NodeID owner_id,
    const std::vector<uint64_t>& node_ids,
    uint32_t principal_id) {
    if (!get_peer_state(owner_id) || !task_pool_ || node_ids.empty()) {
        std::promise<std::unordered_map<uint64_t, std::string>> p;
        p.set_value({});
        return p.get_future();
    }

    return task_pool_->enqueue([this, owner_id, node_ids, principal_id]() -> std::unordered_map<uint64_t, std::string> {
        std::unordered_map<uint64_t, std::string> results;
        try {
            auto& recv_msgs = send_and_recv_rpc(
                owner_id,
                [&](zmq::socket_t& sock) {
                    sock.send(zmq::message_t(), zmq::send_flags::sndmore);
                    uint32_t pid = principal_id;
                    sock.send(zmq::message_t(&pid, 4), zmq::send_flags::sndmore);
                    sock.send(zmq::message_t("M", 1), zmq::send_flags::sndmore);
                    for (size_t i = 0; i < node_ids.size(); ++i) {
                        std::string_view key = KeyBuilder::node_key(node_ids[i]);
                        sock.send(
                            zmq::message_t(key.data(), key.size()),
                            (i == node_ids.size() - 1) ? zmq::send_flags::none : zmq::send_flags::sndmore);
                    }
                },
                "Nodes batch fetch timed out");

            const auto& body = recv_msgs[1];
            if (is_err_frame(body)) {
                if (!is_acl_err_frame(body)) report_failure(owner_id);
                return results;
            }
            report_success(owner_id);
            if (body.size() >= sizeof(lite3cpp::PackedNodeLayout)) {
                auto buf = lite3cpp::Buffer::from_view(static_cast<const uint8_t*>(body.data()), body.size());
                results.reserve(node_ids.size());
                for (auto it = buf.begin(0), end = buf.end(0); it != end; ++it) {
                    std::string_view key = it->key;
                    if (key.starts_with("n:{") && key.ends_with("}") && key.size() > 4) {
                        std::string_view hex_sv = key.substr(3, key.size() - 4);
                        uint64_t id = 0;
                        auto [ptr, ec] = std::from_chars(hex_sv.data(), hex_sv.data() + hex_sv.size(), id, 16);
                        if (ec != std::errc{} || ptr != hex_sv.data() + hex_sv.size()) continue;

                        if (it->value_type == lite3cpp::Type::Bytes && it->value_offset + 5 <= buf.size()) {
                            uint32_t sz = 0;
                            std::memcpy(&sz, buf.data() + it->value_offset + 1, 4);
                            if (it->value_offset + 5 + sz <= buf.size()) {
                                results.emplace(id, std::string(reinterpret_cast<const char*>(buf.data() + it->value_offset + 5), sz));
                            }
                        } else if (it->value_type == lite3cpp::Type::String) {
                            results.emplace(id, std::string(read_iter_string(buf, *it)));
                        }
                    }
                }
            }
            return results;
        } catch (...) {
            return {};
        }
    });
}

std::future<bool> RemoteL3KVClient::put_node_async(
    lite3::NodeID owner_id,
    uint64_t target_node_id,
    const std::string& json_payload,
    uint32_t principal_id) {
    std::string key(KeyBuilder::node_key(target_node_id));
    return put_edge_async(owner_id, key, json_payload, principal_id);
}

std::future<bool> RemoteL3KVClient::del_node_async(
    lite3::NodeID owner_id,
    uint64_t target_node_id,
    uint32_t principal_id) {
    std::string key(KeyBuilder::node_key(target_node_id));
    return del_edge_async(owner_id, key, principal_id);
}

std::future<bool> RemoteL3KVClient::del_edge_async(
    lite3::NodeID owner_id,
    const std::string& edge_key,
    uint32_t principal_id) {
    auto peer = get_peer_state(owner_id);
    if (!peer || !task_pool_) {
        std::promise<bool> p;
        p.set_value(false);
        return p.get_future();
    }
    try {
        check_circuit(*peer);
    } catch (...) {
        std::promise<bool> p;
        p.set_exception(std::current_exception());
        return p.get_future();
    }

    return task_pool_->enqueue([this, owner_id, edge_key, principal_id]() -> bool {
        return del_key_sync(owner_id, edge_key, principal_id);
    });
}

std::future<bool> RemoteL3KVClient::put_batch_async(
    lite3::NodeID /*owner_id*/,
    const std::unordered_map<uint64_t, std::string>& /*batch*/,
    uint32_t /*principal_id*/) {
    std::promise<bool> p;
    p.set_value(false);
    return p.get_future();
}

CircuitState RemoteL3KVClient::get_circuit_state(lite3::NodeID node_id) {
    auto peer = get_peer_state(node_id);
    if (!peer) return CircuitState::OPEN;
    return peer->state.load(std::memory_order_acquire);
}

void RemoteL3KVClient::set_circuit_state(lite3::NodeID node_id, CircuitState state) {
    auto peer = get_peer_state(node_id);
    if (!peer) return;
    std::lock_guard<std::mutex> lock(peer->state_mu);
    peer->state.store(state, std::memory_order_release);
    if (state == CircuitState::OPEN) {
        peer->last_failure_ns.store(steady_now_ns(), std::memory_order_release);
    } else if (state == CircuitState::CLOSED) {
        peer->consecutive_failures.store(0, std::memory_order_relaxed);
    }
}

void RemoteL3KVClient::report_failure(lite3::NodeID node_id) {
    auto peer = get_peer_state(node_id);
    if (!peer) return;

    std::lock_guard<std::mutex> lock(peer->state_mu);
    CircuitState st = peer->state.load(std::memory_order_relaxed);
    if (st == CircuitState::OPEN) return;

    int failures = peer->consecutive_failures.fetch_add(1, std::memory_order_relaxed) + 1;
    if (st == CircuitState::HALF_OPEN || failures >= settings_.breaker_failure_threshold) {
        peer->last_failure_ns.store(steady_now_ns(), std::memory_order_release);
        peer->state.store(CircuitState::OPEN, std::memory_order_release);
    }
}

void RemoteL3KVClient::report_success(lite3::NodeID node_id) {
    auto peer = get_peer_state(node_id);
    if (!peer) return;

    if (peer->consecutive_failures.load(std::memory_order_relaxed) == 0 &&
        peer->state.load(std::memory_order_relaxed) == CircuitState::CLOSED) {
        return;
    }

    std::lock_guard<std::mutex> lock(peer->state_mu);
    peer->consecutive_failures.store(0, std::memory_order_relaxed);
    if (peer->state.load(std::memory_order_relaxed) == CircuitState::HALF_OPEN) {
        peer->state.store(CircuitState::CLOSED, std::memory_order_release);
    }
}

} // namespace l3kvg
