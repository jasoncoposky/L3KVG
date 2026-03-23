#include "L3KVG/Cypher.hpp"
#include "L3KVG/Engine.hpp"
#include "httplib.h"
#include <chrono>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

int main() {
  std::string db_path = "prod_l3kvg_db";
  // We do NOT clear the DB here if we want persistence, but for a fresh start
  // we can: std::filesystem::remove_all(db_path);
  auto engine = std::make_unique<l3kvg::Engine>(db_path, 4);

  // Initial Demo Seed
  engine->put_node("npc_1",
                   R"({"id": "npc_1", "type": "npc", "name": "Aragorn"})");
  engine->put_node(
      "npc_2",
      R"({"id": "npc_2", "type": "npc", "name": "Legolas", "faction": "Elves"})");
  engine->put_node(
      "npc_3",
      R"({"id": "npc_3", "type": "npc", "name": "Gimli", "faction": "Dwarves"})");

  engine->add_edge("npc_1", "loyalty", 1.0, "npc_2");
  engine->add_edge("npc_1", "loyalty", 0.9, "npc_3");

  l3kvg::CypherParser parser(engine.get());

  httplib::Server svr;

  // CORS handler via middleware
  svr.set_post_routing_handler([](const auto &req, auto &res) {
    res.set_header("Access-Control-Allow-Origin", "*");
    res.set_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
    res.set_header("Access-Control-Allow-Headers", "Content-Type");
  });

  svr.Options(".*", [](const httplib::Request &, httplib::Response &res) {
    res.set_header("Access-Control-Allow-Origin", "*");
    res.set_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
    res.set_header("Access-Control-Allow-Headers", "Content-Type");
    res.status = 200;
  });

  // Endpoint 1: Metrics
  svr.Get("/api/metrics", [&](const httplib::Request &,
                              httplib::Response &res) {
    auto &metrics = engine->get_metrics();
    std::string json = "{"
                       "\"hop_latency_us\": " +
                       std::to_string(metrics.hop_latency_us.load()) +
                       ","
                       "\"serialization_time_us\": " +
                       std::to_string(metrics.serialization_time_us.load()) +
                       ","
                       "\"cache_hits\": " +
                       std::to_string(metrics.cache_hits.load()) +
                       ","
                       "\"cache_misses\": " +
                       std::to_string(metrics.cache_misses.load()) + "}";
    res.set_content(json, "application/json");
  });

  // Endpoint 2: Cypher Query
  svr.Post(
      "/api/query", [&](const httplib::Request &req, httplib::Response &res) {
        try {
          auto start = std::chrono::high_resolution_clock::now();
          auto rows = parser.execute(req.body);
          auto end = std::chrono::high_resolution_clock::now();

          // Log latency in engine metrics
          auto duration =
              std::chrono::duration_cast<std::chrono::microseconds>(end - start)
                  .count();
          // Just arbitrarily attributing to hop latency for demo purpose if
          // there's no hops
          auto &metrics = engine->get_metrics();
          metrics.hop_latency_us.store(duration /
                                       (rows.size() == 0 ? 1 : rows.size()));

          std::string json = "[";
          for (size_t i = 0; i < rows.size(); ++i) {
            json += "{";
            size_t j = 0;
            for (const auto &[k, v] : rows[i].fields) {
              json += "\"" + k + "\": \"" + v + "\"";
              if (j++ < rows[i].fields.size() - 1)
                json += ",";
            }
            json += "}";
            if (i < rows.size() - 1)
              json += ",";
          }
          json += "]";
          res.set_content(json, "application/json");
        } catch (const std::exception &e) {
          std::string err = R"({"error": ")" + std::string(e.what()) + R"("})";
          res.status = 400;
          res.set_content(err, "application/json");
        }
      });

  std::cout << "L3KVG Embedded Engine API running on http://localhost:8080..."
            << std::endl;
  svr.listen("0.0.0.0", 8080);
  return 0;
}
