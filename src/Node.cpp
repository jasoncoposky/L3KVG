#include "L3KVG/Node.hpp"
#include "L3KVG/Engine.hpp"
#include "engine/store.hpp"

namespace l3kvg {

Node::Node(Engine *engine, std::string uuid)
    : engine_(engine), uuid_(std::move(uuid)) {}

void Node::ensure_loaded() {
  if (loaded_)
    return;

  std::string key = "n:{" + uuid_ + "}";
  auto buf = engine_->get_store()->get(key);

  if (buf.size() > 0) {
    payload_ = std::move(buf);
  }
  loaded_ = true;
}

std::vector<std::string> Node::get_neighbors(std::string_view label,
                                             double min_weight) {
  std::vector<std::string> neighbors;
  std::string prefix = "e:out:{" + uuid_ + "}:" + std::string(label) + ":";

  std::string min_w_str = Engine::format_weight(min_weight);
  std::string start_key = prefix + min_w_str;

  auto *store = engine_->get_store();
  size_t target_shard = store->get_routing_shard(prefix);

  auto chunk = store->get_prefix_keys(prefix, target_shard, start_key, 1000);
  for (const auto &key : chunk) {
    if (key.ends_with(":meta"))
      continue;
    size_t last_colon = key.find_last_of(':');
    if (last_colon != std::string::npos) {
      neighbors.push_back(key.substr(last_colon + 1));
    }
  }
  return neighbors;
}

} // namespace l3kvg
