#include "grouping_sets_shape.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <format>
#include <map>
#include <memory>
#include <numeric>
#include <optional>
#include <span>
#include <utility>
#include <variant>
#include <vector>

#include <arrow/api.h>

#include "antb1/common/check.h"
#include "antb1/common/int128.h"
#include "antb1/common/narrow.h"
#include "antb1/plan/logical_plan.h"
#include "antb1/plan/types.h"

namespace antb1::exec {
namespace {

// A producer can be reached through several CASE arms, projections or aggregates.
class DecimalProofs {
 public:
  bool Column(const plan::LogicalNode& node, plan::ColumnId id) {
    const auto key = std::pair{&node, id};
    if (const auto found = checked_.find(key); found != checked_.end()) {
      return found->second;
    }
    return checked_.emplace(key, Trace(node, id)).first->second;
  }

 private:
  bool Trace(const plan::LogicalNode& node, plan::ColumnId id);
  bool Expr(const plan::LogicalNode& input, const plan::Expr& expr);

  std::map<std::pair<const plan::LogicalNode*, plan::ColumnId>, bool> checked_;
};

bool DecimalProofs::Expr(const plan::LogicalNode& input, const plan::Expr& expr) {
  if (const auto* col = std::get_if<plan::ColumnExpr>(&expr.node)) {
    return Column(input, col->id);
  }
  if (const auto* cast = std::get_if<plan::CastExpr>(&expr.node)) {
    // Convert range-checks a conversion to a different DECIMAL type. A same-type cast passes
    // through the original array and inherits its proof.
    return cast->operand && (cast->operand->type != expr.type || Expr(input, *cast->operand));
  }
  if (const auto* branch = std::get_if<plan::CaseExpr>(&expr.node)) {
    const auto checked = [&](const plan::ExprPtr& value) {
      return !value || value->type != expr.type || Expr(input, *value);
    };
    return checked(branch->otherwise) && std::ranges::all_of(branch->thens, checked);
  }
  // DecimalArith and its compiled equivalent check the result against the declared width.
  return std::holds_alternative<plan::ArithExpr>(expr.node) ||
         std::holds_alternative<plan::ConstantExpr>(expr.node);
}

bool DecimalProofs::Trace(const plan::LogicalNode& node, plan::ColumnId id) {
  if (id == plan::kNoColumnId) {
    return false;
  }
  if (const auto* compute = std::get_if<plan::ComputeNode>(&node)) {
    const auto it = std::ranges::find(compute->ids, id);
    return it == compute->ids.end()
               ? Column(*compute->input, id)
               : Expr(*compute->input,
                      *compute->exprs[Narrow<std::size_t>(it - compute->ids.begin())]);
  }
  if (const auto* project = std::get_if<plan::ProjectNode>(&node)) {
    const auto it = std::ranges::find(project->ids, id);
    if (it == project->ids.end()) {
      return false;
    }
    const auto index = Narrow<std::size_t>(it - project->ids.begin());
    return (!project->constants.empty() && project->constants[index].has_value()) ||
           Column(*project->input, project->columns[index].id);
  }
  if (const auto* filter = std::get_if<plan::FilterNode>(&node)) {
    return Column(*filter->input, id);
  }
  if (const auto* join = std::get_if<plan::JoinNode>(&node)) {
    return Column(*join->left, id) || Column(*join->right, id);
  }
  return false;
}

bool MergesExactly(const plan::AggregateCall& call, std::optional<int64_t> max_rows) {
  const std::optional<plan::LogicalType> input =
      call.arg.has_value() ? std::optional(call.arg->type) : std::nullopt;
  const auto exact_sum = [max_rows](plan::LogicalType type) {
    if (type != plan::LogicalType::kDecimal) {
      return plan::IsInteger(type) && type != plan::LogicalType::kHugeInt;
    }
    if (type.width() <= plan::LogicalType::kMaxNarrowDecimalWidth) {
      return true;
    }
    // The absolute sum bounds every intermediate in any merge order, including cancellation.
    // Divide first: multiplying a wide unscaled value by the row count can itself overflow.
    return max_rows.has_value() && *max_rows >= 0 &&
           *max_rows <= (PowerOfTen(plan::LogicalType::kMaxDecimalWidth) - 1) /
                            (PowerOfTen(type.width()) - 1);
  };
  switch (call.kind) {
    case plan::AggKind::kCountStar:
    case plan::AggKind::kCount:
    case plan::AggKind::kSumCounts:
      return true;
    case plan::AggKind::kSum:
      return input.has_value() && exact_sum(*input);
    case plan::AggKind::kAvg:
      return input.has_value() && (exact_sum(*input) || *input == plan::LogicalType::kDate ||
                                   *input == plan::LogicalType::kTimestamp);
    case plan::AggKind::kMin:
    case plan::AggKind::kMax:
      return input.has_value() && *input != plan::LogicalType::kDouble;
    case plan::AggKind::kCountDistinct:
    case plan::AggKind::kStddevSamp:
    case plan::AggKind::kStddevPop:
    case plan::AggKind::kVarSamp:
    case plan::AggKind::kVarPop:
    case plan::AggKind::kSingleValue:  // never grouped: it has no grouped state to merge
      return false;
  }
  return false;
}

// Whether ascending `inner` is a subset of ascending `outer`.
bool Contains(const std::vector<std::size_t>& outer, const std::vector<std::size_t>& inner) {
  return std::ranges::includes(outer, inner);
}

std::vector<std::size_t> KeyPositions(const std::vector<std::size_t>& parent,
                                      const std::vector<std::size_t>& child) {
  std::vector<std::size_t> columns;
  columns.reserve(child.size());
  for (const auto key : child) {
    columns.push_back(Narrow<std::size_t>(std::ranges::lower_bound(parent, key) - parent.begin()));
  }
  return columns;
}

std::vector<GroupingSetsShape::RollupLevel> NestedRollup(
    const std::vector<std::vector<std::size_t>>& sets) {
  if (sets.size() < 3) {
    return {};
  }
  std::vector<std::size_t> order(sets.size());
  std::ranges::iota(order, 0U);
  std::ranges::sort(order, [&](auto a, auto b) { return sets[a].size() > sets[b].size(); });
  std::vector<GroupingSetsShape::RollupLevel> levels;
  levels.reserve(order.size() - 1);
  for (std::size_t i = 1; i < order.size(); ++i) {
    const auto parent = order[i - 1];
    const auto set = order[i];
    if (!Contains(sets[parent], sets[set])) {
      return {};
    }
    levels.push_back(
        {.set = set, .parent = parent, .columns = KeyPositions(sets[parent], sets[set])});
  }
  return levels;
}

}  // namespace

bool WideDecimalInputsChecked(const plan::GroupingSetsNode& node) {
  DecimalProofs proofs;
  return std::ranges::all_of(node.aggregates, [&](const plan::AggregateCall& call) {
    if ((call.kind != plan::AggKind::kSum && call.kind != plan::AggKind::kAvg) ||
        !call.arg.has_value() || call.arg->type != plan::LogicalType::kDecimal ||
        call.arg->type.width() <= plan::LogicalType::kMaxNarrowDecimalWidth) {
      return true;
    }
    return proofs.Column(*node.input, call.arg->id);
  });
}

bool MergesExactly(const std::vector<plan::BoundColumn>& keys,
                   const std::vector<plan::AggregateCall>& calls, std::optional<int64_t> max_rows) {
  return std::ranges::none_of(
             keys,
             [](const plan::BoundColumn& key) { return key.type == plan::LogicalType::kDouble; }) &&
         std::ranges::all_of(calls, [max_rows](const plan::AggregateCall& call) {
           return MergesExactly(call, max_rows);
         });
}

GroupingSetsShape PlanGroupingSets(const std::vector<std::vector<std::size_t>>& sets, bool exact) {
  GroupingSetsShape shape;
  shape.entries.reserve(sets.size());
  for (const std::vector<std::size_t>& set : sets) {
    const auto seen = std::ranges::find(shape.sets, set);
    shape.entries.push_back(Narrow<std::size_t>(seen - shape.sets.begin()));
    if (seen == shape.sets.end()) {
      shape.sets.push_back(set);
    }
  }
  const std::size_t n = shape.sets.size();
  std::vector<bool> roots(n, true);
  if (exact) {
    for (std::size_t s = 0; s < n; ++s) {
      roots[s] = std::ranges::none_of(shape.sets, [&](const std::vector<std::size_t>& other) {
        return other.size() > shape.sets[s].size() && Contains(other, shape.sets[s]);
      });
    }
  }
  shape.sources.assign(n, 0);
  shape.columns.assign(n, {});
  for (std::size_t s = 0; s < n; ++s) {
    if (roots[s]) {
      shape.sources[s] = s;
      shape.columns[s].resize(shape.sets[s].size());
      for (std::size_t i = 0; i < shape.columns[s].size(); ++i) {
        shape.columns[s][i] = i;
      }
      continue;
    }
    std::optional<std::size_t> source;
    for (std::size_t r = 0; r < n; ++r) {
      if (roots[r] && Contains(shape.sets[r], shape.sets[s]) &&
          (!source.has_value() || shape.sets[r].size() < shape.sets[*source].size())) {
        source = r;
      }
    }
    ANTB1_CHECK(source.has_value());  // a non-root set lies in a larger set, so in some root
    shape.sources[s] = *source;
    shape.columns[s] = KeyPositions(shape.sets[*source], shape.sets[s]);
  }
  if (exact) {
    shape.rollup = NestedRollup(shape.sets);
  }
  shape.last_entries.assign(n, 0);
  for (std::size_t e = 0; e < shape.entries.size(); ++e) {
    shape.last_entries[shape.entries[e]] = e;
  }
  return shape;
}

std::vector<std::int64_t> GroupingValues(const std::vector<std::size_t>& set,
                                         const std::vector<plan::GroupingCall>& calls) {
  std::vector<std::int64_t> values;
  values.reserve(calls.size());
  for (const plan::GroupingCall& call : calls) {
    ANTB1_CHECK(call.args.size() < 64);
    std::uint64_t bits = 0;
    for (std::size_t i = 0; i < call.args.size(); ++i) {
      if (!std::ranges::binary_search(set, call.args[i])) {
        bits |= std::uint64_t{1} << (call.args.size() - 1 - i);
      }
    }
    values.push_back(Narrow<std::int64_t>(bits));
  }
  return values;
}

std::shared_ptr<arrow::Schema> GroupingSetsSchema(const std::vector<plan::BoundColumn>& keys,
                                                  const std::vector<plan::AggregateCall>& calls,
                                                  std::size_t grouping_calls) {
  arrow::FieldVector fields;
  fields.reserve(keys.size() + calls.size() + grouping_calls);
  for (std::size_t i = 0; i < keys.size(); ++i) {
    fields.push_back(arrow::field(std::format("key{}", i), plan::ToArrow(keys[i].type)));
  }
  for (std::size_t i = 0; i < calls.size(); ++i) {
    fields.push_back(arrow::field(std::format("agg{}", i), plan::ToArrow(calls[i].type)));
  }
  for (std::size_t i = 0; i < grouping_calls; ++i) {
    fields.push_back(arrow::field(std::format("grouping{}", i), arrow::int64()));
  }
  return arrow::schema(std::move(fields));
}

std::vector<plan::BoundColumn> SetKeys(const std::vector<plan::BoundColumn>& keys,
                                       const std::vector<std::size_t>& set) {
  std::vector<plan::BoundColumn> out;
  out.reserve(set.size());
  for (const std::size_t k : set) {
    out.push_back(keys.at(k));
  }
  return out;
}

std::shared_ptr<arrow::Schema> SetSchema(const std::vector<plan::BoundColumn>& keys,
                                         const std::vector<std::size_t>& set,
                                         const std::vector<plan::AggregateCall>& calls) {
  return GroupingSetsSchema(SetKeys(keys, set), calls, 0);
}

arrow::Result<std::shared_ptr<arrow::RecordBatch>> SetRows(
    const arrow::RecordBatch& rows, const std::vector<std::size_t>& set,
    std::span<const std::int64_t> grouping, const std::shared_ptr<arrow::Schema>& schema,
    arrow::MemoryPool* pool) {
  const std::int64_t n = rows.num_rows();
  const std::size_t keys = Narrow<std::size_t>(schema->num_fields()) -
                           (Narrow<std::size_t>(rows.num_columns()) - set.size()) - grouping.size();
  std::vector<std::shared_ptr<arrow::Array>> columns;
  columns.reserve(Narrow<std::size_t>(schema->num_fields()));
  std::size_t next = 0;  // the set's next key in `rows`
  for (std::size_t k = 0; k < keys; ++k) {
    if (next < set.size() && set[next] == k) {
      columns.push_back(rows.column(Narrow<int>(next++)));
    } else {
      ARROW_ASSIGN_OR_RAISE(auto nulls,
                            arrow::MakeArrayOfNull(schema->field(Narrow<int>(k))->type(), n, pool));
      columns.push_back(std::move(nulls));
    }
  }
  for (int c = Narrow<int>(set.size()); c < rows.num_columns(); ++c) {
    columns.push_back(rows.column(c));
  }
  for (const std::int64_t value : grouping) {
    ARROW_ASSIGN_OR_RAISE(auto constant,
                          arrow::MakeArrayFromScalar(arrow::Int64Scalar(value), n, pool));
    columns.push_back(std::move(constant));
  }
  return arrow::RecordBatch::Make(schema, n, std::move(columns));
}

}  // namespace antb1::exec
