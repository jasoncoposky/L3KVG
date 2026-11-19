#include "L3KVG/PropertyIndex.hpp"
#include "engine/store.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>

namespace l3kvg {

namespace {

std::string make_str_prefix(std::string_view scope, std::string_view attr) {
    std::string k;
    k.reserve(scope.size() + attr.size() + 10);
    k += "pidxs:{";
    k.append(scope);
    k += "}:";
    k.append(attr);
    k.push_back(':');
    return k;
}

std::string make_num_prefix(std::string_view scope, std::string_view attr) {
    std::string k;
    k.reserve(scope.size() + attr.size() + 10);
    k += "pidxn:{";
    k.append(scope);
    k += "}:";
    k.append(attr);
    k.push_back(':');
    return k;
}

std::string make_unit_prefix(std::string_view scope, std::string_view attr) {
    std::string k;
    k.reserve(scope.size() + attr.size() + 10);
    k += "pidxu:{";
    k.append(scope);
    k += "}:";
    k.append(attr);
    k.push_back(':');
    return k;
}

std::string make_tomb_prefix(std::string_view scope, std::string_view attr) {
    std::string k;
    k.reserve(scope.size() + attr.size() + 12);
    k += "pidxdel:{";
    k.append(scope);
    k += "}:";
    k.append(attr);
    k.push_back(':');
    return k;
}

} // namespace

std::string PropertyIndex::encode_sortable_double(double value) {
    if (value == 0.0) value = 0.0; // Normalize -0.0 -> +0.0
    uint64_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    if (bits & (1ULL << 63)) {
        bits = ~bits;
    } else {
        bits ^= (1ULL << 63);
    }
    char buf[17];
    std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(bits));
    return std::string(buf, 16);
}

double PropertyIndex::decode_sortable_double(std::string_view hex16) {
    if (hex16.size() != 16) return 0.0;
    uint64_t bits = 0;
    for (char c : hex16) {
        bits <<= 4;
        if (c >= '0' && c <= '9') bits |= static_cast<uint64_t>(c - '0');
        else if (c >= 'a' && c <= 'f') bits |= static_cast<uint64_t>(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') bits |= static_cast<uint64_t>(c - 'A' + 10);
    }
    if (bits & (1ULL << 63)) {
        bits ^= (1ULL << 63);
    } else {
        bits = ~bits;
    }
    double val = 0.0;
    std::memcpy(&val, &bits, sizeof(val));
    return val;
}

std::optional<double> PropertyIndex::try_parse_double(std::string_view sv) {
    if (sv.empty()) return std::nullopt;
    std::string tmp(sv);
    char* end = nullptr;
    double v = std::strtod(tmp.c_str(), &end);
    if (end != tmp.c_str() + tmp.size() || !std::isfinite(v)) {
        return std::nullopt;
    }
    return v;
}

void PropertyIndex::put(std::string_view scope,
                        std::string_view attr,
                        std::string_view val,
                        std::string_view entity_key,
                        std::string_view unit) {
    if (!store_ || scope.empty() || attr.empty() || entity_key.empty()) return;

    std::string sk = make_str_prefix(scope, attr);
    sk.append(val);
    sk.push_back(':');
    sk.append(entity_key);
    store_->put(std::move(sk), "{}", l3kv::INTERNAL_UID);

    if (auto num = try_parse_double(val)) {
        std::string nk = make_num_prefix(scope, attr);
        nk += encode_sortable_double(*num);
        nk.push_back(':');
        nk.append(entity_key);
        store_->put(std::move(nk), "{}", l3kv::INTERNAL_UID);
    }

    if (!unit.empty()) {
        std::string uk = make_unit_prefix(scope, attr);
        uk.append(unit);
        uk.push_back(':');
        uk.append(entity_key);
        store_->put(std::move(uk), "{}", l3kv::INTERNAL_UID);
    }
}

void PropertyIndex::remove(std::string_view scope,
                           std::string_view attr,
                           std::string_view val,
                           std::string_view entity_key,
                           std::string_view unit) {
    if (!store_ || scope.empty() || attr.empty() || entity_key.empty()) return;

    std::string sk = make_str_prefix(scope, attr);
    sk.append(val);
    sk.push_back(':');
    sk.append(entity_key);
    store_->del(sk, l3kv::INTERNAL_UID);

    if (auto num = try_parse_double(val)) {
        std::string nk = make_num_prefix(scope, attr);
        nk += encode_sortable_double(*num);
        nk.push_back(':');
        nk.append(entity_key);
        store_->del(nk, l3kv::INTERNAL_UID);
    }

    if (!unit.empty()) {
        std::string uk = make_unit_prefix(scope, attr);
        uk.append(unit);
        uk.push_back(':');
        uk.append(entity_key);
        store_->del(uk, l3kv::INTERNAL_UID);
    }
}

void PropertyIndex::put_tombstone(std::string_view overlay_scope,
                                  std::string_view attr,
                                  std::string_view val,
                                  std::string_view entity_key) {
    if (!store_ || overlay_scope.empty() || attr.empty() || entity_key.empty()) return;
    std::string tk = make_tomb_prefix(overlay_scope, attr);
    tk.append(val);
    tk.push_back(':');
    tk.append(entity_key);
    store_->put(std::move(tk), "{}", l3kv::INTERNAL_UID);
}

std::vector<std::string> PropertyIndex::query_eq(std::string_view scope,
                                                 std::string_view attr,
                                                 std::string_view val,
                                                 size_t limit) const {
    if (!store_ || scope.empty() || attr.empty()) return {};
    std::string pfx = make_str_prefix(scope, attr);
    pfx.append(val);
    pfx.push_back(':');
    auto keys = store_->get_prefix_keys_all_shards(pfx, "", limit);
    std::vector<std::string> out;
    out.reserve(keys.size());
    for (const auto& k : keys) {
        if (k.size() > pfx.size()) {
            out.emplace_back(k.substr(pfx.size()));
        }
    }
    return out;
}

std::vector<std::string> PropertyIndex::query_prefix(std::string_view scope,
                                                     std::string_view attr,
                                                     std::string_view val_prefix,
                                                     size_t limit) const {
    if (!store_ || scope.empty() || attr.empty()) return {};
    std::string pfx = make_str_prefix(scope, attr);
    pfx.append(val_prefix);
    auto keys = store_->get_prefix_keys_all_shards(pfx, "", limit);
    std::vector<std::string> out;
    out.reserve(keys.size());
    size_t base_len = make_str_prefix(scope, attr).size();
    for (const auto& k : keys) {
        if (k.size() <= base_len) continue;
        std::string_view rest(k.data() + base_len, k.size() - base_len);
        size_t colon = rest.find(':');
        if (colon != std::string_view::npos) {
            out.emplace_back(rest.substr(colon + 1));
        }
    }
    return out;
}

std::vector<std::string> PropertyIndex::query_numeric_range(std::string_view scope,
                                                            std::string_view attr,
                                                            double min_val,
                                                            bool min_inclusive,
                                                            double max_val,
                                                            bool max_inclusive,
                                                            size_t limit) const {
    if (!store_ || scope.empty() || attr.empty() || limit == 0) return {};
    std::string pfx = make_num_prefix(scope, attr);
    std::string min_hex = std::isfinite(min_val) ? encode_sortable_double(min_val) : "0000000000000000";
    std::string max_hex = std::isfinite(max_val) ? encode_sortable_double(max_val) : "ffffffffffffffff";

    // Start directly at lower_bound(pfx + min_hex) on the single co-located shard!
    std::string start_key = pfx + min_hex;
    std::vector<std::string> out;
    out.reserve(std::min<size_t>(limit, 256));

    // Fetch in bounded chunks starting from `start_key`
    std::string cursor = start_key;
    while (out.size() < limit) {
        size_t batch_lim = std::max<size_t>(64, (limit - out.size()) * 2);
        auto keys = store_->get_prefix_keys_all_shards(pfx, cursor, batch_lim);
        if (keys.empty()) break;

        bool hit_upper_bound = false;
        for (const auto& k : keys) {
            if (k.size() <= pfx.size() + 17) continue; // 16 hex + ':' + entity
            std::string_view hex16(k.data() + pfx.size(), 16);
            if (hex16 > max_hex || (!max_inclusive && hex16 == max_hex)) {
                hit_upper_bound = true;
                break;
            }
            if (hex16 < min_hex || (!min_inclusive && hex16 == min_hex)) {
                continue;
            }
            out.emplace_back(k.substr(pfx.size() + 17));
            if (out.size() >= limit) break;
        }
        if (hit_upper_bound || keys.size() < batch_lim) break;
        cursor = keys.back();
        cursor.push_back('\x01');
    }
    return out;
}

std::vector<std::string> PropertyIndex::query_unit(std::string_view scope,
                                                   std::string_view attr,
                                                   std::string_view unit,
                                                   size_t limit) const {
    if (!store_ || scope.empty() || attr.empty() || unit.empty()) return {};
    std::string pfx = make_unit_prefix(scope, attr);
    pfx.append(unit);
    pfx.push_back(':');
    auto keys = store_->get_prefix_keys_all_shards(pfx, "", limit);
    std::vector<std::string> out;
    out.reserve(keys.size());
    for (const auto& k : keys) {
        if (k.size() > pfx.size()) {
            out.emplace_back(k.substr(pfx.size()));
        }
    }
    return out;
}

std::vector<std::string> PropertyIndex::query(std::string_view scope,
                                              const PropertyQuery& q,
                                              size_t limit) const {
    switch (q.op) {
        case PropertyOp::Eq:
            return query_eq(scope, q.attr, q.val, limit);
        case PropertyOp::Prefix:
            return query_prefix(scope, q.attr, q.val, limit);
        case PropertyOp::UnitEq:
            return query_unit(scope, q.attr, q.val, limit);
        case PropertyOp::Gt:
            return query_numeric_range(scope, q.attr, q.num_min, false,
                                       std::numeric_limits<double>::infinity(), true, limit);
        case PropertyOp::Gte:
            return query_numeric_range(scope, q.attr, q.num_min, true,
                                       std::numeric_limits<double>::infinity(), true, limit);
        case PropertyOp::Lt:
            return query_numeric_range(scope, q.attr, -std::numeric_limits<double>::infinity(), true,
                                       q.num_max, false, limit);
        case PropertyOp::Lte:
            return query_numeric_range(scope, q.attr, -std::numeric_limits<double>::infinity(), true,
                                       q.num_max, true, limit);
        case PropertyOp::Between:
            return query_numeric_range(scope, q.attr, q.num_min, q.min_inclusive,
                                       q.num_max, q.max_inclusive, limit);
        case PropertyOp::Contains: {
            if (!store_ || scope.empty() || q.attr.empty()) return {};
            std::string pfx = make_str_prefix(scope, q.attr);
            auto keys = store_->get_prefix_keys_all_shards(pfx, "", limit * 4);
            std::vector<std::string> out;
            for (const auto& k : keys) {
                if (k.size() <= pfx.size()) continue;
                std::string_view rest(k.data() + pfx.size(), k.size() - pfx.size());
                size_t colon = rest.find(':');
                if (colon == std::string_view::npos) continue;
                if (rest.substr(0, colon).find(q.val) != std::string_view::npos) {
                    out.emplace_back(rest.substr(colon + 1));
                    if (out.size() >= limit) break;
                }
            }
            return out;
        }
    }
    return {};
}

std::vector<std::string> PropertyIndex::query_merged(std::string_view base_scope,
                                                     std::string_view overlay_scope,
                                                     const PropertyQuery& q,
                                                     size_t limit) const {
    std::set<std::string> merged;
    if (!base_scope.empty()) {
        for (auto& e : query(base_scope, q, limit)) {
            merged.insert(std::move(e));
        }
    }
    if (!overlay_scope.empty()) {
        // Apply overlay tombstones for this attribute
        if (store_) {
            std::string tpfx = make_tomb_prefix(overlay_scope, q.attr);
            auto tkeys = store_->get_prefix_keys_all_shards(tpfx, "", limit * 4);
            for (const auto& tk : tkeys) {
                if (tk.size() <= tpfx.size()) continue;
                std::string_view rest(tk.data() + tpfx.size(), tk.size() - tpfx.size());
                size_t colon = rest.find(':');
                if (colon != std::string_view::npos) {
                    merged.erase(std::string(rest.substr(colon + 1)));
                }
            }
        }
        for (auto& e : query(overlay_scope, q, limit)) {
            merged.insert(std::move(e));
        }
    }
    std::vector<std::string> out(merged.begin(), merged.end());
    if (out.size() > limit) out.resize(limit);
    return out;
}

} // namespace l3kvg
