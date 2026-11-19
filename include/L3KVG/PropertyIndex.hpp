#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace l3kv {
class Engine;
}

namespace l3kvg {

enum class PropertyOp : uint8_t {
    Eq = 0,
    Prefix = 1,
    Contains = 2,
    Gt = 3,
    Gte = 4,
    Lt = 5,
    Lte = 6,
    Between = 7,
    UnitEq = 8
};

struct PropertyQuery {
    std::string attr;
    PropertyOp op{PropertyOp::Eq};
    std::string val;
    double num_min{-std::numeric_limits<double>::infinity()};
    double num_max{std::numeric_limits<double>::infinity()};
    bool min_inclusive{true};
    bool max_inclusive{true};
};

/// Native scoped secondary property index for L3KVG backed by co-located L3KV `{scope}` hash tags.
/// Maintains exact string keys (`pidxs:`), order-preserving IEEE-754 numeric range keys (`pidxn:`),
/// and optional unit keys (`pidxu:`) with O(log N + K) single-shard range seeks.
class PropertyIndex {
public:
    explicit PropertyIndex(l3kv::Engine* store) noexcept : store_(store) {}

    /// Encodes a finite IEEE-754 double into a 16-char lowercase hex string whose
    /// lexicographical order strictly matches numerical order from -inf to +inf.
    [[nodiscard]] static std::string encode_sortable_double(double value);
    [[nodiscard]] static double decode_sortable_double(std::string_view hex16);
    [[nodiscard]] static std::optional<double> try_parse_double(std::string_view sv);

    /// Indexes `(attr, val, unit) -> entity_key` under `{scope}` (co-located on one L3KV shard).
    void put(std::string_view scope,
             std::string_view attr,
             std::string_view val,
             std::string_view entity_key,
             std::string_view unit = "");

    /// Removes `(attr, val, unit) -> entity_key` from `{scope}`.
    void remove(std::string_view scope,
                std::string_view attr,
                std::string_view val,
                std::string_view entity_key,
                std::string_view unit = "");

    /// Writes an overlay tombstone for `(attr, val) -> entity_key` under `{overlay_scope}`.
    void put_tombstone(std::string_view overlay_scope,
                       std::string_view attr,
                       std::string_view val,
                       std::string_view entity_key);

    /// Single-scope exact match (`pidxs:{scope}:attr:val:`).
    [[nodiscard]] std::vector<std::string> query_eq(std::string_view scope,
                                                    std::string_view attr,
                                                    std::string_view val,
                                                    size_t limit = 1000) const;

    /// Single-scope prefix match (`pidxs:{scope}:attr:val_prefix`).
    [[nodiscard]] std::vector<std::string> query_prefix(std::string_view scope,
                                                        std::string_view attr,
                                                        std::string_view val_prefix,
                                                        size_t limit = 1000) const;

    /// Single-scope O(log N + K) numeric range scan using `pidxn:{scope}:attr:<sortable_hex>:`.
    [[nodiscard]] std::vector<std::string> query_numeric_range(std::string_view scope,
                                                               std::string_view attr,
                                                               double min_val,
                                                               bool min_inclusive,
                                                               double max_val,
                                                               bool max_inclusive,
                                                               size_t limit = 1000) const;

    /// Single-scope unit match (`pidxu:{scope}:attr:unit:`).
    [[nodiscard]] std::vector<std::string> query_unit(std::string_view scope,
                                                      std::string_view attr,
                                                      std::string_view unit,
                                                      size_t limit = 1000) const;

    /// Evaluates a `PropertyQuery` across a single scope.
    [[nodiscard]] std::vector<std::string> query(std::string_view scope,
                                                 const PropertyQuery& q,
                                                 size_t limit = 1000) const;

    /// Evaluates a `PropertyQuery` merging `base_scope` + `overlay_scope` - overlay tombstones.
    [[nodiscard]] std::vector<std::string> query_merged(std::string_view base_scope,
                                                        std::string_view overlay_scope,
                                                        const PropertyQuery& q,
                                                        size_t limit = 1000) const;

private:
    l3kv::Engine* store_{nullptr};
};

} // namespace l3kvg
