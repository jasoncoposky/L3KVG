#pragma once

#include "L3KVG/Engine.hpp"
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>


namespace l3kvg {

class Engine;

class Query {
public:
  explicit Query(Engine *engine);

  // Initial node selection
  Query &match(std::string_view node_alias);

  // Filters
  Query &where_has(std::string_view alias, std::string_view key,
                   std::string_view value_type);
  Query &where_eq(std::string_view alias, std::string_view key,
                  std::string_view value);

  // Traversal Builder
  class OutEdgeBuilder {
    Query &q_;
    std::string label_;
    double weight_;

  public:
    OutEdgeBuilder(Query &q, std::string_view label, double min_weight)
        : q_(q), label_(label), weight_(min_weight) {}
    Query &as(std::string_view dest_alias);
  };

  OutEdgeBuilder out(std::string_view edge_label, double min_weight = 0.0);

  // Projection
  Query &return_(std::string_view alias, std::string_view property);

  // Execution
  struct ResultRow {
    std::unordered_map<std::string, std::string> fields;
  };
  std::vector<ResultRow> execute();

private:
  Engine *engine_;

  // AST State
  struct MatchStep {
    std::string alias;
  };
  struct OutStep {
    std::string label;
    double min_weight;
    std::string target_alias;
  };
  struct FilterEq {
    std::string alias;
    std::string key;
    std::string value;
  };
  struct FilterHas {
    std::string alias;
    std::string key;
    std::string type;
  };
  struct ReturnStep {
    std::string alias;
    std::string property;
  };

  std::optional<MatchStep> initial_match_;
  std::vector<OutStep> traversals_;
  std::vector<FilterEq> filters_eq_;
  std::vector<FilterHas> filters_has_;
  std::vector<ReturnStep> projections_;
};

} // namespace l3kvg
