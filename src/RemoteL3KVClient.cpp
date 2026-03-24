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

std::future<std::unordered_map<std::string, std::string>> RemoteL3KVClient::get_nodes_batch_async(
    lite3::NodeID owner_id,
    const std::vector<std::string>& node_uuids
) {
    auto it = peer_endpoints_.find(owner_id);
    if (it == peer_endpoints_.end()) {
        return std::async(std::launch::async, [owner_id]() -> std::unordered_map<std::string, std::string> {
            throw std::runtime_error("Unknown peer ID for batch_get: " + std::to_string(owner_id));
        });
    }
    std::string host_port = it->second;

    return std::async(std::launch::async, [host_port, node_uuids]() {
        httplib::Client cli(host_port);
        
        // Build batch request body: {"uuids": ["id1", "id2", ...]}
        std::string req_body = "{\"uuids\": [";
        for (size_t i = 0; i < node_uuids.size(); ++i) {
            req_body += "\"" + node_uuids[i] + "\"";
            if (i < node_uuids.size() - 1) req_body += ", ";
        }
        req_body += "]}";

        std::unordered_map<std::string, std::string> results;
        if (auto res = cli.Post("/api/internal/nodes/batch", req_body, "application/json")) {
            if (res->status == 200) {
                // Primitive JSON parsing for the map: {"id1": "payload1", "id2": "payload2"}
                std::string body = res->body;
                // Simple parser assuming well-formatted JSON from internal peer
                size_t pos = body.find('{');
                if (pos != std::string::npos) {
                    pos++;
                    while (pos < body.size()) {
                        size_t q1 = body.find('"', pos);
                        if (q1 == std::string::npos) break;
                        size_t q2 = body.find('"', q1 + 1);
                        if (q2 == std::string::npos) break;
                        std::string key = body.substr(q1 + 1, q2 - q1 - 1);
                        
                        size_t colon = body.find(':', q2 + 1);
                        if (colon == std::string::npos) break;
                        
                        size_t val_start = body.find('"', colon + 1);
                        if (val_start == std::string::npos) break;
                        size_t val_end = body.find('"', val_start + 1);
                        if (val_end == std::string::npos) break;
                        
                        results[key] = body.substr(val_start + 1, val_end - val_start - 1);
                        pos = val_end + 1;
                    }
                }
                return results;
            } else {
                throw std::runtime_error("Peer returned status " + std::to_string(res->status) + " for batch_get");
            }
        } else {
            throw std::runtime_error("Failed to connect to peer at " + host_port + " for batch_get");
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
