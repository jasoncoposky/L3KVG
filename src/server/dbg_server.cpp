#include <winsock2.h>
#include <ws2tcpip.h>
#include <iostream>
#include <fstream>
#include <memory>
#include <string>
#include <vector>
#include <mutex>
#include <nlohmann/json.hpp>
#include "httplib.h"
#include "L3KVG/RemoteL3KVClient.hpp"
#include "L3KVG/Engine.hpp"
#include "L3KVG/Cypher.hpp"
#include "lite3/ring.hpp"
#include "observability.hpp"

using json = nlohmann::json;

class FileLogger : public lite3cpp::ILogger {
public:
    FileLogger(const std::string& path) : out_(path, std::ios::app) {}
    bool log(lite3cpp::LogLevel level, std::string_view message,
             std::string_view operation,
             std::chrono::microseconds duration, size_t buffer_offset,
             std::string_view key = "") override {
        std::lock_guard<std::mutex> lock(mutex_);
        out_ << "[" << static_cast<int>(level) << "] " << operation << ": " << message << std::endl;
        out_.flush();
        return true;
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
    std::cout << "DEBUG START\n" << std::flush;
    WSADATA wsaData;
    WSAStartup(MAKEWORD(2, 2), &wsaData);

    try {
        std::string path = (argc > 1) ? argv[1] : "../../config1.json";
        FileLogger logger("dbg_out.log");
        lite3cpp::set_logger(&logger);
        
        std::cout << "Loading config: " << path << "...\n" << std::flush;
        Config cfg = load_config(path);
        
        std::cout << "Creating ring for node " << cfg.node_id << "...\n" << std::flush;
        auto ring = std::make_shared<lite3::ConsistentHash>();
        ring->add_node(cfg.node_id);
        for (const auto &p : cfg.peers) ring->add_node(p.id);
        
        std::cout << "Creating Engine (" << cfg.db_path << ")...\n" << std::flush;
        auto engine = std::make_unique<l3kvg::Engine>(cfg.db_path, cfg.node_id, ring);
        
        std::cout << "Creating Parser...\n" << std::flush;
        l3kvg::CypherParser parser(engine.get());
        
        std::cout << "Creating httplib::Server...\n" << std::flush;
        httplib::Server svr;
        
        std::cout << "ALL SUCCESSFUL!\n" << std::flush;
    } catch (const std::exception& e) {
        std::cout << "ERROR: " << e.what() << "\n" << std::flush;
    }
    WSACleanup();
    return 0;
}
