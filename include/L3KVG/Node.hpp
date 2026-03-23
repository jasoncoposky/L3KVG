#pragma once

#include "buffer.hpp"
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace l3kvg {

class Engine;

class Node {
public:
  Node(Engine *engine, std::string uuid);

  void ensure_loaded();

  template <typename T> T get_attribute(std::string_view key);

  std::vector<std::string> get_neighbors(std::string_view label,
                                         double min_weight = 0.0);

  const std::string &get_uuid() const { return uuid_; }

private:
  Engine *engine_;
  std::string uuid_;
  std::optional<lite3cpp::Buffer> payload_;
  bool loaded_{false};
};

template <typename T> T Node::get_attribute(std::string_view key) {
  ensure_loaded();
  if (!payload_ || payload_->size() == 0)
    return T{};

  std::string k(key);
  if constexpr (std::is_same_v<T, std::string>) {
    return std::string(payload_->get_str(0, k));
  } else if constexpr (std::is_integral_v<T>) {
    return static_cast<T>(payload_->get_i64(0, k));
  } else if constexpr (std::is_floating_point_v<T>) {
    return static_cast<T>(payload_->get_f64(0, k));
  }
  return T{};
}

} // namespace l3kvg
