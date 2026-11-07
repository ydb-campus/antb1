#pragma once

#include <cstdint>
#include <string_view>

namespace antb1::plan {

// The kinds of joins (ADR 0022). A match is a pair of a left and a right row with equal keys whose
// residual is true; a NULL key never matches.
enum class JoinKind : std::uint8_t {
  kInner,          // every match
  kLeft,           // every match, and each left row without one, padded with NULLs
  kSemi,           // each left row with a match, once
  kAnti,           // each left row without a match
  kNullAwareAnti,  // as SQL's NOT IN: no left row when the right has a NULL key; every left row
                   // when it is empty; else each left row with a non-NULL key and no match
  kOneRow,         // each left row with the right input's single row (no keys)
};

// "INNER", "LEFT", "SEMI", "ANTI", "NULL-AWARE ANTI", "ONE-ROW".
std::string_view ToString(JoinKind kind);

// The input a join builds its hash table on; the other is probed.
enum class BuildSide : std::uint8_t { kLeft, kRight };

// "left", "right".
std::string_view ToString(BuildSide side);

// What a join kind does with its rows (ADR 0037's OJ1): the one table every rule that tells kinds
// apart reads, in the binder, the optimizer, the physical planner and the operator, so that a new
// kind is one more row here -- which the compiler demands (Traits) -- rather than an audit of
// scattered conditions. Each property is about rows and columns, never about a rule that uses it.
struct JoinKindTraits {
  // The right input's columns follow the left input's in the output; otherwise the output is the
  // left input's columns only.
  bool outputs_right = false;
  // The output is a subset of the left input's rows, each at most once, with its columns only.
  bool filters_left = false;
  // A left row without a match comes out once, its right columns NULL.
  bool pads_right = false;
  // A right row without a match comes out once, its left columns NULL.
  bool pads_left = false;
  // A left row without a match never comes out.
  bool drops_unmatched_left = false;
  // The join has no keys, and its right input exactly one row; otherwise it has at least one key.
  bool keyless = false;
  // SQL's NOT IN: exactly one key, and a NULL key on either side decides the answer.
  bool null_aware = false;
  // Residual conditions over both inputs may refine its matches.
  bool residuals = false;
  // The executor can build on the left input (ADR 0026's C2b and C2c) as well as the right one.
  bool may_build_left = false;
};

// The row of `kind` in the table above. An exhaustive switch: a new kind does not compile without
// its row.
constexpr JoinKindTraits Traits(JoinKind kind) {
  switch (kind) {
    case JoinKind::kInner:
      return {.outputs_right = true,
              .drops_unmatched_left = true,
              .residuals = true,
              .may_build_left = true};
    case JoinKind::kLeft:
      return {.outputs_right = true, .pads_right = true, .residuals = true};
    case JoinKind::kSemi:
      return {.filters_left = true,
              .drops_unmatched_left = true,
              .residuals = true,
              .may_build_left = true};
    case JoinKind::kAnti:
      return {.filters_left = true, .residuals = true, .may_build_left = true};
    case JoinKind::kNullAwareAnti:
      return {.filters_left = true, .null_aware = true};
    case JoinKind::kOneRow:
      return {.outputs_right = true, .keyless = true};
  }
  return {};
}

// Whether a join of `kind` outputs its matches and nothing else: both inputs' columns, no padded
// row and no row without a match (an inner join) -- the kind across which a rule may treat the join
// as a plain product of its inputs filtered by its keys and residuals.
constexpr bool OutputsMatchesOnly(JoinKind kind) {
  const JoinKindTraits traits = Traits(kind);
  return traits.outputs_right && !traits.pads_left && !traits.pads_right && !traits.keyless;
}

// Whether a join of `kind` building on `side` outputs its build's rows rather than producing its
// output from the probe alone (docs/adr/0026-join-performance.md, C2b and C2c): it must remember
// which build rows matched and emit them -- or those that did not -- after the probe, so it is the
// sink of its probe pipeline. A left build does when the left rows decide the output (a filter of
// them, or a join that pads their unmatched rows), a right build when its unmatched rows are
// padded. One function so that the planner, the build and the probe cannot disagree about the
// direction.
constexpr bool EmitsBuildRows(JoinKind kind, BuildSide side) {
  const JoinKindTraits traits = Traits(kind);
  return side == BuildSide::kLeft ? traits.filters_left || traits.pads_right : traits.pads_left;
}

// Whether such a join emits the build rows that did *not* match, rather than only those that did:
// a left build that pads its unmatched rows or keeps them (an anti join), a right build that pads
// them. Those need the build to retain the rows whose keys are NULL
// (JoinBuildSpec::keep_null_keys), since a NULL key never matches. One function for the same reason
// as EmitsBuildRows: the planner sets the flag from it, the probe refuses a build without one, and
// the sink reads the marks by it.
constexpr bool EmitsUnmatchedBuildRows(JoinKind kind, BuildSide side) {
  const JoinKindTraits traits = Traits(kind);
  if (side == BuildSide::kLeft) {
    return traits.pads_right || (traits.filters_left && !traits.drops_unmatched_left);
  }
  return traits.pads_left;
}

// Whether a join of `kind` whose build input holds no row outputs no row, whatever the other input
// holds: every output row needs a matched left row and no right row is padded. True for inner and
// semi joins; an anti join over no row keeps every left row, and a padding join pads.
constexpr bool EmptyBuildEmptiesJoin(JoinKind kind) {
  const JoinKindTraits traits = Traits(kind);
  return traits.drops_unmatched_left && !traits.pads_left;
}

// Whether no probe row without a match comes out of a join of `kind` building on `build` -- the
// condition for filtering the probe side by the build's keys before the join (ADR 0029's dynamic
// filters, D1, D2 and D5): a left probe (a right build) whose kind drops its unmatched rows, or a
// right probe (a left build) whose unmatched rows are not padded, since a right row comes out of
// every kind only as part of a match or not at all. A null-aware or keyless join never takes such a
// filter: its keys do not decide alone.
constexpr bool ProbeDropsUnmatchedRows(JoinKind kind, BuildSide build) {
  const JoinKindTraits traits = Traits(kind);
  if (traits.null_aware || traits.keyless) {
    return false;
  }
  return build == BuildSide::kRight ? traits.drops_unmatched_left : !traits.pads_left;
}

}  // namespace antb1::plan
