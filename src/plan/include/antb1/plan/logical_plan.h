#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include <arrow/result.h>
#include <arrow/type_fwd.h>

#include "antb1/common/int128.h"
#include "antb1/common/source_span.h"
#include "antb1/plan/table.h"
#include "antb1/plan/types.h"

// Logical plan: a tree of immutable nodes (docs/architecture.md). The binder builds
//
//   [Limit] <- Aggregate | Project <- [Sort] <- [GroupAggregate] <- [Filter] <- Scan (every field)
//
// and plan::Optimize rewrites it (Limit below Project, projection pruning, COUNT(*) -> RowCount).
// New operators are added as new node structs in the LogicalNode variant; every std::visit over it
// lists each node explicitly, so the physical planner and EXPLAIN fail to compile until they handle
// a new one.

namespace antb1::plan {

struct OutputColumn {
  std::string name;
  LogicalType type = LogicalType::kBigInt;
};

enum class CompareOp : std::uint8_t { kEq, kNe, kLt, kLe, kGt, kGe };

// "=", "<>", "<", "<=", ">", ">=".
std::string_view ToString(CompareOp op);

// kCountDistinct is COUNT(DISTINCT col): the number of distinct non-NULL values (a DOUBLE -0.0 is
// 0.0 and every NaN one value, as in DuckDB).
enum class AggKind : std::uint8_t { kCountStar, kCount, kSum, kAvg, kMin, kMax, kCountDistinct };

// "COUNT", "SUM", "AVG", "MIN", "MAX" (COUNT for COUNT(*), COUNT(col) and COUNT(DISTINCT col)).
std::string_view ToString(AggKind kind);

// A constant of a column's logical type, folded exactly from a SQL literal by the binder.
struct Constant {
  LogicalType type = LogicalType::kBigInt;
  // Every integer type and DATE (days since 1970-01-01): Int128 inside the type's range.
  // DOUBLE: double. VARCHAR: the bytes.
  std::variant<Int128, double, std::string> value;
};

// `text` with bytes outside printable ASCII and backslashes as \xHH and `quote` characters doubled
// ('\0': none), so that EXPLAIN output stays one line of ASCII whatever the data.
std::string EscapeText(std::string_view text, char quote);

// Deterministic text for EXPLAIN: 42, -1.5, 'it''s' (bytes outside printable ASCII as \xHH),
// DATE '2022-01-08'.
std::string ToString(const Constant& constant);

// The constant as an Arrow scalar of type ToArrow(constant.type).
arrow::Result<std::shared_ptr<arrow::Scalar>> ToArrowScalar(const Constant& constant);

// A column of a node's input.
struct BoundColumn {
  int index = 0;     // position in the input node's output columns
  std::string name;  // the table column's name as declared in the table schema
  LogicalType type = LogicalType::kBigInt;
};

// One comparison of the WHERE conjunction after exact literal folding (docs/sql-subset.md).
struct Predicate {
  enum class Kind : std::uint8_t {
    kCompare,    // column <op> constant
    kLike,       // column LIKE constant (a VARCHAR column and pattern; docs/sql-subset.md)
    kNotLike,    // column NOT LIKE constant
    kIn,         // column IN (values): column = v1 OR column = v2 ... (Kleene)
    kNotIn,      // column NOT IN (values): NOT (column IN (values))
    kIsNotNull,  // folded: true for every non-NULL value (NULL still rejects the row)
    kFalse,      // folded: true for no row
  };

  Kind kind = Kind::kCompare;
  std::optional<BoundColumn> column;  // empty for kFalse
  CompareOp op = CompareOp::kEq;      // kCompare only
  Constant constant;                  // kCompare, kLike and kNotLike; typed as the column
  std::vector<Constant> values;       // kIn and kNotIn (not empty); typed as the column
  SourceSpan span;                    // the comparison in the query
};

struct AggregateCall {
  AggKind kind = AggKind::kCountStar;
  std::optional<BoundColumn> arg;           // empty for COUNT(*)
  LogicalType type = LogicalType::kBigInt;  // result type
  SourceSpan span;                          // the call in the query
};

struct ScanNode;
struct FilterNode;
struct ProjectNode;
struct AggregateNode;
struct GroupAggregateNode;
struct SortNode;
struct LimitNode;
struct RowCountNode;

using LogicalNode = std::variant<ScanNode, FilterNode, ProjectNode, AggregateNode,
                                 GroupAggregateNode, SortNode, LimitNode, RowCountNode>;
using LogicalNodePtr = std::shared_ptr<const LogicalNode>;

// Reads top-level fields of a table. Output: the fields, in this order.
struct ScanNode {
  std::shared_ptr<Table> table;
  std::string table_name;   // the FROM reference: the name as written, or the path
  std::vector<int> fields;  // indices into table->schema()
  SourceSpan span;          // the FROM reference
};

// Keeps the rows for which every predicate is true (a NULL comparison rejects the row). Output:
// the input columns.
struct FilterNode {
  LogicalNodePtr input;
  std::vector<Predicate> predicates;
  SourceSpan span;  // the WHERE conjunction
};

// Output: the listed input columns, in this order, and constants. When `constants` is not empty it
// has one entry per output column, and a set entry replaces columns[i] (whose index is then -1):
// that output column holds the constant in every row.
struct ProjectNode {
  LogicalNodePtr input;
  std::vector<BoundColumn> columns;
  std::vector<std::optional<Constant>> constants;
  SourceSpan span;  // the select list
};

// Global aggregation (no GROUP BY). Output: one row with one column per call.
struct AggregateNode {
  LogicalNodePtr input;
  std::vector<AggregateCall> aggregates;
  SourceSpan span;  // the select list
};

// Grouped aggregation (GROUP BY): one row per distinct combination of the keys (NULL is a key
// value; a DOUBLE key groups -0.0 with 0.0 and every NaN together, keeping the value first seen).
// Output: the keys, then one column per call; no row over no input rows. Without keys (GROUP BY
// only constants) there is one group when there is any input row. Row order is unspecified
// (deterministic for a given input in the executor).
struct GroupAggregateNode {
  LogicalNodePtr input;
  std::vector<BoundColumn> keys;  // distinct input columns
  std::vector<AggregateCall> aggregates;
  SourceSpan span;  // GROUP BY and its list
};

// One ORDER BY key. Values compare as in DuckDB: VARCHAR by bytes, DOUBLE with -0.0 equal to 0.0
// and NaN above every number; NULLs come first or last whatever the direction.
struct SortKey {
  BoundColumn column;
  bool descending = false;
  bool nulls_first = false;  // DuckDB's default: NULLS LAST for ASC and DESC
};

// Output: the input rows ordered by the keys (the first key first). Rows with equal keys keep no
// particular order (the executor keeps their input order).
struct SortNode {
  LogicalNodePtr input;
  std::vector<SortKey> keys;  // not empty
  SourceSpan span;            // ORDER BY and its list
};

// Output: the input rows after skipping `offset`, at most `limit` of them.
struct LimitNode {
  LogicalNodePtr input;
  std::optional<int64_t> limit;  // >= 0; none: every row after the offset
  int64_t offset = 0;            // >= 0
  SourceSpan span;               // LIMIT n and OFFSET m
};

// COUNT(*) without WHERE, answered from table metadata (Table::exact_row_count()). Output: one
// BIGINT row.
struct RowCountNode {
  std::shared_ptr<Table> table;
  std::string table_name;
  SourceSpan span;  // the COUNT(*) call
};

struct LogicalPlan {
  LogicalNodePtr root;
  std::vector<OutputColumn> output;  // result columns of root: names (aliases applied) and types
};

// "Scan", "Filter", "Project", "Aggregate", "GroupAggregate", "Sort", "Limit" or "RowCount".
std::string_view NodeName(const LogicalNode& node);

// The span of the query text a node was bound from.
SourceSpan SpanOf(const LogicalNode& node);

// The input of a node; nullptr for leaves (Scan, RowCount).
const LogicalNodePtr* InputOf(const LogicalNode& node);

}  // namespace antb1::plan
