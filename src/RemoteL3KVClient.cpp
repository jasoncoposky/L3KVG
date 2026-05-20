#include "L3KVG/RemoteL3KVClient.hpp"
#include "L3KVG/KeyBuilder.hpp"
#include <iostream>
#include <thread>
#include <zmq_addon.hpp>
#include <nlohmann/json.hpp>

namespace l3kvg {

RemoteL3KVClient::RemoteL3KVClient(const Settings& settings) 
    : settings_(settings), zmq_sndhwm_(settings.zmq_sndhwm), zmq_ctx_(1) {
    health_check_thread_ = std::thread(&RemoteL3KVClient::run_health_check_loop, this);
}

RemoteL3KVClient::~RemoteL3KVClient() {
    stop_health_check_ = true;
    if (health_check_thread_.joinable()) {
        health_check_thread_.join();
    }

    std::lock_guard<std::mutex> lock(endpoints_mutex_);
    for (auto& [id, session] : peer_sessions_) {
        std::lock_guard<std::recursive_mutex> s_lock(session->mu);
        if (session->socket) {
            session->socket->close();
        }
    }
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
        std::this_thread::sleep_for(std::chrono::milliseconds(settings_.health_check_interval_ms));
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

    return task_pool_->enqueue([this, owner_id, session, &batch_buffer, principal_id]() -> bool {
        std::lock_guard<std::recursive_mutex> lock(session->mu);
        try {
            session->socket->send(zmq::message_t(), zmq::send_flags::sndmore);
            session->socket->send(zmq::message_t(&principal_id, 4), zmq::send_flags::sndmore);
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
            session->socket->send(zmq::message_t(&principal_id, 4), zmq::send_flags::sndmore);
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
            session->socket->send(zmq::message_t(&principal_id, 4), zmq::send_flags::sndmore);
            session->socket->send(zmq::message_t("N", 1), zmq::send_flags::sndmore);
            session->socket->send(zmq::message_t(id_buf, 16), zmq::send_flags::sndmore);
            session->socket->send(zmq::message_t(label.data(), label.size()), zmq::send_flags::sndmore);
            
            std::string w_str = std::to_string(min_weight);
            session->socket->send(zmq::message_t(w_str.data(), w_str.size()), zmq::send_flags::none);
            
            std::vector<zmq::message_t> recv_msgs;
            auto res = zmq::recv_multipart(*session->socket, std::back_inserter(recv_msgs));
            
            if (res && recv_msgs.size() >= 2) {
                report_success(owner_id);
                nlohmann::json j_neighs = nlohmann::json::parse(recv_msgs[1].to_string());
                std::vector<uint64_t> results;
                for (const auto& item : j_neighs) {
                    results.push_back(std::stoull(item.get<std::string>(), nullptr, 16));
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
            session->socket->send(zmq::message_t(&principal_id, 4), zmq::send_flags::sndmore);
            session->socket->send(zmq::message_t("I", 1), zmq::send_flags::sndmore);
            session->socket->send(zmq::message_t(id_buf, 16), zmq::send_flags::sndmore);
            session->socket->send(zmq::message_t(label.data(), label.size()), zmq::send_flags::none);
            
            std::vector<zmq::message_t> recv_msgs;
            auto res = zmq::recv_multipart(*session->socket, std::back_inserter(recv_msgs));
            
            if (res && recv_msgs.size() >= 2) {
                report_success(owner_id);
                nlohmann::json j_neighs = nlohmann::json::parse(recv_msgs[1].to_string());
                std::vector<uint64_t> results;
                for (const auto& item : j_neighs) {
                    results.push_back(std::stoull(item.get<std::string>(), nullptr, 16));
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

std::future<std::vector<ResultRow>> RemoteL3KVClient::resume_query_async(uint16_t cluster_id, const std::vector<uint64_t>& starting_nodes, const std::string& query_json, uint32_t principal_id) {
    if (!task_pool_) {
        std::promise<std::vector<ResultRow>> p; p.set_value({}); return p.get_future();
    }

    return task_pool_->enqueue([this, cluster_id, starting_nodes, query_json, principal_id]() -> std::vector<ResultRow> {
        auto session = get_session(cluster_id);
        if (!session) return {};
        
        try {
            check_circuit(session);
            ensure_authenticated(session, cluster_id);
        } catch (...) {
            return {};
        }

        std::lock_guard<std::recursive_mutex> lock(session->mu);
        try {
            nlohmann::json j_nodes = starting_nodes;
            std::string nodes_json = j_nodes.dump();
            
            session->socket->send(zmq::message_t(), zmq::send_flags::sndmore);
            session->socket->send(zmq::message_t(&principal_id, 4), zmq::send_flags::sndmore);
            session->socket->send(zmq::message_t("R", 1), zmq::send_flags::sndmore);
            session->socket->send(zmq::message_t(nodes_json.data(), nodes_json.size()), zmq::send_flags::sndmore);
            session->socket->send(zmq::message_t(query_json.data(), query_json.size()), zmq::send_flags::none);
            
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
                report_success(cluster_id);
                nlohmann::json j_res = nlohmann::json::parse(recv_msgs[1].to_string());
                std::vector<ResultRow> results;
                for (const auto& jr : j_res) {
                    ResultRow row;
                    for (auto it = jr.begin(); it != jr.end(); ++it) {
                        row.fields[it.key()] = it.value();
                    }
                    results.push_back(std::move(row));
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
            session->socket->send(zmq::message_t(), zmq::send_flags::sndmore);
            session->socket->send(zmq::message_t(&principal_id, 4), zmq::send_flags::sndmore);
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
            return "";
        }

        std::lock_guard<std::recursive_mutex> lock(session->mu);

        try {
            char id_buf[17];
            std::snprintf(id_buf, sizeof(id_buf), "%016llx", (unsigned long long)target_node_id);
            
            session->socket->send(zmq::message_t(), zmq::send_flags::sndmore);
            session->socket->send(zmq::message_t(&principal_id, 4), zmq::send_flags::sndmore);
            session->socket->send(zmq::message_t("G", 1), zmq::send_flags::sndmore);
            session->socket->send(zmq::message_t(id_buf, 16), zmq::send_flags::none);
            
            std::vector<zmq::message_t> recv_msgs;
            auto res = zmq::recv_multipart(*session->socket, std::back_inserter(recv_msgs));
            
            if (res && recv_msgs.size() >= 2) {
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
            session->socket->send(zmq::message_t(&principal_id, 4), zmq::send_flags::sndmore);
            session->socket->send(zmq::message_t("M", 1), zmq::send_flags::sndmore);
            
            for (size_t i = 0; i < node_ids.size(); ++i) {
                std::string key = std::string(KeyBuilder::node_key(node_ids[i]));
                session->socket->send(zmq::message_t(key.data(), key.size()), (i == node_ids.size() - 1) ? zmq::send_flags::none : zmq::send_flags::sndmore);
            }
            
            std::vector<zmq::message_t> recv_msgs;
            auto res = zmq::recv_multipart(*session->socket, std::back_inserter(recv_msgs));
            
            std::unordered_map<uint64_t, std::string> results;
            if (res && recv_msgs.size() >= 2) {
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
            session->socket->send(zmq::message_t(&principal_id, 4), zmq::send_flags::sndmore);
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
