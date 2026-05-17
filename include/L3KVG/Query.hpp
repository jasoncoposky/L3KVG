#pragma once

#include "L3KVG/Engine.hpp"
#include "L3KVG/QueryResult.hpp"
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
  using ResultRow = l3kvg::ResultRow;
  explicit Query(Engine *engine);

  // Initial node selection
  Query &match(std::string_view node_alias);

  // Filters
  enum class Op { Eq, Ne, Gt, Ge, Lt, Le, Like };
  enum class LogicalOp { And, Or };

  struct Filter {
    std::string alias;
    std::string key;
    Op op;
    std::string value;
  };

  struct FilterGroup;

  struct FilterNode {
    std::variant<Filter, std::shared_ptr<FilterGroup>> node;
    LogicalOp prepended_op = LogicalOp::And;
  };

  struct FilterGroup {
    std::vector<FilterNode> nodes;

    FilterGroup& where(std::string_view alias, std::string_view key, Op op, std::string_view value);
    FilterGroup& or_where(std::string_view alias, std::string_view key, Op op, std::string_view value);
    FilterGroup& where_group(std::function<void(FilterGroup&)> cb);
    FilterGroup& or_where_group(std::function<void(FilterGroup&)> cb);
  };

  Query &where(std::string_view alias, std::string_view key, Op op, std::string_view value);
  Query &or_where(std::string_view alias, std::string_view key, Op op, std::string_view value);
  Query &where_group(std::function<void(FilterGroup&)> cb);
  Query &or_where_group(std::function<void(FilterGroup&)> cb);

  FilterGroup& get_root_filters() { return root_filters_; }

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
  enum class AggOp { None, Count, Sum, Avg, Min, Max };
  Query &return_(std::string_view alias, std::string_view property, AggOp agg = AggOp::None);

  // Sorting & Pagination
  Query &order_by(std::string_view alias, std::string_view property, bool ascending = true);
  Query &limit(size_t limit);
  Query &offset(size_t offset);

  // Grouping
  Query &group_by(std::string_view alias, std::string_view property);
  Query &distinct(bool enable = true);

  // Execution
  std::vector<ResultRow> execute();

  // Federation Support
  Query &resume(const std::vector<uint64_t>& starting_nodes, std::string_view query_json);

private:
  Engine *engine_;
  std::vector<uint64_t> starting_nodes_;
  std::string root_alias_ = "__root";

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
    AggOp agg = AggOp::None;
  };
  struct SortStep {
    std::string alias;
    std::string property;
    bool ascending = true;
  };
  struct GroupStep {
    std::string alias;
    std::string property;
  };

  std::optional<MatchStep> initial_match_;
  std::vector<Step> steps_;
  FilterGroup root_filters_;
  std::vector<FilterHas> filters_has_;
  std::vector<ReturnStep> projections_;
  std::vector<SortStep> sorts_;
  std::vector<GroupStep> groups_;
  bool distinct_ = false;
  std::optional<size_t> limit_;
  std::optional<size_t> offset_;

  static std::string serialize_steps(const std::vector<Step>& steps);
};

} // namespace l3kvg
