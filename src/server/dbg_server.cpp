#ifdef _WIN32
#include <winsock2.h>
#endif
#ifdef _WIN32
#include <ws2tcpip.h>
#endif
#include <iostream>
#include <fstream>
#include <memory>
#include <string>
#include <vector>
#include <mutex>
#include "httplib.h"
#include "L3KVG/RemoteL3KVClient.hpp"
#include "L3KVG/Engine.hpp"
#include "L3KVG/Cypher.hpp"
#include "lite3/ring.hpp"
#include "observability.hpp"
#include "buffer.hpp"
#include "json.hpp"

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
      std::string str((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
      lite3cpp::Buffer buf = lite3cpp::lite3_json::from_json_string(str);
      if (buf.get_type(0, "address") == lite3cpp::Type::String) {
          cfg.address = std::string(buf.get_str(0, "address"));
      }
      if (buf.get_type(0, "port") == lite3cpp::Type::Int64) {
          cfg.port = static_cast<int>(buf.get_i64(0, "port"));
      }
      if (buf.get_type(0, "node_id") == lite3cpp::Type::Int64) {
          cfg.node_id = static_cast<uint32_t>(buf.get_i64(0, "node_id"));
      }
      if (buf.get_type(0, "db_path") == lite3cpp::Type::String) {
          cfg.db_path = std::string(buf.get_str(0, "db_path"));
      }
      if (buf.get_type(0, "peers") == lite3cpp::Type::Array) {
          size_t peers_arr_ofs = buf.get_arr(0, "peers");
          lite3cpp::NodeView pn(reinterpret_cast<const lite3cpp::PackedNodeLayout*>(buf.data() + peers_arr_ofs));
          for (uint32_t i = 0; i < pn.size(); ++i) {
              if (buf.arr_get_type(peers_arr_ofs, i) != lite3cpp::Type::Object) continue;
              size_t po = buf.arr_get_obj(peers_arr_ofs, i);
              PeerConfig pc;
              pc.id = (buf.get_type(po, "id") == lite3cpp::Type::Int64) ? static_cast<uint32_t>(buf.get_i64(po, "id")) : 0u;
              pc.host = (buf.get_type(po, "host") == lite3cpp::Type::String) ? std::string(buf.get_str(po, "host")) : "127.0.0.1";
              pc.port = (buf.get_type(po, "port") == lite3cpp::Type::Int64) ? static_cast<int>(buf.get_i64(po, "port")) : 8080;
              cfg.peers.push_back(std::move(pc));
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
#ifdef _WIN32
    WSADATA wsaData;
    WSAStartup(MAKEWORD(2, 2), &wsaData);
#endif

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
#ifdef _WIN32
    WSACleanup();
#endif
    return 0;
}
