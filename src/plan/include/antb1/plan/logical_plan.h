#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <source_location>
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
//     <- [Compute] <- [Filter] <- [Compute] <- [Filter] <- Scan (every field)
//
// and plan::Optimize rewrites it (COUNT(*) -> RowCount, GROUP BY keys that are functions of
// other keys, Limit below Project, projection pruning).
//
// Every column has a plan-unique ColumnId, defined once by the node that creates it (see
// OutputIds); a column reference (BoundColumn, ColumnExpr) names the column it reads by its id and
// by its `index`, the position in the input's output, which plan::ResolvePositions computes from
// the ids (ADR 0022).
// New operators are added as new node structs in the LogicalNode variant; every std::visit over it
// lists each node explicitly, so the physical planner and EXPLAIN fail to compile until they handle
// a new one.

namespace antb1::plan {

// The identity of a column in a plan: minted from 1 per query, in binding order. A column keeps
// its id when its position changes; Filter, Sort and Limit pass their input's ids through.
enum class ColumnId : std::uint32_t {};

// No column: the reference of a Project constant, the operand-local columns of a PredicateExpr,
// and the references of the executor's positional plans.
inline constexpr ColumnId kNoColumnId{};

struct OutputColumn {
  std::string name;
  LogicalType type = LogicalType::kBigInt;
  ColumnId id = kNoColumnId;  // the root's output column
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
  int index = 0;              // position in the input node's output columns
  ColumnId id = kNoColumnId;  // the column read
  std::string name;           // the table column's name as declared in the table schema
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
  ColumnId id = kNoColumnId;
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
  kEpochMs,    // epoch_ms(integer milliseconds): TIMESTAMP (toDateTime(x) is epoch_ms(x * 1000))
  kExtract,    // extract(field FROM timestamp or date): BIGINT; args: the value, the field
  kDateTrunc,  // date_trunc('unit', timestamp or date): TIMESTAMP; args: the value, the unit
};

// "strlen", "regexp_replace", "epoch_ms", "extract", "date_trunc".
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

// The expression with every column replaced by f(column); the same pointer when no column changes.
// The operand-local columns of a PredicateExpr's predicate are no ColumnExpr and stay as they are.
ExprPtr MapColumns(const ExprPtr& expr, const std::function<ColumnExpr(const ColumnExpr&)>& f);

// Every input column the expression reads.
void CollectColumns(const Expr& expr, std::vector<int>& out);

struct AggregateCall {
  AggKind kind = AggKind::kCountStar;
  std::optional<BoundColumn> arg;           // empty for COUNT(*)
  LogicalType type = LogicalType::kBigInt;  // result type
  ColumnId id = kNoColumnId;                // the result column
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
  std::string table_name;     // the FROM reference: the name as written, or the path
  std::vector<int> fields;    // indices into table->schema()
  std::vector<ColumnId> ids;  // per field: its column
  SourceSpan span;            // the FROM reference
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
  std::vector<ColumnId> ids;   // per expression: its column
  SourceSpan span;             // the first expression in the query
};

// Output: the listed input columns, in this order, and constants. When `constants` is not empty it
// has one entry per output column, and a set entry replaces columns[i] (then with index -1 and id
// kNoColumnId): that output column holds the constant in every row. `ids` names the
// output columns: the binder's are new columns; a column whose id is the id it reads passes it
// through (the optimizer's reordering Project).
struct ProjectNode {
  LogicalNodePtr input;
  std::vector<BoundColumn> columns;
  std::vector<std::optional<Constant>> constants;
  std::vector<ColumnId> ids;  // per output column
  SourceSpan span;            // the select list
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
// (deterministic for a given input in the executor). A key's output is a new column (`key_ids`):
// it is not the input column it groups by (one value per group, merged as above).
struct GroupAggregateNode {
  LogicalNodePtr input;
  std::vector<BoundColumn> keys;  // distinct input columns
  std::vector<ColumnId> key_ids;  // per key: its output column
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
  ColumnId id = kNoColumnId;  // the COUNT(*) column
  SourceSpan span;            // the COUNT(*) call
};

struct LogicalPlan {
  LogicalNodePtr root;
  // Result columns of root: names (aliases applied), types and ids (OutputIds(*root)).
  std::vector<OutputColumn> output;
};

// "Scan", "Filter", "Compute", "Project", "Aggregate", "GroupAggregate", "Sort", "Limit" or
// "RowCount".
std::string_view NodeName(const LogicalNode& node);

// The span of the query text a node was bound from.
SourceSpan SpanOf(const LogicalNode& node);

// The input of a node; nullptr for leaves (Scan, RowCount).
const LogicalNodePtr* InputOf(const LogicalNode& node);

// The ids of a node's output columns, in order. A node defines new columns where it creates them:
// Scan (`ids`), Compute (its input's ids, then `ids`), Project (`ids`), Aggregate (each call's id),
// GroupAggregate (`key_ids`, then each call's id) and RowCount (`id`); Filter, Sort and Limit
// output their input's ids.
std::vector<ColumnId> OutputIds(const LogicalNode& node);

// The plan with every reference's index set to the position of its id in the ids it reads (its
// input's output ids); unchanged nodes keep their pointers, so the result of a resolved plan is the
// plan itself. The last step of plan::Optimize. A plan that breaks an invariant (see
// PositionMismatch) is a programming error: the process aborts with a description that names node
// kinds, ids and positions only.
LogicalPlan ResolvePositions(const LogicalPlan& plan);

// Why the plan is not resolved, or std::nullopt: the first broken invariant (a reference without an
// id, or whose id is not exactly once among the ids it reads; a column defined twice in the plan,
// or without an id; a list of ids whose length differs from its columns'; output ids that are not
// the root's), else the first index that differs from its resolved position, e.g. "Filter: column
// #12 is at 3, not 4". The binder and the optimizer still compute positions themselves; Bind and
// Optimize abort when this finds a difference (ADR 0022, PR P1).
std::optional<std::string> PositionMismatch(const LogicalPlan& plan);

// Aborts with the PositionMismatch description, if there is one, reported at the caller.
void CheckPositions(const LogicalPlan& plan,
                    std::source_location location = std::source_location::current());

}  // namespace antb1::plan
