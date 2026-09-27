#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

#include "canonical.h"
#include "engine.h"
#include "result_diff.h"

// Comparison of the answer to a query with ORDER BY, where rows with equal sort keys may come in
// any order, and with LIMIT/OFFSET any of the rows tied at the window's edges are right.
//
// The oracle side is an "augmented" query: the original select list followed by one column per
// ORDER BY key (an alias resolved to its select item's expression, as the binder does), the same
// ORDER BY, and no LIMIT or OFFSET. Its answer, in DuckDB's order, defines the rank of every row
// and the runs of rows with equal keys (R keys equal within the tolerance of CompareBlocks).
// antb1's row i must then be a distinct row of the run that holds rank offset + i.

namespace antb1::slt {

struct OrderedQuery {
  std::string augmented_sql;  // without LIMIT and OFFSET
  std::size_t keys = 0;       // ORDER BY keys, the last columns of the augmented query
  std::optional<int64_t> limit;
  int64_t offset = 0;
};

// The augmented form of a query with ORDER BY; std::nullopt without ORDER BY or for text antb1's
// parser does not accept.
std::optional<OrderedQuery> MakeOrderedQuery(std::string_view sql);

// `augmented_sql` with LIMIT `rows` (none: as is).
std::string WithLimit(const OrderedQuery& q, std::optional<int64_t> rows);

// std::nullopt if antb1's answer is a right answer to the ordered query `q`. The oracle's answer
// `oracle` is compared first (equal rows in equal order are right); only otherwise is the augmented
// query run, through `run(limit)`, with limits that grow until the run of ties at the window's end
// is complete.
std::optional<Discrepancy> CompareOrdered(
    const ResultSet& oracle, const ResultSet& antb1, const OrderedQuery& q,
    const std::function<ExecResult(std::optional<int64_t>)>& run);

// The comparison that fits `sql`: CompareOrdered with ORDER BY; CompareLimited for LIMIT or OFFSET
// without ORDER BY when `rows` (a projection or GROUP BY: any rows are right); else CompareAnswers
// with `sort`. The extra oracle queries run on `oracle`.
std::optional<Discrepancy> CompareQueryAnswers(std::string_view sql, const ResultSet& oracle_answer,
                                               const ResultSet& antb1_answer, Engine& oracle,
                                               bool rows, SortMode sort);

}  // namespace antb1::slt
