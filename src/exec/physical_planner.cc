#include "antb1/exec/physical_planner.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <utility>
#include <variant>
#include <vector>

#include <arrow/api.h>

#include "antb1/exec/compute.h"
#include "antb1/exec/filter.h"
#include "antb1/exec/group_aggregate.h"
#include "antb1/exec/limit.h"
#include "antb1/exec/project.h"
#include "antb1/exec/row_count.h"
#include "antb1/exec/scalar_aggregate.h"
#include "antb1/exec/sort.h"
#include "antb1/exec/table_scan.h"
#include "antb1/plan/logical_plan.h"

#include "part_operators.h"
#include "part_pruning.h"

namespace antb1::exec {
namespace {

using OperatorResult = arrow::Result<std::unique_ptr<Operator>>;

// Builds a node's operators. Without a part, a part pipeline (PipelineScan) becomes the part
// operators over it; with one, the node is inside the pipeline of that part.
OperatorResult Build(const plan::LogicalNodePtr& node, std::optional<int64_t> part = std::nullopt);

// The scan at the bottom of a part pipeline, a chain of streaming nodes over a scan; nullptr if
// `node` is not the top of one.
const plan::ScanNode* PipelineScan(const plan::LogicalNodePtr& node) {
  const plan::LogicalNode* n = node.get();
  while (n != nullptr) {
    if (const auto* scan = std::get_if<plan::ScanNode>(n)) {
      return scan->table == nullptr ? nullptr : scan;
    }
    if (const auto* filter = std::get_if<plan::FilterNode>(n)) {
      n = filter->input.get();
    } else if (const auto* compute = std::get_if<plan::ComputeNode>(n)) {
      n = compute->input.get();
    } else if (const auto* project = std::get_if<plan::ProjectNode>(n)) {
      n = project->input.get();
    } else {
      return nullptr;
    }
  }
  return nullptr;
}

// The predicates of the Filters directly on the scan of a part pipeline (below any Compute or
// Project): their columns are the scan's output columns.
std::vector<plan::Predicate> FiltersOnScan(const plan::LogicalNodePtr& node) {
  std::vector<const plan::LogicalNode*> chain;  // top to scan
  for (const plan::LogicalNode* n = node.get(); n != nullptr;) {
    chain.push_back(n);
    if (const auto* filter = std::get_if<plan::FilterNode>(n)) {
      n = filter->input.get();
    } else if (const auto* compute = std::get_if<plan::ComputeNode>(n)) {
      n = compute->input.get();
    } else if (const auto* project = std::get_if<plan::ProjectNode>(n)) {
      n = project->input.get();
    } else {
      break;
    }
  }
  std::vector<plan::Predicate> predicates;
  for (std::size_t i = chain.size(); i-- > 1;) {  // from just above the scan up
    const auto* filter = std::get_if<plan::FilterNode>(chain[i - 1]);
    if (filter == nullptr) {
      break;
    }
    predicates.insert(predicates.end(), filter->predicates.begin(), filter->predicates.end());
  }
  return predicates;
}

// The pipelines of the parts a part pipeline reads: every part of the scan's table but those its
// filters rule out by their statistics (part_pruning.h), in part order, numbered 0 .. count - 1.
struct Parts {
  PartPipeline pipeline;
  int64_t count = 0;
};

Parts PartsOf(const plan::LogicalNodePtr& node, const plan::ScanNode& scan) {
  auto kept = std::make_shared<const std::vector<int64_t>>(
      KeptParts(*scan.table, scan.fields, FiltersOnScan(node)));
  const auto count = static_cast<int64_t>(kept->size());
  return Parts{.pipeline =
                   [node, kept](int64_t i) {
                     // Past the kept parts only for the schema sample, which is never opened.
                     const int64_t part =
                         std::cmp_less(i, kept->size()) ? (*kept)[static_cast<std::size_t>(i)] : i;
                     return Build(node, part);
                   },
               .count = count};
}

// A part pipeline's batches in part order, at most row_cap selected rows per part.
OperatorResult BuildPartUnion(const plan::LogicalNodePtr& node, const plan::ScanNode& scan,
                              std::optional<int64_t> row_cap) {
  Parts parts = PartsOf(node, scan);
  ARROW_ASSIGN_OR_RAISE(auto sample, parts.pipeline(0));  // for the output schema; never opened
  return std::make_unique<PartUnionOperator>(std::move(parts.pipeline), parts.count,
                                             sample->output_schema(), row_cap);
}

// The column every call counts distinctly, when every call is COUNT(DISTINCT) of the same column.
// A global aggregation of only such calls is planned as a GROUP BY of that column, which merges in
// parallel (partitioned); a grouped one keeps its COUNT(DISTINCT) states: grouping by the keys and
// the column would leave the outer grouping serial over up to every row.
std::optional<plan::BoundColumn> OnlyDistinctColumn(const std::vector<plan::AggregateCall>& calls) {
  if (calls.empty() || !calls.front().arg.has_value()) {
    return std::nullopt;
  }
  const plan::BoundColumn& first = *calls.front().arg;
  for (const plan::AggregateCall& call : calls) {
    if (call.kind != plan::AggKind::kCountDistinct || !call.arg.has_value() ||
        call.arg->index != first.index) {
      return std::nullopt;
    }
  }
  return first;
}

// The calls counting the distinct column `x`, over a GROUP BY by x: COUNT(key0), which skips the
// NULL group as COUNT(DISTINCT) skips NULL.
std::vector<plan::AggregateCall> CountsOfKey(const std::vector<plan::AggregateCall>& calls,
                                             const plan::BoundColumn& x) {
  std::vector<plan::AggregateCall> counts = calls;
  for (plan::AggregateCall& call : counts) {
    call.kind = plan::AggKind::kCount;
    call.arg = plan::BoundColumn{.index = 0, .name = x.name, .type = x.type};
  }
  return counts;
}

// One overload per logical node type: a node type without one fails to compile.
struct Builder {
  std::optional<int64_t> part;  // inside the pipeline of this part

  OperatorResult operator()(const plan::ScanNode& node) const {
    if (node.table == nullptr) {
      return arrow::Status::Invalid("scan without a table");
    }
    return std::make_unique<TableScanOperator>(node.table, node.fields, part);
  }
  OperatorResult operator()(const plan::FilterNode& node) const {
    ARROW_ASSIGN_OR_RAISE(auto input, Build(node.input, part));
    return std::make_unique<FilterOperator>(std::move(input), node.predicates);
  }
  OperatorResult operator()(const plan::ComputeNode& node) const {
    ARROW_ASSIGN_OR_RAISE(auto input, Build(node.input, part));
    return std::make_unique<ComputeOperator>(std::move(input), node.exprs);
  }
  OperatorResult operator()(const plan::ProjectNode& node) const {
    ARROW_ASSIGN_OR_RAISE(auto input, Build(node.input, part));
    std::vector<int> columns;
    std::vector<std::shared_ptr<arrow::Scalar>> constants;
    columns.reserve(node.columns.size());
    for (std::size_t i = 0; i < node.columns.size(); ++i) {
      std::shared_ptr<arrow::Scalar> constant;
      if (!node.constants.empty()) {
        if (const std::optional<plan::Constant>& value = node.constants[i]; value.has_value()) {
          ARROW_ASSIGN_OR_RAISE(constant, plan::ToArrowScalar(*value));
        }
      }
      columns.push_back(node.columns[i].index);
      constants.push_back(std::move(constant));
    }
    return std::make_unique<ProjectOperator>(std::move(input), std::move(columns),
                                             std::move(constants));
  }
  OperatorResult operator()(const plan::AggregateNode& node) const {
    if (const auto x = OnlyDistinctColumn(node.aggregates);
        x.has_value() && PipelineScan(node.input) != nullptr) {
      // COUNT(DISTINCT x) only: the rows grouped by x in parallel (partitioned GROUP BY), then
      // COUNT(x) over the groups. The same DOUBLE normalization groups x as COUNT(DISTINCT) does.
      const auto groups = std::make_shared<const plan::LogicalNode>(
          plan::GroupAggregateNode{.input = node.input, .keys = {*x}, .aggregates = {}});
      return (*this)(plan::AggregateNode{
          .input = groups, .aggregates = CountsOfKey(node.aggregates, *x), .span = node.span});
    }
    if (const plan::ScanNode* scan = PipelineScan(node.input)) {
      // Aggregated per part, the parts' states merged in part order.
      Parts parts = PartsOf(node.input, *scan);
      ARROW_ASSIGN_OR_RAISE(auto sample, parts.pipeline(0));
      return std::make_unique<PartAggregateOperator>(std::move(parts.pipeline), parts.count,
                                                     sample->output_schema()->num_fields(),
                                                     node.aggregates);
    }
    ARROW_ASSIGN_OR_RAISE(auto input, Build(node.input));
    return std::make_unique<ScalarAggregateOperator>(std::move(input), node.aggregates);
  }
  OperatorResult operator()(const plan::GroupAggregateNode& node) const {
    if (const plan::ScanNode* scan = PipelineScan(node.input)) {
      // Grouped per part, the parts' groups merged in part order.
      Parts parts = PartsOf(node.input, *scan);
      ARROW_ASSIGN_OR_RAISE(auto sample, parts.pipeline(0));
      const int width = sample->output_schema()->num_fields();
      // The output schema, as the serial operator names it.
      const GroupAggregateOperator serial(std::move(sample), node.keys, node.aggregates);
      return std::make_unique<PartGroupAggregateOperator>(std::move(parts.pipeline), parts.count,
                                                          width, node.keys, node.aggregates,
                                                          serial.output_schema());
    }
    ARROW_ASSIGN_OR_RAISE(auto input, Build(node.input));
    return std::make_unique<GroupAggregateOperator>(std::move(input), node.keys, node.aggregates);
  }
  OperatorResult operator()(const plan::SortNode& node) const {
    ARROW_ASSIGN_OR_RAISE(auto input, Build(node.input));
    return std::make_unique<SortOperator>(std::move(input), node.keys);
  }
  OperatorResult operator()(const plan::LimitNode& node) const {
    // Limit(Sort) with a limit is a top-N: it keeps only limit + offset rows while it reads.
    const auto* sort = std::get_if<plan::SortNode>(node.input.get());
    if (sort != nullptr && node.limit.has_value() && *node.limit > 0) {
      if (const plan::ScanNode* scan = PipelineScan(sort->input)) {
        // Every part keeps its own first rows, merged in part order.
        Parts parts = PartsOf(sort->input, *scan);
        ARROW_ASSIGN_OR_RAISE(auto sample, parts.pipeline(0));
        return std::make_unique<PartTopNOperator>(std::move(parts.pipeline), parts.count,
                                                  sample->output_schema(), sort->keys, *node.limit,
                                                  node.offset);
      }
      ARROW_ASSIGN_OR_RAISE(auto input, Build(sort->input));
      return std::make_unique<SortOperator>(std::move(input), sort->keys, node.limit, node.offset);
    }
    std::unique_ptr<Operator> input;
    if (const plan::ScanNode* scan = PipelineScan(node.input)) {
      // No part needs more than limit + offset rows.
      std::optional<int64_t> cap;
      if (node.limit.has_value()) {
        int64_t rows = 0;
        cap = __builtin_add_overflow(*node.limit, node.offset, &rows)
                  ? std::nullopt
                  : std::optional<int64_t>(rows);
      }
      ARROW_ASSIGN_OR_RAISE(input, BuildPartUnion(node.input, *scan, cap));
    } else {
      ARROW_ASSIGN_OR_RAISE(input, Build(node.input));
    }
    return std::make_unique<LimitOperator>(std::move(input), node.limit, node.offset);
  }
  OperatorResult operator()(const plan::RowCountNode& node) const {
    const auto rows = node.table == nullptr ? std::nullopt : node.table->exact_row_count();
    if (!rows) {
      return arrow::Status::Invalid("RowCount over a table without an exact row count");
    }
    return std::make_unique<RowCountOperator>("count_star()", *rows);
  }
};

OperatorResult Build(const plan::LogicalNodePtr& node, std::optional<int64_t> part) {
  if (node == nullptr) {
    return arrow::Status::Invalid("logical plan node without its input");
  }
  if (!part.has_value()) {
    if (const plan::ScanNode* scan = PipelineScan(node)) {
      return BuildPartUnion(node, *scan, std::nullopt);
    }
  }
  return std::visit(Builder{.part = part}, *node);
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
