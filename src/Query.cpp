#include "L3KVG/Query.hpp"
#include "L3KVG/Node.hpp"
#include "L3KVG/Engine.hpp"
#include "engine/store.hpp"
#include <variant>
#include <regex>
#include <iostream>
#include <iomanip>

#ifdef IRODS_SERVER
#include "irods/rodsLog.h"
#define L3_LOG(level, ...) rodsLog(level, __VA_ARGS__)
#else
#include <cstdio>
#define L3_LOG(level, ...) std::fprintf(stderr, "[L3KVG] " __VA_ARGS__); std::fprintf(stderr, "\n")
#endif

namespace l3kvg {

template<class... Ts> struct overloaded : Ts... { using Ts::operator()...; };
template<class... Ts> overloaded(Ts...) -> overloaded<Ts...>;

static bool evaluate_filter(Node* node, const Query::Filter& f) {
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

static bool evaluate_group(const Query::FilterGroup& g, const std::unordered_map<std::string, std::shared_ptr<Node>>& available_nodes, std::string_view current_alias) {
    if (g.nodes.empty()) return true;
    bool result = true;
    for (const auto& n : g.nodes) {
        bool val = std::visit(overloaded{
            [&](const Query::Filter& f) {
                auto it = available_nodes.find(f.alias);
                if (it != available_nodes.end()) return evaluate_filter(it->second.get(), f);
                return true; 
            },
            [&](const std::shared_ptr<Query::FilterGroup>& sub) { return evaluate_group(*sub, available_nodes, current_alias); }
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
  q_.steps_.push_back(OutStep{label_, weight_, std::string(dest_alias)});
  return q_;
}

Query::InEdgeBuilder Query::in(std::string_view edge_label) {
  return InEdgeBuilder(*this, edge_label);
}

Query &Query::InEdgeBuilder::as(std::string_view dest_alias) {
  q_.steps_.push_back(InStep{label_, std::string(dest_alias)});
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

std::vector<Query::ResultRow> Query::execute() {
  std::vector<ResultRow> results; if (!initial_match_) return results;
  std::vector<std::string> frontier;
  if (auto* f = find_first_eq_filter(root_filters_, initial_match_->alias, "id")) frontier.push_back(f->value);
  if (frontier.empty()) {
    for (const auto &n : root_filters_.nodes) {
        if (auto* f = std::get_if<Filter>(&n.node)) {
            if (f->alias == initial_match_->alias && f->op == Op::Eq) {
                std::string idx_key = "idx:" + f->alias + ":" + f->key + ":" + f->value;
                auto idx_node = engine_->get_node(idx_key);
                if (idx_node && idx_node->has_attribute("id")) {
                    frontier.push_back(idx_node->get_attribute_as_string("id")); break;
                }
            }
        }
    }
  }
  if (frontier.empty()) {
    std::string inner_prefix = "idx:" + initial_match_->alias + ":id:";
    std::string store_prefix = "n:{" + inner_prefix; 
    auto keys = engine_->get_store()->get_prefix_keys_all_shards(store_prefix, "", 1000);
    for (const auto& k : keys) {
        std::string uuid = k.substr(3, k.size() - 4);
        auto idx_node = engine_->get_node(uuid);
        if (idx_node && idx_node->has_attribute("id")) frontier.push_back(idx_node->get_attribute_as_string("id"));
    }
  }
  if (frontier.empty()) return results;
  {
      std::vector<std::string> filtered;
      for (const auto& uuid : frontier) {
          auto node = engine_->get_node(uuid);
          if (!node) continue;
          std::unordered_map<std::string, std::shared_ptr<Node>> available;
          available[initial_match_->alias] = node;
          if (evaluate_group(root_filters_, available, initial_match_->alias)) filtered.push_back(uuid);
      }
      frontier = std::move(filtered);
  }
  if (frontier.empty()) return results;
  struct Path { std::unordered_map<std::string, std::shared_ptr<Node>> alias_to_node; std::string last_alias; };
  std::vector<Path> paths;
  for (const auto& uuid : frontier) { Path p; p.alias_to_node[initial_match_->alias] = engine_->get_node(uuid); p.last_alias = initial_match_->alias; paths.push_back(std::move(p)); }

  for (const auto &step : steps_) {
    std::vector<Path> next_paths;
    std::visit(overloaded{
        [&](const OutStep& s) {
            for (const auto &path : paths) {
                auto neighbors = path.alias_to_node.at(path.last_alias)->get_neighbors(s.label, s.min_weight);
                for (const auto& neighbor_uuid : neighbors) {
                    auto neighbor_node = engine_->get_node(neighbor_uuid);
                    if (!neighbor_node) continue;
                    Path new_path = path; new_path.alias_to_node[s.target_alias] = neighbor_node; new_path.last_alias = s.target_alias;
                    if (evaluate_group(root_filters_, new_path.alias_to_node, s.target_alias)) next_paths.push_back(std::move(new_path));
                }
            }
        },
        [&](const InStep& s) {
            for (const auto &path : paths) {
                auto neighbors = path.alias_to_node.at(path.last_alias)->get_in_neighbors(s.label);
                for (const auto& neighbor_uuid : neighbors) {
                    auto neighbor_node = engine_->get_node(neighbor_uuid);
                    if (!neighbor_node) continue;
                    Path new_path = path; new_path.alias_to_node[s.target_alias] = neighbor_node; new_path.last_alias = s.target_alias;
                    if (evaluate_group(root_filters_, new_path.alias_to_node, s.target_alias)) next_paths.push_back(std::move(new_path));
                }
            }
        }
    }, step);
    paths = std::move(next_paths);
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
                  double val = 0; bool first = true;
                  for (const auto& r : part) {
                      double rv = 0; try { rv = std::stod(r.fields.at(k)); } catch(...) {}
                      if (first) { val = rv; first = false; }
                      else {
                          if (p.agg == AggOp::Sum || p.agg == AggOp::Avg) val += rv;
                          else if (p.agg == AggOp::Min) val = std::min(val, rv);
                          else if (p.agg == AggOp::Max) val = std::max(val, rv);
                      }
                  }
                  if (p.agg == AggOp::Avg && !part.empty()) val /= part.size();
                  std::stringstream ss; if (val == (long long)val) ss << (long long)val; else ss << std::fixed << std::setprecision(2) << val;
                  agg_row.fields[k] = ss.str();
              }
          }
          final_res.push_back(std::move(agg_row));
      }
      if (final_res.empty() && groups_.empty()) {
          ResultRow agg_row;
          for (size_t i = 0; i < projections_.size(); ++i) {
              if (projections_[i].agg == AggOp::Count) agg_row.fields[std::to_string(i)] = "0";
              else agg_row.fields[std::to_string(i)] = "";
          }
          final_res.push_back(std::move(agg_row));
      }
      return final_res;
  }

  if (!sorts_.empty()) {
      std::sort(results.begin(), results.end(), [&](const ResultRow& a, const ResultRow& b) {
          for (const auto& s : sorts_) {
              std::string k = s.alias + "." + s.property;
              auto it_a = a.fields.find(k); auto it_b = b.fields.find(k);
              if (it_a == a.fields.end() || it_b == b.fields.end()) continue;
              if (it_a->second == it_b->second) continue;
              try {
                  double d_a = std::stod(it_a->second); double d_b = std::stod(it_b->second);
                  if (s.ascending) return d_a < d_b; return d_a > d_b;
              } catch(...) { if (s.ascending) return it_a->second < it_b->second; return it_a->second > it_b->second; }
          }
          return false;
      });
  }

  if (offset_.has_value() || limit_.has_value()) {
      size_t start = offset_.value_or(0); if (start >= results.size()) return {};
      size_t end = results.size(); if (limit_.has_value()) end = std::min(end, start + limit_.value());
      std::vector<ResultRow> sliced; for (size_t i = start; i < end; ++i) sliced.push_back(std::move(results[i]));
      return sliced;
  }
  return results;
}

} // namespace l3kvg
