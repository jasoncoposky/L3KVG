#include "L3KVG/Node.hpp"
#include "L3KVG/Cypher.hpp"
#include "L3KVG/Engine.hpp"
#include "L3KVG/KeyBuilder.hpp"
#include "L3KVG/RemoteL3KVClient.hpp"
#include "L3KVG/Settings.hpp"
#include "L3KVG/MutationBatch.hpp"
#include "engine/store.hpp"
#include <cstdio>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>
#include <zmq.hpp>
#include <zmq_addon.hpp>
#include "httplib.h"
#include "buffer.hpp"
#include "L3KVG/Query.hpp"

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
    std::string str((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    lite3cpp::Buffer buf = lite3cpp::lite3_json::from_json_string(str);
    Config cfg;
    if (buf.get_type(0, "db_path") == lite3cpp::Type::String) {
        cfg.db_path = std::string(buf.get_str(0, "db_path"));
    }
    if (buf.get_type(0, "node_id") == lite3cpp::Type::Int64) {
        cfg.node_id = static_cast<uint16_t>(buf.get_i64(0, "node_id"));
    }
    cfg.zmq_port = (buf.get_type(0, "zmq_port") == lite3cpp::Type::Int64) ? static_cast<uint32_t>(buf.get_i64(0, "zmq_port")) : 5556;
    cfg.http_port = (buf.get_type(0, "http_port") == lite3cpp::Type::Int64) ? static_cast<uint32_t>(buf.get_i64(0, "http_port")) : 8080;
    cfg.auth_secret = (buf.get_type(0, "auth_secret") == lite3cpp::Type::String) ? std::string(buf.get_str(0, "auth_secret")) : "";
    cfg.local_cluster_name = (buf.get_type(0, "local_cluster_name") == lite3cpp::Type::String) ? std::string(buf.get_str(0, "local_cluster_name")) : "";
    cfg.local_cluster_id = (buf.get_type(0, "local_cluster_id") == lite3cpp::Type::Int64) ? static_cast<uint16_t>(buf.get_i64(0, "local_cluster_id")) : 0;
    if (buf.get_type(0, "federations") == lite3cpp::Type::Array) {
        size_t fed_arr_ofs = buf.get_arr(0, "federations");
        lite3cpp::NodeView fn(reinterpret_cast<const lite3cpp::PackedNodeLayout*>(buf.data() + fed_arr_ofs));
        for (uint32_t i = 0; i < fn.size(); ++i) {
            if (buf.arr_get_type(fed_arr_ofs, i) != lite3cpp::Type::Object) continue;
            size_t fo = buf.arr_get_obj(fed_arr_ofs, i);
            Config::Federation fed;
            if (buf.get_type(fo, "name") == lite3cpp::Type::String) {
                fed.name = std::string(buf.get_str(fo, "name"));
            }
            if (buf.get_type(fo, "id") == lite3cpp::Type::Int64) {
                fed.id = static_cast<uint16_t>(buf.get_i64(fo, "id"));
            }
            if (buf.get_type(fo, "endpoints") == lite3cpp::Type::Array) {
                size_t ep_arr_ofs = buf.get_arr(fo, "endpoints");
                lite3cpp::NodeView epn(reinterpret_cast<const lite3cpp::PackedNodeLayout*>(buf.data() + ep_arr_ofs));
                for (uint32_t j = 0; j < epn.size(); ++j) {
                    if (buf.arr_get_type(ep_arr_ofs, j) == lite3cpp::Type::String) {
                        fed.endpoints.push_back(std::string(buf.arr_get_str(ep_arr_ofs, j)));
                    }
                }
            }
            cfg.federations.push_back(std::move(fed));
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
              static const bool s_server_debug = (std::getenv("L3_SERVER_DEBUG") != nullptr);
              if (s_server_debug) {
                  std::fprintf(stderr, "L3_SERVER: Handling opcode [%s] from pid [%u]\n", opcode.c_str(), principal_id);
                  std::fflush(stderr);
              }

              if (opcode == "A") { // Auth
                  if (data_idx + 2 > recv_msgs.size()) {
                      sock.send(identity, zmq::send_flags::sndmore);
                      sock.send(zmq::message_t(), zmq::send_flags::sndmore);
                      sock.send(zmq::message_t("ERR", 3), zmq::send_flags::none);
                      continue;
                  }
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
                  if (data_idx + 1 > recv_msgs.size()) {
                      sock.send(identity, zmq::send_flags::sndmore);
                      sock.send(zmq::message_t(), zmq::send_flags::sndmore);
                      sock.send(zmq::message_t("ERR", 3), zmq::send_flags::none);
                      continue;
                  }
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
                  if (data_idx + 2 > recv_msgs.size()) {
                      lite3cpp::Buffer empty_buf;
                      empty_buf.init_array();
                      sock.send(identity, zmq::send_flags::sndmore);
                      sock.send(zmq::message_t(), zmq::send_flags::sndmore);
                      sock.send(zmq::message_t(empty_buf.data(), empty_buf.size()), zmq::send_flags::none);
                      continue;
                  }
                  try {
                      const auto& nodes_msg = recv_msgs[data_idx];
                      data_idx++;
                      std::vector<uint64_t> nodes;
                      bool parsed_as_json = false;
                      std::string_view sv(static_cast<const char*>(nodes_msg.data()), nodes_msg.size());
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
                                              nodes.push_back(static_cast<uint64_t>(buf.arr_get_i64(0, i)));
                                          } else if (t == lite3cpp::Type::String) {
                                              nodes.push_back(std::stoull(std::string(buf.arr_get_str(0, i)), nullptr, 16));
                                          }
                                      }
                                  }
                              }
                          } catch (...) {
                              parsed_as_json = false;
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
                      lite3cpp::Buffer qbuf;
                      const uint8_t* qptr = static_cast<const uint8_t*>(qmsg.data());
                      if (qmsg.size() >= sizeof(lite3cpp::PackedNodeLayout) && (qptr[0] == 0x06 || qptr[0] == 0x07)) {
                          qbuf = lite3cpp::Buffer(std::vector<uint8_t>(qptr, qptr + qmsg.size()));
                      } else {
                          std::string qstr = qmsg.to_string();
                          qbuf = lite3cpp::lite3_json::from_json_string(qstr.empty() ? "{}" : qstr);
                      }

                      if (s_server_debug) {
                          std::fprintf(stderr, "L3_SERVER: Handling opcode R: nodes=%zu, query_size=%zu\n", nodes.size(), qmsg.size());
                          std::fflush(stderr);
                      }
                      uint32_t eff_principal = (principal_id != 0) ? principal_id : l3kvg::INTERNAL_UID;
                      auto results = engine->query().set_principal_id(eff_principal).resume(nodes, qbuf).execute();
                      
                      lite3cpp::Buffer resp_buf = l3kvg::Query::serialize_results(results);
                      sock.send(identity, zmq::send_flags::sndmore);
                      sock.send(zmq::message_t(), zmq::send_flags::sndmore);
                      sock.send(zmq::message_t(resp_buf.data(), resp_buf.size()), zmq::send_flags::none);
                  } catch (const std::exception& e) {
                      if(1) std::fprintf(stderr, "L3_SERVER: Error R (std::exception): %s\n", e.what()); //std::fflush(stderr);
                      sock.send(identity, zmq::send_flags::sndmore);
                      sock.send(zmq::message_t(), zmq::send_flags::sndmore);
                      sock.send(zmq::message_t("ERR", 3), zmq::send_flags::none);
                  } catch (...) {
                      if(1) std::fprintf(stderr, "L3_SERVER: Error R (unknown exception)\n"); //std::fflush(stderr);
                      sock.send(identity, zmq::send_flags::sndmore);
                      sock.send(zmq::message_t(), zmq::send_flags::sndmore);
                      sock.send(zmq::message_t("ERR", 3), zmq::send_flags::none);
                  }
              } else if (opcode == "P") {
                  if (data_idx + 2 > recv_msgs.size()) {
                      sock.send(identity, zmq::send_flags::sndmore);
                      sock.send(zmq::message_t(), zmq::send_flags::sndmore);
                      sock.send(zmq::message_t("ERR", 3), zmq::send_flags::none);
                      continue;
                  }
                  try {
                      std::string key = recv_msgs[data_idx].to_string(); data_idx++;
                      std::string payload = recv_msgs[data_idx].to_string();
                      
                      static const bool s_l3_debug = (std::getenv("L3_DEBUG") != nullptr);
                      if (s_l3_debug) {
                          std::fprintf(stderr, "[L3_SERVER] Opcode P: key=%s val_len=%zu\n", key.c_str(), payload.size());
                      }

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
                  if (data_idx + 1 > recv_msgs.size()) {
                      sock.send(identity, zmq::send_flags::sndmore);
                      sock.send(zmq::message_t(), zmq::send_flags::sndmore);
                      sock.send(zmq::message_t("", 0), zmq::send_flags::none);
                      continue;
                  }
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
                  if (data_idx + 1 > recv_msgs.size()) {
                      sock.send(identity, zmq::send_flags::sndmore);
                      sock.send(zmq::message_t(), zmq::send_flags::sndmore);
                      sock.send(zmq::message_t("ERR", 3), zmq::send_flags::none);
                      continue;
                  }
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
              } else if (opcode == "B") {
                  if (data_idx + 1 > recv_msgs.size()) {
                      sock.send(identity, zmq::send_flags::sndmore);
                      sock.send(zmq::message_t(), zmq::send_flags::sndmore);
                      sock.send(zmq::message_t("ERR", 3), zmq::send_flags::none);
                      continue;
                  }
                  try {
                      const auto& msg = recv_msgs[data_idx];
                      lite3cpp::Buffer batch_buf(std::vector<uint8_t>(
                          static_cast<const uint8_t*>(msg.data()),
                          static_cast<const uint8_t*>(msg.data()) + msg.size()
                      ));
                      bool ok = engine->apply_batch(batch_buf, principal_id);
                      sock.send(identity, zmq::send_flags::sndmore);
                      sock.send(zmq::message_t(), zmq::send_flags::sndmore);
                      if (ok) {
                          sock.send(zmq::message_t("OK", 2), zmq::send_flags::none);
                      } else {
                          sock.send(zmq::message_t("ERR", 3), zmq::send_flags::none);
                      }
                  } catch (const std::exception& e) {
                      if (1) std::fprintf(stderr, "L3_SERVER: Error B: %s\n", e.what());
                      sock.send(identity, zmq::send_flags::sndmore);
                      sock.send(zmq::message_t(), zmq::send_flags::sndmore);
                      sock.send(zmq::message_t("ERR", 3), zmq::send_flags::none);
                  }
              } else if (opcode == "E") {
                  if (data_idx + 4 > recv_msgs.size()) {
                      sock.send(identity, zmq::send_flags::sndmore);
                      sock.send(zmq::message_t(), zmq::send_flags::sndmore);
                      sock.send(zmq::message_t("ERR", 3), zmq::send_flags::none);
                      continue;
                  }
                  try {
                      uint64_t src = std::stoull(recv_msgs[data_idx].to_string(), nullptr, 16); data_idx++;
                      std::string label = recv_msgs[data_idx].to_string(); data_idx++;
                      double weight = std::stod(recv_msgs[data_idx].to_string()); data_idx++;
                      uint64_t dst = std::stoull(recv_msgs[data_idx].to_string(), nullptr, 16);
                      engine->add_edge(src, label, weight, dst);
                      sock.send(identity, zmq::send_flags::sndmore);
                      sock.send(zmq::message_t(), zmq::send_flags::sndmore);
                      sock.send(zmq::message_t("OK", 2), zmq::send_flags::none);
                  } catch (const std::exception& e) {
                      if (1) std::fprintf(stderr, "L3_SERVER: Error E: %s\n", e.what());
                      sock.send(identity, zmq::send_flags::sndmore);
                      sock.send(zmq::message_t(), zmq::send_flags::sndmore);
                      sock.send(zmq::message_t("ERR", 3), zmq::send_flags::none);
                  }
              } else if (opcode == "N") {
                  if (data_idx + 3 > recv_msgs.size()) {
                      sock.send(identity, zmq::send_flags::sndmore);
                      sock.send(zmq::message_t(), zmq::send_flags::sndmore);
                      sock.send(zmq::message_t("ERR", 3), zmq::send_flags::none);
                      continue;
                  }
                  try {
                      std::string id_str = recv_msgs[data_idx].to_string(); data_idx++;
                      std::string label = recv_msgs[data_idx].to_string(); data_idx++;
                      std::string w_str = recv_msgs[data_idx].to_string();
                      
                      uint64_t target_node_id = std::stoull(id_str, nullptr, 16);
                      double min_weight = std::stod(w_str);
                      
                      auto node = engine->get_node(target_node_id);
                      std::vector<uint64_t> neighs;
                      if (node) neighs = node->get_neighbors(label, min_weight, principal_id);
                      
                      static const bool s_l3_debug = (std::getenv("L3_DEBUG") != nullptr);
                      if (s_l3_debug) {
                          std::fprintf(stderr, "[L3_SERVER] Opcode N: id=%016llx label=%s min_w=%f -> count=%zu\n",
                                       (unsigned long long)target_node_id, label.c_str(), min_weight, neighs.size());
                      }

                      sock.send(identity, zmq::send_flags::sndmore);
                      sock.send(zmq::message_t(), zmq::send_flags::sndmore);
                      sock.send(zmq::message_t(neighs.data(), neighs.size() * sizeof(uint64_t)), zmq::send_flags::none);
                  } catch (const std::exception& e) {
                      if(1) std::fprintf(stderr, "L3_SERVER: Error N: %s\n", e.what()); //std::fflush(stderr);
                      sock.send(identity, zmq::send_flags::sndmore);
                      sock.send(zmq::message_t(), zmq::send_flags::sndmore);
                      sock.send(zmq::message_t("ERR", 3), zmq::send_flags::none);
                  } catch (...) {
                      if(1) std::fprintf(stderr, "L3_SERVER: Unknown Error N\n");
                      sock.send(identity, zmq::send_flags::sndmore);
                      sock.send(zmq::message_t(), zmq::send_flags::sndmore);
                      sock.send(zmq::message_t("ERR", 3), zmq::send_flags::none);
                  }
              } else if (opcode == "I") {
                  if (data_idx + 2 > recv_msgs.size()) {
                      sock.send(identity, zmq::send_flags::sndmore);
                      sock.send(zmq::message_t(), zmq::send_flags::sndmore);
                      sock.send(zmq::message_t("ERR", 3), zmq::send_flags::none);
                      continue;
                  }
                  try {
                      std::string id_str = recv_msgs[data_idx].to_string(); data_idx++;
                      std::string label = recv_msgs[data_idx].to_string();
                      
                      uint64_t target_node_id = std::stoull(id_str, nullptr, 16);
                      
                      auto node = engine->get_node(target_node_id);
                      std::vector<uint64_t> neighs;
                      if (node) neighs = node->get_in_neighbors(label, principal_id);
                      
                      static const bool s_l3_debug = (std::getenv("L3_DEBUG") != nullptr);
                      if (s_l3_debug) {
                          std::fprintf(stderr, "[L3_SERVER] Opcode I: id=%016llx label=%s -> count=%zu\n",
                                       (unsigned long long)target_node_id, label.c_str(), neighs.size());
                      }

                      sock.send(identity, zmq::send_flags::sndmore);
                      sock.send(zmq::message_t(), zmq::send_flags::sndmore);
                      sock.send(zmq::message_t(neighs.data(), neighs.size() * sizeof(uint64_t)), zmq::send_flags::none);
                  } catch (const std::exception& e) {
                      if(1) std::fprintf(stderr, "L3_SERVER: Error I: %s\n", e.what()); //std::fflush(stderr);
                      sock.send(identity, zmq::send_flags::sndmore);
                      sock.send(zmq::message_t(), zmq::send_flags::sndmore);
                      sock.send(zmq::message_t("ERR", 3), zmq::send_flags::none);
                  } catch (...) {
                      if(1) std::fprintf(stderr, "L3_SERVER: Unknown Error I\n");
                      sock.send(identity, zmq::send_flags::sndmore);
                      sock.send(zmq::message_t(), zmq::send_flags::sndmore);
                      sock.send(zmq::message_t("ERR", 3), zmq::send_flags::none);
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
