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
//   [Limit] <- Project | Aggregate <- [Filter] <- Scan (every field of the table)
//
// and plan::Optimize rewrites it (projection pruning, COUNT(*) -> RowCount). New operators are
// added as new node structs in the LogicalNode variant; every std::visit over it lists each node
// explicitly, so the physical planner and EXPLAIN fail to compile until they handle a new one.

namespace antb1::plan {

struct OutputColumn {
  std::string name;
  LogicalType type = LogicalType::kBigInt;
};

enum class CompareOp : std::uint8_t { kEq, kNe, kLt, kLe, kGt, kGe };

// "=", "<>", "<", "<=", ">", ">=".
std::string_view ToString(CompareOp op);

enum class AggKind : std::uint8_t { kCountStar, kCount, kSum, kAvg, kMin, kMax };

// "COUNT", "SUM", "AVG", "MIN", "MAX" (COUNT for both COUNT(*) and COUNT(col)).
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
    kIsNotNull,  // folded: true for every non-NULL value (NULL still rejects the row)
    kFalse,      // folded: true for no row
  };

  Kind kind = Kind::kCompare;
  std::optional<BoundColumn> column;  // empty for kFalse
  CompareOp op = CompareOp::kEq;      // kCompare only
  Constant constant;                  // kCompare only; typed as the column
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
struct LimitNode;
struct RowCountNode;

using LogicalNode =
    std::variant<ScanNode, FilterNode, ProjectNode, AggregateNode, LimitNode, RowCountNode>;
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

// Output: the listed input columns, in this order.
struct ProjectNode {
  LogicalNodePtr input;
  std::vector<BoundColumn> columns;
  SourceSpan span;  // the select list
};

// Global aggregation (no GROUP BY). Output: one row with one column per call.
struct AggregateNode {
  LogicalNodePtr input;
  std::vector<AggregateCall> aggregates;
  SourceSpan span;  // the select list
};

// Output: at most `limit` rows of the input.
struct LimitNode {
  LogicalNodePtr input;
  int64_t limit = 0;  // >= 0
  SourceSpan span;    // LIMIT n
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

// "Scan", "Filter", "Project", "Aggregate", "Limit" or "RowCount".
std::string_view NodeName(const LogicalNode& node);

// The span of the query text a node was bound from.
SourceSpan SpanOf(const LogicalNode& node);

// The input of a node; nullptr for leaves (Scan, RowCount).
const LogicalNodePtr* InputOf(const LogicalNode& node);

}  // namespace antb1::plan
