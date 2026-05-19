#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#ifdef _WIN32
#include <winsock2.h>
#endif
#ifdef _WIN32
#include <ws2tcpip.h>
#endif
#ifdef _WIN32
#include <BaseTsd.h>
#endif

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>
#include <zmq.hpp>
#include <zmq_addon.hpp>
#include "httplib.h"

#include "L3KVG/Cypher.hpp"
#include "L3KVG/Engine.hpp"
#include "L3KVG/Node.hpp"
#include "L3KVG/Query.hpp"
#include "engine/store.hpp"
#include "buffer.hpp"
#include "observability.hpp"

using json = nlohmann::json;

class FileLogger : public l3kv::ILogger {
public:
    FileLogger(const std::string& path) : out_(path, std::ios::app) {}
    void log(const std::string& msg) override {
        std::lock_guard<std::mutex> lock(mutex_);
        out_ << msg << std::endl;
        out_.flush();
    }
private:
    std::ofstream out_;
    std::mutex mutex_;
};

struct PeerConfig {
  uint32_t id;
  std::string host;
  int port;
};

struct FederationConfig {
  uint16_t id;
  std::string name;
  std::vector<std::string> endpoints;
};

struct Config {
  std::string address = "0.0.0.0";
  int port = 8080;
  int zmq_port = 8081;
  uint32_t node_id = 1;
  uint16_t cluster_id = 1;
  std::string cluster_name = "local";
  std::string db_path = "prod_l3kvg_db";
  size_t thread_pool_size = 256;
  std::vector<PeerConfig> peers;
  std::vector<FederationConfig> federations;
  size_t node_cache_size_per_shard = 2000;
  size_t node_cache_shards = 8;
  size_t edge_write_shards = 8;
  size_t prefix_scan_limit = 1000;
  int zmq_sndhwm = 1000;
  int edge_flush_interval_ms = 2;
  int fed_timeout_ms = 500;
  int breaker_failure_threshold = 3;
  int breaker_reset_timeout_ms = 5000;
  int health_check_interval_ms = 1000;
  size_t node_initial_buffer_size = 1024;
  double default_min_weight = -999999.0;
  std::string auth_secret;
};

Config load_config(const std::string &path) {
  Config cfg;
  std::ifstream f(path);
  if (f.is_open()) {
    try {
      json j;
      f >> j;
      cfg.address = j.value("address", cfg.address);
      cfg.port = j.value("port", cfg.port);
      cfg.zmq_port = j.value("zmq_port", cfg.port + 1);
      cfg.node_id = j.value("node_id", cfg.node_id);
      cfg.cluster_id = j.value("cluster_id", cfg.cluster_id);
      cfg.cluster_name = j.value("cluster_name", cfg.cluster_name);
      cfg.db_path = j.value("db_path", cfg.db_path);
      cfg.thread_pool_size = j.value("thread_pool_size", cfg.thread_pool_size);
      if (j.contains("peers")) {
        for (auto &p : j["peers"]) {
          cfg.peers.push_back({p.value("id", 0u), p.value("host", "127.0.0.1"),
                               p.value("port", 8080)});
        }
      }
      if (j.contains("federations")) {
          for (auto &f : j["federations"]) {
              cfg.federations.push_back({
                  static_cast<uint16_t>(f.value("id", 0u)),
                  f.value("name", ""),
                  f.value("endpoints", std::vector<std::string>{})
              });
          }
      }
      cfg.node_cache_size_per_shard = j.value("node_cache_size_per_shard", cfg.node_cache_size_per_shard);
      cfg.node_cache_shards = j.value("node_cache_shards", cfg.node_cache_shards);
      cfg.edge_write_shards = j.value("edge_write_shards", cfg.edge_write_shards);
      cfg.prefix_scan_limit = j.value("prefix_scan_limit", cfg.prefix_scan_limit);
      cfg.zmq_sndhwm = j.value("zmq_sndhwm", cfg.zmq_sndhwm);
      cfg.edge_flush_interval_ms = j.value("edge_flush_interval_ms", cfg.edge_flush_interval_ms);
      cfg.fed_timeout_ms = j.value("fed_timeout_ms", cfg.fed_timeout_ms);
      cfg.breaker_failure_threshold = j.value("breaker_failure_threshold", cfg.breaker_failure_threshold);
      cfg.breaker_reset_timeout_ms = j.value("breaker_reset_timeout_ms", cfg.breaker_reset_timeout_ms);
      cfg.health_check_interval_ms = j.value("health_check_interval_ms", cfg.health_check_interval_ms);
      cfg.node_initial_buffer_size = j.value("node_initial_buffer_size", cfg.node_initial_buffer_size);
      cfg.default_min_weight = j.value("default_min_weight", cfg.default_min_weight);
      cfg.auth_secret = j.value("auth_secret", "");
    } catch (...) {
      std::cerr << "Failed to parse config, using defaults.\n";
    }
  }
  return cfg;
}

int main(int argc, char *argv[]) {
  fprintf(stdout, "--- MAIN START ---\n");
  fflush(stdout);
#ifdef _WIN32
  WSADATA wsaData;
  if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
      fprintf(stderr, "WSAStartup failed.\n");
      return 1;
  }
#endif
  try {
    std::string config_path = "config.json";
    if (argc > 1) {
      config_path = argv[1];
    }

    printf("Step 1: Loading config: %s\n", config_path.c_str()); fflush(stdout);
    Config cfg = load_config(config_path);
    printf("Step 2: Config loaded. Node: %u\n", cfg.node_id); fflush(stdout);

    l3kvg::Settings settings;
    settings.node_cache_size_per_shard = cfg.node_cache_size_per_shard;
    settings.node_cache_shards = cfg.node_cache_shards;
    settings.edge_write_shards = cfg.edge_write_shards;
    settings.prefix_scan_limit = cfg.prefix_scan_limit;
    settings.zmq_sndhwm = cfg.zmq_sndhwm;
    settings.edge_flush_interval_ms = cfg.edge_flush_interval_ms;
    settings.fed_timeout_ms = cfg.fed_timeout_ms;
    settings.node_id = cfg.node_id;
    settings.breaker_failure_threshold = cfg.breaker_failure_threshold;
    settings.breaker_reset_timeout_ms = cfg.breaker_reset_timeout_ms;
    settings.health_check_interval_ms = cfg.health_check_interval_ms;
    settings.node_initial_buffer_size = cfg.node_initial_buffer_size;
    settings.default_min_weight = cfg.default_min_weight;

    auto ring = std::make_shared<lite3::ConsistentHash>();
    ring->add_node(cfg.node_id);
    for (const auto &p : cfg.peers) {
        ring->add_node(p.id);
    }
    printf("Step 3: Ring initialized with %zu local peers\n", cfg.peers.size() + 1); fflush(stdout);

    auto engine = std::make_unique<l3kvg::Engine>(cfg.db_path, cfg.node_id, ring, cfg.thread_pool_size, settings);
    engine->get_resolver().register_local_cluster(cfg.cluster_name, cfg.cluster_id);
    engine->set_auth_secret(cfg.auth_secret);
    engine->get_store()->credentials().register_user(cfg.node_id, cfg.cluster_name, "local-node-key"); // UID is node_id for simplicity
    
    auto logger = std::make_shared<FileLogger>("node" + std::to_string(cfg.node_id) + ".log");
    engine->get_store()->set_logger(logger);
    printf("Step 4: Engine created with logging to node%u.log\n", cfg.node_id); fflush(stdout);

    for (const auto &p : cfg.peers) {
      printf("Step 5: Adding peer client: %u at %s:%d\n", p.id, p.host.c_str(), p.port); fflush(stdout);
      engine->get_remote_client().add_peer(
          p.id, "tcp://" + p.host + ":" + std::to_string(p.port));
    }

    for (const auto &f : cfg.federations) {
        printf("Step 5.5: Registering federation: %s (ID: %u)\n", f.name.c_str(), f.id); fflush(stdout);
        engine->get_resolver().register_federation(f.name, f.id, f.endpoints);
        for (const auto &ep : f.endpoints) {
            engine->get_remote_client().add_peer(f.id, ep);
        }
    }
    printf("Step 6: Peers and Federations added\n"); fflush(stdout);

    l3kvg::CypherParser parser(engine.get());
    printf("Step 7: Parser created\n"); fflush(stdout);

    // Setup ZMQ Server for Federation
    std::thread zmq_thread([&]() {
        zmq::context_t ctx(1);
        zmq::socket_t sock(ctx, ZMQ_ROUTER);
        std::string zmq_endpoint = "tcp://0.0.0.0:" + std::to_string(cfg.zmq_port);
        sock.bind(zmq_endpoint);
        printf("ZMQ Federation Server listening on %s\n", zmq_endpoint.c_str()); fflush(stdout);

        while (true) {
            std::vector<zmq::message_t> recv_msgs;
            auto result = zmq::recv_multipart(sock, std::back_inserter(recv_msgs));
            if (!result || recv_msgs.size() < 4) continue;

            auto& identity = recv_msgs[0];
            auto opcode = recv_msgs[2].to_string();

            if (opcode == "R") {
                try {
                    std::vector<uint64_t> nodes = json::parse(recv_msgs[3].to_string());
                    std::string query_json = recv_msgs[4].to_string();

                    auto results = engine->query().resume(nodes, query_json).execute();

                    json j_res = json::array();
                    for (const auto& row : results) {
                        json jr = json::object();
                        for (const auto& [k, v] : row.fields) jr[k] = v;
                        j_res.push_back(jr);
                    }

                    std::string resp_json = j_res.dump();
                    sock.send(identity, zmq::send_flags::sndmore);
                    sock.send(zmq::message_t(), zmq::send_flags::sndmore);
                    sock.send(zmq::message_t(resp_json.data(), resp_json.size()), zmq::send_flags::none);
                } catch (...) {
                    sock.send(identity, zmq::send_flags::sndmore);
                    sock.send(zmq::message_t(), zmq::send_flags::sndmore);
                    sock.send(zmq::message_t("[]", 2), zmq::send_flags::none);
                }
            } else if (opcode == "N") {
                // GET neighbors
                try {
                    uint64_t target_id = std::stoull(recv_msgs[3].to_string(), nullptr, 16);
                    std::string label = recv_msgs[4].to_string();
                    double min_weight = 0.0;
                    if (recv_msgs.size() >= 6) min_weight = std::stod(recv_msgs[5].to_string());
                    
                    auto node = engine->get_node(target_id);
                    auto neighbors = node->get_neighbors(label, min_weight);
                    
                    json j_neighs = json::array();
                    for (auto id : neighbors) {
                        char id_buf[17];
                        std::snprintf(id_buf, sizeof(id_buf), "%016llx", (unsigned long long)id);
                        j_neighs.push_back(std::string(id_buf));
                    }
                    
                    std::string resp_json = j_neighs.dump();
                    sock.send(identity, zmq::send_flags::sndmore);
                    sock.send(zmq::message_t(), zmq::send_flags::sndmore);
                    sock.send(zmq::message_t(resp_json.data(), resp_json.size()), zmq::send_flags::none);
                } catch (...) {
                    sock.send(identity, zmq::send_flags::sndmore);
                    sock.send(zmq::message_t(), zmq::send_flags::sndmore);
                    sock.send(zmq::message_t("[]", 2), zmq::send_flags::none);
                }
            } else if (opcode == "G") {
                // GET node
                try {
                    uint64_t id = std::stoull(recv_msgs[3].to_string(), nullptr, 16);
                    std::string key = std::string(l3kvg::KeyBuilder::node_key(id));
                    auto buf = engine->get_store()->get(key);
                    
                    sock.send(identity, zmq::send_flags::sndmore);
                    sock.send(zmq::message_t(), zmq::send_flags::sndmore);
                    if (buf.size() > 0) {
                        sock.send(zmq::message_t(buf.data(), buf.size()), zmq::send_flags::none);
                    } else {
                        sock.send(zmq::message_t("", 0), zmq::send_flags::none);
                    }
                } catch (...) {
                    sock.send(identity, zmq::send_flags::sndmore);
                    sock.send(zmq::message_t(), zmq::send_flags::sndmore);
                    sock.send(zmq::message_t("", 0), zmq::send_flags::none);
                }
            } else if (opcode == "P") {
                // PUT node/edge (key based)
                try {
                    std::string key = recv_msgs[3].to_string();
                    std::string payload = recv_msgs[4].to_string();
                    engine->get_store()->put(key, payload);
                    
                    sock.send(identity, zmq::send_flags::sndmore);
                    sock.send(zmq::message_t(), zmq::send_flags::sndmore);
                    sock.send(zmq::message_t("OK", 2), zmq::send_flags::none);
                } catch (...) {
                    sock.send(identity, zmq::send_flags::sndmore);
                    sock.send(zmq::message_t(), zmq::send_flags::sndmore);
                    sock.send(zmq::message_t("ERR", 3), zmq::send_flags::none);
                }
            } else if (opcode == "B") {
                // BATCH PUT (binary buffer)
                try {
                    std::vector<uint8_t> data(reinterpret_cast<const uint8_t*>(recv_msgs[3].data()), 
                                             reinterpret_cast<const uint8_t*>(recv_msgs[3].data()) + recv_msgs[3].size());
                    lite3cpp::Buffer batch_buf(std::move(data));
                    for (auto it = batch_buf.begin(0); it != batch_buf.end(0); ++it) {
                        std::string key_str(it->key);
                        std::span<const std::byte> val_bytes = batch_buf.get_bytes(0, it->key);
                        std::string val_str(reinterpret_cast<const char*>(val_bytes.data()), val_bytes.size());
                        engine->get_store()->put(key_str, val_str);
                    }
                    sock.send(identity, zmq::send_flags::sndmore);
                    sock.send(zmq::message_t(), zmq::send_flags::sndmore);
                    sock.send(zmq::message_t("OK", 2), zmq::send_flags::none);
                } catch (...) {
                    sock.send(identity, zmq::send_flags::sndmore);
                    sock.send(zmq::message_t(), zmq::send_flags::sndmore);
                    sock.send(zmq::message_t("ERR", 3), zmq::send_flags::none);
                }
            } else if (opcode == "H") {
                // HEARTBEAT
                sock.send(identity, zmq::send_flags::sndmore);
                sock.send(zmq::message_t(), zmq::send_flags::sndmore);
                sock.send(zmq::message_t("OK", 2), zmq::send_flags::none);
            } else if (opcode == "S") {
                // REPLICATION SYNC
                try {
                    std::string key = recv_msgs[3].to_string();
                    std::string payload = recv_msgs[4].to_string();
                    uint16_t origin_cluster_id = 0;
                    if (recv_msgs.size() >= 6) {
                        origin_cluster_id = static_cast<uint16_t>(std::stoi(recv_msgs[5].to_string()));
                    }
                    engine->replicate_key(key, payload, origin_cluster_id);
                    
                    sock.send(identity, zmq::send_flags::sndmore);
                    sock.send(zmq::message_t(), zmq::send_flags::sndmore);
                    sock.send(zmq::message_t("OK", 2), zmq::send_flags::none);
                } catch (...) {
                    sock.send(identity, zmq::send_flags::sndmore);
                    sock.send(zmq::message_t(), zmq::send_flags::sndmore);
                    sock.send(zmq::message_t("ERR", 3), zmq::send_flags::none);
                }
            }
        }
    });
    zmq_thread.detach();

    httplib::Server svr;
    svr.new_task_queue = [tp_size = cfg.thread_pool_size] { return new httplib::ThreadPool(tp_size); };
    svr.set_keep_alive_max_count(1000000);
    svr.set_keep_alive_timeout(60);

    svr.set_post_routing_handler([](const auto &req, auto &res) {
      res.set_header("Access-Control-Allow-Origin", "*");
      res.set_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
      res.set_header("Access-Control-Allow-Headers", "Content-Type");
    });

  svr.Options(".*", [](const httplib::Request &, httplib::Response &res) {
    res.status = 200;
  });

  svr.Get("/api/metrics", [&](const httplib::Request &, httplib::Response &res) {
    auto &m = engine->get_metrics();
    json j_metrics = {{"hop_latency_us", static_cast<uint64_t>(m.hop_latency_us.load())},
              {"serialization_time_us", static_cast<uint64_t>(m.serialization_time_us.load())},
              {"cache_hits", static_cast<uint64_t>(m.cache_hits.load())},
              {"cache_misses", static_cast<uint64_t>(m.cache_misses.load())}};
    res.set_content(j_metrics.dump(), "application/json");
  });

  svr.Post("/api/query", [&](const httplib::Request &req, httplib::Response &res) {
    try {
      auto start = std::chrono::high_resolution_clock::now();
      auto rows = parser.execute(req.body);
      auto end = std::chrono::high_resolution_clock::now();
      auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
      engine->get_metrics().hop_latency_us.store(static_cast<uint64_t>(duration / (rows.empty() ? 1 : rows.size())));

      json j_rows = json::array();
      for (const auto &row : rows) {
        json r = json::object();
        for (const auto &[k, v] : row.fields) r[k] = v;
        j_rows.push_back(r);
      }
      res.set_content(j_rows.dump(), "application/json");
    } catch (const std::exception &e) {
      res.status = 400;
      res.set_content(json{{"error", e.what()}}.dump(), "application/json");
    }
  });

  svr.Get("/api/internal/node/:uuid", [&](const httplib::Request &req, httplib::Response &res) {
    std::string uuid = req.path_params.at("uuid");
    lite3cpp::Buffer buf = engine->get_store()->get(uuid);
    if (buf.size() > 0) {
      res.set_content(std::string(reinterpret_cast<const char *>(buf.data()), buf.size()), "application/octet-stream");
    } else {
      res.status = 404;
    }
  });

  svr.Post("/api/internal/nodes/batch", [&](const httplib::Request &req, httplib::Response &res) {
    try {
      json j_batch_req = json::parse(req.body);
      std::vector<std::string> ids = j_batch_req.at("uuids").get<std::vector<std::string>>();
      json j_batch_resp = json::object();
      for (const auto &id : ids) {
        lite3cpp::Buffer b = engine->get_store()->get(id);
        if (b.size() > 0) {
          j_batch_resp[id] = std::string(reinterpret_cast<const char *>(b.data()), b.size());
        }
      }
      res.set_content(j_batch_resp.dump(), "application/json");
    } catch (...) { res.status = 400; }
  });

  svr.Post("/api/internal/neighbors", [&](const httplib::Request &req, httplib::Response &res) {
    try {
      json j_neigh_req = json::parse(req.body);
      std::string t = j_neigh_req.at("target").get<std::string>();
      std::string l = j_neigh_req.at("label").get<std::string>();
      double w = j_neigh_req.value("min_weight", 0.0);
      auto n = engine->get_node(t);
      std::vector<uint64_t> neighbors = n->get_neighbors(l, w);
      json j_neigh_resp = json::array();
      for (auto id : neighbors) {
          char buf[32];
          std::snprintf(buf, sizeof(buf), "%016llx", (unsigned long long)id);
          j_neigh_resp.push_back(std::string(buf));
      }
      res.set_content(j_neigh_resp.dump(), "application/json");
    } catch (...) { res.status = 400; }
  });

  svr.Post("/api/internal/put_node", [&](const httplib::Request &req, httplib::Response &res) {
    try {
      json j_pn = json::parse(req.body);
      engine->get_store()->put(j_pn.at("target").get<std::string>(), j_pn.at("payload").dump());
      res.status = 200;
    } catch (...) { res.status = 400; }
  });

  svr.Post("/api/internal/batch_put", [&](const httplib::Request &req, httplib::Response &res) {
    try {
      bool is_binary = (req.get_header_value("Content-Type").find("application/octet-stream") != std::string::npos);
      if (!is_binary && !req.body.empty() && (unsigned char)req.body[0] < 32) is_binary = true;

      if (is_binary) {
          std::vector<uint8_t> data(req.body.begin(), req.body.end());
          lite3cpp::Buffer batch_buf(std::move(data));
          for (auto it = batch_buf.begin(0); it != batch_buf.end(0); ++it) {
              std::string key_str(it->key);
              std::span<const std::byte> val_bytes = batch_buf.get_bytes(0, it->key);
              std::string val_str(reinterpret_cast<const char*>(val_bytes.data()), val_bytes.size());
              engine->get_store()->put(key_str, val_str);
          }
      } else {
          json j_batch = json::parse(req.body);
          for (auto it = j_batch.begin(); it != j_batch.end(); ++it) {
            engine->get_store()->put(it.key(), it.value().dump());
          }
      }
      res.status = 200;
    } catch (...) {
      res.status = 400;
    }
  });

  svr.Post("/api/internal/put_edge", [&](const httplib::Request &req, httplib::Response &res) {
    try {
      json j_pe = json::parse(req.body);
      engine->get_store()->put(j_pe.at("key").get<std::string>(), j_pe.at("payload").dump());
      res.status = 200;
    } catch (...) { res.status = 400; }
  });

  printf("L3KVG Server listening on %s:%d\n", cfg.address.c_str(), cfg.port); fflush(stdout);
  svr.listen(cfg.address.c_str(), cfg.port);
  } catch (const std::exception &e) {
    std::cerr << "CRITICAL SERVER ERROR: " << e.what() << std::endl;
    return 1;
  } catch (...) {
    std::cerr << "CRITICAL UNKNOWN SERVER ERROR" << std::endl;
    return 1;
  }
  return 0;
}
