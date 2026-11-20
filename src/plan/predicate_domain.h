#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

#include "antb1/plan/logical_plan.h"

namespace antb1::plan {

struct PredicateDomainKey {
  ColumnId id = kNoColumnId;
  // Upper bound on all values reaching this predicate, counting NULL as one grouping value.
  // Unknown is fine when the predicate itself bounds the key in each branch.
  std::optional<int64_t> max_values;
};

inline constexpr std::size_t kMaxPredicateDomainKeys = 8;
inline constexpr int kPredicateDomainNodeBudget = 64;

// Bounds distinct key tuples surviving the filter's conjuncts, including Boolean expressions
// supplied by its immediate Compute input. Returns a bound only when tighter than the product
// of the supplied domains. A caller can share the work budget across several filters. Exhaustion
// leaves the estimate unchanged.
std::optional<int64_t> PredicateTupleBound(const FilterNode& filter,
                                           std::span<const PredicateDomainKey> keys,
                                           int& remaining_nodes);

}  // namespace antb1::plan
