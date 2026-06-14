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

#ifdef IRODS_SERVER
#include "irods/rodsLog.h"
#define L3_LOG(level, ...) if (level >= 0) rodsLog(level, __VA_ARGS__)
#else
#include <cstdio>
#define L3_LOG(level, ...) if(0) std::fprintf(stderr, "[L3KVG] " __VA_ARGS__); if(0) std::fprintf(stderr, "\n")
#endif

namespace l3kvg {

using json = nlohmann::json;

template<class... Ts> struct overloaded : Ts... { using Ts::operator()...; };
template<class... Ts> overloaded(Ts...) -> overloaded<Ts...>;

static bool evaluate_filter(Node* node, const Query::Filter& f, Engine* engine) {
    if (f.key == "id") {
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

    if (f.key == "pn") {
    }

    if (!node->has_attribute(f.key)) return false;

    std::string s_val;
    try { s_val = node->get_attribute_as_string(f.key); } catch (...) { return false; }

    bool res = false;
    auto type = node->get_attribute_type(f.key);
    switch (f.op) {
        case Query::Op::Eq: res = (s_val == f.value); break;
        case Query::Op::Ne: res = (s_val != f.value); break;
        case Query::Op::Gt: {
            if (type == lite3cpp::Type::Int64) { try { res = (std::stoll(s_val) > std::stoll(f.value)); } catch(...) { res = false; } }
            else if (type == lite3cpp::Type::Float64) { try { res = (std::stod(s_val) > std::stod(f.value)); } catch(...) { res = false; } }
            else res = (s_val > f.value);
            break;
        }
        case Query::Op::Ge: {
            if (type == lite3cpp::Type::Int64) { try { res = (std::stoll(s_val) >= std::stoll(f.value)); } catch(...) { res = false; } }
            else if (type == lite3cpp::Type::Float64) { try { res = (std::stod(s_val) >= std::stod(f.value)); } catch(...) { res = false; } }
            else res = (s_val >= f.value);
            break;
        }
        case Query::Op::Lt: {
            if (type == lite3cpp::Type::Int64) { try { res = (std::stoll(s_val) < std::stoll(f.value)); } catch(...) { res = false; } }
            else if (type == lite3cpp::Type::Float64) { try { res = (std::stod(s_val) < std::stod(f.value)); } catch(...) { res = false; } }
            else res = (s_val < f.value);
            break;
        }
        case Query::Op::Le: {
            if (type == lite3cpp::Type::Int64) { try { res = (std::stoll(s_val) <= std::stoll(f.value)); } catch(...) { res = false; } }
            else if (type == lite3cpp::Type::Float64) { try { res = (std::stod(s_val) <= std::stod(f.value)); } catch(...) { res = false; } }
            else res = (s_val <= f.value);
            break;
        }
        case Query::Op::Like: {
            std::string regex_str = "^";
            for (char c : f.value) {
                if (c == '%') regex_str += ".*";
                else if (c == '_') regex_str += ".";
                else if (c == '.' || c == '*' || c == '+' || c == '?' || c == '(' || c == ')' || c == '[' || c == ']' || c == '{' || c == '}' || c == '|') { regex_str += "\\"; regex_str += c; }
                else regex_str += c;
            }
            regex_str += "$";
            try { std::regex re(regex_str, std::regex_constants::icase); res = std::regex_match(s_val, re); } catch (...) { res = false; }
            break;
        }
        default: res = false;
    }
    return res;
}

static bool evaluate_group(const Query::FilterGroup& g, const std::unordered_map<std::string, std::shared_ptr<Node>>& available_nodes, std::string_view current_alias, Engine* engine) {
    if (g.nodes.empty()) return true;
    bool result = true;
    for (const auto& n : g.nodes) {
        bool val = std::visit(overloaded{
            [&](const Query::Filter& f) {
                auto it = available_nodes.find(f.alias);
                if (it != available_nodes.end()) return evaluate_filter(it->second.get(), f, engine);
                return true; 
            },
            [&](const std::shared_ptr<Query::FilterGroup>& sub) { return evaluate_group(*sub, available_nodes, current_alias, engine); }
        }, n.node);
        if (n.prepended_op == Query::LogicalOp::And) { if (&n == &g.nodes.front()) result = val; else result = result && val; }
        else result = result || val;
    }
    return result;
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
Query& Query::return_(std::string_view alias, std::string_view property, AggOp agg) { projections_.push_back(ReturnStep{std::string(alias), std::string(property), agg}); return *this; }

static const Query::Filter* find_first_eq_filter(const Query::FilterGroup& g, std::string_view alias, std::string_view key = "") {
    for (const auto& n : g.nodes) {
        if (auto* f = std::get_if<Query::Filter>(&n.node)) { if (f->alias == alias && f->op == Query::Op::Eq && (key.empty() || f->key == key)) return f; }
        else if (auto* sub = std::get_if<std::shared_ptr<Query::FilterGroup>>(&n.node)) { if (auto* res = find_first_eq_filter(**sub, alias, key)) return res; }
    }
    return nullptr;
}

std::string Query::serialize_steps(const std::vector<Step>& steps) {
    std::stringstream ss; ss << "[";
    for (size_t i = 0; i < steps.size(); ++i) {
        if (i > 0) ss << ",";
        std::visit(overloaded{
            [&](const Query::OutStep& s) { 
                ss << "{\"type\":\"out\",\"label\":\"" << s.label << "\",\"min_weight\":" << s.min_weight << ",\"target_alias\":\"" << s.target_alias << "\"";
                if (!s.source_alias.empty()) ss << ",\"source_alias\":\"" << s.source_alias << "\"";
                ss << "}";
            },
            [&](const Query::InStep& s) { 
                ss << "{\"type\":\"in\",\"label\":\"" << s.label << "\",\"target_alias\":\"" << s.target_alias << "\"";
                if (!s.source_alias.empty()) ss << ",\"source_alias\":\"" << s.source_alias << "\"";
                ss << "}";
            }
        }, steps[i]);
    }
    ss << "]"; return ss.str();
}

std::vector<ResultRow> Query::execute() {
  std::vector<ResultRow> results; std::set<uint64_t> frontier_set;
  if (!starting_nodes_.empty()) {
      for(auto id : starting_nodes_) frontier_set.insert(id);
  } else {
      if (!initial_match_) return results;
      root_alias_ = initial_match_->alias;
      if (auto* f = find_first_eq_filter(root_filters_, root_alias_, "id")) frontier_set.insert(engine_->get_resolver().parse_uuid(f->value));
      if (frontier_set.empty()) {
        for (const auto &n : root_filters_.nodes) {
            if (auto* f = std::get_if<Filter>(&n.node)) {
                if (f->alias == root_alias_ && f->op == Op::Eq) {
                    std::string idx_prefix = "idx:" + f->alias + ":" + f->key + ":" + f->value;
                    auto idx_keys = engine_->get_store()->get_prefix_keys_all_shards(idx_prefix, "", 100);
                    if (!idx_keys.empty()) {
                        for (const auto& k : idx_keys) {
                            auto idx_buf = engine_->get_store()->get(k);
                            if (idx_buf.size() > 0) {
                                std::string id_str(reinterpret_cast<const char*>(idx_buf.data()), idx_buf.size());
                                try { frontier_set.insert(std::stoull(id_str, nullptr, 16)); } catch(...) {}
                            }
                        }
                    }
                }
            }
        }
      }
      if (frontier_set.empty()) {
        std::string store_prefix = "n:{"; 
        auto keys = engine_->get_store()->get_prefix_keys_all_shards(store_prefix, "", engine_->get_settings().prefix_scan_limit);
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

  if (frontier_set.empty()) return results;

  std::vector<uint64_t> frontier(frontier_set.begin(), frontier_set.end());
  {
      std::vector<uint64_t> filtered;
      auto nodes = engine_->fetch_nodes(frontier, principal_id_);
      for (auto& node : nodes) {
          if (!node || !node->is_loaded()) continue;
          std::string key = std::string(KeyBuilder::node_key(node->get_id()));
          auto perm = engine_->get_store()->credentials().check_permission(principal_id_, key);
          if (!(perm & l3kv::Permission::READ) && !(perm & l3kv::Permission::ADMIN)) continue;
          std::unordered_map<std::string, std::shared_ptr<Node>> available; available[root_alias_] = node;
          if (evaluate_group(root_filters_, available, root_alias_, engine_)) filtered.push_back(node->get_id());
      }
      frontier = std::move(filtered);
  }

  if (frontier.empty()) return results;
  
  struct Path { std::unordered_map<std::string, std::shared_ptr<Node>> alias_to_node; std::string last_alias; };
  std::vector<Path> paths;
  for (const auto& id : frontier) {
      auto node = engine_->get_node(id);
      if (!node) continue;
      std::string actual_type;
      try { actual_type = node->get_attribute_as_string("t"); } catch (...) { actual_type = ""; }
      bool type_match = false;
      if (root_alias_ == "Zone") type_match = (actual_type == "zone" || actual_type == "local" || actual_type == "remote");
      else if (root_alias_ == "User") type_match = (actual_type == "user" || actual_type == "rodsuser" || actual_type == "rodsadmin" || actual_type == "groupadmin" || actual_type == "rodsgroup");
      else if (root_alias_ == "Collection") type_match = (actual_type == "collection" || actual_type == "local" || actual_type == "federated" || actual_type == "");
      else if (root_alias_ == "DataObject") type_match = (actual_type == "data_object" || actual_type == "generic");
      else if (root_alias_ == "Replica") type_match = (actual_type == "replica");
      else if (root_alias_ == "Resource") type_match = (actual_type == "resource" || actual_type == "unixfilesystem" || actual_type == "s3");
      else if (root_alias_ == "Metadata") type_match = (actual_type == "metadata");
      else if (root_alias_ == "Access") type_match = (actual_type == "access");
      else type_match = true; // Generic alias, allow any type
      


      if (!type_match) continue;
      Path p; p.alias_to_node[root_alias_] = node; p.last_alias = root_alias_; paths.push_back(std::move(p));
  }

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
                    auto node = path.alias_to_node.at(src);
                    auto neighbors = node->get_neighbors(s.label, s.min_weight, principal_id_);
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
                            Path new_path = path; new_path.alias_to_node[s.target_alias] = neighbor_node; new_path.last_alias = s.target_alias;
                            if (evaluate_group(root_filters_, new_path.alias_to_node, s.target_alias, engine_)) local_next_paths.push_back(std::move(new_path));
                        } catch (...) {}
                    }
                },
                [&](const InStep& s) {
                    std::string src = s.source_alias.empty() ? path.last_alias : s.source_alias;
                    auto node = path.alias_to_node.at(src);
                    auto neighbors = node->get_in_neighbors(s.label, principal_id_);
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
                            Path new_path = path; new_path.alias_to_node[s.target_alias] = neighbor_node; new_path.last_alias = s.target_alias;
                            if (evaluate_group(root_filters_, new_path.alias_to_node, s.target_alias, engine_)) local_next_paths.push_back(std::move(new_path));
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
      std::unordered_map<std::string, std::vector<uint64_t>> groups;
      for (auto& b : branches) {
          json j_sub; j_sub["root_alias"] = b.second.first; j_sub["principal_id"] = principal_id_;
          json j_steps = json::array();
          for (const auto& step : b.second.second) {
              std::visit(overloaded{
                  [&](const OutStep& s) { j_steps.push_back({{"type", "out"}, {"label", s.label}, {"min_weight", s.min_weight}, {"target_alias", s.target_alias}}); },
                  [&](const InStep& s) { j_steps.push_back({{"type", "in"}, {"label", s.label}, {"target_alias", s.target_alias}}); }
              }, step);
          }
          j_sub["steps"] = j_steps; json j_projs = json::array();
          for (const auto& p : projections_) j_projs.push_back({{"alias", p.alias}, {"property", p.property}, {"agg", static_cast<int>(p.agg)}});
          j_sub["projections"] = j_projs; groups[j_sub.dump()].push_back(b.first);
      }
      for (auto& [query_json, nodes] : groups) remote_futures.push_back(engine_->get_remote_client().resume_query_async(cluster_id, nodes, query_json, principal_id_));
  }

  for (const auto &path : paths) {
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
    for (size_t i = 0; i < projections_.size(); ++i) { if (!row.fields.contains("idx_" + std::to_string(i))) row.fields["idx_" + std::to_string(i)] = ""; }
    results.push_back(std::move(row));
  }
  for (auto& f : remote_futures) {
      auto remote_res = f.get();
      results.insert(results.end(), remote_res.begin(), remote_res.end());
  }

  bool has_agg = false; for (const auto& p : projections_) if (p.agg != AggOp::None) { has_agg = true; break; }
  if (has_agg) {
      std::unordered_map<std::string, std::vector<ResultRow>> partitions;
      if (groups_.empty()) partitions["ALL"] = std::move(results);
      else {
          for (auto& row : results) {
              std::string g_key;
              for (const auto& g : groups_) {
                  auto it = row.fields.find(g.alias + "." + g.property);
                  if (it != row.fields.end()) g_key += it->second + "|"; else g_key += "|";
              }
              partitions[g_key].push_back(std::move(row));
          }
      }
      std::vector<ResultRow> final_res;
      for (auto& pair : partitions) {
          auto& part = pair.second; ResultRow agg_row;
          for (size_t i = 0; i < projections_.size(); ++i) {
              const auto& p = projections_[i]; std::string k = "idx_" + std::to_string(i);
              if (p.agg == AggOp::None) { if (!part.empty()) agg_row.fields[k] = part[0].fields.at(k); }
              else if (p.agg == AggOp::Count) agg_row.fields[k] = std::to_string(part.size());
              else {
                  double acc = 0; bool first = true;
                  for (auto& r : part) {
                      double v = 0; try { v = std::stod(std::string(r.fields.at(k))); } catch(...) {}
                      if (first) { acc = v; first = false; }
                      else {
                          if (p.agg == AggOp::Sum || p.agg == AggOp::Avg) acc += v;
                          else if (p.agg == AggOp::Min) acc = std::min(acc, v);
                          else if (p.agg == AggOp::Max) acc = std::max(acc, v);
                      }
                  }
                  if (p.agg == AggOp::Avg && !part.empty()) acc /= part.size();
                  if (acc == static_cast<int64_t>(acc)) agg_row.fields[k] = std::to_string(static_cast<int64_t>(acc));
                  else agg_row.fields[k] = std::to_string(acc);
              }
          }
          final_res.push_back(std::move(agg_row));
      }
      results = std::move(final_res);
  }

  if (distinct_ || (!projections_.empty() && projections_[0].agg == AggOp::None)) {
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

  if (!sorts_.empty()) {
      std::sort(results.begin(), results.end(), [&](const ResultRow& a, const ResultRow& b) {
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
          return false;
      });
  }
  if (offset_) { if (*offset_ >= results.size()) results.clear(); else results.erase(results.begin(), results.begin() + *offset_); }
  if (limit_ && results.size() > *limit_) results.resize(*limit_);
  return results;
}

Query &Query::resume(const std::vector<uint64_t>& starting_nodes, std::string_view query_json) {
    starting_nodes_ = starting_nodes; is_federated_branch_ = true;
    try {
        json j = json::parse(query_json);
        if (j.contains("root_alias")) { root_alias_ = j["root_alias"]; initial_match_ = {root_alias_}; }
        if (j.contains("principal_id")) { principal_id_ = j["principal_id"]; }
        if (j.contains("steps")) {
            for (const auto& sj : j["steps"]) {
                std::string type = sj["type"];
                std::string src = sj.value("source_alias", "");
                if (type == "out") steps_.push_back(OutStep{sj["label"], sj["min_weight"], sj["target_alias"], src});
                else if (type == "in") steps_.push_back(InStep{sj["label"], sj["target_alias"], src});
            }
        }
        if (j.contains("projections")) {
            for (const auto& pj : j["projections"]) projections_.push_back(ReturnStep{pj["alias"], pj["property"], static_cast<AggOp>(pj["agg"])});
        }
        if (j.contains("filters")) {
            for (const auto& fj : j["filters"]) root_filters_.where(fj["alias"].get<std::string>(), fj["key"].get<std::string>(), static_cast<Op>(fj["op"].get<int>()), fj["value"].get<std::string>());
        }
    } catch (...) {}
    return *this;
}

} // namespace l3kvg
