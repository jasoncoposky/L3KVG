#include "L3KVG/Query.hpp"
#include "L3KVG/Engine.hpp"
#include "L3KVG/Node.hpp"
#include "L3KVG/Edge.hpp"
#include "L3KVG/FederationID.hpp"
#include <iostream>
#include <iomanip>
#include <unordered_set>
#include <set>
#include <algorithm>
#include <mutex>
#include <sstream>
#include <regex>
#include "engine/store.hpp"

#include <cstdio>
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

static bool evaluate_filter(Node* node, const Query::Filter& f, Engine* engine) {
    if (f.key == "id") {
        if (node->has_attribute("id")) {
            std::string s_val;
            try { s_val = node->get_attribute_as_string("id"); } catch (...) {}
            if (!s_val.empty()) {
                if (f.op == Query::Op::Eq) return (s_val == f.value);
                if (f.op == Query::Op::Ne) return (s_val != f.value);
            }
        }
        uint64_t target_id = engine->get_resolver().parse_uuid(f.value);
        uint64_t node_id = node->get_id();
        bool res = false;
        switch (f.op) {
            case Query::Op::Eq: res = (node_id == target_id); break;
            case Query::Op::Ne: res = (node_id != target_id); break;
            default: res = false;
        }
        return res;
    }

    if (f.key == "_access_user") {
        std::string user_name = f.value;
        if (user_name.empty()) return true;

        // 1. Check if node is owned by the user
        std::string owner;
        try { owner = node->get_attribute_as_string("o"); } catch (...) {}
        if (!owner.empty() && owner == user_name) return true;

        // 2. Check system root collections accessible to all users
        std::string p;
        try { p = node->get_attribute_as_string("p"); } catch (...) {}
        if (p.empty()) try { p = node->get_attribute_as_string("n"); } catch (...) {}
        bool is_system_root = (p == "/");
        if (!is_system_root && !p.empty() && p[0] == '/') {
            size_t second_slash = p.find('/', 1);
            if (second_slash == std::string::npos) {
                is_system_root = true;
            } else {
                std::string_view sub(p.data() + second_slash, p.size() - second_slash);
                if (sub == "/home" || sub == "/trash" || sub == "/trash/home" ||
                    sub == "/home/public" || sub == "/trash/home/public") {
                    is_system_root = true;
                }
            }
        }
        if (is_system_root) {
            return true;
        }

        static auto perm_rank = [](std::string_view lvl) -> int {
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
            a_node->ensure_loaded();
            if (!a_node->is_loaded()) continue;
            std::string level;
            try { level = a_node->get_attribute_as_string("l"); } catch (...) {}
            if (perm_rank(level) < 1050) continue;
            
            auto user_nodes = a_node->get_in_neighbors("HAS_ACCESS", INTERNAL_UID);
            for (auto uid : user_nodes) {
                auto u_node = engine->get_node(uid);
                if (!u_node) continue;
                u_node->ensure_loaded();
                if (!u_node->is_loaded()) continue;
                std::string un;
                try { un = u_node->get_attribute_as_string("n"); } catch (...) {}
                if (un == user_name || un == "public") return true;
                auto mem_nodes = u_node->get_in_neighbors("MEMBER_OF", INTERNAL_UID);
                for (auto m_uid : mem_nodes) {
                    auto m_node = engine->get_node(m_uid);
                    if (m_node) {
                        m_node->ensure_loaded();
                        if (m_node->is_loaded()) {
                            std::string mn;
                            try { mn = m_node->get_attribute_as_string("n"); } catch (...) {}
                            if (mn == user_name) return true;
                        }
                    }
                }
            }
        }
        return false;
    }

    L3_LOG(0, "evaluate_filter() key=%s, val=%s, op=%d", f.key.c_str(), f.value.c_str(), (int)f.op);
    if (!node->has_attribute(f.key)) {
        L3_LOG(0, "evaluate_filter() node %016llx does NOT have attribute '%s'", (unsigned long long)node->get_id(), f.key.c_str());
        return false;
    }

    std::string s_val;
    try { s_val = node->get_attribute_as_string(f.key); } catch (...) { 
        L3_LOG(0, "evaluate_filter() node %016llx failed to get attribute '%s' as string", (unsigned long long)node->get_id(), f.key.c_str());
        return false; 
    }

    bool res = false;
    auto type = node->get_attribute_type(f.key);
    switch (f.op) {
        case Query::Op::Eq: 
            res = (s_val == f.value); 
            L3_LOG(0, "evaluate_filter() [Filter] %s.%s: '%s' == '%s' ? %s", f.alias.c_str(), f.key.c_str(), s_val.c_str(), f.value.c_str(), res ? "YES" : "NO");
            break;
        case Query::Op::Ne: res = (s_val != f.value); break;
        case Query::Op::Gt: {
            bool compared = false;
            try {
                size_t p1 = 0, p2 = 0;
                long long v1 = std::stoll(s_val, &p1);
                long long v2 = std::stoll(f.value, &p2);
                if (p1 == s_val.size() && p2 == f.value.size()) {
                    res = (v1 > v2);
                    compared = true;
                }
            } catch (...) {}
            if (!compared) {
                if (type == lite3cpp::Type::Float64) { try { res = (std::stod(s_val) > std::stod(f.value)); } catch(...) { res = false; } }
                else res = (s_val > f.value);
            }
            #ifdef IRODS_SERVER
            rodsLog(LOG_NOTICE, "[Filter] %s.%s: '%s' > '%s' ? %s", f.alias.c_str(), f.key.c_str(), s_val.c_str(), f.value.c_str(), res ? "YES" : "NO");
            #endif
            break;
        }
        case Query::Op::Ge: {
            bool compared = false;
            try {
                size_t p1 = 0, p2 = 0;
                long long v1 = std::stoll(s_val, &p1);
                long long v2 = std::stoll(f.value, &p2);
                if (p1 == s_val.size() && p2 == f.value.size()) {
                    res = (v1 >= v2);
                    compared = true;
                }
            } catch (...) {}
            if (!compared) {
                if (type == lite3cpp::Type::Float64) { try { res = (std::stod(s_val) >= std::stod(f.value)); } catch(...) { res = false; } }
                else res = (s_val >= f.value);
            }
            #ifdef IRODS_SERVER
            rodsLog(LOG_NOTICE, "[Filter] %s.%s: '%s' >= '%s' ? %s", f.alias.c_str(), f.key.c_str(), s_val.c_str(), f.value.c_str(), res ? "YES" : "NO");
            #endif
            break;
        }
        case Query::Op::Lt: {
            bool compared = false;
            try {
                size_t p1 = 0, p2 = 0;
                long long v1 = std::stoll(s_val, &p1);
                long long v2 = std::stoll(f.value, &p2);
                if (p1 == s_val.size() && p2 == f.value.size()) {
                    res = (v1 < v2);
                    compared = true;
                }
            } catch (...) {}
            if (!compared) {
                if (type == lite3cpp::Type::Float64) { try { res = (std::stod(s_val) < std::stod(f.value)); } catch(...) { res = false; } }
                else res = (s_val < f.value);
            }
            #ifdef IRODS_SERVER
            rodsLog(LOG_NOTICE, "[Filter] %s.%s: '%s' < '%s' ? %s", f.alias.c_str(), f.key.c_str(), s_val.c_str(), f.value.c_str(), res ? "YES" : "NO");
            #endif
            break;
        }
        case Query::Op::Le: {
            bool compared = false;
            try {
                size_t p1 = 0, p2 = 0;
                long long v1 = std::stoll(s_val, &p1);
                long long v2 = std::stoll(f.value, &p2);
                if (p1 == s_val.size() && p2 == f.value.size()) {
                    res = (v1 <= v2);
                    compared = true;
                }
            } catch (...) {}
            if (!compared) {
                if (type == lite3cpp::Type::Float64) { try { res = (std::stod(s_val) <= std::stod(f.value)); } catch(...) { res = false; } }
                else res = (s_val <= f.value);
            }
            #ifdef IRODS_SERVER
            rodsLog(LOG_NOTICE, "[Filter] %s.%s: '%s' <= '%s' ? %s", f.alias.c_str(), f.key.c_str(), s_val.c_str(), f.value.c_str(), res ? "YES" : "NO");
            #endif
            break;
        }
        case Query::Op::Like:
        case Query::Op::NotLike: {
            bool matches = false;
            bool has_underscore = (f.value.find('_') != std::string::npos);
            size_t first_pct = f.value.find('%');
            size_t last_pct = f.value.rfind('%');

            if (!has_underscore) {
                if (first_pct == std::string::npos) {
                    // Exact match (case-insensitive)
                    matches = (strcasecmp(s_val.c_str(), f.value.c_str()) == 0);
                } else if (first_pct == f.value.size() - 1 && last_pct == first_pct) {
                    // Prefix match: "prefix%"
                    std::string_view pfx(f.value.data(), first_pct);
                    matches = (s_val.size() >= pfx.size() && strncasecmp(s_val.data(), pfx.data(), pfx.size()) == 0);
                } else if (first_pct == 0 && last_pct == 0) {
                    // Suffix match: "%suffix"
                    std::string_view sfx(f.value.data() + 1, f.value.size() - 1);
                    matches = (s_val.size() >= sfx.size() && strncasecmp(s_val.data() + s_val.size() - sfx.size(), sfx.data(), sfx.size()) == 0);
                } else if (first_pct == 0 && last_pct == f.value.size() - 1 && f.value.size() >= 2) {
                    // Substring match: "%substr%"
                    size_t mid_pct = f.value.find('%', 1);
                    if (mid_pct == last_pct) {
                        std::string sub = f.value.substr(1, last_pct - 1);
                        auto it = std::search(s_val.begin(), s_val.end(), sub.begin(), sub.end(),
                                              [](char a, char b) { return ::tolower(a) == ::tolower(b); });
                        matches = (it != s_val.end());
                    }
                }
            }

            if (!matches && (has_underscore || (first_pct != std::string::npos && !(first_pct == f.value.size() - 1 && last_pct == first_pct)))) {
                thread_local static std::unordered_map<std::string, std::regex> s_regex_cache;
                auto it = s_regex_cache.find(f.value);
                if (it == s_regex_cache.end()) {
                    std::string regex_str = "^";
                    for (char c : f.value) {
                        if (c == '%') regex_str += ".*";
                        else if (c == '_') regex_str += ".";
                        else if (c == '.' || c == '*' || c == '+' || c == '?' || c == '(' || c == ')' || c == '[' || c == ']' || c == '{' || c == '}' || c == '|') { regex_str += "\\"; regex_str += c; }
                        else regex_str += c;
                    }
                    regex_str += "$";
                    try {
                        it = s_regex_cache.emplace(f.value, std::regex(regex_str, std::regex_constants::icase)).first;
                    } catch (...) {
                        it = s_regex_cache.end();
                    }
                }
                if (it != s_regex_cache.end()) {
                    try { matches = std::regex_match(s_val, it->second); } catch (...) { matches = false; }
                }
            }
            res = (f.op == Query::Op::Like) ? matches : !matches;
            break;
        }
        default: res = false;
    }
    return res;
}

enum class TriBool {
    False = 0,
    True = 1,
    Unknown = 2
};

inline TriBool tb_and(TriBool a, TriBool b) {
    if (a == TriBool::False || b == TriBool::False) return TriBool::False;
    if (a == TriBool::True && b == TriBool::True) return TriBool::True;
    return TriBool::Unknown;
}

inline TriBool tb_or(TriBool a, TriBool b) {
    if (a == TriBool::True || b == TriBool::True) return TriBool::True;
    if (a == TriBool::False && b == TriBool::False) return TriBool::False;
    return TriBool::Unknown;
}

inline TriBool tb_not(TriBool a) {
    if (a == TriBool::True) return TriBool::False;
    if (a == TriBool::False) return TriBool::True;
    return TriBool::Unknown;
}

static TriBool evaluate_group_tribool(const Query::FilterGroup& g, const std::unordered_map<std::string, std::shared_ptr<Node>>& available_nodes, std::string_view current_alias, Engine* engine) {
    if (g.nodes.empty()) return TriBool::True;
    TriBool result = TriBool::True;
    bool is_first = true;
    for (const auto& n : g.nodes) {
        TriBool val = std::visit(overloaded{
            [&](const Query::Filter& f) -> TriBool {
                auto it = available_nodes.find(f.alias);
                if (it != available_nodes.end()) return evaluate_filter(it->second.get(), f, engine) ? TriBool::True : TriBool::False;
                return TriBool::Unknown; 
            },
            [&](const std::shared_ptr<Query::FilterGroup>& sub) -> TriBool { return evaluate_group_tribool(*sub, available_nodes, current_alias, engine); }
        }, n.node);
        if (is_first) {
            result = val;
            is_first = false;
        } else {
            if (n.prepended_op == Query::LogicalOp::Or) {
                result = tb_or(result, val);
            } else {
                result = tb_and(result, val);
            }
        }
    }
    return result;
}

static bool evaluate_group(const Query::FilterGroup& g, const std::unordered_map<std::string, std::shared_ptr<Node>>& available_nodes, std::string_view current_alias, Engine* engine) {
    TriBool res = evaluate_group_tribool(g, available_nodes, current_alias, engine);
    return res != TriBool::False;
}

Query::FilterGroup& Query::FilterGroup::where(std::string_view alias, std::string_view key, Op op, std::string_view value) { nodes.push_back({Filter{std::string(alias), std::string(key), op, std::string(value)}, LogicalOp::And}); return *this; }
Query::FilterGroup& Query::FilterGroup::or_where(std::string_view alias, std::string_view key, Op op, std::string_view value) { nodes.push_back({Filter{std::string(alias), std::string(key), op, std::string(value)}, LogicalOp::Or}); return *this; }
Query::FilterGroup& Query::FilterGroup::where_group(std::function<void(FilterGroup&)> cb) { auto sub = std::make_shared<FilterGroup>(); cb(*sub); nodes.push_back({sub, LogicalOp::And}); return *this; }
Query::FilterGroup& Query::FilterGroup::or_where_group(std::function<void(FilterGroup&)> cb) { auto sub = std::make_shared<FilterGroup>(); cb(*sub); nodes.push_back({sub, LogicalOp::Or}); return *this; }

Query::Query(Engine *engine) : engine_(engine) {}
Query &Query::match(std::string_view node_alias) { initial_match_ = {std::string(node_alias)}; return *this; }
Query &Query::match_id(uint64_t id, std::string_view alias) { initial_match_ = {std::string(alias)}; char buf[24]; std::snprintf(buf, sizeof(buf), "%016llx", (unsigned long long)id); root_filters_.where(alias, "id", Op::Eq, std::string(buf)); return *this; }
Query &Query::match_id(std::string_view uuid, std::string_view alias) { initial_match_ = {std::string(alias)}; root_filters_.where(alias, "id", Op::Eq, std::string(uuid)); return *this; }
Query &Query::where(std::string_view node_alias, std::string_view key, Op op, std::string_view value) { root_filters_.where(node_alias, key, op, value); return *this; }
Query &Query::where_group(std::function<void(FilterGroup&)> cb) { root_filters_.where_group(cb); return *this; }
Query &Query::or_where(std::string_view node_alias, std::string_view key, Op op, std::string_view value) { root_filters_.or_where(node_alias, key, op, value); return *this; }
Query &Query::or_where_group(std::function<void(FilterGroup&)> cb) { root_filters_.or_where_group(cb); return *this; }

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
Query& Query::group_by(std::string_view alias, std::string_view property, std::string_view func_name, const std::vector<std::string>& func_args) {
    groups_.push_back(GroupStep{std::string(alias), std::string(property), std::string(func_name), func_args});
    return *this;
}
Query& Query::distinct(bool enable) {
    distinct_ = enable;
    return *this;
}

static bool has_or_or_like_filter(const Query::FilterGroup& g) {
    for (const auto& n : g.nodes) {
        if (n.prepended_op == Query::LogicalOp::Or) return true;
        if (auto* f = std::get_if<Query::Filter>(&n.node)) {
            if (f->op == Query::Op::Like || f->op == Query::Op::NotLike || f->op == Query::Op::Ne) return true;
        } else if (auto* sub = std::get_if<std::shared_ptr<Query::FilterGroup>>(&n.node)) {
            if (has_or_or_like_filter(**sub)) return true;
        }
    }
    return false;
}

static const Query::Filter* find_first_eq_filter(const Query::FilterGroup& g, std::string_view alias, std::string_view key = "") {
    for (const auto& n : g.nodes) {
        if (auto* f = std::get_if<Query::Filter>(&n.node)) { if (f->alias == alias && f->op == Query::Op::Eq && (key.empty() || f->key == key)) return f; }
        else if (auto* sub = std::get_if<std::shared_ptr<Query::FilterGroup>>(&n.node)) { if (auto* res = find_first_eq_filter(**sub, alias, key)) return res; }
    }
    return nullptr;
}

static void serialize_filter_nodes(const Query::FilterGroup& group, lite3cpp::Buffer& buf, size_t arr_ofs, int depth = 0) {
    if (depth > 32) return;
    for (const auto& n : group.nodes) {
        size_t item_ofs = buf.arr_append_obj(arr_ofs);
        std::string prep = (n.prepended_op == Query::LogicalOp::Or) ? "or" : "and";
        buf.set_str(item_ofs, "prepended_op", prep);
        std::visit(overloaded{
            [&](const Query::Filter& f) {
                buf.set_str(item_ofs, "alias", f.alias);
                buf.set_str(item_ofs, "key", f.key);
                buf.set_i64(item_ofs, "op", static_cast<int64_t>(f.op));
                buf.set_str(item_ofs, "value", f.value);
            },
            [&](const std::shared_ptr<Query::FilterGroup>& sub) {
                buf.set_bool(item_ofs, "group", true);
                if (sub) {
                    size_t sub_arr_ofs = buf.set_arr(item_ofs, "filters");
                    serialize_filter_nodes(*sub, buf, sub_arr_ofs, depth + 1);
                }
            }
        }, n.node);
    }
}

static void parse_filter_nodes(const lite3cpp::Buffer& buf, size_t filters_arr_ofs, Query::FilterGroup& group, int depth = 0) {
    if (depth > 32) return;
    if (filters_arr_ofs + sizeof(lite3cpp::PackedNodeLayout) > buf.size()) return;
    lite3cpp::NodeView fn(reinterpret_cast<const lite3cpp::PackedNodeLayout*>(buf.data() + filters_arr_ofs));
    for (uint32_t i = 0; i < fn.size(); ++i) {
        if (buf.arr_get_type(filters_arr_ofs, i) != lite3cpp::Type::Object) continue;
        size_t item_ofs = buf.arr_get_obj(filters_arr_ofs, i);
        if (buf.get_type(item_ofs, "group") != lite3cpp::Type::Invalid) {
            std::string grp_prep = (buf.get_type(item_ofs, "prepended_op") == lite3cpp::Type::String) 
                                   ? std::string(buf.get_str(item_ofs, "prepended_op")) : "and";
            size_t sub_filters_ofs = 0;
            bool has_sub = (buf.get_type(item_ofs, "filters") == lite3cpp::Type::Array);
            if (has_sub) sub_filters_ofs = buf.get_arr(item_ofs, "filters");
            auto cb = [&buf, has_sub, sub_filters_ofs, depth](Query::FilterGroup& sub) {
                if (has_sub) {
                    parse_filter_nodes(buf, sub_filters_ofs, sub, depth + 1);
                }
            };
            if (grp_prep == "or") {
                group.or_where_group(cb);
            } else {
                group.where_group(cb);
            }
        } else if (buf.get_type(item_ofs, "alias") == lite3cpp::Type::String &&
                   buf.get_type(item_ofs, "key") == lite3cpp::Type::String &&
                   (buf.get_type(item_ofs, "op") == lite3cpp::Type::Int64 || buf.get_type(item_ofs, "op") == lite3cpp::Type::Float64) &&
                   buf.get_type(item_ofs, "value") == lite3cpp::Type::String) {
            std::string alias = std::string(buf.get_str(item_ofs, "alias"));
            std::string key = std::string(buf.get_str(item_ofs, "key"));
            int op_val = (buf.get_type(item_ofs, "op") == lite3cpp::Type::Int64) 
                         ? static_cast<int>(buf.get_i64(item_ofs, "op")) 
                         : static_cast<int>(buf.get_f64(item_ofs, "op"));
            auto op = static_cast<Query::Op>(op_val);
            std::string val = std::string(buf.get_str(item_ofs, "value"));
            std::string prep = (buf.get_type(item_ofs, "prepended_op") == lite3cpp::Type::String) 
                               ? std::string(buf.get_str(item_ofs, "prepended_op")) : "and";
            if (prep == "or") {
                group.or_where(alias, key, op, val);
            } else {
                group.where(alias, key, op, val);
            }
        }
    }
}

static bool is_entity_type_match(std::string_view alias, std::string_view et, std::string_view actual_type, bool has_v) {
    if (!et.empty()) {
        if (alias == "DataObject") return (et == "data_object" || et == "generic");
        if (alias == "Collection" || alias == "ParentCollection") return (et == "collection");
        if (alias == "Resource" || alias == "ChildResource" || alias == "ParentResource") return (et == "resource" || et == "unixfilesystem" || et == "passthru" || et == "replication" || et == "compound" || et == "load_balanced" || et == "random" || et == "deferred" || et == "structfile" || et == "s3" || et == "mockarchive");
        if (alias == "User") return (et == "user" || et == "group" || et == "rodsuser" || et == "rodsadmin" || et == "groupadmin" || et == "rodsgroup");
        if (alias == "Group") return (et == "group" || et == "rodsgroup" || et == "user");
        if (alias == "Zone") return (et == "zone" || et == "local" || et == "remote");
        if (alias == "Replica") return (et == "replica");
        if (alias == "Metadata") return (et == "metadata");
        if (alias == "Access" || alias == "CollAccess") return (et == "access" || et == "access_type");
        if (alias == "Rule") return (et == "rule");
        if (alias == "Ticket") return (et == "ticket");
        if (strcasecmp(std::string(alias).c_str(), std::string(et).c_str()) == 0) return true;
        return false;
    }
    // Fallback when entity_type is empty
    if (alias == "Zone") return (actual_type == "zone" || actual_type == "local" || actual_type == "remote");
    if (alias == "User") return (actual_type == "user" || actual_type == "rodsuser" || actual_type == "rodsadmin" || actual_type == "groupadmin" || actual_type == "rodsgroup");
    if (alias == "Group") return (actual_type == "rodsgroup" || actual_type == "group" || actual_type == "rodsuser" || actual_type == "rodsadmin" || actual_type == "groupadmin" || actual_type == "user");
    if (alias == "Collection" || alias == "ParentCollection") return (!has_v && actual_type != "data_object" && actual_type != "generic" && actual_type != "replica" && actual_type != "resource" && actual_type != "unixfilesystem" && actual_type != "passthru" && actual_type != "replication" && actual_type != "compound" && actual_type != "load_balanced" && actual_type != "random" && actual_type != "deferred" && actual_type != "structfile" && actual_type != "s3" && actual_type != "mockarchive" && actual_type != "user" && actual_type != "rodsuser" && actual_type != "rodsadmin" && actual_type != "groupadmin" && actual_type != "rodsgroup" && actual_type != "zone" && actual_type != "local" && actual_type != "remote" && actual_type != "metadata" && actual_type != "access" && actual_type != "access_type" && actual_type != "rule" && actual_type != "ticket");
    if (alias == "DataObject") return (actual_type == "data_object" || actual_type == "generic");
    if (alias == "Replica") return (actual_type == "replica");
    if (alias == "Resource" || alias == "ChildResource" || alias == "ParentResource") return (actual_type == "resource" || actual_type == "unixfilesystem" || actual_type == "passthru" || actual_type == "replication" || actual_type == "compound" || actual_type == "load_balanced" || actual_type == "random" || actual_type == "deferred" || actual_type == "structfile" || actual_type == "s3" || actual_type == "mockarchive");
    if (alias == "Metadata") return (actual_type == "metadata");
    if (alias == "Access" || alias == "CollAccess") return (actual_type == "access" || actual_type == "access_type");
    if (alias == "Rule") return (actual_type == "rule");
    if (alias == "Ticket") return (actual_type == "ticket");
    return true;
}

std::vector<ResultRow> Query::execute() {
  std::vector<ResultRow> results; std::set<uint64_t> frontier_set;
  L3_LOG(0, "Query::execute() ENTER: starting_nodes=%zu, root_filters=%zu, steps=%zu", starting_nodes_.size(), root_filters_.nodes.size(), steps_.size());
  if (!starting_nodes_.empty()) {
      for(auto id : starting_nodes_) frontier_set.insert(id);
  } else {
      if (!initial_match_) return results;
      root_alias_ = initial_match_->alias;
      bool has_complex = has_or_or_like_filter(root_filters_);
      bool had_eq_filter = false;

      if (auto* f = find_first_eq_filter(root_filters_, root_alias_, "id")) {
          had_eq_filter = true;
          frontier_set.insert(engine_->get_resolver().parse_uuid(f->value));
      }

      if (frontier_set.empty()) {
        std::function<void(const FilterGroup&)> find_index_filters = [&](const FilterGroup& fg) {
            for (const auto &n : fg.nodes) {
                if (auto* f = std::get_if<Filter>(&n.node)) {
                    if (f->alias == root_alias_) {
                        if (f->op == Op::Eq) {
                            if (f->key == "n" || f->key == "path" || f->key == "id") {
                                had_eq_filter = true;
                            }
                            std::string idx_prefix = "idx:" + f->alias + ":" + f->key + ":" + f->value;
                            auto idx_keys = engine_->get_store()->get_prefix_keys_all_shards(idx_prefix, "", 100000);
                            for (const auto& k : idx_keys) {
                                auto idx_buf = engine_->get_store()->get(k);
                                if (idx_buf.size() > 0) {
                                    std::string id_str(reinterpret_cast<const char*>(idx_buf.data()), idx_buf.size());
                                    try { frontier_set.insert(std::stoull(id_str, nullptr, 16)); } catch(...) {}
                                }
                            }
                        } else if (f->op == Op::Like) {
                            size_t first_wc = f->value.find_first_of("%_");
                            std::string prefix_literal = (first_wc == std::string::npos) ? f->value : f->value.substr(0, first_wc);
                            if (!prefix_literal.empty()) {
                                std::string idx_prefix = "idx:" + f->alias + ":" + f->key + ":" + prefix_literal;
                                auto idx_keys = engine_->get_store()->get_prefix_keys_all_shards(idx_prefix, "", 100000);
                                for (const auto& k : idx_keys) {
                                    auto idx_buf = engine_->get_store()->get(k);
                                    if (idx_buf.size() > 0) {
                                        std::string id_str(reinterpret_cast<const char*>(idx_buf.data()), idx_buf.size());
                                        try { frontier_set.insert(std::stoull(id_str, nullptr, 16)); } catch(...) {}
                                    }
                                }
                            } else {
                                // Substring / trigram search
                                std::vector<std::string> trigrams;
                                std::string segment;
                                for (char c : f->value) {
                                    if (c == '%' || c == '_') {
                                        if (segment.size() >= 3) {
                                            for (size_t ti = 0; ti + 3 <= segment.size(); ++ti) {
                                                trigrams.push_back(segment.substr(ti, 3));
                                            }
                                        }
                                        segment.clear();
                                    } else {
                                        segment += c;
                                    }
                                }
                                if (segment.size() >= 3) {
                                    for (size_t ti = 0; ti + 3 <= segment.size(); ++ti) {
                                        trigrams.push_back(segment.substr(ti, 3));
                                    }
                                }
                                if (!trigrams.empty()) {
                                    std::set<uint64_t> tri_candidates;
                                    bool first_tri = true;
                                    for (const auto& tri : trigrams) {
                                        std::string tri_prefix = "idx:" + f->alias + ":tri:" + tri + ":";
                                        auto idx_keys = engine_->get_store()->get_prefix_keys_all_shards(tri_prefix, "", 100000);
                                        std::set<uint64_t> cur_tri_set;
                                        for (const auto& k : idx_keys) {
                                            auto idx_buf = engine_->get_store()->get(k);
                                            if (idx_buf.size() > 0) {
                                                std::string id_str(reinterpret_cast<const char*>(idx_buf.data()), idx_buf.size());
                                                try { cur_tri_set.insert(std::stoull(id_str, nullptr, 16)); } catch(...) {}
                                            }
                                        }
                                        if (first_tri) {
                                            tri_candidates = std::move(cur_tri_set);
                                            first_tri = false;
                                        } else {
                                            std::set<uint64_t> inter;
                                            std::set_intersection(tri_candidates.begin(), tri_candidates.end(),
                                                                  cur_tri_set.begin(), cur_tri_set.end(),
                                                                  std::inserter(inter, inter.begin()));
                                            tri_candidates = std::move(inter);
                                        }
                                    }
                                    frontier_set.insert(tri_candidates.begin(), tri_candidates.end());
                                }
                            }
                        }
                    }
                } else if (auto* sub = std::get_if<std::shared_ptr<FilterGroup>>(&n.node)) {
                    if (*sub && n.prepended_op == LogicalOp::And) {
                        find_index_filters(**sub);
                    }
                }
            }
        };
        find_index_filters(root_filters_);
      }
      if (frontier_set.empty()) {
        if (had_eq_filter && !has_complex) {
            return results;
        }
        if (!root_alias_.empty()) {
            std::string entity_name = (root_alias_ == "Group" ? "User" : root_alias_);
            std::string idx_id_prefix = "idx:" + entity_name + ":id:";
            auto idx_keys = engine_->get_store()->get_prefix_keys_all_shards(idx_id_prefix, "", 10000);
            for (const auto& k : idx_keys) {
                auto idx_buf = engine_->get_store()->get(k);
                if (idx_buf.size() > 0) {
                    std::string id_str(reinterpret_cast<const char*>(idx_buf.data()), idx_buf.size());
                    try { frontier_set.insert(std::stoull(id_str, nullptr, 16)); } catch(...) {}
                }
            }
            if (frontier_set.empty()) {
                std::string idx_n_prefix = "idx:" + entity_name + ":n:";
                auto idx_keys_n = engine_->get_store()->get_prefix_keys_all_shards(idx_n_prefix, "", 10000);
                for (const auto& k : idx_keys_n) {
                    auto idx_buf = engine_->get_store()->get(k);
                    if (idx_buf.size() > 0) {
                        std::string id_str(reinterpret_cast<const char*>(idx_buf.data()), idx_buf.size());
                        try { frontier_set.insert(std::stoull(id_str, nullptr, 16)); } catch(...) {}
                    }
                }
            }
        }
        if (frontier_set.empty() && root_alias_.empty()) {
            std::string store_prefix = "n:{"; 
            auto keys = engine_->get_store()->get_prefix_keys_all_shards(store_prefix, "", engine_->get_settings().prefix_scan_limit);
            L3_LOG(0, "Query::execute() store_prefix scan returned keys=%zu", keys.size());
            for (const auto& k : keys) {
                if (k.starts_with("n:{") && !k.ends_with(":meta")) {
                    size_t end_pos = k.find('}', 3);
                    if (end_pos != std::string::npos) {
                        try { frontier_set.insert(std::stoull(k.substr(3, end_pos - 3), nullptr, 16)); } catch(...) {}
                    }
                }
            }
        }
      }
  }

  L3_LOG(0, "Query::execute() root_alias=%s, frontier_set.size=%zu", root_alias_.c_str(), frontier_set.size());
  if (frontier_set.empty()) return results;

  std::vector<uint64_t> frontier(frontier_set.begin(), frontier_set.end());
  {
      std::vector<uint64_t> filtered;
      auto nodes = engine_->fetch_nodes(frontier, principal_id_);
      for (auto& node : nodes) {
          L3_LOG(0, "Query::execute() checking node %016llx: ptr=%d, loaded=%d", 
                 (unsigned long long)(node ? node->get_id() : 0), (node != nullptr), (node && node->is_loaded()));
          if (!node || !node->is_loaded()) continue;
          if (!root_alias_.empty()) {
              std::string et;
              try { et = node->get_attribute_as_string("entity_type"); } catch (...) {}
              std::string actual_type;
              try { actual_type = node->get_attribute_as_string("t"); } catch (...) {}
              bool has_v = node->has_attribute("v");
              if (!is_entity_type_match(root_alias_, et, actual_type, has_v)) {
                  L3_LOG(0, "Query::execute() node %016llx skipped: root_alias '%s' mismatch with et '%s', actual_type '%s'",
                         (unsigned long long)node->get_id(), root_alias_.c_str(), et.c_str(), actual_type.c_str());
                  continue;
              }
          }
          std::string key = std::string(KeyBuilder::node_key(node->get_id()));
          auto perm = (principal_id_ == INTERNAL_UID || principal_id_ == 0) ? l3kv::Permission::ADMIN : engine_->get_store()->credentials().check_permission(principal_id_, key);
          L3_LOG(0, "Query::execute() node %016llx: perm=0x%x, principal=%u", (unsigned long long)node->get_id(), (unsigned int)perm, principal_id_);
          if (!(perm & l3kv::Permission::READ) && !(perm & l3kv::Permission::ADMIN)) continue;
          std::unordered_map<std::string, std::shared_ptr<Node>> available; available[root_alias_] = node;
          bool eval_res = evaluate_group(root_filters_, available, root_alias_, engine_);
          L3_LOG(0, "Query::execute() evaluate_group result=%d", eval_res);
          if (eval_res) filtered.push_back(node->get_id());
      }
      frontier = std::move(filtered);
  }

  L3_LOG(0, "Query::execute() filtered frontier count=%zu", frontier.size());
  if (frontier.empty()) return results;
  
  struct Path { std::unordered_map<std::string, std::shared_ptr<Node>> alias_to_node; std::string last_alias; };
  std::vector<Path> paths;
  for (const auto& id : frontier) {
      auto node = engine_->get_node(id);
      if (!node) continue;
      std::string actual_type;
      try { actual_type = node->get_attribute_as_string("t"); } catch (...) { actual_type = ""; }
      std::string entity_type;
      try { entity_type = node->get_attribute_as_string("entity_type"); } catch (...) { entity_type = ""; }
      L3_LOG(0, "Query::execute() checking candidate %016llx: alias='%s', loaded=%d, type='%s', entity_type='%s', has_v=%d", (unsigned long long)id, root_alias_.c_str(), node->is_loaded(), actual_type.c_str(), entity_type.c_str(), node->has_attribute("v"));
      bool type_match = is_entity_type_match(root_alias_, entity_type, actual_type, node->has_attribute("v"));

      L3_LOG(0, "Query::execute() candidate %016llx: type_match=%d", (unsigned long long)id, type_match);
      if (!type_match) continue;
      Path p; p.alias_to_node[root_alias_] = node; p.last_alias = root_alias_; paths.push_back(std::move(p));
  }

  L3_LOG(0, "Query::execute() initial paths count=%zu", paths.size());

  std::unordered_map<uint16_t, std::vector<std::pair<uint64_t, std::pair<std::string, std::vector<Step>>>>> suspended_branches;

  for (size_t i = 0; i < steps_.size(); ++i) {
    const auto& step = steps_[i];
    std::vector<Path> next_paths;
    std::mutex result_mu;
    engine_->get_thread_pool().parallel_for(0, paths.size(), [&](size_t first, size_t last) {
        std::vector<Path> local_next_paths;
        std::unordered_map<uint16_t, std::vector<std::pair<uint64_t, std::pair<std::string, std::vector<Step>>>>> local_suspended;
        for (size_t p_idx = first; p_idx < last; ++p_idx) {
            const auto &path = paths[p_idx];
            std::visit(overloaded{
                [&](const OutStep& s) {
                    std::string src = s.source_alias.empty() ? path.last_alias : s.source_alias;
                    auto it_src = path.alias_to_node.find(src);
                    if (it_src == path.alias_to_node.end()) {
                        if(0) std::fprintf(stderr, "[Query] Step %zu: Source alias [%s] not found in path!\n", i, src.c_str());
                        return;
                    }
                    auto node = it_src->second;
                    auto neighbors = node->get_neighbors(s.label, s.min_weight, principal_id_);
                    #ifdef IRODS_SERVER
                    rodsLog(LOG_NOTICE, "[Query] Step %zu (OUT %s -> %s): Found %zu neighbors for node %016llx", i, src.c_str(), s.target_alias.c_str(), neighbors.size(), (unsigned long long)node->get_id());
#else
                    if(0) std::fprintf(stderr, "[Query] Step %zu (OUT %s -> %s): Found %zu neighbors for node %016llx\n", i, src.c_str(), s.target_alias.c_str(), neighbors.size(), (unsigned long long)node->get_id());
#endif
                    std::unordered_set<uint64_t> unique_neighbors(neighbors.begin(), neighbors.end());
                    for (const auto& neighbor_id : unique_neighbors) {
                        try {
                            uint16_t cluster_id = FederationID::get_cluster(neighbor_id);
                            if (!engine_->get_resolver().is_local_cluster(cluster_id)) {
                                std::vector<Step> remaining(steps_.begin() + i + 1, steps_.end());
                                local_suspended[cluster_id].push_back({neighbor_id, {s.target_alias, remaining}});
                                continue;
                            }
                            auto neighbor_node = engine_->get_node(neighbor_id);
                            if (!neighbor_node) continue;
                            std::string et;
                            try { et = neighbor_node->get_attribute_as_string("entity_type"); } catch (...) {}
                            std::string actual_type;
                            try { actual_type = neighbor_node->get_attribute_as_string("t"); } catch (...) {}
                            bool has_v = neighbor_node->has_attribute("v");
                            if (!is_entity_type_match(s.target_alias, et, actual_type, has_v)) continue;
                            Path new_path = path; new_path.alias_to_node[s.target_alias] = neighbor_node; new_path.last_alias = s.target_alias;
                            if (evaluate_group(root_filters_, new_path.alias_to_node, s.target_alias, engine_)) {
                                local_next_paths.push_back(std::move(new_path));
                            } else {
                                if(0) std::fprintf(stderr, "  [Query] Neighbor %016llx filtered out\n", (unsigned long long)neighbor_id);
                            }
                        } catch (...) {}
                    }
                },
                [&](const InStep& s) {
                    std::string src = s.source_alias.empty() ? path.last_alias : s.source_alias;
                    auto it_src = path.alias_to_node.find(src);
                    if (it_src == path.alias_to_node.end()) {
                        if(0) std::fprintf(stderr, "[Query] Step %zu: Source alias [%s] not found in path!\n", i, src.c_str());
                        return;
                    }
                    auto node = it_src->second;
                    auto neighbors = node->get_in_neighbors(s.label, principal_id_);
                    #ifdef IRODS_SERVER
                    rodsLog(LOG_NOTICE, "[Query] Step %zu (IN %s -> %s): Found %zu neighbors for node %016llx", i, src.c_str(), s.target_alias.c_str(), neighbors.size(), (unsigned long long)node->get_id());
#else
                    if(0) std::fprintf(stderr, "[Query] Step %zu (IN %s -> %s): Found %zu neighbors for node %016llx\n", i, src.c_str(), s.target_alias.c_str(), neighbors.size(), (unsigned long long)node->get_id());
#endif
                    std::unordered_set<uint64_t> unique_neighbors(neighbors.begin(), neighbors.end());
                    for (const auto& neighbor_id : unique_neighbors) {
                        try {
                            uint16_t cluster_id = FederationID::get_cluster(neighbor_id);
                            if (!engine_->get_resolver().is_local_cluster(cluster_id)) {
                                std::vector<Step> remaining(steps_.begin() + i + 1, steps_.end());
                                local_suspended[cluster_id].push_back({neighbor_id, {s.target_alias, remaining}});
                                continue;
                            }
                            auto neighbor_node = engine_->get_node(neighbor_id);
                            if (!neighbor_node) continue;
                            std::string et;
                            try { et = neighbor_node->get_attribute_as_string("entity_type"); } catch (...) {}
                            std::string actual_type;
                            try { actual_type = neighbor_node->get_attribute_as_string("t"); } catch (...) {}
                            bool has_v = neighbor_node->has_attribute("v");
                            if (!is_entity_type_match(s.target_alias, et, actual_type, has_v)) continue;
                            Path new_path = path; new_path.alias_to_node[s.target_alias] = neighbor_node; new_path.last_alias = s.target_alias;
                            if (evaluate_group(root_filters_, new_path.alias_to_node, s.target_alias, engine_)) {
                                local_next_paths.push_back(std::move(new_path));
                            } else {
                                if(0) std::fprintf(stderr, "  [Query] Neighbor %016llx filtered out\n", (unsigned long long)neighbor_id);
                            }
                        } catch (...) {}
                    }
                }
            }, step);
        }
        std::lock_guard<std::mutex> lock(result_mu);
        next_paths.insert(next_paths.end(), std::make_move_iterator(local_next_paths.begin()), std::make_move_iterator(local_next_paths.end()));
        for (auto& [cluster_id, branches] : local_suspended) {
            auto& target = suspended_branches[cluster_id];
            target.insert(target.end(), std::make_move_iterator(branches.begin()), std::make_move_iterator(branches.end()));
        }
    });
    paths = std::move(next_paths);
    if (paths.empty() && suspended_branches.empty()) break;
  }

  std::vector<std::future<std::vector<ResultRow>>> remote_futures;
  for (auto& [cluster_id, branches] : suspended_branches) {
      struct QueryGroup {
          lite3cpp::Buffer buf;
          std::vector<uint64_t> nodes;
      };
      std::unordered_map<std::string, QueryGroup> groups;
      for (auto& b : branches) {
          lite3cpp::Buffer sub_buf;
          sub_buf.init_object();
          sub_buf.set_str(0, "root_alias", b.second.first);
          sub_buf.set_i64(0, "principal_id", static_cast<int64_t>(principal_id_));
          size_t steps_ofs = sub_buf.set_arr(0, "steps");
          for (const auto& step : b.second.second) {
              std::visit(overloaded{
                  [&](const OutStep& s) {
                      size_t s_ofs = sub_buf.arr_append_obj(steps_ofs);
                      sub_buf.set_str(s_ofs, "type", "out");
                      sub_buf.set_str(s_ofs, "label", s.label);
                      sub_buf.set_f64(s_ofs, "min_weight", s.min_weight);
                      sub_buf.set_str(s_ofs, "target_alias", s.target_alias);
                      if (!s.source_alias.empty()) {
                          sub_buf.set_str(s_ofs, "source_alias", s.source_alias);
                      }
                  },
                  [&](const InStep& s) {
                      size_t s_ofs = sub_buf.arr_append_obj(steps_ofs);
                      sub_buf.set_str(s_ofs, "type", "in");
                      sub_buf.set_str(s_ofs, "label", s.label);
                      sub_buf.set_str(s_ofs, "target_alias", s.target_alias);
                      if (!s.source_alias.empty()) {
                          sub_buf.set_str(s_ofs, "source_alias", s.source_alias);
                      }
                  }
              }, step);
          }
          size_t projs_ofs = sub_buf.set_arr(0, "projections");
          for (const auto& p : projections_) {
              size_t p_ofs = sub_buf.arr_append_obj(projs_ofs);
              sub_buf.set_str(p_ofs, "alias", p.alias);
              sub_buf.set_str(p_ofs, "property", p.property);
              sub_buf.set_i64(p_ofs, "agg", static_cast<int64_t>(p.agg));
              sub_buf.set_bool(p_ofs, "distinct", p.distinct);
          }
          if (!root_filters_.nodes.empty()) {
              size_t filters_ofs = sub_buf.set_arr(0, "filters");
              serialize_filter_nodes(root_filters_, sub_buf, filters_ofs);
          }
          std::string query_bytes(reinterpret_cast<const char*>(sub_buf.data()), sub_buf.size());
          auto it = groups.find(query_bytes);
          if (it == groups.end()) {
              groups.emplace(std::move(query_bytes), QueryGroup{std::move(sub_buf), {b.first}});
          } else {
              it->second.nodes.push_back(b.first);
          }
      }
      for (auto& [_, qg] : groups) {
          remote_futures.push_back(engine_->get_remote_client().resume_query_async(cluster_id, qg.nodes, qg.buf, principal_id_));
      }
  }

  for (const auto &path : paths) {
    TriBool tb = evaluate_group_tribool(root_filters_, path.alias_to_node, "", engine_);
    if (is_federated_branch_ ? (tb == TriBool::False) : (tb != TriBool::True)) {
        continue;
    }
    ResultRow row;
    for (const auto& [alias, node] : path.alias_to_node) {
        row.nodes.push_back(node);
        for (size_t i = 0; i < projections_.size(); ++i) {
            if (projections_[i].alias == alias) {
                if (node->has_attribute(projections_[i].property)) {
                    try {
                        std::string val = node->get_attribute_as_string(projections_[i].property);
                        row.fields[alias + "." + projections_[i].property] = val;
                        row.fields["idx_" + std::to_string(i)] = val;
                    } catch (...) { row.fields["idx_" + std::to_string(i)] = ""; }
                }
            }
        }
        for (const auto& s : sorts_) if (s.alias == alias) { std::string k = s.alias + "." + s.property; if (row.fields.find(k) == row.fields.end()) { if (node->has_attribute(s.property)) row.fields[k] = node->get_attribute_as_string(s.property); } }
        for (const auto& g : groups_) if (g.alias == alias) { std::string k = g.alias + "." + g.property; if (row.fields.find(k) == row.fields.end()) { if (node->has_attribute(g.property)) row.fields[k] = node->get_attribute_as_string(g.property); } }
    }
    for (size_t i = 0; i < projections_.size(); ++i) {
        if (!row.fields.contains("idx_" + std::to_string(i))) row.fields["idx_" + std::to_string(i)] = "";
        row.fields["_col_" + std::to_string(i)] = projections_[i].alias + "." + projections_[i].property;
    }
    results.push_back(std::move(row));
  }
  for (auto& f : remote_futures) {
      auto remote_res = f.get();
      results.insert(results.end(), remote_res.begin(), remote_res.end());
  }

  bool has_agg = false; for (const auto& p : projections_) if (p.agg != AggOp::None) { has_agg = true; break; }
  if (has_agg || !groups_.empty()) {
      std::vector<std::string> partition_order;
      std::unordered_map<std::string, std::vector<ResultRow>> partitions;
      std::vector<GroupStep> effective_groups = groups_;
      if (effective_groups.empty() && has_agg) {
          for (const auto& p : projections_) {
              if (p.agg == AggOp::None) {
                  bool exists = false;
                  for (const auto& eg : effective_groups) {
                      if (eg.alias == p.alias && eg.property == p.property) {
                          exists = true;
                          break;
                      }
                  }
                  if (!exists) {
                      effective_groups.push_back(GroupStep{p.alias, p.property, "", {}});
                  }
              }
          }
      }
      if (effective_groups.empty()) {
          partition_order.push_back("ALL");
          partitions["ALL"] = std::move(results);
      } else {
          for (auto& row : results) {
              std::string g_key;
              for (const auto& g : effective_groups) {
                  auto it = row.fields.find(g.alias + "." + g.property);
                  std::string val = (it != row.fields.end() ? it->second : "");
                  if (!g.func_name.empty()) {
                      std::string fn = g.func_name;
                      std::transform(fn.begin(), fn.end(), fn.begin(), ::toupper);
                      if (fn == "LENGTH") {
                          val = std::to_string(val.size());
                      } else if (fn == "SUBSTRING" || fn == "SUBSTR") {
                          if (!g.func_args.empty()) {
                              try {
                                  int pos = std::stoi(g.func_args[0]);
                                  int start = std::max(0, pos);
                                  if (start < static_cast<int>(val.size())) {
                                      if (g.func_args.size() > 1) {
                                          int len = std::stoi(g.func_args[1]);
                                          val = (len > 0) ? val.substr(start, len) : "";
                                      } else {
                                          val = val.substr(start);
                                      }
                                  } else {
                                      val = "";
                                  }
                              } catch (...) {}
                          }
                      }
                  }
                  g_key += val + "|";
              }
              if (partitions.find(g_key) == partitions.end()) {
                  partition_order.push_back(g_key);
              }
              partitions[g_key].push_back(std::move(row));
          }
      }
      std::vector<ResultRow> final_res;
      for (const auto& pk : partition_order) {
          auto& part = partitions[pk]; ResultRow agg_row;
          for (const auto& g : effective_groups) {
              std::string gk = g.alias + "." + g.property;
              if (!part.empty() && part[0].fields.contains(gk)) {
                  agg_row.fields[gk] = part[0].fields.at(gk);
              }
          }
          for (size_t i = 0; i < projections_.size(); ++i) {
              const auto& p = projections_[i]; std::string k = "idx_" + std::to_string(i);
              agg_row.fields["_col_" + std::to_string(i)] = p.alias + "." + p.property;
              if (p.agg == AggOp::None) { 
                  if (!part.empty()) {
                      auto it = part[0].fields.find(p.alias + "." + p.property);
                      if (it == part[0].fields.end()) it = part[0].fields.find(k);
                      if (it != part[0].fields.end()) {
                          agg_row.fields[k] = it->second;
                          agg_row.fields[p.alias + "." + p.property] = it->second;
                      } else {
                          agg_row.fields[k] = "";
                      }
                  } else {
                      agg_row.fields[k] = "";
                  }
              }
              else if (p.agg == AggOp::Count) {
                  if (p.distinct) {
                      std::unordered_set<std::string> dist_vals;
                      for (auto& r : part) {
                          auto it = r.fields.find(p.alias + "." + p.property);
                          if (it == r.fields.end()) it = r.fields.find(k);
                          if (it != r.fields.end() && !it->second.empty()) dist_vals.insert(it->second);
                      }
                      agg_row.fields[k] = std::to_string(dist_vals.size());
                  } else {
                      agg_row.fields[k] = std::to_string(part.size());
                  }
                  agg_row.fields[p.alias + "." + p.property] = agg_row.fields[k];
              }
              else {
                  std::vector<double> vals;
                  if (p.distinct) {
                      std::set<double> dist_set;
                      for (auto& r : part) {
                          auto it = r.fields.find(p.alias + "." + p.property);
                          if (it == r.fields.end()) it = r.fields.find(k);
                          if (it != r.fields.end() && !it->second.empty()) {
                              try { dist_set.insert(std::stod(it->second)); } catch(...) {}
                          }
                      }
                      vals.assign(dist_set.begin(), dist_set.end());
                  } else {
                      for (auto& r : part) {
                          auto it = r.fields.find(p.alias + "." + p.property);
                          if (it == r.fields.end()) it = r.fields.find(k);
                          if (it != r.fields.end() && !it->second.empty()) {
                              try { vals.push_back(std::stod(it->second)); } catch(...) {}
                          }
                      }
                  }
                  if (vals.empty()) {
                      agg_row.fields[k] = "0";
                  } else {
                      double acc = 0;
                      if (p.agg == AggOp::Min) {
                          acc = *std::min_element(vals.begin(), vals.end());
                      } else if (p.agg == AggOp::Max) {
                          acc = *std::max_element(vals.begin(), vals.end());
                      } else {
                          for (double v : vals) acc += v;
                          if (p.agg == AggOp::Avg) acc /= vals.size();
                      }
                      if (acc == static_cast<int64_t>(acc)) agg_row.fields[k] = std::to_string(static_cast<int64_t>(acc));
                      else agg_row.fields[k] = std::to_string(acc);
                  }
                  agg_row.fields[p.alias + "." + p.property] = agg_row.fields[k];
              }
          }
          final_res.push_back(std::move(agg_row));
      }
      results = std::move(final_res);
  }

  if (distinct_) {
      std::set<std::string> seen; std::vector<ResultRow> unique_res;
      for (auto& row : results) {
          std::string key; 
          for (size_t i = 0; i < projections_.size(); ++i) {
              std::string k = "idx_" + std::to_string(i);
              auto it = row.fields.find(k);
              if (it != row.fields.end()) key += it->second + "|";
              else key += "|";
          }
          if (seen.insert(key).second) unique_res.push_back(std::move(row));
      }
      results = std::move(unique_res);
  }

  std::sort(results.begin(), results.end(), [&](const ResultRow& a, const ResultRow& b) {
      if (!sorts_.empty()) {
          for (const auto& s : sorts_) {
              std::string k = s.alias + "." + s.property;
              auto it_a = a.fields.find(k);
              auto it_b = b.fields.find(k);
              if (it_a != a.fields.end() && it_b != b.fields.end()) {
                  if (it_a->second != it_b->second) { 
                      if (s.ascending) return it_a->second < it_b->second; 
                      return it_a->second > it_b->second; 
                  }
              }
          }
      }
      for (size_t i = 0; i < projections_.size(); ++i) {
          std::string k = "idx_" + std::to_string(i);
          auto it_a = a.fields.find(k);
          auto it_b = b.fields.find(k);
          const std::string& v_a = (it_a != a.fields.end()) ? it_a->second : "";
          const std::string& v_b = (it_b != b.fields.end()) ? it_b->second : "";
          if (v_a != v_b) return v_a < v_b;
      }
      size_t min_nodes = std::min(a.nodes.size(), b.nodes.size());
      for (size_t n = 0; n < min_nodes; ++n) {
          if (a.nodes[n] && b.nodes[n]) {
              uint64_t id_a = a.nodes[n]->get_id();
              uint64_t id_b = b.nodes[n]->get_id();
              if (id_a != id_b) return id_a < id_b;
          }
      }
      return a.nodes.size() < b.nodes.size();
  });
  if (offset_) { if (*offset_ >= results.size()) results.clear(); else results.erase(results.begin(), results.begin() + *offset_); }
  if (limit_ && results.size() > *limit_) results.resize(*limit_);
  L3_LOG(0, "Query::execute() returning results.size=%zu", results.size());
  return results;
}

lite3cpp::Buffer Query::serialize_results(const std::vector<ResultRow>& rows) {
    lite3cpp::Buffer out_buf;
    out_buf.init_array();
    for (const auto& row : rows) {
        size_t row_ofs = out_buf.arr_append_obj(0);
        size_t fields_ofs = out_buf.set_obj(row_ofs, "fields");
        for (const auto& [k, v] : row.fields) {
            out_buf.set_str(fields_ofs, k, v);
        }
    }
    return out_buf;
}


Query &Query::resume(const std::vector<uint64_t>& starting_nodes, const lite3cpp::Buffer& query_buf) {
    starting_nodes_ = starting_nodes;
    is_federated_branch_ = true;
    try {
        if (query_buf.size() < sizeof(lite3cpp::PackedNodeLayout)) return *this;
        lite3cpp::NodeView root_node(reinterpret_cast<const lite3cpp::PackedNodeLayout*>(query_buf.data()));
        if (root_node.type() != lite3cpp::Type::Object) return *this;

        if (query_buf.get_type(0, "root_alias") == lite3cpp::Type::String) {
            root_alias_ = std::string(query_buf.get_str(0, "root_alias"));
            initial_match_ = {root_alias_};
        }
        if (query_buf.get_type(0, "principal_id") == lite3cpp::Type::Int64) {
            principal_id_ = static_cast<uint32_t>(query_buf.get_i64(0, "principal_id"));
        }
        if (query_buf.get_type(0, "steps") == lite3cpp::Type::Array) {
            size_t steps_ofs = query_buf.get_arr(0, "steps");
            lite3cpp::NodeView sn(reinterpret_cast<const lite3cpp::PackedNodeLayout*>(query_buf.data() + steps_ofs));
            for (uint32_t i = 0; i < sn.size(); ++i) {
                if (query_buf.arr_get_type(steps_ofs, i) != lite3cpp::Type::Object) continue;
                size_t s_ofs = query_buf.arr_get_obj(steps_ofs, i);
                std::string type = (query_buf.get_type(s_ofs, "type") == lite3cpp::Type::String) 
                                   ? std::string(query_buf.get_str(s_ofs, "type")) : "";
                std::string src = (query_buf.get_type(s_ofs, "source_alias") == lite3cpp::Type::String) 
                                  ? std::string(query_buf.get_str(s_ofs, "source_alias")) : "";
                std::string label = (query_buf.get_type(s_ofs, "label") == lite3cpp::Type::String) 
                                    ? std::string(query_buf.get_str(s_ofs, "label")) : "";
                std::string target = (query_buf.get_type(s_ofs, "target_alias") == lite3cpp::Type::String) 
                                     ? std::string(query_buf.get_str(s_ofs, "target_alias")) : "";
                if (type == "out") {
                    double min_w = 0.0;
                    if (query_buf.get_type(s_ofs, "min_weight") == lite3cpp::Type::Float64) {
                        min_w = query_buf.get_f64(s_ofs, "min_weight");
                    } else if (query_buf.get_type(s_ofs, "min_weight") == lite3cpp::Type::Int64) {
                        min_w = static_cast<double>(query_buf.get_i64(s_ofs, "min_weight"));
                    }
                    steps_.push_back(OutStep{std::move(label), min_w, std::move(target), std::move(src)});
                } else if (type == "in") {
                    steps_.push_back(InStep{std::move(label), std::move(target), std::move(src)});
                }
            }
        }
        if (query_buf.get_type(0, "projections") == lite3cpp::Type::Array) {
            size_t projs_ofs = query_buf.get_arr(0, "projections");
            lite3cpp::NodeView pn(reinterpret_cast<const lite3cpp::PackedNodeLayout*>(query_buf.data() + projs_ofs));
            for (uint32_t i = 0; i < pn.size(); ++i) {
                if (query_buf.arr_get_type(projs_ofs, i) != lite3cpp::Type::Object) continue;
                size_t p_ofs = query_buf.arr_get_obj(projs_ofs, i);
                std::string alias = (query_buf.get_type(p_ofs, "alias") == lite3cpp::Type::String) 
                                    ? std::string(query_buf.get_str(p_ofs, "alias")) : "";
                std::string prop = (query_buf.get_type(p_ofs, "property") == lite3cpp::Type::String) 
                                   ? std::string(query_buf.get_str(p_ofs, "property")) : "";
                int agg = 0;
                if (query_buf.get_type(p_ofs, "agg") == lite3cpp::Type::Int64) {
                    agg = static_cast<int>(query_buf.get_i64(p_ofs, "agg"));
                }
                bool dist = false;
                if (query_buf.get_type(p_ofs, "distinct") == lite3cpp::Type::Bool) {
                    dist = query_buf.get_bool(p_ofs, "distinct");
                }
                projections_.push_back(ReturnStep{std::move(alias), std::move(prop), static_cast<AggOp>(agg), dist});
            }
        }
        if (query_buf.get_type(0, "groups") == lite3cpp::Type::Array) {
            size_t groups_ofs = query_buf.get_arr(0, "groups");
            lite3cpp::NodeView gn(reinterpret_cast<const lite3cpp::PackedNodeLayout*>(query_buf.data() + groups_ofs));
            for (uint32_t i = 0; i < gn.size(); ++i) {
                if (query_buf.arr_get_type(groups_ofs, i) != lite3cpp::Type::Object) continue;
                size_t g_ofs = query_buf.arr_get_obj(groups_ofs, i);
                std::string alias = (query_buf.get_type(g_ofs, "alias") == lite3cpp::Type::String) 
                                    ? std::string(query_buf.get_str(g_ofs, "alias")) : "";
                std::string prop = (query_buf.get_type(g_ofs, "property") == lite3cpp::Type::String) 
                                   ? std::string(query_buf.get_str(g_ofs, "property")) : "";
                std::string func_name = (query_buf.get_type(g_ofs, "func_name") == lite3cpp::Type::String) 
                                        ? std::string(query_buf.get_str(g_ofs, "func_name")) : "";
                std::vector<std::string> func_args;
                if (query_buf.get_type(g_ofs, "func_args") == lite3cpp::Type::Array) {
                    size_t fa_ofs = query_buf.get_arr(g_ofs, "func_args");
                    lite3cpp::NodeView fan(reinterpret_cast<const lite3cpp::PackedNodeLayout*>(query_buf.data() + fa_ofs));
                    for (uint32_t j = 0; j < fan.size(); ++j) {
                        if (query_buf.arr_get_type(fa_ofs, j) == lite3cpp::Type::String) {
                            func_args.push_back(std::string(query_buf.arr_get_str(fa_ofs, j)));
                        }
                    }
                }
                groups_.push_back(GroupStep{std::move(alias), std::move(prop), std::move(func_name), std::move(func_args)});
            }
        }
        if (query_buf.get_type(0, "sorts") == lite3cpp::Type::Array) {
            size_t sorts_ofs = query_buf.get_arr(0, "sorts");
            lite3cpp::NodeView sn(reinterpret_cast<const lite3cpp::PackedNodeLayout*>(query_buf.data() + sorts_ofs));
            for (uint32_t i = 0; i < sn.size(); ++i) {
                if (query_buf.arr_get_type(sorts_ofs, i) != lite3cpp::Type::Object) continue;
                size_t s_ofs = query_buf.arr_get_obj(sorts_ofs, i);
                std::string alias = (query_buf.get_type(s_ofs, "alias") == lite3cpp::Type::String) 
                                    ? std::string(query_buf.get_str(s_ofs, "alias")) : "";
                std::string prop = (query_buf.get_type(s_ofs, "property") == lite3cpp::Type::String) 
                                   ? std::string(query_buf.get_str(s_ofs, "property")) : "";
                bool asc = true;
                if (query_buf.get_type(s_ofs, "ascending") == lite3cpp::Type::Bool) {
                    asc = query_buf.get_bool(s_ofs, "ascending");
                }
                sorts_.push_back(SortStep{std::move(alias), std::move(prop), asc});
            }
        }
        if (query_buf.get_type(0, "limit") == lite3cpp::Type::Int64) {
            limit_ = static_cast<size_t>(query_buf.get_i64(0, "limit"));
        }
        if (query_buf.get_type(0, "offset") == lite3cpp::Type::Int64) {
            offset_ = static_cast<size_t>(query_buf.get_i64(0, "offset"));
        }
        if (query_buf.get_type(0, "distinct") == lite3cpp::Type::Bool) {
            distinct_ = query_buf.get_bool(0, "distinct");
        }
        if (query_buf.get_type(0, "filters") == lite3cpp::Type::Array) {
            parse_filter_nodes(query_buf, query_buf.get_arr(0, "filters"), root_filters_);
        }
    } catch (...) {}
    return *this;
}

Query &Query::resume(const std::vector<uint64_t>& starting_nodes, std::string_view query_payload) {
    if (query_payload.size() >= sizeof(lite3cpp::PackedNodeLayout) && 
        (static_cast<uint8_t>(query_payload[0]) == 0x06 || static_cast<uint8_t>(query_payload[0]) == 0x07)) {
        const uint8_t* ptr = reinterpret_cast<const uint8_t*>(query_payload.data());
        lite3cpp::Buffer buf(std::vector<uint8_t>(ptr, ptr + query_payload.size()));
        return resume(starting_nodes, buf);
    } else {
        lite3cpp::Buffer buf = lite3cpp::lite3_json::from_json_string(query_payload.empty() ? "{}" : std::string(query_payload));
        return resume(starting_nodes, buf);
    }
}

} // namespace l3kvg
