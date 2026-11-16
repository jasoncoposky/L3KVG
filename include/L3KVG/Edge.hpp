#pragma once

#include "buffer.hpp"
#include <charconv>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <cstdint>

namespace l3kvg {

class Engine;

class Edge {
public:
  Edge(Engine *engine, uint64_t src, std::string label, double weight, uint64_t dst, std::optional<lite3cpp::Buffer> payload = std::nullopt)
      : engine_(engine), src_(src), label_(std::move(label)), 
        weight_(weight), dst_(dst), payload_(std::move(payload)) {}

  Edge(Engine *engine, uint64_t src, std::string label, double weight, uint64_t dst, std::string_view payload_str)
      : engine_(engine), src_(src), label_(std::move(label)), 
        weight_(weight), dst_(dst),
        payload_(payload_str.empty() ? std::nullopt : std::make_optional<lite3cpp::Buffer>(payload_str)) {}

  uint64_t get_src() const { return src_; }
  const std::string& get_label() const { return label_; }
  double get_weight() const { return weight_; }
  uint64_t get_dst() const { return dst_; }

  template <typename T> T get_attribute(std::string_view key) {
    using namespace lite3cpp::literals;
    if (!payload_ || payload_->size() < sizeof(lite3cpp::PackedNodeLayout))
      return T{};

    size_t props_ofs = 0;
    if (auto p = payload_->try_get_obj(0, "props"_hk)) {
      props_ofs = *p;
    }

    const lite3cpp::HashedKey hk(key);
    if constexpr (std::is_same_v<T, std::string>) {
      if (auto sv = payload_->try_get_str(props_ofs, hk)) {
        return std::string(*sv);
      }
      return {};
    } else if constexpr (std::is_same_v<T, std::string_view>) {
      if (auto sv = payload_->try_get_str(props_ofs, hk)) {
        return *sv;
      }
      return {};
    } else if constexpr (std::is_same_v<T, bool>) {
      if (auto b = payload_->try_get_bool(props_ofs, hk)) {
        return *b;
      }
      if (auto iv = payload_->try_get_i64(props_ofs, hk)) {
        return *iv != 0;
      }
      if (auto sv = payload_->try_get_str(props_ofs, hk)) {
        return *sv == "true" || *sv == "1" || *sv == "True";
      }
      return false;
    } else if constexpr (std::is_integral_v<T>) {
      if (auto iv = payload_->try_get_i64(props_ofs, hk)) {
        return static_cast<T>(*iv);
      }
      if (auto b = payload_->try_get_bool(props_ofs, hk)) {
        return static_cast<T>(*b ? 1 : 0);
      }
      if (auto fv = payload_->try_get_f64(props_ofs, hk)) {
        return static_cast<T>(*fv);
      }
      if (auto sv = payload_->try_get_str(props_ofs, hk); sv && !sv->empty()) {
        int64_t parsed = 0;
        auto [ptr, ec] = std::from_chars(sv->data(), sv->data() + sv->size(), parsed);
        if (ec == std::errc{} && ptr == sv->data() + sv->size()) {
          return static_cast<T>(parsed);
        }
      }
      return T{};
    } else if constexpr (std::is_floating_point_v<T>) {
      if (auto fv = payload_->try_get_f64(props_ofs, hk)) {
        return static_cast<T>(*fv);
      }
      if (auto iv = payload_->try_get_i64(props_ofs, hk)) {
        return static_cast<T>(*iv);
      }
      if (auto sv = payload_->try_get_str(props_ofs, hk); sv && !sv->empty()) {
        double parsed = 0.0;
        auto [ptr, ec] = std::from_chars(sv->data(), sv->data() + sv->size(), parsed);
        if (ec == std::errc{} && ptr == sv->data() + sv->size()) {
          return static_cast<T>(parsed);
        }
      }
      return T{};
    }
    return T{};
  }

private:
  Engine *engine_;
  uint64_t src_;
  std::string label_;
  double weight_;
  uint64_t dst_;
  std::optional<lite3cpp::Buffer> payload_;
};

} // namespace l3kvg
