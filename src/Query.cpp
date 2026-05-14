#include "L3KVG/Query.hpp"
#include "L3KVG/Node.hpp"
#include "L3KVG/Engine.hpp"
#include "engine/store.hpp"
#include <variant>
#include <regex>
#include <iostream>

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
        case Query::Op::Eq: 
            return s_val == f.value;
        case Query::Op::Ne: 
            return s_val != f.value;
        case Query::Op::Gt: 
            if (type == lite3cpp::Type::Int64 || type == lite3cpp::Type::Float64) {
                return std::stod(s_val) > std::stod(f.value);
            }
            return s_val > f.value;
        case Query::Op::Ge:
            if (type == lite3cpp::Type::Int64 || type == lite3cpp::Type::Float64) {
                return std::stod(s_val) >= std::stod(f.value);
            }
            return s_val >= f.value;
        case Query::Op::Lt:
            if (type == lite3cpp::Type::Int64 || type == lite3cpp::Type::Float64) {
                return std::stod(s_val) < std::stod(f.value);
            }
            return s_val < f.value;
        case Query::Op::Le:
            if (type == lite3cpp::Type::Int64 || type == lite3cpp::Type::Float64) {
                return std::stod(s_val) <= std::stod(f.value);
            }
            return s_val <= f.value;
        case Query::Op::Like: {
            std::string pattern = f.value;
            size_t pos = 0;
            while ((pos = pattern.find('%', pos)) != std::string::npos) {
                pattern.replace(pos, 1, ".*");
                pos += 2;
            }
            pos = 0;
            while ((pos = pattern.find('_', pos)) != std::string::npos) {
                pattern.replace(pos, 1, ".");
                pos += 1;
            }
            try {
                std::regex re(pattern);
                return std::regex_match(std::string(s_val), re);
            } catch(...) { return false; }
        }
    }
    return false;
}

Query::Query(Engine *engine) : engine_(engine) {}

Query &Query::match(std::string_view node_alias) {
  initial_match_ = MatchStep{std::string(node_alias)};
  return *this;
}

Query &Query::where_has(std::string_view alias, std::string_view key,
                        std::string_view value_type) {
  filters_has_.push_back(
      {std::string(alias), std::string(key), std::string(value_type)});
  return *this;
}

Query &Query::where(std::string_view alias, std::string_view key, Op op,
                    std::string_view value) {
  filters_.push_back({std::string(alias), std::string(key), op, std::string(value)});
  return *this;
}

Query::OutEdgeBuilder Query::out(std::string_view edge_label,
                                 double min_weight) {
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

Query &Query::return_(std::string_view alias, std::string_view property) {
  projections_.push_back({std::string(alias), std::string(property)});
  return *this;
}

std::vector<Query::ResultRow> Query::execute() {
  std::vector<ResultRow> results;
  if (!initial_match_)
    return results;

  std::vector<std::string> frontier;

  // Naive index lookup: requires `id` exact match on the root node
  for (const auto &f : filters_) {
    if (f.alias == initial_match_->alias && f.key == "id" && f.op == Op::Eq) {
      L3_LOG(LOG_DEBUG, "L3KVG: Query - ID lookup for [%s]: %s", f.alias.c_str(), f.value.c_str());
      frontier.push_back(f.value);
      break;
    }
  }

  if (frontier.empty()) {
    // Try secondary index lookup if available (Only for Equality)
    for (const auto &f : filters_) {
      if (f.alias == initial_match_->alias && f.op == Op::Eq) {
        std::string idx_key = "idx:" + f.alias + ":" + f.key + ":" + f.value;
        L3_LOG(LOG_DEBUG, "L3KVG: Query - Secondary Index lookup: %s", idx_key.c_str());
        auto idx_node = engine_->get_node(idx_key);
        if (idx_node && idx_node->has_attribute("id")) {
          std::string target_id = idx_node->get_attribute_as_string("id");
          L3_LOG(LOG_DEBUG, "L3KVG: Query - Resolved index to ID: %s", target_id.c_str());
          frontier.push_back(target_id);
          break;
        }
      }
    }
  }

  if (frontier.empty()) {
    // FALLBACK: Full Scan by Prefix for Prototype
    std::string inner_prefix = "idx:" + initial_match_->alias + ":id:";
    std::string store_prefix = "n:{" + inner_prefix; 
    L3_LOG(LOG_DEBUG, "L3KVG: Query - Falling back to prefix scan for [%s] using prefix [%s]", initial_match_->alias.c_str(), store_prefix.c_str());
    // We scan the id index prefix which should exist for major types
    auto keys = engine_->get_store()->get_prefix_keys_all_shards(store_prefix, "", 1000);
    for (const auto& k : keys) {
        // k is n:{uuid}
        std::string uuid = k.substr(3, k.size() - 4);
        auto idx_node = engine_->get_node(uuid);
        if (idx_node && idx_node->has_attribute("id")) {
             frontier.push_back(idx_node->get_attribute_as_string("id"));
        }
    }
  }

  if (frontier.empty()) {
    return results;
  }

  // Filter the initial frontier
  {
      std::vector<std::string> filtered;
      L3_LOG(LOG_DEBUG, "L3KVG: Query - Filtering initial frontier of size %zu", frontier.size());
      for (const auto& uuid : frontier) {
          auto node = engine_->get_node(uuid);
          if (!node) continue;
          bool all_pass = true;
          for (const auto& f : filters_) {
              if (f.alias == initial_match_->alias) {
                  bool pass = evaluate_filter(node.get(), f);
                  L3_LOG(LOG_DEBUG, "L3KVG: Query - Alias [%s] Filter [%s %d %s] on node [%s] results in: %s", 
                         f.alias.c_str(), f.key.c_str(), (int)f.op, f.value.c_str(), uuid.c_str(), pass ? "PASS" : "FAIL");
                  if (!pass) {
                      all_pass = false;
                      break;
                  }
              }
          }
          if (all_pass) filtered.push_back(uuid);
      }
      frontier = std::move(filtered);
  }

  if (frontier.empty()) return results;
  
  // Process linear traversals and keep track of aliases
  struct Path {
      std::unordered_map<std::string, std::string> alias_to_uuid;
      std::string last_alias;
  };
  std::vector<Path> paths;
  for (const auto& uuid : frontier) {
      Path p;
      p.alias_to_uuid[initial_match_->alias] = uuid;
      p.last_alias = initial_match_->alias;
      paths.push_back(std::move(p));
  }

  for (const auto &step : steps_) {
    std::vector<Path> next_paths;
    
    std::visit(overloaded{
        [&](const OutStep& s) {
            for (const auto &path : paths) {
                auto it = path.alias_to_uuid.find(path.last_alias);
                if (it == path.alias_to_uuid.end()) continue;

                auto node = engine_->get_node(it->second);
                auto neighbors = node->get_neighbors(s.label, s.min_weight);
                
                L3_LOG(LOG_DEBUG, "L3KVG: Query - Traversal [%s] -> [%s] found %zu neighbors", 
                       path.last_alias.c_str(), s.target_alias.c_str(), neighbors.size());

                for (const auto& neighbor_uuid : neighbors) {
                    auto neighbor_node = engine_->get_node(neighbor_uuid);
                    if (!neighbor_node) continue;
                    
                    bool all_pass = true;
                    for (const auto& f : filters_) {
                        if (f.alias == s.target_alias) {
                            if (!evaluate_filter(neighbor_node.get(), f)) {
                                all_pass = false;
                                break;
                            }
                        }
                    }
                    if (all_pass) {
                        Path new_path = path;
                        new_path.alias_to_uuid[s.target_alias] = neighbor_uuid; 
                        new_path.last_alias = s.target_alias;
                        next_paths.push_back(std::move(new_path));
                    }
                }
            }
        },
        [&](const InStep& s) {
            for (const auto &path : paths) {
                auto it = path.alias_to_uuid.find(path.last_alias);
                if (it == path.alias_to_uuid.end()) continue;

                auto node = engine_->get_node(it->second);
                auto neighbors = node->get_in_neighbors(s.label);

                for (const auto& neighbor_uuid : neighbors) {
                    auto neighbor_node = engine_->get_node(neighbor_uuid);
                    if (!neighbor_node) continue;

                    bool all_pass = true;
                    for (const auto& f : filters_) {
                        if (f.alias == s.target_alias) {
                            if (!evaluate_filter(neighbor_node.get(), f)) {
                                all_pass = false;
                                break;
                            }
                        }
                    }
                    if (all_pass) {
                        Path new_path = path;
                        new_path.alias_to_uuid[s.target_alias] = neighbor_uuid; 
                        new_path.last_alias = s.target_alias;
                        next_paths.push_back(std::move(new_path));
                    }
                }
            }
        }
    }, step);
    paths = std::move(next_paths);
  }

  // Output Materialization
  L3_LOG(LOG_DEBUG, "L3KVG: Query - Materializing %zu paths", paths.size());
  for (const auto &path : paths) {
    try {
        ResultRow row;
        // Pre-populate fields for all aliases in the path
        for (const auto& [alias, uuid] : path.alias_to_uuid) {
            auto node = engine_->get_node(uuid);
            if (!node) continue;
            row.nodes.push_back(node);
            
            // Find all projections for THIS alias and populate them
            for (size_t col_idx = 0; col_idx < projections_.size(); ++col_idx) {
                if (projections_[col_idx].alias == alias) {
                    std::string_view view = "";
                    try {
                        view = node->get_attribute_view(projections_[col_idx].property);
                    } catch (const std::exception& e) {
                        L3_LOG(LOG_ERROR, "L3KVG: Query - Exception in get_attribute_view for [%s.%s]: %s", 
                               alias.c_str(), projections_[col_idx].property.c_str(), e.what());
                    }

                    if (view.empty()) {
                        try {
                            std::string val = node->get_attribute<std::string>(projections_[col_idx].property);
                            if (!val.empty()) {
                                row.fallback_strings.push_back(std::move(val));
                                view = row.fallback_strings.back();
                            }
                        } catch (...) {}
                    }
                    row.fields[alias + "." + projections_[col_idx].property] = view;
                    row.fields[std::to_string(col_idx)] = view;
                }
            }
        }
        
        // Ensure every projection has at least an empty entry to avoid "Key not found"
        for (size_t col_idx = 0; col_idx < projections_.size(); ++col_idx) {
            if (row.fields.find(std::to_string(col_idx)) == row.fields.end()) {
                row.fields[std::to_string(col_idx)] = "";
            }
        }
        
        results.push_back(std::move(row));
    } catch (const std::exception& e) {
        L3_LOG(LOG_ERROR, "L3KVG: Query - Exception during path materialization: %s", e.what());
        // Re-throw to be caught by execute_query
        throw;
    }
  }

  return results;
}

} // namespace l3kvg
