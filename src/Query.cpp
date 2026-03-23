#include "L3KVG/Query.hpp"
#include "L3KVG/Node.hpp"

namespace l3kvg {

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

Query &Query::where_eq(std::string_view alias, std::string_view key,
                       std::string_view value) {
  filters_eq_.push_back(
      {std::string(alias), std::string(key), std::string(value)});
  return *this;
}

Query::OutEdgeBuilder Query::out(std::string_view edge_label,
                                 double min_weight) {
  return OutEdgeBuilder(*this, edge_label, min_weight);
}

Query &Query::OutEdgeBuilder::as(std::string_view dest_alias) {
  q_.traversals_.push_back({label_, weight_, std::string(dest_alias)});
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
  for (const auto &f : filters_eq_) {
    if (f.alias == initial_match_->alias && f.key == "id") {
      frontier.push_back(f.value);
      break;
    }
  }

  if (frontier.empty()) {
    // Fallback or full table scan not implemented yet for embedded engine.
    // Requires an explicit where_eq(..., "id", ...)
    return results;
  }

  // Process linear traversals
  for (const auto &step : traversals_) {
    std::vector<std::string> next_frontier;
    for (const auto &src_uuid : frontier) {
      auto node = engine_->get_node(src_uuid);
      auto neighbors = node->get_neighbors(step.label, step.min_weight);
      next_frontier.insert(next_frontier.end(), neighbors.begin(),
                           neighbors.end());
    }
    frontier = next_frontier;
  }

  // Output Materialization and Filtering
  for (const auto &uuid : frontier) {
    auto node = engine_->get_node(uuid);
    bool match = true;

    for (const auto &f : filters_eq_) {
      // Apply filtering on the final resolved alias
      if (!traversals_.empty() && f.alias == traversals_.back().target_alias) {
        auto attr = node->get_attribute<std::string>(f.key);
        if (attr != f.value) {
          match = false;
          break;
        }
      }
    }

    if (match) {
      ResultRow row;
      for (const auto &proj : projections_) {
        auto val = node->get_attribute<std::string>(proj.property);
        row.fields[proj.alias + "." + proj.property] = val;
      }
      results.push_back(row);
    }
  }

  return results;
}

} // namespace l3kvg
