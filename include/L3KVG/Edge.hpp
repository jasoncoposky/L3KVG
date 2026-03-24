#pragma once

#include "buffer.hpp"
#include "json.hpp"
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <iostream>
#include <stdexcept>

namespace l3kvg {

class Engine;

class Edge {
public:
  Edge(Engine *engine, std::string src, std::string label, double weight, std::string dst, std::string payload_raw = "")
      : engine_(engine), src_(std::move(src)), label_(std::move(label)), 
        weight_(weight), dst_(std::move(dst)) {
      if (!payload_raw.empty()) {
          payload_ = lite3cpp::lite3_json::from_json_string(payload_raw);
      }
  }

  const std::string& get_src() const { return src_; }
  const std::string& get_label() const { return label_; }
  double get_weight() const { return weight_; }
  const std::string& get_dst() const { return dst_; }

  template <typename T> T get_attribute(std::string_view key) {
    if (!payload_ || payload_->size() == 0)
      return T{};

    // DEBUG: print keys at root
    // std::cerr << "[Edge::get_attribute] Debug: root type: " << (int)payload_->get_type(0, "") << "\n";

    // In Evolution 3, user properties are stored under the "props" key
    size_t props_ofs = 0;
    try {
        if (payload_->get_type(0, "props") == lite3cpp::Type::Object) {
            props_ofs = payload_->get_obj(0, "props");
        } else {
            // Fallback to root for nodes or non-nested edges
            props_ofs = 0;
        }
    } catch (...) {
        props_ofs = 0;
    }

    std::string k(key);
    try {
        if constexpr (std::is_same_v<T, std::string>) {
          return std::string(payload_->get_str(props_ofs, k));
        } else if constexpr (std::is_integral_v<T>) {
          return static_cast<T>(payload_->get_i64(props_ofs, k));
        } else if constexpr (std::is_floating_point_v<T>) {
          return static_cast<T>(payload_->get_f64(props_ofs, k));
        }
    } catch (const std::exception& e) {
        std::cerr << "[Edge::get_attribute] Error retrieving key '" << k << "': " << e.what() << "\n";
        throw;
    }
    return T{};
  }

private:
  Engine *engine_;
  std::string src_;
  std::string label_;
  double weight_;
  std::string dst_;
  std::optional<lite3cpp::Buffer> payload_;
};

} // namespace l3kvg
