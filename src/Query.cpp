#include "L3KVG/Query.hpp"
#include "L3KVG/Node.hpp"
#include "L3KVG/Engine.hpp"
#include "L3KVG/FederationID.hpp"
#include "engine/store.hpp"
#include <variant>
#include <regex>
#include <iostream>
#include <iomanip>
#include <unordered_set>
#include <set>
#include <algorithm>
#include <sstream>

#ifdef IRODS_SERVER
#include "irods/rodsLog.h"
#define L3_LOG(level, ...) rodsLog(level, __VA_ARGS__)
#else
#include <cstdio>
#define L3_LOG(level, ...) std::fprintf(stderr, "[L3KVG] " __VA_ARGS__); std::fprintf(stderr, "\n")
#endif

#include <nlohmann/json.hpp>

using json = nlohmann::json;

namespace l3kvg {

template<class... Ts> struct overloaded : Ts... { using Ts::operator()...; };
template<class... Ts> overloaded(Ts...) -> overloaded<Ts...>;

static bool evaluate_filter(Node* node, const Query::Filter& f, Engine* engine) {
    if (f.key == "id") {
        uint64_t target_id = engine->get_resolver().parse_uuid(f.value);
        uint64_t node_id = node->get_id();
        switch (f.op) {
            case Query::Op::Eq: return node_id == target_id;
            case Query::Op::Ne: return node_id != target_id;
            case Query::Op::Gt: return node_id > target_id;
            case Query::Op::Ge: return node_id >= target_id;
            case Query::Op::Lt: return node_id < target_id;
            case Query::Op::Le: return node_id <= target_id;
            default: return false;
        }
    }

    if (!node->has_attribute(f.key)) return false;
    auto type = node->get_attribute_type(f.key);
    std::string s_val = node->get_attribute_as_string(f.key);
    switch (f.op) {
        case Query::Op::Eq: return s_val == f.value;
        case Query::Op::Ne: return s_val != f.value;
        case Query::Op::Gt: 
            if (type == lite3cpp::Type::Int64 || type == lite3cpp::Type::Float64) return std::stod(s_val) > std::stod(f.value);
            return s_val > f.value;
        case Query::Op::Ge:
            if (type == lite3cpp::Type::Int64 || type == lite3cpp::Type::Float64) return std::stod(s_val) >= std::stod(f.value);
            return s_val >= f.value;
        case Query::Op::Lt:
            if (type == lite3cpp::Type::Int64 || type == lite3cpp::Type::Float64) return std::stod(s_val) < std::stod(f.value);
            return s_val < f.value;
        case Query::Op::Le:
            if (type == lite3cpp::Type::Int64 || type == lite3cpp::Type::Float64) return std::stod(s_val) <= std::stod(f.value);
            return s_val <= f.value;
        case Query::Op::Like: {
            std::string pattern = f.value; size_t pos = 0;
            while ((pos = pattern.find('%', pos)) != std::string::npos) { pattern.replace(pos, 1, ".*"); pos += 2; }
            pos = 0; while ((pos = pattern.find('_', pos)) != std::string::npos) { pattern.replace(pos, 1, "."); pos += 1; }
            try { std::regex re(pattern); return std::regex_match(s_val, re); } catch(...) { return false; }
        }
    }
    return false;
}

Query::Query(Engine *engine) : engine_(engine) {}

Query &Query::match(std::string_view node_alias) {
  initial_match_ = MatchStep{std::string(node_alias)};
  root_alias_ = std::string(node_alias);
  return *this;
}

Query &Query::match_id(uint64_t id, std::string_view alias) {
    starting_nodes_ = {id};
    initial_match_ = MatchStep{std::string(alias)};
    root_alias_ = std::string(alias);
    return *this;
}

Query::FilterGroup& Query::FilterGroup::where(std::string_view alias, std::string_view key, Query::Op op, std::string_view value) {
    nodes.push_back({Query::Filter{std::string(alias), std::string(key), op, std::string(value)}, Query::LogicalOp::And});
    return *this;
}

Query::FilterGroup& Query::FilterGroup::or_where(std::string_view alias, std::string_view key, Query::Op op, std::string_view value) {
    nodes.push_back({Query::Filter{std::string(alias), std::string(key), op, std::string(value)}, Query::LogicalOp::Or});
    return *this;
}

Query::FilterGroup& Query::FilterGroup::where_group(std::function<void(FilterGroup&)> cb) {
    auto group = std::make_shared<FilterGroup>();
    cb(*group);
    nodes.push_back({group, Query::LogicalOp::And});
    return *this;
}

Query::FilterGroup& Query::FilterGroup::or_where_group(std::function<void(FilterGroup&)> cb) {
    auto group = std::make_shared<FilterGroup>();
    cb(*group);
    nodes.push_back({group, Query::LogicalOp::Or});
    return *this;
}

Query &Query::where(std::string_view alias, std::string_view key, Op op, std::string_view value) {
  root_filters_.where(alias, key, op, value);
  return *this;
}

Query &Query::or_where(std::string_view alias, std::string_view key, Op op, std::string_view value) {
  root_filters_.or_where(alias, key, op, value);
  return *this;
}

Query &Query::where_group(std::function<void(FilterGroup&)> cb) {
  root_filters_.where_group(cb);
  return *this;
}

Query &Query::or_where_group(std::function<void(FilterGroup&)> cb) {
  root_filters_.or_where_group(cb);
  return *this;
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
        if (n.prepended_op == Query::LogicalOp::And) {
            if (&n == &g.nodes.front()) result = val; else result = result && val;
        } else result = result || val;
    }
    return result;
}

Query &Query::where_has(std::string_view alias, std::string_view key, std::string_view value_type) {
  filters_has_.push_back({std::string(alias), std::string(key), std::string(value_type)});
  return *this;
}

Query::OutEdgeBuilder Query::out(std::string_view edge_label, double min_weight) {
  return OutEdgeBuilder(*this, edge_label, min_weight);
}

Query &Query::OutEdgeBuilder::as(std::string_view dest_alias) {
  q_.steps_.push_back(Query::OutStep{std::string(label_), weight_, std::string(dest_alias)});
  return q_;
}

Query::InEdgeBuilder Query::in(std::string_view edge_label) {
  return InEdgeBuilder(*this, edge_label);
}

Query &Query::InEdgeBuilder::as(std::string_view dest_alias) {
  q_.steps_.push_back(Query::InStep{std::string(label_), std::string(dest_alias)});
  return q_;
}

Query &Query::return_(std::string_view alias, std::string_view property, AggOp agg) {
  projections_.push_back({std::string(alias), std::string(property), agg});
  return *this;
}

Query &Query::order_by(std::string_view alias, std::string_view property, bool ascending) {
    sorts_.push_back({std::string(alias), std::string(property), ascending});
    return *this;
}

Query &Query::limit(size_t limit) { limit_ = limit; return *this; }
Query &Query::offset(size_t offset) { offset_ = offset; return *this; }
Query &Query::group_by(std::string_view alias, std::string_view property) {
    groups_.push_back({std::string(alias), std::string(property)});
    return *this;
}

Query &Query::distinct(bool enable) {
    distinct_ = enable;
    return *this;
}

static const Query::Filter* find_first_eq_filter(const Query::FilterGroup& g, std::string_view alias, std::string_view key = "") {
    for (const auto& n : g.nodes) {
        if (auto* f = std::get_if<Query::Filter>(&n.node)) {
            if (f->alias == alias && f->op == Query::Op::Eq && (key.empty() || f->key == key)) return f;
        } else if (auto* sub = std::get_if<std::shared_ptr<Query::FilterGroup>>(&n.node)) {
            if (auto* res = find_first_eq_filter(**sub, alias, key)) return res;
        }
    }
    return nullptr;
}

std::string Query::serialize_steps(const std::vector<Step>& steps) {
    std::stringstream ss;
    ss << "[";
    for (size_t i = 0; i < steps.size(); ++i) {
        if (i > 0) ss << ",";
        std::visit(overloaded{
            [&](const Query::OutStep& s) {
                ss << "{\"type\":\"out\",\"label\":\"" << s.label 
                   << "\",\"min_weight\":" << s.min_weight 
                   << ",\"target_alias\":\"" << s.target_alias << "\"}";
            },
            [&](const Query::InStep& s) {
                ss << "{\"type\":\"in\",\"label\":\"" << s.label 
                   << "\",\"target_alias\":\"" << s.target_alias << "\"}";
            }
        }, steps[i]);
    }
    ss << "]";
    return ss.str();
}

std::vector<ResultRow> Query::execute() {
  std::vector<ResultRow> results; 
  std::vector<uint64_t> frontier;

  if (!starting_nodes_.empty()) {
      frontier = starting_nodes_;
  } else {
      if (!initial_match_) return results;
      root_alias_ = initial_match_->alias;
      
      if (auto* f = find_first_eq_filter(root_filters_, root_alias_, "id")) {
          frontier.push_back(engine_->get_resolver().parse_uuid(f->value));
      }
      
      if (frontier.empty()) {
        for (const auto &n : root_filters_.nodes) {
            if (auto* f = std::get_if<Filter>(&n.node)) {
                if (f->alias == root_alias_ && f->op == Op::Eq) {
                    // Secondary index lookup: idx:alias:key:value -> node_id
                    std::string idx_key = "idx:" + f->alias + ":" + f->key + ":" + f->value;
                    auto idx_buf = engine_->get_store()->get(idx_key);
                    if (idx_buf.size() > 0) {
                        std::string id_str(reinterpret_cast<const char*>(idx_buf.data()), idx_buf.size());
                        try {
                            frontier.push_back(std::stoull(id_str, nullptr, 16));
                            break;
                        } catch(...) {}
                    }
                }
            }
        }
      }
      
      if (frontier.empty()) {
        // Global scan fallback
        std::string store_prefix = "n:{"; 
        auto keys = engine_->get_store()->get_prefix_keys_all_shards(store_prefix, "", engine_->get_settings().prefix_scan_limit);
        for (const auto& k : keys) {
            if (k.size() >= 19) {
                std::string id_str = k.substr(3, k.size() - 4);
                try {
                    frontier.push_back(std::stoull(id_str, nullptr, 16));
                } catch(...) {}
            }
        }
      }
  }

  if (frontier.empty()) return results;

  {
      std::vector<uint64_t> filtered;
      // Use principal_id_ for initial node fetch
      auto nodes = engine_->fetch_nodes(frontier, principal_id_);
      for (auto& node : nodes) {
          if (!node || !node->is_loaded()) continue;
          
          // Re-verify ACL even if node was already cached/loaded
          std::string key = std::string(KeyBuilder::node_key(node->get_id()));
          auto perm = engine_->get_store()->credentials().check_permission(principal_id_, key);
          if (!(perm & l3kv::Permission::READ) && !(perm & l3kv::Permission::ADMIN)) {
              continue;
          }

          std::unordered_map<std::string, std::shared_ptr<Node>> available;
          available[root_alias_] = node;
          if (evaluate_group(root_filters_, available, root_alias_, engine_)) filtered.push_back(node->get_id());
      }
      frontier = std::move(filtered);
  }

  if (frontier.empty()) return results;
  struct Path { std::unordered_map<std::string, std::shared_ptr<Node>> alias_to_node; std::string last_alias; };
  std::vector<Path> paths;
  for (const auto& id : frontier) { Path p; p.alias_to_node[root_alias_] = engine_->get_node(id); p.last_alias = root_alias_; paths.push_back(std::move(p)); }

  std::unordered_map<uint16_t, std::vector<std::pair<uint64_t, std::pair<std::string, std::vector<Step>>>>> suspended_branches;

  for (size_t i = 0; i < steps_.size(); ++i) {
    const auto& step = steps_[i];
    std::vector<Path> next_paths;
    std::visit(overloaded{
        [&](const OutStep& s) {
            for (const auto &path : paths) {
                auto node = path.alias_to_node.at(path.last_alias);
                auto neighbors = node->get_neighbors(s.label, s.min_weight, principal_id_);
                std::unordered_set<uint64_t> unique_neighbors(neighbors.begin(), neighbors.end());
                for (const auto& neighbor_id : unique_neighbors) {
                    uint16_t cluster_id = FederationID::get_cluster(neighbor_id);
                    if (!engine_->get_resolver().is_local_cluster(cluster_id)) {
                        std::vector<Step> remaining(steps_.begin() + i + 1, steps_.end());
                        suspended_branches[cluster_id].push_back({neighbor_id, {s.target_alias, remaining}});
                        continue;
                    }
                    auto neighbor_node = engine_->get_node(neighbor_id);
                    if (!neighbor_node) continue;
                    Path new_path = path; new_path.alias_to_node[s.target_alias] = neighbor_node; new_path.last_alias = s.target_alias;
                    if (evaluate_group(root_filters_, new_path.alias_to_node, s.target_alias, engine_)) next_paths.push_back(std::move(new_path));
                }
            }
        },
        [&](const InStep& s) {
            for (const auto &path : paths) {
                auto node = path.alias_to_node.at(path.last_alias);
                auto neighbors = node->get_in_neighbors(s.label, principal_id_);
                std::unordered_set<uint64_t> unique_neighbors(neighbors.begin(), neighbors.end());
                for (const auto& neighbor_id : unique_neighbors) {
                    uint16_t cluster_id = FederationID::get_cluster(neighbor_id);
                    if (!engine_->get_resolver().is_local_cluster(cluster_id)) {
                        std::vector<Step> remaining(steps_.begin() + i + 1, steps_.end());
                        suspended_branches[cluster_id].push_back({neighbor_id, {s.target_alias, remaining}});
                        continue;
                    }
                    auto neighbor_node = engine_->get_node(neighbor_id);
                    if (!neighbor_node) continue;
                    Path new_path = path; new_path.alias_to_node[s.target_alias] = neighbor_node; new_path.last_alias = s.target_alias;
                    if (evaluate_group(root_filters_, new_path.alias_to_node, s.target_alias, engine_)) next_paths.push_back(std::move(new_path));
                }
            }
        }
    }, step);
    paths = std::move(next_paths);
    if (paths.empty() && suspended_branches.empty()) break;
  }

  std::vector<std::future<std::vector<ResultRow>>> remote_futures;
  for (auto& [cluster_id, branches] : suspended_branches) {
      std::unordered_map<std::string, std::vector<uint64_t>> groups;
      for (auto& b : branches) {
          json j_sub;
          j_sub["root_alias"] = b.second.first;
          j_sub["principal_id"] = principal_id_;
          json j_steps = json::array();
          for (const auto& step : b.second.second) {
              std::visit(overloaded{
                  [&](const OutStep& s) {
                      j_steps.push_back({{"type", "out"}, {"label", s.label}, {"min_weight", s.min_weight}, {"target_alias", s.target_alias}});
                  },
                  [&](const InStep& s) {
                      j_steps.push_back({{"type", "in"}, {"label", s.label}, {"target_alias", s.target_alias}});
                  }
              }, step);
          }
          j_sub["steps"] = j_steps;
          
          json j_projs = json::array();
          for (const auto& p : projections_) {
              j_projs.push_back({{"alias", p.alias}, {"property", p.property}, {"agg", static_cast<int>(p.agg)}});
          }
          j_sub["projections"] = j_projs;
          
          groups[j_sub.dump()].push_back(b.first);
      }
      for (auto& [query_json, nodes] : groups) {
          remote_futures.push_back(engine_->get_remote_client().resume_query_async(cluster_id, nodes, query_json, principal_id_));
      }
  }

  for (const auto &path : paths) {
    ResultRow row;
    for (const auto& [alias, node] : path.alias_to_node) {
        row.nodes.push_back(node);
        for (size_t i = 0; i < projections_.size(); ++i) {
            if (projections_[i].alias == alias) {
                row.fields[alias + "." + projections_[i].property] = node->get_attribute_as_string(projections_[i].property);
                row.fields[std::to_string(i)] = row.fields[alias + "." + projections_[i].property];
            }
        }
        for (const auto& s : sorts_) {
            if (s.alias == alias) {
                std::string k = s.alias + "." + s.property;
                if (row.fields.find(k) == row.fields.end()) row.fields[k] = node->get_attribute_as_string(s.property);
            }
        }
        for (const auto& g : groups_) {
            if (g.alias == alias) {
                std::string k = g.alias + "." + g.property;
                if (row.fields.find(k) == row.fields.end()) row.fields[k] = node->get_attribute_as_string(g.property);
            }
        }
    }
    for (size_t i = 0; i < projections_.size(); ++i) { if (!row.fields.contains(std::to_string(i))) row.fields[std::to_string(i)] = ""; }
    results.push_back(std::move(row));
  }

  // Wait and Merge remote results
  for (auto& f : remote_futures) {
      try {
          auto remote_res = f.get();
          results.insert(results.end(), remote_res.begin(), remote_res.end());
      } catch (const std::exception& e) {
          if (!is_federated_branch_) throw; // Re-throw if top-level user query
      }
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
              const auto& p = projections_[i]; std::string k = std::to_string(i);
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
                  if (acc == static_cast<int64_t>(acc)) {
                      agg_row.fields[k] = std::to_string(static_cast<int64_t>(acc));
                  } else {
                      agg_row.fields[k] = std::to_string(acc);
                  }
              }
          }
          final_res.push_back(std::move(agg_row));
      }
      results = std::move(final_res);
  }

  if (distinct_) {
      std::set<std::string> seen; std::vector<ResultRow> unique_res;
      for (auto& row : results) {
          std::string key; for (size_t i = 0; i < projections_.size(); ++i) key += std::string(row.fields.at(std::to_string(i))) + "|";
          if (seen.find(key) == seen.end()) { seen.insert(key); unique_res.push_back(std::move(row)); }
      }
      results = std::move(unique_res);
  }

  // Final sort
  if (!sorts_.empty()) {
      std::sort(results.begin(), results.end(), [&](const ResultRow& a, const ResultRow& b) {
          for (const auto& s : sorts_) {
              std::string k = s.alias + "." + s.property;
              if (a.fields.at(k) != b.fields.at(k)) {
                  if (s.ascending) return a.fields.at(k) < b.fields.at(k);
                  return a.fields.at(k) > b.fields.at(k);
              }
          }
          return false;
      });
  }

  if (offset_) {
      if (*offset_ >= results.size()) results.clear();
      else results.erase(results.begin(), results.begin() + *offset_);
  }
  if (limit_ && results.size() > *limit_) results.resize(*limit_);

  return results;
}

Query &Query::resume(const std::vector<uint64_t>& starting_nodes, std::string_view query_json) {
    starting_nodes_ = starting_nodes;
    is_federated_branch_ = true;

    try {

        json j = json::parse(query_json);
        if (j.contains("root_alias")) {
            root_alias_ = j["root_alias"];
        }
        if (j.contains("principal_id")) {
            principal_id_ = j["principal_id"];
        }
        if (j.contains("steps")) {
            for (const auto& sj : j["steps"]) {
                std::string type = sj["type"];
                if (type == "out") {
                    steps_.push_back(OutStep{sj["label"], sj["min_weight"], sj["target_alias"]});
                } else if (type == "in") {
                    steps_.push_back(InStep{sj["label"], sj["target_alias"]});
                }
            }
        }
        if (j.contains("projections")) {
            for (const auto& pj : j["projections"]) {
                projections_.push_back(ReturnStep{pj["alias"], pj["property"], static_cast<AggOp>(pj["agg"])});
            }
        }
    } catch (...) {}
    return *this;
}

} // namespace l3kvg
