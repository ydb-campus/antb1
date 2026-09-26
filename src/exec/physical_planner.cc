#include "antb1/exec/physical_planner.h"

#include <memory>
#include <string>
#include <string_view>
#include <variant>

#include <arrow/api.h>

#include "antb1/exec/row_count.h"
#include "antb1/plan/logical_plan.h"
#include "antb1/plan/sql_status.h"

namespace antb1::exec {
namespace {

using OperatorResult = arrow::Result<std::unique_ptr<Operator>>;

// The executor answers only COUNT(*) without WHERE for now (from metadata); every node that needs
// table data is valid SQL the engine does not run yet: Unsupported (exit code 4), at the node's
// span.
OperatorResult NotExecutedYet(std::string_view what, SourceSpan span) {
  return plan::UnsupportedError(
      std::string(what) +
          " cannot be executed yet: only SELECT COUNT(*) FROM <table> (without WHERE) is answered",
      span);
}

// One overload per logical node type: a node type without one fails to compile.
struct Builder {
  const plan::LogicalPlan& plan;

  OperatorResult operator()(const plan::ScanNode& node) const {
    return NotExecutedYet("a table scan", node.span);
  }
  OperatorResult operator()(const plan::FilterNode& node) const {
    return NotExecutedYet("WHERE", node.span);
  }
  OperatorResult operator()(const plan::ProjectNode& node) const {
    return NotExecutedYet("selecting columns", node.span);
  }
  OperatorResult operator()(const plan::AggregateNode& node) const {
    return NotExecutedYet("an aggregate over table data", node.span);
  }
  OperatorResult operator()(const plan::LimitNode& node) const {
    return NotExecutedYet("LIMIT", node.span);
  }
  OperatorResult operator()(const plan::RowCountNode& node) const {
    const auto rows = node.table->exact_row_count();
    if (!rows) {
      return arrow::Status::Invalid("RowCount over a table without an exact row count");
    }
    return std::make_unique<RowCountOperator>(plan.output.front().name, *rows);
  }
};

}  // namespace

arrow::Result<std::unique_ptr<Operator>> BuildPhysicalPlan(const plan::LogicalPlan& plan) {
  if (!plan.root || plan.output.empty()) {
    return arrow::Status::Invalid("empty logical plan");
  }
  return std::visit(Builder{.plan = plan}, *plan.root);
}

}  // namespace antb1::exec
