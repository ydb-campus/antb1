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
#include "antb1/plan/catalog.h"
#include "antb1/plan/explain.h"
#include "antb1/plan/logical_plan.h"
#include "antb1/plan/shared_sub_plans.h"
#include "antb1/plan/sql_status.h"
#include "antb1/plan/types.h"

#include "hash_join.h"
#include "materialized_table.h"
#include "parallel_compute.h"
#include "part_operators.h"
#include "part_pruning.h"
#include "profiled_operator.h"
#include "shared_builds.h"

namespace antb1::exec {
namespace {

using OperatorResult = arrow::Result<std::unique_ptr<Operator>>;

// The joins whose probes run in one part pipeline, and their builds, outermost join first: the
// order in which the operator that runs the pipeline prepares them (PrepareBuilds). Made once per
// pipeline, before the factory of its parts, so every part's probe reads the same build and no
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

// A key filter published by a join *above* this node, carried down to a grouped aggregate so that
// the aggregate's own input can drop the rows whose key that join cannot match.
//
// This restores what ADR 0023's unnesting discards. Its Path 2 replaces the domain D with a map
// (the formalization's Lemma 4.2), which keeps the answer and drops the restriction D carried,
// because a map restricts nothing -- so a grouped aggregate under a join ends up grouping a whole
// table by a key when the join above it will read a fraction of those groups. The join's build has
// the keys that matter and already publishes them (ADR 0029's D1); this is the one place more that
// the published filter has to reach.
//
// The key travels as a **ColumnId and not an index**, because every node between the join and the
// aggregate moves positions while an id survives one (logical_plan.h). It is remapped, not merely
// passed: a Project may mint a new id for a column it passes through.
struct AggregateFilter {
  // The build, not its filter: this is decided while the planner builds operators, and a build
  // publishes its filter only when it is prepared, which happens later. D1 reads `key_filter()` at
  // the same late moment and for the same reason.
  std::shared_ptr<JoinBuild> build;
  plan::ColumnId key = plan::kNoColumnId;
};

// Builds a node's operators. Without a part, a part pipeline (PipelineScan) becomes the part
// operators over it; with one, the node is inside the pipeline of that part, whose joins' builds
// are `builds`. With a profile node (`slot`), the operator is profiled into it (profile.h) and its
// inputs into its children. With `top_n` (a top-N right above `node`), a partitioned grouped
// aggregation keeps only each partition's first rows in its order (PartitionTopN). With `above`, a
// join over this node published a filter its grouped aggregate may apply (AggregateFilter).
OperatorResult Build(const plan::LogicalNodePtr& node, std::optional<int64_t> part = std::nullopt,
                     ProfileNode* slot = nullptr, const LateScan* late = nullptr,
                     const PartitionTopN* top_n = nullptr, const PipelineBuilds* builds = nullptr,
                     const AggregateFilter* above = nullptr);
// The operator profiled into `slot` (with the node's EXPLAIN line when it has no detail yet); the
// operator itself without a slot.
OperatorResult Profiled(OperatorResult op, const plan::LogicalNodePtr& node, ProfileNode* slot);

// The node below `node` that a part pipeline goes on through: the input of a Filter, a Compute or
// a Project, and a join's probe input (the input it does not build on: the left one, but for an
// inner join that builds on the left); nullptr for any other node. PipelineScan, FiltersOnScan and
// PipelineBuildsOf all walk a pipeline with it, so they never disagree on its nodes.
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
  if (const auto* join = std::get_if<plan::JoinNode>(&node)) {
    // The probe is whichever input the join does not build on, for every kind: a semi, anti or
    // outer join that builds on its left input probes with its right (ADR 0026's C2b), and the part
    // pipeline is that side's.
    const bool build_left = join->build == plan::BuildSide::kLeft;
    return (build_left ? join->right : join->left).get();
  }
  return nullptr;
}

// Whether `node` is a join that emits its build rows (EmitsBuildRows), which only the top of a
// part pipeline may be: such a join is the sink of its own probe pipeline, so a node above it is
// not in that pipeline and has no part pipeline of its own.
bool IsRightJoin(const plan::LogicalNode& node) {
  const auto* join = std::get_if<plan::JoinNode>(&node);
  return join != nullptr && EmitsBuildRows(join->kind, join->build);
}

// The scan at the bottom of a part pipeline, a chain of streaming nodes over a scan (Filter,
// Compute, Project and joins' probes, PipelineInput); nullptr if `node` is not the top of one. Any
// other node ends the chain: an aggregation, a sort, a limit.
//
// A join that emits its build rows ends the chain too, because it is the sink of its own probe
// pipeline and never a node inside another's. `through_right_join` is how its own sink asks the
// other question -- where does the pipeline this join tops begin -- and only Build's dispatch and
// the PartsOf below it pass true. Every other caller is asking whether it may run `node` inside a
// pipeline of its own, and for such a join the answer is no: an operator above it is serial.
const plan::ScanNode* PipelineScan(const plan::LogicalNodePtr& node,
                                   bool through_right_join = false) {
  for (const plan::LogicalNode* n = node.get(); n != nullptr; n = PipelineInput(*n)) {
    if (IsRightJoin(*n) && (!through_right_join || n != node.get())) {
      return nullptr;
    }
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

// The table field of `key`'s column, for a top-N whose part pipeline's top is `node`: the field a
// part's statistics of that key live under (exec::PartMayBeat), or empty when there are none to
// read.
//
// The pipeline must be the scan itself or nothing but Filters over it -- every node from its top
// down to the scan -- exactly as FiltersOnScan requires, and for the same reason: a Filter keeps
// its input's columns, so the key's index is still the scan's output column. Every other node moves
// them. A Compute appends, a Project may reorder or drop, and a join prepends the columns of the
// input it does not probe, so above a build-on-the-left join the key's index is the probe's
// position plus the build's width. Taking `scan->fields[index]` there reads another column's
// statistics and prunes parts that hold the answer, so none of them may qualify.
//
// The key's type must be one part statistics exist for as well (ParquetTable::part_stats keeps them
// for the integer types and DATE, and for nothing else). A VARCHAR or DOUBLE key would read no
// statistics and prune nothing, so the top-N does not take the bound's cost for it either.
std::optional<int> LeadingKeyField(const plan::LogicalNodePtr& node, const plan::SortKey& key) {
  const plan::ScanNode* scan = PipelineScan(node);
  // A DECIMAL key is admitted as well (ADR 0021's part-statistics item): its bound and the
  // statistics it is compared with are the same column's unscaled values, so no scale enters.
  if (scan == nullptr || key.column.index < 0 || !plan::HasComparableStatistics(key.column.type)) {
    return std::nullopt;
  }
  std::vector<const plan::LogicalNode*> chain;  // the top down to the scan
  for (const plan::LogicalNode* n = node.get(); n != nullptr; n = PipelineInput(*n)) {
    chain.push_back(n);
  }
  for (std::size_t i = 0; i + 1 < chain.size(); ++i) {  // the top included, the scan not
    if (!std::holds_alternative<plan::FilterNode>(*chain[i])) {
      return std::nullopt;
    }
  }
  if (std::cmp_greater_equal(key.column.index, scan->fields.size())) {
    return std::nullopt;
  }
  return scan->fields[static_cast<std::size_t>(key.column.index)];
}

// A join as a hash join runs it: its probe and build inputs, and their keys (a key's left column is
// on the left input, its right column on the right one; a one-row join has none).
struct JoinShape {
  plan::LogicalNodePtr probe;
  plan::LogicalNodePtr build;
  std::vector<int> probe_keys;                // columns of the probe input's output
  std::vector<plan::BoundColumn> build_keys;  // columns of the build input's output
};

// Invalid, never unsupported, for what a correct plan never holds, in this order: a missing input;
// a one-row join with keys or residuals; any other kind without keys; a null-aware
// anti join of other than one key, or with residuals; a residual that is missing or not BOOLEAN; a
// key whose two columns differ in type (the binder casts them to one type); a DOUBLE or BOOLEAN key
// (E1's table takes neither: such an equality is a residual). Checked before any profile line names
// the join by its EXPLAIN text, which reads every residual. The columns of its keys and residuals
// are checked against its inputs once their operators are made (JoinBuildSpec::Make,
// HashJoinOperator::Make): Invalid too.
arrow::Result<JoinShape> ShapeOf(const plan::JoinNode& join) {
  if (join.left == nullptr || join.right == nullptr) {
    return arrow::Status::Invalid("a join without its inputs");
  }
  const std::string_view kind = plan::ToString(join.kind);
  if (join.kind == plan::JoinKind::kOneRow) {
    if (!join.keys.empty() || !join.residual.empty()) {
      return arrow::Status::Invalid("a ONE-ROW join with keys or residuals");
    }
  } else if (join.keys.empty()) {
    return arrow::Status::Invalid("a ", kind, " hash join without keys");
  }
  if (join.kind == plan::JoinKind::kNullAwareAnti &&
      (join.keys.size() != 1 || !join.residual.empty())) {
    return arrow::Status::Invalid("a NULL-AWARE ANTI join of ", join.keys.size(), " keys and ",
                                  join.residual.size(), " residuals");
  }
  if (std::ranges::any_of(join.residual,
                          [](const plan::ExprPtr& residual) { return residual == nullptr; })) {
    return arrow::Status::Invalid("a join residual that is missing");
  }
  if (std::ranges::any_of(join.residual, [](const plan::ExprPtr& residual) {
        return residual->type != plan::LogicalType::kBoolean;
      })) {
    return arrow::Status::Invalid("a join residual that is not BOOLEAN");
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
  // The builds of the joins the pipeline probes; nullptr when it probes none.
  std::shared_ptr<const PipelineBuilds> builds;
};

arrow::Result<Parts> PartsOf(const plan::LogicalNodePtr& node, const plan::ScanNode& scan,
                             ProfileNode* consumer = nullptr,
                             const std::shared_ptr<const LateScan>& late = nullptr,
                             const AggregateFilter* above = nullptr);

// The build of a join of shape `shape`, profiled into `slot` (nullptr: not profiled): keyless for
// a one-row join. A build input that is a part pipeline is built from its parts, with the builds of
// its own joins, which the build prepares first; any other input's operator is drained.
arrow::Result<std::shared_ptr<JoinBuild>> MakeJoinBuild(const plan::JoinNode& join,
                                                        const JoinShape& shape, ProfileNode* slot) {
  if (slot != nullptr) {
    slot->set_name("HashBuild");
    slot->set_detail(plan::ExplainNode(plan::LogicalNode(join)));
  }
  // A join that emits the build rows which did not match needs the rows whose keys are NULL kept:
  // those are unmatched by definition. One predicate decides it here, in the probe and in the sink
  // (EmitsUnmatchedBuildRows), and the probe refuses a build whose spec disagrees. Note the
  // fallback that rewrites a node with build=kRight runs before this, so the flag follows the side
  // the join really runs on, not the one the logical plan asked for.
  const bool keep_null_keys = EmitsUnmatchedBuildRows(join.kind, join.build);
  const auto spec_of = [&](std::shared_ptr<arrow::Schema> schema) {
    return join.kind == plan::JoinKind::kOneRow
               ? JoinBuildSpec::Keyless(std::move(schema))
               : JoinBuildSpec::Make(std::move(schema), shape.build_keys, keep_null_keys);
  };
  if (const plan::ScanNode* scan = PipelineScan(shape.build)) {
    ARROW_ASSIGN_OR_RAISE(Parts parts, PartsOf(shape.build, *scan, slot));
    ARROW_ASSIGN_OR_RAISE(auto sample, parts.pipeline(0));  // for the schema; never opened
    ARROW_ASSIGN_OR_RAISE(auto spec, spec_of(sample->output_schema()));
    return std::make_shared<JoinBuild>(
        std::move(spec), std::move(parts.pipeline), parts.count,
        parts.builds == nullptr ? std::vector<std::shared_ptr<JoinBuild>>{} : parts.builds->builds,
        slot, join.kind);
  }
  ARROW_ASSIGN_OR_RAISE(
      auto input, Build(shape.build, std::nullopt, slot == nullptr ? nullptr : slot->Child(0)));
  ARROW_ASSIGN_OR_RAISE(auto spec, spec_of(input->output_schema()));
  return std::make_shared<JoinBuild>(std::move(spec), std::move(input), slot, join.kind);
}

// The builds of the joins in the part pipeline whose top is `top`, outermost first; nullptr when it
// has none. With the profile node of the operator that runs the pipeline (`consumer`), the k-th
// build is profiled into its child 1 + k (child 0 is the pipeline's).
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
                             ProfileNode* consumer, const std::shared_ptr<const LateScan>& late,
                             const AggregateFilter* above) {
  // Copied into the factory: the aggregate above outlives no part task, so the pointer must not.
  const std::optional<AggregateFilter> inherited =
      above == nullptr ? std::nullopt : std::optional<AggregateFilter>(*above);
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
                   [node, kept, pipeline, late, builds, inherited](int64_t i) {
                     // Past the kept parts only for the schema sample, which is never opened.
                     const int64_t part =
                         std::cmp_less(i, kept->size()) ? (*kept)[static_cast<std::size_t>(i)] : i;
                     if (late == nullptr) {
                       return Build(node, part, pipeline, nullptr, nullptr, builds.get(),
                                    inherited.has_value() ? &*inherited : nullptr);
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

// A join that emits its build rows (EmitsBuildRows; ADR 0026's C2b) as the sink of its own probe
// pipeline, whose top `node` is the join itself: its parts' probes mark, and it emits the build
// rows the marks select. The one sink whose own build is among the pipeline's builds -- the
// operator that prepares them must prepare it before this one opens, which WithBuilds arranges.
OperatorResult BuildRightJoin(const plan::JoinNode& join, const plan::LogicalNodePtr& node,
                              const plan::ScanNode& scan, ProfileNode* slot) {
  if (slot != nullptr) {
    slot->set_name("RightJoin");
    slot->set_detail(plan::ExplainNode(plan::LogicalNode(join)));
  }
  ARROW_ASSIGN_OR_RAISE(Parts parts, PartsOf(node, scan, slot));
  const std::shared_ptr<JoinBuild> build =
      parts.builds == nullptr ? nullptr : parts.builds->Find(&join);
  if (build == nullptr) {
    return arrow::Status::Invalid("a join that emits its build rows without its build");
  }
  ARROW_ASSIGN_OR_RAISE(auto sample, parts.pipeline(0));  // for the output schema; never opened
  return WithBuilds(std::make_unique<PartRightJoinOperator>(std::move(parts.pipeline), parts.count,
                                                            sample->output_schema(), build),
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

// The most of a probe's rows a build may hold and still be worth filtering with: a build of more
// than this share of them rejects too little to pay for the hash of every probe row (ADR 0029's
// D1).
constexpr int64_t kFilterBuildShare = 16;

// What a scan of this part pipeline may be filtered by: the key filter the innermost join's build
// published, and the position of the probe key in `scan`'s field list; none when no join of the
// pipeline offers one this scan can apply. The conditions, in order:
//
//   - only the innermost join is a candidate. The chain from any other one's probe input passes
//     through a join, which prepends the other side's width, so a column index above it is not a
//     column index at the scan -- the same hazard LeadingKeyField states.
//   - the join must not emit an unmatched probe row, or dropping one would lose an answer. INNER
//     and SEMI never do; ANTI and LEFT only when they build on their left and emit build rows
//     (EmitsBuildRows), which is the form ADR 0026's C2b gave them. A null-aware anti join is
//     excluded outright: it answers from the whole set, not row by row.
//   - one key, whose column maps to a field of the scan through FilterNodes alone (a Compute
//     appends, a Project reorders), and whose type the build hashed and a scan decodes the same
//     (CanPublishKeyFilter).
//   - the build must have published a filter, which the type above makes possible and nothing
//     guarantees: its keys may not have fit the budget.
//
// These are the conditions on the *rows* the join can be asked about, so both halves of ADR 0029
// share them: the per-row test (D1, KeyScanOf) and the part test (D2, PartHasNoKey), which then
// require different things of the scan itself.
//
// Called per part, from the part's own pipeline build, which is why the build has already finished
// and `key_filter()` is there to read (hash_join.cc's PrepareBuilds). The last condition is the
// only one of these a plan cannot decide, which is why it is a step of its own
// (ProbeKeyStructure) -- and why the type test has to be stated there rather than ridden in on a
// null filter, which is how it used to be enforced.
struct ProbeKey {
  const JoinBuild* build = nullptr;  // from ProbeKeyOf: its key_filter() is published and not null
  int column = 0;                    // a position in scan.fields, of the probe's one key
};

// ProbeKeyOf's conditions on the join and on the chain down to the scan, every one of them a fact
// of the plan. The one condition that is not -- the build having published its filter, which it
// makes in its finish -- is ProbeKeyOf's own, and so **the key this returns may have no filter to
// read**: only ProbeKeyOf, for a run, and KeyFilterCandidate, for a report of what the planner
// decided, may call it.
std::optional<ProbeKey> ProbeKeyStructure(const plan::ScanNode& scan, const PipelineBuilds* builds,
                                          const std::optional<LateScan>& late) {
  if (builds == nullptr || builds->joins.empty() || builds->builds.empty() || late.has_value()) {
    return std::nullopt;  // a narrow scan reads other fields: its indices are the late scan's
  }
  const plan::JoinNode* join = builds->joins.back();
  const std::shared_ptr<JoinBuild>& build = builds->builds.back();
  if (join == nullptr || build == nullptr) {
    return std::nullopt;
  }
  if (join->kind != plan::JoinKind::kInner && join->kind != plan::JoinKind::kSemi &&
      !EmitsBuildRows(join->kind, join->build)) {
    return std::nullopt;
  }
  if (join->kind == plan::JoinKind::kNullAwareAnti || join->kind == plan::JoinKind::kOneRow) {
    return std::nullopt;
  }
  // No build on this key can make a filter, whatever happens at run time: it is one key of a type
  // the probe hashes the same way or nothing (CanPublishKeyFilter). This is the condition that made
  // the old `key_filter() == nullptr` test do two jobs, and it belongs here, with the other
  // conditions a plan decides -- a report that left it out would offer a candidate on, say, a
  // VARCHAR key, where no run could ever attach one.
  if (!CanPublishKeyFilter(build->spec())) {
    return std::nullopt;
  }
  const arrow::Result<JoinShape> shape = ShapeOf(*join);
  if (!shape.ok() || shape->probe_keys.size() != 1) {
    return std::nullopt;
  }
  const int column = shape->probe_keys.front();
  if (column < 0 || std::cmp_greater_equal(column, scan.fields.size())) {
    return std::nullopt;
  }
  // Only FilterNodes between the join's probe input and the scan preserve a column's index.
  for (const plan::LogicalNode* n = PipelineInput(*join); n != nullptr; n = PipelineInput(*n)) {
    if (std::holds_alternative<plan::ScanNode>(*n)) {
      if (&std::get<plan::ScanNode>(*n) != &scan) {
        return std::nullopt;  // another scan of the pipeline, not this one
      }
      break;
    }
    if (!std::holds_alternative<plan::FilterNode>(*n)) {
      return std::nullopt;
    }
  }
  return ProbeKey{.build = build.get(), .column = column};
}

std::optional<ProbeKey> ProbeKeyOf(const plan::ScanNode& scan, const PipelineBuilds* builds,
                                   const std::optional<LateScan>& late) {
  const std::optional<ProbeKey> key = ProbeKeyStructure(scan, builds, late);
  if (!key.has_value() || key->build->key_filter() == nullptr) {
    return std::nullopt;
  }
  return key;
}

// The field of `scan` a key filter could be applied to, as a position in `scan.fields`: every
// condition a plan decides holds, and none of the four only a run can test -- whether the build
// published a filter at all, whether it stayed small enough for D1's share gate, which layout it
// chose (a direct build takes no per-row filter), and whether a part's statistics exclude its keys.
// For `explain --physical`, which reports what the planner decided and must not claim what it
// cannot know, so what this finds is a *candidate*.
std::optional<int> KeyFilterCandidate(const plan::ScanNode& scan, const PipelineBuilds* builds,
                                      const std::optional<LateScan>& late) {
  const std::optional<ProbeKey> key = ProbeKeyStructure(scan, builds, late);
  return key.has_value() ? std::optional<int>(key->column) : std::nullopt;
}

// A scan field's name as EXPLAIN writes a column (plan/explain.cc's Name): a plain identifier as
// is, anything else double-quoted.
std::string ScanFieldName(const plan::ScanNode& scan, int column) {
  const std::string& name =
      scan.table->schema()->field(scan.fields[static_cast<std::size_t>(column)])->name();
  return plan::IsPlainIdentifier(name) ? name : "\"" + plan::EscapeText(name, '"') + "\"";
}

// Whether `scan` can reject rows with the build's filter, row by row as it decodes them (ADR
// 0029's D1). Beyond ProbeKeyOf's conditions this needs the table to support a scan filter at all
// over the fields it reads, the build to have come out hashed rather than direct, and the build to
// be small enough that the filter's own cost is worth paying.
bool CanFilterRows(const plan::ScanNode& scan, const ProbeKey& key) {
  if (!scan.table->supports_scan_filter(ReadFields(scan, nullptr))) {
    return false;
  }
  const std::shared_ptr<const JoinTable>& table = key.build->table();
  const std::optional<int64_t> probe_rows = scan.table->exact_row_count();
  if (table == nullptr || !probe_rows.has_value()) {
    return false;
  }
  // **Not for a direct-layout build, whatever it would reject.** A direct probe is two loads of
  // `offsets_` and no hash at all, about half a nanosecond a row, where the filter costs about five
  // -- so it cannot pay there even when it rejects almost everything, and it is the one shape where
  // it can lose badly. Measured (ADR 0029's D1): refusing it is 0.999x over the 22 **while removing
  // filters that reject 137,992,368 and 58,293,472 probe rows**, which is the mechanism confirmed
  // by removal rather than by addition; and a dense-key build of 5,000,000 keys under a
  // 200,000,000-row probe it covers entirely goes 0.691x, the filter having cost 45% for nothing.
  //
  // **This gates the probe-side arm of D1, and deliberately not the other two paths.** D2 keeps the
  // interval it prunes parts with, because the build still publishes its filter and `KeyScanOf`
  // asks `PartHasNoKey` separately -- a direct build still skips a part that holds no key of its.
  // And D5, the filter a join above a grouped aggregate offers that aggregate's own scan, still
  // takes a direct build's filter (its own share gate is inline below): **the argument above does
  // not transfer to it, measured.** What D5 buys is groups the aggregate never builds, so what it
  // has to beat is the aggregate's per-row cost and not the join probe's -- a cheap probe bounds
  // neither. Extending this refusal to D5 costs **1.849x** on a direct build above a grouped
  // aggregate (ADR 0029 carries the construction), where refusing it here is worth 0.594x to
  // 0.765x. The two paths want opposite answers for the same build.
  if (table->layout() == JoinTable::Layout::kDirect) {
    return false;
  }
  // The build must be small against what it filters, or the filter holds so many keys that it
  // rejects nothing and the probe pays the hash for it. A build's rows are an upper bound on its
  // distinct keys, and the probe's table rows a lower bound on what the filter is asked about, so
  // the ratio errs towards refusing -- which is the right direction for a filter that is only ever
  // an optimisation. Measured: the four queries this helps build 0.01 to 0.02 of their probe's
  // rows, and the one it cost 14% builds 0.25 of them (every key of the probe's domain), so the
  // share separates them by an order of magnitude either way. It bounds the build's ROWS against
  // the probe table's and so says nothing about key coverage, which is why the layout test above
  // does the work for the shape that covers everything.
  return table->num_rows() <= *probe_rows / kFilterBuildShare;
}

// Whether part `part` of `scan` holds no key the innermost join's build published, so that it
// matches nothing and need not be read at all: no open, no decode, no filtering (ADR 0029's D2).
//
// It asks less of the scan than the per-row filter does, and deliberately so. A part that is never
// read needs no filtered decoder, so `supports_scan_filter` is beside the point; it takes none of
// the single filter slot, so a scan that already pushes static predicates can still be skipped; and
// the test costs one binary search whether it prunes or not, so the build's share of the probe --
// which bounds the per-row filter's cost -- has nothing to say about it either.
//
// Statistics the table does not have never prune. A part whose key column is entirely NULL always
// does: a NULL key matches nothing on either side, which is the answer the join gives anyway and is
// why the build published no NULL key to test against.
bool PartHasNoKey(const plan::ScanNode& scan, const ProbeKey& key, int64_t part) {
  if (!key.build->key_filter()->prunes_parts()) {
    return false;
  }
  const std::optional<plan::PartStats> stats =
      scan.table->part_stats(part, scan.fields[static_cast<std::size_t>(key.column)]);
  if (!stats.has_value() || stats->rows <= 0) {
    return false;
  }
  if (!stats->min.has_value() || !stats->max.has_value()) {
    return stats->null_count >= stats->rows;  // every value NULL, so no value to match
  }
  return !key.build->key_filter()->MayHoldAKeyIn(*stats->min, *stats->max);
}

// What a hash join's build says about part `part` of `scan`: both halves of ADR 0029, resolved
// together because they share every condition on the join (ProbeKeyOf) and differ only in what they
// then ask of the scan. None when neither applies.
std::optional<TableScanOperator::KeyScan> KeyScanOf(const plan::ScanNode& scan,
                                                    const PipelineBuilds* builds,
                                                    const std::optional<LateScan>& late,
                                                    int64_t part) {
  const std::optional<ProbeKey> key = ProbeKeyOf(scan, builds, late);
  if (!key.has_value()) {
    return std::nullopt;
  }
  const bool no_key = PartHasNoKey(scan, *key, part);
  const bool rows = CanFilterRows(scan, *key);
  if (!no_key && !rows) {
    return std::nullopt;
  }
  return TableScanOperator::KeyScan{.filter = rows ? key->build->key_filter() : nullptr,
                                    .column = key->column,
                                    .part_has_no_key = no_key};
}

// One overload per logical node type: a node type without one fails to compile.
// The filter `node` may offer a grouped aggregate under its probe, or none.
//
// One key, because the aggregate applies it to one column; the probe side's key by id, since that
// is what survives the nodes in between. The join must not emit an unmatched probe row -- dropping
// rows from the aggregate's input drops whole groups and so drops probe rows -- which is D1's rule
// (EmitsBuildRows), and a null-aware anti join and a one-row join are excluded as they are there.
AggregateFilter ProbeAggregateFilter(const plan::JoinNode& node, const JoinShape& shape,
                                     const std::shared_ptr<JoinBuild>& build) {
  if (build == nullptr || node.keys.size() != 1 || shape.probe == nullptr) {
    return {};
  }
  if (node.kind != plan::JoinKind::kInner && node.kind != plan::JoinKind::kSemi &&
      !EmitsBuildRows(node.kind, node.build)) {
    return {};
  }
  if (node.kind == plan::JoinKind::kNullAwareAnti || node.kind == plan::JoinKind::kOneRow) {
    return {};
  }
  const plan::BoundColumn& key =
      node.build == plan::BuildSide::kLeft ? node.keys.front().right : node.keys.front().left;
  if (key.id == plan::kNoColumnId) {
    return {};
  }
  return AggregateFilter{.build = build, .key = key.id};
}

struct Builder {
  std::optional<int64_t> part;     // inside the pipeline of this part
  ProfileNode* slot = nullptr;     // the profile node of the operator built (nullptr: no profile)
  const LateScan* late = nullptr;  // the narrow scan of the part pipeline (late materialization)
  const PartitionTopN* top_n = nullptr;    // a top-N right above this node
  const PipelineBuilds* builds = nullptr;  // with `part`: the builds of the pipeline's joins
  // A filter a join above published, on its way to a grouped aggregate (AggregateFilter). Only the
  // nodes that keep every row carry it on: a Filter as it is, a Project and a Compute with the key
  // remapped, and every other node drops it.
  const AggregateFilter* above = nullptr;

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

  // The field of `node` a key filter could be applied to, from either source a run would take one
  // from: a join of this pipeline (ADR 0029's D1 and D2, KeyFilterCandidate) or a join above the
  // grouped aggregate this scan feeds. Only the conditions a plan decides, so the answer is a
  // candidate and never a fact; none outside a part pipeline, which is the only place either is
  // attached.
  [[nodiscard]] std::optional<int> CandidateKeyField(
      const plan::ScanNode& node, const std::optional<LateScan>& late_scan) const {
    if (!part.has_value()) {
      return std::nullopt;
    }
    if (const std::optional<int> column = KeyFilterCandidate(node, builds, late_scan);
        column.has_value()) {
      return column;
    }
    // CanPublishKeyFilter here too, and for the same reason it is in ProbeKeyStructure: this arm
    // reads its filter from a build as well, so a key no build can publish for has no candidate on
    // either of them. The run path tests it through `above->build->key_filter()`, which is null for
    // such a key -- the very implication a report cannot lean on.
    if (above == nullptr || above->build == nullptr || late_scan.has_value() ||
        !CanPublishKeyFilter(above->build->spec()) ||
        !node.table->supports_scan_filter(ReadFields(node, nullptr))) {
      return std::nullopt;
    }
    const auto at = std::ranges::find(node.ids, above->key);
    return at == node.ids.end() ? std::nullopt
                                : std::optional<int>(static_cast<int>(at - node.ids.begin()));
  }

  OperatorResult operator()(const plan::ScanNode& node) const {
    if (node.table == nullptr) {
      return arrow::Status::Invalid("scan without a table");
    }
    const std::optional<LateScan> late_scan =
        late == nullptr ? std::nullopt : std::optional<LateScan>(*late);
    // A dynamic join filter, where this scan is the probe side of a join whose build published one
    // (ADR 0029). Both halves are attached here and in the pushdown branch of the FilterNode below:
    // a scan takes one ScanFilter, and where it also has predicates of its own it applies the
    // conjunction of the two (MakeBothScanFilters).
    std::optional<TableScanOperator::KeyScan> key_scan;
    if (part.has_value()) {
      key_scan = KeyScanOf(node, builds, late_scan, *part);
    }
    // A filter from a join above the grouped aggregate this scan feeds, where the scan still has
    // the column it groups by. Finding the key among `node.ids` is also the check that nothing
    // between here and the aggregate moved it: only a Filter keeps an id in place, which is the
    // same chain D1 requires. It yields to a filter of this scan's own, since a scan takes one.
    if (!key_scan.has_value() && above != nullptr && part.has_value() && !late_scan.has_value() &&
        above->build != nullptr && above->build->key_filter() != nullptr &&
        node.table->supports_scan_filter(ReadFields(node, nullptr))) {
      const auto at = std::ranges::find(node.ids, above->key);
      const std::shared_ptr<const JoinTable>& table = above->build->table();
      const std::optional<int64_t> rows = node.table->exact_row_count();
      // The same share the per-row filter is gated on, and for the same reason: a build holding
      // too many of the keys asked about rejects nothing and the scan pays the hash for it.
      if (at != node.ids.end() && table != nullptr && rows.has_value() &&
          table->num_rows() <= *rows / kFilterBuildShare) {
        key_scan = TableScanOperator::KeyScan{
            .filter = above->build->key_filter(),
            .column = static_cast<int>(at - node.ids.begin()),
        };
      }
    }
    Name("Scan");
    // What a key filter could do here, which is all a plan knows and all `explain --physical`
    // reports: the planner's own conditions hold on this scan's field, and the conditions that
    // decide whether a filter is really attached are the build's at run time. Written on the first
    // build of the plan only, where the detail is still empty (Profiled fills it after this), and
    // the node is rewrapped for ExplainNode because a visitor sees the alternative and not the
    // LogicalNode holding it -- a copy of a scan's vectors, once per scan, at planning time.
    if (slot != nullptr && slot->detail().empty()) {
      if (const std::optional<int> column = CandidateKeyField(node, late_scan);
          column.has_value()) {
        // Which of the two mechanisms this scan could take, which is not the same question as
        // whether a key reaches it: the per-row filter needs a filtered decoder over the fields
        // this scan reads, and a part the published keys are not in is skipped whole and needs
        // none (ADR 0029's D2). Without that decoder the honest claim is the narrower one.
        const bool rows = node.table->supports_scan_filter(ReadFields(node, nullptr));
        slot->set_detail(
            std::format("{}, {} candidate on {}", plan::ExplainNode(plan::LogicalNode(node)),
                        rows ? "key filter" : "part skip", ScanFieldName(node, *column)));
      }
    }
    // Per part, so a reader can see the filter is in use. It is a metric and not the node's detail
    // because the plan-time build of this pipeline runs before the join's build exists and so
    // carries no filter -- and only that first build may write a name or a detail.
    if (key_scan.has_value() && slot != nullptr) {
      if (key_scan->filter != nullptr) {
        slot->Max("key_filter", MetricUnit::kCount, 1);
      }
      if (key_scan->part_has_no_key) {
        slot->Add("key_pruned", MetricUnit::kCount, 1);
      }
    }
    return std::make_unique<TableScanOperator>(node.table, node.fields, part, late_scan,
                                               std::vector<plan::Predicate>{}, std::move(key_scan));
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
        const bool names_it = input_slot != nullptr && input_slot->name().empty();
        if (names_it) {
          input_slot->set_name("Scan");
          input_slot->set_detail(std::format("{}, {} pushed predicate{}",
                                             plan::ExplainNode(*node.input), pushed.size(),
                                             pushed.size() == 1 ? "" : "s"));
        }
        const std::optional<LateScan> late_scan =
            late == nullptr ? std::nullopt : std::optional<LateScan>(*late);
        // What a key filter could do here, for `explain --physical`, and it is the same as on a
        // scan with no predicates of its own now that the two can be applied together.
        if (names_it) {
          if (const std::optional<int> column = KeyFilterCandidate(*scan, builds, late_scan);
              column.has_value()) {
            input_slot->set_detail(
                input_slot->detail() +
                std::format(", key filter candidate on {}", ScanFieldName(*scan, *column)));
          }
        }
        // **Both halves of ADR 0029 over pushed predicates.** The part test never needed the filter
        // slot -- a part no published key can be in is not read at all -- and the per-row filter no
        // longer competes for it either: the scan applies the conjunction of the two
        // (MakeBothScanFilters), which is the item that ADR deferred. A scan reaching here is
        // already decoding with a filter because of its predicates, so the key filter adds its own
        // cost and nothing else; measured 0.992x over the 22, with no query slower.
        std::optional<TableScanOperator::KeyScan> key_scan =
            KeyScanOf(*scan, builds, late_scan, *part);
        if (key_scan.has_value() && input_slot != nullptr) {
          // The same two metrics as the branch above, now that the same two things can happen here:
          // a filter that runs unreported is one no test can assert and no profile can explain.
          if (key_scan->filter != nullptr) {
            input_slot->Max("key_filter", MetricUnit::kCount, 1);
          }
          if (key_scan->part_has_no_key) {
            input_slot->Add("key_pruned", MetricUnit::kCount, 1);
          }
        }
        ARROW_ASSIGN_OR_RAISE(auto input, Profiled(std::make_unique<TableScanOperator>(
                                                       scan->table, scan->fields, part, late_scan,
                                                       std::move(pushed), std::move(key_scan)),
                                                   node.input, input_slot));
        return std::make_unique<FilterOperator>(std::move(input), std::move(rest));
      }
    }
    // `above` carried on: a Filter only removes rows, so it neither mints nor moves a ColumnId and
    // the key the aggregate below groups by is still the key the join above published (the field's
    // own contract). The branch over this one does not read it -- it builds the scan itself, from
    // the joins of the scan's own pipeline -- which is why an offer does not reach a scan whose
    // predicates are pushed. Not the filter slot: a scan applies the conjunction of the two.
    ARROW_ASSIGN_OR_RAISE(auto input,
                          Build(node.input, part, Input(), late, nullptr, builds, above));
    return std::make_unique<FilterOperator>(std::move(input), node.predicates);
  }
  OperatorResult operator()(const plan::ComputeNode& node) const {
    Name("Compute");
    // A Compute appends, so its input's ids are its own: the key passes through unchanged unless it
    // *is* one of the appended expressions, which is a computed column and never a grouping key.
    const AggregateFilter* carried = above;
    if (carried != nullptr && std::ranges::contains(node.ids, carried->key)) {
      carried = nullptr;
    }
    ARROW_ASSIGN_OR_RAISE(auto input,
                          Build(node.input, part, Input(), late, nullptr, builds, carried));
    if (!part.has_value()) {  // over a whole input (an aggregation, a sort): batches in parallel
      return std::make_unique<ParallelComputeOperator>(std::move(input), node.exprs);
    }
    return std::make_unique<ComputeOperator>(std::move(input), node.exprs);
  }
  OperatorResult operator()(const plan::ProjectNode& node) const {
    Name("Project");
    // A Project renames: the key arrives as one of *its* output ids, and what the input calls that
    // column is the id the matching entry reads. Without this remap the match at the aggregate
    // fails silently, since a Project mints a new id even for a column it only passes through.
    AggregateFilter remapped;
    const AggregateFilter* carried = nullptr;
    if (above != nullptr) {
      for (std::size_t i = 0; i < node.ids.size() && i < node.columns.size(); ++i) {
        if (node.ids[i] == above->key && node.columns[i].id != plan::kNoColumnId) {
          remapped = AggregateFilter{.build = above->build, .key = node.columns[i].id};
          carried = &remapped;
          break;
        }
      }
    }
    ARROW_ASSIGN_OR_RAISE(auto input,
                          Build(node.input, part, Input(), nullptr, nullptr, builds, carried));
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
      // A filter the join above offered, if its key is one this aggregate **groups by**: then the
      // filter drops whole groups and never changes a surviving group's value, which is the whole
      // correctness argument. On any other column it would corrupt every aggregate, so the key has
      // to be found among key_ids and nowhere else. `keys[i]` is what the input calls that column.
      AggregateFilter inner;
      const AggregateFilter* below = nullptr;
      if (above != nullptr) {
        for (std::size_t i = 0; i < node.key_ids.size() && i < node.keys.size(); ++i) {
          if (node.key_ids[i] == above->key && node.keys[i].id != plan::kNoColumnId) {
            inner = AggregateFilter{.build = above->build, .key = node.keys[i].id};
            below = &inner;
            break;
          }
        }
      }
      // Grouped per part, the parts' groups merged in part order.
      ARROW_ASSIGN_OR_RAISE(Parts parts, PartsOf(node.input, *scan, slot, nullptr, below));
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
        // The first sort key's table field, when its statistics can skip a part as the top-N runs.
        const std::optional<int> key_field = LeadingKeyField(sort->input, sort->keys.front());
        const auto pruning = [&](const std::shared_ptr<const std::vector<int64_t>>& kept) {
          return key_field.has_value()
                     ? std::optional(
                           TopNPruning{.table = scan->table, .field = *key_field, .parts = kept})
                     : std::nullopt;
        };
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
          return WithBuilds(
              std::make_unique<PartTopNOperator>(
                  std::move(narrow.pipeline), narrow.count, sample->output_schema(), sort->keys,
                  *node.limit, node.offset, std::move(columns), pruning(narrow.kept)),
              narrow.builds, slot);
        }
        return WithBuilds(
            std::make_unique<PartTopNOperator>(std::move(parts.pipeline), parts.count,
                                               sample->output_schema(), sort->keys, *node.limit,
                                               node.offset, std::nullopt, pruning(parts.kept)),
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
    // A join is a hash join (ADR 0022): a build of one input, probed by the other.
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
      // The published filter offered to a grouped aggregate below the probe (AggregateFilter).
      //
      // The probe side only, because that is where it is ready: Start() prepares the build and
      // *then* opens the probe input (hash_join.cc), while the build side's own pipeline runs
      // inside that Prepare. **This is about the benefit and not about correctness** -- a build
      // that is not prepared has no filter to publish, `key_filter()` is null, and nothing
      // attaches. Reading one too early cannot produce a wrong answer, only a missed filter, which
      // is why no test can catch the difference.
      const AggregateFilter offered = ProbeAggregateFilter(node, shape, build);
      ARROW_ASSIGN_OR_RAISE(probe, Build(shape.probe, std::nullopt, Input(), nullptr, nullptr,
                                         nullptr, offered.build == nullptr ? nullptr : &offered));
    }
    ARROW_ASSIGN_OR_RAISE(
        std::unique_ptr<HashJoinOperator> join,
        HashJoinOperator::Make(std::move(probe), std::move(build), std::move(shape.probe_keys),
                               node.build, node.residual, /*prepares=*/!part.has_value()));
    return join;
  }
  OperatorResult operator()(const plan::DependentJoinNode& node) const {
    // No operator runs a dependent join: the first pass of plan::Optimize replaces every one or
    // rejects the query, so only a plan a test builds by hand reaches here. kUnsupported, never
    // NotImplemented, so that such a plan exits 4 as the same query does (ADR 0023, ADR 0005).
    return plan::UnsupportedError("a correlated subquery is not supported", node.span);
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
                     const PipelineBuilds* builds, const AggregateFilter* above) {
  if (node == nullptr) {
    return arrow::Status::Invalid("logical plan node without its input");
  }
  if (!part.has_value() && IsRightJoin(*node)) {
    // It emits the build's rows, so it has to see every part's marks before it emits one: it is
    // the sink of its own probe pipeline.
    if (const plan::ScanNode* scan = PipelineScan(node, /*through_right_join=*/true)) {
      return Profiled(BuildRightJoin(std::get<plan::JoinNode>(*node), node, *scan, slot), node,
                      slot);
    }
    // Its probe has no part pipeline under it, so there are no parts and no sink to emit from. The
    // build side is a performance choice and never a semantic one (ADR 0026), so the join runs the
    // way every join ran before C2b instead: building on its right input. The node carries the
    // side it really runs, so a profile and an EXPLAIN name that one.
    plan::JoinNode right = std::get<plan::JoinNode>(*node);
    right.build = plan::BuildSide::kRight;
    const auto rebuilt = std::make_shared<const plan::LogicalNode>(plan::LogicalNode(right));
    return Build(rebuilt, part, slot, late, top_n, builds);
  }
  if (!part.has_value()) {
    if (const plan::ScanNode* scan = PipelineScan(node)) {
      return Profiled(BuildPartUnion(node, *scan, std::nullopt, slot), node, slot);
    }
  }
  return Profiled(std::visit(Builder{.part = part,
                                     .slot = slot,
                                     .late = late,
                                     .top_n = top_n,
                                     .builds = builds,
                                     .above = above},
                             *node),
                  node, slot);
}

// The least a shared sub-plan must read before holding its result is worth it. ADR 0030 asks two
// things of the gate -- that the sub-plan be "dear enough to be worth materialising" and its result
// "small enough to hold" -- and this is the first. Measured: a shared join reading about a million
// rows is **1.017x**, slower than recomputing it, because recomputing something cheap costs less
// than materialising it and scanning it twice; the one that pays reads 600,037,902.
constexpr int64_t kMinSharedRows = 10'000'000;

// The rows the scans under `node` read, which is what sharing it avoids reading twice. Unknown
// (none) if any of them cannot say, which refuses the group: a sub-plan whose cost cannot be seen
// is not one to hold a result for.
std::optional<int64_t> InputRows(const plan::LogicalNode& node) {
  if (const auto* scan = std::get_if<plan::ScanNode>(&node)) {
    return scan->table == nullptr ? std::nullopt : scan->table->exact_row_count();
  }
  int64_t rows = 0;
  for (const plan::LogicalNodePtr& input : plan::InputsOf(node)) {
    if (input == nullptr) {
      return std::nullopt;
    }
    const std::optional<int64_t> theirs = InputRows(*input);
    if (!theirs.has_value()) {
      return std::nullopt;
    }
    rows += *theirs;
  }
  return rows;
}

// Whether a shared sub-plan whose top is `node` is worth computing once.
//
// Two questions, and the measurement had to answer both. **Does it reduce?** Holding a result to
// avoid recomputing it pays only when the result is much smaller than the work behind it. **Is the
// work worth avoiding?** A sub-plan that reduces a little data is cheaper to run twice than to
// hold.
//
// Project and Compute are looked through rather than judged: they keep every row, so whether the
// sub-plan reduces is decided below them. That matters -- the shared sub-plan of the query this
// item was built for is headed by a Project over a grouped aggregate, and a gate that read only the
// top node would refuse the one case it exists to serve.
//
// Below them, **only an aggregate qualifies**, and that is a measurement and not a judgement about
// shapes. An aggregate reduces by definition. A join does not reliably: the two join-topped groups
// of this workload came out at **0.937x for 269 MB held** and **1.017x**, so one bought 30 ms with
// a quarter of a gigabyte and the other was slower than recomputing. Carrying a path with those
// numbers would be worse than recording them, so the join case is gone and ADR 0030 says why.
// Anything else -- a Filter, a bare Scan -- never qualified: that is how a 379-million-row filtered
// scan is refused by **shape**, not by an estimate that might be wrong.
bool WorthSharing(const plan::LogicalNode& node) {
  const plan::LogicalNode* reducing = nullptr;
  for (const plan::LogicalNode* n = &node; n != nullptr;) {
    if (std::holds_alternative<plan::AggregateNode>(*n) ||
        std::holds_alternative<plan::GroupAggregateNode>(*n)) {
      reducing = n;
      break;
    }
    if (!std::holds_alternative<plan::ProjectNode>(*n) &&
        !std::holds_alternative<plan::ComputeNode>(*n)) {
      return false;
    }
    const std::vector<plan::LogicalNodePtr> inputs = plan::InputsOf(*n);
    n = inputs.empty() ? nullptr : inputs.front().get();
  }
  if (reducing == nullptr) {
    return false;
  }
  // The result's own size is **not** tested here, and that is deliberate rather than forgotten: the
  // gate admits a sub-plan because it reduces, and nothing records an aggregate's output
  // cardinality, so the size is not knowable until the rows arrive.
  // MaterializedTable::kMaxBytes is the bound, enforced where it can be.
  const std::optional<int64_t> rows = InputRows(node);
  return rows.has_value() && *rows >= kMinSharedRows;
}

// `root` with every node of `replacements` replaced by its scan of a shared result.
plan::LogicalNodePtr Substituted(
    const plan::LogicalNodePtr& root,
    const std::unordered_map<const plan::LogicalNode*, plan::LogicalNodePtr>& replacements) {
  if (root == nullptr) {
    return root;
  }
  if (const auto found = replacements.find(root.get()); found != replacements.end()) {
    return found->second;
  }
  std::vector<plan::LogicalNodePtr> inputs = plan::InputsOf(*root);
  bool changed = false;
  for (plan::LogicalNodePtr& input : inputs) {
    plan::LogicalNodePtr next = Substituted(input, replacements);
    changed = changed || next.get() != input.get();
    input = std::move(next);
  }
  return changed ? plan::WithInputs(*root, std::move(inputs)) : root;
}

}  // namespace

arrow::Result<std::unique_ptr<Operator>> BuildPhysicalPlan(const plan::LogicalPlan& plan,
                                                           ProfileNode* profile) {
  if (!plan.root || plan.output.empty()) {
    return arrow::Status::Invalid("empty logical plan");
  }
  // A sub-plan the plan computes more than once is computed once instead, into a table its
  // consumers scan (ADR 0030's E2). Each member becomes its own ScanNode over one shared Table
  // object, carrying its own column ids, so nothing above a consumer is rewritten and no pass ever
  // sees a DAG -- the sharing lives entirely in a shared_ptr<Table> that ScanNode already holds.
  std::vector<SharedBuild> shared;
  std::unordered_map<const plan::LogicalNode*, plan::LogicalNodePtr> replacements;
  for (const plan::SharedSubPlan& group : plan::SharedSubPlans(plan)) {
    if (group.members.size() < 2 || group.widest >= group.members.size() ||
        group.columns.size() != group.members.size()) {
      continue;
    }
    // A group with a member inside an already-chosen one is left alone: nested sharing would have
    // to decide which result the inner consumers read, and E1 reports the outermost first.
    if (std::ranges::any_of(group.members, [&replacements](const plan::LogicalNodePtr& member) {
          return replacements.contains(member.get());
        })) {
      continue;
    }
    const plan::LogicalNodePtr& widest = group.members[group.widest];
    ARROW_ASSIGN_OR_RAISE(std::unique_ptr<Operator> op, Build(widest, std::nullopt, nullptr));
    if (!WorthSharing(*widest)) {
      continue;
    }
    auto table = std::make_shared<MaterializedTable>(op->output_schema());
    for (std::size_t i = 0; i < group.members.size(); ++i) {
      const plan::LogicalNodePtr& member = group.members[i];
      std::vector<plan::ColumnId> ids = plan::OutputIds(*member);
      if (ids.size() != group.columns[i].size()) {
        return arrow::Status::Invalid("a shared sub-plan's columns do not match its output");
      }
      replacements.emplace(member.get(), std::make_shared<const plan::LogicalNode>(plan::ScanNode{
                                             .table = table,
                                             .table_name = "shared",
                                             .fields = group.columns[i],
                                             .ids = std::move(ids),
                                         }));
    }
    shared.push_back(SharedBuild{.table = std::move(table), .op = std::move(op)});
  }

  const plan::LogicalNodePtr root_node =
      replacements.empty() ? plan.root : Substituted(plan.root, replacements);
  ARROW_ASSIGN_OR_RAISE(auto root, Build(root_node, std::nullopt, profile));
  if (std::cmp_not_equal(root->output_schema()->num_fields(), plan.output.size())) {
    return arrow::Status::Invalid("the physical plan has ", root->output_schema()->num_fields(),
                                  " columns, the logical plan ", plan.output.size());
  }
  if (shared.empty()) {
    return root;
  }
  auto builds = std::make_unique<SharedBuildsOperator>(std::move(root), std::move(shared));
  // Profiled into the root's own node, so the shared results' time and rows are visible without
  // the profile tree changing shape. The sub-plans themselves are unprofiled: a reader sees what
  // sharing cost and what it produced, and the consumers' `Scan ... source=shared` lines say who
  // reads it, but not the breakdown inside. Enough to tell the optimisation happened, which is
  // what ADR 0030's consequences ask for; less than a full accounting.
  builds->set_profile(profile);
  return builds;
}

}  // namespace antb1::exec
