#include "antb1/plan/join_kind.h"

#include <array>

#include <gtest/gtest.h>

namespace antb1::plan {
namespace {

constexpr std::array<JoinKind, 6> kKinds = {JoinKind::kInner,         JoinKind::kLeft,
                                            JoinKind::kSemi,          JoinKind::kAnti,
                                            JoinKind::kNullAwareAnti, JoinKind::kOneRow};

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
                          .residuals = true,
                          .may_build_left = true}},
           Row{.kind = JoinKind::kLeft,
               .traits = {.outputs_right = true, .pads_right = true, .residuals = true}},
           Row{.kind = JoinKind::kSemi,
               .traits = {.filters_left = true,
                          .drops_unmatched_left = true,
                          .residuals = true,
                          .may_build_left = true}},
           Row{.kind = JoinKind::kAnti,
               .traits = {.filters_left = true, .residuals = true, .may_build_left = true}},
           Row{.kind = JoinKind::kNullAwareAnti,
               .traits = {.filters_left = true, .null_aware = true}},
           Row{.kind = JoinKind::kOneRow, .traits = {.outputs_right = true, .keyless = true}},
       }) {
    SCOPED_TRACE(ToString(row.kind));
    const JoinKindTraits traits = Traits(row.kind);
    EXPECT_EQ(traits.outputs_right, row.traits.outputs_right);
    EXPECT_EQ(traits.filters_left, row.traits.filters_left);
    EXPECT_EQ(traits.pads_right, row.traits.pads_right);
    EXPECT_EQ(traits.pads_left, row.traits.pads_left);
    EXPECT_EQ(traits.drops_unmatched_left, row.traits.drops_unmatched_left);
    EXPECT_EQ(traits.keyless, row.traits.keyless);
    EXPECT_EQ(traits.null_aware, row.traits.null_aware);
    EXPECT_EQ(traits.residuals, row.traits.residuals);
    EXPECT_EQ(traits.may_build_left, row.traits.may_build_left);
  }
}

// The properties are about rows and columns, so some pairs exclude each other whatever a new kind
// adds: a filter of the left input outputs no right column and pads nothing; a padded row carries
// both inputs' columns; a keyless join has a single right row and nothing to refine; NOT IN is a
// filter; a join that drops its unmatched left rows does not pad them.
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
    if (traits.keyless) {
      EXPECT_FALSE(traits.residuals);
      EXPECT_FALSE(traits.may_build_left);
      EXPECT_FALSE(traits.null_aware);
    }
    if (traits.null_aware) {
      EXPECT_TRUE(traits.filters_left);
      EXPECT_FALSE(traits.residuals);
    }
    if (traits.drops_unmatched_left) {
      EXPECT_FALSE(traits.pads_right);
    }
  }
}

// The rules derived from the table give what the code gave before it (ADR 0037's OJ1 changed no
// behavior): a left build emits its rows for every kind but inner (a keyless join never builds on
// its left), the unmatched ones for left, anti and null-aware anti; no right build emits its rows;
// an empty build empties inner and semi joins; a probe drops its unmatched rows -- the gate of ADR
// 0029's dynamic filters -- for inner and semi joins built on the right, and for every keyed kind
// but null-aware anti built on the left; only inner outputs its matches alone.
TEST(JoinKindTest, DerivedRulesKeepTheirAnswers) {
  for (const JoinKind kind : kKinds) {
    SCOPED_TRACE(ToString(kind));
    const bool inner = kind == JoinKind::kInner;
    const bool semi = kind == JoinKind::kSemi;
    const bool keyless = kind == JoinKind::kOneRow;
    const bool null_aware = kind == JoinKind::kNullAwareAnti;
    EXPECT_EQ(EmitsBuildRows(kind, BuildSide::kLeft), !inner && !keyless);
    EXPECT_FALSE(EmitsBuildRows(kind, BuildSide::kRight));
    EXPECT_EQ(EmitsUnmatchedBuildRows(kind, BuildSide::kLeft),
              kind == JoinKind::kLeft || kind == JoinKind::kAnti || null_aware);
    EXPECT_FALSE(EmitsUnmatchedBuildRows(kind, BuildSide::kRight));
    EXPECT_EQ(EmptyBuildEmptiesJoin(kind), inner || semi);
    EXPECT_EQ(ProbeDropsUnmatchedRows(kind, BuildSide::kRight), inner || semi);
    EXPECT_EQ(ProbeDropsUnmatchedRows(kind, BuildSide::kLeft), !keyless && !null_aware);
    EXPECT_EQ(OutputsMatchesOnly(kind), inner);
  }
}

// Traits is constexpr, so a rule may read it where only a constant will do.
static_assert(Traits(JoinKind::kLeft).pads_right);
static_assert(!Traits(JoinKind::kSemi).outputs_right);

}  // namespace
}  // namespace antb1::plan
