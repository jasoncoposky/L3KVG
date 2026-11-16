#pragma once

#include <mutex>
#include <atomic>
#include <charconv>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <variant>
#include <vector>
#include <cstdint>
#include "L3KVG/Edge.hpp"
#include "L3KVG/KeyBuilder.hpp"
#include "engine/credential_manager.hpp"

namespace l3kv {
class Blob;
}

namespace l3kvg {

class Engine;

class Node {
public:
  using AttrValue = std::variant<std::monostate, int64_t, double, bool, std::string_view>;

  Node(Engine *engine, uint64_t id);
  ~Node();

  void ensure_loaded();
  bool try_load_local(uint32_t principal_id = l3kv::INTERNAL_UID);
  bool is_loaded() const { return loaded_.load(std::memory_order_acquire); }
  void hydrate(const std::string &data);

  AttrValue try_get_attr(lite3cpp::HashedKey hk);
  AttrValue try_get_attr(std::string_view key) {
    return try_get_attr(lite3cpp::HashedKey(key));
  }

  bool has_attribute(std::string_view key);
  lite3cpp::Type get_attribute_type(std::string_view key);
  std::string get_attribute_as_string(std::string_view key);

  // Zero-copy Raw Attribute Access
  std::string_view get_raw_view(std::string_view key);
  std::string_view get_attribute_view(std::string_view key);

  // Custom Edge Label Bloom Filter
  bool might_have_edge(std::string_view label) const;
  void register_edge_bloom(std::string_view label);

  // Memory-Resident Pointer Swizzling (Returns instant pointer resolution)
  std::vector<std::shared_ptr<Node>>
  get_hot_neighbors(std::string_view label, double min_weight = -999999.0);

  template <typename T> T get_attribute(std::string_view key);

  std::vector<uint64_t> get_neighbors(std::string_view label,
                                         double min_weight = -999999.0,
                                         uint32_t principal_id = l3kv::INTERNAL_UID);

  std::vector<uint64_t> get_in_neighbors(std::string_view label,
                                            uint32_t principal_id = l3kv::INTERNAL_UID);

  std::vector<std::shared_ptr<Edge>> get_edges(std::string_view label,
                                               double min_weight = -999999.0,
                                               uint32_t principal_id = l3kv::INTERNAL_UID);

  uint64_t get_id() const { return id_; }

private:
  Engine *engine_;
  uint64_t id_;

  // Zero-copy pin to L3KV Blob (when loaded locally) or owning Buffer (when hydrated remotely).
  std::shared_ptr<const l3kv::Blob> blob_pin_;
  std::optional<lite3cpp::Buffer> payload_;

  std::atomic<bool> loaded_{false};
  std::mutex loading_mutex_;
  std::atomic<uint64_t> bloom_filter_{0};
};

template <typename T> T Node::get_attribute(std::string_view key) {
  const AttrValue val = try_get_attr(lite3cpp::HashedKey(key));
  if (std::holds_alternative<std::monostate>(val)) {
    return T{};
  }

  if constexpr (std::is_same_v<T, std::string>) {
    if (const auto* sv = std::get_if<std::string_view>(&val)) return std::string(*sv);
    if (const auto* iv = std::get_if<int64_t>(&val)) return std::to_string(*iv);
    if (const auto* fv = std::get_if<double>(&val)) return std::to_string(*fv);
    if (const auto* bv = std::get_if<bool>(&val)) return *bv ? "true" : "false";
    return {};
  } else if constexpr (std::is_same_v<T, std::string_view>) {
    if (const auto* sv = std::get_if<std::string_view>(&val)) return *sv;
    return {};
  } else if constexpr (std::is_same_v<T, bool>) {
    if (const auto* bv = std::get_if<bool>(&val)) return *bv;
    if (const auto* iv = std::get_if<int64_t>(&val)) return *iv != 0;
    if (const auto* sv = std::get_if<std::string_view>(&val)) {
      return *sv == "true" || *sv == "1" || *sv == "True";
    }
    if (const auto* fv = std::get_if<double>(&val)) return *fv != 0.0;
    return false;
  } else if constexpr (std::is_integral_v<T>) {
    if (const auto* iv = std::get_if<int64_t>(&val)) return static_cast<T>(*iv);
    if (const auto* bv = std::get_if<bool>(&val)) return static_cast<T>(*bv ? 1 : 0);
    if (const auto* fv = std::get_if<double>(&val)) return static_cast<T>(*fv);
    if (const auto* sv = std::get_if<std::string_view>(&val); sv && !sv->empty()) {
      int64_t parsed = 0;
      auto [ptr, ec] = std::from_chars(sv->data(), sv->data() + sv->size(), parsed);
      if (ec == std::errc{} && ptr == sv->data() + sv->size()) {
        return static_cast<T>(parsed);
      }
    }
    return T{};
  } else if constexpr (std::is_floating_point_v<T>) {
    if (const auto* fv = std::get_if<double>(&val)) return static_cast<T>(*fv);
    if (const auto* iv = std::get_if<int64_t>(&val)) return static_cast<T>(*iv);
    if (const auto* bv = std::get_if<bool>(&val)) return static_cast<T>(*bv ? 1.0 : 0.0);
    if (const auto* sv = std::get_if<std::string_view>(&val); sv && !sv->empty()) {
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

} // namespace l3kvg
