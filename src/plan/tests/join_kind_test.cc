#include "antb1/plan/join_kind.h"

#include <array>
#include <cstdint>
#include <limits>

#include <gtest/gtest.h>

namespace antb1::plan {
namespace {

constexpr std::array<JoinKind, 8> kKinds = {
    JoinKind::kInner,         JoinKind::kLeft,   JoinKind::kSemi, JoinKind::kAnti,
    JoinKind::kNullAwareAnti, JoinKind::kOneRow, JoinKind::kFull, JoinKind::kNullAwareMark};

// Each kind's row of the table (ADR 0037's OJ1), as its definition in join_kind.h says: what the
// rules that read it rely on, pinned so that a change to a row is a change to this test too.
TEST(JoinKindTest, EachKindHasItsRow) {
  struct Row {
    JoinKind kind;
    JoinKindTraits traits;
  };
  for (const Row& row : {
           Row{.kind = JoinKind::kInner,
               .traits = {.outputs_right = true,
                          .drops_unmatched_left = true,
                          .nested_loop = true,
                          .residuals = true,
                          .may_build_left = true}},
           Row{.kind = JoinKind::kLeft,
               .traits = {.outputs_right = true,
                          .pads_right = true,
                          .nested_loop = true,
                          .residuals = true}},
           Row{.kind = JoinKind::kSemi,
               .traits = {.filters_left = true,
                          .drops_unmatched_left = true,
                          .residuals = true,
                          .may_build_left = true}},
           Row{.kind = JoinKind::kAnti,
               .traits = {.filters_left = true, .residuals = true, .may_build_left = true}},
           Row{.kind = JoinKind::kNullAwareAnti,
               .traits = {.filters_left = true, .null_aware = true}},
           Row{.kind = JoinKind::kOneRow, .traits = {.outputs_right = true, .one_row = true}},
           Row{.kind = JoinKind::kFull,
               .traits = {.outputs_right = true,
                          .pads_right = true,
                          .pads_left = true,
                          .nested_loop = true,
                          .residuals = true}},
           Row{.kind = JoinKind::kNullAwareMark,
               .traits = {.null_aware = true, .appends_mark = true}},
       }) {
    SCOPED_TRACE(ToString(row.kind));
    const JoinKindTraits traits = Traits(row.kind);
    EXPECT_EQ(traits.outputs_right, row.traits.outputs_right);
    EXPECT_EQ(traits.filters_left, row.traits.filters_left);
    EXPECT_EQ(traits.pads_right, row.traits.pads_right);
    EXPECT_EQ(traits.pads_left, row.traits.pads_left);
    EXPECT_EQ(traits.drops_unmatched_left, row.traits.drops_unmatched_left);
    EXPECT_EQ(traits.one_row, row.traits.one_row);
    EXPECT_EQ(traits.nested_loop, row.traits.nested_loop);
    EXPECT_EQ(traits.null_aware, row.traits.null_aware);
    EXPECT_EQ(traits.residuals, row.traits.residuals);
    EXPECT_EQ(traits.may_build_left, row.traits.may_build_left);
    EXPECT_EQ(traits.appends_mark, row.traits.appends_mark);
  }
}

// The properties are about rows and columns, so some pairs exclude each other whatever a new kind
// adds: a filter of the left input outputs no right column and pads nothing; a padded row carries
// both inputs' columns; a one-row join has a single right row and nothing to refine; a nested loop
// (ADR 0040) outputs both inputs and has residuals to decide its matches, all it has without keys;
// NOT IN is a filter or a mark; a join that drops its unmatched left rows does not pad them; a mark
// join outputs each left row once and no right column, and builds on its right input only.
TEST(JoinKindTest, EveryRowIsConsistent) {
  for (const JoinKind kind : kKinds) {
    SCOPED_TRACE(ToString(kind));
    const JoinKindTraits traits = Traits(kind);
    if (traits.filters_left) {
      EXPECT_FALSE(traits.outputs_right);
      EXPECT_FALSE(traits.pads_left);
      EXPECT_FALSE(traits.pads_right);
    }
    if (traits.pads_left || traits.pads_right) {
      EXPECT_TRUE(traits.outputs_right);
    }
    if (traits.one_row) {
      EXPECT_FALSE(traits.nested_loop);
      EXPECT_FALSE(traits.residuals);
      EXPECT_FALSE(traits.may_build_left);
      EXPECT_FALSE(traits.null_aware);
    }
    if (traits.nested_loop) {
      EXPECT_TRUE(traits.outputs_right);
      EXPECT_TRUE(traits.residuals);
      EXPECT_FALSE(traits.null_aware);
    }
    if (traits.null_aware) {
      EXPECT_TRUE(traits.filters_left || traits.appends_mark);
      EXPECT_FALSE(traits.residuals);
    }
    if (traits.appends_mark) {
      EXPECT_FALSE(traits.outputs_right);
      EXPECT_FALSE(traits.filters_left);
      EXPECT_FALSE(traits.pads_left);
      EXPECT_FALSE(traits.pads_right);
      EXPECT_FALSE(traits.drops_unmatched_left);
      EXPECT_FALSE(traits.one_row);
      EXPECT_FALSE(traits.may_build_left);
    }
    if (traits.drops_unmatched_left) {
      EXPECT_FALSE(traits.pads_right);
    }
  }
}

// The rules derived from the table give what the code gave before it (ADR 0037's OJ1 changed no
// behavior): a left build emits its rows for every kind but inner (a one-row join never builds on
// its left), the unmatched ones for left, anti and null-aware anti; no right build emits its rows;
// an empty build empties inner and semi joins; a probe drops its unmatched rows -- the gate of ADR
// 0029's dynamic filters -- for inner and semi joins built on the right, and for every keyed kind
// but null-aware anti built on the left; only inner outputs its matches alone. A mark join (ADR
// 0041's decision 13) emits no build row, is never emptied and takes no dynamic filter on either
// side.
TEST(JoinKindTest, DerivedRulesKeepTheirAnswers) {
  for (const JoinKind kind : kKinds) {
    SCOPED_TRACE(ToString(kind));
    const bool inner = kind == JoinKind::kInner;
    const bool semi = kind == JoinKind::kSemi;
    const bool keyless = kind == JoinKind::kOneRow;
    const bool null_aware = kind == JoinKind::kNullAwareAnti;
    const bool full = kind == JoinKind::kFull;
    const bool mark = kind == JoinKind::kNullAwareMark;
    EXPECT_EQ(EmitsBuildRows(kind, BuildSide::kLeft), !inner && !keyless && !mark);
    EXPECT_EQ(EmitsBuildRows(kind, BuildSide::kRight), full) << "only FULL pads a right build";
    EXPECT_EQ(EmitsUnmatchedBuildRows(kind, BuildSide::kLeft),
              kind == JoinKind::kLeft || kind == JoinKind::kAnti || null_aware || full);
    EXPECT_EQ(EmitsUnmatchedBuildRows(kind, BuildSide::kRight), full);
    EXPECT_EQ(EmitsProbeAndBuildRows(kind, BuildSide::kRight), full);
    EXPECT_EQ(EmptyBuildEmptiesJoin(kind), inner || semi);
    EXPECT_EQ(ProbeDropsUnmatchedRows(kind, BuildSide::kRight), inner || semi);
    EXPECT_EQ(ProbeDropsUnmatchedRows(kind, BuildSide::kLeft),
              !keyless && !null_aware && !full && !mark);
    EXPECT_EQ(OutputsMatchesOnly(kind), inner);
  }
}

// A padded input's rows each come out: a left join outputs at least its left input's rows, and a
// FULL join at most every match and every row of both inputs, saturated (ADR 0037's OJ2).
TEST(JoinKindTest, PaddedJoinRowsBoundTheOutput) {
  EXPECT_EQ(PaddedJoinRows(JoinKind::kInner, 5, 100, 7), 5);
  EXPECT_EQ(PaddedJoinRows(JoinKind::kLeft, 5, 100, 7), 100);
  EXPECT_EQ(PaddedJoinRows(JoinKind::kLeft, 500, 100, 7), 500);
  EXPECT_EQ(PaddedJoinRows(JoinKind::kFull, 5, 100, 7), 112);
  constexpr int64_t kMax = std::numeric_limits<int64_t>::max();
  EXPECT_EQ(PaddedJoinRows(JoinKind::kFull, kMax - 1, 1, 1), kMax);
  // A mark join outputs each left row exactly once, whatever matches (ADR 0041's decision 13).
  EXPECT_EQ(PaddedJoinRows(JoinKind::kNullAwareMark, 5, 100, 7), 100);
  EXPECT_EQ(PaddedJoinRows(JoinKind::kNullAwareMark, 500, 100, 7), 100);
}

// Traits is constexpr, so a rule may read it where only a constant will do.
static_assert(Traits(JoinKind::kLeft).pads_right);
static_assert(!Traits(JoinKind::kSemi).outputs_right);

}  // namespace
}  // namespace antb1::plan
