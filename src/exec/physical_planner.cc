#include "antb1/exec/physical_planner.h"

#include <algorithm>
#include <cstdint>
#include <format>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <arrow/api.h>

#include "antb1/exec/compute.h"
#include "antb1/exec/filter.h"
#include "antb1/exec/group_aggregate.h"
#include "antb1/exec/join_table.h"
#include "antb1/exec/limit.h"
#include "antb1/exec/profile.h"
#include "antb1/exec/project.h"
#include "antb1/exec/row_count.h"
#include "antb1/exec/scalar_aggregate.h"
#include "antb1/exec/scan_filter.h"
#include "antb1/exec/sort.h"
#include "antb1/exec/table_scan.h"
#include "antb1/plan/explain.h"
#include "antb1/plan/logical_plan.h"
#include "antb1/plan/sql_status.h"
#include "antb1/plan/types.h"

#include "hash_join.h"
#include "parallel_compute.h"
#include "part_operators.h"
#include "part_pruning.h"
#include "profiled_operator.h"

namespace antb1::exec {
namespace {

using OperatorResult = arrow::Result<std::unique_ptr<Operator>>;

// The inner joins whose probes run in one part pipeline, and their builds, outermost join first:
// the order in which the operator that runs the pipeline prepares them (PrepareBuilds). Made once
// per pipeline, before the factory of its parts, so every part's probe reads the same build and no
// part number of the pipeline ever reaches a build input.
struct PipelineBuilds {
  std::vector<const plan::JoinNode*> joins;
  std::vector<std::shared_ptr<JoinBuild>> builds;  // of each join

  // The build of `join`; nullptr if it is none of the pipeline's joins.
  [[nodiscard]] std::shared_ptr<JoinBuild> Find(const plan::JoinNode* join) const {
    for (std::size_t i = 0; i < joins.size(); ++i) {
      if (joins[i] == join) {
        return builds[i];
      }
    }
    return nullptr;
  }
};

// Builds a node's operators. Without a part, a part pipeline (PipelineScan) becomes the part
// operators over it; with one, the node is inside the pipeline of that part, whose joins' builds
// are `builds`. With a profile node (`slot`), the operator is profiled into it (profile.h) and its
// inputs into its children. With `top_n` (a top-N right above `node`), a partitioned grouped
// aggregation keeps only each partition's first rows in its order (PartitionTopN).
OperatorResult Build(const plan::LogicalNodePtr& node, std::optional<int64_t> part = std::nullopt,
                     ProfileNode* slot = nullptr, const LateScan* late = nullptr,
                     const PartitionTopN* top_n = nullptr, const PipelineBuilds* builds = nullptr);
// The operator profiled into `slot` (with the node's EXPLAIN line when it has no detail yet); the
// operator itself without a slot.
OperatorResult Profiled(OperatorResult op, const plan::LogicalNodePtr& node, ProfileNode* slot);

// The node below `node` that a part pipeline goes on through: the input of a Filter, a Compute or
// a Project, and an inner join's probe input (the input it does not build on); nullptr for any
// other node. PipelineScan, FiltersOnScan and PipelineBuildsOf all walk a pipeline with it, so they
// never disagree on its nodes.
const plan::LogicalNode* PipelineInput(const plan::LogicalNode& node) {
  if (const auto* filter = std::get_if<plan::FilterNode>(&node)) {
    return filter->input.get();
  }
  if (const auto* compute = std::get_if<plan::ComputeNode>(&node)) {
    return compute->input.get();
  }
  if (const auto* project = std::get_if<plan::ProjectNode>(&node)) {
    return project->input.get();
  }
  if (const auto* join = std::get_if<plan::JoinNode>(&node);
      join != nullptr && join->kind == plan::JoinKind::kInner) {
    return (join->build == plan::BuildSide::kLeft ? join->right : join->left).get();
  }
  return nullptr;
}

// The scan at the bottom of a part pipeline, a chain of streaming nodes over a scan (Filter,
// Compute, Project and inner joins' probes, PipelineInput); nullptr if `node` is not the top of
// one. Any other node ends the chain: a join of another kind, an aggregation, a sort, a limit.
const plan::ScanNode* PipelineScan(const plan::LogicalNodePtr& node) {
  for (const plan::LogicalNode* n = node.get(); n != nullptr; n = PipelineInput(*n)) {
    if (const auto* scan = std::get_if<plan::ScanNode>(n)) {
      return scan->table == nullptr ? nullptr : scan;
    }
  }
  return nullptr;
}

// The predicates of the Filters directly on the scan of the part pipeline whose top is `node`
// (below any other node of it: a Compute, a Project, a join): their columns are the scan's output
// columns. A Filter above a join reads the join's columns, so it is never one of them.
std::vector<plan::Predicate> FiltersOnScan(const plan::LogicalNodePtr& node) {
  std::vector<const plan::LogicalNode*> chain;  // top to scan
  for (const plan::LogicalNode* n = node.get(); n != nullptr; n = PipelineInput(*n)) {
    chain.push_back(n);
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

// An inner join as a hash join runs it: its probe and build inputs, and their keys (a key's left
// column is on the left input, its right column on the right one).
struct JoinShape {
  plan::LogicalNodePtr probe;
  plan::LogicalNodePtr build;
  std::vector<int> probe_keys;                // columns of the probe input's output
  std::vector<plan::BoundColumn> build_keys;  // columns of the build input's output
};

// Invalid, never unsupported, for what a correct plan never holds: a missing input, no key, a key
// whose two columns differ in type (the binder casts them to one type), a DOUBLE or BOOLEAN key
// (E1's table takes neither: such an equality is a residual), a missing residual. Checked before
// any profile line names the join by its EXPLAIN text, which reads every residual.
arrow::Result<JoinShape> ShapeOf(const plan::JoinNode& join) {
  if (join.left == nullptr || join.right == nullptr) {
    return arrow::Status::Invalid("a join without its inputs");
  }
  if (join.keys.empty()) {
    return arrow::Status::Invalid("a hash join without keys");
  }
  if (std::ranges::any_of(join.residual,
                          [](const plan::ExprPtr& residual) { return residual == nullptr; })) {
    return arrow::Status::Invalid("a join residual that is missing");
  }
  const bool build_left = join.build == plan::BuildSide::kLeft;
  JoinShape shape{.probe = build_left ? join.right : join.left,
                  .build = build_left ? join.left : join.right,
                  .probe_keys = {},
                  .build_keys = {}};
  for (const plan::JoinKey& key : join.keys) {
    if (key.left.type != key.right.type) {
      return arrow::Status::Invalid("a join key of two types: ", plan::ToString(key.left.type),
                                    " and ", plan::ToString(key.right.type));
    }
    if (key.left.type == plan::LogicalType::kDouble ||
        key.left.type == plan::LogicalType::kBoolean) {
      return arrow::Status::Invalid("a hash join key of type ", plan::ToString(key.left.type));
    }
    shape.probe_keys.push_back((build_left ? key.right : key.left).index);
    shape.build_keys.push_back(build_left ? key.left : key.right);
  }
  return shape;
}

// The pipelines of the parts a part pipeline reads: every part of the scan's table but those its
// filters rule out by their statistics (part_pruning.h), in part order, numbered 0 .. count - 1.
struct Parts {
  PartPipeline pipeline;
  int64_t count = 0;
  std::shared_ptr<const std::vector<int64_t>> kept;  // the table part of each part number
  // The first parts that hold kTwoLevelSampleRows rows by the table's part_rows (all of them if
  // they hold fewer): chosen from metadata, so never by the number of threads.
  int64_t sample = 0;
  // The builds of the inner joins the pipeline probes; nullptr when it probes none.
  std::shared_ptr<const PipelineBuilds> builds;
};

arrow::Result<Parts> PartsOf(const plan::LogicalNodePtr& node, const plan::ScanNode& scan,
                             ProfileNode* consumer = nullptr,
                             const std::shared_ptr<const LateScan>& late = nullptr);

// The build of an inner join of shape `shape`, profiled into `slot` (nullptr: not profiled). A
// build input that is a part pipeline is built from its parts, with the builds of its own joins,
// which the build prepares first; any other input's operator is drained.
arrow::Result<std::shared_ptr<JoinBuild>> MakeJoinBuild(const plan::JoinNode& join,
                                                        const JoinShape& shape, ProfileNode* slot) {
  if (slot != nullptr) {
    slot->set_name("HashBuild");
    slot->set_detail(plan::ExplainNode(plan::LogicalNode(join)));
  }
  if (const plan::ScanNode* scan = PipelineScan(shape.build)) {
    ARROW_ASSIGN_OR_RAISE(Parts parts, PartsOf(shape.build, *scan, slot));
    ARROW_ASSIGN_OR_RAISE(auto sample, parts.pipeline(0));  // for the schema; never opened
    ARROW_ASSIGN_OR_RAISE(auto spec,
                          JoinBuildSpec::Make(sample->output_schema(), shape.build_keys));
    return std::make_shared<JoinBuild>(
        std::move(spec), std::move(parts.pipeline), parts.count,
        parts.builds == nullptr ? std::vector<std::shared_ptr<JoinBuild>>{} : parts.builds->builds,
        slot);
  }
  ARROW_ASSIGN_OR_RAISE(
      auto input, Build(shape.build, std::nullopt, slot == nullptr ? nullptr : slot->Child(0)));
  ARROW_ASSIGN_OR_RAISE(auto spec, JoinBuildSpec::Make(input->output_schema(), shape.build_keys));
  return std::make_shared<JoinBuild>(std::move(spec), std::move(input), slot);
}

// The builds of the inner joins in the part pipeline whose top is `top`, outermost first; nullptr
// when it has none. With the profile node of the operator that runs the pipeline (`consumer`),
// the k-th build is profiled into its child 1 + k (child 0 is the pipeline's).
arrow::Result<std::shared_ptr<const PipelineBuilds>> PipelineBuildsOf(
    const plan::LogicalNodePtr& top, ProfileNode* consumer) {
  auto builds = std::make_shared<PipelineBuilds>();
  for (const plan::LogicalNode* n = top.get(); n != nullptr; n = PipelineInput(*n)) {
    const auto* join = std::get_if<plan::JoinNode>(n);
    if (join == nullptr) {
      continue;
    }
    ARROW_ASSIGN_OR_RAISE(const JoinShape shape, ShapeOf(*join));
    ARROW_ASSIGN_OR_RAISE(
        auto build,
        MakeJoinBuild(*join, shape,
                      consumer == nullptr ? nullptr : consumer->Child(1 + builds->builds.size())));
    builds->joins.push_back(join);
    builds->builds.push_back(std::move(build));
  }
  if (builds->builds.empty()) {
    return nullptr;
  }
  return builds;
}

// With a profile node of the operator that consumes the parts (`consumer`), the pipeline is
// profiled into its first child, once per part, the builds of its joins into the next ones, and
// the parts read and skipped are counted. With `late` (the late columns of the pipeline's scan and
// its row-id column), every part's scan is narrow (LateScan), numbered by the part's number.
arrow::Result<Parts> PartsOf(const plan::LogicalNodePtr& node, const plan::ScanNode& scan,
                             ProfileNode* consumer, const std::shared_ptr<const LateScan>& late) {
  auto kept = std::make_shared<const std::vector<int64_t>>(
      KeptParts(*scan.table, scan.fields, FiltersOnScan(node)));
  const auto count = static_cast<int64_t>(kept->size());
  ProfileNode* pipeline = consumer == nullptr ? nullptr : consumer->Child(0);
  if (consumer != nullptr) {
    pipeline->set_per_part(true);
    consumer->Max("parts", MetricUnit::kCount, count);
    consumer->Max("skipped", MetricUnit::kCount, scan.table->num_parts() - count);
  }
  ARROW_ASSIGN_OR_RAISE(std::shared_ptr<const PipelineBuilds> builds,
                        PipelineBuildsOf(node, consumer));
  int64_t sample = 0;
  int64_t rows = 0;
  while (sample < count && rows < kTwoLevelSampleRows) {
    rows += scan.table->part_rows((*kept)[static_cast<std::size_t>(sample)]).value_or(0);
    ++sample;
  }
  return Parts{.pipeline =
                   [node, kept, pipeline, late, builds](int64_t i) {
                     // Past the kept parts only for the schema sample, which is never opened.
                     const int64_t part =
                         std::cmp_less(i, kept->size()) ? (*kept)[static_cast<std::size_t>(i)] : i;
                     if (late == nullptr) {
                       return Build(node, part, pipeline, nullptr, nullptr, builds.get());
                     }
                     LateScan numbered = *late;
                     numbered.ordinal = i;
                     return Build(node, part, pipeline, &numbered, nullptr, builds.get());
                   },
               .count = count,
               .kept = kept,
               .sample = sample,
               .builds = std::move(builds)};
}

// The sink of a part pipeline as it runs: the sink itself when the pipeline probes no build, else
// the operator that prepares its builds before the sink runs a part, and releases them once its
// parts are done (BuildsFirstOperator). The sink adds its own metrics to `slot` either way.
std::unique_ptr<Operator> WithBuilds(std::unique_ptr<PartSink> sink,
                                     const std::shared_ptr<const PipelineBuilds>& builds,
                                     ProfileNode* slot) {
  if (builds == nullptr) {
    return sink;
  }
  sink->set_profile(slot);  // Profiled() profiles the operator returned, which is not the sink
  return std::make_unique<BuildsFirstOperator>(std::move(sink), builds->builds);
}

// A part pipeline's batches in part order, at most row_cap selected rows per part.
OperatorResult BuildPartUnion(const plan::LogicalNodePtr& node, const plan::ScanNode& scan,
                              std::optional<int64_t> row_cap, ProfileNode* slot) {
  if (slot != nullptr) {
    slot->set_name("PartUnion");
    slot->set_detail(row_cap.has_value() ? std::format("at most {} rows per part", *row_cap)
                                         : std::string("in part order"));
  }
  ARROW_ASSIGN_OR_RAISE(Parts parts, PartsOf(node, scan, slot));
  ARROW_ASSIGN_OR_RAISE(auto sample, parts.pipeline(0));  // for the output schema; never opened
  return WithBuilds(std::make_unique<PartUnionOperator>(std::move(parts.pipeline), parts.count,
                                                        sample->output_schema(), row_cap),
                    parts.builds, slot);
}

// The column every call counts distinctly, when every call is COUNT(DISTINCT) of the same column.
// A global aggregation of only such calls is planned as a GROUP BY of that column, which merges in
// parallel (partitioned); other aggregations with COUNT(DISTINCT) run in two levels
// (TwoLevelAggregation) when they can.
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

// The most rows a late top-N fetches the late columns of (limit + offset).
constexpr int64_t kMaxLateRows = int64_t{64} * 1024;

// The late columns of a part pipeline under a top-N (ADR 0016): the scan's columns that no Filter,
// no Compute expression and no sort key reads, when the pipeline is Filters and Computes over the
// scan (they pass the scan's columns through at their positions); the first of them carries the row
// ids. std::nullopt when no column is late or the pipeline has another node: a Project, or a join's
// probe (ADR 0016's update: late materialization declines over joins).
std::optional<LateScan> LateSplit(const plan::LogicalNodePtr& top, const plan::ScanNode& scan,
                                  const std::vector<plan::SortKey>& keys) {
  const std::size_t width = scan.fields.size();
  std::vector<bool> early(width, false);
  const auto read = [&](int column) {
    if (column >= 0 && std::cmp_less(column, width)) {
      early[static_cast<std::size_t>(column)] = true;
    }
  };
  for (const plan::SortKey& key : keys) {
    read(key.column.index);
  }
  for (const plan::LogicalNode* n = top.get(); n != nullptr;) {
    if (std::holds_alternative<plan::ScanNode>(*n)) {
      break;
    }
    if (const auto* filter = std::get_if<plan::FilterNode>(n)) {
      for (const plan::Predicate& predicate : filter->predicates) {
        if (predicate.column.has_value()) {
          read(predicate.column->index);
        }
        if (predicate.other.has_value()) {
          read(predicate.other->index);
        }
      }
      n = filter->input.get();
    } else if (const auto* compute = std::get_if<plan::ComputeNode>(n)) {
      for (const plan::ExprPtr& expr : compute->exprs) {
        std::vector<int> columns;
        plan::CollectColumns(*expr, columns);
        for (const int column : columns) {
          read(column);
        }
      }
      n = compute->input.get();
    } else {
      return std::nullopt;  // a Project (or other node) renumbers the columns
    }
  }
  LateScan late{.late = std::vector<bool>(width, false), .row_id = -1, .ordinal = 0};
  for (std::size_t i = 0; i < width; ++i) {
    late.late[i] = !early[i];
    if (late.late[i] && late.row_id < 0) {
      late.row_id = static_cast<int>(i);
    }
  }
  if (late.row_id < 0) {
    return std::nullopt;
  }
  return late;
}

// The fields a part pipeline's scan reads: all of them, or with `late` (a narrow scan) the early
// ones.
std::vector<int> ReadFields(const plan::ScanNode& scan, const LateScan* late) {
  if (late == nullptr) {
    return scan.fields;
  }
  std::vector<int> read;
  for (std::size_t i = 0; i < scan.fields.size(); ++i) {
    if (i >= late->late.size() || !late->late[i]) {
      read.push_back(scan.fields[i]);
    }
  }
  return read;
}

// One overload per logical node type: a node type without one fails to compile.
struct Builder {
  std::optional<int64_t> part;     // inside the pipeline of this part
  ProfileNode* slot = nullptr;     // the profile node of the operator built (nullptr: no profile)
  const LateScan* late = nullptr;  // the narrow scan of the part pipeline (late materialization)
  const PartitionTopN* top_n = nullptr;    // a top-N right above this node
  const PipelineBuilds* builds = nullptr;  // with `part`: the builds of the pipeline's joins

  // The profile node of the operator's input (per part as the operator is). A node's name, detail
  // and per-part flag are written by the first build of its plan (at planning time, before the
  // query runs); the builds of the other parts, on worker threads, only read them.
  [[nodiscard]] ProfileNode* Input() const {
    if (slot == nullptr) {
      return nullptr;
    }
    ProfileNode* input = slot->Child(0);
    if (input->per_part() != slot->per_part()) {
      input->set_per_part(slot->per_part());
    }
    return input;
  }
  void Name(std::string_view name, std::string detail = {}) const {
    if (slot != nullptr && slot->name().empty()) {
      slot->set_name(std::string(name));
      if (!detail.empty()) {
        slot->set_detail(std::move(detail));
      }
    }
  }

  OperatorResult operator()(const plan::ScanNode& node) const {
    if (node.table == nullptr) {
      return arrow::Status::Invalid("scan without a table");
    }
    Name("Scan");
    return std::make_unique<TableScanOperator>(
        node.table, node.fields, part,
        late == nullptr ? std::nullopt : std::optional<LateScan>(*late));
  }
  OperatorResult operator()(const plan::FilterNode& node) const {
    Name("Filter");
    if (const auto* scan = std::get_if<plan::ScanNode>(node.input.get());
        scan != nullptr && scan->table != nullptr && part.has_value() &&
        scan->table->supports_scan_filter(ReadFields(*scan, late))) {
      // Filter pushdown (ADR 0020): the scan applies the predicates it can while it decodes.
      std::vector<plan::Predicate> pushed;
      std::vector<plan::Predicate> rest;
      for (const plan::Predicate& predicate : node.predicates) {
        (PushableToScan(predicate) ? pushed : rest).push_back(predicate);
      }
      if (!pushed.empty() && std::ranges::none_of(rest, [](const plan::Predicate& p) {
            return p.kind == plan::Predicate::Kind::kFalse;
          })) {
        ProfileNode* input_slot = Input();
        if (input_slot != nullptr && input_slot->name().empty()) {
          input_slot->set_name("Scan");
          input_slot->set_detail(std::format("{}, {} pushed predicate{}",
                                             plan::ExplainNode(*node.input), pushed.size(),
                                             pushed.size() == 1 ? "" : "s"));
        }
        ARROW_ASSIGN_OR_RAISE(
            auto input,
            Profiled(std::make_unique<TableScanOperator>(
                         scan->table, scan->fields, part,
                         late == nullptr ? std::nullopt : std::optional<LateScan>(*late),
                         std::move(pushed)),
                     node.input, input_slot));
        return std::make_unique<FilterOperator>(std::move(input), std::move(rest));
      }
    }
    ARROW_ASSIGN_OR_RAISE(auto input, Build(node.input, part, Input(), late, nullptr, builds));
    return std::make_unique<FilterOperator>(std::move(input), node.predicates);
  }
  OperatorResult operator()(const plan::ComputeNode& node) const {
    Name("Compute");
    ARROW_ASSIGN_OR_RAISE(auto input, Build(node.input, part, Input(), late, nullptr, builds));
    if (!part.has_value()) {  // over a whole input (an aggregation, a sort): batches in parallel
      return std::make_unique<ParallelComputeOperator>(std::move(input), node.exprs);
    }
    return std::make_unique<ComputeOperator>(std::move(input), node.exprs);
  }
  OperatorResult operator()(const plan::ProjectNode& node) const {
    Name("Project");
    ARROW_ASSIGN_OR_RAISE(auto input, Build(node.input, part, Input(), nullptr, nullptr, builds));
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
      ARROW_ASSIGN_OR_RAISE(Parts parts, PartsOf(node.input, *scan, slot));
      ARROW_ASSIGN_OR_RAISE(auto sample, parts.pipeline(0));
      const int width = sample->output_schema()->num_fields();
      if (TwoLevelAggregation({}, node.aggregates)) {
        Name("PartTwoLevelAggregate");
        // COUNT(DISTINCT) of several columns, or with other calls: grouped by each column in
        // parallel, then counted.
        const ScalarAggregateOperator serial(std::move(sample), node.aggregates);
        return WithBuilds(std::make_unique<PartTwoLevelAggregateOperator>(
                              std::move(parts.pipeline), parts.count, parts.sample, width,
                              std::vector<plan::BoundColumn>{}, node.aggregates,
                              serial.output_schema(), /*global=*/true),
                          parts.builds, slot);
      }
      // Aggregated per part, the parts' states merged in part order.
      Name("PartAggregate");
      return WithBuilds(std::make_unique<PartAggregateOperator>(
                            std::move(parts.pipeline), parts.count, width, node.aggregates),
                        parts.builds, slot);
    }
    Name("ScalarAggregate");
    ARROW_ASSIGN_OR_RAISE(auto input, Build(node.input, std::nullopt, Input()));
    return std::make_unique<ScalarAggregateOperator>(std::move(input), node.aggregates);
  }
  OperatorResult operator()(const plan::GroupAggregateNode& node) const {
    if (const plan::ScanNode* scan = PipelineScan(node.input)) {
      // Grouped per part, the parts' groups merged in part order.
      ARROW_ASSIGN_OR_RAISE(Parts parts, PartsOf(node.input, *scan, slot));
      ARROW_ASSIGN_OR_RAISE(auto sample, parts.pipeline(0));
      const int width = sample->output_schema()->num_fields();
      // The output schema, as the serial operator names it.
      const GroupAggregateOperator serial(std::move(sample), node.keys, node.aggregates);
      if (TwoLevelAggregation(node.keys, node.aggregates)) {
        // COUNT(DISTINCT): an inner GROUP BY of the keys and each distinct column, an outer one
        // of the keys, both partitioned with the heavy keys spread (ADR 0014).
        Name("PartTwoLevelAggregate");
        return WithBuilds(std::make_unique<PartTwoLevelAggregateOperator>(
                              std::move(parts.pipeline), parts.count, parts.sample, width,
                              node.keys, node.aggregates, serial.output_schema(),
                              /*global=*/false),
                          parts.builds, slot);
      }
      Name("PartGroupAggregate",
           top_n == nullptr ? std::string()
                            : std::format("{} top-N per partition keep={}",
                                          plan::ExplainNode(plan::LogicalNode(node)), top_n->keep));
      return WithBuilds(std::make_unique<PartGroupAggregateOperator>(
                            std::move(parts.pipeline), parts.count, width, node.keys,
                            node.aggregates, serial.output_schema(),
                            top_n == nullptr ? std::nullopt : std::optional<PartitionTopN>(*top_n)),
                        parts.builds, slot);
    }
    Name("GroupAggregate");
    ARROW_ASSIGN_OR_RAISE(auto input, Build(node.input, std::nullopt, Input()));
    return std::make_unique<GroupAggregateOperator>(std::move(input), node.keys, node.aggregates);
  }
  OperatorResult operator()(const plan::SortNode& node) const {
    Name("Sort");
    ARROW_ASSIGN_OR_RAISE(auto input, Build(node.input, std::nullopt, Input()));
    return std::make_unique<SortOperator>(std::move(input), node.keys);
  }
  OperatorResult operator()(const plan::LimitNode& node) const {
    // Limit(Sort) with a limit is a top-N: it keeps only limit + offset rows while it reads.
    const auto* sort = std::get_if<plan::SortNode>(node.input.get());
    if (sort != nullptr && node.limit.has_value() && *node.limit > 0) {
      const std::string detail =
          plan::ExplainNode(*node.input) + " " + plan::ExplainNode(plan::LogicalNode(node));
      if (const plan::ScanNode* scan = PipelineScan(sort->input)) {
        // Every part keeps its own first rows, merged in part order.
        Name("PartTopN", detail);
        ARROW_ASSIGN_OR_RAISE(Parts parts, PartsOf(sort->input, *scan, slot));
        ARROW_ASSIGN_OR_RAISE(auto sample, parts.pipeline(0));
        int64_t keep = 0;
        if (__builtin_add_overflow(*node.limit, node.offset, &keep)) {
          keep = std::numeric_limits<int64_t>::max();
        }
        // Late materialization (ADR 0016): the parts read only the columns the pipeline and the
        // keys use, when at most half of the parts can own a row of the result.
        if (auto split = LateSplit(sort->input, *scan, sort->keys);
            split.has_value() && keep <= kMaxLateRows && keep <= parts.count / 2) {
          const auto spec = std::make_shared<const LateScan>(std::move(*split));
          ARROW_ASSIGN_OR_RAISE(Parts narrow, PartsOf(sort->input, *scan, slot, spec));
          ARROW_ASSIGN_OR_RAISE(auto narrow_sample, narrow.pipeline(0));
          LateColumns columns{.table = scan->table,
                              .slots = {},
                              .fields = {},
                              .row_id = spec->row_id,
                              .parts = narrow.kept,
                              .narrow_schema = narrow_sample->output_schema()};
          for (std::size_t i = 0; i < spec->late.size(); ++i) {
            if (spec->late[i]) {
              columns.slots.push_back(static_cast<int>(i));
              columns.fields.push_back(scan->fields[i]);
            }
          }
          if (slot != nullptr) {
            slot->set_detail(std::format("{} late={} columns", detail, columns.slots.size()));
          }
          return WithBuilds(std::make_unique<PartTopNOperator>(
                                std::move(narrow.pipeline), narrow.count, sample->output_schema(),
                                sort->keys, *node.limit, node.offset, std::move(columns)),
                            narrow.builds, slot);
        }
        return WithBuilds(std::make_unique<PartTopNOperator>(std::move(parts.pipeline), parts.count,
                                                             sample->output_schema(), sort->keys,
                                                             *node.limit, node.offset),
                          parts.builds, slot);
      }
      Name("TopN", detail);
      if (std::holds_alternative<plan::GroupAggregateNode>(*sort->input)) {
        // A partitioned GROUP BY below keeps only each partition's first rows (in parallel).
        int64_t keep = 0;
        if (__builtin_add_overflow(*node.limit, node.offset, &keep)) {
          keep = std::numeric_limits<int64_t>::max();
        }
        const PartitionTopN partition_top_n{.keys = sort->keys, .keep = keep};
        ARROW_ASSIGN_OR_RAISE(auto input,
                              Build(sort->input, std::nullopt, Input(), nullptr, &partition_top_n));
        return std::make_unique<SortOperator>(std::move(input), sort->keys, node.limit,
                                              node.offset);
      }
      ARROW_ASSIGN_OR_RAISE(auto input, Build(sort->input, std::nullopt, Input()));
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
      ARROW_ASSIGN_OR_RAISE(
          input, Profiled(BuildPartUnion(node.input, *scan, cap, Input()), node.input, Input()));
    } else {
      ARROW_ASSIGN_OR_RAISE(input, Build(node.input, std::nullopt, Input()));
    }
    Name("Limit");
    return std::make_unique<LimitOperator>(std::move(input), node.limit, node.offset);
  }
  OperatorResult operator()(const plan::RowCountNode& node) const {
    const auto rows = node.table == nullptr ? std::nullopt : node.table->exact_row_count();
    if (!rows) {
      return arrow::Status::Invalid("RowCount over a table without an exact row count");
    }
    Name("RowCount");
    return std::make_unique<RowCountOperator>("count_star()", *rows);
  }
  OperatorResult operator()(const plan::JoinNode& node) const {
    if (node.kind != plan::JoinKind::kInner) {
      // Exit code 4 until E2 runs the other kinds (ADR 0022).
      return plan::UnsupportedError(
          std::format("{} joins are not supported yet", plan::ToString(node.kind)), node.span);
    }
    // An inner join is a hash join (ADR 0022): a build of one input, probed by the other.
    ARROW_ASSIGN_OR_RAISE(JoinShape shape, ShapeOf(node));
    Name("HashJoin");
    std::shared_ptr<JoinBuild> build;
    std::unique_ptr<Operator> probe;
    if (part.has_value()) {
      // A probe of the part pipeline: its build was made with the pipeline (PartsOf), once for
      // all parts, and the operator that runs the pipeline prepares it. The part reaches the
      // probe input only, and only the probe input is profiled under this node.
      build = builds == nullptr ? nullptr : builds->Find(&node);
      if (build == nullptr) {
        return arrow::Status::Invalid("a hash join probe without its build");
      }
      ARROW_ASSIGN_OR_RAISE(probe, Build(shape.probe, part, Input(), nullptr, nullptr, builds));
    } else {
      // A probe over a serial input (no part pipeline below it) prepares its own build at its
      // first Next, profiled under it after its input.
      ARROW_ASSIGN_OR_RAISE(build,
                            MakeJoinBuild(node, shape, slot == nullptr ? nullptr : slot->Child(1)));
      ARROW_ASSIGN_OR_RAISE(probe, Build(shape.probe, std::nullopt, Input()));
    }
    ARROW_ASSIGN_OR_RAISE(
        std::unique_ptr<HashJoinOperator> join,
        HashJoinOperator::Make(std::move(probe), std::move(build), std::move(shape.probe_keys),
                               node.build, node.residual, /*prepares=*/!part.has_value()));
    return join;
  }
};

OperatorResult Profiled(OperatorResult op, const plan::LogicalNodePtr& node, ProfileNode* slot) {
  if (!op.ok() || slot == nullptr) {
    return op;
  }
  if (slot->detail().empty()) {
    slot->set_detail(plan::ExplainNode(*node));
  }
  (*op)->set_profile(slot);
  return std::make_unique<ProfiledOperator>(*std::move(op), slot);
}

OperatorResult Build(const plan::LogicalNodePtr& node, std::optional<int64_t> part,
                     ProfileNode* slot, const LateScan* late, const PartitionTopN* top_n,
                     const PipelineBuilds* builds) {
  if (node == nullptr) {
    return arrow::Status::Invalid("logical plan node without its input");
  }
  if (!part.has_value()) {
    if (const plan::ScanNode* scan = PipelineScan(node)) {
      return Profiled(BuildPartUnion(node, *scan, std::nullopt, slot), node, slot);
    }
  }
  return Profiled(
      std::visit(
          Builder{.part = part, .slot = slot, .late = late, .top_n = top_n, .builds = builds},
          *node),
      node, slot);
}

}  // namespace

arrow::Result<std::unique_ptr<Operator>> BuildPhysicalPlan(const plan::LogicalPlan& plan,
                                                           ProfileNode* profile) {
  if (!plan.root || plan.output.empty()) {
    return arrow::Status::Invalid("empty logical plan");
  }
  ARROW_ASSIGN_OR_RAISE(auto root, Build(plan.root, std::nullopt, profile));
  if (std::cmp_not_equal(root->output_schema()->num_fields(), plan.output.size())) {
    return arrow::Status::Invalid("the physical plan has ", root->output_schema()->num_fields(),
                                  " columns, the logical plan ", plan.output.size());
  }
  return root;
}

}  // namespace antb1::exec
