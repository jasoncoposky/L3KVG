#pragma once

#include "L3KVG/Engine.hpp"
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>
#include <variant>


namespace l3kvg {

class Engine;

class Query {
public:
  explicit Query(Engine *engine);

  // Initial node selection
  Query &match(std::string_view node_alias);

  // Filters
  enum class Op { Eq, Ne, Gt, Ge, Lt, Le, Like };

  struct Filter {
    std::string alias;
    std::string key;
    Op op;
    std::string value;
  };

  Query &where(std::string_view alias, std::string_view key, Op op, std::string_view value);

  Query &where_has(std::string_view alias, std::string_view key,
                   std::string_view value_type);

  // Legacy helper
  Query &where_eq(std::string_view alias, std::string_view key,
                  std::string_view value) {
      return where(alias, key, Op::Eq, value);
  }

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

  class InEdgeBuilder {
    Query &q_;
    std::string label_;

  public:
    InEdgeBuilder(Query &q, std::string_view label)
        : q_(q), label_(label) {}
    Query &as(std::string_view dest_alias);
  };

  OutEdgeBuilder out(std::string_view edge_label, double min_weight = 0.0);
  InEdgeBuilder in(std::string_view edge_label);

  // Projection
  Query &return_(std::string_view alias, std::string_view property);

  // Execution
  struct ResultRow {
    std::unordered_map<std::string, std::string_view> fields;
    std::vector<std::shared_ptr<Node>> nodes; // Keep memory alive for views
    std::vector<std::string> fallback_strings; // Keep memory alive for fallbacks
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
  struct InStep {
    std::string label;
    std::string target_alias;
  };
  using Step = std::variant<OutStep, InStep>;

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
  std::vector<Step> steps_;
  std::vector<Filter> filters_;
  std::vector<FilterHas> filters_has_;
  std::vector<ReturnStep> projections_;
};

} // namespace l3kvg
