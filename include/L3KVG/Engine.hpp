#pragma once

#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <unordered_map>

namespace l3kv {
class Engine;
}

namespace l3kvg {

class Node;
class Query;

class Engine {
public:
  Engine(const std::string &db_path, uint32_t node_id = 1);
  ~Engine();

  Engine(const Engine &) = delete;
  Engine &operator=(const Engine &) = delete;

  Query query();

  std::shared_ptr<Node> get_node(std::string_view uuid);
  void put_node(std::string_view uuid, const std::string &json_payload);

  static std::string format_weight(double weight);

  void add_edge(std::string_view src_uuid, std::string_view label,
                double weight, std::string_view dst_uuid);

  l3kv::Engine *get_store() const { return store_.get(); }

private:
  std::unique_ptr<l3kv::Engine> store_;
  std::mutex cache_mutex_;
  std::unordered_map<std::string, std::shared_ptr<Node>> node_cache_;
};

} // namespace l3kvg
