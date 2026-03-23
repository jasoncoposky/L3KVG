#include "L3KVG/RemoteL3KVClient.hpp"
#include "httplib.h"
#include <stdexcept>
#include <iostream>

namespace l3kvg {

RemoteL3KVClient::RemoteL3KVClient() {}

void RemoteL3KVClient::add_peer(lite3::NodeID node_id, const std::string& endpoint_url) {
    peer_endpoints_[node_id] = endpoint_url;
}

std::future<std::vector<std::string>> RemoteL3KVClient::get_neighbors_async(
    lite3::NodeID owner_id,
    const std::string& target_node_id, 
    const std::string& label,
    double min_weight
) {
    auto it = peer_endpoints_.find(owner_id);
    if (it == peer_endpoints_.end()) {
        return std::async(std::launch::async, [owner_id]() -> std::vector<std::string> {
            throw std::runtime_error("Unknown peer ID: " + std::to_string(owner_id));
        });
    }

    std::string host_port = it->second;

    return std::async(std::launch::async, [host_port, target_node_id, label, min_weight]() {
        httplib::Client cli(host_port);
        
        std::string req_body = "{\"target\": \"" + target_node_id + 
                               "\", \"label\": \"" + label + 
                               "\", \"min_weight\": " + std::to_string(min_weight) + "}";

        if (auto res = cli.Post("/api/internal/neighbors", req_body, "application/json")) {
            if (res->status == 200) {
                std::vector<std::string> neighbors;
                std::string body = res->body;
                size_t start = body.find('[');
                size_t end = body.find(']');
                if (start != std::string::npos && end != std::string::npos) {
                    std::string arr = body.substr(start + 1, end - start - 1);
                    size_t pos = 0;
                    while (pos < arr.size()) {
                        size_t q1 = arr.find('"', pos);
                        if (q1 == std::string::npos) break;
                        size_t q2 = arr.find('"', q1 + 1);
                        if (q2 == std::string::npos) break;
                        neighbors.push_back(arr.substr(q1 + 1, q2 - q1 - 1));
                        pos = q2 + 1;
                    }
                }
                return neighbors;
            } else {
                throw std::runtime_error("Peer returned status " + std::to_string(res->status));
            }
        } else {
            throw std::runtime_error("Failed to connect to peer at " + host_port);
        }
    });
}

std::future<bool> RemoteL3KVClient::put_edge_async(
    lite3::NodeID owner_id,
    const std::string& edge_key,
    const std::string& json_payload
) {
    auto it = peer_endpoints_.find(owner_id);
    if (it == peer_endpoints_.end()) {
        return std::async(std::launch::async, [owner_id]() -> bool {
            throw std::runtime_error("Unknown peer ID: " + std::to_string(owner_id));
        });
    }

    std::string host_port = it->second;

    return std::async(std::launch::async, [host_port, edge_key, json_payload]() {
        httplib::Client cli(host_port);
        
        std::string req_body = "{\"key\": \"" + edge_key + "\", \"payload\": " + json_payload + "}";

        if (auto res = cli.Post("/api/internal/put_edge", req_body, "application/json")) {
            if (res->status == 200) {
                return true;
            } else {
                throw std::runtime_error("Peer returned status " + std::to_string(res->status));
            }
        } else {
            throw std::runtime_error("Failed to connect to peer at " + host_port);
        }
    });
}

std::future<std::string> RemoteL3KVClient::get_node_payload_async(
    lite3::NodeID owner_id,
    const std::string& target_node_id
) {
    auto it = peer_endpoints_.find(owner_id);
    if (it == peer_endpoints_.end()) {
        return std::async(std::launch::async, [owner_id]() -> std::string {
            throw std::runtime_error("Unknown peer ID for get_node: " + std::to_string(owner_id));
        });
    }
    std::string host_port = it->second;
    return std::async(std::launch::async, [host_port, target_node_id]() {
        httplib::Client cli(host_port);
        
        if (auto res = cli.Get("/api/internal/node/" + target_node_id)) {
            if (res->status == 200) {
                return res->body; // Return raw bytes directly
            } else {
                throw std::runtime_error("Peer returned status " + std::to_string(res->status) + " for get_node");
            }
        } else {
            throw std::runtime_error("Failed to connect to peer at " + host_port + " for get_node");
        }
    });
}

std::future<bool> RemoteL3KVClient::put_node_async(
    lite3::NodeID owner_id,
    const std::string& target_node_id,
    const std::string& json_payload
) {
    auto it = peer_endpoints_.find(owner_id);
    if (it == peer_endpoints_.end()) {
        return std::async(std::launch::async, [owner_id]() -> bool {
            throw std::runtime_error("Unknown peer ID for put_node: " + std::to_string(owner_id));
        });
    }
    std::string host_port = it->second;
    return std::async(std::launch::async, [host_port, target_node_id, json_payload]() {
        httplib::Client cli(host_port);
        std::string req_body = "{\"target\": \"" + target_node_id + "\", \"payload\": " + json_payload + "}";
        if (auto res = cli.Post("/api/internal/put_node", req_body, "application/json")) {
            if (res->status == 200) {
                return true;
            } else {
                throw std::runtime_error("Peer returned status " + std::to_string(res->status) + " for put_node");
            }
        } else {
            throw std::runtime_error("Failed to connect to peer at " + host_port + " for put_node");
        }
    });
}

} // namespace l3kvg
