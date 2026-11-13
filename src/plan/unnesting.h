#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <vector>

#include <arrow/result.h>

#include "antb1/plan/logical_plan.h"

// The unnesting pass (docs/adr/0023-unnesting.md; ADR 0041's decisions 16 to 20): the dependent
// joins of a bound plan replaced by ordinary joins, outermost first. Private to the plan module:
// the optimizer and its tests include it.

namespace antb1::plan {

// A column's value when the static aggregation below it had no input row (ADR 0041's decision 19):
// a Constant, whose std::monostate is NULL as branch_truth.h spells it, or std::nullopt where the
// pass does not know it.
using EmptyValue = std::optional<Constant>;

// Decision 19's table, an exhaustive switch over AggKind: 0 for the COUNT kinds and a sum of
// counts, NULL for every other call, both of the call's type.
EmptyValue EmptyValueOf(const AggregateCall& call);

// Decision 20, the consumer proof: whether a dependent join that appends its right input's row may
// be joined back INNER, which drops an outer row the subquery has no row for. Only where every
// reader of the subquery's value rejects its empty value: from the dependent join's parent up
// through Computes to the first Filter that reads the value, one of whose conjuncts over it must
// be true for no row whose value is the empty value (TruthOver). Anything else -- a node that is
// not a Compute on the way, a Filter that reads no value, the root reached -- leaves it LEFT.
enum class JoinBack : std::uint8_t { kInner, kLeft };

// `above`: the nodes from the plan's root down to the dependent join's parent, root first.
// `values`: each column the dependent join appends, with its empty value.
JoinBack ProveJoinBack(std::span<const LogicalNode* const> above,
                       const std::map<ColumnId, EmptyValue>& values);

// The proof's verdict for each dependent join that appends its right input's row, in the order the
// pass met them: the join back each got (ADR 0041's SQ6).
struct UnnestReport {
  struct Entry {
    SourceSpan span;
    JoinBack join_back = JoinBack::kLeft;
  };
  std::vector<Entry> join_backs;
};

// The outermost dependent join of the plan, nullptr when it has none. Pre-order, so the first one
// found is the one whose span names the whole correlated subquery.
const DependentJoinNode* FirstDependentJoin(const LogicalNodePtr& node);

// `root` with every dependent join rewritten into ordinary joins, or the refusal of the first one
// the pass does not answer (exit code 4 at the subquery's span). `root` itself when it holds none.
arrow::Result<LogicalNodePtr> Unnest(const LogicalNodePtr& root, int64_t build_side_ratio,
                                     UnnestReport* report = nullptr);

}  // namespace antb1::plan
