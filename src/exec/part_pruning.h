#pragma once

#include <cstdint>
#include <vector>

#include "antb1/plan/logical_plan.h"
#include "antb1/plan/table.h"

// Skipping table parts by their statistics (plan::Table::part_stats): a part is skipped only when
// some predicate of the WHERE conjunction is false for every one of its rows, so the result never
// changes. Unknown statistics never skip.

namespace antb1::exec {

// Whether part `part` may hold a row that passes every predicate. `fields` maps a predicate's
// column index (a column of the scan's output) to the table field (plan::ScanNode::fields).
bool PartMayMatch(const plan::Table& table, int64_t part, const std::vector<int>& fields,
                  const std::vector<plan::Predicate>& predicates);

// The parts that may match, in part order.
std::vector<int64_t> KeptParts(const plan::Table& table, const std::vector<int>& fields,
                               const std::vector<plan::Predicate>& predicates);

}  // namespace antb1::exec
