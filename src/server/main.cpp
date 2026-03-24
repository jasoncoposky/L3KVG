#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <winsock2.h>
#include <ws2tcpip.h>
#include <BaseTsd.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>
#include "httplib.h"

#include "L3KVG/Cypher.hpp"
#include "L3KVG/Engine.hpp"
#include "L3KVG/Node.hpp"
#include "L3KVG/Query.hpp"
#include "engine/store.hpp"
#include "buffer.hpp"

using json = nlohmann::json;

struct PeerConfig {
  uint32_t id;
  std::string host;
  int port;
};

struct Config {
  std::string address = "0.0.0.0";
  int port = 8080;
  uint32_t node_id = 1;
  std::string db_path = "prod_l3kvg_db";
  std::vector<PeerConfig> peers;
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
      cfg.node_id = j.value("node_id", cfg.node_id);
      cfg.db_path = j.value("db_path", cfg.db_path);
      if (j.contains("peers")) {
        for (auto &p : j["peers"]) {
          cfg.peers.push_back({p.value("id", 0u), p.value("host", "127.0.0.1"),
                               p.value("port", 8080)});
        }
      }
    } catch (...) {
      std::cerr << "Failed to parse config, using defaults.\n";
    }
  }
  return cfg;
}

int main(int argc, char *argv[]) {
  std::string config_path = "config.json";
  if (argc > 1) {
    config_path = argv[1];
  }

  Config cfg = load_config(config_path);

  auto ring = std::make_shared<lite3::ConsistentHash>();
  ring->add_node(cfg.node_id);
  for (const auto &p : cfg.peers) {
    ring->add_node(p.id);
  }

  auto engine = std::make_unique<l3kvg::Engine>(cfg.db_path, cfg.node_id, ring);

  for (const auto &p : cfg.peers) {
    engine->get_remote_client().add_peer(
        p.id, "http://" + p.host + ":" + std::to_string(p.port));
  }

  l3kvg::CypherParser parser(engine.get());
  httplib::Server svr;

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
      std::vector<std::string> neighbors = n->get_neighbors(l, w);
      json j_neigh_resp = neighbors;
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

  svr.Post("/api/internal/put_edge", [&](const httplib::Request &req, httplib::Response &res) {
    try {
      json j_pe = json::parse(req.body);
      engine->get_store()->put(j_pe.at("key").get<std::string>(), j_pe.at("payload").dump());
      res.status = 200;
    } catch (...) { res.status = 400; }
  });

  std::cout << "L3KVG Server [" << cfg.node_id << "] listening on " << cfg.address << ":" << cfg.port << std::endl;
  svr.listen(cfg.address.c_str(), cfg.port);
  return 0;
}
