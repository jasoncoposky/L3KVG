#include "L3KVG/Node.hpp"
#include "L3KVG/Cypher.hpp"
#include "L3KVG/Engine.hpp"
#include "L3KVG/KeyBuilder.hpp"
#include "L3KVG/RemoteL3KVClient.hpp"
#include "L3KVG/Settings.hpp"
#include "L3KVG/MutationBatch.hpp"
#include "L3KVG/Query.hpp"
#include "engine/store.hpp"
#include "buffer.hpp"
#include "httplib.h"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <condition_variable>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_set>
#include <vector>
#include <zmq.hpp>
#include <zmq_addon.hpp>

struct Config {
    std::string db_path;
    uint16_t node_id{1};
    uint32_t zmq_port{5556};
    uint32_t http_port{8080};
    uint32_t num_workers{4};
    std::string auth_secret;
    std::string local_cluster_name;
    uint16_t local_cluster_id{0};
    struct Federation {
        std::string name;
        uint16_t id{0};
        std::vector<std::string> endpoints;
    };
    std::vector<Federation> federations;
};

namespace {

struct TransparentStringHash {
    using is_transparent = void;
    size_t operator()(std::string_view sv) const noexcept {
        return std::hash<std::string_view>{}(sv);
    }
    size_t operator()(const std::string& s) const noexcept {
        return std::hash<std::string_view>{}(s);
    }
};

struct AuthRegistry {
    std::shared_mutex mu;
    std::unordered_set<std::string, TransparentStringHash, std::equal_to<>> authenticated_identities;
};

inline void send_reply(zmq::socket_t& sock, zmq::message_t& identity, const void* data, size_t size) {
    sock.send(identity, zmq::send_flags::sndmore);
    sock.send(zmq::message_t(), zmq::send_flags::sndmore);
    sock.send(zmq::message_t(data, size), zmq::send_flags::none);
}

inline bool has_read_acl(l3kvg::Engine* engine, uint32_t uid, std::string_view key) {
    if (uid == l3kvg::INTERNAL_UID || uid == l3kvg::ADMIN_UID) return true;
    auto perm = engine->get_store()->credentials().check_permission(uid, key);
    return (perm & l3kv::Permission::READ) || (perm & l3kv::Permission::ADMIN);
}

inline bool has_write_acl(l3kvg::Engine* engine, uint32_t uid, std::string_view key) {
    if (uid == l3kvg::INTERNAL_UID || uid == l3kvg::ADMIN_UID) return true;
    auto perm = engine->get_store()->credentials().check_permission(uid, key);
    return (perm & l3kv::Permission::WRITE) || (perm & l3kv::Permission::ADMIN);
}

} // namespace

Config load_config(const std::string &path) {
    using namespace lite3cpp::literals;
    std::ifstream f(path);
    if (!f.is_open()) {
        throw std::runtime_error("Could not open config file: " + path);
    }
    std::string str((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    lite3cpp::Buffer buf = lite3cpp::lite3_json::from_json_string(str);
    Config cfg;
    uint32_t hw = std::thread::hardware_concurrency();
    cfg.num_workers = std::clamp(hw, 4u, 32u);

    if (auto v = buf.try_get_str(0, "db_path"_hk)) cfg.db_path.assign(v->data(), v->size());
    if (auto v = buf.try_get_i64(0, "node_id"_hk)) cfg.node_id = static_cast<uint16_t>(*v);
    if (auto v = buf.try_get_i64(0, "zmq_port"_hk)) cfg.zmq_port = static_cast<uint32_t>(*v);
    if (auto v = buf.try_get_i64(0, "http_port"_hk)) cfg.http_port = static_cast<uint32_t>(*v);
    if (auto v = buf.try_get_i64(0, "num_workers"_hk)) cfg.num_workers = static_cast<uint32_t>(*v);
    if (auto v = buf.try_get_str(0, "auth_secret"_hk)) cfg.auth_secret.assign(v->data(), v->size());
    if (auto v = buf.try_get_str(0, "local_cluster_name"_hk)) cfg.local_cluster_name.assign(v->data(), v->size());
    if (auto v = buf.try_get_i64(0, "local_cluster_id"_hk)) cfg.local_cluster_id = static_cast<uint16_t>(*v);

    if (auto fed_arr_opt = buf.try_get_arr(0, "federations"_hk)) {
        const size_t fed_arr_ofs = *fed_arr_opt;
        for (auto it = buf.begin(fed_arr_ofs), end = buf.end(fed_arr_ofs); it != end; ++it) {
            if (it->value_type != lite3cpp::Type::Object) continue;
            const size_t fo = it->value_offset + 1;
            if (fo + sizeof(lite3cpp::PackedNodeLayout) > buf.size()) continue;
            Config::Federation fed;
            if (auto name = buf.try_get_str(fo, "name"_hk)) fed.name.assign(name->data(), name->size());
            if (auto id = buf.try_get_i64(fo, "id"_hk)) fed.id = static_cast<uint16_t>(*id);
            if (auto ep_arr_opt = buf.try_get_arr(fo, "endpoints"_hk)) {
                const size_t ep_arr_ofs = *ep_arr_opt;
                for (auto eit = buf.begin(ep_arr_ofs), eend = buf.end(ep_arr_ofs); eit != eend; ++eit) {
                    if (eit->value_type == lite3cpp::Type::String && eit->value_offset + 5 <= buf.size()) {
                        uint32_t sz = 0;
                        std::memcpy(&sz, buf.data() + eit->value_offset + 1, 4);
                        if (eit->value_offset + 5 + sz <= buf.size()) {
                            fed.endpoints.emplace_back(
                                reinterpret_cast<const char*>(buf.data() + eit->value_offset + 5), sz);
                        }
                    }
                }
            }
            cfg.federations.push_back(std::move(fed));
        }
    }
    return cfg;
}

static std::atomic<bool> s_running{true};
static std::atomic<httplib::Server*> s_http_svr{nullptr};
static std::condition_variable s_flusher_cv;
static std::mutex s_flusher_mu;

static void signal_handler(int) {
    s_running.store(false, std::memory_order_relaxed);
    auto* svr = s_http_svr.load(std::memory_order_relaxed);
    if (svr) {
        svr->stop();
    }
    s_flusher_cv.notify_all();
}

static void process_request(
    l3kvg::Engine* engine,
    const Config& cfg,
    AuthRegistry& auth_registry,
    zmq::socket_t& sock,
    std::vector<zmq::message_t>& recv_msgs) {
    using namespace lite3cpp::literals;

    if (recv_msgs.size() < 3) {
        return;
    }

    auto identity = std::move(recv_msgs[0]);
    size_t data_idx = 1;
    if (data_idx < recv_msgs.size() && recv_msgs[data_idx].size() == 0) {
        data_idx++; // Skip delimiter
    }

    uint32_t principal_id = 0;
    if (data_idx + 1 < recv_msgs.size() && recv_msgs[data_idx].size() == 4) {
        std::memcpy(&principal_id, recv_msgs[data_idx].data(), 4);
        data_idx++;
    }

    if (data_idx >= recv_msgs.size()) {
        send_reply(sock, identity, "ERR_UNKNOWN_OP", 14);
        return;
    }

    std::string_view opcode(static_cast<const char*>(recv_msgs[data_idx].data()), recv_msgs[data_idx].size());
    data_idx++;

    static const bool s_server_debug = (std::getenv("L3_SERVER_DEBUG") != nullptr);
    if (s_server_debug) {
        std::fprintf(stderr, "L3_SERVER: Handling opcode [%.*s] from pid [%u]\n",
                     static_cast<int>(opcode.size()), opcode.data(), principal_id);
        std::fflush(stderr);
    }

    if (opcode == "A") {
        if (data_idx + 2 > recv_msgs.size()) {
            send_reply(sock, identity, "ERR", 3);
            return;
        }
        data_idx++; // node_id frame
        std::string_view secret(static_cast<const char*>(recv_msgs[data_idx].data()), recv_msgs[data_idx].size());
        data_idx++;

        if (cfg.auth_secret.empty() || secret == cfg.auth_secret) {
            if (!cfg.auth_secret.empty()) {
                std::unique_lock<std::shared_mutex> lk(auth_registry.mu);
                auth_registry.authenticated_identities.emplace(
                    static_cast<const char*>(identity.data()), identity.size());
            }
            send_reply(sock, identity, "OK", 2);
        } else {
            send_reply(sock, identity, "ERR_AUTH", 8);
        }
        return;
    }

    if (!cfg.auth_secret.empty()) {
        std::string_view id_sv(static_cast<const char*>(identity.data()), identity.size());
        bool authed = false;
        {
            std::shared_lock<std::shared_mutex> lk(auth_registry.mu);
            authed = auth_registry.authenticated_identities.contains(id_sv);
        }
        if (!authed) {
            send_reply(sock, identity, "ERR_UNAUTH", 10);
            return;
        }
    }

    if (opcode == "H") {
        send_reply(sock, identity, "PONG", 4);
        return;
    }

    if (opcode == "S") {
        if (data_idx + 2 > recv_msgs.size()) {
            send_reply(sock, identity, "ERR", 3);
            return;
        }
        try {
            std::string key = recv_msgs[data_idx].to_string(); data_idx++;
            std::string val = recv_msgs[data_idx].to_string(); data_idx++;
            uint16_t origin_cluster = 0;
            if (data_idx < recv_msgs.size()) {
                std::string_view orig_sv(static_cast<const char*>(recv_msgs[data_idx].data()), recv_msgs[data_idx].size());
                std::from_chars(orig_sv.data(), orig_sv.data() + orig_sv.size(), origin_cluster);
            }
            engine->replicate_key(key, std::move(val), origin_cluster);
            send_reply(sock, identity, "OK", 2);
        } catch (...) {
            send_reply(sock, identity, "ERR", 3);
        }
        return;
    }

    if (opcode == "+") {
        if (data_idx + 1 > recv_msgs.size()) {
            send_reply(sock, identity, "ERR", 3);
            return;
        }
        std::string key = recv_msgs[data_idx].to_string(); data_idx++;
        int64_t delta = 1;
        if (data_idx < recv_msgs.size()) {
            std::string_view dsv(static_cast<const char*>(recv_msgs[data_idx].data()), recv_msgs[data_idx].size());
            std::from_chars(dsv.data(), dsv.data() + dsv.size(), delta);
            data_idx++;
        }

        try {
            static std::mutex inc_locks[64];
            size_t lidx = std::hash<std::string_view>{}(key) % 64;
            std::lock_guard<std::mutex> lock(inc_locks[lidx]);

            auto blob = engine->get_store()->get_blob(key, principal_id);
            int64_t val = 0;
            if (blob && blob->view().size() >= sizeof(lite3cpp::PackedNodeLayout)) {
                auto v = blob->view();
                auto buf = lite3cpp::Buffer::from_view(v.data(), v.size());
                val = buf.try_get_i64(0, "v"_hk).value_or(0);
            }
            val += delta;

            lite3cpp::Buffer nbuf(128);
            nbuf.init_object();
            nbuf.set_i64(0, "v"_hk, val);
            engine->get_store()->put(key, nbuf.move_to_string(), principal_id);

            char out_buf[32];
            auto [ptr, ec] = std::to_chars(out_buf, out_buf + sizeof(out_buf), val);
            send_reply(sock, identity, out_buf, (ec == std::errc{}) ? static_cast<size_t>(ptr - out_buf) : 0);
        } catch (...) {
            send_reply(sock, identity, "ERR", 3);
        }
        return;
    }

    if (opcode == "R") {
        if (data_idx + 2 > recv_msgs.size()) {
            lite3cpp::Buffer empty_buf;
            empty_buf.init_array();
            send_reply(sock, identity, empty_buf.data(), empty_buf.size());
            return;
        }
        try {
            const auto& nodes_msg = recv_msgs[data_idx];
            data_idx++;
            std::vector<uint64_t> nodes;
            bool parsed_as_json = false;
            std::string_view sv(static_cast<const char*>(nodes_msg.data()), nodes_msg.size());
            if (nodes_msg.size() % sizeof(uint64_t) != 0 || (!sv.empty() && sv.front() == '[')) {
                size_t first = sv.find_first_not_of(" \t\r\n");
                size_t last = sv.find_last_not_of(" \t\r\n");
                if (first != std::string_view::npos && last != std::string_view::npos &&
                    last >= first + 1 && sv[first] == '[' && sv[last] == ']') {
                    try {
                        lite3cpp::Buffer buf = lite3cpp::lite3_json::from_json_string(sv.substr(first, last - first + 1));
                        if (buf.size() >= sizeof(lite3cpp::PackedNodeLayout)) {
                            lite3cpp::NodeView nv(reinterpret_cast<const lite3cpp::PackedNodeLayout*>(buf.data()));
                            if (nv.type() == lite3cpp::Type::Array) {
                                parsed_as_json = true;
                                nodes.reserve(nv.size());
                                for (uint32_t i = 0; i < nv.size(); ++i) {
                                    if (auto iv = buf.arr_try_get_i64(0, i)) {
                                        nodes.push_back(static_cast<uint64_t>(*iv));
                                    } else if (auto s = buf.arr_try_get_str(0, i)) {
                                        uint64_t uid = 0;
                                        auto [p, ec] = std::from_chars(s->data(), s->data() + s->size(), uid, 16);
                                        if (ec == std::errc{}) nodes.push_back(uid);
                                    }
                                }
                            }
                        }
                    } catch (...) {
                        parsed_as_json = false;
                    }
                }
            }
            if (!parsed_as_json && nodes_msg.size() % sizeof(uint64_t) == 0) {
                size_t count = nodes_msg.size() / sizeof(uint64_t);
                nodes.resize(count);
                if (count > 0) {
                    std::memcpy(nodes.data(), nodes_msg.data(), count * sizeof(uint64_t));
                }
            }

            const auto& qmsg = recv_msgs[data_idx];
            lite3cpp::Buffer owned_json_qbuf;
            lite3cpp::Buffer qbuf;
            const uint8_t* qptr = static_cast<const uint8_t*>(qmsg.data());
            if (qmsg.size() >= sizeof(lite3cpp::PackedNodeLayout) && (qptr[0] == 0x06 || qptr[0] == 0x07)) {
                qbuf = lite3cpp::Buffer::from_view(qptr, qmsg.size());
            } else {
                std::string_view qsv(reinterpret_cast<const char*>(qptr), qmsg.size());
                owned_json_qbuf = lite3cpp::lite3_json::from_json_string(qsv.empty() ? "{}" : qsv);
                qbuf = lite3cpp::Buffer::from_view(owned_json_qbuf.data(), owned_json_qbuf.size());
            }

            uint32_t eff_principal = (principal_id != 0) ? principal_id : l3kvg::INTERNAL_UID;
            lite3cpp::Buffer resp_buf = engine->query()
                .set_principal_id(eff_principal)
                .set_populate_fields(false)
                .resume(nodes, qbuf)
                .execute_to_buffer();

            send_reply(sock, identity, resp_buf.data(), resp_buf.size());
        } catch (...) {
            send_reply(sock, identity, "ERR", 3);
        }
        return;
    }

    if (opcode == "P") {
        if (data_idx + 2 > recv_msgs.size()) {
            send_reply(sock, identity, "ERR", 3);
            return;
        }
        try {
            std::string key = recv_msgs[data_idx].to_string(); data_idx++;
            std::string payload = recv_msgs[data_idx].to_string();

            if (!has_write_acl(engine, principal_id, key)) {
                send_reply(sock, identity, "ERR_FORBIDDEN", 13);
                return;
            }

            if (key.starts_with("e:out:{")) {
                uint64_t src = 0, dst = 0; double weight = 0; char label_buf[256] = {0};
                int parsed = std::sscanf(key.c_str(), "e:out:{%llx\x7d:%255[^:]:%lf:{%llx\x7d",
                                         reinterpret_cast<unsigned long long*>(&src),
                                         label_buf, &weight,
                                         reinterpret_cast<unsigned long long*>(&dst));
                if (parsed == 4) {
                    engine->add_edge(src, label_buf, weight, dst, std::move(payload));
                } else {
                    engine->mark_legacy_kv_edges();
                    engine->get_store()->put(key, std::move(payload), principal_id);
                }
            } else if (key.starts_with("n:{")) {
                uint64_t id = 0;
                int parsed = std::sscanf(key.c_str(), "n:{%llx\x7d", reinterpret_cast<unsigned long long*>(&id));
                if (parsed == 1) {
                    engine->put_node(id, std::move(payload));
                } else {
                    engine->get_store()->put(key, std::move(payload), principal_id);
                }
            } else {
                if (key.starts_with("e:in:")) {
                    engine->mark_legacy_kv_edges();
                }
                engine->get_store()->put(key, std::move(payload), principal_id);
            }

            send_reply(sock, identity, "OK", 2);
        } catch (...) {
            send_reply(sock, identity, "ERR", 3);
        }
        return;
    }

    if (opcode == "M") {
        try {
            lite3cpp::Buffer res_buf;
            res_buf.init_object();
            while (data_idx < recv_msgs.size()) {
                std::string_view key_sv(static_cast<const char*>(recv_msgs[data_idx].data()), recv_msgs[data_idx].size());
                data_idx++;
                auto blob = engine->get_store()->get_blob(key_sv, principal_id);
                if (blob && blob->view().size() > 0) {
                    auto v = blob->view();
                    auto slot = res_buf.set_bytes_uninitialized(0, lite3cpp::HashedKey(key_sv), v.size());
                    if (!slot.empty()) {
                        std::memcpy(slot.data(), v.data(), v.size());
                    }
                }
            }
            send_reply(sock, identity, res_buf.data(), res_buf.size());
        } catch (...) {
            send_reply(sock, identity, "", 0);
        }
        return;
    }

    if (opcode == "G") {
        if (data_idx + 1 > recv_msgs.size()) {
            send_reply(sock, identity, "", 0);
            return;
        }
        try {
            const auto& msg = recv_msgs[data_idx];
            const uint8_t* ptr = static_cast<const uint8_t*>(msg.data());

            // Check for Graph Multi-Read Lite3 request buffer ({"ops": [...]})
            if (msg.size() >= sizeof(lite3cpp::PackedNodeLayout) &&
                ptr[0] == static_cast<uint8_t>(lite3cpp::Type::Object)) {
                auto req_buf = lite3cpp::Buffer::from_view(ptr, msg.size());
                if (auto ops_ofs_opt = req_buf.try_get_arr(0, "ops"_hk)) {
                    const size_t ops_ofs = *ops_ofs_opt;
                    lite3cpp::NodeView ops_nv(reinterpret_cast<const lite3cpp::PackedNodeLayout*>(req_buf.data() + ops_ofs));
                    lite3cpp::Buffer resp_buf;
                    resp_buf.ensure_capacity(ops_nv.size() * (lite3cpp::config::node_size + 128) + 256);
                    resp_buf.init_array();

                    const uint32_t eff_uid = (principal_id != 0) ? principal_id : l3kvg::INTERNAL_UID;
                    for (auto it = req_buf.begin(ops_ofs), end = req_buf.end(ops_ofs); it != end; ++it) {
                        size_t r_ofs = resp_buf.arr_append_obj(0);
                        if (it->value_type != lite3cpp::Type::Object) {
                            resp_buf.set_bool(r_ofs, "f"_hk, false);
                            continue;
                        }
                        const size_t item_ofs = it->value_offset + 1;
                        if (item_ofs + sizeof(lite3cpp::PackedNodeLayout) > req_buf.size()) {
                            resp_buf.set_bool(r_ofs, "f"_hk, false);
                            continue;
                        }

                        const auto kind = static_cast<l3kvg::GraphReadKind>(
                            req_buf.try_get_i64(item_ofs, "k"_hk).value_or(0));
                        const uint64_t nid = static_cast<uint64_t>(
                            req_buf.try_get_i64(item_ofs, "id"_hk).value_or(0));
                        const std::string_view str_arg = req_buf.try_get_str(item_ofs, "s"_hk).value_or("");

                        switch (kind) {
                            case l3kvg::GraphReadKind::NodePayload: {
                                std::string_view nkey = l3kvg::KeyBuilder::node_key(nid);
                                auto blob = engine->get_store()->get_blob(nkey, eff_uid);
                                if (blob && blob->view().size() > 0) {
                                    resp_buf.set_bool(r_ofs, "f"_hk, true);
                                    auto v = blob->view();
                                    auto slot = resp_buf.set_bytes_uninitialized(r_ofs, "p"_hk, v.size());
                                    if (!slot.empty()) std::memcpy(slot.data(), v.data(), v.size());
                                } else {
                                    resp_buf.set_bool(r_ofs, "f"_hk, false);
                                }
                                break;
                            }
                            case l3kvg::GraphReadKind::RawKey: {
                                auto blob = engine->get_store()->get_blob(str_arg, eff_uid);
                                if (blob && blob->view().size() > 0) {
                                    resp_buf.set_bool(r_ofs, "f"_hk, true);
                                    auto v = blob->view();
                                    auto slot = resp_buf.set_bytes_uninitialized(r_ofs, "p"_hk, v.size());
                                    if (!slot.empty()) std::memcpy(slot.data(), v.data(), v.size());
                                } else {
                                    resp_buf.set_bool(r_ofs, "f"_hk, false);
                                }
                                break;
                            }
                            case l3kvg::GraphReadKind::OutNeighbors: {
                                if (!has_read_acl(engine, eff_uid, l3kvg::KeyBuilder::node_key(nid))) {
                                    resp_buf.set_bool(r_ofs, "f"_hk, false);
                                    break;
                                }
                                double min_w = req_buf.try_get_f64(item_ofs, "w"_hk).value_or(-999999.0);
                                auto node = engine->get_node(nid);
                                std::vector<uint64_t> neighs = node
                                    ? node->get_neighbors(str_arg, min_w, eff_uid)
                                    : std::vector<uint64_t>{};
                                resp_buf.set_bool(r_ofs, "f"_hk, true);
                                if (!neighs.empty()) {
                                    auto slot = resp_buf.set_bytes_uninitialized(
                                        r_ofs, "n"_hk, neighs.size() * sizeof(uint64_t));
                                    if (!slot.empty()) std::memcpy(slot.data(), neighs.data(), slot.size());
                                }
                                break;
                            }
                            case l3kvg::GraphReadKind::InNeighbors: {
                                if (!has_read_acl(engine, eff_uid, l3kvg::KeyBuilder::node_key(nid))) {
                                    resp_buf.set_bool(r_ofs, "f"_hk, false);
                                    break;
                                }
                                auto node = engine->get_node(nid);
                                std::vector<uint64_t> neighs = node
                                    ? node->get_in_neighbors(str_arg, eff_uid)
                                    : std::vector<uint64_t>{};
                                resp_buf.set_bool(r_ofs, "f"_hk, true);
                                if (!neighs.empty()) {
                                    auto slot = resp_buf.set_bytes_uninitialized(
                                        r_ofs, "n"_hk, neighs.size() * sizeof(uint64_t));
                                    if (!slot.empty()) std::memcpy(slot.data(), neighs.data(), slot.size());
                                }
                                break;
                            }
                            default:
                                resp_buf.set_bool(r_ofs, "f"_hk, false);
                                break;
                        }
                    }
                    send_reply(sock, identity, resp_buf.data(), resp_buf.size());
                    return;
                }
            }

            // Single-key raw Get (zero-copy via get_blob)
            std::string_view key_sv(reinterpret_cast<const char*>(ptr), msg.size());
            auto blob = engine->get_store()->get_blob(key_sv, principal_id);
            if (blob && blob->view().size() > 0) {
                auto v = blob->view();
                send_reply(sock, identity, v.data(), v.size());
            } else {
                send_reply(sock, identity, "", 0);
            }
        } catch (...) {
            send_reply(sock, identity, "", 0);
        }
        return;
    }

    if (opcode == "D") {
        if (data_idx + 1 > recv_msgs.size()) {
            send_reply(sock, identity, "ERR", 3);
            return;
        }
        try {
            std::string key = recv_msgs[data_idx].to_string();
            if (!has_write_acl(engine, principal_id, key)) {
                send_reply(sock, identity, "ERR_FORBIDDEN", 13);
                return;
            }
            if (key.starts_with("n:{")) {
                size_t end_pos = key.find('}', 3);
                if (end_pos != std::string::npos && end_pos > 3) {
                    uint64_t nid = 0;
                    std::string_view hex_sv(key.data() + 3, end_pos - 3);
                    auto [p, ec] = std::from_chars(hex_sv.data(), hex_sv.data() + hex_sv.size(), nid, 16);
                    if (ec == std::errc{} && p == hex_sv.data() + hex_sv.size()) {
                        engine->del_node(nid);
                    } else {
                        engine->get_store()->del(key, principal_id);
                    }
                } else {
                    engine->get_store()->del(key, principal_id);
                }
            } else if (key.starts_with("e:out:{")) {
                uint64_t src = 0, dst = 0; double weight = 0; char label_buf[256] = {0};
                int parsed = std::sscanf(key.c_str(), "e:out:{%llx\x7d:%255[^:]:%lf:{%llx\x7d",
                                         reinterpret_cast<unsigned long long*>(&src),
                                         label_buf, &weight,
                                         reinterpret_cast<unsigned long long*>(&dst));
                if (parsed == 4) {
                    engine->del_edge(src, label_buf, weight, dst);
                } else {
                    engine->get_store()->del(key, principal_id);
                }
            } else {
                engine->get_store()->del(key, principal_id);
            }
            send_reply(sock, identity, "OK", 2);
        } catch (...) {
            send_reply(sock, identity, "ERR", 3);
        }
        return;
    }

    if (opcode == "B") {
        if (data_idx + 1 > recv_msgs.size()) {
            send_reply(sock, identity, "ERR", 3);
            return;
        }
        try {
            const auto& msg = recv_msgs[data_idx];
            auto batch_buf = lite3cpp::Buffer::from_view(static_cast<const uint8_t*>(msg.data()), msg.size());
            bool ok = engine->apply_batch(batch_buf, principal_id);
            if (ok) {
                send_reply(sock, identity, "OK", 2);
            } else {
                send_reply(sock, identity, "ERR", 3);
            }
        } catch (...) {
            send_reply(sock, identity, "ERR", 3);
        }
        return;
    }

    if (opcode == "E") {
        if (data_idx + 4 > recv_msgs.size()) {
            send_reply(sock, identity, "ERR", 3);
            return;
        }
        try {
            std::string_view src_sv(static_cast<const char*>(recv_msgs[data_idx].data()), recv_msgs[data_idx].size()); data_idx++;
            std::string_view label_sv(static_cast<const char*>(recv_msgs[data_idx].data()), recv_msgs[data_idx].size()); data_idx++;
            std::string_view w_sv(static_cast<const char*>(recv_msgs[data_idx].data()), recv_msgs[data_idx].size()); data_idx++;
            std::string_view dst_sv(static_cast<const char*>(recv_msgs[data_idx].data()), recv_msgs[data_idx].size());

            uint64_t src = 0, dst = 0;
            double weight = 0.0;
            auto [ps, ecs] = std::from_chars(src_sv.data(), src_sv.data() + src_sv.size(), src, 16);
            auto [pw, ecw] = std::from_chars(w_sv.data(), w_sv.data() + w_sv.size(), weight);
            auto [pd, ecd] = std::from_chars(dst_sv.data(), dst_sv.data() + dst_sv.size(), dst, 16);
            if (ecs != std::errc{} || ecw != std::errc{} || ecd != std::errc{}) {
                send_reply(sock, identity, "ERR", 3);
                return;
            }
            if (!has_write_acl(engine, principal_id, l3kvg::KeyBuilder::edge_out_key(src, label_sv, weight, dst)) ||
                !has_write_acl(engine, principal_id, l3kvg::KeyBuilder::edge_in_key(dst, label_sv, src))) {
                send_reply(sock, identity, "ERR_FORBIDDEN", 13);
                return;
            }
            engine->add_edge(src, label_sv, weight, dst);
            send_reply(sock, identity, "OK", 2);
        } catch (...) {
            send_reply(sock, identity, "ERR", 3);
        }
        return;
    }

    if (opcode == "N") {
        if (data_idx + 3 > recv_msgs.size()) {
            send_reply(sock, identity, "ERR", 3);
            return;
        }
        try {
            std::string_view id_sv(static_cast<const char*>(recv_msgs[data_idx].data()), recv_msgs[data_idx].size()); data_idx++;
            std::string_view label_sv(static_cast<const char*>(recv_msgs[data_idx].data()), recv_msgs[data_idx].size()); data_idx++;
            std::string_view w_sv(static_cast<const char*>(recv_msgs[data_idx].data()), recv_msgs[data_idx].size());

            uint64_t target_node_id = 0;
            double min_weight = 0.0;
            auto [pi, eci] = std::from_chars(id_sv.data(), id_sv.data() + id_sv.size(), target_node_id, 16);
            auto [pw, ecw] = std::from_chars(w_sv.data(), w_sv.data() + w_sv.size(), min_weight);
            if (eci != std::errc{} || ecw != std::errc{}) {
                send_reply(sock, identity, "ERR", 3);
                return;
            }
            if (!has_read_acl(engine, principal_id, l3kvg::KeyBuilder::node_key(target_node_id))) {
                send_reply(sock, identity, "ERR_FORBIDDEN", 13);
                return;
            }

            auto node = engine->get_node(target_node_id);
            std::vector<uint64_t> neighs;
            if (node) neighs = node->get_neighbors(label_sv, min_weight, principal_id);
            send_reply(sock, identity, neighs.data(), neighs.size() * sizeof(uint64_t));
        } catch (...) {
            send_reply(sock, identity, "ERR", 3);
        }
        return;
    }

    if (opcode == "I") {
        if (data_idx + 2 > recv_msgs.size()) {
            send_reply(sock, identity, "ERR", 3);
            return;
        }
        try {
            std::string_view id_sv(static_cast<const char*>(recv_msgs[data_idx].data()), recv_msgs[data_idx].size()); data_idx++;
            std::string_view label_sv(static_cast<const char*>(recv_msgs[data_idx].data()), recv_msgs[data_idx].size());

            uint64_t target_node_id = 0;
            auto [pi, eci] = std::from_chars(id_sv.data(), id_sv.data() + id_sv.size(), target_node_id, 16);
            if (eci != std::errc{}) {
                send_reply(sock, identity, "ERR", 3);
                return;
            }
            if (!has_read_acl(engine, principal_id, l3kvg::KeyBuilder::node_key(target_node_id))) {
                send_reply(sock, identity, "ERR_FORBIDDEN", 13);
                return;
            }

            auto node = engine->get_node(target_node_id);
            std::vector<uint64_t> neighs;
            if (node) neighs = node->get_in_neighbors(label_sv, principal_id);
            send_reply(sock, identity, neighs.data(), neighs.size() * sizeof(uint64_t));
        } catch (...) {
            send_reply(sock, identity, "ERR", 3);
        }
        return;
    }

    if (opcode == "K") {
        if (data_idx + 1 > recv_msgs.size()) {
            send_reply(sock, identity, "ERR", 3);
            return;
        }
        try {
            std::string prefix = recv_msgs[data_idx].to_string(); data_idx++;
            auto perm = engine->get_store()->credentials().check_permission(principal_id, prefix);
            if (!(perm & l3kv::Permission::READ) && !(perm & l3kv::Permission::ADMIN)) {
                send_reply(sock, identity, "ERR_AUTH", 8);
                return;
            }
            size_t limit = engine->get_settings().prefix_scan_limit;
            auto entries = engine->get_store()->get_prefix_entries_all_shards(prefix, "", limit);
            lite3cpp::Buffer kbuf;
            kbuf.ensure_capacity(entries.size() * (lite3cpp::config::node_size + 64) + 256);
            kbuf.init_array();
            for (const auto& [k, v] : entries) {
                if (k.ends_with(":meta")) continue;
                size_t e = kbuf.arr_append_obj(0);
                kbuf.set_str(e, "k"_hk, k);
                kbuf.set_str(e, "v"_hk, v);
            }
            send_reply(sock, identity, kbuf.data(), kbuf.size());
        } catch (...) {
            send_reply(sock, identity, "ERR", 3);
        }
        return;
    }

    send_reply(sock, identity, "ERR_UNKNOWN_OP", 14);
}

static void worker_routine(
    zmq::context_t* ctx,
    l3kvg::Engine* engine,
    const Config& cfg,
    AuthRegistry& auth_registry,
    std::atomic<bool>& running) {
    try {
        zmq::socket_t worker_sock(*ctx, zmq::socket_type::dealer);
        worker_sock.set(zmq::sockopt::rcvtimeo, 250);
        worker_sock.set(zmq::sockopt::rcvhwm, 0);
        worker_sock.set(zmq::sockopt::sndhwm, 0);
        worker_sock.set(zmq::sockopt::linger, 0);
        worker_sock.connect("inproc://workers");

        std::vector<zmq::message_t> recv_msgs;
        recv_msgs.reserve(8);
        while (running.load(std::memory_order_relaxed)) {
            recv_msgs.clear();
            try {
                auto res = zmq::recv_multipart(worker_sock, std::back_inserter(recv_msgs));
                if (!res || recv_msgs.empty()) {
                    continue;
                }
            } catch (const zmq::error_t& e) {
                if (e.num() == ETERM) break;
                continue;
            } catch (...) {
                break;
            }
            process_request(engine, cfg, auth_registry, worker_sock, recv_msgs);
        }
    } catch (...) {}
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        std::fprintf(stderr, "Usage: %s <config.json>\n", argv[0]);
        return 1;
    }

    Config cfg;
    try {
        cfg = load_config(argv[1]);
    } catch (const std::exception &e) {
        std::fprintf(stderr, "Failed to load config: %s\n", e.what());
        return 1;
    }

    l3kvg::Settings settings;
    settings.node_id = cfg.node_id;

    const size_t pool_size = std::max<size_t>(4, static_cast<size_t>(cfg.num_workers));
    auto engine = std::make_unique<l3kvg::Engine>(cfg.db_path, cfg.node_id, nullptr, pool_size, settings);
    if (!cfg.auth_secret.empty()) {
        engine->set_auth_secret(cfg.auth_secret);
    }

    if (!cfg.local_cluster_name.empty()) {
        engine->get_resolver().register_local_cluster(cfg.local_cluster_name, cfg.local_cluster_id);
    }

    for (const auto &f : cfg.federations) {
        engine->get_resolver().register_federation(f.name, f.id, f.endpoints);
        for (const auto &ep : f.endpoints) {
            engine->get_remote_client().add_peer(f.id, ep);
        }
    }

    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);
#ifndef _WIN32
    std::signal(SIGPIPE, SIG_IGN);
#endif

    AuthRegistry auth_registry;
    const int zmq_io_threads = static_cast<int>(std::clamp(cfg.num_workers / 2, 2u, 4u));
    zmq::context_t ctx(zmq_io_threads);

    zmq::socket_t frontend(ctx, zmq::socket_type::router);
    frontend.set(zmq::sockopt::linger, 0);
    frontend.set(zmq::sockopt::rcvhwm, 0);
    frontend.set(zmq::sockopt::sndhwm, 0);
    std::string zmq_endpoint = "tcp://0.0.0.0:" + std::to_string(cfg.zmq_port);
    frontend.bind(zmq_endpoint);

    zmq::socket_t backend(ctx, zmq::socket_type::dealer);
    backend.set(zmq::sockopt::linger, 0);
    backend.set(zmq::sockopt::rcvhwm, 0);
    backend.set(zmq::sockopt::sndhwm, 0);
    backend.bind("inproc://workers");

    std::vector<std::thread> worker_threads;
    worker_threads.reserve(cfg.num_workers);
    for (uint32_t i = 0; i < cfg.num_workers; ++i) {
        worker_threads.emplace_back(worker_routine, &ctx, engine.get(), std::cref(cfg), std::ref(auth_registry), std::ref(s_running));
    }

    std::thread routing_thread([&frontend, &backend, &engine, &cfg, &auth_registry]() {
        try {
            std::vector<zmq::message_t> msgs;
            msgs.reserve(8);
            zmq::pollitem_t items[] = {
                {frontend.handle(), 0, ZMQ_POLLIN, 0},
                {backend.handle(), 0, ZMQ_POLLIN, 0}
            };
            while (s_running.load(std::memory_order_relaxed)) {
                int rc = zmq::poll(items, 2, std::chrono::milliseconds(250));
                if (rc <= 0) continue;

                if (items[0].revents & ZMQ_POLLIN) {
                    while (true) {
                        msgs.clear();
                        auto res = zmq::recv_multipart(frontend, std::back_inserter(msgs), zmq::recv_flags::dontwait);
                        if (!res || msgs.empty()) break;

                        // Peek opcode: [identity][empty?][pid(4B)?][opcode]
                        size_t idx = 1;
                        if (idx < msgs.size() && msgs[idx].size() == 0) ++idx;
                        if (idx + 1 < msgs.size() && msgs[idx].size() == 4) ++idx;
                        bool offload = false;
                        if (idx < msgs.size()) {
                            std::string_view op(static_cast<const char*>(msgs[idx].data()), msgs[idx].size());
                            offload = (op == "R" || op == "K");
                        }
                        if (offload) {
                            zmq::send_multipart(backend, msgs);
                        } else {
                            process_request(engine.get(), cfg, auth_registry, frontend, msgs);
                        }
                    }
                }

                if (items[1].revents & ZMQ_POLLIN) {
                    while (true) {
                        msgs.clear();
                        auto res = zmq::recv_multipart(backend, std::back_inserter(msgs), zmq::recv_flags::dontwait);
                        if (!res || msgs.empty()) break;
                        zmq::send_multipart(frontend, msgs);
                    }
                }
            }
        } catch (...) {}
    });

    std::thread flusher_thread([&engine]() {
        while (s_running.load(std::memory_order_relaxed)) {
            std::unique_lock<std::mutex> lock(s_flusher_mu);
            s_flusher_cv.wait_for(lock, std::chrono::seconds(1), [] {
                return !s_running.load(std::memory_order_relaxed);
            });
            if (!s_running.load(std::memory_order_relaxed)) break;
            try {
                engine->flush();
            } catch (...) {}
        }
    });

    httplib::Server svr;
    s_http_svr.store(&svr, std::memory_order_relaxed);
    svr.Get("/api/health", [](const httplib::Request&, httplib::Response& res) {
        res.set_content("OK", "text/plain");
    });

    bool listen_ok = svr.listen("0.0.0.0", cfg.http_port);
    int exit_code = 0;
    if (!listen_ok && s_running.load(std::memory_order_relaxed)) {
        std::fprintf(stderr, "Error: HTTP server failed to bind/listen on port %u\n", cfg.http_port);
        exit_code = 1;
    }

    s_running.store(false, std::memory_order_relaxed);
    s_flusher_cv.notify_all();
    try {
        ctx.shutdown();
    } catch (...) {}

    if (routing_thread.joinable()) routing_thread.join();
    for (auto& w : worker_threads) {
        if (w.joinable()) w.join();
    }
    if (flusher_thread.joinable()) flusher_thread.join();

    try {
        frontend.close();
        backend.close();
    } catch (...) {}

    try {
        engine->flush();
    } catch (...) {}

    s_http_svr.store(nullptr, std::memory_order_relaxed);

    return exit_code;
}
