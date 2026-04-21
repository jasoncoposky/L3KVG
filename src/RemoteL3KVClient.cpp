#include "L3KVG/RemoteL3KVClient.hpp"
#include <iostream>
#include <thread>
#include <zmq_addon.hpp>

namespace l3kvg {

RemoteL3KVClient::RemoteL3KVClient() : zmq_ctx_(1) {}

RemoteL3KVClient::~RemoteL3KVClient() {
    std::lock_guard<std::mutex> lock(endpoints_mutex_);
    for (auto& [id, session] : peer_sessions_) {
        std::lock_guard<std::mutex> s_lock(session->mu);
        if (session->socket) {
            session->socket->close();
        }
    }
}

void RemoteL3KVClient::add_peer(lite3::NodeID node_id, const std::string& endpoint_url) {
    std::lock_guard<std::mutex> lock(endpoints_mutex_);
    
    std::string url = endpoint_url;
    if (url.starts_with("http://")) url = url.substr(7);
    
    if (url.find("tcp://") == std::string::npos) {
        url = "tcp://" + url;
    }

    peer_endpoints_[node_id] = url;
    auto session = std::make_shared<Session>();
    
    session->socket = std::make_unique<zmq::socket_t>(zmq_ctx_, ZMQ_DEALER);
    session->socket->set(zmq::sockopt::sndhwm, 1000);
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

std::future<bool> RemoteL3KVClient::put_batch_binary_async(lite3::NodeID owner_id, const lite3cpp::Buffer& batch_buffer) {
    auto session = get_session(owner_id);
    if (!session) {
        std::promise<bool> p; p.set_value(false); return p.get_future();
    }
    std::lock_guard<std::mutex> lock(session->mu);
    try {
        session->socket->send(zmq::message_t(), zmq::send_flags::sndmore);
        session->socket->send(zmq::message_t("B", 1), zmq::send_flags::sndmore);
        session->socket->send(zmq::message_t(batch_buffer.data(), batch_buffer.size()), zmq::send_flags::none);
        std::promise<bool> p; p.set_value(true); return p.get_future();
    } catch (...) {
        std::promise<bool> p; p.set_value(false); return p.get_future();
    }
}

std::future<std::vector<std::string>> RemoteL3KVClient::get_neighbors_async(lite3::NodeID owner_id, const std::string& target_node_id, const std::string& label, double min_weight) {
    std::promise<std::vector<std::string>> p; p.set_value({}); return p.get_future();
}

std::future<bool> RemoteL3KVClient::put_edge_async(lite3::NodeID owner_id, const std::string& edge_key, const std::string& json_payload) {
    auto session = get_session(owner_id);
    if (!session) {
        std::promise<bool> p; p.set_value(false); return p.get_future();
    }
    std::lock_guard<std::mutex> lock(session->mu);
    try {
        session->socket->send(zmq::message_t(), zmq::send_flags::sndmore);
        session->socket->send(zmq::message_t("P", 1), zmq::send_flags::sndmore);
        session->socket->send(zmq::message_t(edge_key.data(), edge_key.size()), zmq::send_flags::sndmore);
        session->socket->send(zmq::message_t(json_payload.data(), json_payload.size()), zmq::send_flags::none);
        std::promise<bool> p; p.set_value(true); return p.get_future();
    } catch (...) {
        std::promise<bool> p; p.set_value(false); return p.get_future();
    }
}

std::future<std::string> RemoteL3KVClient::get_node_payload_async(lite3::NodeID owner_id, const std::string& target_node_id) {
    std::promise<std::string> p; p.set_value(""); return p.get_future();
}

std::future<std::unordered_map<std::string, std::string>> RemoteL3KVClient::get_nodes_batch_async(lite3::NodeID owner_id, const std::vector<std::string>& node_uuids) {
    std::promise<std::unordered_map<std::string, std::string>> p; p.set_value({}); return p.get_future();
}

std::future<bool> RemoteL3KVClient::put_node_async(lite3::NodeID owner_id, const std::string& target_node_id, const std::string& json_payload) {
    return put_edge_async(owner_id, target_node_id, json_payload);
}

std::future<bool> RemoteL3KVClient::put_batch_async(lite3::NodeID owner_id, const std::unordered_map<std::string, std::string>& batch) {
    std::promise<bool> p; p.set_value(false); return p.get_future();
}

} // namespace l3kvg
