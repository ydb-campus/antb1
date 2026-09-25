#include "antb1/exec/physical_planner.h"

#include <memory>
#include <optional>
#include <utility>
#include <variant>
#include <vector>

#include <arrow/api.h>

#include "antb1/exec/filter.h"
#include "antb1/exec/limit.h"
#include "antb1/exec/project.h"
#include "antb1/exec/row_count.h"
#include "antb1/exec/scalar_aggregate.h"
#include "antb1/exec/table_scan.h"
#include "antb1/plan/logical_plan.h"

namespace antb1::exec {
namespace {

using OperatorResult = arrow::Result<std::unique_ptr<Operator>>;

OperatorResult Build(const plan::LogicalNodePtr& node);

// One overload per logical node type: a node type without one fails to compile.
struct Builder {
  OperatorResult operator()(const plan::ScanNode& node) const {
    if (node.table == nullptr) {
      return arrow::Status::Invalid("scan without a table");
    }
    return std::make_unique<TableScanOperator>(node.table, node.fields);
  }
  OperatorResult operator()(const plan::FilterNode& node) const {
    ARROW_ASSIGN_OR_RAISE(auto input, Build(node.input));
    return std::make_unique<FilterOperator>(std::move(input), node.predicates);
  }
  OperatorResult operator()(const plan::ProjectNode& node) const {
    ARROW_ASSIGN_OR_RAISE(auto input, Build(node.input));
    std::vector<int> columns;
    columns.reserve(node.columns.size());
    for (const plan::BoundColumn& c : node.columns) {
      columns.push_back(c.index);
    }
    return std::make_unique<ProjectOperator>(std::move(input), std::move(columns));
  }
  OperatorResult operator()(const plan::AggregateNode& node) const {
    ARROW_ASSIGN_OR_RAISE(auto input, Build(node.input));
    return std::make_unique<ScalarAggregateOperator>(std::move(input), node.aggregates);
  }
  OperatorResult operator()(const plan::LimitNode& node) const {
    ARROW_ASSIGN_OR_RAISE(auto input, Build(node.input));
    return std::make_unique<LimitOperator>(std::move(input), node.limit);
  }
  OperatorResult operator()(const plan::RowCountNode& node) const {
    const auto rows = node.table == nullptr ? std::nullopt : node.table->exact_row_count();
    if (!rows) {
      return arrow::Status::Invalid("RowCount over a table without an exact row count");
    }
    return std::make_unique<RowCountOperator>("count_star()", *rows);
  }
};

OperatorResult Build(const plan::LogicalNodePtr& node) {
  if (node == nullptr) {
    return arrow::Status::Invalid("logical plan node without its input");
  }
  return std::visit(Builder{}, *node);
}

}  // namespace

arrow::Result<std::unique_ptr<Operator>> BuildPhysicalPlan(const plan::LogicalPlan& plan) {
  if (!plan.root || plan.output.empty()) {
    return arrow::Status::Invalid("empty logical plan");
  }
  ARROW_ASSIGN_OR_RAISE(auto root, Build(plan.root));
  if (std::cmp_not_equal(root->output_schema()->num_fields(), plan.output.size())) {
    return arrow::Status::Invalid("the physical plan has ", root->output_schema()->num_fields(),
                                  " columns, the logical plan ", plan.output.size());
  }
  return root;
}

}  // namespace antb1::exec
