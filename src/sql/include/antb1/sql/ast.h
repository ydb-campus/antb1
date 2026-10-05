#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "antb1/common/source_span.h"

// AST of the supported SQL subset (docs/sql-subset.md). Every node carries the byte span of the
// source text it was parsed from; EqualIgnoringSpans() compares structure only (used by the
// Parse(ToSql(x)) == x round-trip property). Names keep their spelling (quoted ones unescaped); the
// binder matches them case-insensitively. Expressions are trees (Expr); the parser keeps them as
// written (5 < c stays literal-first), and AsComparison() gives a condition's normalized form.

namespace antb1::sql {

// A value-semantic owning pointer: copies deep-copy the pointee. Lets recursive nodes hold
// children of a type that is incomplete where the node is declared.
template <class T>
class Box {
 public:
  explicit Box(T value) : ptr_(std::make_unique<T>(std::move(value))) {}
  Box(const Box& other) : ptr_(std::make_unique<T>(*other.ptr_)) {}
  Box(Box&& other) noexcept = default;
  Box& operator=(const Box& other) {
    if (this != &other) {
      ptr_ = std::make_unique<T>(*other.ptr_);
    }
    return *this;
  }
  Box& operator=(Box&& other) noexcept = default;
  ~Box() = default;

  const T& operator*() const { return *ptr_; }
  T& operator*() { return *ptr_; }
  const T* operator->() const { return ptr_.get(); }
  T* operator->() { return ptr_.get(); }

 private:
  std::unique_ptr<T> ptr_;
};

struct Expr;

// A column name, optionally qualified by the name of a FROM item (t.x).
struct ColumnRef {
  std::string name;
  bool quoted = false;
  SourceSpan span;                // the qualifier (when there is one) through the name
  std::string qualifier;          // t.x: "t", as written (a quoted one unescaped); empty: none
  bool qualifier_quoted = false;  // (the lexer makes no empty quoted identifier)
};

struct Literal {
  enum class Kind : std::uint8_t { kInteger, kDecimal, kString, kDate, kTimestamp };
  Kind kind = Kind::kInteger;
  bool negative = false;  // leading '-' (integers/decimals only)
  std::string
      text;  // number as written without the sign / unescaped string, date or timestamp text
  SourceSpan span;
};

enum class AggKind : std::uint8_t { kCountStar, kCount, kSum, kAvg, kMin, kMax };

struct AggregateCall {
  AggKind kind = AggKind::kCountStar;
  std::optional<Box<Expr>> arg;  // empty only for kCountStar
  bool distinct = false;         // COUNT(DISTINCT expr); only kCount
  SourceSpan span;

  // The argument when it is a plain column, else nullptr (also for COUNT(*)).
  [[nodiscard]] const ColumnRef* arg_column() const;
};

enum class UnaryOp : std::uint8_t { kNegate, kNot };

// Arithmetic, comparisons and the boolean connectives, by precedence (docs/sql-subset.md).
enum class BinaryOp : std::uint8_t {
  kAdd,
  kSubtract,
  kMultiply,
  kDivide,         // /
  kIntegerDivide,  // //
  kModulo,         // %
  kEq,
  kNe,
  kLt,
  kLe,
  kGt,
  kGe,
  kAnd,
  kOr,
};

struct UnaryExpr {
  UnaryOp op = UnaryOp::kNegate;
  Box<Expr> operand;
  SourceSpan op_span;  // the '-' or NOT
  SourceSpan span;
};

struct BinaryExpr {
  BinaryOp op = BinaryOp::kAdd;
  Box<Expr> left;
  Box<Expr> right;
  SourceSpan op_span;  // the operator token
  SourceSpan span;
};

// operand [NOT] LIKE pattern
struct LikeExpr {
  Box<Expr> operand;
  Box<Expr> pattern;
  bool negated = false;
  SourceSpan op_span;  // [NOT] LIKE
  SourceSpan span;
};

// operand [NOT] IN (value, ...)
struct InExpr {
  Box<Expr> operand;
  std::vector<Expr> list;  // at least one
  bool negated = false;
  SourceSpan op_span;  // [NOT] IN
  SourceSpan span;     // operand .. ')'
};

// operand [NOT] BETWEEN low AND high
struct BetweenExpr {
  Box<Expr> operand;
  Box<Expr> low;
  Box<Expr> high;
  bool negated = false;
  SourceSpan op_span;  // [NOT] BETWEEN
  SourceSpan span;     // operand .. high
};

// name(arg, ...): any function other than the five aggregates.
struct FunctionCall {
  std::string name;  // as written
  bool quoted = false;
  std::vector<Expr> args;
  SourceSpan name_span;
  SourceSpan span;
};

struct CaseBranch {
  Box<Expr> when;
  Box<Expr> then;
};

// CASE [operand] WHEN .. THEN .. [WHEN .. THEN ..] [ELSE ..] END
struct CaseExpr {
  std::optional<Box<Expr>> operand;  // the simple form CASE x WHEN v THEN ...
  std::vector<CaseBranch> branches;  // at least one
  std::optional<Box<Expr>> otherwise;
  SourceSpan span;
};

// EXTRACT(field FROM source)
struct ExtractExpr {
  std::string field;  // as written
  Box<Expr> source;
  SourceSpan field_span;
  SourceSpan span;
};

// CAST(operand AS type), TRY_CAST(operand AS type) and operand::type: one node for both spellings,
// which ToSql writes as CAST. `type` is the type's name upper-cased, `type_params` its integer
// parameters as written: DECIMAL(15, 2) is "DECIMAL" {"15", "2"}.
struct CastExpr {
  Box<Expr> operand;
  std::string type;
  std::vector<std::string> type_params;
  bool try_cast = false;
  SourceSpan op_span;    // CAST, TRY_CAST or '::'
  SourceSpan type_span;  // the type's name through its ')'
  SourceSpan span;       // CAST .. ')', or the operand's first token .. the type
};

using ExprNode = std::variant<ColumnRef, Literal, AggregateCall, UnaryExpr, BinaryExpr, LikeExpr,
                              InExpr, BetweenExpr, FunctionCall, CaseExpr, ExtractExpr, CastExpr>;

template <class T>
concept ExprNodeKind =
    std::is_same_v<T, ColumnRef> || std::is_same_v<T, Literal> ||
    std::is_same_v<T, AggregateCall> || std::is_same_v<T, UnaryExpr> ||
    std::is_same_v<T, BinaryExpr> || std::is_same_v<T, LikeExpr> || std::is_same_v<T, InExpr> ||
    std::is_same_v<T, BetweenExpr> || std::is_same_v<T, FunctionCall> ||
    std::is_same_v<T, CaseExpr> || std::is_same_v<T, ExtractExpr> || std::is_same_v<T, CastExpr>;

// An expression: a variant of its node kinds (std::get, std::get_if and std::visit apply). It
// converts only from its node kinds: variant's catch-all converting constructor would make every
// node constructible from anything that converts to one of its first members, which recurses
// through Box<Expr>.
struct Expr : ExprNode {
  template <class Node>
    requires ExprNodeKind<std::remove_cvref_t<Node>>
  explicit Expr(Node&& node) : ExprNode(std::forward<Node>(node)) {}
  [[nodiscard]] SourceSpan span() const;
};

// A select item, an ORDER BY item or a GROUP BY item. In GROUP BY and ORDER BY an unsigned integer
// literal is a position in the select list (1-based); see the binder.
using SelectExpr = Expr;
using GroupExpr = Expr;

// kLike and kNotLike: column [NOT] LIKE 'pattern' (the pattern is always the literal, on the
// right). kIn and kNotIn: column [NOT] IN (literal, ...), the values in Comparison::list.
enum class CompareOp : std::uint8_t { kEq, kNe, kLt, kLe, kGt, kGe, kLike, kNotLike, kIn, kNotIn };

// The normalized form of a simple condition: column <op> literal (see AsComparison).
struct Comparison {
  ColumnRef column;
  CompareOp op = CompareOp::kEq;
  Literal literal;            // every op but kIn and kNotIn
  std::vector<Literal> list;  // kIn and kNotIn: the values, in order (at least one)
  SourceSpan span;
};

// The normalized form of a simple HAVING condition: <aggregate or column> <op> literal.
using HavingOperand = std::variant<AggregateCall, ColumnRef>;

struct HavingComparison {
  HavingOperand operand;
  CompareOp op = CompareOp::kEq;
  Literal literal;
  std::vector<Literal> list;
  SourceSpan span;
};

// `expr` as `column <op> literal` (a literal-first comparison mirrored: 5 < c is c > 5; LIKE with
// a literal pattern; IN with a list of literals), or std::nullopt for any other expression.
std::optional<Comparison> AsComparison(const Expr& expr);
// Likewise with an aggregate call or a column on the operand side.
std::optional<HavingComparison> AsHavingComparison(const Expr& expr);
// The expression of a normalized condition (operand first); AsComparison(ToExpr(c)) is c.
Expr ToExpr(const Comparison& cmp);
Expr ToExpr(const HavingComparison& cmp);

struct SelectItem {
  SelectExpr expr;
  std::optional<std::string> alias;  // as written (quoted aliases unescaped)
  SourceSpan span;                   // expression and alias
};

// NULLS FIRST / NULLS LAST as written; kDefault when omitted.
enum class NullsOrder : std::uint8_t { kDefault, kFirst, kLast };

// One ORDER BY item: an expression (a name may be a select alias, resolved by the binder; an
// integer literal a position in the select list).
struct OrderItem {
  SelectExpr expr;
  bool descending = false;  // DESC; ASC (written or not) is false
  NullsOrder nulls = NullsOrder::kDefault;
  SourceSpan span;  // expression and modifiers
};

struct TableRef {
  enum class Kind : std::uint8_t { kName, kPath };
  Kind kind = Kind::kName;
  std::string name;  // identifier, or the path from a string literal
  bool quoted = false;
  SourceSpan span;
};

// How a FROM item connects to the items before it (ADR 0022): a JOIN binds tighter than a comma
// and associates to the left, so the items from the last comma up to a JOIN are its left input.
enum class Connector : std::uint8_t {
  kFirst,  // the first item
  kComma,  // ,
  kCross,  // CROSS JOIN
  kInner,  // [INNER] JOIN ... ON
  kLeft,   // LEFT [OUTER] JOIN ... ON
};

// One item of a FROM list: a table or a path, its alias, and how it joins the items before it.
struct FromItem {
  Connector connector = Connector::kFirst;
  TableRef table;
  std::optional<std::string> alias;  // as written (quoted and string aliases unescaped)
  std::vector<Expr> on;              // kInner and kLeft: the ON conjuncts (at least one), as WHERE
  SourceSpan connector_span;  // ",", "CROSS JOIN", "JOIN", "LEFT OUTER JOIN", ... (not kFirst)
  SourceSpan alias_span;      // [AS] alias (when alias)
  SourceSpan on_span;         // ON and its condition (kInner and kLeft)
  SourceSpan span;            // the table through its alias
};

struct SelectStatement {
  bool star = false;     // SELECT *
  SourceSpan star_span;  // the '*' (when star)
  std::vector<SelectItem> items;
  std::vector<FromItem> from;          // at least one: a flat list, never a tree (ADR 0022)
  std::vector<Expr> where;             // conjuncts: the top-level AND chain, split
  std::vector<GroupExpr> group_by;     // GROUP BY items (aliases and positions: see the binder)
  SourceSpan group_by_span;            // GROUP BY and its list (when group_by is not empty)
  std::vector<Expr> having;            // conjuncts, as in WHERE
  SourceSpan having_span;              // HAVING and its predicate (when having is not empty)
  std::vector<OrderItem> order_by;     // ORDER BY items
  SourceSpan order_by_span;            // ORDER BY and its list (when order_by is not empty)
  std::optional<std::int64_t> limit;   // non-negative
  SourceSpan limit_span;               // LIMIT and its value (when limit)
  std::optional<std::int64_t> offset;  // non-negative
  SourceSpan offset_span;              // OFFSET and its value (when offset)
  SourceSpan span;                     // SELECT .. last token of the query (without ';')
};

std::string_view ToString(AggKind kind);
std::string_view ToString(CompareOp op);
std::string_view ToString(BinaryOp op);          // "+", "//", "<>", "AND", ...
std::string_view ToString(NullsOrder nulls);     // "", "NULLS FIRST" or "NULLS LAST"
std::string_view ToString(Connector connector);  // "", ",", "CROSS JOIN", "INNER JOIN", "LEFT JOIN"

bool EqualIgnoringSpans(const Expr& a, const Expr& b);
bool EqualIgnoringSpans(const SelectStatement& a, const SelectStatement& b);

// The levels of an expression tree: the nodes on its longest path from the root to a leaf (a leaf
// alone is 1). The parser accepts no tree deeper than kMaxExpressionDepth (parser.h).
std::size_t Depth(const Expr& expr);
// The deepest expression tree of a statement (its select items, ON, WHERE and HAVING conjuncts and
// GROUP BY and ORDER BY items); 0 when it has none.
std::size_t Depth(const SelectStatement& stmt);

}  // namespace antb1::sql
