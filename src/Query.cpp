#include "L3KVG/Query.hpp"
#include "L3KVG/Engine.hpp"
#include "L3KVG/Node.hpp"
#include "L3KVG/Edge.hpp"
#include "L3KVG/FederationID.hpp"
#include "engine/store.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdio>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <numeric>
#include <regex>
#include <set>
#include <sstream>
#include <strings.h>
#include <unordered_set>

#define L3_LOG(level, ...) do { \
    static const bool s_l3_debug = (std::getenv("L3_DEBUG") != nullptr); \
    if (s_l3_debug) { \
        std::fprintf(stderr, "[L3KVG] " __VA_ARGS__); \
        std::fprintf(stderr, "\n"); \
        std::fflush(stderr); \
    } \
} while(0)

namespace l3kvg {

template<class... Ts> struct overloaded : Ts... { using Ts::operator()...; };
template<class... Ts> overloaded(Ts...) -> overloaded<Ts...>;

namespace {

Query::Filter make_filter(std::string_view alias, std::string_view key, Query::Op op, std::string_view value) {
    Query::Filter f;
    f.alias.assign(alias.data(), alias.size());
    f.key.assign(key.data(), key.size());
    f.op = op;
    f.value.assign(value.data(), value.size());
    f.key_hash = lite3cpp::utils::djb2_hash(key);
    f.numeric_ready = true;
    if (!value.empty()) {
        int64_t iv = 0;
        auto [p_i, ec_i] = std::from_chars(value.data(), value.data() + value.size(), iv);
        if (ec_i == std::errc{} && p_i == value.data() + value.size()) {
            f.has_i64 = true;
            f.i64_val = iv;
            f.has_f64 = true;
            f.f64_val = static_cast<double>(iv);
        } else {
            double fv = 0.0;
            auto [p_f, ec_f] = std::from_chars(value.data(), value.data() + value.size(), fv);
            if (ec_f == std::errc{} && p_f == value.data() + value.size()) {
                f.has_f64 = true;
                f.f64_val = fv;
            }
        }
    }
    return f;
}

inline bool icase_eq(std::string_view a, std::string_view b) noexcept {
    return a.size() == b.size() && (a.empty() || ::strncasecmp(a.data(), b.data(), a.size()) == 0);
}

bool match_like_sv(std::string_view sv, const std::string& pattern) {
    const bool has_underscore = (pattern.find('_') != std::string::npos);
    const size_t first_pct = pattern.find('%');
    const size_t last_pct = pattern.rfind('%');

    if (!has_underscore) {
        if (first_pct == std::string::npos) {
            return icase_eq(sv, pattern);
        }
        if (first_pct == pattern.size() - 1 && last_pct == first_pct) {
            std::string_view pfx(pattern.data(), first_pct);
            return sv.size() >= pfx.size() &&
                   (pfx.empty() || ::strncasecmp(sv.data(), pfx.data(), pfx.size()) == 0);
        }
        if (first_pct == 0 && last_pct == 0) {
            std::string_view sfx(pattern.data() + 1, pattern.size() - 1);
            return sv.size() >= sfx.size() &&
                   (sfx.empty() || ::strncasecmp(sv.data() + sv.size() - sfx.size(), sfx.data(), sfx.size()) == 0);
        }
        if (first_pct == 0 && last_pct == pattern.size() - 1 && pattern.size() >= 2) {
            size_t mid_pct = pattern.find('%', 1);
            if (mid_pct == last_pct) {
                std::string_view sub(pattern.data() + 1, last_pct - 1);
                if (sub.empty()) return true;
                auto it = std::search(
                    sv.begin(), sv.end(), sub.begin(), sub.end(),
                    [](unsigned char a, unsigned char b) { return std::tolower(a) == std::tolower(b); });
                return it != sv.end();
            }
        }
    }

    thread_local static std::unordered_map<std::string, std::regex> s_regex_cache;
    auto it = s_regex_cache.find(pattern);
    if (it == s_regex_cache.end()) {
        std::string regex_str;
        regex_str.reserve(pattern.size() * 2 + 4);
        regex_str.push_back('^');
        for (char c : pattern) {
            if (c == '%') regex_str += ".*";
            else if (c == '_') regex_str.push_back('.');
            else if (c == '.' || c == '*' || c == '+' || c == '?' || c == '(' || c == ')' ||
                     c == '[' || c == ']' || c == '{' || c == '}' || c == '|' || c == '\\' ||
                     c == '^' || c == '$') {
                regex_str.push_back('\\');
                regex_str.push_back(c);
            } else {
                regex_str.push_back(c);
            }
        }
        regex_str.push_back('$');
        try {
            it = s_regex_cache.emplace(pattern, std::regex(regex_str, std::regex_constants::icase)).first;
        } catch (...) {
            return false;
        }
    }
    try {
        return std::regex_match(sv.begin(), sv.end(), it->second);
    } catch (...) {
        return false;
    }
}

bool compare_string_filter(std::string_view sv, const Query::Filter& f,
                           bool has_i64, int64_t f_i64, bool has_f64, double f_f64) {
    switch (f.op) {
        case Query::Op::Eq: return sv == f.value;
        case Query::Op::Ne: return sv != f.value;
        case Query::Op::Gt:
        case Query::Op::Ge:
        case Query::Op::Lt:
        case Query::Op::Le: {
            if (has_i64 && !sv.empty()) {
                int64_t lhs = 0;
                auto [ptr, ec] = std::from_chars(sv.data(), sv.data() + sv.size(), lhs);
                if (ec == std::errc{} && ptr == sv.data() + sv.size()) {
                    if (f.op == Query::Op::Gt) return lhs > f_i64;
                    if (f.op == Query::Op::Ge) return lhs >= f_i64;
                    if (f.op == Query::Op::Lt) return lhs < f_i64;
                    return lhs <= f_i64;
                }
            }
            if (has_f64 && !sv.empty()) {
                double lhs = 0.0;
                auto [ptr, ec] = std::from_chars(sv.data(), sv.data() + sv.size(), lhs);
                if (ec == std::errc{} && ptr == sv.data() + sv.size()) {
                    if (f.op == Query::Op::Gt) return lhs > f_f64;
                    if (f.op == Query::Op::Ge) return lhs >= f_f64;
                    if (f.op == Query::Op::Lt) return lhs < f_f64;
                    return lhs <= f_f64;
                }
            }
            std::string_view rhs(f.value);
            if (f.op == Query::Op::Gt) return sv > rhs;
            if (f.op == Query::Op::Ge) return sv >= rhs;
            if (f.op == Query::Op::Lt) return sv < rhs;
            return sv <= rhs;
        }
        case Query::Op::Like:
            return match_like_sv(sv, f.value);
        case Query::Op::NotLike:
            return !match_like_sv(sv, f.value);
    }
    return false;
}

bool evaluate_filter(Node* node, const Query::Filter& f, Engine* engine) {
    using namespace lite3cpp::literals;

    bool has_i64 = f.has_i64;
    int64_t f_i64 = f.i64_val;
    bool has_f64 = f.has_f64;
    double f_f64 = f.f64_val;
    uint32_t key_hash = f.key_hash;
    if (!f.numeric_ready) {
        key_hash = lite3cpp::utils::djb2_hash(f.key);
        if (!f.value.empty()) {
            auto [pi, eci] = std::from_chars(f.value.data(), f.value.data() + f.value.size(), f_i64);
            if (eci == std::errc{} && pi == f.value.data() + f.value.size()) {
                has_i64 = true;
                has_f64 = true;
                f_f64 = static_cast<double>(f_i64);
            } else {
                auto [pf, ecf] = std::from_chars(f.value.data(), f.value.data() + f.value.size(), f_f64);
                if (ecf == std::errc{} && pf == f.value.data() + f.value.size()) {
                    has_f64 = true;
                }
            }
        }
    }

    if (f.key == "id" && (f.op == Query::Op::Eq || f.op == Query::Op::Ne)) {
        auto id_attr = node->try_get_attr("id"_hk);
        if (const auto* sv = std::get_if<std::string_view>(&id_attr); sv && !sv->empty()) {
            if (f.op == Query::Op::Eq && *sv == f.value) return true;
            if (f.op == Query::Op::Ne && *sv == f.value) return false;
        } else if (const auto* iv = std::get_if<int64_t>(&id_attr)) {
            if (has_i64) {
                if (f.op == Query::Op::Eq && *iv == f_i64) return true;
                if (f.op == Query::Op::Ne && *iv == f_i64) return false;
            }
        }
        uint64_t target_id = engine->get_resolver().parse_uuid(f.value);
        uint64_t node_id = node->get_id();
        return (f.op == Query::Op::Eq) ? (node_id == target_id) : (node_id != target_id);
    }

    if (f.key == "_access_user") {
        std::string_view user_name(f.value);
        if (user_name.empty()) return true;

        auto o_attr = node->try_get_attr("o"_hk);
        if (const auto* owner = std::get_if<std::string_view>(&o_attr); owner && !owner->empty() && *owner == user_name) {
            return true;
        }

        std::string_view p;
        auto p_attr = node->try_get_attr("p"_hk);
        if (const auto* psv = std::get_if<std::string_view>(&p_attr)) p = *psv;
        if (p.empty()) {
            auto n_attr = node->try_get_attr("n"_hk);
            if (const auto* nsv = std::get_if<std::string_view>(&n_attr)) p = *nsv;
        }
        bool is_system_root = (p == "/");
        if (!is_system_root && !p.empty() && p[0] == '/') {
            size_t second_slash = p.find('/', 1);
            if (second_slash == std::string_view::npos) {
                is_system_root = true;
            } else {
                std::string_view sub = p.substr(second_slash);
                if (sub == "/home" || sub == "/trash" || sub == "/trash/home" ||
                    sub == "/home/public" || sub == "/trash/home/public") {
                    is_system_root = true;
                }
            }
        }
        if (is_system_root) {
            return true;
        }

        static constexpr auto perm_rank = [](std::string_view lvl) noexcept -> int {
            if (lvl.starts_with("admin:")) lvl = lvl.substr(6);
            if (lvl == "null") return 1000;
            if (lvl == "execute") return 1010;
            if (lvl == "read_annotation") return 1020;
            if (lvl == "read_system_metadata") return 1030;
            if (lvl == "read_metadata") return 1040;
            if (lvl == "read_object" || lvl == "read") return 1050;
            if (lvl == "write_annotation") return 1060;
            if (lvl == "create_metadata") return 1070;
            if (lvl == "modify_metadata") return 1080;
            if (lvl == "delete_metadata") return 1090;
            if (lvl == "administer_object") return 1100;
            if (lvl == "create_object") return 1110;
            if (lvl == "modify_object" || lvl == "write") return 1120;
            if (lvl == "delete_object" || lvl == "delete") return 1130;
            if (lvl == "create_token") return 1140;
            if (lvl == "delete_token") return 1150;
            if (lvl == "curate") return 1160;
            if (lvl == "own") return 1200;
            return 0;
        };

        auto access_nodes = node->get_in_neighbors("FOR_OBJECT", INTERNAL_UID);
        for (auto aid : access_nodes) {
            auto a_node = engine->get_node(aid);
            if (!a_node) continue;
            auto lvl_attr = a_node->try_get_attr("l"_hk);
            std::string_view level = std::holds_alternative<std::string_view>(lvl_attr)
                                     ? std::get<std::string_view>(lvl_attr) : std::string_view{};
            if (perm_rank(level) < 1050) continue;

            auto user_nodes = a_node->get_in_neighbors("HAS_ACCESS", INTERNAL_UID);
            for (auto uid : user_nodes) {
                auto u_node = engine->get_node(uid);
                if (!u_node) continue;
                auto un_attr = u_node->try_get_attr("n"_hk);
                std::string_view un = std::holds_alternative<std::string_view>(un_attr)
                                      ? std::get<std::string_view>(un_attr) : std::string_view{};
                if (un == user_name || un == "public") return true;
                auto mem_nodes = u_node->get_in_neighbors("MEMBER_OF", INTERNAL_UID);
                for (auto m_uid : mem_nodes) {
                    auto m_node = engine->get_node(m_uid);
                    if (!m_node) continue;
                    auto mn_attr = m_node->try_get_attr("n"_hk);
                    if (const auto* msv = std::get_if<std::string_view>(&mn_attr); msv && *msv == user_name) {
                        return true;
                    }
                }
            }
        }
        return false;
    }

    const lite3cpp::HashedKey hk(f.key, key_hash);
    const Node::AttrValue attr = node->try_get_attr(hk);
    if (std::holds_alternative<std::monostate>(attr)) {
        if (f.key == "id") {
            char hex_id[17];
            std::snprintf(hex_id, sizeof(hex_id), "%016llx", static_cast<unsigned long long>(node->get_id()));
            return compare_string_filter(std::string_view(hex_id, 16), f, has_i64, f_i64, has_f64, f_f64);
        }
        return false;
    }

    if (const auto* sv = std::get_if<std::string_view>(&attr)) {
        return compare_string_filter(*sv, f, has_i64, f_i64, has_f64, f_f64);
    }

    if (const auto* iv = std::get_if<int64_t>(&attr)) {
        const int64_t lhs = *iv;
        if (has_i64 && f.op != Query::Op::Like && f.op != Query::Op::NotLike) {
            switch (f.op) {
                case Query::Op::Eq: return lhs == f_i64;
                case Query::Op::Ne: return lhs != f_i64;
                case Query::Op::Gt: return lhs > f_i64;
                case Query::Op::Ge: return lhs >= f_i64;
                case Query::Op::Lt: return lhs < f_i64;
                case Query::Op::Le: return lhs <= f_i64;
                default: break;
            }
        }
        if (has_f64 && f.op != Query::Op::Like && f.op != Query::Op::NotLike) {
            const double lhs_d = static_cast<double>(lhs);
            switch (f.op) {
                case Query::Op::Eq: return lhs_d == f_f64;
                case Query::Op::Ne: return lhs_d != f_f64;
                case Query::Op::Gt: return lhs_d > f_f64;
                case Query::Op::Ge: return lhs_d >= f_f64;
                case Query::Op::Lt: return lhs_d < f_f64;
                case Query::Op::Le: return lhs_d <= f_f64;
                default: break;
            }
        }
        char buf[32];
        auto [ptr, ec] = std::to_chars(buf, buf + sizeof(buf), lhs);
        std::string_view num_sv(buf, (ec == std::errc{}) ? static_cast<size_t>(ptr - buf) : 0);
        return compare_string_filter(num_sv, f, has_i64, f_i64, has_f64, f_f64);
    }

    if (const auto* fv = std::get_if<double>(&attr)) {
        const double lhs = *fv;
        if (has_f64 && f.op != Query::Op::Like && f.op != Query::Op::NotLike) {
            switch (f.op) {
                case Query::Op::Eq: return lhs == f_f64;
                case Query::Op::Ne: return lhs != f_f64;
                case Query::Op::Gt: return lhs > f_f64;
                case Query::Op::Ge: return lhs >= f_f64;
                case Query::Op::Lt: return lhs < f_f64;
                case Query::Op::Le: return lhs <= f_f64;
                default: break;
            }
        }
        std::string num_s = std::to_string(lhs);
        return compare_string_filter(num_s, f, has_i64, f_i64, has_f64, f_f64);
    }

    if (const auto* bv = std::get_if<bool>(&attr)) {
        std::string_view b_sv = *bv ? std::string_view("true") : std::string_view("false");
        return compare_string_filter(b_sv, f, has_i64, f_i64, has_f64, f_f64);
    }

    return false;
}

enum class TriBool : uint8_t {
    False = 0,
    True = 1,
    Unknown = 2
};

inline TriBool tb_and(TriBool a, TriBool b) noexcept {
    if (a == TriBool::False || b == TriBool::False) return TriBool::False;
    if (a == TriBool::True && b == TriBool::True) return TriBool::True;
    return TriBool::Unknown;
}

inline TriBool tb_or(TriBool a, TriBool b) noexcept {
    if (a == TriBool::True || b == TriBool::True) return TriBool::True;
    if (a == TriBool::False && b == TriBool::False) return TriBool::False;
    return TriBool::Unknown;
}

template <typename NodeLookupFn>
TriBool evaluate_group_tribool_fn(const Query::FilterGroup& g, NodeLookupFn&& lookup_node, Engine* engine) {
    if (g.nodes.empty()) return TriBool::True;
    TriBool result = TriBool::True;
    bool is_first = true;
    for (const auto& n : g.nodes) {
        TriBool val = std::visit(overloaded{
            [&](const Query::Filter& f) -> TriBool {
                Node* node = lookup_node(f.alias);
                if (node != nullptr) {
                    return evaluate_filter(node, f, engine) ? TriBool::True : TriBool::False;
                }
                return TriBool::Unknown;
            },
            [&](const std::shared_ptr<Query::FilterGroup>& sub) -> TriBool {
                return sub ? evaluate_group_tribool_fn(*sub, lookup_node, engine) : TriBool::True;
            }
        }, n.node);
        if (is_first) {
            result = val;
            is_first = false;
        } else if (n.prepended_op == Query::LogicalOp::Or) {
            result = tb_or(result, val);
        } else {
            result = tb_and(result, val);
        }
    }
    return result;
}

struct AliasTable {
    std::vector<std::string> names;

    size_t intern(std::string_view alias) {
        for (size_t i = 0; i < names.size(); ++i) {
            if (names[i] == alias) return i;
        }
        names.emplace_back(alias);
        return names.size() - 1;
    }

    size_t find(std::string_view alias) const noexcept {
        for (size_t i = 0; i < names.size(); ++i) {
            if (names[i] == alias) return i;
        }
        return SIZE_MAX;
    }
};

struct TraversalPath {
    static constexpr size_t INLINE_SLOTS = 16;
    std::array<std::shared_ptr<Node>, INLINE_SLOTS> inline_nodes{};
    std::vector<std::shared_ptr<Node>> overflow_nodes;
    size_t last_slot = 0;

    const std::shared_ptr<Node>& get(size_t slot) const noexcept {
        static const std::shared_ptr<Node> kNullNode{};
        if (slot < INLINE_SLOTS) return inline_nodes[slot];
        size_t ext = slot - INLINE_SLOTS;
        return ext < overflow_nodes.size() ? overflow_nodes[ext] : kNullNode;
    }

    void set(size_t slot, std::shared_ptr<Node> node) {
        if (slot < INLINE_SLOTS) {
            inline_nodes[slot] = std::move(node);
        } else {
            size_t ext = slot - INLINE_SLOTS;
            if (overflow_nodes.size() <= ext) {
                overflow_nodes.resize(ext + 1);
            }
            overflow_nodes[ext] = std::move(node);
        }
    }
};

bool is_entity_type_match(std::string_view alias, std::string_view et, std::string_view actual_type, bool has_v) noexcept {
    if (!et.empty()) {
        if (alias == "DataObject") return (et == "data_object" || et == "generic");
        if (alias == "Collection" || alias == "ParentCollection") return (et == "collection");
        if (alias == "Resource" || alias == "ChildResource" || alias == "ParentResource") {
            return (et == "resource" || et == "unixfilesystem" || et == "passthru" || et == "replication" ||
                    et == "compound" || et == "load_balanced" || et == "random" || et == "deferred" ||
                    et == "structfile" || et == "s3" || et == "mockarchive");
        }
        if (alias == "User") return (et == "user" || et == "group" || et == "rodsuser" || et == "rodsadmin" || et == "groupadmin" || et == "rodsgroup");
        if (alias == "Group") return (et == "group" || et == "rodsgroup" || et == "user");
        if (alias == "Zone") return (et == "zone" || et == "local" || et == "remote");
        if (alias == "Replica") return (et == "replica");
        if (alias == "Metadata") return (et == "metadata");
        if (alias == "Access" || alias == "CollAccess") return (et == "access" || et == "access_type");
        if (alias == "Rule") return (et == "rule");
        if (alias == "Ticket") return (et == "ticket");
        return icase_eq(alias, et);
    }
    if (alias == "Zone") return (actual_type == "zone" || actual_type == "local" || actual_type == "remote");
    if (alias == "User") return (actual_type == "user" || actual_type == "rodsuser" || actual_type == "rodsadmin" || actual_type == "groupadmin" || actual_type == "rodsgroup");
    if (alias == "Group") return (actual_type == "rodsgroup" || actual_type == "group" || actual_type == "rodsuser" || actual_type == "rodsadmin" || actual_type == "groupadmin" || actual_type == "user");
    if (alias == "Collection" || alias == "ParentCollection") {
        return (!has_v && actual_type != "data_object" && actual_type != "generic" && actual_type != "replica" &&
                actual_type != "resource" && actual_type != "unixfilesystem" && actual_type != "passthru" &&
                actual_type != "replication" && actual_type != "compound" && actual_type != "load_balanced" &&
                actual_type != "random" && actual_type != "deferred" && actual_type != "structfile" &&
                actual_type != "s3" && actual_type != "mockarchive" && actual_type != "user" &&
                actual_type != "rodsuser" && actual_type != "rodsadmin" && actual_type != "groupadmin" &&
                actual_type != "rodsgroup" && actual_type != "zone" && actual_type != "local" &&
                actual_type != "remote" && actual_type != "metadata" && actual_type != "access" &&
                actual_type != "access_type" && actual_type != "rule" && actual_type != "ticket");
    }
    if (alias == "DataObject") return (actual_type == "data_object" || actual_type == "generic");
    if (alias == "Replica") return (actual_type == "replica");
    if (alias == "Resource" || alias == "ChildResource" || alias == "ParentResource") {
        return (actual_type == "resource" || actual_type == "unixfilesystem" || actual_type == "passthru" ||
                actual_type == "replication" || actual_type == "compound" || actual_type == "load_balanced" ||
                actual_type == "random" || actual_type == "deferred" || actual_type == "structfile" ||
                actual_type == "s3" || actual_type == "mockarchive");
    }
    if (alias == "Metadata") return (actual_type == "metadata");
    if (alias == "Access" || alias == "CollAccess") return (actual_type == "access" || actual_type == "access_type");
    if (alias == "Rule") return (actual_type == "rule");
    if (alias == "Ticket") return (actual_type == "ticket");
    return true;
}

bool check_node_entity_type(Node* node, std::string_view alias) {
    using namespace lite3cpp::literals;
    if (alias.empty() || alias == "__root") return true;
    auto et_attr = node->try_get_attr("entity_type"_hk);
    std::string_view et = std::holds_alternative<std::string_view>(et_attr)
                          ? std::get<std::string_view>(et_attr) : std::string_view{};
    auto t_attr = node->try_get_attr("t"_hk);
    std::string_view actual_type = std::holds_alternative<std::string_view>(t_attr)
                                   ? std::get<std::string_view>(t_attr) : std::string_view{};
    bool has_v = !std::holds_alternative<std::monostate>(node->try_get_attr("v"_hk));
    return is_entity_type_match(alias, et, actual_type, has_v);
}

bool format_node_attr_into(Node* node, lite3cpp::HashedKey hk, std::string& out) {
    Node::AttrValue v = node->try_get_attr(hk);
    if (std::holds_alternative<std::monostate>(v)) {
        if (node->get_attribute_type(hk.key) == lite3cpp::Type::Bytes) {
            out = node->get_attribute_as_string(hk.key);
            return !out.empty();
        }
        return false;
    }
    if (const auto* sv = std::get_if<std::string_view>(&v)) {
        out.assign(sv->data(), sv->size());
        return true;
    }
    if (const auto* iv = std::get_if<int64_t>(&v)) {
        char buf[32];
        auto [ptr, ec] = std::to_chars(buf, buf + sizeof(buf), *iv);
        if (ec == std::errc{}) {
            out.assign(buf, static_cast<size_t>(ptr - buf));
        } else {
            out = std::to_string(*iv);
        }
        return true;
    }
    if (const auto* fv = std::get_if<double>(&v)) {
        out = std::to_string(*fv);
        return true;
    }
    if (const auto* bv = std::get_if<bool>(&v)) {
        out = *bv ? "true" : "false";
        return true;
    }
    return false;
}

inline bool parse_hex_id_sv(std::string_view sv, uint64_t& out_id) noexcept {
    if (sv.empty()) return false;
    auto [ptr, ec] = std::from_chars(sv.data(), sv.data() + sv.size(), out_id, 16);
    return ec == std::errc{} && ptr == sv.data() + sv.size();
}

bool has_or_or_like_filter(const Query::FilterGroup& g) {
    for (const auto& n : g.nodes) {
        if (n.prepended_op == Query::LogicalOp::Or) return true;
        if (auto* f = std::get_if<Query::Filter>(&n.node)) {
            if (f->op == Query::Op::Like || f->op == Query::Op::NotLike || f->op == Query::Op::Ne) return true;
        } else if (auto* sub = std::get_if<std::shared_ptr<Query::FilterGroup>>(&n.node)) {
            if (*sub && has_or_or_like_filter(**sub)) return true;
        }
    }
    return false;
}

const Query::Filter* find_first_eq_filter(const Query::FilterGroup& g, std::string_view alias, std::string_view key = "") {
    for (const auto& n : g.nodes) {
        if (auto* f = std::get_if<Query::Filter>(&n.node)) {
            if (f->alias == alias && f->op == Query::Op::Eq && (key.empty() || f->key == key)) return f;
        } else if (auto* sub = std::get_if<std::shared_ptr<Query::FilterGroup>>(&n.node)) {
            if (*sub) {
                if (auto* res = find_first_eq_filter(**sub, alias, key)) return res;
            }
        }
    }
    return nullptr;
}

void serialize_filter_nodes(const Query::FilterGroup& group, lite3cpp::Buffer& buf, size_t arr_ofs, int depth = 0) {
    using namespace lite3cpp::literals;
    if (depth > 32) return;
    for (const auto& n : group.nodes) {
        size_t item_ofs = buf.arr_append_obj(arr_ofs);
        std::string_view prep = (n.prepended_op == Query::LogicalOp::Or) ? "or" : "and";
        buf.set_str(item_ofs, "prepended_op"_hk, prep);
        std::visit(overloaded{
            [&](const Query::Filter& f) {
                buf.set_str(item_ofs, "alias"_hk, f.alias);
                buf.set_str(item_ofs, "key"_hk, f.key);
                buf.set_i64(item_ofs, "op"_hk, static_cast<int64_t>(f.op));
                buf.set_str(item_ofs, "value"_hk, f.value);
            },
            [&](const std::shared_ptr<Query::FilterGroup>& sub) {
                buf.set_bool(item_ofs, "group"_hk, true);
                if (sub) {
                    size_t sub_arr_ofs = buf.set_arr(item_ofs, "filters"_hk);
                    serialize_filter_nodes(*sub, buf, sub_arr_ofs, depth + 1);
                }
            }
        }, n.node);
    }
}

void parse_filter_nodes(const lite3cpp::Buffer& buf, size_t filters_arr_ofs, Query::FilterGroup& group, int depth = 0) {
    using namespace lite3cpp::literals;
    if (depth > 32) return;
    if (filters_arr_ofs + sizeof(lite3cpp::PackedNodeLayout) > buf.size()) return;
    for (auto it = buf.begin(filters_arr_ofs), end = buf.end(filters_arr_ofs); it != end; ++it) {
        if (it->value_type != lite3cpp::Type::Object) continue;
        const size_t item_ofs = it->value_offset + 1;
        if (item_ofs + sizeof(lite3cpp::PackedNodeLayout) > buf.size()) continue;

        if (buf.get_type_noexcept(item_ofs, "group"_hk) != lite3cpp::Type::Invalid) {
            std::string_view grp_prep = buf.try_get_str(item_ofs, "prepended_op"_hk).value_or("and");
            auto sub_ofs_opt = buf.try_get_arr(item_ofs, "filters"_hk);
            auto cb = [&buf, sub_ofs_opt, depth](Query::FilterGroup& sub) {
                if (sub_ofs_opt) {
                    parse_filter_nodes(buf, *sub_ofs_opt, sub, depth + 1);
                }
            };
            if (grp_prep == "or") {
                group.or_where_group(cb);
            } else {
                group.where_group(cb);
            }
        } else {
            auto alias = buf.try_get_str(item_ofs, "alias"_hk);
            auto key = buf.try_get_str(item_ofs, "key"_hk);
            auto val = buf.try_get_str(item_ofs, "value"_hk);
            std::optional<int64_t> op_i64 = buf.try_get_i64(item_ofs, "op"_hk);
            if (!op_i64) {
                if (auto op_f64 = buf.try_get_f64(item_ofs, "op"_hk)) {
                    op_i64 = static_cast<int64_t>(*op_f64);
                }
            }
            if (alias && key && val && op_i64) {
                auto op = static_cast<Query::Op>(*op_i64);
                std::string_view prep = buf.try_get_str(item_ofs, "prepended_op"_hk).value_or("and");
                if (prep == "or") {
                    group.or_where(*alias, *key, op, *val);
                } else {
                    group.where(*alias, *key, op, *val);
                }
            }
        }
    }
}

} // namespace

Query::FilterGroup& Query::FilterGroup::where(std::string_view alias, std::string_view key, Op op, std::string_view value) {
    nodes.push_back({make_filter(alias, key, op, value), LogicalOp::And});
    return *this;
}
Query::FilterGroup& Query::FilterGroup::or_where(std::string_view alias, std::string_view key, Op op, std::string_view value) {
    nodes.push_back({make_filter(alias, key, op, value), LogicalOp::Or});
    return *this;
}
Query::FilterGroup& Query::FilterGroup::where_group(std::function<void(FilterGroup&)> cb) {
    auto sub = std::make_shared<FilterGroup>();
    cb(*sub);
    nodes.push_back({std::move(sub), LogicalOp::And});
    return *this;
}
Query::FilterGroup& Query::FilterGroup::or_where_group(std::function<void(FilterGroup&)> cb) {
    auto sub = std::make_shared<FilterGroup>();
    cb(*sub);
    nodes.push_back({std::move(sub), LogicalOp::Or});
    return *this;
}

Query::Query(Engine *engine) : engine_(engine) {}
Query &Query::match(std::string_view node_alias) { initial_match_ = {std::string(node_alias)}; return *this; }
Query &Query::match_id(uint64_t id, std::string_view alias) {
    initial_match_ = {std::string(alias)};
    char buf[24];
    std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(id));
    root_filters_.where(alias, "id", Op::Eq, buf);
    return *this;
}
Query &Query::match_id(std::string_view uuid, std::string_view alias) {
    initial_match_ = {std::string(alias)};
    root_filters_.where(alias, "id", Op::Eq, uuid);
    return *this;
}
Query &Query::where(std::string_view node_alias, std::string_view key, Op op, std::string_view value) {
    root_filters_.where(node_alias, key, op, value);
    return *this;
}
Query &Query::where_group(std::function<void(FilterGroup&)> cb) { root_filters_.where_group(std::move(cb)); return *this; }
Query &Query::or_where(std::string_view node_alias, std::string_view key, Op op, std::string_view value) {
    root_filters_.or_where(node_alias, key, op, value);
    return *this;
}
Query &Query::or_where_group(std::function<void(FilterGroup&)> cb) { root_filters_.or_where_group(std::move(cb)); return *this; }
Query &Query::where_has(std::string_view alias, std::string_view key, std::string_view value_type) {
    filters_has_.push_back(FilterHas{std::string(alias), std::string(key), std::string(value_type)});
    return *this;
}

Query::OutEdgeBuilder Query::out(std::string_view edge_label, double min_weight) { return OutEdgeBuilder(*this, edge_label, min_weight); }
Query::InEdgeBuilder Query::in(std::string_view edge_label) { return InEdgeBuilder(*this, edge_label); }
Query& Query::OutEdgeBuilder::as(std::string_view dest_alias) {
    q_.steps_.push_back(OutStep{label_, weight_, std::string(dest_alias), q_.current_source_alias_});
    q_.current_source_alias_.clear();
    return q_;
}
Query& Query::InEdgeBuilder::as(std::string_view dest_alias) {
    q_.steps_.push_back(InStep{label_, std::string(dest_alias), q_.current_source_alias_});
    q_.current_source_alias_.clear();
    return q_;
}
Query& Query::return_(std::string_view alias, std::string_view property, AggOp agg, bool distinct) {
    projections_.push_back(ReturnStep{std::string(alias), std::string(property), agg, distinct});
    return *this;
}
Query& Query::order_by(std::string_view alias, std::string_view property, bool ascending) {
    sorts_.push_back(SortStep{std::string(alias), std::string(property), ascending});
    return *this;
}
Query& Query::limit(size_t lim) { limit_ = lim; return *this; }
Query& Query::offset(size_t ofs) { offset_ = ofs; return *this; }
Query& Query::group_by(std::string_view alias, std::string_view property, std::string_view func_name, const std::vector<std::string>& func_args) {
    groups_.push_back(GroupStep{std::string(alias), std::string(property), std::string(func_name), func_args});
    return *this;
}
Query& Query::distinct(bool enable) {
    distinct_ = enable;
    return *this;
}

namespace {

using FilterGroup = Query::FilterGroup;
using Filter = Query::Filter;
using Op = Query::Op;
using LogicalOp = Query::LogicalOp;

void append_index_prefix_ids(l3kv::Engine& store,
                             const std::string& prefix,
                             size_t limit,
                             std::vector<uint64_t>& out) {
    auto idx_entries = store.get_prefix_entries_all_shards(prefix, "", limit);
    out.reserve(out.size() + idx_entries.size());
    for (const auto& [k, v] : idx_entries) {
        if (k.ends_with(":meta")) continue;
        uint64_t parsed_id = 0;
        if (parse_hex_id_sv(v, parsed_id)) {
            out.push_back(parsed_id);
        }
    }
}

void sort_unique_ids(std::vector<uint64_t>& ids) {
    if (ids.size() > 1) {
        std::sort(ids.begin(), ids.end());
        ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    }
}

std::vector<uint64_t> lookup_trigram_candidates(l3kv::Engine& store,
                                                const std::string& alias,
                                                const std::string& pattern,
                                                size_t limit) {
    std::vector<std::string> trigrams;
    std::string segment;
    auto flush_segment = [&]() {
        if (segment.size() >= 3) {
            for (size_t ti = 0; ti + 3 <= segment.size(); ++ti) {
                trigrams.push_back(segment.substr(ti, 3));
            }
        }
        segment.clear();
    };
    for (char c : pattern) {
        if (c == '%' || c == '_') {
            flush_segment();
        } else {
            segment += c;
        }
    }
    flush_segment();
    if (trigrams.empty()) return {};

    std::vector<uint64_t> candidates;
    bool first_tri = true;
    for (const auto& tri : trigrams) {
        std::vector<uint64_t> cur_tri;
        append_index_prefix_ids(store, "idx:" + alias + ":tri:" + tri + ":", limit, cur_tri);
        sort_unique_ids(cur_tri);
        if (first_tri) {
            candidates = std::move(cur_tri);
            first_tri = false;
        } else {
            std::vector<uint64_t> inter;
            inter.reserve(std::min(candidates.size(), cur_tri.size()));
            std::set_intersection(candidates.begin(), candidates.end(),
                                  cur_tri.begin(), cur_tri.end(),
                                  std::back_inserter(inter));
            candidates = std::move(inter);
        }
        if (candidates.empty()) break;
    }
    return candidates;
}

void collect_filter_index_frontier(l3kv::Engine& store,
                                   const FilterGroup& fg,
                                   const std::string& root_alias,
                                   size_t scan_limit,
                                   bool& had_eq_filter,
                                   std::vector<uint64_t>& frontier) {
    for (const auto& n : fg.nodes) {
        if (auto* f = std::get_if<Filter>(&n.node)) {
            if (f->alias != root_alias) continue;
            if (f->op == Op::Eq) {
                if (f->key == "n" || f->key == "path" || f->key == "id") {
                    had_eq_filter = true;
                }
                append_index_prefix_ids(store, "idx:" + f->alias + ":" + f->key + ":" + f->value,
                                        scan_limit, frontier);
            } else if (f->op == Op::Like) {
                size_t first_wc = f->value.find_first_of("%_");
                std::string prefix_literal = (first_wc == std::string::npos) ? f->value : f->value.substr(0, first_wc);
                if (!prefix_literal.empty()) {
                    append_index_prefix_ids(store, "idx:" + f->alias + ":" + f->key + ":" + prefix_literal,
                                            scan_limit, frontier);
                } else {
                    auto tri = lookup_trigram_candidates(store, f->alias, f->value, scan_limit);
                    frontier.insert(frontier.end(), tri.begin(), tri.end());
                }
            }
        } else if (auto* sub = std::get_if<std::shared_ptr<FilterGroup>>(&n.node)) {
            if (*sub && n.prepended_op == LogicalOp::And) {
                collect_filter_index_frontier(store, **sub, root_alias, scan_limit, had_eq_filter, frontier);
            }
        }
    }
}

} // namespace

std::string Query::eval_group_key_value(const GroupStep& g, std::string val) {
    if (g.func_name.empty()) return val;
    if (icase_eq(g.func_name, "LENGTH")) {
        return std::to_string(val.size());
    }
    if ((icase_eq(g.func_name, "SUBSTRING") || icase_eq(g.func_name, "SUBSTR")) && !g.func_args.empty()) {
        int pos = 0;
        auto [p0, ec0] = std::from_chars(g.func_args[0].data(), g.func_args[0].data() + g.func_args[0].size(), pos);
        if (ec0 != std::errc{}) return val;
        int start = std::max(0, pos);
        if (start >= static_cast<int>(val.size())) return {};
        if (g.func_args.size() > 1) {
            int len = 0;
            auto [p1, ec1] = std::from_chars(g.func_args[1].data(), g.func_args[1].data() + g.func_args[1].size(), len);
            return (ec1 == std::errc{} && len > 0)
                       ? val.substr(static_cast<size_t>(start), static_cast<size_t>(len))
                       : std::string{};
        }
        return val.substr(static_cast<size_t>(start));
    }
    return val;
}

std::string Query::compute_numeric_agg(const ReturnStep& p,
                                       const std::vector<ResultRow>& part,
                                       const std::string& full_name,
                                       const std::string& idx_key) {
    std::vector<double> vals;
    vals.reserve(part.size());
    for (const auto& r : part) {
        auto it = r.fields.find(full_name);
        if (it == r.fields.end()) it = r.fields.find(idx_key);
        if (it != r.fields.end() && !it->second.empty()) {
            double dv = 0.0;
            auto [ptr, ec] = std::from_chars(it->second.data(), it->second.data() + it->second.size(), dv);
            if (ec == std::errc{} && ptr == it->second.data() + it->second.size()) {
                vals.push_back(dv);
            }
        }
    }
    if (p.distinct && vals.size() > 1) {
        std::sort(vals.begin(), vals.end());
        vals.erase(std::unique(vals.begin(), vals.end()), vals.end());
    }
    if (vals.empty()) return "0";

    double acc = 0.0;
    if (p.agg == AggOp::Min) {
        acc = *std::min_element(vals.begin(), vals.end());
    } else if (p.agg == AggOp::Max) {
        acc = *std::max_element(vals.begin(), vals.end());
    } else {
        for (double v : vals) acc += v;
        if (p.agg == AggOp::Avg) acc /= static_cast<double>(vals.size());
    }
    if (acc == static_cast<double>(static_cast<int64_t>(acc))) {
        return std::to_string(static_cast<int64_t>(acc));
    }
    return std::to_string(acc);
}

std::vector<ResultRow> Query::apply_group_by_and_aggregations(
    std::vector<ResultRow> results,
    const std::vector<std::string>& invariant_proj_names,
    const std::vector<std::string>& idx_names,
    bool has_agg,
    bool should_populate_fields) const {
    std::vector<std::string> partition_order;
    std::unordered_map<std::string, std::vector<ResultRow>> partitions;
    std::vector<GroupStep> effective_groups = groups_;
    if (effective_groups.empty() && has_agg) {
        for (const auto& p : projections_) {
            if (p.agg != AggOp::None) continue;
            bool exists = std::any_of(effective_groups.begin(), effective_groups.end(), [&](const GroupStep& eg) {
                return eg.alias == p.alias && eg.property == p.property;
            });
            if (!exists) {
                effective_groups.push_back(GroupStep{p.alias, p.property, "", {}});
            }
        }
    }

    if (effective_groups.empty()) {
        partition_order.push_back("ALL");
        partitions["ALL"] = std::move(results);
    } else {
        std::vector<std::string> eff_group_keys(effective_groups.size());
        for (size_t gi = 0; gi < effective_groups.size(); ++gi) {
            eff_group_keys[gi] = effective_groups[gi].alias + "." + effective_groups[gi].property;
        }
        for (auto& row : results) {
            std::string g_key;
            for (size_t gi = 0; gi < effective_groups.size(); ++gi) {
                auto it = row.fields.find(eff_group_keys[gi]);
                std::string val = (it != row.fields.end() ? it->second : "");
                g_key += eval_group_key_value(effective_groups[gi], std::move(val));
                g_key.push_back('|');
            }
            auto [pit, inserted] = partitions.try_emplace(g_key);
            if (inserted) partition_order.push_back(g_key);
            pit->second.push_back(std::move(row));
        }
    }

    std::vector<ResultRow> final_res;
    final_res.reserve(partition_order.size());
    for (const auto& pk : partition_order) {
        auto& part = partitions[pk];
        ResultRow agg_row;
        for (const auto& g : effective_groups) {
            std::string gk = g.alias + "." + g.property;
            if (!part.empty()) {
                if (auto it = part[0].fields.find(gk); it != part[0].fields.end()) {
                    agg_row.fields[gk] = it->second;
                }
            }
        }
        agg_row.projected_values.resize(projections_.size());
        if (!flat_rows_) agg_row.projected_names = invariant_proj_names;

        for (size_t i = 0; i < projections_.size(); ++i) {
            const auto& p = projections_[i];
            const std::string& k = idx_names[i];
            const std::string& full_name = invariant_proj_names[i];
            std::string computed_val;

            if (p.agg == AggOp::None) {
                if (!part.empty()) {
                    auto it = part[0].fields.find(full_name);
                    if (it == part[0].fields.end()) it = part[0].fields.find(k);
                    if (it != part[0].fields.end()) computed_val = it->second;
                }
            } else if (p.agg == AggOp::Count) {
                if (p.distinct) {
                    std::unordered_set<std::string_view> dist_vals;
                    for (const auto& r : part) {
                        auto it = r.fields.find(full_name);
                        if (it == r.fields.end()) it = r.fields.find(k);
                        if (it != r.fields.end() && !it->second.empty()) dist_vals.insert(it->second);
                    }
                    computed_val = std::to_string(dist_vals.size());
                } else {
                    computed_val = std::to_string(part.size());
                }
            } else {
                computed_val = compute_numeric_agg(p, part, full_name, k);
            }

            agg_row.projected_values[i] = computed_val;
            if (should_populate_fields) {
                agg_row.fields["_col_" + std::to_string(i)] = full_name;
                agg_row.fields[k] = computed_val;
                agg_row.fields[full_name] = std::move(computed_val);
            }
        }
        final_res.push_back(std::move(agg_row));
    }
    return final_res;
}

void Query::apply_distinct_sort_limit(
    std::vector<ResultRow>& results,
    const std::vector<std::string>& idx_names,
    const std::vector<std::string>& sort_field_keys) const {
    if (distinct_ && results.size() > 1) {
        std::unordered_set<std::string> seen;
        seen.reserve(results.size());
        std::vector<ResultRow> unique_res;
        unique_res.reserve(results.size());
        for (auto& row : results) {
            std::string key;
            for (size_t i = 0; i < projections_.size(); ++i) {
                if (i < row.projected_values.size() && !row.projected_values[i].empty()) {
                    key.append(row.projected_values[i]);
                } else if (i < idx_names.size()) {
                    auto it = row.fields.find(idx_names[i]);
                    if (it != row.fields.end()) key.append(it->second);
                }
                key.push_back('\x1F');
            }
            if (seen.insert(std::move(key)).second) {
                unique_res.push_back(std::move(row));
            }
        }
        results = std::move(unique_res);
    }

    const size_t start_ofs = offset_.value_or(0);
    if (start_ofs >= results.size()) {
        results.clear();
        return;
    }

    if (!sorts_.empty()) {
        std::vector<size_t> order(results.size());
        std::iota(order.begin(), order.end(), 0);

        auto row_cmp = [&](size_t ia, size_t ib) -> bool {
            const ResultRow& a = results[ia];
            const ResultRow& b = results[ib];
            for (size_t si = 0; si < sorts_.size(); ++si) {
                const auto& k = sort_field_keys[si];
                auto it_a = a.fields.find(k);
                auto it_b = b.fields.find(k);
                std::string_view va = (it_a != a.fields.end()) ? std::string_view(it_a->second) : std::string_view{};
                std::string_view vb = (it_b != b.fields.end()) ? std::string_view(it_b->second) : std::string_view{};
                if (va != vb) {
                    return sorts_[si].ascending ? (va < vb) : (va > vb);
                }
            }
            for (size_t i = 0; i < projections_.size(); ++i) {
                std::string_view va = (i < a.projected_values.size()) ? std::string_view(a.projected_values[i]) : std::string_view{};
                std::string_view vb = (i < b.projected_values.size()) ? std::string_view(b.projected_values[i]) : std::string_view{};
                if (va != vb) return va < vb;
            }
            return ia < ib;
        };

        const size_t end_ofs = limit_ ? std::min(results.size(), start_ofs + *limit_) : results.size();
        if (end_ofs < results.size()) {
            std::partial_sort(order.begin(), order.begin() + static_cast<std::ptrdiff_t>(end_ofs), order.end(), row_cmp);
        } else {
            std::sort(order.begin(), order.end(), row_cmp);
        }

        std::vector<ResultRow> sorted_res;
        sorted_res.reserve(end_ofs - start_ofs);
        for (size_t p = start_ofs; p < end_ofs; ++p) {
            sorted_res.push_back(std::move(results[order[p]]));
        }
        results = std::move(sorted_res);
    } else {
        if (start_ofs > 0) {
            results.erase(results.begin(), results.begin() + static_cast<std::ptrdiff_t>(start_ofs));
        }
        if (limit_ && results.size() > *limit_) {
            results.resize(*limit_);
        }
    }
}

std::vector<ResultRow> Query::execute() {
  using namespace lite3cpp::literals;
  std::vector<ResultRow> results;
  std::vector<uint64_t> frontier;
  const size_t scan_limit = engine_->get_settings().prefix_scan_limit;

  if (!starting_nodes_.empty()) {
      frontier = starting_nodes_;
      sort_unique_ids(frontier);
      if (initial_match_ && (root_alias_.empty() || root_alias_ == "__root")) {
          root_alias_ = initial_match_->alias;
      }
  } else if (!start_index_prefix_.empty()) {
      if (initial_match_) root_alias_ = initial_match_->alias;
      append_index_prefix_ids(*engine_->get_store(), start_index_prefix_, scan_limit, frontier);
      sort_unique_ids(frontier);
  } else {
      if (!initial_match_) return results;
      root_alias_ = initial_match_->alias;
      const bool has_complex = has_or_or_like_filter(root_filters_);
      bool had_eq_filter = false;

      if (auto* f = find_first_eq_filter(root_filters_, root_alias_, "id")) {
          had_eq_filter = true;
          frontier.push_back(engine_->get_resolver().parse_uuid(f->value));
      }
      if (frontier.empty()) {
          collect_filter_index_frontier(*engine_->get_store(), root_filters_, root_alias_,
                                        scan_limit, had_eq_filter, frontier);
      }
      if (frontier.empty()) {
          if (had_eq_filter && !has_complex) return results;
          if (!root_alias_.empty() && root_alias_ != "__root") {
              std::string entity_name = (root_alias_ == "Group" ? "User" : root_alias_);
              append_index_prefix_ids(*engine_->get_store(), "idx:" + entity_name + ":id:", 10000, frontier);
              if (frontier.empty()) {
                  append_index_prefix_ids(*engine_->get_store(), "idx:" + entity_name + ":n:", 10000, frontier);
              }
          }
          if (frontier.empty() &&
              (root_alias_.empty() || root_alias_ == "__root" ||
               engine_->get_settings().fallback_to_full_scan_on_index_miss)) {
              auto keys = engine_->get_store()->get_prefix_keys_all_shards("n:{", "", scan_limit);
              frontier.reserve(keys.size());
              for (const auto& k : keys) {
                  if (!k.starts_with("n:{") || k.ends_with(":meta")) continue;
                  size_t end_pos = k.find('}', 3);
                  uint64_t parsed_id = 0;
                  if (end_pos != std::string::npos && end_pos > 3 &&
                      parse_hex_id_sv(std::string_view(k.data() + 3, end_pos - 3), parsed_id)) {
                      frontier.push_back(parsed_id);
                  }
              }
          }
      }
      sort_unique_ids(frontier);
  }

  if (initial_match_ && root_alias_.empty()) {
      root_alias_ = initial_match_->alias;
  }

  if (frontier.empty()) return results;

  bool has_agg = false;
  for (const auto& p : projections_) {
      if (p.agg != AggOp::None) {
          has_agg = true;
          break;
      }
  }

  const bool needs_fields = (!sorts_.empty() || !groups_.empty() || has_agg);
  const bool should_populate_fields = needs_fields || ((!flat_rows_) && (populate_fields_ || projections_.empty()));

  std::vector<std::string> invariant_proj_names(projections_.size());
  std::vector<lite3cpp::HashedKey> proj_hks(projections_.size());
  for (size_t i = 0; i < projections_.size(); ++i) {
      invariant_proj_names[i] = projections_[i].alias + "." + projections_[i].property;
      proj_hks[i] = lite3cpp::HashedKey(projections_[i].property);
  }

  std::vector<std::string> idx_names;
  if (should_populate_fields) {
      idx_names.resize(projections_.size());
      for (size_t i = 0; i < projections_.size(); ++i) {
          idx_names[i] = "idx_" + std::to_string(i);
      }
  }

  std::vector<std::string> sort_field_keys(sorts_.size());
  std::vector<lite3cpp::HashedKey> sort_hks(sorts_.size());
  for (size_t i = 0; i < sorts_.size(); ++i) {
      sort_field_keys[i] = sorts_[i].alias + "." + sorts_[i].property;
      sort_hks[i] = lite3cpp::HashedKey(sorts_[i].property);
  }

  std::vector<std::string> group_field_keys(groups_.size());
  std::vector<lite3cpp::HashedKey> group_hks(groups_.size());
  for (size_t i = 0; i < groups_.size(); ++i) {
      group_field_keys[i] = groups_[i].alias + "." + groups_[i].property;
      group_hks[i] = lite3cpp::HashedKey(groups_[i].property);
  }

  const bool is_privileged_principal = (principal_id_ == INTERNAL_UID || principal_id_ == ADMIN_UID || principal_id_ == 0);
  auto has_node_read_perm = [&](uint64_t nid) -> bool {
      if (is_privileged_principal) return true;
      std::string_view key = KeyBuilder::node_key(nid);
      auto perm = engine_->get_store()->credentials().check_permission(principal_id_, key);
      return (perm & l3kv::Permission::READ) || (perm & l3kv::Permission::ADMIN);
  };

  auto nodes = engine_->fetch_nodes(frontier, principal_id_);
  if (nodes.empty()) return results;

  const size_t pool_threads = ThreadPool::is_worker_thread()
                              ? 1
                              : std::max<size_t>(1, engine_->get_thread_pool().size());

  if (steps_.empty()) {
      const size_t num_chunks = (nodes.size() < 64 || pool_threads <= 1)
                                ? 1
                                : std::min(pool_threads, nodes.size() / 32);
      std::vector<std::vector<ResultRow>> chunk_results(num_chunks);

      auto run_chunk = [&](size_t c_first, size_t c_last) {
          for (size_t c = c_first; c < c_last; ++c) {
              const size_t start = (nodes.size() * c) / num_chunks;
              const size_t end = (nodes.size() * (c + 1)) / num_chunks;
              auto& local_results = chunk_results[c];
              local_results.reserve(end - start);

              for (size_t idx = start; idx < end; ++idx) {
                  auto& node = nodes[idx];
                  if (!node || !node->is_loaded()) continue;
                  if (!check_node_entity_type(node.get(), root_alias_)) continue;
                  if (!has_node_read_perm(node->get_id())) continue;

                  Node* raw_node = node.get();
                  TriBool tb = evaluate_group_tribool_fn(
                      root_filters_,
                      [&](std::string_view alias) -> Node* {
                          return (alias == root_alias_) ? raw_node : nullptr;
                      },
                      engine_);
                  if (tb == TriBool::False) continue;

                  ResultRow row;
                  if (!flat_rows_) {
                      row.nodes.push_back(node);
                  }
                  row.projected_values.resize(projections_.size());

                  for (size_t i = 0; i < projections_.size(); ++i) {
                      if (projections_[i].alias == root_alias_ || projections_[i].alias.empty() || root_alias_.empty()) {
                          std::string& dst_val = row.projected_values[i];
                          if (format_node_attr_into(raw_node, proj_hks[i], dst_val) && should_populate_fields) {
                              row.fields[invariant_proj_names[i]] = dst_val;
                              row.fields[idx_names[i]] = dst_val;
                          }
                      }
                  }
                  if (needs_fields) {
                      for (size_t si = 0; si < sorts_.size(); ++si) {
                          const auto& s = sorts_[si];
                          if (s.alias == root_alias_ || s.alias.empty() || root_alias_.empty()) {
                              if (!row.fields.contains(sort_field_keys[si])) {
                                  std::string val;
                                  if (format_node_attr_into(raw_node, sort_hks[si], val)) {
                                      row.fields[sort_field_keys[si]] = std::move(val);
                                  }
                              }
                          }
                      }
                      for (size_t gi = 0; gi < groups_.size(); ++gi) {
                          const auto& g = groups_[gi];
                          if (g.alias == root_alias_ || g.alias.empty() || root_alias_.empty()) {
                              if (!row.fields.contains(group_field_keys[gi])) {
                                  std::string val;
                                  if (format_node_attr_into(raw_node, group_hks[gi], val)) {
                                      row.fields[group_field_keys[gi]] = std::move(val);
                                  }
                              }
                          }
                      }
                  }
                  local_results.push_back(std::move(row));
              }
          }
      };

      if (num_chunks == 1) {
          run_chunk(0, 1);
      } else {
          // Dispatch chunk indices across thread pool
          std::vector<std::future<void>> futs;
          futs.reserve(num_chunks - 1);
          for (size_t c = 0; c + 1 < num_chunks; ++c) {
              futs.push_back(engine_->get_thread_pool().enqueue([&, c]() { run_chunk(c, c + 1); }));
          }
          run_chunk(num_chunks - 1, num_chunks);
          for (auto& f : futs) f.get();
      }

      size_t total_rows = 0;
      for (const auto& cr : chunk_results) total_rows += cr.size();
      results.reserve(total_rows);
      for (auto& cr : chunk_results) {
          for (auto& r : cr) {
              results.push_back(std::move(r));
          }
      }
  } else {
      AliasTable aliases;
      const size_t root_slot = aliases.intern(root_alias_);
      struct ResolvedStep {
          bool is_out = true;
          std::string_view label;
          double min_weight = 0.0;
          std::string_view target_alias;
          size_t source_slot = SIZE_MAX; // SIZE_MAX => path.last_slot
          size_t target_slot = 0;
      };
      std::vector<ResolvedStep> resolved_steps;
      resolved_steps.reserve(steps_.size());
      for (const auto& st : steps_) {
          std::visit(overloaded{
              [&](const OutStep& s) {
                  size_t src_slot = s.source_alias.empty() ? SIZE_MAX : aliases.intern(s.source_alias);
                  size_t tgt_slot = aliases.intern(s.target_alias);
                  resolved_steps.push_back(ResolvedStep{true, s.label, s.min_weight, s.target_alias, src_slot, tgt_slot});
              },
              [&](const InStep& s) {
                  size_t src_slot = s.source_alias.empty() ? SIZE_MAX : aliases.intern(s.source_alias);
                  size_t tgt_slot = aliases.intern(s.target_alias);
                  resolved_steps.push_back(ResolvedStep{false, s.label, 0.0, s.target_alias, src_slot, tgt_slot});
              }
          }, st);
      }

      std::vector<size_t> proj_slots(projections_.size());
      for (size_t i = 0; i < projections_.size(); ++i) {
          proj_slots[i] = aliases.find(projections_[i].alias);
      }
      std::vector<size_t> sort_slots(sorts_.size());
      for (size_t i = 0; i < sorts_.size(); ++i) {
          sort_slots[i] = aliases.find(sorts_[i].alias);
      }
      std::vector<size_t> group_slots(groups_.size());
      for (size_t i = 0; i < groups_.size(); ++i) {
          group_slots[i] = aliases.find(groups_[i].alias);
      }

      std::vector<TraversalPath> paths;
      {
          const size_t num_chunks = (nodes.size() < 64 || pool_threads <= 1)
                                    ? 1
                                    : std::min(pool_threads, nodes.size() / 32);
          std::vector<std::vector<TraversalPath>> chunk_paths(num_chunks);
          auto seed_chunk = [&](size_t c_first, size_t c_last) {
              for (size_t c = c_first; c < c_last; ++c) {
                  const size_t start = (nodes.size() * c) / num_chunks;
                  const size_t end = (nodes.size() * (c + 1)) / num_chunks;
                  auto& local_paths = chunk_paths[c];
                  for (size_t idx = start; idx < end; ++idx) {
                      auto& node = nodes[idx];
                      if (!node || !node->is_loaded()) continue;
                      if (!check_node_entity_type(node.get(), root_alias_)) continue;
                      if (!has_node_read_perm(node->get_id())) continue;

                      Node* raw_node = node.get();
                      TriBool tb = evaluate_group_tribool_fn(
                          root_filters_,
                          [&](std::string_view alias) -> Node* {
                              return (alias == root_alias_) ? raw_node : nullptr;
                          },
                          engine_);
                      if (tb != TriBool::False) {
                          TraversalPath p;
                          p.set(root_slot, node);
                          p.last_slot = root_slot;
                          local_paths.push_back(std::move(p));
                      }
                  }
              }
          };

          if (num_chunks == 1) {
              seed_chunk(0, 1);
          } else {
              std::vector<std::future<void>> futs;
              futs.reserve(num_chunks - 1);
              for (size_t c = 0; c + 1 < num_chunks; ++c) {
                  futs.push_back(engine_->get_thread_pool().enqueue([&, c]() { seed_chunk(c, c + 1); }));
              }
              seed_chunk(num_chunks - 1, num_chunks);
              for (auto& f : futs) f.get();
          }

          size_t total_paths = 0;
          for (const auto& cp : chunk_paths) total_paths += cp.size();
          paths.reserve(total_paths);
          for (auto& cp : chunk_paths) {
              for (auto& p : cp) paths.push_back(std::move(p));
          }
      }

      if (paths.empty()) return results;

      using SuspendedItem = std::pair<uint64_t, std::pair<std::string, std::vector<Step>>>;
      std::unordered_map<uint16_t, std::vector<SuspendedItem>> suspended_branches;

      for (size_t i = 0; i < resolved_steps.size(); ++i) {
          const auto& rstep = resolved_steps[i];
          const size_t num_chunks = (paths.size() < 64 || pool_threads <= 1)
                                    ? 1
                                    : std::min(pool_threads, paths.size() / 32);
          std::vector<std::vector<TraversalPath>> chunk_next(num_chunks);
          std::vector<std::unordered_map<uint16_t, std::vector<SuspendedItem>>> chunk_susp(num_chunks);

          auto step_chunk = [&](size_t c_first, size_t c_last) {
              std::vector<uint64_t> neighbors;
              for (size_t c = c_first; c < c_last; ++c) {
                  const size_t start = (paths.size() * c) / num_chunks;
                  const size_t end = (paths.size() * (c + 1)) / num_chunks;
                  auto& local_next = chunk_next[c];
                  auto& local_suspended = chunk_susp[c];

                  for (size_t p_idx = start; p_idx < end; ++p_idx) {
                      const auto& path = paths[p_idx];
                      const size_t src_slot = (rstep.source_slot == SIZE_MAX) ? path.last_slot : rstep.source_slot;
                      const auto& src_node = path.get(src_slot);
                      if (!src_node) continue;

                      if (rstep.is_out) {
                          src_node->get_neighbors_into(neighbors, rstep.label, rstep.min_weight, principal_id_);
                      } else {
                          src_node->get_in_neighbors_into(neighbors, rstep.label, principal_id_);
                      }
                      if (neighbors.size() > 1) {
                          std::sort(neighbors.begin(), neighbors.end());
                          neighbors.erase(std::unique(neighbors.begin(), neighbors.end()), neighbors.end());
                      }

                      for (uint64_t neighbor_id : neighbors) {
                          uint16_t cluster_id = FederationID::get_cluster(neighbor_id);
                          if (!engine_->get_resolver().is_local_cluster(cluster_id)) {
                              std::vector<Step> remaining(steps_.begin() + static_cast<std::ptrdiff_t>(i + 1), steps_.end());
                              local_suspended[cluster_id].push_back({neighbor_id, {std::string(rstep.target_alias), std::move(remaining)}});
                              continue;
                          }
                          auto neighbor_node = engine_->get_node(neighbor_id);
                          if (!neighbor_node) continue;
                          if (!check_node_entity_type(neighbor_node.get(), rstep.target_alias)) continue;

                          TraversalPath new_path = path;
                          new_path.set(rstep.target_slot, neighbor_node);
                          new_path.last_slot = rstep.target_slot;

                          TriBool tb = evaluate_group_tribool_fn(
                              root_filters_,
                              [&](std::string_view alias) -> Node* {
                                  size_t s_idx = aliases.find(alias);
                                  if (s_idx == SIZE_MAX) return nullptr;
                                  return new_path.get(s_idx).get();
                              },
                              engine_);
                          if (tb != TriBool::False) {
                              local_next.push_back(std::move(new_path));
                          }
                      }
                  }
              }
          };

          if (num_chunks == 1) {
              step_chunk(0, 1);
          } else {
              engine_->get_thread_pool().parallel_for(0, num_chunks, step_chunk);
          }

          size_t total_next = 0;
          for (const auto& cn : chunk_next) total_next += cn.size();
          std::vector<TraversalPath> next_paths;
          next_paths.reserve(total_next);
          for (auto& cn : chunk_next) {
              for (auto& p : cn) next_paths.push_back(std::move(p));
          }
          for (auto& cs : chunk_susp) {
              for (auto& [cid, branches] : cs) {
                  auto& target = suspended_branches[cid];
                  target.insert(target.end(), std::make_move_iterator(branches.begin()), std::make_move_iterator(branches.end()));
              }
          }
          paths = std::move(next_paths);
          if (paths.empty() && suspended_branches.empty()) break;
      }

      std::vector<std::future<std::vector<ResultRow>>> remote_futures;
      for (auto& [cluster_id, branches] : suspended_branches) {
          struct QueryGroup {
              lite3cpp::Buffer buf;
              std::vector<uint64_t> nodes;
          };
          std::unordered_map<std::string, QueryGroup> qgroups;
          for (auto& b : branches) {
              lite3cpp::Buffer sub_buf;
              sub_buf.init_object();
              sub_buf.set_str(0, "root_alias"_hk, b.second.first);
              sub_buf.set_i64(0, "principal_id"_hk, static_cast<int64_t>(principal_id_));
              size_t steps_ofs = sub_buf.set_arr(0, "steps"_hk);
              for (const auto& step : b.second.second) {
                  std::visit(overloaded{
                      [&](const OutStep& s) {
                          size_t s_ofs = sub_buf.arr_append_obj(steps_ofs);
                          sub_buf.set_str(s_ofs, "type"_hk, "out");
                          sub_buf.set_str(s_ofs, "label"_hk, s.label);
                          sub_buf.set_f64(s_ofs, "min_weight"_hk, s.min_weight);
                          sub_buf.set_str(s_ofs, "target_alias"_hk, s.target_alias);
                          if (!s.source_alias.empty()) {
                              sub_buf.set_str(s_ofs, "source_alias"_hk, s.source_alias);
                          }
                      },
                      [&](const InStep& s) {
                          size_t s_ofs = sub_buf.arr_append_obj(steps_ofs);
                          sub_buf.set_str(s_ofs, "type"_hk, "in");
                          sub_buf.set_str(s_ofs, "label"_hk, s.label);
                          sub_buf.set_str(s_ofs, "target_alias"_hk, s.target_alias);
                          if (!s.source_alias.empty()) {
                              sub_buf.set_str(s_ofs, "source_alias"_hk, s.source_alias);
                          }
                      }
                  }, step);
              }
              size_t projs_ofs = sub_buf.set_arr(0, "projections"_hk);
              for (const auto& p : projections_) {
                  size_t p_ofs = sub_buf.arr_append_obj(projs_ofs);
                  sub_buf.set_str(p_ofs, "alias"_hk, p.alias);
                  sub_buf.set_str(p_ofs, "property"_hk, p.property);
                  sub_buf.set_i64(p_ofs, "agg"_hk, static_cast<int64_t>(p.agg));
                  sub_buf.set_bool(p_ofs, "distinct"_hk, p.distinct);
              }
              if (!root_filters_.nodes.empty()) {
                  size_t filters_ofs = sub_buf.set_arr(0, "filters"_hk);
                  serialize_filter_nodes(root_filters_, sub_buf, filters_ofs);
              }
              std::string query_bytes(reinterpret_cast<const char*>(sub_buf.data()), sub_buf.size());
              auto it = qgroups.find(query_bytes);
              if (it == qgroups.end()) {
                  qgroups.emplace(std::move(query_bytes), QueryGroup{std::move(sub_buf), {b.first}});
              } else {
                  it->second.nodes.push_back(b.first);
              }
          }
          for (auto& [_, qg] : qgroups) {
              remote_futures.push_back(engine_->get_remote_client().resume_query_async(cluster_id, qg.nodes, qg.buf, principal_id_));
          }
      }

      results.reserve(paths.size());
      for (const auto& path : paths) {
          TriBool tb = evaluate_group_tribool_fn(
              root_filters_,
              [&](std::string_view alias) -> Node* {
                  size_t s_idx = aliases.find(alias);
                  if (s_idx == SIZE_MAX) return nullptr;
                  return path.get(s_idx).get();
              },
              engine_);
          if (is_federated_branch_ ? (tb == TriBool::False) : (tb != TriBool::True)) {
              continue;
          }
          ResultRow row;
          if (!flat_rows_) {
              for (size_t s_idx = 0; s_idx < aliases.names.size(); ++s_idx) {
                  const auto& n = path.get(s_idx);
                  if (n) row.nodes.push_back(n);
              }
          }
          row.projected_values.resize(projections_.size());
          for (size_t i = 0; i < projections_.size(); ++i) {
              size_t ps = proj_slots[i];
              if (ps == SIZE_MAX) continue;
              const auto& n = path.get(ps);
              if (!n) continue;
              std::string& dst_val = row.projected_values[i];
              if (format_node_attr_into(n.get(), proj_hks[i], dst_val) && should_populate_fields) {
                  row.fields[invariant_proj_names[i]] = dst_val;
                  row.fields[idx_names[i]] = dst_val;
              }
          }
          if (needs_fields) {
              for (size_t si = 0; si < sorts_.size(); ++si) {
                  size_t ss = sort_slots[si];
                  if (ss == SIZE_MAX) continue;
                  const auto& n = path.get(ss);
                  if (n && !row.fields.contains(sort_field_keys[si])) {
                      std::string val;
                      if (format_node_attr_into(n.get(), sort_hks[si], val)) {
                          row.fields[sort_field_keys[si]] = std::move(val);
                      }
                  }
              }
              for (size_t gi = 0; gi < groups_.size(); ++gi) {
                  size_t gs = group_slots[gi];
                  if (gs == SIZE_MAX) continue;
                  const auto& n = path.get(gs);
                  if (n && !row.fields.contains(group_field_keys[gi])) {
                      std::string val;
                      if (format_node_attr_into(n.get(), group_hks[gi], val)) {
                          row.fields[group_field_keys[gi]] = std::move(val);
                      }
                  }
              }
          }
          results.push_back(std::move(row));
      }

      for (auto& f : remote_futures) {
          auto remote_res = f.get();
          results.insert(results.end(),
                         std::make_move_iterator(remote_res.begin()),
                         std::make_move_iterator(remote_res.end()));
      }
  }

  if (has_agg || !groups_.empty()) {
      results = apply_group_by_and_aggregations(
          std::move(results), invariant_proj_names, idx_names, has_agg, should_populate_fields);
  }

  apply_distinct_sort_limit(results, idx_names, sort_field_keys);

  if (!flat_rows_ && !results.empty() && results[0].projected_names.empty() && !invariant_proj_names.empty()) {
      results[0].projected_names = std::move(invariant_proj_names);
  }
  return results;
}

lite3cpp::Buffer Query::serialize_results(const std::vector<ResultRow>& rows) {
    using namespace lite3cpp::literals;
    lite3cpp::Buffer out_buf;
    size_t num_proj = rows.empty() ? 0 : rows[0].projected_values.size();
    size_t est_row_bytes = 2 * lite3cpp::config::node_size + num_proj * 48;
    out_buf.ensure_capacity(rows.size() * est_row_bytes + 512);
    out_buf.init_array();
    bool wrote_cols = false;
    for (const auto& row : rows) {
        size_t row_ofs = out_buf.arr_append_obj(0);
        if (!row.projected_values.empty()) {
            if (!wrote_cols && !row.projected_names.empty()) {
                size_t cols_ofs = out_buf.set_arr(row_ofs, "cols"_hk);
                for (const auto& cn : row.projected_names) {
                    out_buf.arr_append_str(cols_ofs, cn);
                }
                wrote_cols = true;
            }
            size_t proj_ofs = out_buf.set_arr(row_ofs, "proj"_hk);
            for (const auto& pv : row.projected_values) {
                out_buf.arr_append_str(proj_ofs, pv);
            }
        } else {
            size_t fields_ofs = out_buf.set_obj(row_ofs, "fields"_hk);
            for (const auto& [k, v] : row.fields) {
                out_buf.set_str(fields_ofs, lite3cpp::HashedKey(k), v);
            }
        }
    }
    return out_buf;
}

lite3cpp::Buffer Query::serialize_flat_results(const std::vector<ResultRow>& rows) {
    lite3cpp::Buffer out_buf;
    size_t num_proj = rows.empty() ? 0 : rows[0].projected_values.size();
    size_t est_row_bytes = lite3cpp::config::node_size + num_proj * 36;
    out_buf.ensure_capacity(rows.size() * est_row_bytes + 256);
    out_buf.init_array();
    for (const auto& row : rows) {
        size_t r_ofs = out_buf.arr_append_arr(0);
        for (const auto& pv : row.projected_values) {
            out_buf.arr_append_str(r_ofs, pv);
        }
    }
    return out_buf;
}

lite3cpp::Buffer Query::execute_to_flat_buffer() {
    const bool prev_flat = flat_rows_;
    const bool prev_pop = populate_fields_;
    flat_rows_ = true;
    populate_fields_ = false;
    auto rows = execute();
    flat_rows_ = prev_flat;
    populate_fields_ = prev_pop;
    return serialize_flat_results(rows);
}

lite3cpp::Buffer Query::execute_to_buffer() {
    if (flat_rows_) {
        return execute_to_flat_buffer();
    }
    auto rows = execute();
    return serialize_results(rows);
}

Query &Query::resume(const std::vector<uint64_t>& starting_nodes, const lite3cpp::Buffer& query_buf) {
    using namespace lite3cpp::literals;
    starting_nodes_ = starting_nodes;
    is_federated_branch_ = true;

    if (query_buf.size() < sizeof(lite3cpp::PackedNodeLayout)) return *this;
    lite3cpp::NodeView root_node(reinterpret_cast<const lite3cpp::PackedNodeLayout*>(query_buf.data()));
    if (root_node.type() != lite3cpp::Type::Object) return *this;

    if (auto ra = query_buf.try_get_str(0, "root_alias"_hk)) {
        root_alias_.assign(ra->data(), ra->size());
        initial_match_ = {root_alias_};
    }
    if (auto pid = query_buf.try_get_i64(0, "principal_id"_hk)) {
        principal_id_ = static_cast<uint32_t>(*pid);
    }
    if (auto sip = query_buf.try_get_str(0, "start_index_prefix"_hk)) {
        start_index_prefix_.assign(sip->data(), sip->size());
    }
    if (auto fr = query_buf.try_get_bool(0, "flat_rows"_hk)) {
        flat_rows_ = *fr;
    }

    if (auto steps_ofs_opt = query_buf.try_get_arr(0, "steps"_hk)) {
        const size_t steps_ofs = *steps_ofs_opt;
        for (auto it = query_buf.begin(steps_ofs), end = query_buf.end(steps_ofs); it != end; ++it) {
            if (it->value_type != lite3cpp::Type::Object) continue;
            const size_t s_ofs = it->value_offset + 1;
            if (s_ofs + sizeof(lite3cpp::PackedNodeLayout) > query_buf.size()) continue;

            std::string_view type = query_buf.try_get_str(s_ofs, "type"_hk).value_or("");
            std::string src(query_buf.try_get_str(s_ofs, "source_alias"_hk).value_or(""));
            std::string label(query_buf.try_get_str(s_ofs, "label"_hk).value_or(""));
            std::string target(query_buf.try_get_str(s_ofs, "target_alias"_hk).value_or(""));
            if (type == "out") {
                double min_w = 0.0;
                if (auto mw = query_buf.try_get_f64(s_ofs, "min_weight"_hk)) {
                    min_w = *mw;
                } else if (auto mwi = query_buf.try_get_i64(s_ofs, "min_weight"_hk)) {
                    min_w = static_cast<double>(*mwi);
                }
                steps_.push_back(OutStep{std::move(label), min_w, std::move(target), std::move(src)});
            } else if (type == "in") {
                steps_.push_back(InStep{std::move(label), std::move(target), std::move(src)});
            }
        }
    }

    if (auto projs_ofs_opt = query_buf.try_get_arr(0, "projections"_hk)) {
        const size_t projs_ofs = *projs_ofs_opt;
        for (auto it = query_buf.begin(projs_ofs), end = query_buf.end(projs_ofs); it != end; ++it) {
            if (it->value_type != lite3cpp::Type::Object) continue;
            const size_t p_ofs = it->value_offset + 1;
            if (p_ofs + sizeof(lite3cpp::PackedNodeLayout) > query_buf.size()) continue;

            std::string alias(query_buf.try_get_str(p_ofs, "alias"_hk).value_or(""));
            std::string prop(query_buf.try_get_str(p_ofs, "property"_hk).value_or(""));
            int agg = static_cast<int>(query_buf.try_get_i64(p_ofs, "agg"_hk).value_or(0));
            bool dist = query_buf.try_get_bool(p_ofs, "distinct"_hk).value_or(false);
            projections_.push_back(ReturnStep{std::move(alias), std::move(prop), static_cast<AggOp>(agg), dist});
        }
    }

    if (auto groups_ofs_opt = query_buf.try_get_arr(0, "groups"_hk)) {
        const size_t groups_ofs = *groups_ofs_opt;
        for (auto it = query_buf.begin(groups_ofs), end = query_buf.end(groups_ofs); it != end; ++it) {
            if (it->value_type != lite3cpp::Type::Object) continue;
            const size_t g_ofs = it->value_offset + 1;
            if (g_ofs + sizeof(lite3cpp::PackedNodeLayout) > query_buf.size()) continue;

            std::string alias(query_buf.try_get_str(g_ofs, "alias"_hk).value_or(""));
            std::string prop(query_buf.try_get_str(g_ofs, "property"_hk).value_or(""));
            std::string func_name(query_buf.try_get_str(g_ofs, "func_name"_hk).value_or(""));
            std::vector<std::string> func_args;
            if (auto fa_ofs_opt = query_buf.try_get_arr(g_ofs, "func_args"_hk)) {
                const size_t fa_ofs = *fa_ofs_opt;
                for (auto fit = query_buf.begin(fa_ofs), fend = query_buf.end(fa_ofs); fit != fend; ++fit) {
                    if (fit->value_type == lite3cpp::Type::String && fit->value_offset + 5 <= query_buf.size()) {
                        uint32_t sz = 0;
                        std::memcpy(&sz, query_buf.data() + fit->value_offset + 1, 4);
                        if (fit->value_offset + 5 + sz <= query_buf.size()) {
                            func_args.emplace_back(
                                reinterpret_cast<const char*>(query_buf.data() + fit->value_offset + 5), sz);
                        }
                    }
                }
            }
            groups_.push_back(GroupStep{std::move(alias), std::move(prop), std::move(func_name), std::move(func_args)});
        }
    }

    if (auto sorts_ofs_opt = query_buf.try_get_arr(0, "sorts"_hk)) {
        const size_t sorts_ofs = *sorts_ofs_opt;
        for (auto it = query_buf.begin(sorts_ofs), end = query_buf.end(sorts_ofs); it != end; ++it) {
            if (it->value_type != lite3cpp::Type::Object) continue;
            const size_t s_ofs = it->value_offset + 1;
            if (s_ofs + sizeof(lite3cpp::PackedNodeLayout) > query_buf.size()) continue;

            std::string alias(query_buf.try_get_str(s_ofs, "alias"_hk).value_or(""));
            std::string prop(query_buf.try_get_str(s_ofs, "property"_hk).value_or(""));
            bool asc = query_buf.try_get_bool(s_ofs, "ascending"_hk).value_or(true);
            sorts_.push_back(SortStep{std::move(alias), std::move(prop), asc});
        }
    }

    if (auto lim = query_buf.try_get_i64(0, "limit"_hk)) {
        limit_ = static_cast<size_t>(*lim);
    }
    if (auto ofs = query_buf.try_get_i64(0, "offset"_hk)) {
        offset_ = static_cast<size_t>(*ofs);
    }
    if (auto dist = query_buf.try_get_bool(0, "distinct"_hk)) {
        distinct_ = *dist;
    }
    if (auto filters_ofs = query_buf.try_get_arr(0, "filters"_hk)) {
        parse_filter_nodes(query_buf, *filters_ofs, root_filters_);
    }

    return *this;
}

Query &Query::resume(const std::vector<uint64_t>& starting_nodes, std::string_view query_payload) {
    if (query_payload.size() >= sizeof(lite3cpp::PackedNodeLayout) &&
        (static_cast<uint8_t>(query_payload[0]) == 0x06 || static_cast<uint8_t>(query_payload[0]) == 0x07)) {
        const uint8_t* ptr = reinterpret_cast<const uint8_t*>(query_payload.data());
        lite3cpp::Buffer buf = lite3cpp::Buffer::from_view(ptr, query_payload.size());
        return resume(starting_nodes, buf);
    }
    lite3cpp::Buffer buf = lite3cpp::lite3_json::from_json_string(query_payload.empty() ? "{}" : query_payload);
    return resume(starting_nodes, buf);
}

} // namespace l3kvg
