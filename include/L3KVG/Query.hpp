#pragma once

#include "L3KVG/Engine.hpp"
#include "L3KVG/QueryResult.hpp"
#include "buffer.hpp"
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
  Query &match_id(uint64_t id, std::string_view alias = "a");
  Query &match_id(std::string_view uuid, std::string_view alias = "a");

  // Filters
  enum class Op { Eq, Ne, Gt, Ge, Lt, Le, Like, NotLike };
  enum class LogicalOp { And, Or };

  struct Filter {
    std::string alias;
    std::string key;
    Op op = Op::Eq;
    std::string value;
    uint32_t key_hash = 0;
    bool numeric_ready = false;
    bool has_i64 = false;
    bool has_f64 = false;
    int64_t i64_val = 0;
    double f64_val = 0.0;
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

  Query &from(std::string_view alias) { current_source_alias_ = alias; return *this; }

  // Projection
  enum class AggOp { None, Count, Sum, Avg, Min, Max };
  Query &return_(std::string_view alias, std::string_view property, AggOp agg = AggOp::None, bool distinct = false);

  // Sorting & Pagination
  Query &order_by(std::string_view alias, std::string_view property, bool ascending = true);
  Query &limit(size_t limit);
  Query &offset(size_t offset);

  // Grouping
  Query &group_by(std::string_view alias, std::string_view property, std::string_view func_name = "", const std::vector<std::string>& func_args = {});
  Query &distinct(bool enable = true);

  // Execution
  std::vector<ResultRow> execute();
  lite3cpp::Buffer execute_to_flat_buffer();
  lite3cpp::Buffer execute_to_buffer();

  // Federation & Server Extensions
  static lite3cpp::Buffer serialize_results(const std::vector<ResultRow>& rows);
  static lite3cpp::Buffer serialize_flat_results(const std::vector<ResultRow>& rows);
  Query &resume(const std::vector<uint64_t>& starting_nodes, const lite3cpp::Buffer& query_buf);
  Query &resume(const std::vector<uint64_t>& starting_nodes, std::string_view query_payload);

  Query &set_principal_id(uint32_t uid) { principal_id_ = uid; return *this; }
  Query &set_start_index_prefix(std::string_view prefix) { start_index_prefix_.assign(prefix.data(), prefix.size()); return *this; }
  const std::string& get_start_index_prefix() const noexcept { return start_index_prefix_; }
  Query &set_flat_rows(bool enable = true) noexcept { flat_rows_ = enable; return *this; }
  bool is_flat_rows() const noexcept { return flat_rows_; }
  Query &set_populate_fields(bool enable) noexcept { populate_fields_ = enable; return *this; }
  bool get_populate_fields() const noexcept { return populate_fields_; }

  bool is_federated_branch_ = false;

private:
  Engine *engine_;
  uint32_t principal_id_ = INTERNAL_UID;
  std::vector<uint64_t> starting_nodes_;
  std::string start_index_prefix_;
  std::string root_alias_ = "__root";
  bool flat_rows_ = false;
  bool populate_fields_ = true;

  // AST State
  struct MatchStep {
    std::string alias;
  };
  struct OutStep {
    std::string label;
    double min_weight;
    std::string target_alias;
    std::string source_alias; // If empty, use last_alias
  };
  struct InStep {
    std::string label;
    std::string target_alias;
    std::string source_alias; // If empty, use last_alias
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
    bool distinct = false;
  };
  struct SortStep {
    std::string alias;
    std::string property;
    bool ascending = true;
  };
  struct GroupStep {
    std::string alias;
    std::string property;
    std::string func_name;
    std::vector<std::string> func_args;
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
  std::string current_source_alias_;

  static std::string eval_group_key_value(const GroupStep &g, std::string val);
  static std::string compute_numeric_agg(const ReturnStep &p,
                                         const std::vector<ResultRow> &part,
                                         const std::string &full_name,
                                         const std::string &idx_key);
  std::vector<ResultRow> apply_group_by_and_aggregations(
      std::vector<ResultRow> results,
      const std::vector<std::string> &invariant_proj_names,
      const std::vector<std::string> &idx_names,
      bool has_agg,
      bool should_populate_fields) const;
  void apply_distinct_sort_limit(
      std::vector<ResultRow> &results,
      const std::vector<std::string> &idx_names,
      const std::vector<std::string> &sort_field_keys) const;
};

} // namespace l3kvg
