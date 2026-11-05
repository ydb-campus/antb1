#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include <arrow/memory_pool.h>
#include <arrow/record_batch.h>
#include <arrow/result.h>
#include <arrow/type_fwd.h>

#include "antb1/plan/logical_plan.h"

// How the grouping-sets operators answer a GroupingSetsNode (docs/adr/0038-grouping-sets.md): which
// sets are aggregated from the input, which are merged from another set's groups, and how a set's
// groups become the node's rows.

namespace antb1::exec {

// Whether every set of an aggregation by `keys` with `calls` may be merged from the groups of a set
// that contains it and still equal the GROUP BY of that set exactly, whatever the order of the
// merge: no DOUBLE key (a merge keeps the normalized value, not the one first seen), and only calls
// whose partial states add exactly -- COUNT(*), COUNT(x), sums of counts; SUM and AVG of an integer
// but HUGEINT and of a DECIMAL of at most 18 digits (a 128-bit sum that never overflows); AVG of a
// DATE or a TIMESTAMP; MIN and MAX of anything but DOUBLE (whose -0.0 and NaN keep the value seen
// first). COUNT(DISTINCT), a DOUBLE sum, the variance family and a wider sum do not.
bool MergesExactly(const std::vector<plan::BoundColumn>& keys,
                   const std::vector<plan::AggregateCall>& calls);

// The work of a list of grouping sets (indices into the node's keys, each ascending).
struct GroupingSetsShape {
  // The distinct sets, in the order they first appear in the list.
  std::vector<std::vector<std::size_t>> sets;
  // Per entry of the list: its distinct set.
  std::vector<std::size_t> entries;
  // Per distinct set: the distinct set it is merged from, or itself for a set aggregated from the
  // input (a root).
  std::vector<std::size_t> sources;
  // Per distinct set: its keys' positions among its source's keys (0, 1, ... for a root).
  std::vector<std::vector<std::size_t>> columns;
  // Per distinct set: the entries of the list after which nothing reads its rows any more.
  std::vector<std::size_t> last_entries;

  [[nodiscard]] bool root(std::size_t set) const { return sources.at(set) == set; }
};

// With `exact` (MergesExactly), the roots are the distinct sets no other distinct set contains and
// every other set is merged from the smallest root that contains it (the first of them on a tie);
// without, every distinct set is a root, aggregated from the input in a table of its own.
GroupingSetsShape PlanGroupingSets(const std::vector<std::vector<std::size_t>>& sets, bool exact);

// The GROUPING values of a set's rows: one per call, bit n-1-i set when argument i is not in `set`.
std::vector<std::int64_t> GroupingValues(const std::vector<std::size_t>& set,
                                         const std::vector<plan::GroupingCall>& calls);

// The node's output schema: key{i} per key (plan::ToArrow of its type), agg{i} per call, then
// grouping{i} per GROUPING call (BIGINT).
std::shared_ptr<arrow::Schema> GroupingSetsSchema(const std::vector<plan::BoundColumn>& keys,
                                                  const std::vector<plan::AggregateCall>& calls,
                                                  std::size_t grouping_calls);

// The schema of one set's table: its keys, then the calls, as GroupTable::NextChunk writes them.
std::shared_ptr<arrow::Schema> SetSchema(const std::vector<plan::BoundColumn>& keys,
                                         const std::vector<std::size_t>& set,
                                         const std::vector<plan::AggregateCall>& calls);

// The keys of a set, as BoundColumns of the node's input.
std::vector<plan::BoundColumn> SetKeys(const std::vector<plan::BoundColumn>& keys,
                                       const std::vector<std::size_t>& set);

// A batch of the node's output from a batch of a set's table (`rows`: the set's keys, then the
// calls): every key, NULL where the set lacks it, then the calls, then each GROUPING value.
arrow::Result<std::shared_ptr<arrow::RecordBatch>> SetRows(
    const arrow::RecordBatch& rows, const std::vector<std::size_t>& set,
    std::span<const std::int64_t> grouping, const std::shared_ptr<arrow::Schema>& schema,
    arrow::MemoryPool* pool);

}  // namespace antb1::exec
