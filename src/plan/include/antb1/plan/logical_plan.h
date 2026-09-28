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
//   [Limit] <- [Project] <- [Sort] <- [Filter] <- [Compute] <- [Aggregate | GroupAggregate]
//     <- [Filter] <- [Compute] <- [Filter] <- Scan (every field)
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

enum class ArithOp : std::uint8_t {
  kAdd,
  kSubtract,
  kMultiply,
  kDivide,         // / : DOUBLE division
  kIntegerDivide,  // // : truncating integer division (on DOUBLE: division)
  kModulo,         // % : the remainder, with the sign of the dividend
};

// "+", "-", "*", "/", "//", "%".
std::string_view ToString(ArithOp op);

// A column of a node's input.
struct BoundColumn {
  int index = 0;     // position in the input node's output columns
  std::string name;  // the table column's name as declared in the table schema
  LogicalType type = LogicalType::kBigInt;
};

// One comparison of the WHERE conjunction after exact literal folding (docs/sql-subset.md).
struct Predicate {
  enum class Kind : std::uint8_t {
    kCompare,         // column <op> constant
    kCompareColumns,  // column <op> other (comparable types, compared as DuckDB compares them)
    kLike,            // column LIKE constant (a VARCHAR column and pattern; docs/sql-subset.md)
    kNotLike,         // column NOT LIKE constant
    kIn,              // column IN (values): column = v1 OR column = v2 ... (Kleene)
    kNotIn,           // column NOT IN (values): NOT (column IN (values))
    kIsNotNull,       // folded: true for every non-NULL value (NULL still rejects the row)
    kFalse,           // folded: true for no row (inside an expression: NULL for a NULL column)
    kIsTrue,          // a BOOLEAN column (a computed condition) is true
  };

  Kind kind = Kind::kCompare;
  std::optional<BoundColumn> column;  // empty for kFalse
  std::optional<BoundColumn> other;   // kCompareColumns only
  CompareOp op = CompareOp::kEq;      // kCompare and kCompareColumns
  Constant constant;                  // kCompare, kLike and kNotLike; typed as the column
  std::vector<Constant> values;       // kIn and kNotIn (not empty); typed as the column
  SourceSpan span;                    // the comparison in the query
};

struct Expr;
using ExprPtr = std::shared_ptr<const Expr>;

// A column of the input of the node that evaluates the expression.
struct ColumnExpr {
  int index = 0;
};

struct ConstantExpr {
  Constant value;  // of the expression's type
};

// left <op> right, both of the expression's type (the executor casts the operands to it), except
// kDivide, which computes in DOUBLE whatever the operand types.
struct ArithExpr {
  ArithOp op = ArithOp::kAdd;
  ExprPtr left;
  ExprPtr right;
};

struct NegateExpr {
  ExprPtr operand;
};

enum class Function : std::uint8_t {
  kStrlen,         // strlen(varchar): its length in bytes, BIGINT
  kRegexpReplace,  // regexp_replace(varchar, pattern, replacement): the first match replaced (RE2)
};

// "strlen", "regexp_replace".
std::string_view ToString(Function function);

// A scalar function of the arguments (constants where the function needs them: the pattern and
// the replacement of regexp_replace).
struct FunctionExpr {
  Function function = Function::kStrlen;
  std::vector<ExprPtr> args;
};

// A condition of the binder's WHERE forms (a comparison, [NOT] LIKE, [NOT] IN, folded as in
// WHERE) inside a boolean expression: its column is operands[0] (index 0) and, for
// kCompareColumns, its other column operands[1] (index 1). BOOLEAN, with NULL where SQL's
// three-valued logic has it: a kFalse or kIsNotNull predicate is NULL for a NULL operand.
struct PredicateExpr {
  Predicate predicate;
  std::vector<ExprPtr> operands;
};

enum class BoolOp : std::uint8_t { kAnd, kOr, kNot };

// AND or OR of two or more BOOLEAN arguments, or NOT of one, in three-valued logic.
struct BoolExpr {
  BoolOp op = BoolOp::kAnd;
  std::vector<ExprPtr> args;
};

// CASE WHEN whens[0] THEN thens[0] ... ELSE otherwise END: the first branch whose condition is
// true (a NULL condition is not), else `otherwise` (NULL when it is null). The values are of the
// expression's type (the executor casts them to it).
struct CaseExpr {
  std::vector<ExprPtr> whens;  // BOOLEAN
  std::vector<ExprPtr> thens;  // as many as whens
  ExprPtr otherwise;           // may be null
};

// A scalar expression, typed as DuckDB types it (docs/sql-subset.md). Integer arithmetic is exact
// in its type: an overflow is an execution error, as in DuckDB.
struct Expr {
  std::variant<ColumnExpr, ConstantExpr, ArithExpr, NegateExpr, FunctionExpr, PredicateExpr,
               BoolExpr, CaseExpr>
      node;
  LogicalType type = LogicalType::kBigInt;
  std::string name;  // DuckDB's result name of the expression, e.g. (a + 1)
};

// Whether two expressions compute the same values: the same structure, ignoring names.
bool SameExpr(const Expr& a, const Expr& b);

// The expression with every column index i replaced by remap[i] (which must be >= 0).
ExprPtr Renumber(const ExprPtr& expr, const std::vector<int>& remap);

// Every input column the expression reads.
void CollectColumns(const Expr& expr, std::vector<int>& out);

struct AggregateCall {
  AggKind kind = AggKind::kCountStar;
  std::optional<BoundColumn> arg;           // empty for COUNT(*)
  LogicalType type = LogicalType::kBigInt;  // result type
  SourceSpan span;                          // the call in the query
};

struct ScanNode;
struct FilterNode;
struct ComputeNode;
struct ProjectNode;
struct AggregateNode;
struct GroupAggregateNode;
struct SortNode;
struct LimitNode;
struct RowCountNode;

using LogicalNode = std::variant<ScanNode, FilterNode, ComputeNode, ProjectNode, AggregateNode,
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

// Output: the input columns, then one column per expression (over the input columns), in order.
struct ComputeNode {
  LogicalNodePtr input;
  std::vector<ExprPtr> exprs;  // not empty
  SourceSpan span;             // the first expression in the query
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

// "Scan", "Filter", "Compute", "Project", "Aggregate", "GroupAggregate", "Sort", "Limit" or
// "RowCount".
std::string_view NodeName(const LogicalNode& node);

// The span of the query text a node was bound from.
SourceSpan SpanOf(const LogicalNode& node);

// The input of a node; nullptr for leaves (Scan, RowCount).
const LogicalNodePtr* InputOf(const LogicalNode& node);

}  // namespace antb1::plan
