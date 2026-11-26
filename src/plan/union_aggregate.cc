#include "union_aggregate.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "antb1/common/int128.h"

#include "join_order.h"

namespace antb1::plan {
namespace {

LogicalNodePtr Make(LogicalNode node) {
  return std::make_shared<const LogicalNode>(std::move(node));
}

ColumnId Mint(ColumnId& last) {
  last = ColumnId{std::to_underlying(last) + 1};
  return last;
}

// Pass through projections of scan columns only. A cast, filter or computed projection needs
// different row, error and domain reasoning.
const LogicalNode* BelowProjects(const LogicalNode* node) {
  while (const auto* project = std::get_if<ProjectNode>(node)) {
    if (std::ranges::any_of(project->constants, [](const auto& c) { return c.has_value(); })) {
      return nullptr;
    }
    node = project->input.get();
  }
  return node;
}

std::optional<std::size_t> Position(const std::vector<ColumnId>& ids, ColumnId id) {
  const auto it = std::ranges::find(ids, id);
  if (it == ids.end()) {
    return std::nullopt;
  }
  return static_cast<std::size_t>(it - ids.begin());
}

std::optional<ColumnId> ThroughProjects(const LogicalNode* node, ColumnId id) {
  while (const auto* project = std::get_if<ProjectNode>(node)) {
    const auto pos = Position(project->ids, id);
    if (!pos.has_value() || (!project->constants.empty() && project->constants[*pos].has_value())) {
      return std::nullopt;
    }
    id = project->columns[*pos].id;
    node = project->input.get();
  }
  return id;
}

// Use a table-wide bound including NULL. A single part's dictionary hint cannot bound an arm.
std::optional<int64_t> GroupDomain(const LogicalNodePtr& input, ColumnId id) {
  const auto column = ThroughProjects(input.get(), id);
  if (!column.has_value()) {
    return std::nullopt;
  }
  std::array<ScanKeyDomain, 1> source;
  EachDistinctValues(*input, std::array{*column}, source);
  return source.front().max_values;
}

struct Arm {
  int64_t rows;
  int64_t parts;
  int64_t largest;
};

std::optional<Arm> ScanArm(const LogicalNodePtr& input) {
  const auto* scan = std::get_if<ScanNode>(BelowProjects(input.get()));
  if (scan == nullptr || !scan->table) {
    return std::nullopt;
  }
  const auto total = scan->table->exact_row_count();
  if (!total || *total < 0) {
    return std::nullopt;
  }
  Arm arm{.rows = *total, .parts = 0, .largest = 0};
  for (int64_t p = 0; p < scan->table->num_parts(); ++p) {
    const auto rows = scan->table->part_rows(p);
    if (!rows.has_value() || *rows < 0) {
      return std::nullopt;
    }
    if (*rows > 0) {
      ++arm.parts;
      arm.largest = std::max(arm.largest, *rows);
    }
  }
  return arm;
}

bool NarrowSum(LogicalType type) {
  return (IsInteger(type) && type != LogicalType::kHugeInt) ||
         (type == LogicalType::kDecimal && type.width() <= LogicalType::kMaxNarrowDecimalWidth);
}

bool Mergeable(const AggregateCall& call, Int128 max_join_rows) {
  if (call.kind == AggKind::kCount || call.kind == AggKind::kCountStar) {
    return max_join_rows <= std::numeric_limits<int64_t>::max();
  }
  if (!call.arg.has_value() || call.arg->type == LogicalType::kVarchar ||
      call.arg->type == LogicalType::kDouble) {
    return false;
  }
  if (call.kind == AggKind::kMin || call.kind == AggKind::kMax) {
    return true;
  }
  // Bound every intermediate as well as the final decimal(38,s) partial. Even a decimal64
  // outside the declared precision lies within the physical signed 64-bit range.
  return call.kind == AggKind::kSum && NarrowSum(call.arg->type) &&
         max_join_rows <= (PowerOfTen(38) - 1) / (Int128{std::numeric_limits<int64_t>::max()} + 1);
}

struct Candidate {
  const GroupAggregateNode& group;
  const ComputeNode* compute;
  const JoinNode& join;
  const LogicalNodePtr& probe;
  const LogicalNodePtr& build;
  const UnionAllNode& branches;
  std::vector<Arm> arms;
  int64_t build_rows;
  int64_t groups;
  int64_t rows;
};

std::optional<Candidate> Find(const GroupAggregateNode& group) {
  const auto* compute = std::get_if<ComputeNode>(group.input.get());
  const auto* join =
      std::get_if<JoinNode>(compute != nullptr ? compute->input.get() : group.input.get());
  if (join == nullptr || join->kind != JoinKind::kInner || join->keys.size() != 1 ||
      !join->residual.empty() || group.keys.size() != 1 || group.aggregates.empty()) {
    return std::nullopt;
  }
  const auto& probe = join->build == BuildSide::kRight ? join->left : join->right;
  const auto& build = join->build == BuildSide::kRight ? join->right : join->left;
  const auto* branches = std::get_if<UnionAllNode>(BelowProjects(probe.get()));
  const auto* dim = std::get_if<ScanNode>(BelowProjects(build.get()));
  if (branches == nullptr || branches->inputs.size() < 2 || dim == nullptr || !dim->table ||
      group.keys[0].type == LogicalType::kDouble || group.keys[0].type == LogicalType::kVarchar) {
    return std::nullopt;
  }
  const auto build_rows = dim->table->exact_row_count();
  const auto groups = GroupDomain(build, group.keys[0].id);
  if (!build_rows || *build_rows <= 0 || !groups.has_value()) {
    return std::nullopt;
  }
  Candidate candidate{.group = group,
                      .compute = compute,
                      .join = *join,
                      .probe = probe,
                      .build = build,
                      .branches = *branches,
                      .arms = {},
                      .build_rows = *build_rows,
                      .groups = *groups,
                      .rows = 0};
  for (const auto& input : branches->inputs) {
    const auto arm = ScanArm(input);
    if (!arm.has_value() || arm->rows < 0 ||
        arm->rows > std::numeric_limits<int64_t>::max() - candidate.rows) {
      return std::nullopt;
    }
    candidate.arms.push_back(*arm);
    candidate.rows += arm->rows;
  }
  const Int128 max_rows = Int128{candidate.rows} * candidate.build_rows;
  if (candidate.rows == 0 || !std::ranges::all_of(group.aggregates, [&](const auto& call) {
        return Mergeable(call, max_rows);
      })) {
    return std::nullopt;
  }
  return candidate;
}

bool FitsStates(const BindOptions& options, int64_t groups, std::size_t columns) {
  if (!options.memory_limit.has_value()) {
    return true;
  }
  // The scheduler holds at most four parts per worker, plus the branch and final tables.
  // Reserve room for hash slots, keys, validity, aggregate state and table growth. The rest
  // of the budget covers the join and batches; their actual sizes are charged at execution.
  const Int128 bytes =
      (Int128{128} + (64 * Int128{columns})) * groups * ((4 * Int128{options.threads}) + 2);
  return bytes <= *options.memory_limit / 2;
}

bool WorthSplitting(const Candidate& c, const BindOptions& options) {
  if (options.threads < 2 || Int128{4} * c.groups > std::min<int64_t>(65536, options.batch_size) ||
      !FitsStates(options, c.groups, c.group.aggregates.size())) {
    return false;
  }
  // Units of probe rows: charge a build row or a serial state merge four units and a task 1024.
  // Fold the largest-part limit into the quotient rather than rounding the effective workers.
  const int workers = std::min(options.threads, 4);
  const Int128 keep = c.rows + (Int128{4} * c.build_rows);
  Int128 split = 0;
  for (const auto& arm : c.arms) {
    // Even an empty arm may open its join build.
    split += Int128{4} * c.build_rows;
    if (arm.parts > 0) {
      const int64_t lanes = std::min<int64_t>(workers, arm.parts);
      const Int128 probe = std::max<Int128>(arm.largest, (Int128{arm.rows} + lanes - 1) / lanes);
      split +=
          probe + (Int128{4} * (arm.parts + Int128{1}) * c.groups) + (Int128{1024} * arm.parts);
    }
    if (split > keep) {
      return false;
    }
  }
  return 5 * split <= 4 * keep;
}

LogicalNodePtr ReplaceUnion(const LogicalNodePtr& probe, const LogicalNodePtr& arm) {
  if (const auto* p = std::get_if<ProjectNode>(probe.get())) {
    return WithInputs(*probe, {ReplaceUnion(p->input, arm)});
  }
  return arm;
}

LogicalNodePtr JoinOver(const Candidate& c, const LogicalNodePtr& probe) {
  auto join = c.join;
  (join.build == BuildSide::kRight ? join.left : join.right) = probe;
  join.estimate.reset();
  join.needed_above.clear();
  return Make(std::move(join));
}

// SUM(CASE WHEN <build predicate> THEN <probe column> END) can consume a sum for the
// probe key. Retain the original join: duplicate build keys must repeat the partial,
// missing keys must discard it, and a NULL partial must stay NULL.
std::optional<std::size_t> UnionSlot(const Candidate& c, ColumnId id) {
  const auto column = ThroughProjects(c.probe.get(), id);
  return column ? Position(c.branches.ids, *column) : std::nullopt;
}

struct Measure {
  std::size_t slot;
  LogicalType sum_type;
  ColumnId partial;
};

LogicalNodePtr PreSum(const Candidate& c, const BindOptions& options, ColumnId& last) {
  if (c.compute == nullptr) {
    return nullptr;
  }
  const auto& pair = c.join.keys.front();
  const BoundColumn& key = c.join.build == BuildSide::kRight ? pair.left : pair.right;
  const auto key_slot = UnionSlot(c, key.id);
  if (!key_slot || key.type == LogicalType::kVarchar || key.type == LogicalType::kDouble) {
    return nullptr;
  }
  const auto build_ids = OutputIds(*c.build);
  std::vector<Measure> measures;
  auto compute = *c.compute;
  auto final = c.group;
  std::vector<bool> used(compute.exprs.size(), false);
  for (auto& call : final.aggregates) {
    if (call.kind != AggKind::kSum || !call.arg) {
      return nullptr;
    }
    const auto pos = Position(compute.ids, call.arg->id);
    if (!pos) {
      return nullptr;
    }
    const Expr& original = *c.compute->exprs[*pos];
    const auto* conditional = std::get_if<CaseExpr>(&original.node);
    if (conditional == nullptr || conditional->whens.size() != 1 || !conditional->whens[0] ||
        !conditional->thens[0] || conditional->otherwise ||
        conditional->thens[0]->type != original.type) {
      return nullptr;
    }
    const auto* column = std::get_if<ColumnExpr>(&conditional->thens[0]->node);
    const auto* pred = std::get_if<PredicateExpr>(&conditional->whens[0]->node);
    if (column == nullptr || pred == nullptr ||
        !std::ranges::all_of(pred->operands, [&](const ExprPtr& operand) {
          const auto* read = std::get_if<ColumnExpr>(&operand->node);
          return read != nullptr && std::ranges::contains(build_ids, read->id);
        })) {
      return nullptr;
    }
    const auto slot = UnionSlot(c, column->id);
    if (!slot) {
      return nullptr;
    }
    auto measure = std::ranges::find(measures, *slot, &Measure::slot);
    if (measure == measures.end()) {
      measures.push_back({.slot = *slot, .sum_type = call.type, .partial = Mint(last)});
      measure = std::prev(measures.end());
    }
    auto value = *conditional->thens[0];
    value.node = ColumnExpr{.id = measure->partial};
    value.type = measure->sum_type;
    auto widened = *conditional;
    widened.thens[0] = std::make_shared<const Expr>(std::move(value));
    compute.exprs[*pos] = std::make_shared<const Expr>(
        Expr{.node = std::move(widened), .type = call.type, .name = original.name});
    call.arg->type = call.type;
    used[*pos] = true;
  }
  if (std::ranges::find(used, false) != used.end()) {
    return nullptr;
  }

  UnionAllNode partials{.ids = {key.id}, .span = c.branches.span};
  for (const auto& measure : measures) {
    partials.ids.push_back(measure.partial);
  }
  Int128 output_groups = 0;
  for (std::size_t i = 0; i < c.arms.size(); ++i) {
    const auto& input = c.branches.inputs[i];
    const auto& columns = c.branches.columns[i];
    const auto& arm_key = columns[*key_slot];
    const auto domain = GroupDomain(input, arm_key.id);
    if (!domain || !FitsStates(options, *domain, measures.size())) {
      return nullptr;
    }
    output_groups += *domain;
    GroupAggregateNode partial{
        .input = input, .keys = {arm_key}, .key_ids = {Mint(last)}, .span = c.group.span};
    std::vector<BoundColumn> outputs{arm_key};
    outputs[0].id = partial.key_ids[0];
    for (const auto& measure : measures) {
      auto value = columns[measure.slot];
      // UnionAll's binder makes all casts explicit on the inputs. Require a direct scan
      // column of the exact measure type so no failing expression runs on unmatched rows.
      const auto id = Mint(last);
      partial.aggregates.push_back({.kind = AggKind::kSum,
                                    .arg = value,
                                    .type = measure.sum_type,
                                    .id = id,
                                    .span = c.group.span});
      value.id = id;
      value.type = measure.sum_type;
      outputs.push_back(std::move(value));
    }
    partials.columns.push_back(std::move(outputs));
    partials.inputs.push_back(Make(std::move(partial)));
  }
  // Reserve a substantial reduction for the extra scan-side hash aggregation. Every emitted
  // group is a row the join still has to probe, including groups from different arms.
  if (8 * output_groups > c.rows) {
    return nullptr;
  }
  compute.input = JoinOver(c, Make(std::move(partials)));
  final.input = Make(std::move(compute));
  return Make(std::move(final));
}

LogicalNodePtr Split(const Candidate& c, ColumnId& last) {
  auto final = c.group;
  UnionAllNode result{.span = c.branches.span};
  std::vector<BoundColumn> output = final.keys;
  for (auto& key : final.keys) {
    key.id = Mint(last);
    result.ids.push_back(key.id);
  }
  for (auto& call : final.aggregates) {
    BoundColumn state{
        .id = Mint(last), .name = call.arg ? call.arg->name : "count_star()", .type = call.type};
    output.push_back(state);
    result.ids.push_back(state.id);
    call.arg = state;
    if (call.kind == AggKind::kCount || call.kind == AggKind::kCountStar) {
      call.kind = AggKind::kSumCounts;
    }
  }
  for (std::size_t i = 0; i < c.arms.size(); ++i) {
    auto alias = Make(ProjectNode{.input = c.branches.inputs[i],
                                  .columns = c.branches.columns[i],
                                  .ids = c.branches.ids,
                                  .span = c.branches.span});
    auto child = JoinOver(c, ReplaceUnion(c.probe, alias));
    auto partial = c.group;
    partial.input = c.compute != nullptr ? WithInputs(*c.group.input, {child}) : child;
    auto cloned = CloneWithFreshIds(Make(std::move(partial)), last);
    const auto ids = OutputIds(*cloned);
    auto columns = output;
    for (std::size_t j = 0; j < ids.size(); ++j) {
      columns[j].id = ids[j];
    }
    result.columns.push_back(std::move(columns));
    result.inputs.push_back(std::move(cloned));
  }
  final.input = Make(std::move(result));
  return Make(std::move(final));
}

}  // namespace

LogicalNodePtr RewriteUnionAggregates(const LogicalNodePtr& root, const BindOptions& options,
                                      ColumnId& last_id,
                                      const std::set<const LogicalNode*>& preserve) {
  if (!root || preserve.contains(root.get())) {
    return root;
  }
  auto inputs = InputsOf(*root);
  bool changed = false;
  for (auto& input : inputs) {
    auto rewritten = RewriteUnionAggregates(input, options, last_id, preserve);
    changed |= rewritten != input;
    input = std::move(rewritten);
  }
  auto here = changed ? WithInputs(*root, std::move(inputs)) : root;
  if (const auto* group = std::get_if<GroupAggregateNode>(here.get())) {
    const auto c = Find(*group);
    if (c) {
      ColumnId tentative = last_id;
      if (auto partial = PreSum(*c, options, tentative)) {
        last_id = tentative;
        return partial;
      }
      if (WorthSplitting(*c, options)) {
        return Split(*c, last_id);
      }
    }
  }
  return here;
}

}  // namespace antb1::plan
