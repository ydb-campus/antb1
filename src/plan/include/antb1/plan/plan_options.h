#pragma once

#include <cstdint>

#include "antb1/plan/logical_plan.h"

namespace antb1::plan {

// What a caller may tell the planner about the plan it should choose, as opposed to what the query
// means. Both passes take it -- plan::Bind chooses the join order and the build sides of a block's
// joins, plan::Optimize chooses the build side of the join the unnesting pass creates -- so it
// lives here rather than in either header, which keeps the two passes siblings instead of making
// the optimizer depend on the binder's API. Every field has a default that holds for every query,
// so a caller with no opinion passes none and gets the plan the engine would choose for itself.
//
// Named for binding because that is where it started (ADR 0026's C1); it governs planning as a
// whole.
// How the join order of an inner block is chosen (docs/adr/0026-join-performance.md, C5).
enum class JoinOrderSearch : std::uint8_t {
  // Dynamic programming over connected subsets, minimising the rows the joins carry between them;
  // the greedy rule still orders a block of more relations than the search is bounded for.
  kCost,
  // ADR 0022's rule throughout: the largest relation probes, then the neighbour whose own join is
  // smallest. One join at a time, with no account of what the order costs as a whole.
  kGreedy,
};

struct BindOptions {
  // How much larger a join's build side must be than the side that probes it before the join builds
  // on the probe side instead (docs/adr/0026-join-performance.md), in hundredths of a factor: 400,
  // the default, flips only a decisive difference; 100 is ADR 0022's rule, the smaller side always.
  // Below 100 is invalid.
  int64_t build_side_ratio = kBuildSideRatio;
  // Which search chooses a block's join order. The greedy rule is what every plan used before C5;
  // it is kept so that a plan can be compared against it and so that a query whose order the
  // search gets wrong has somewhere to go.
  JoinOrderSearch join_order = JoinOrderSearch::kCost;
};

}  // namespace antb1::plan
