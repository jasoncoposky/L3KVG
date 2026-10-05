#include "L3KVG/RemoteL3KVClient.hpp"
#include "L3KVG/MutationBatch.hpp"
#include "L3KVG/KeyBuilder.hpp"
#include <iostream>
#include <thread>
#include <zmq_addon.hpp>

#include <unistd.h>

#ifdef IRODS_SERVER
#include "irods/rodsLog.h"
#endif

namespace lite3cpp::lite3_json {
    Buffer from_json_string(const std::string& json_str);
}

namespace l3kvg {

RemoteL3KVClient::RemoteL3KVClient(const Settings& settings) 
    : settings_(settings), zmq_sndhwm_(settings.zmq_sndhwm), zmq_ctx_(1), creator_pid_(getpid()) {
    health_check_thread_ = std::thread(&RemoteL3KVClient::run_health_check_loop, this);
}

RemoteL3KVClient::~RemoteL3KVClient() {
    if (getpid() != creator_pid_) {
        if (health_check_thread_.joinable()) {
            health_check_thread_.detach();
        }
        return;
    }
    stop_health_check_ = true;
    health_check_cv_.notify_all();
    if (health_check_thread_.joinable()) {
        health_check_thread_.join();
    }

    // 1. Reset and join task_pool_ BEFORE closing sockets or destroying zmq_ctx_
    task_pool_.reset();

    // 2. Close all sockets with linger=0
    std::lock_guard<std::mutex> lock(endpoints_mutex_);
    for (auto& [id, session] : peer_sessions_) {
        std::lock_guard<std::recursive_mutex> s_lock(session->mu);
        if (session->socket) {
            session->socket->set(zmq::sockopt::linger, 0);
            session->socket->close();
            session->socket.reset();
        }
    }

    // 3. Shutdown zmq_ctx_ explicitly
    try {
        zmq_ctx_.shutdown();
        zmq_ctx_.close();
    } catch (...) {}
}

void RemoteL3KVClient::add_peer(lite3::NodeID node_id, const std::string& endpoint_url) {
    std::lock_guard<std::mutex> lock(endpoints_mutex_);
    
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

    peer_endpoints_[node_id] = url;
    std::cout << "[L3KV_CLIENT] Peer registered: " << node_id << " at " << url << std::endl;

    auto session = std::make_shared<Session>();
    
    session->socket = std::make_unique<zmq::socket_t>(zmq_ctx_, ZMQ_DEALER);
    session->socket->set(zmq::sockopt::sndhwm, zmq_sndhwm_);
    session->socket->set(zmq::sockopt::linger, 0);
    session->socket->set(zmq::sockopt::rcvtimeo, settings_.fed_timeout_ms);
    session->socket->connect(url);
    session->connected = true;
    
    peer_sessions_[node_id] = session;
    std::cout << "Connected ZMQ Dealer to peer " << node_id << " at " << url << std::endl;
}

std::shared_ptr<RemoteL3KVClient::Session> RemoteL3KVClient::get_session(lite3::NodeID node_id) {
    std::lock_guard<std::mutex> lock(endpoints_mutex_);
    auto it = peer_sessions_.find(node_id);
    if (it == peer_sessions_.end()) return nullptr;
    return it->second;
}

void RemoteL3KVClient::ensure_authenticated(std::shared_ptr<Session> session, lite3::NodeID node_id) {
    if (session->authenticated.load()) return;
    if (auth_secret_.empty()) {
        session->authenticated = true;
        return;
    }

    std::lock_guard<std::recursive_mutex> lock(session->mu);
    if (session->authenticated.load()) return;

    try {
        std::string uid_str = std::to_string(settings_.node_id);

        // Delimiter frame
        session->socket->send(zmq::message_t(), zmq::send_flags::sndmore);

        // EffectiveUID frame for Auth (dummy 0)
        uint32_t dummy_uid = 0;
        session->socket->send(zmq::message_t(&dummy_uid, 4), zmq::send_flags::sndmore);

        session->socket->send(zmq::message_t("A", 1), zmq::send_flags::sndmore);
        session->socket->send(zmq::message_t(uid_str.data(), uid_str.size()), zmq::send_flags::sndmore);
        session->socket->send(zmq::message_t(auth_secret_.data(), auth_secret_.size()), zmq::send_flags::none);

        std::vector<zmq::message_t> recv_msgs;
        auto res = zmq::recv_multipart(*session->socket, std::back_inserter(recv_msgs));
        if (res && recv_msgs.size() >= 2 && recv_msgs[1].to_string() == "OK") {
            session->authenticated = true;
            std::cout << "[RemoteL3KVClient] Auth SUCCESS for peer " << node_id << std::endl;
        } else {
            std::string err = (res && recv_msgs.size() >= 2) ? recv_msgs[1].to_string() : "TIMEOUT";
            std::cerr << "[RemoteL3KVClient] Auth FAILED for peer " << node_id << " error=" << err << std::endl;
            throw std::runtime_error("Authentication failed: " + err);
        }
    } catch (...) {
        report_failure(node_id);
        throw;
    }
}

void RemoteL3KVClient::check_circuit(std::shared_ptr<Session> session) {
    if (session->state.load() == CircuitState::OPEN) {
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - session->last_failure_time).count();
        if (elapsed > settings_.breaker_reset_timeout_ms) {
            std::lock_guard<std::recursive_mutex> lock(session->mu);
            if (session->state == CircuitState::OPEN) {
                session->state.store(CircuitState::HALF_OPEN);
            }
        } else {
            throw CircuitBreakerOpenException("Circuit breaker is OPEN for this peer");
        }
    }
}

void RemoteL3KVClient::run_health_check_loop() {
    while (!stop_health_check_) {
        {
            std::unique_lock<std::mutex> lk(health_check_cv_mu_);
            health_check_cv_.wait_for(lk, std::chrono::milliseconds(settings_.health_check_interval_ms), [this] {
                return stop_health_check_.load();
            });
        }
        if (stop_health_check_) break;

        std::vector<lite3::NodeID> to_check;
        {
            std::lock_guard<std::mutex> lock(endpoints_mutex_);
            for (auto& [id, session] : peer_sessions_) {
                if (session->state.load() == CircuitState::HALF_OPEN) {
                    to_check.push_back(id);
                } else if (session->state.load() == CircuitState::OPEN) {
                    // Also check if we should move from OPEN to HALF_OPEN
                    auto now = std::chrono::steady_clock::now();
                    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - session->last_failure_time).count();
                    if (elapsed > settings_.breaker_reset_timeout_ms) {
                        to_check.push_back(id);
                    }
                }
            }
        }

        for (auto id : to_check) {
            if (stop_health_check_) break;
            
            auto session = get_session(id);
            if (!session) continue;

            // Ensure we are in HALF_OPEN before pinging
            if (session->state.load() == CircuitState::OPEN) {
                std::lock_guard<std::recursive_mutex> lock(session->mu);
                if (session->state == CircuitState::OPEN) {
                    session->state.store(CircuitState::HALF_OPEN);
                }
            }

            if (session->state.load() == CircuitState::HALF_OPEN) {
                // We use ping_peer but we need to wait for it.
                // Since this is a background thread, we can wait on the future.
                auto future = ping_peer(id);
                if (future.wait_for(std::chrono::milliseconds(settings_.fed_timeout_ms + 100)) == std::future_status::ready) {
                    if (future.get()) {
                        report_success(id);
                    } else {
                        report_failure(id);
                    }
                } else {
                    report_failure(id);
                }
            }
        }
    }
}

std::future<bool> RemoteL3KVClient::put_batch_binary_async(lite3::NodeID owner_id, const lite3cpp::Buffer& batch_buffer, uint32_t principal_id) {
    auto session = get_session(owner_id);
    if (!session) {
        std::promise<bool> p; p.set_value(false); return p.get_future();
    }
    
    try {
        check_circuit(session);
        ensure_authenticated(session, owner_id);
    } catch (...) {
        std::promise<bool> p; p.set_exception(std::current_exception()); return p.get_future();
    }

    return task_pool_->enqueue([this, owner_id, session, batch_buffer, principal_id]() -> bool {
        std::lock_guard<std::recursive_mutex> lock(session->mu);
        try {
            session->socket->send(zmq::message_t(), zmq::send_flags::sndmore);
            uint32_t pid = principal_id;
            session->socket->send(zmq::message_t(&pid, 4), zmq::send_flags::sndmore);
            session->socket->send(zmq::message_t("B", 1), zmq::send_flags::sndmore);
            session->socket->send(zmq::message_t(batch_buffer.data(), batch_buffer.size()), zmq::send_flags::none);
            
            std::vector<zmq::message_t> recv_msgs;
            auto res = zmq::recv_multipart(*session->socket, std::back_inserter(recv_msgs));
            if (res && recv_msgs.size() >= 2 && recv_msgs[1].to_string() == "OK") {
                report_success(owner_id);
                return true;
            }
            return false;
        } catch (...) {
            report_failure(owner_id);
            return false;
        }
    });
}

std::future<bool> RemoteL3KVClient::execute_batch_async(lite3::NodeID owner_id, const MutationBatch& batch, uint32_t principal_id) {
    return put_batch_binary_async(owner_id, batch.get_buffer(), principal_id);
}

std::future<bool> RemoteL3KVClient::replicate_async(uint16_t cluster_id, const std::string& key, const std::string& payload, uint16_t origin_cluster_id, uint32_t principal_id) {
    // Note: for now we map ClusterID directly to NodeID for peer lookups
    auto session = get_session(cluster_id);
    if (!session || !task_pool_) {
        std::promise<bool> p; p.set_value(false); return p.get_future();
    }

    return task_pool_->enqueue([this, cluster_id, key, payload, origin_cluster_id, principal_id]() -> bool {
        auto session = get_session(cluster_id);
        if (!session) return false;

        try {
            check_circuit(session);
            ensure_authenticated(session, cluster_id);
        } catch (...) {
            return false;
        }

        std::lock_guard<std::recursive_mutex> lock(session->mu);
        try {
            session->socket->send(zmq::message_t(), zmq::send_flags::sndmore);
            uint32_t pid = principal_id;
            session->socket->send(zmq::message_t(&pid, 4), zmq::send_flags::sndmore);
            session->socket->send(zmq::message_t("S", 1), zmq::send_flags::sndmore);
            session->socket->send(zmq::message_t(key.data(), key.size()), zmq::send_flags::sndmore);
            session->socket->send(zmq::message_t(payload.data(), payload.size()), zmq::send_flags::sndmore);
            
            std::string origin_str = std::to_string(origin_cluster_id);
            session->socket->send(zmq::message_t(origin_str.data(), origin_str.size()), zmq::send_flags::none);

            zmq::message_t msg;
            auto res = session->socket->recv(msg, zmq::recv_flags::none);
            if (res) {
                while (msg.more()) {
                    (void)session->socket->recv(msg, zmq::recv_flags::none);
                }
                report_success(cluster_id);
                return true;
            } else {
                throw FederationTimeoutException("Replication sync timed out");
            }
        } catch (...) {
            report_failure(cluster_id);
            return false;
        }
    });
}

std::future<bool> RemoteL3KVClient::ping_peer(lite3::NodeID node_id) {
    if (!task_pool_) {
        std::promise<bool> p; p.set_value(false); return p.get_future();
    }

    return task_pool_->enqueue([this, node_id]() -> bool {
        auto session = get_session(node_id);
        if (!session) return false;

        std::lock_guard<std::recursive_mutex> lock(session->mu);
        try {
            session->socket->send(zmq::message_t(), zmq::send_flags::sndmore);
            uint32_t internal_uid = INTERNAL_UID;
            session->socket->send(zmq::message_t(&internal_uid, 4), zmq::send_flags::sndmore);
            session->socket->send(zmq::message_t("H", 1), zmq::send_flags::sndmore);
            session->socket->send(zmq::message_t(), zmq::send_flags::none); // Dummy payload

            std::vector<zmq::message_t> recv_msgs;
            auto res = zmq::recv_multipart(*session->socket, std::back_inserter(recv_msgs));
            if (res && recv_msgs.size() >= 2 && recv_msgs[1].to_string() == "OK") {
                return true;
            }
            return false;
        } catch (...) {
            return false;
        }
    });
}

std::future<std::vector<uint64_t>> RemoteL3KVClient::get_neighbors_async(lite3::NodeID owner_id, uint64_t target_node_id, const std::string& label, double min_weight, uint32_t principal_id) {
    if (!task_pool_) {
        std::promise<std::vector<uint64_t>> p; p.set_value({}); return p.get_future();
    }

    return task_pool_->enqueue([this, owner_id, target_node_id, label, min_weight, principal_id]() -> std::vector<uint64_t> {
        auto session = get_session(owner_id);
        if (!session) return {};
        
        try {
            check_circuit(session);
            ensure_authenticated(session, owner_id);
        } catch (...) {
            return {};
        }

        std::lock_guard<std::recursive_mutex> lock(session->mu);
        try {
            char id_buf[17];
            std::snprintf(id_buf, sizeof(id_buf), "%016llx", (unsigned long long)target_node_id);
            
            session->socket->send(zmq::message_t(), zmq::send_flags::sndmore);
            uint32_t pid = principal_id;
            session->socket->send(zmq::message_t(&pid, 4), zmq::send_flags::sndmore);
            session->socket->send(zmq::message_t("N", 1), zmq::send_flags::sndmore);
            session->socket->send(zmq::message_t(id_buf, 16), zmq::send_flags::sndmore);
            session->socket->send(zmq::message_t(label.data(), label.size()), zmq::send_flags::sndmore);
            
            std::string w_str = std::to_string(min_weight);
            session->socket->send(zmq::message_t(w_str.data(), w_str.size()), zmq::send_flags::none);
            
            std::vector<zmq::message_t> recv_msgs;
            auto res = zmq::recv_multipart(*session->socket, std::back_inserter(recv_msgs));
            
            if (res && recv_msgs.size() >= 2) {
                const auto& msg = recv_msgs[1];
                if (msg.size() == 3 && std::memcmp(msg.data(), "ERR", 3) == 0) {
                    report_failure(owner_id);
                    return {};
                }
                report_success(owner_id);
                std::vector<uint64_t> results;
                bool parsed_as_json = false;
                std::string_view sv(static_cast<const char*>(msg.data()), msg.size());
                size_t first = sv.find_first_not_of(" \t\r\n");
                size_t last = sv.find_last_not_of(" \t\r\n");
                if (first != std::string_view::npos && last != std::string_view::npos && last >= first + 1 && sv[first] == '[' && sv[last] == ']') {
                    try {
                        lite3cpp::Buffer buf = lite3cpp::lite3_json::from_json_string(std::string(sv.substr(first, last - first + 1)));
                        if (buf.size() >= sizeof(lite3cpp::PackedNodeLayout)) {
                            lite3cpp::NodeView nv(reinterpret_cast<const lite3cpp::PackedNodeLayout*>(buf.data()));
                            if (nv.type() == lite3cpp::Type::Array) {
                                parsed_as_json = true;
                                for (uint32_t i = 0; i < nv.size(); ++i) {
                                    auto t = buf.arr_get_type(0, i);
                                    if (t == lite3cpp::Type::Int64) {
                                        results.push_back(static_cast<uint64_t>(buf.arr_get_i64(0, i)));
                                    } else if (t == lite3cpp::Type::String) {
                                        results.push_back(std::stoull(std::string(buf.arr_get_str(0, i)), nullptr, 16));
                                    }
                                }
                            }
                        }
                    } catch (...) {
                        parsed_as_json = false;
                    }
                }
                if (!parsed_as_json && msg.size() % sizeof(uint64_t) == 0) {
                    size_t count = msg.size() / sizeof(uint64_t);
                    results.resize(count);
                    if (count > 0) {
                        std::memcpy(results.data(), msg.data(), count * sizeof(uint64_t));
                    }
                }
                return results;
            } else {
                throw FederationTimeoutException("Neighbor fetch timed out");
            }
        } catch (...) {
            report_failure(owner_id);
            throw;
        }
    });
}

std::future<std::vector<uint64_t>> RemoteL3KVClient::get_in_neighbors_async(lite3::NodeID owner_id, uint64_t target_node_id, const std::string& label, uint32_t principal_id) {
    if (!task_pool_) {
        std::promise<std::vector<uint64_t>> p; p.set_value({}); return p.get_future();
    }

    return task_pool_->enqueue([this, owner_id, target_node_id, label, principal_id]() -> std::vector<uint64_t> {
        auto session = get_session(owner_id);
        if (!session) return {};
        
        try {
            check_circuit(session);
            ensure_authenticated(session, owner_id);
        } catch (...) {
            return {};
        }

        std::lock_guard<std::recursive_mutex> lock(session->mu);
        try {
            char id_buf[17];
            std::snprintf(id_buf, sizeof(id_buf), "%016llx", (unsigned long long)target_node_id);
            
            session->socket->send(zmq::message_t(), zmq::send_flags::sndmore);
            uint32_t pid = principal_id;
            session->socket->send(zmq::message_t(&pid, 4), zmq::send_flags::sndmore);
            session->socket->send(zmq::message_t("I", 1), zmq::send_flags::sndmore);
            session->socket->send(zmq::message_t(id_buf, 16), zmq::send_flags::sndmore);
            session->socket->send(zmq::message_t(label.data(), label.size()), zmq::send_flags::none);
            
            std::vector<zmq::message_t> recv_msgs;
            auto res = zmq::recv_multipart(*session->socket, std::back_inserter(recv_msgs));
            
            if (res && recv_msgs.size() >= 2) {
                const auto& msg = recv_msgs[1];
                if (msg.size() == 3 && std::memcmp(msg.data(), "ERR", 3) == 0) {
                    report_failure(owner_id);
                    return {};
                }
                report_success(owner_id);
                std::vector<uint64_t> results;
                bool parsed_as_json = false;
                std::string_view sv(static_cast<const char*>(msg.data()), msg.size());
                size_t first = sv.find_first_not_of(" \t\r\n");
                size_t last = sv.find_last_not_of(" \t\r\n");
                if (first != std::string_view::npos && last != std::string_view::npos && last >= first + 1 && sv[first] == '[' && sv[last] == ']') {
                    try {
                        lite3cpp::Buffer buf = lite3cpp::lite3_json::from_json_string(std::string(sv.substr(first, last - first + 1)));
                        if (buf.size() >= sizeof(lite3cpp::PackedNodeLayout)) {
                            lite3cpp::NodeView nv(reinterpret_cast<const lite3cpp::PackedNodeLayout*>(buf.data()));
                            if (nv.type() == lite3cpp::Type::Array) {
                                parsed_as_json = true;
                                for (uint32_t i = 0; i < nv.size(); ++i) {
                                    auto t = buf.arr_get_type(0, i);
                                    if (t == lite3cpp::Type::Int64) {
                                        results.push_back(static_cast<uint64_t>(buf.arr_get_i64(0, i)));
                                    } else if (t == lite3cpp::Type::String) {
                                        results.push_back(std::stoull(std::string(buf.arr_get_str(0, i)), nullptr, 16));
                                    }
                                }
                            }
                        }
                    } catch (...) {
                        parsed_as_json = false;
                    }
                }
                if (!parsed_as_json && msg.size() % sizeof(uint64_t) == 0) {
                    size_t count = msg.size() / sizeof(uint64_t);
                    results.resize(count);
                    if (count > 0) {
                        std::memcpy(results.data(), msg.data(), count * sizeof(uint64_t));
                    }
                }
                return results;
            } else {
                throw FederationTimeoutException("In-Neighbor fetch timed out");
            }
        } catch (...) {
            report_failure(owner_id);
            throw;
        }
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
    if (!task_pool_) { std::promise<std::vector<ResultRow>> p; p.set_value({}); return p.get_future(); }
    return task_pool_->enqueue([this, cluster_id, starting_nodes, query_buf, principal_id]() -> std::vector<ResultRow> {
        auto session = get_session(cluster_id);
        if (!session) {
            #ifdef IRODS_SERVER
            rodsLog(LOG_ERROR, "L3KV_CLIENT: No session for cluster %u", cluster_id);
            #endif
            return {};
        }

        try {
            check_circuit(session);
            ensure_authenticated(session, cluster_id);
        } catch (...) {
            throw; 
        }

        std::lock_guard<std::recursive_mutex> lock(session->mu);
        try {
            session->socket->send(zmq::message_t(), zmq::send_flags::sndmore);
            uint32_t pid = principal_id;
            session->socket->send(zmq::message_t(&pid, 4), zmq::send_flags::sndmore);
            session->socket->send(zmq::message_t("R", 1), zmq::send_flags::sndmore);
            session->socket->send(zmq::message_t(starting_nodes.data(), starting_nodes.size() * sizeof(uint64_t)), zmq::send_flags::sndmore);
            session->socket->send(zmq::message_t(query_buf.data(), query_buf.size()), zmq::send_flags::none);
            
            std::vector<zmq::message_t> recv_msgs;
            
            zmq::message_t msg;
            auto res = session->socket->recv(msg, zmq::recv_flags::none);
            
            if (res) {
                recv_msgs.push_back(std::move(msg));
                while (recv_msgs.back().more()) {
                    zmq::message_t m;
                    auto res_more = session->socket->recv(m, zmq::recv_flags::none);
                    if (!res_more) break;
                    recv_msgs.push_back(std::move(m));
                }
            }
            
            if (res && recv_msgs.size() >= 2) {
                const auto& resp_msg = recv_msgs[1];
                if (resp_msg.size() == 3 && std::memcmp(resp_msg.data(), "ERR", 3) == 0) {
                    report_failure(cluster_id);
                    return {};
                }
                report_success(cluster_id);
                lite3cpp::Buffer resp_buf;
                const uint8_t* ptr = static_cast<const uint8_t*>(resp_msg.data());
                if (resp_msg.size() >= sizeof(lite3cpp::PackedNodeLayout) && (ptr[0] == 0x06 || ptr[0] == 0x07)) {
                    resp_buf = lite3cpp::Buffer(std::vector<uint8_t>(ptr, ptr + resp_msg.size()));
                } else {
                    std::string raw_json = resp_msg.to_string();
                    resp_buf = lite3cpp::lite3_json::from_json_string(raw_json.empty() ? "[]" : raw_json);
                }

                std::vector<ResultRow> results;
                if (resp_buf.size() >= sizeof(lite3cpp::PackedNodeLayout)) {
                    lite3cpp::NodeView root_nv(reinterpret_cast<const lite3cpp::PackedNodeLayout*>(resp_buf.data()));
                    if (root_nv.type() == lite3cpp::Type::Array) {
                        uint32_t row_count = root_nv.size();
                        for (uint32_t r = 0; r < row_count; ++r) {
                            if (resp_buf.arr_get_type(0, r) != lite3cpp::Type::Object) continue;
                            size_t row_ofs = resp_buf.arr_get_obj(0, r);
                            size_t fields_ofs = row_ofs;
                            if (resp_buf.get_type(row_ofs, "fields") == lite3cpp::Type::Object) {
                                fields_ofs = resp_buf.get_obj(row_ofs, "fields");
                            }
                            ResultRow row;
                            for (auto it = resp_buf.begin(fields_ofs); it != resp_buf.end(fields_ofs); ++it) {
                                std::string k(it->key);
                                if (it->value_type == lite3cpp::Type::String) {
                                    row.fields[k] = std::string(resp_buf.get_str(fields_ofs, k));
                                } else if (it->value_type == lite3cpp::Type::Int64) {
                                    row.fields[k] = std::to_string(resp_buf.get_i64(fields_ofs, k));
                                } else if (it->value_type == lite3cpp::Type::Float64) {
                                    row.fields[k] = std::to_string(resp_buf.get_f64(fields_ofs, k));
                                } else if (it->value_type == lite3cpp::Type::Bool) {
                                    row.fields[k] = resp_buf.get_bool(fields_ofs, k) ? "true" : "false";
                                } else if (it->value_type == lite3cpp::Type::Null) {
                                    row.fields[k] = "";
                                } else {
                                    row.fields[k] = "";
                                }
                            }
                            results.push_back(std::move(row));
                        }
                    }
                }
                return results;
            } else {
                throw FederationTimeoutException("Remote query timed out");
            }
        } catch (...) {
            report_failure(cluster_id);
            throw;
        }
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
        buf = lite3cpp::Buffer(std::vector<uint8_t>(ptr, ptr + query_payload.size()));
    } else {
        buf = lite3cpp::lite3_json::from_json_string(query_payload.empty() ? "{}" : query_payload);
    }
    return resume_query_async(cluster_id, starting_nodes, buf, principal_id);
}

std::future<bool> RemoteL3KVClient::put_edge_async(lite3::NodeID owner_id, const std::string& edge_key, const std::string& json_payload, uint32_t principal_id) {
    auto session = get_session(owner_id);
    if (!session) {
        std::promise<bool> p; p.set_value(false); return p.get_future();
    }

    try {
        check_circuit(session);
        ensure_authenticated(session, owner_id);
    } catch (...) {
        std::promise<bool> p; p.set_exception(std::current_exception()); return p.get_future();
    }

    return task_pool_->enqueue([this, owner_id, session, edge_key, json_payload, principal_id]() -> bool {
        std::lock_guard<std::recursive_mutex> lock(session->mu);
        try {
            if(0) std::fprintf(stderr, "[RemoteClient] Sending P to owner %u: key=[%s]\n", owner_id, edge_key.c_str()); //std::fflush(stderr);
            session->socket->send(zmq::message_t(), zmq::send_flags::sndmore);
            uint32_t pid = principal_id;
            session->socket->send(zmq::message_t(&pid, 4), zmq::send_flags::sndmore);
            session->socket->send(zmq::message_t("P", 1), zmq::send_flags::sndmore);
            session->socket->send(zmq::message_t(edge_key.data(), edge_key.size()), zmq::send_flags::sndmore);
            session->socket->send(zmq::message_t(json_payload.data(), json_payload.size()), zmq::send_flags::none);
            
            std::vector<zmq::message_t> recv_msgs;
            auto res = zmq::recv_multipart(*session->socket, std::back_inserter(recv_msgs));
            if (res && recv_msgs.size() >= 2 && recv_msgs[1].to_string() == "OK") {
                report_success(owner_id);
                return true;
            }
            return false;
        } catch (...) {
            report_failure(owner_id);
            return false;
        }
    });
}

std::future<uint64_t> RemoteL3KVClient::atomic_incr_async(lite3::NodeID owner_id, const std::string& key, int64_t delta, uint32_t principal_id) {
    auto promise = std::make_shared<std::promise<uint64_t>>();
    auto future = promise->get_future();

    if (!task_pool_) { promise->set_value(0); return future; }

    task_pool_->enqueue([this, owner_id, key, delta, principal_id, promise]() {
        auto session = get_session(owner_id);
        if (!session) { promise->set_value(0); return; }

        std::lock_guard<std::recursive_mutex> lock(session->mu);
        try {
            session->socket->send(zmq::message_t(), zmq::send_flags::sndmore);
            uint32_t pid = principal_id;
            session->socket->send(zmq::message_t(&pid, 4), zmq::send_flags::sndmore);
            session->socket->send(zmq::message_t("+", 1), zmq::send_flags::sndmore);
            session->socket->send(zmq::message_t(key.data(), key.size()), zmq::send_flags::sndmore);
            std::string d_str = std::to_string(delta);
            session->socket->send(zmq::message_t(d_str.data(), d_str.size()), zmq::send_flags::none);

            std::vector<zmq::message_t> recv_msgs;
            auto res = zmq::recv_multipart(*session->socket, std::back_inserter(recv_msgs));
            if (res && recv_msgs.size() >= 2) {
                std::string val_str = recv_msgs[1].to_string();
                if (val_str == "ERR") promise->set_value(0);
                else promise->set_value(std::stoull(val_str));
            } else {
                promise->set_value(0);
            }
        } catch (...) {
            promise->set_value(0);
        }
    });

    return future;
}

std::future<std::string> RemoteL3KVClient::get_raw_key_async(lite3::NodeID owner_id, const std::string& key, uint32_t principal_id) {
    if (!task_pool_) { std::promise<std::string> p; p.set_value(""); return p.get_future(); }
    return task_pool_->enqueue([this, owner_id, key, principal_id]() -> std::string {
        auto session = get_session(owner_id);
        if (!session) return "";
        try { check_circuit(session); ensure_authenticated(session, owner_id); } catch (...) { throw; }
        std::lock_guard<std::recursive_mutex> lock(session->mu);
        try {
            session->socket->send(zmq::message_t(), zmq::send_flags::sndmore);
            uint32_t pid = principal_id;
            session->socket->send(zmq::message_t(&pid, 4), zmq::send_flags::sndmore);
            session->socket->send(zmq::message_t("G", 1), zmq::send_flags::sndmore);
            session->socket->send(zmq::message_t(key.data(), key.size()), zmq::send_flags::none);
            std::vector<zmq::message_t> recv_msgs;
            auto res = zmq::recv_multipart(*session->socket, std::back_inserter(recv_msgs));
            if (res && recv_msgs.size() >= 2) {
                std::string resp = recv_msgs[1].to_string();
                if (resp.starts_with("ERR_")) return "";
                report_success(owner_id);
                return resp;
            }
        } catch (...) { report_failure(owner_id); }
        return "";
    });
}

std::future<std::vector<std::pair<std::string, std::string>>> RemoteL3KVClient::get_prefix_entries_async(
    lite3::NodeID owner_id,
    const std::string& prefix,
    uint32_t principal_id
) {
    if (!task_pool_) {
        std::promise<std::vector<std::pair<std::string, std::string>>> p;
        p.set_value({});
        return p.get_future();
    }

    return task_pool_->enqueue([this, owner_id, prefix, principal_id]() -> std::vector<std::pair<std::string, std::string>> {
        auto session = get_session(owner_id);
        if (!session) return {};

        try {
            check_circuit(session);
            ensure_authenticated(session, owner_id);
        } catch (...) {
            return {};
        }

        std::lock_guard<std::recursive_mutex> lock(session->mu);
        try {
            session->socket->send(zmq::message_t(), zmq::send_flags::sndmore);
            uint32_t pid = principal_id;
            session->socket->send(zmq::message_t(&pid, 4), zmq::send_flags::sndmore);
            session->socket->send(zmq::message_t("K", 1), zmq::send_flags::sndmore);
            session->socket->send(zmq::message_t(prefix.data(), prefix.size()), zmq::send_flags::none);

            std::vector<zmq::message_t> recv_msgs;
            auto res = zmq::recv_multipart(*session->socket, std::back_inserter(recv_msgs));
            if (res && recv_msgs.size() >= 2) {
                std::string resp = recv_msgs[1].to_string();
                if (resp.starts_with("ERR_") || resp == "ERR") {
                    return {};
                }
                report_success(owner_id);

                std::vector<std::pair<std::string, std::string>> entries;
                if (recv_msgs[1].size() > 0) {
                    std::vector<uint8_t> bytes(static_cast<const uint8_t*>(recv_msgs[1].data()),
                                               static_cast<const uint8_t*>(recv_msgs[1].data()) + recv_msgs[1].size());
                    lite3cpp::Buffer buf(std::move(bytes));
                    if (buf.size() >= sizeof(lite3cpp::PackedNodeLayout)) {
                        lite3cpp::NodeView nv(reinterpret_cast<const lite3cpp::PackedNodeLayout*>(buf.data()));
                        if (nv.type() == lite3cpp::Type::Array) {
                            uint32_t arr_size = nv.size();
                            for (uint32_t i = 0; i < arr_size; ++i) {
                                auto elem_type = buf.arr_get_type(0, i);
                                if (elem_type == lite3cpp::Type::Object) {
                                    size_t obj_ofs = buf.arr_get_obj(0, i);
                                    std::string k, v;
                                    if (buf.get_type(obj_ofs, "k") == lite3cpp::Type::String) {
                                        k = std::string(buf.get_str(obj_ofs, "k"));
                                    }
                                    if (buf.get_type(obj_ofs, "v") == lite3cpp::Type::String) {
                                        v = std::string(buf.get_str(obj_ofs, "v"));
                                    }
                                    entries.push_back({std::move(k), std::move(v)});
                                } else if (elem_type == lite3cpp::Type::String) {
                                    std::string k = std::string(buf.arr_get_str(0, i));
                                    entries.push_back({std::move(k), ""});
                                }
                            }
                        }
                    }
                }
                return entries;
            }
        } catch (...) {
            report_failure(owner_id);
        }
        return {};
    });
}

std::future<std::vector<std::string>> RemoteL3KVClient::get_prefix_keys_async(
    lite3::NodeID owner_id,
    const std::string& prefix,
    uint32_t principal_id
) {
    if (!task_pool_) {
        std::promise<std::vector<std::string>> p;
        p.set_value({});
        return p.get_future();
    }

    auto entries_fut = get_prefix_entries_async(owner_id, prefix, principal_id);
    return task_pool_->enqueue([fut = std::move(entries_fut)]() mutable -> std::vector<std::string> {
        auto entries = fut.get();
        std::vector<std::string> keys;
        keys.reserve(entries.size());
        for (auto& [k, v] : entries) {
            keys.push_back(std::move(k));
        }
        return keys;
    });
}

std::future<std::string> RemoteL3KVClient::get_node_payload_async(lite3::NodeID owner_id, uint64_t target_node_id, uint32_t principal_id) {
    if (!task_pool_) {
        std::promise<std::string> p; p.set_value(""); return p.get_future();
    }

    return task_pool_->enqueue([this, owner_id, target_node_id, principal_id]() -> std::string {
        auto session = get_session(owner_id);
        if (!session) return "";

        try {
            check_circuit(session);
            ensure_authenticated(session, owner_id);
        } catch (...) {
            throw;
        }

        std::lock_guard<std::recursive_mutex> lock(session->mu);

        try {
            std::string key = std::string(l3kvg::KeyBuilder::node_key(target_node_id));
            
            session->socket->send(zmq::message_t(), zmq::send_flags::sndmore);
            uint32_t pid = principal_id;
            session->socket->send(zmq::message_t(&pid, 4), zmq::send_flags::sndmore);
            session->socket->send(zmq::message_t("G", 1), zmq::send_flags::sndmore);
            session->socket->send(zmq::message_t(key.data(), key.size()), zmq::send_flags::none);
            
            std::vector<zmq::message_t> recv_msgs;
            auto start_ts = std::chrono::steady_clock::now();
            auto res = zmq::recv_multipart(*session->socket, std::back_inserter(recv_msgs));
            auto end_ts = std::chrono::steady_clock::now();
            auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_ts - start_ts).count();
            
            if (res && recv_msgs.size() >= 2) {
                if(0) std::fprintf(stderr, "  [ZMQ] G SUCCESS. took %lld ms. size=%zu\n", (long long)duration, recv_msgs[1].size()); //std::fflush(stderr);
                std::string resp = recv_msgs[1].to_string();
                if (resp.starts_with("ERR_")) {
                    return ""; // Security rejection
                }
                report_success(owner_id);
                return resp;
            } else {
                throw FederationTimeoutException("Node fetch timed out");
            }
        } catch (const std::exception& e) {
            report_failure(owner_id);
            throw;
        } catch (...) {
            report_failure(owner_id);
            throw;
        }
    });
}

std::future<std::unordered_map<uint64_t, std::string>> RemoteL3KVClient::get_nodes_batch_async(lite3::NodeID owner_id, const std::vector<uint64_t>& node_ids, uint32_t principal_id) {
    auto session = get_session(owner_id);
    if (!session || !task_pool_) {
        std::promise<std::unordered_map<uint64_t, std::string>> p; p.set_value({}); return p.get_future();
    }

    return task_pool_->enqueue([this, owner_id, session, node_ids, principal_id]() -> std::unordered_map<uint64_t, std::string> {
        try {
            check_circuit(session);
            ensure_authenticated(session, owner_id);
        } catch (...) {
            return {};
        }

        std::lock_guard<std::recursive_mutex> lock(session->mu);
        try {
            session->socket->send(zmq::message_t(), zmq::send_flags::sndmore);
            uint32_t pid = principal_id;
            session->socket->send(zmq::message_t(&pid, 4), zmq::send_flags::sndmore);
            session->socket->send(zmq::message_t("M", 1), zmq::send_flags::sndmore);
            
            for (size_t i = 0; i < node_ids.size(); ++i) {
                std::string key = std::string(KeyBuilder::node_key(node_ids[i]));
                session->socket->send(zmq::message_t(key.data(), key.size()), (i == node_ids.size() - 1) ? zmq::send_flags::none : zmq::send_flags::sndmore);
            }
            
            std::vector<zmq::message_t> recv_msgs;
            auto start_ts = std::chrono::steady_clock::now();
            auto res = zmq::recv_multipart(*session->socket, std::back_inserter(recv_msgs));
            auto end_ts = std::chrono::steady_clock::now();
            auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_ts - start_ts).count();
            
            std::unordered_map<uint64_t, std::string> results;
            if (res && recv_msgs.size() >= 2) {
                if(0) std::fprintf(stderr, "  [ZMQ] M SUCCESS. took %lld ms. size=%zu\n", (long long)duration, recv_msgs[1].size()); //std::fflush(stderr);
                report_success(owner_id);
                auto& body = recv_msgs[1];
                lite3cpp::Buffer buf(std::vector<uint8_t>((uint8_t*)body.data(), (uint8_t*)body.data() + body.size()));
                
                size_t root = 0;
                for (auto it = buf.begin(root); it != buf.end(root); ++it) {
                    std::string key(it->key);
                    if (key.starts_with("n:{") && key.ends_with("}")) {
                        uint64_t id = std::stoull(key.substr(3, key.size() - 4), nullptr, 16);
                        auto type = buf.get_type(root, key);
                        if (type == lite3cpp::Type::Bytes) {
                            auto b = buf.get_bytes(root, key);
                            results[id] = std::string(reinterpret_cast<const char*>(b.data()), b.size());
                        } else if (type == lite3cpp::Type::String) {
                            results[id] = buf.get_str(root, key);
                        }
                    }
                }
            }
            return results;
        } catch (...) {
            report_failure(owner_id);
            return {};
        }
    });
}

std::future<bool> RemoteL3KVClient::put_node_async(lite3::NodeID owner_id, uint64_t target_node_id, const std::string& json_payload, uint32_t principal_id) {
    std::string key = std::string(KeyBuilder::node_key(target_node_id));
    return put_edge_async(owner_id, key, json_payload, principal_id);
}

std::future<bool> RemoteL3KVClient::del_node_async(lite3::NodeID owner_id, uint64_t target_node_id, uint32_t principal_id) {
    std::string key = std::string(KeyBuilder::node_key(target_node_id));
    return del_edge_async(owner_id, key, principal_id);
}

std::future<bool> RemoteL3KVClient::del_edge_async(lite3::NodeID owner_id, const std::string& edge_key, uint32_t principal_id) {
    auto session = get_session(owner_id);
    if (!session) {
        std::promise<bool> p; p.set_value(false); return p.get_future();
    }

    try {
        check_circuit(session);
        ensure_authenticated(session, owner_id);
    } catch (...) {
        std::promise<bool> p; p.set_exception(std::current_exception()); return p.get_future();
    }

    return task_pool_->enqueue([this, owner_id, session, edge_key, principal_id]() -> bool {
        std::lock_guard<std::recursive_mutex> lock(session->mu);
        try {
            session->socket->send(zmq::message_t(), zmq::send_flags::sndmore);
            uint32_t pid = principal_id;
            session->socket->send(zmq::message_t(&pid, 4), zmq::send_flags::sndmore);
            session->socket->send(zmq::message_t("D", 1), zmq::send_flags::sndmore);
            session->socket->send(zmq::message_t(edge_key.data(), edge_key.size()), zmq::send_flags::none);
            
            std::vector<zmq::message_t> recv_msgs;
            auto res = zmq::recv_multipart(*session->socket, std::back_inserter(recv_msgs));
            if (res && recv_msgs.size() >= 2 && recv_msgs[1].to_string() == "OK") {
                report_success(owner_id);
                return true;
            }
            return false;
        } catch (...) {
            report_failure(owner_id);
            return false;
        }
    });
}

std::future<bool> RemoteL3KVClient::put_batch_async(lite3::NodeID owner_id, const std::unordered_map<uint64_t, std::string>& batch, uint32_t principal_id) {
    std::promise<bool> p; p.set_value(false); return p.get_future();
}

CircuitState RemoteL3KVClient::get_circuit_state(lite3::NodeID node_id) {
    auto session = get_session(node_id);
    if (!session) return CircuitState::OPEN;
    return session->state.load();
}

void RemoteL3KVClient::set_circuit_state(lite3::NodeID node_id, CircuitState state) {
    auto session = get_session(node_id);
    if (!session) return;
    std::lock_guard<std::recursive_mutex> lock(session->mu);
    session->state.store(state);
    if (state == CircuitState::OPEN) {
        session->last_failure_time = std::chrono::steady_clock::now();
    } else if (state == CircuitState::CLOSED) {
        session->consecutive_failures = 0;
    }
}

void RemoteL3KVClient::report_failure(lite3::NodeID node_id) {
    auto session = get_session(node_id);
    if (!session) return;

    std::lock_guard<std::recursive_mutex> lock(session->mu);
    if (session->state == CircuitState::OPEN) return;

    int failures = ++session->consecutive_failures;
    if (session->state == CircuitState::HALF_OPEN || failures >= settings_.breaker_failure_threshold) {
        session->state.store(CircuitState::OPEN);
        session->last_failure_time = std::chrono::steady_clock::now();
        std::cout << "[RemoteL3KVClient] Circuit OPEN for peer " << node_id << " after " << failures << " failures" << std::endl;
    }
}

void RemoteL3KVClient::report_success(lite3::NodeID node_id) {
    auto session = get_session(node_id);
    if (!session) return;

    std::lock_guard<std::recursive_mutex> lock(session->mu);
    session->consecutive_failures = 0;
    if (session->state == CircuitState::HALF_OPEN) {
        session->state.store(CircuitState::CLOSED);
        std::cout << "[RemoteL3KVClient] Circuit CLOSED for peer " << node_id << std::endl;
    }
}

} // namespace l3kvg
