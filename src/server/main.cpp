#include "L3KVG/Node.hpp"
#include "L3KVG/Cypher.hpp"
#include "L3KVG/Engine.hpp"
#include "L3KVG/KeyBuilder.hpp"
#include "L3KVG/RemoteL3KVClient.hpp"
#include "L3KVG/Settings.hpp"
#include "engine/store.hpp"
#include <cstdio>
#include <fstream>
#include <iostream>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <thread>
#include <vector>
#include <zmq.hpp>
#include <zmq_addon.hpp>
#include "httplib.h"

using json = nlohmann::json;

struct Config {
    std::string db_path;
    uint16_t node_id;
    uint32_t zmq_port;
    uint32_t http_port;
    std::string auth_secret;
    std::string local_cluster_name;
    uint16_t local_cluster_id;
    struct Federation {
        std::string name;
        uint16_t id;
        std::vector<std::string> endpoints;
    };
    std::vector<Federation> federations;
};

Config load_config(const std::string &path) {
    std::ifstream f(path);
    if (!f.is_open()) {
        throw std::runtime_error("Could not open config file: " + path);
    }
    json j = json::parse(f);
    Config cfg;
    cfg.db_path = j.at("db_path").get<std::string>();
    cfg.node_id = j.at("node_id").get<uint16_t>();
    cfg.zmq_port = j.value("zmq_port", 5556);
    cfg.http_port = j.value("http_port", 8080);
    cfg.auth_secret = j.value("auth_secret", "");
    cfg.local_cluster_name = j.value("local_cluster_name", "");
    cfg.local_cluster_id = j.value("local_cluster_id", 0);
    if (j.contains("federations")) {
        for (const auto &fj : j["federations"]) {
            Config::Federation fed;
            fed.name = fj.at("name").get<std::string>();
            fed.id = fj.at("id").get<uint16_t>();
            for (const auto &ep : fj.at("endpoints")) {
                fed.endpoints.push_back(ep.get<std::string>());
            }
            cfg.federations.push_back(fed);
        }
    }
    return cfg;
}

int main(int argc, char *argv[]) {
  if(1) std::fprintf(stderr, "DEBUG_SERVER_v3: STARTING\n"); //std::fflush(stderr);

  if (argc < 2) {
      if(1) std::fprintf(stderr, "Usage: %s <config.json>\n", argv[0]);
      return 1;
  }

  Config cfg;
  try {
      cfg = load_config(argv[1]);
  } catch (const std::exception &e) {
      if(1) std::fprintf(stderr, "DEBUG_SERVER_v3: Failed to load config: %s\n", e.what()); //std::fflush(stderr);
      return 1;
  }

  l3kvg::Settings settings;
  settings.node_id = cfg.node_id;

  auto engine = std::make_unique<l3kvg::Engine>(cfg.db_path, cfg.node_id, nullptr, 4, settings);
  if(1) std::fprintf(stderr, "DEBUG_SERVER_v3: Engine Ready\n"); //std::fflush(stderr);

  if (cfg.local_cluster_name.size() > 0) {
      engine->get_resolver().register_local_cluster(cfg.local_cluster_name, cfg.local_cluster_id);
      if(1) std::fprintf(stderr, "DEBUG_SERVER_v3: Registered Local Cluster: %s (%u)\n", cfg.local_cluster_name.c_str(), cfg.local_cluster_id); //std::fflush(stderr);
  }

  for (const auto &f : cfg.federations) {
      engine->get_resolver().register_federation(f.name, f.id, f.endpoints);
      for (const auto &ep : f.endpoints) {
          engine->get_remote_client().add_peer(f.id, ep);
      }
  }

  // Setup ZMQ Server for Federation
  std::thread zmq_thread([&]() {
      try {
          zmq::context_t ctx(1);
          zmq::socket_t sock(ctx, ZMQ_ROUTER);
          std::string zmq_endpoint = "tcp://0.0.0.0:" + std::to_string(cfg.zmq_port);
          sock.bind(zmq_endpoint);
          if(1) std::fprintf(stderr, "DEBUG_SERVER_v3: ZMQ Ready on %s\n", zmq_endpoint.c_str()); //std::fflush(stderr);

          auto last_flush = std::chrono::steady_clock::now();
          while (true) {
              zmq::pollitem_t items[] = { { (void*)sock, 0, ZMQ_POLLIN, 0 } };
              zmq::poll(&items[0], 1, std::chrono::milliseconds(100));

              if (std::chrono::steady_clock::now() - last_flush > std::chrono::seconds(1)) {
                  engine->get_store()->flush();
                  last_flush = std::chrono::steady_clock::now();
              }

              if (!(items[0].revents & ZMQ_POLLIN)) continue;

              std::vector<zmq::message_t> recv_msgs;
              auto res = zmq::recv_multipart(sock, std::back_inserter(recv_msgs));
              if (!res || recv_msgs.empty()) continue;

              if(0) {
                  std::fprintf(stderr, "L3_SERVER: Received %zu frames\n", recv_msgs.size());
                  for (size_t i = 0; i < recv_msgs.size(); ++i) {
                      std::fprintf(stderr, "  Frame %zu: size=%zu, content=[%s]\n", i, recv_msgs[i].size(), (recv_msgs[i].size() < 64 ? recv_msgs[i].to_string().c_str() : "LONG"));
                  }
              }

              if (recv_msgs.size() < 4) {
                  continue;
              }

              auto identity = std::move(recv_msgs[0]);
              size_t data_idx = 1;
              if (recv_msgs[data_idx].size() == 0) data_idx++; // Skip delimiter

              uint32_t principal_id = 0;
              if (data_idx < recv_msgs.size() && recv_msgs[data_idx].size() == 4) {
                  std::memcpy(&principal_id, recv_msgs[data_idx].data(), 4);
                  data_idx++;
              }

              if (data_idx >= recv_msgs.size()) continue;

              std::string opcode = recv_msgs[data_idx].to_string(); data_idx++;
              std::fprintf(stderr, "L3_SERVER: Handling opcode [%s] from pid [%u]\n", opcode.c_str(), principal_id);
              std::fflush(stderr);

              if (opcode == "A") { // Auth
                  std::string node_id_str = recv_msgs[data_idx].to_string(); data_idx++;
                  std::string secret = recv_msgs[data_idx].to_string(); data_idx++;
                  
                  sock.send(identity, zmq::send_flags::sndmore);
                  sock.send(zmq::message_t(), zmq::send_flags::sndmore);
                  if (cfg.auth_secret.empty() || secret == cfg.auth_secret) {
                      sock.send(zmq::message_t("OK", 2), zmq::send_flags::none);
                  } else {
                      sock.send(zmq::message_t("ERR_AUTH", 8), zmq::send_flags::none);
                  }
              }

              if (opcode == "+") { // Atomic Increment
                  if (data_idx >= recv_msgs.size()) continue;
                  std::string key = recv_msgs[data_idx].to_string(); data_idx++;
                  int64_t delta = 1;
                  if (data_idx < recv_msgs.size()) {
                      delta = std::stoll(recv_msgs[data_idx].to_string());
                      data_idx++;
                  }

                  try {
                      auto buf = engine->get_store()->get(key);
                      int64_t val = 0;
                      if (buf.size() > 0) {
                          lite3cpp::Buffer lbuf(std::vector<uint8_t>(buf.data(), buf.data() + buf.size()));
                          val = lbuf.get_i64(0, "v");
                      }
                      val += delta;
                      
                      lite3cpp::Buffer nbuf; nbuf.init_object(); nbuf.set_i64(0, "v", val);
                      engine->get_store()->put(key, nbuf.move_to_string());
                      
                      sock.send(identity, zmq::send_flags::sndmore);
                      sock.send(zmq::message_t(), zmq::send_flags::sndmore);
                      sock.send(zmq::message_t(std::to_string(val)), zmq::send_flags::none);
                  } catch (const std::exception& e) {
                      sock.send(identity, zmq::send_flags::sndmore);
                      sock.send(zmq::message_t(), zmq::send_flags::sndmore);
                      sock.send(zmq::message_t("ERR", 3), zmq::send_flags::none);
                  }
              }

              if (opcode == "R") {
                  try {
                      std::vector<uint64_t> nodes = json::parse(recv_msgs[data_idx].to_string());
                      data_idx++;
                      std::string query_json = recv_msgs[data_idx].to_string();
                      std::fprintf(stderr, "L3_SERVER: Handling opcode R: nodes=%zu, query=%s\n", nodes.size(), query_json.c_str());
                      std::fflush(stderr);
                      auto results = engine->query().resume(nodes, query_json).execute();
                      
                      json j_res = json::array();
                      for (const auto& row : results) {
                          json j_row;
                          j_row["fields"] = row.fields;
                          j_res.push_back(j_row);
                      }
                      
                      std::string resp_json = j_res.dump();
                      sock.send(identity, zmq::send_flags::sndmore);
                      sock.send(zmq::message_t(), zmq::send_flags::sndmore);
                      sock.send(zmq::message_t(resp_json.data(), resp_json.size()), zmq::send_flags::none);
                  } catch (const std::exception& e) {
                      if(1) std::fprintf(stderr, "L3_SERVER: Error R (std::exception): %s\n", e.what()); //std::fflush(stderr);
                      sock.send(identity, zmq::send_flags::sndmore);
                      sock.send(zmq::message_t(), zmq::send_flags::sndmore);
                      sock.send(zmq::message_t("[]", 2), zmq::send_flags::none);
                  } catch (...) {
                      if(1) std::fprintf(stderr, "L3_SERVER: Error R (unknown exception)\n"); //std::fflush(stderr);
                      sock.send(identity, zmq::send_flags::sndmore);
                      sock.send(zmq::message_t(), zmq::send_flags::sndmore);
                      sock.send(zmq::message_t("[]", 2), zmq::send_flags::none);
                  }
              } else if (opcode == "P") {
                  try {
                      std::string key = recv_msgs[data_idx].to_string(); data_idx++;
                      std::string payload = recv_msgs[data_idx].to_string();
                      

                      if (key.starts_with("e:out:{")) {
                          uint64_t src = 0, dst = 0; double weight = 0; char label_buf[256] = {0};
                          int parsed = std::sscanf(key.c_str(), "e:out:{%llx\x7d:%255[^:]:%lf:{%llx\x7d", (unsigned long long*)&src, label_buf, &weight, (unsigned long long*)&dst);
                          if (parsed == 4) {
                              engine->add_edge(src, label_buf, weight, dst, payload);
                          } else {
                              engine->get_store()->put(key, payload);
                          }
                      } else if (key.starts_with("n:{")) {
                          uint64_t id = 0;
                          int parsed = std::sscanf(key.c_str(), "n:{%llx\x7d", (unsigned long long*)&id);
                          if (parsed == 1) {
                              engine->put_node(id, payload);
                          } else {
                              engine->get_store()->put(key, payload);
                          }
                      } else {
                          engine->get_store()->put(key, payload);
                      }
                      
                      sock.send(identity, zmq::send_flags::sndmore);
                      sock.send(zmq::message_t(), zmq::send_flags::sndmore);
                      sock.send(zmq::message_t("OK", 2), zmq::send_flags::none);
                  } catch (const std::exception& e) {
                      if(1) std::fprintf(stderr, "L3_SERVER: Error P: %s\n", e.what()); //std::fflush(stderr);
                      sock.send(identity, zmq::send_flags::sndmore);
                      sock.send(zmq::message_t(), zmq::send_flags::sndmore);
                      sock.send(zmq::message_t("ERR", 3), zmq::send_flags::none);
                  }
              } else if (opcode == "M") {
                  try {
                      lite3cpp::Buffer res_buf; res_buf.init_object();
                      while (data_idx < recv_msgs.size()) {
                          std::string key = recv_msgs[data_idx].to_string(); data_idx++;
                          auto buf = engine->get_store()->get(key);
                          if (buf.size() > 0) {
                              res_buf.set_bytes(0, key, std::span<const std::byte>((const std::byte*)buf.data(), buf.size()));
                          }
                      }
                      std::string resp_bin = res_buf.move_to_string();
                      sock.send(identity, zmq::send_flags::sndmore);
                      sock.send(zmq::message_t(), zmq::send_flags::sndmore);
                      sock.send(zmq::message_t(resp_bin.data(), resp_bin.size()), zmq::send_flags::none);
                  } catch (const std::exception& e) {
                      if(1) std::fprintf(stderr, "L3_SERVER: Error M: %s\n", e.what()); //std::fflush(stderr);
                      sock.send(identity, zmq::send_flags::sndmore);
                      sock.send(zmq::message_t(), zmq::send_flags::sndmore);
                      sock.send(zmq::message_t("", 0), zmq::send_flags::none);
                  }
              } else if (opcode == "G") {
                  try {
                      std::string key = recv_msgs[data_idx].to_string();
                      auto buf = engine->get_store()->get(key);
                      sock.send(identity, zmq::send_flags::sndmore);
                      sock.send(zmq::message_t(), zmq::send_flags::sndmore);
                      sock.send(zmq::message_t(buf.data(), buf.size()), zmq::send_flags::none);
                  } catch (const std::exception& e) {
                      if(1) std::fprintf(stderr, "L3_SERVER: Error G: %s\n", e.what()); //std::fflush(stderr);
                      sock.send(identity, zmq::send_flags::sndmore);
                      sock.send(zmq::message_t(), zmq::send_flags::sndmore);
                      sock.send(zmq::message_t("", 0), zmq::send_flags::none);
                  }
              } else if (opcode == "D") {
                  try {
                      std::string key = recv_msgs[data_idx].to_string();
                      if (key.starts_with("n:{")) {
                          size_t end_pos = key.find('}', 3);
                          if (end_pos != std::string::npos) {
                              uint64_t nid = std::stoull(key.substr(3, end_pos - 3), nullptr, 16);
                              engine->del_node(nid);
                          } else {
                              engine->get_store()->del(key);
                          }
                      } else if (key.starts_with("e:out:{")) {
                          uint64_t src = 0, dst = 0; double weight = 0; char label_buf[256] = {0};
                          int parsed = std::sscanf(key.c_str(), "e:out:{%llx\x7d:%255[^:]:%lf:{%llx\x7d", (unsigned long long*)&src, label_buf, &weight, (unsigned long long*)&dst);
                          if (parsed == 4) {
                              engine->del_edge(src, label_buf, weight, dst);
                          } else {
                              engine->get_store()->del(key);
                          }
                      } else {
                          engine->get_store()->del(key);
                      }
                      sock.send(identity, zmq::send_flags::sndmore);
                      sock.send(zmq::message_t(), zmq::send_flags::sndmore);
                      sock.send(zmq::message_t("OK", 2), zmq::send_flags::none);
                  } catch (const std::exception& e) {
                      if(1) std::fprintf(stderr, "L3_SERVER: Error D: %s\n", e.what()); //std::fflush(stderr);
                      sock.send(identity, zmq::send_flags::sndmore);
                      sock.send(zmq::message_t(), zmq::send_flags::sndmore);
                      sock.send(zmq::message_t("ERR", 3), zmq::send_flags::none);
                  }
              } else if (opcode == "N") {
                  try {
                      std::string id_str = recv_msgs[data_idx].to_string(); data_idx++;
                      std::string label = recv_msgs[data_idx].to_string(); data_idx++;
                      std::string w_str = recv_msgs[data_idx].to_string();
                      
                      uint64_t target_node_id = std::stoull(id_str, nullptr, 16);
                      double min_weight = std::stod(w_str);
                      
                      auto node = engine->get_node(target_node_id);
                      std::vector<uint64_t> neighs;
                      if (node) neighs = node->get_neighbors(label, min_weight, principal_id);
                      
                      json j_res = neighs;
                      std::string resp_json = j_res.dump();
                      sock.send(identity, zmq::send_flags::sndmore);
                      sock.send(zmq::message_t(), zmq::send_flags::sndmore);
                      sock.send(zmq::message_t(resp_json.data(), resp_json.size()), zmq::send_flags::none);
                  } catch (const std::exception& e) {
                      if(1) std::fprintf(stderr, "L3_SERVER: Error N: %s\n", e.what()); //std::fflush(stderr);
                      sock.send(identity, zmq::send_flags::sndmore);
                      sock.send(zmq::message_t(), zmq::send_flags::sndmore);
                      sock.send(zmq::message_t("[]", 2), zmq::send_flags::none);
                  }
              } else if (opcode == "I") {
                  try {
                      std::string id_str = recv_msgs[data_idx].to_string(); data_idx++;
                      std::string label = recv_msgs[data_idx].to_string();
                      
                      uint64_t target_node_id = std::stoull(id_str, nullptr, 16);
                      
                      auto node = engine->get_node(target_node_id);
                      std::vector<uint64_t> neighs;
                      if (node) neighs = node->get_in_neighbors(label, principal_id);
                      
                      json j_res = neighs;
                      std::string resp_json = j_res.dump();
                      sock.send(identity, zmq::send_flags::sndmore);
                      sock.send(zmq::message_t(), zmq::send_flags::sndmore);
                      sock.send(zmq::message_t(resp_json.data(), resp_json.size()), zmq::send_flags::none);
                  } catch (const std::exception& e) {
                      if(1) std::fprintf(stderr, "L3_SERVER: Error I: %s\n", e.what()); //std::fflush(stderr);
                      sock.send(identity, zmq::send_flags::sndmore);
                      sock.send(zmq::message_t(), zmq::send_flags::sndmore);
                      sock.send(zmq::message_t("[]", 2), zmq::send_flags::none);
                  }
              }
          }
      } catch (const std::exception& e) {
          if(1) std::fprintf(stderr, "DEBUG_SERVER_v3: ZMQ Error: %s\n", e.what()); //std::fflush(stderr);
      }
  });

  httplib::Server svr;
  svr.Get("/api/health", [](const httplib::Request&, httplib::Response& res) {
      res.set_content("OK", "text/plain");
  });
  if(1) std::fprintf(stderr, "DEBUG_SERVER_v3: HTTP Ready\n"); //std::fflush(stderr);
  svr.listen("0.0.0.0", cfg.http_port);

  if (zmq_thread.joinable()) zmq_thread.join();
  return 0;
}
