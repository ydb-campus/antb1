#include "antb1/plan/binder.h"

#include <algorithm>
#include <cstddef>
#include <format>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <arrow/result.h>
#include <arrow/type.h>

#include "antb1/common/check.h"
#include "antb1/common/int128.h"
#include "antb1/common/narrow.h"
#include "antb1/common/source_span.h"
#include "antb1/plan/catalog.h"
#include "antb1/plan/literal.h"
#include "antb1/plan/logical_plan.h"
#include "antb1/plan/sql_status.h"
#include "antb1/plan/types.h"
#include "antb1/sql/ast.h"
#include "antb1/sql/error.h"
#include "antb1/sql/parser.h"

// The binding rules are documented in docs/sql-subset.md and
// docs/adr/0004-types-null-overflow-semantics.md.
//
// A function that answers for every kind of expression node visits the node with a struct of one
// overload per kind (FoldDateCastsOf, FirstUnsupportedOf, RejectConditionOf, ReadsColumnOf,
// ContainsAggregateOf, ExprNameOf, BindInputOf, BindOutputOf, BindConditionOf), so a new kind fails
// to compile until each one handles it.

namespace antb1::plan {
namespace {

constexpr std::size_t kMaxEchoedBytes = 32;

// Literal text echoed in an error message, clipped (it may be long).
std::string Clip(std::string_view text) {
  if (text.size() <= kMaxEchoedBytes) {
    return std::string(text);
  }
  return std::string(text.substr(0, kMaxEchoedBytes)) + "...";
}

SourceSpan Cover(SourceSpan first, SourceSpan last) {
  return SourceSpan{.offset = first.offset, .length = last.offset + last.length - first.offset};
}

LogicalNodePtr Make(LogicalNode node) {
  return std::make_shared<const LogicalNode>(std::move(node));
}

CompareOp ToPlan(sql::CompareOp op) {
  switch (op) {
    case sql::CompareOp::kEq:
      return CompareOp::kEq;
    case sql::CompareOp::kNe:
      return CompareOp::kNe;
    case sql::CompareOp::kLt:
      return CompareOp::kLt;
    case sql::CompareOp::kLe:
      return CompareOp::kLe;
    case sql::CompareOp::kGt:
      return CompareOp::kGt;
    case sql::CompareOp::kGe:
      return CompareOp::kGe;
    case sql::CompareOp::kLike:  // bound as Predicate::Kind::kLike, never as a comparison
    case sql::CompareOp::kNotLike:
    case sql::CompareOp::kIn:  // bound as Predicate::Kind::kIn
    case sql::CompareOp::kNotIn:
      break;
  }
  return CompareOp::kEq;
}

AggKind ToPlan(sql::AggKind kind) {
  switch (kind) {
    case sql::AggKind::kCountStar:
      return AggKind::kCountStar;
    case sql::AggKind::kCount:
      return AggKind::kCount;
    case sql::AggKind::kSum:
      return AggKind::kSum;
    case sql::AggKind::kAvg:
      return AggKind::kAvg;
    case sql::AggKind::kMin:
      return AggKind::kMin;
    case sql::AggKind::kMax:
      return AggKind::kMax;
  }
  return AggKind::kCountStar;
}

// The argument of DuckDB's result name of an aggregate: as written in the query, double-quoted
// when it is not a plain identifier or is a reserved word (sum("from"), sum("a b")).
std::string ArgumentName(std::string_view name) {
  if (IsPlainIdentifier(name) && !sql::IsReservedWord(name)) {
    return std::string(name);
  }
  std::string out = "\"";
  for (const char c : name) {
    out += c;
    if (c == '"') {
      out += '"';
    }
  }
  return out + "\"";
}

// DuckDB's name of a literal inside an expression: numbers as their value (a DECIMAL as its value
// prints, 007.50 as 7.50; a DOUBLE as written), strings quoted.
std::string LiteralName(const sql::Literal& lit) {
  switch (lit.kind) {
    case sql::Literal::Kind::kInteger: {
      const auto exact = ParseExactNumber(lit.text, lit.negative);
      if (exact.has_value() && !exact->huge) {
        return Int128ToString(exact->negative ? -exact->magnitude : exact->magnitude);
      }
      break;
    }
    case sql::Literal::Kind::kDecimal:
      if (const auto decimal = ParseDecimalLiteral(lit.text, lit.negative)) {
        return FormatDecimal(decimal->unscaled, decimal->type.width(), decimal->type.scale());
      }
      break;
    case sql::Literal::Kind::kString:
    case sql::Literal::Kind::kDate:
    case sql::Literal::Kind::kTimestamp:
      return "'" + lit.text + "'";
  }
  return (lit.negative ? "-" : "") + lit.text;
}

std::string ResultName(const sql::AggregateCall& call);

// DuckDB's date part spellings (checked against DuckDB 1.5.5), lower case: the EXTRACT field and
// the date_trunc unit each one means ("": not one). date_trunc to a day-of-week or day-of-year
// part is to the day, to epoch to the second; `dec` is a date_trunc unit only (EXTRACT(dec ...)
// does not parse in DuckDB).
struct DatePart {
  std::string_view spelling;
  std::string_view field;
  std::string_view unit;
  bool keyword = false;  // a DuckDB keyword: EXTRACT names it by its field, in lower case
};

constexpr auto kDateParts = std::to_array<DatePart>({
    {.spelling = "year", .field = "year", .unit = "year", .keyword = true},
    {.spelling = "years", .field = "year", .unit = "year", .keyword = true},
    {.spelling = "y", .field = "year", .unit = "year"},
    {.spelling = "yr", .field = "year", .unit = "year"},
    {.spelling = "yrs", .field = "year", .unit = "year"},
    {.spelling = "month", .field = "month", .unit = "month", .keyword = true},
    {.spelling = "months", .field = "month", .unit = "month", .keyword = true},
    {.spelling = "mon", .field = "month", .unit = "month"},
    {.spelling = "mons", .field = "month", .unit = "month"},
    {.spelling = "day", .field = "day", .unit = "day", .keyword = true},
    {.spelling = "days", .field = "day", .unit = "day", .keyword = true},
    {.spelling = "d", .field = "day", .unit = "day"},
    {.spelling = "dayofmonth", .field = "day", .unit = "day"},
    {.spelling = "hour", .field = "hour", .unit = "hour", .keyword = true},
    {.spelling = "hours", .field = "hour", .unit = "hour", .keyword = true},
    {.spelling = "h", .field = "hour", .unit = "hour"},
    {.spelling = "hr", .field = "hour", .unit = "hour"},
    {.spelling = "hrs", .field = "hour", .unit = "hour"},
    {.spelling = "minute", .field = "minute", .unit = "minute", .keyword = true},
    {.spelling = "minutes", .field = "minute", .unit = "minute", .keyword = true},
    {.spelling = "m", .field = "minute", .unit = "minute"},
    {.spelling = "min", .field = "minute", .unit = "minute"},
    {.spelling = "mins", .field = "minute", .unit = "minute"},
    {.spelling = "second", .field = "second", .unit = "second", .keyword = true},
    {.spelling = "seconds", .field = "second", .unit = "second", .keyword = true},
    {.spelling = "s", .field = "second", .unit = "second"},
    {.spelling = "sec", .field = "second", .unit = "second"},
    {.spelling = "secs", .field = "second", .unit = "second"},
    {.spelling = "millisecond", .field = "millisecond", .unit = "millisecond", .keyword = true},
    {.spelling = "milliseconds", .field = "millisecond", .unit = "millisecond", .keyword = true},
    {.spelling = "ms", .field = "millisecond", .unit = "millisecond"},
    {.spelling = "msec", .field = "millisecond", .unit = "millisecond"},
    {.spelling = "msecs", .field = "millisecond", .unit = "millisecond"},
    {.spelling = "microsecond", .field = "microsecond", .unit = "microsecond", .keyword = true},
    {.spelling = "microseconds", .field = "microsecond", .unit = "microsecond", .keyword = true},
    {.spelling = "us", .field = "microsecond", .unit = "microsecond"},
    {.spelling = "usec", .field = "microsecond", .unit = "microsecond"},
    {.spelling = "usecs", .field = "microsecond", .unit = "microsecond"},
    {.spelling = "quarter", .field = "quarter", .unit = "quarter", .keyword = true},
    {.spelling = "quarters", .field = "quarter", .unit = "quarter", .keyword = true},
    {.spelling = "week", .field = "week", .unit = "week", .keyword = true},
    {.spelling = "weeks", .field = "week", .unit = "week", .keyword = true},
    {.spelling = "w", .field = "week", .unit = "week"},
    {.spelling = "weekofyear", .field = "week", .unit = "week"},
    {.spelling = "dow", .field = "dow", .unit = "day"},
    {.spelling = "dayofweek", .field = "dow", .unit = "day"},
    {.spelling = "weekday", .field = "dow", .unit = "day"},
    {.spelling = "isodow", .field = "isodow", .unit = "day"},
    {.spelling = "doy", .field = "doy", .unit = "day"},
    {.spelling = "dayofyear", .field = "doy", .unit = "day"},
    {.spelling = "isoyear", .field = "isoyear", .unit = "isoyear"},
    {.spelling = "epoch", .field = "epoch", .unit = "second"},
    {.spelling = "decade", .field = "decade", .unit = "decade", .keyword = true},
    {.spelling = "decades", .field = "decade", .unit = "decade", .keyword = true},
    {.spelling = "dec", .field = "", .unit = "decade"},
    {.spelling = "decs", .field = "decade", .unit = "decade"},
    {.spelling = "century", .field = "century", .unit = "century", .keyword = true},
    {.spelling = "centuries", .field = "century", .unit = "century", .keyword = true},
    {.spelling = "c", .field = "century", .unit = "century"},
    {.spelling = "cent", .field = "century", .unit = "century"},
    {.spelling = "millennium", .field = "millennium", .unit = "millennium", .keyword = true},
    {.spelling = "millennia", .field = "millennium", .unit = "millennium", .keyword = true},
    {.spelling = "mil", .field = "millennium", .unit = "millennium"},
    {.spelling = "mils", .field = "millennium", .unit = "millennium"},
});

// The date part a spelling (any case) means, or std::nullopt.
std::optional<DatePart> FindDatePart(std::string_view spelling) {
  const std::string lower = AsciiLower(spelling);
  const auto* part = std::ranges::find(kDateParts, lower, &DatePart::spelling);
  return part == kDateParts.end() ? std::nullopt : std::optional(*part);
}

// Whether a spelling (any case) is a date_trunc unit.
bool IsTruncUnit(std::string_view spelling) {
  const std::optional<DatePart> part = FindDatePart(spelling);
  return part.has_value() && !part->unit.empty();
}

// The field of EXTRACT as DuckDB names it: a keyword spelling by its field in lower case
// (EXTRACT(Years ...) is 'year'), any other spelling as written.
std::string ExtractFieldName(std::string_view field) {
  const std::optional<DatePart> part = FindDatePart(field);
  return part.has_value() && part->keyword ? std::string(part->field) : std::string(field);
}

std::string ExprName(const sql::Expr& expr);

// CASE  WHEN ((a = 1)) THEN (b) ELSE NULL END, as DuckDB names it; the simple form as the searched
// one.
std::string CaseName(const sql::CaseExpr& c) {
  std::string out = "CASE ";
  for (const sql::CaseBranch& branch : c.branches) {
    const std::string when = c.operand.has_value() ? std::format("({} = {})", ExprName(**c.operand),
                                                                 ExprName(*branch.when))
                                                   : ExprName(*branch.when);
    out += std::format(" WHEN ({}) THEN ({})", when, ExprName(*branch.then));
  }
  return out + " ELSE " + (c.otherwise.has_value() ? ExprName(**c.otherwise) : "NULL") + " END";
}

bool IsComparison(sql::BinaryOp op) {
  switch (op) {
    case sql::BinaryOp::kEq:
    case sql::BinaryOp::kNe:
    case sql::BinaryOp::kLt:
    case sql::BinaryOp::kLe:
    case sql::BinaryOp::kGt:
    case sql::BinaryOp::kGe:
      return true;
    default:
      return false;
  }
}

// The operator as DuckDB names it (<> is !=).
std::string_view OperatorName(sql::BinaryOp op) {
  return op == sql::BinaryOp::kNe ? "!=" : sql::ToString(op);
}

// The comparison that NOT of `op` is (DuckDB names NOT (a < b) as (a >= b)).
sql::BinaryOp Negated(sql::BinaryOp op) {
  switch (op) {
    case sql::BinaryOp::kEq:
      return sql::BinaryOp::kNe;
    case sql::BinaryOp::kNe:
      return sql::BinaryOp::kEq;
    case sql::BinaryOp::kLt:
      return sql::BinaryOp::kGe;
    case sql::BinaryOp::kLe:
      return sql::BinaryOp::kGt;
    case sql::BinaryOp::kGt:
      return sql::BinaryOp::kLe;
    case sql::BinaryOp::kGe:
      return sql::BinaryOp::kLt;
    default:
      return op;
  }
}

std::string InName(const sql::InExpr& in, bool negated) {
  std::string values;
  for (const sql::Expr& value : in.list) {
    values += (values.empty() ? "" : ", ") + ExprName(value);
  }
  return std::format("({} {}IN ({}))", ExprName(*in.operand), negated ? "NOT " : "", values);
}

// The comparison `expr` is once DuckDB folds its NOTs in (each NOT flips it), or std::nullopt when
// no comparison is under the NOTs.
struct ComparisonUnderNots {
  const sql::Expr* left;
  sql::BinaryOp op;
  const sql::Expr* right;
};
std::optional<ComparisonUnderNots> FoldNots(const sql::Expr& expr) {
  if (const auto* binary = std::get_if<sql::BinaryExpr>(&expr);
      binary != nullptr && IsComparison(binary->op)) {
    return ComparisonUnderNots{.left = &*binary->left, .op = binary->op, .right = &*binary->right};
  }
  if (const auto* unary = std::get_if<sql::UnaryExpr>(&expr);
      unary != nullptr && unary->op == sql::UnaryOp::kNot) {
    if (auto inner = FoldNots(*unary->operand)) {
      inner->op = Negated(inner->op);
      return inner;
    }
  }
  return std::nullopt;
}

// NOT of `operand` as DuckDB names it: over a comparison (through any NOTs) the flipped
// comparison, over IN a NOT IN, else (NOT x).
std::string NotName(const sql::Expr& operand) {
  if (const auto folded = FoldNots(operand)) {
    return std::format("({} {} {})", ExprName(*folded->left), OperatorName(Negated(folded->op)),
                       ExprName(*folded->right));
  }
  if (const auto* in = std::get_if<sql::InExpr>(&operand); in != nullptr && !in->negated) {
    return InName(*in, /*negated=*/true);
  }
  return "(NOT " + ExprName(operand) + ")";
}

// The type of a cast as ToSql writes it: DECIMAL(15, 2).
std::string TypeText(const sql::CastExpr& cast) {
  std::string out = cast.type;
  for (std::size_t i = 0; i < cast.type_params.size(); ++i) {
    out += (i == 0 ? "(" : ", ") + cast.type_params[i];
  }
  return cast.type_params.empty() ? out : out + ")";
}

[[noreturn]] void UnfoldedCast();

// DuckDB's name of an expression of the binder's subset: (a + 1), -(a), sum((a + 1)); conditions
// as ((a = 1) OR (b != 2) OR (c IN (1, 2))).
struct ExprNameOf {
  std::string operator()(const sql::ColumnRef& ref) const { return ArgumentName(ref.name); }
  std::string operator()(const sql::Literal& lit) const {
    if (lit.kind == sql::Literal::Kind::kDate) {
      return "CAST('" + lit.text + "' AS \"DATE\")";
    }
    if (lit.kind == sql::Literal::Kind::kTimestamp) {
      return "CAST('" + lit.text + "' AS TIMESTAMP)";
    }
    return LiteralName(lit);
  }
  std::string operator()(const sql::AggregateCall& call) const { return ResultName(call); }
  std::string operator()(const sql::UnaryExpr& unary) const {
    return unary.op == sql::UnaryOp::kNot ? NotName(*unary.operand)
                                          : "-(" + ExprName(*unary.operand) + ")";
  }
  std::string operator()(const sql::BinaryExpr& binary) const {
    if (binary.op != sql::BinaryOp::kAnd && binary.op != sql::BinaryOp::kOr) {
      return std::format("({} {} {})", ExprName(*binary.left), OperatorName(binary.op),
                         ExprName(*binary.right));
    }
    // A chain of one operator is one list: ((a) OR (b) OR (c)).
    std::vector<const sql::Expr*> chain;
    std::vector<const sql::Expr*> pending{&*binary.right, &*binary.left};
    while (!pending.empty()) {
      const sql::Expr* e = pending.back();
      pending.pop_back();
      const auto* b = std::get_if<sql::BinaryExpr>(e);
      if (b != nullptr && b->op == binary.op) {
        pending.push_back(&*b->right);
        pending.push_back(&*b->left);
      } else {
        chain.push_back(e);
      }
    }
    std::string out;
    for (const sql::Expr* e : chain) {
      out += (out.empty() ? "(" : std::format(" {} ", sql::ToString(binary.op))) + ExprName(*e);
    }
    return out + ")";
  }
  std::string operator()(const sql::LikeExpr& like) const {
    return std::format("({} {} {})", ExprName(*like.operand), like.negated ? "!~~" : "~~",
                       ExprName(*like.pattern));
  }
  std::string operator()(const sql::InExpr& in) const { return InName(in, in.negated); }
  // DuckDB names both negations alike: (NOT (x BETWEEN 1 AND 2)).
  std::string operator()(const sql::BetweenExpr& between) const {
    const std::string name = std::format("({} BETWEEN {} AND {})", ExprName(*between.operand),
                                         ExprName(*between.low), ExprName(*between.high));
    return between.negated ? "(NOT " + name + ")" : name;
  }
  std::string operator()(const sql::FunctionCall& call) const {
    std::string args;
    for (const sql::Expr& arg : call.args) {
      args += (args.empty() ? "" : ", ") + ExprName(arg);
    }
    return AsciiLower(call.name) + "(" + args + ")";
  }
  std::string operator()(const sql::CaseExpr& c) const { return CaseName(c); }
  std::string operator()(const sql::ExtractExpr& e) const {
    return std::format("main.date_part('{}', {})", ExtractFieldName(e.field), ExprName(*e.source));
  }
  std::string operator()(const sql::CastExpr& /*cast*/) const { UnfoldedCast(); }
};

std::string ExprName(const sql::Expr& expr) {
  return std::visit(ExprNameOf{}, static_cast<const sql::ExprNode&>(expr));
}

// count_star(), count(x), count(DISTINCT x), sum(x), avg(x), min(x), max(x), sum((x + 1)).
std::string ResultName(const sql::AggregateCall& call) {
  if (call.kind == sql::AggKind::kCountStar || !call.arg.has_value()) {
    return "count_star()";
  }
  return AsciiLower(ToString(ToPlan(call.kind))) + "(" + (call.distinct ? "DISTINCT " : "") +
         ExprName(**call.arg) + ")";
}

// ---- expressions the binder does not answer (yet): kUnsupported at their first token ----

struct Rejection {
  SourceSpan span;
  std::string message;
};

// The first construct of a value expression, in source order, that is not a column, a literal
// or (where allowed) an aggregate of a column.
std::optional<Rejection> FirstUnsupported(const sql::Expr& expr);

// Why a condition (WHERE, HAVING, CASE WHEN) is not one the binder answers, or std::nullopt.
std::optional<Rejection> RejectCondition(const sql::Expr& expr, bool having);

// Whether the expression reads a column (or, `aggregates`, calls an aggregate): one that does
// neither is a constant.
bool ReadsColumn(const sql::Expr& expr, bool aggregates = false);

// The scalar functions the binder answers: one value argument, the others string literals (the
// pattern and the replacement of regexp_replace, the unit of date_trunc). toDateTime is
// ClickBench's DuckDB macro, epoch_ms(t * 1000).
struct FunctionSpec {
  std::string_view name;  // lower case, as DuckDB names the result
  Function function;
  std::size_t args;
  std::size_t value_arg = 0;  // the value argument; the others are string literals
};

constexpr auto kFunctions = std::to_array<FunctionSpec>({
    {.name = "strlen", .function = Function::kStrlen, .args = 1},
    {.name = "regexp_replace", .function = Function::kRegexpReplace, .args = 3},
    {.name = "todatetime", .function = Function::kEpochMs, .args = 1},
    {.name = "date_trunc", .function = Function::kDateTrunc, .args = 2, .value_arg = 1},
});

std::optional<FunctionSpec> FindFunction(const sql::FunctionCall& call) {
  const std::string name = AsciiLower(call.name);
  const auto* spec = std::ranges::find(kFunctions, name, &FunctionSpec::name);
  return spec == kFunctions.end() ? std::nullopt : std::optional(*spec);
}

struct ReadsColumnOf {
  bool aggregates = false;

  bool operator()(const sql::ColumnRef& /*column*/) const { return true; }
  bool operator()(const sql::Literal& /*lit*/) const { return false; }
  bool operator()(const sql::AggregateCall& /*call*/) const { return aggregates; }
  bool operator()(const sql::UnaryExpr& unary) const {
    return ReadsColumn(*unary.operand, aggregates);
  }
  bool operator()(const sql::BinaryExpr& binary) const {
    return ReadsColumn(*binary.left, aggregates) || ReadsColumn(*binary.right, aggregates);
  }
  bool operator()(const sql::LikeExpr& like) const {
    return ReadsColumn(*like.operand, aggregates) || ReadsColumn(*like.pattern, aggregates);
  }
  bool operator()(const sql::InExpr& in) const { return ReadsColumn(*in.operand, aggregates); }
  bool operator()(const sql::BetweenExpr& between) const {
    return ReadsColumn(*between.operand, aggregates) || ReadsColumn(*between.low, aggregates) ||
           ReadsColumn(*between.high, aggregates);
  }
  bool operator()(const sql::FunctionCall& call) const {
    return std::ranges::any_of(call.args,
                               [this](const sql::Expr& a) { return ReadsColumn(a, aggregates); });
  }
  bool operator()(const sql::CaseExpr& c) const {
    const auto reads = [this](const sql::Expr& e) { return ReadsColumn(e, aggregates); };
    return (c.operand.has_value() && reads(**c.operand)) ||
           (c.otherwise.has_value() && reads(**c.otherwise)) ||
           std::ranges::any_of(c.branches, [&](const sql::CaseBranch& b) {
             return reads(*b.when) || reads(*b.then);
           });
  }
  bool operator()(const sql::ExtractExpr& e) const { return ReadsColumn(*e.source, aggregates); }
  bool operator()(const sql::CastExpr& cast) const {
    return ReadsColumn(*cast.operand, aggregates);
  }
};

bool ReadsColumn(const sql::Expr& expr, bool aggregates) {
  return std::visit(ReadsColumnOf{.aggregates = aggregates},
                    static_cast<const sql::ExprNode&>(expr));
}

std::optional<Rejection> FirstUnsupportedIn(const std::vector<sql::Expr>& exprs) {
  for (const sql::Expr& e : exprs) {
    if (auto r = FirstUnsupported(e)) {
      return r;
    }
  }
  return std::nullopt;
}

constexpr const char* kConditionsOnly =
    " is only supported in conditions (WHERE, HAVING and CASE WHEN)";

struct FirstUnsupportedOf {
  // Qualified names resolve once FROM lists do (J2b in ADR 0022).
  std::optional<Rejection> operator()(const sql::ColumnRef& column) const {
    if (!column.qualifier.empty()) {
      return Rejection{.span = column.span,
                       .message = "qualified column names (t.x) are not supported"};
    }
    return std::nullopt;
  }
  std::optional<Rejection> operator()(const sql::Literal& /*lit*/) const { return std::nullopt; }
  std::optional<Rejection> operator()(const sql::AggregateCall& call) const {
    if (!call.arg.has_value()) {
      return std::nullopt;
    }
    if (const sql::ColumnRef* column = call.arg_column()) {
      return (*this)(*column);
    }
    const sql::Expr& arg = **call.arg;
    if (auto r = FirstUnsupported(arg)) {
      return r;
    }
    if (!ReadsColumn(arg)) {
      return Rejection{.span = arg.span(),
                       .message = std::string("constant aggregate arguments are not supported") +
                                  (call.kind == sql::AggKind::kCount ? " (use COUNT(*))" : "")};
    }
    return std::nullopt;
  }
  std::optional<Rejection> operator()(const sql::UnaryExpr& unary) const {
    if (unary.op == sql::UnaryOp::kNegate) {
      return FirstUnsupported(*unary.operand);
    }
    return Rejection{.span = unary.op_span, .message = std::string("NOT") + kConditionsOnly};
  }
  std::optional<Rejection> operator()(const sql::BinaryExpr& binary) const {
    if (auto r = FirstUnsupported(*binary.left)) {
      return r;
    }
    std::string message;
    switch (binary.op) {
      case sql::BinaryOp::kAdd:
      case sql::BinaryOp::kSubtract:
      case sql::BinaryOp::kMultiply:
      case sql::BinaryOp::kDivide:
      case sql::BinaryOp::kIntegerDivide:
      case sql::BinaryOp::kModulo:
        return FirstUnsupported(*binary.right);
      case sql::BinaryOp::kAnd:
        message = std::string("AND") + kConditionsOnly;
        break;
      case sql::BinaryOp::kOr:
        message = std::string("OR") + kConditionsOnly;
        break;
      default:
        message = "comparisons are only supported in conditions (WHERE, HAVING and CASE WHEN)";
        break;
    }
    return Rejection{.span = binary.op_span, .message = std::move(message)};
  }
  std::optional<Rejection> operator()(const sql::LikeExpr& like) const {
    if (auto r = FirstUnsupported(*like.operand)) {
      return r;
    }
    return Rejection{.span = like.op_span, .message = std::string("LIKE") + kConditionsOnly};
  }
  std::optional<Rejection> operator()(const sql::InExpr& in) const {
    if (auto r = FirstUnsupported(*in.operand)) {
      return r;
    }
    return Rejection{.span = in.op_span, .message = std::string("IN") + kConditionsOnly};
  }
  std::optional<Rejection> operator()(const sql::BetweenExpr& between) const {
    if (auto r = FirstUnsupported(*between.operand)) {
      return r;
    }
    return Rejection{.span = between.op_span, .message = std::string("BETWEEN") + kConditionsOnly};
  }
  std::optional<Rejection> operator()(const sql::FunctionCall& call) const {
    const std::optional<FunctionSpec> spec = FindFunction(call);
    if (!spec.has_value()) {
      return Rejection{.span = call.name_span,
                       .message = std::format("function {}() is not supported (only COUNT, SUM, "
                                              "AVG, MIN, MAX, STRLEN, REGEXP_REPLACE, TODATETIME "
                                              "and DATE_TRUNC)",
                                              Clip(call.name))};
    }
    if (spec->function == Function::kRegexpReplace && call.args.size() == 4) {
      // DuckDB's options argument ('g', 'i', ...): valid there, not here.
      return Rejection{.span = call.args[3].span(),
                       .message =
                           "regexp_replace() with options (a fourth argument) is not "
                           "supported"};
    }
    if (call.args.size() != spec->args) {
      return std::nullopt;  // a bind error (the binder reports the arity), as in DuckDB
    }
    for (std::size_t i = 0; i < call.args.size(); ++i) {
      if (i != spec->value_arg && !std::holds_alternative<sql::Literal>(call.args[i])) {
        return Rejection{
            .span = call.args[i].span(),
            .message = spec->value_arg == 0
                           ? std::format("the arguments of {}() after the first must be string "
                                         "literals",
                                         spec->name)
                           : std::format("the unit of {}() must be a string literal", spec->name)};
      }
      if (const auto* unit = std::get_if<sql::Literal>(&call.args[i]);
          spec->function == Function::kDateTrunc && i == 0 && unit != nullptr &&
          unit->kind == sql::Literal::Kind::kString && !IsTruncUnit(unit->text)) {
        return Rejection{.span = unit->span,
                         .message = std::format("date_trunc() with the unit '{}' is not supported "
                                                "(docs/sql-subset.md lists the units)",
                                                Clip(unit->text))};
      }
      // The evaluator anchors the pattern as ^(\C*?)(pattern) and shifts the replacement's groups
      // by two (exec/compute.cc): \8 and \9 would pass RE2's \9, and \Q would quote the ')'.
      const auto* lit = std::get_if<sql::Literal>(&call.args[i]);
      if (spec->function == Function::kRegexpReplace && i > 0 && lit != nullptr) {
        const std::string_view escapes = i == 1 ? "Q" : "89";
        for (std::size_t j = 0; j + 1 < lit->text.size(); ++j) {
          if (lit->text[j] != '\\') {
            continue;
          }
          if (escapes.contains(lit->text[j + 1])) {
            return Rejection{
                .span = call.args[i].span(),
                .message = std::format("regexp_replace() with \\{} in the {} is not "
                                       "supported",
                                       lit->text[j + 1], i == 1 ? "pattern" : "replacement")};
          }
          ++j;  // the escaped character
        }
      }
      if (auto r = FirstUnsupported(call.args[i])) {
        return r;
      }
    }
    return std::nullopt;
  }
  // CASE: its values as value expressions, its conditions as those of HAVING (an aggregate is an
  // operand; the binder rejects it where the scope has none); CASE x WHEN v as x = v.
  std::optional<Rejection> operator()(const sql::CaseExpr& c) const {
    if (c.operand.has_value()) {
      if (auto r = FirstUnsupported(**c.operand)) {
        return r;
      }
    }
    for (const sql::CaseBranch& branch : c.branches) {
      if (c.operand.has_value()) {
        const sql::Expr equal(sql::BinaryExpr{.op = sql::BinaryOp::kEq,
                                              .left = sql::Box<sql::Expr>(**c.operand),
                                              .right = sql::Box<sql::Expr>(*branch.when),
                                              .op_span = branch.when->span(),
                                              .span = branch.when->span()});
        if (auto r = RejectCondition(equal, /*having=*/true)) {
          return r;
        }
      } else if (auto r = RejectCondition(*branch.when, /*having=*/true)) {
        return r;
      }
      if (auto r = FirstUnsupported(*branch.then)) {
        return r;
      }
    }
    if (c.otherwise.has_value()) {
      return FirstUnsupported(**c.otherwise);
    }
    return std::nullopt;
  }
  std::optional<Rejection> operator()(const sql::ExtractExpr& e) const {
    if (const auto part = FindDatePart(e.field); !part.has_value() || part->field.empty()) {
      return Rejection{.span = e.field_span,
                       .message = std::format("EXTRACT of {} is not supported (docs/sql-subset.md "
                                              "lists the fields)",
                                              Clip(e.field))};
    }
    return FirstUnsupported(*e.source);
  }
  // Every cast that FoldDateCasts left: all but CAST('YYYY-MM-DD' AS DATE), in source order.
  std::optional<Rejection> operator()(const sql::CastExpr& cast) const {
    if (cast.try_cast) {
      return Rejection{.span = cast.op_span, .message = "TRY_CAST is not supported"};
    }
    if (auto r = FirstUnsupported(*cast.operand)) {
      return r;
    }
    if (cast.type != "DATE" || !cast.type_params.empty()) {
      return Rejection{.span = cast.type_span,
                       .message = std::format("CAST to {} is not supported (only a string literal "
                                              "cast to DATE is)",
                                              Clip(TypeText(cast)))};
    }
    return Rejection{.span = cast.operand->span(),
                     .message =
                         "CAST to DATE is only supported for a string literal "
                         "('YYYY-MM-DD')"};
  }
};

std::optional<Rejection> FirstUnsupported(const sql::Expr& expr) {
  return std::visit(FirstUnsupportedOf{}, static_cast<const sql::ExprNode&>(expr));
}

arrow::Status Reject(const Rejection& r) {
  return UnsupportedError(r.message + std::string(sql::kUnsupportedHint), r.span);
}

// A value expression (a select, GROUP BY or ORDER BY item, an aggregate argument): kUnsupported
// unless it is a column, a literal or an aggregate of a column.
arrow::Status CheckValue(const sql::Expr& expr) {
  if (auto r = FirstUnsupported(expr)) {
    return Reject(*r);
  }
  return arrow::Status::OK();
}

// operand BETWEEN low AND high as its two comparisons, operand >= low and operand <= high, each
// spanning the BETWEEN (a negation stays with the caller).
std::array<sql::Expr, 2> BetweenComparisons(const sql::BetweenExpr& between) {
  const auto comparison = [&](sql::BinaryOp op, const sql::Expr& bound) {
    return sql::Expr(sql::BinaryExpr{.op = op,
                                     .left = sql::Box<sql::Expr>(*between.operand),
                                     .right = sql::Box<sql::Expr>(bound),
                                     .op_span = between.op_span,
                                     .span = between.span});
  };
  return {comparison(sql::BinaryOp::kGe, *between.low),
          comparison(sql::BinaryOp::kLe, *between.high)};
}

// Whether `expr` is a constant integer expression the binder folds (FoldConstant), overflowing or
// not.
bool IsConstantInteger(const sql::Expr& expr);

constexpr std::string_view kOtherConditions =
    "conditions other than comparisons, [NOT] LIKE, [NOT] IN, [NOT] BETWEEN, AND, OR and NOT are "
    "not supported";
constexpr std::string_view kThisCondition = "this condition is not supported";

// Why a WHERE (or, with `having`, HAVING) condition is not one the binder answers, or std::nullopt
// when it is (AsComparison / AsHavingComparison accept it, or a conjunction of such).
struct RejectConditionOf {
  const sql::Expr& expr;  // the condition
  bool having = false;

  // An operand reads a column (in HAVING, or calls an aggregate); the other side of a comparison
  // is then an operand too, or a literal.
  bool IsOperand(const sql::Expr& e) const { return ReadsColumn(e, having); }
  std::string_view OperandKind() const { return having ? "a column or an aggregate" : "a column"; }

  // `expr` is no condition the binder answers: the first construct in it that is not supported
  // anywhere, else `message`, at the whole of `expr`.
  std::optional<Rejection> NotACondition(std::string_view message) const {
    if (auto r = FirstUnsupported(expr)) {
      return r;
    }
    return Rejection{.span = expr.span(), .message = std::string(message)};
  }

  std::optional<Rejection> operator()(const sql::ColumnRef& /*column*/) const {
    return NotACondition(kOtherConditions);
  }
  std::optional<Rejection> operator()(const sql::Literal& /*lit*/) const {
    return NotACondition(kOtherConditions);
  }
  std::optional<Rejection> operator()(const sql::AggregateCall& /*call*/) const {
    return NotACondition(kOtherConditions);
  }
  std::optional<Rejection> operator()(const sql::UnaryExpr& unary) const {
    if (unary.op == sql::UnaryOp::kNot) {
      return RejectCondition(*unary.operand, having);
    }
    return NotACondition(kOtherConditions);
  }
  std::optional<Rejection> operator()(const sql::BinaryExpr& binary) const {
    switch (binary.op) {
      case sql::BinaryOp::kAnd:
      case sql::BinaryOp::kOr:
        if (auto r = RejectCondition(*binary.left, having)) {
          return r;
        }
        return RejectCondition(*binary.right, having);
      case sql::BinaryOp::kEq:
      case sql::BinaryOp::kNe:
      case sql::BinaryOp::kLt:
      case sql::BinaryOp::kLe:
      case sql::BinaryOp::kGt:
      case sql::BinaryOp::kGe: {
        if (auto r = FirstUnsupported(*binary.left)) {
          return r;
        }
        if (auto r = FirstUnsupported(*binary.right)) {
          return r;
        }
        const bool left = IsOperand(*binary.left);
        const bool right = IsOperand(*binary.right);
        if (!left && !right) {
          return Rejection{.span = binary.right->span(),
                           .message = "comparisons between two literals are not supported"};
        }
        for (const sql::Expr* side : {&*binary.left, &*binary.right}) {
          if (!IsOperand(*side) && !std::holds_alternative<sql::Literal>(*side) &&
              !IsConstantInteger(*side)) {
            return Rejection{.span = side->span(),
                             .message =
                                 "a constant expression in a comparison is not supported "
                                 "(only integer literals under +, - and * are folded)"};
          }
        }
        return std::nullopt;
      }
      default:  // arithmetic
        break;
    }
    return NotACondition(kOtherConditions);
  }
  std::optional<Rejection> operator()(const sql::LikeExpr& like) const {
    if (auto r = FirstUnsupported(*like.operand)) {
      return r;
    }
    if (!IsOperand(*like.operand)) {
      return Rejection{.span = like.operand->span(),
                       .message = std::format("LIKE needs {} on the left", OperandKind())};
    }
    if (std::holds_alternative<sql::Literal>(*like.pattern)) {
      return std::nullopt;
    }
    if (auto r = FirstUnsupported(*like.pattern)) {
      return r;
    }
    return Rejection{.span = like.pattern->span(),
                     .message =
                         "LIKE with a column or an aggregate as the pattern is not "
                         "supported"};
  }
  std::optional<Rejection> operator()(const sql::InExpr& in) const {
    if (auto r = FirstUnsupported(*in.operand)) {
      return r;
    }
    if (!IsOperand(*in.operand)) {
      return Rejection{.span = in.operand->span(),
                       .message = std::format("IN needs {} on the left", OperandKind())};
    }
    if (auto r = FirstUnsupportedIn(in.list)) {
      return r;
    }
    for (const sql::Expr& value : in.list) {
      if (!std::holds_alternative<sql::Literal>(value)) {
        return Rejection{.span = value.span(),
                         .message = "only literals are supported in an IN list"};
      }
    }
    return std::nullopt;
  }
  // As the two comparisons it means: operand >= low AND operand <= high.
  std::optional<Rejection> operator()(const sql::BetweenExpr& between) const {
    for (const sql::Expr& comparison : BetweenComparisons(between)) {
      if (auto r = RejectCondition(comparison, having)) {
        return r;
      }
    }
    return std::nullopt;
  }
  std::optional<Rejection> operator()(const sql::FunctionCall& /*call*/) const {
    return NotACondition(kThisCondition);
  }
  std::optional<Rejection> operator()(const sql::CaseExpr& /*c*/) const {
    return NotACondition(kThisCondition);
  }
  std::optional<Rejection> operator()(const sql::ExtractExpr& /*e*/) const {
    return NotACondition(kThisCondition);
  }
  std::optional<Rejection> operator()(const sql::CastExpr& /*cast*/) const {
    return NotACondition(kThisCondition);
  }
};

std::optional<Rejection> RejectCondition(const sql::Expr& expr, bool having) {
  return std::visit(RejectConditionOf{.expr = expr, .having = having},
                    static_cast<const sql::ExprNode&>(expr));
}

// The conjuncts of a WHERE or HAVING predicate, with parenthesized AND chains flattened.
void Conjuncts(const sql::Expr& expr, std::vector<const sql::Expr*>& out) {
  const auto* binary = std::get_if<sql::BinaryExpr>(&expr);
  if (binary != nullptr && binary->op == sql::BinaryOp::kAnd) {
    Conjuncts(*binary->left, out);
    Conjuncts(*binary->right, out);
    return;
  }
  out.push_back(&expr);
}

std::vector<const sql::Expr*> Conjuncts(const std::vector<sql::Expr>& predicate) {
  std::vector<const sql::Expr*> out;
  for (const sql::Expr& expr : predicate) {
    Conjuncts(expr, out);
  }
  return out;
}

arrow::Result<std::shared_ptr<Table>> ResolveTable(const sql::TableRef& ref,
                                                   const Catalog& catalog) {
  if (ref.kind == sql::TableRef::Kind::kPath) {
    auto table = catalog.OpenPath(ref.name);
    if (!table.ok()) {
      if (table.status().IsNotImplemented()) {
        return UnsupportedError(table.status().message(), ref.span);
      }
      return table.status();  // I/O errors keep their code (exit 3)
    }
    return table;
  }
  auto table = catalog.Find(ref.name);
  if (!table) {
    return BindError("table '" + ref.name + "' does not exist", ref.span);
  }
  return table;
}

// Mints the ids of a query's columns (plan::ColumnId): from 1, in binding order.
class ColumnIdSource {
 public:
  ColumnId Next() { return ColumnId{++last_}; }

 private:
  std::uint32_t last_ = 0;
};

// Resolves column names of one table: ASCII case-insensitively, quoted names too (DuckDB). Each
// field is one column, with its own id.
class Columns {
 public:
  Columns(const arrow::Schema& schema, ColumnIdSource& ids) : schema_(schema) {
    lower_.reserve(Narrow<std::size_t>(schema.num_fields()));
    ids_.reserve(Narrow<std::size_t>(schema.num_fields()));
    for (const auto& field : schema.fields()) {
      lower_.push_back(AsciiLower(field->name()));
      ids_.push_back(ids.Next());
    }
  }

  // The column of field `index`.
  [[nodiscard]] ColumnId Id(int index) const { return ids_.at(Narrow<std::size_t>(index)); }

  // The field that column `id` is, if it is one (and not a computed column).
  [[nodiscard]] std::optional<int> FieldOf(ColumnId id) const {
    const auto it = std::ranges::find(ids_, id);
    if (it == ids_.end()) {
      return std::nullopt;
    }
    return Narrow<int>(it - ids_.begin());
  }

  // The column of the field that `ref` names, by its id (plan::ResolvePositions sets its position
  // at the end).
  [[nodiscard]] arrow::Result<BoundColumn> Resolve(const sql::ColumnRef& ref) const {
    // CheckSupported rejected every qualified name, except in the arguments of a call with the
    // wrong number of them, which BindFunction rejects before it binds an argument.
    ANTB1_CHECK(ref.qualifier.empty());
    const std::string wanted = AsciiLower(ref.name);
    std::optional<int> found;
    for (std::size_t i = 0; i < lower_.size(); ++i) {
      if (lower_[i] != wanted) {
        continue;
      }
      const int index = Narrow<int>(i);
      if (found.has_value()) {
        return BindError(
            std::format("column name '{}' is ambiguous: it matches the columns '{}' "
                        "and '{}', which differ only in case",
                        ref.name, schema_.field(*found)->name(), schema_.field(index)->name()),
            ref.span);
      }
      found = index;
    }
    if (!found.has_value()) {
      return BindError(std::format("column '{}' does not exist", ref.name), ref.span);
    }
    return Field(*found, ref.span);
  }

  // Field `index`, or kUnsupported (at `span`) if its type is not supported.
  [[nodiscard]] arrow::Result<BoundColumn> Field(int index, SourceSpan span) const {
    const auto& field = schema_.field(index);
    auto type = FromArrow(*field->type());
    if (!type.ok()) {
      return UnsupportedError(std::format("column '{}' has the unsupported type {}", field->name(),
                                          field->type()->ToString()),
                              span);
    }
    return BoundColumn{.id = Id(index), .name = field->name(), .type = *type};
  }

 private:
  const arrow::Schema& schema_;
  std::vector<std::string> lower_;
  std::vector<ColumnId> ids_;  // per field
};

arrow::Result<LogicalType> AggregateType(const sql::AggregateCall& call, const BoundColumn& arg) {
  switch (call.kind) {
    case sql::AggKind::kCountStar:
    case sql::AggKind::kCount:
      return LogicalType::kBigInt;
    case sql::AggKind::kSum:
    case sql::AggKind::kAvg:
      // DuckDB's AVG of a DATE or TIMESTAMP is a TIMESTAMP.
      if (call.kind == sql::AggKind::kAvg &&
          (arg.type == LogicalType::kDate || arg.type == LogicalType::kTimestamp)) {
        return LogicalType::kTimestamp;
      }
      // DuckDB's SUM of a DECIMAL(p,s) is DECIMAL(38,s), exact; its AVG is DOUBLE (ADR 0021).
      if (arg.type == LogicalType::kDecimal) {
        return call.kind == sql::AggKind::kSum
                   ? LogicalType::Decimal(LogicalType::kMaxDecimalWidth, arg.type.scale())
                   : LogicalType::kDouble;
      }
      if (!IsNumeric(arg.type)) {
        return BindError(std::format("{} needs a numeric column, but '{}' is {}",
                                     ToString(ToPlan(call.kind)), arg.name, ToString(arg.type)),
                         call.span);
      }
      // Integer SUM is exact in 128 bits (HUGEINT), like DuckDB; AVG is always DOUBLE.
      return call.kind == sql::AggKind::kSum && IsInteger(arg.type) ? LogicalType::kHugeInt
                                                                    : LogicalType::kDouble;
    case sql::AggKind::kMin:
    case sql::AggKind::kMax:
      break;
  }
  return arg.type;
}

constexpr std::string_view kInvalidTimestamp = "invalid timestamp";
constexpr std::string_view kTimestampForm = "YYYY-MM-DD[ HH:MM[:SS[.fraction]]]";

std::string_view LiteralKind(const sql::Literal& lit) {
  switch (lit.kind) {
    case sql::Literal::Kind::kInteger:
    case sql::Literal::Kind::kDecimal:
      return "a number";
    case sql::Literal::Kind::kString:
      return "a string";
    case sql::Literal::Kind::kDate:
      return "a DATE literal";
    case sql::Literal::Kind::kTimestamp:
      return "a TIMESTAMP literal";
  }
  return "a literal";
}

// `column [NOT] LIKE 'pattern'`: a VARCHAR column and a string pattern (DuckDB rejects LIKE on
// other types). A pattern of only % holds for every value: LIKE folds to IS NOT NULL, NOT LIKE to
// FALSE.
arrow::Result<Predicate> BindLike(const sql::Comparison& cmp, const BoundColumn& column) {
  const sql::Literal& lit = cmp.literal;
  const bool negated = cmp.op == sql::CompareOp::kNotLike;
  if (column.type != LogicalType::kVarchar) {
    return BindError(std::format("{} needs a VARCHAR column, but '{}' is {}",
                                 negated ? "NOT LIKE" : "LIKE", column.name, ToString(column.type)),
                     cmp.column.span);
  }
  if (lit.kind != sql::Literal::Kind::kString) {
    return BindError(std::format("the pattern of {} must be a string literal ('...')",
                                 negated ? "NOT LIKE" : "LIKE"),
                     lit.span);
  }
  Predicate p{.kind = negated ? Predicate::Kind::kNotLike : Predicate::Kind::kLike,
              .column = column,
              .op = CompareOp::kEq,
              .constant = Constant{.type = LogicalType::kVarchar, .value = lit.text},
              .span = cmp.span};
  if (!lit.text.empty() && std::ranges::all_of(lit.text, [](char c) { return c == '%'; })) {
    p.kind = negated ? Predicate::Kind::kFalse : Predicate::Kind::kIsNotNull;
    if (negated) {
      p.column.reset();  // no row passes, whatever the column holds
    }
  }
  return p;
}

arrow::Result<Predicate> BindEquality(const sql::Comparison& cmp, const BoundColumn& column,
                                      bool stored_as_float, bool as_double = false);

// `column [NOT] IN (v1, ...)`: each value is bound as `column = v` (the same typing and exact
// folding); a value no column value can equal is dropped. With no value left, IN is FALSE and
// NOT IN is IS NOT NULL (NULL still rejects the row). DuckDB gives the list one type: with a
// number it types as DOUBLE every number is a double, so each value is then bound as that DOUBLE
// would be. A DECIMAL column then compares in DOUBLE with every value (ADR 0021 rule 11), and the
// list is DOUBLE for any number DuckDB types so (DuckDbTypesAsDouble). Other columns look for an
// exponent or a decimal of more than 38 digits: an integer column folds the nearest doubles
// exactly (divergence D7), and a FLOAT column takes no FLOAT literals.
arrow::Result<Predicate> BindIn(const sql::Comparison& cmp, const BoundColumn& column,
                                bool stored_as_float) {
  const bool negated = cmp.op == sql::CompareOp::kNotIn;
  Predicate p{.kind = negated ? Predicate::Kind::kNotIn : Predicate::Kind::kIn,
              .column = column,
              .op = CompareOp::kEq,
              .constant = {},
              .values = {},
              .span = cmp.span};
  const bool decimal = column.type == LogicalType::kDecimal;
  const bool as_double = std::ranges::any_of(cmp.list, [decimal](const sql::Literal& value) {
    if (value.kind != sql::Literal::Kind::kInteger && value.kind != sql::Literal::Kind::kDecimal) {
      return false;
    }
    return decimal ? DuckDbTypesAsDouble(value.text, value.negative)
                   : IsApproximateNumber(value.text);
  });
  for (const sql::Literal& value : cmp.list) {
    const sql::Comparison equal{.column = cmp.column,
                                .op = sql::CompareOp::kEq,
                                .literal = value,
                                .list = {},
                                .span = cmp.span};
    ARROW_ASSIGN_OR_RAISE(const Predicate one,
                          BindEquality(equal, column, stored_as_float, as_double));
    if (one.kind == Predicate::Kind::kCompare) {
      p.values.push_back(one.constant);
    }
  }
  if (p.values.empty()) {
    p.kind = negated ? Predicate::Kind::kIsNotNull : Predicate::Kind::kFalse;
    if (!negated) {
      p.column.reset();  // no row passes, whatever the column holds
    }
  }
  return p;
}

// `stored_as_float`: the column holds FLOAT values (widened to DOUBLE), which DuckDB compares in
// FLOAT.
arrow::Result<Predicate> BindComparison(const sql::Comparison& cmp, const BoundColumn& column,
                                        bool stored_as_float) {
  if (cmp.op == sql::CompareOp::kLike || cmp.op == sql::CompareOp::kNotLike) {
    return BindLike(cmp, column);
  }
  if (cmp.op == sql::CompareOp::kIn || cmp.op == sql::CompareOp::kNotIn) {
    return BindIn(cmp, column, stored_as_float);
  }
  return BindEquality(cmp, column, stored_as_float);
}

// `column <op> literal`, with the literal folded exactly into the column's type. `as_double`: a
// number is read as DuckDB reads a DOUBLE-typed one, even when it is not written that way (an IN
// list with such a number). A DECIMAL column compared in DOUBLE keeps a DOUBLE constant, which the
// executor compares with the column's values converted as DuckDB converts them.
arrow::Result<Predicate> BindEquality(const sql::Comparison& cmp, const BoundColumn& column,
                                      bool stored_as_float, bool as_double) {
  const sql::Literal& lit = cmp.literal;
  const auto mismatch = [&](std::string_view hint) {
    return BindError(std::format("cannot compare {} column '{}' with {}; {}", ToString(column.type),
                                 column.name, LiteralKind(lit), hint),
                     lit.span);
  };
  const bool number =
      lit.kind == sql::Literal::Kind::kInteger || lit.kind == sql::Literal::Kind::kDecimal;
  Predicate p{.kind = Predicate::Kind::kCompare,
              .column = column,
              .op = ToPlan(cmp.op),
              .constant = Constant{.type = column.type, .value = Int128{0}},
              .span = cmp.span};
  switch (column.type.id()) {
    case LogicalType::kSmallInt:
    case LogicalType::kInteger:
    case LogicalType::kBigInt:
    case LogicalType::kUSmallInt:
    case LogicalType::kHugeInt: {
      if (!number) {
        return mismatch("write a number without quotes");
      }
      std::optional<ExactNumber> exact;
      if (as_double || IsApproximateNumber(lit.text)) {
        // DuckDB reads it as a DOUBLE: compare with the nearest double, exactly (divergence D7).
        const auto value = ParseDoubleLiteral(lit.text, lit.negative);
        exact = value.has_value() ? std::optional(ExactNumberOf(*value)) : std::nullopt;
      } else {
        exact = ParseExactNumber(lit.text, lit.negative);
      }
      if (!exact.has_value()) {
        return BindError("invalid number " + Clip(lit.text), lit.span);
      }
      const FoldedComparison folded = FoldIntegerComparison(p.op, *exact, RangeOf(column.type));
      p.kind = folded.kind;
      p.op = folded.op;
      p.constant.value = folded.value;
      if (folded.kind == Predicate::Kind::kFalse) {
        p.column.reset();  // no row passes, whatever the column holds
      }
      return p;
    }
    case LogicalType::kDecimal: {
      if (!number) {
        return mismatch("write a number without quotes");
      }
      // A number DuckDB types as DOUBLE (an exponent, a decimal of more than 38 digits, an integer
      // outside -2^127 to 2^128 - 1), or any number of an IN list with one, is compared in DOUBLE,
      // not folded: the literal as DuckDB converts it to DOUBLE, the column as DuckDB converts a
      // DECIMAL (ADR 0021 rules 8 and 11), so `p = 1e-1` holds for 0.10.
      if (as_double || DuckDbTypesAsDouble(lit.text, lit.negative)) {
        const auto value = DuckDbDoubleOf(lit.text, lit.negative);
        if (!value.has_value()) {
          return BindError("invalid number " + Clip(lit.text), lit.span);
        }
        p.constant = Constant{.type = LogicalType::kDouble, .value = *value};
        return p;
      }
      // The literal in the column's scale, folded exactly like an integer literal into an integer
      // column: 1.5 in DECIMAL(15,2) is 150, and x <= 12.345 is x <= 12.34 (ADR 0021 rule 11). An
      // integer literal beyond the type folds to a constant, where DuckDB fails to cast one of more
      // than 38 - s digits to its capped common type DECIMAL(38,s) (divergence D13).
      const auto exact = ParseExactNumber(lit.text, lit.negative, column.type.scale());
      if (!exact.has_value()) {
        return BindError("invalid number " + Clip(lit.text), lit.span);
      }
      const FoldedComparison folded = FoldIntegerComparison(p.op, *exact, RangeOf(column.type));
      p.kind = folded.kind;
      p.op = folded.op;
      p.constant.value = folded.value;
      if (folded.kind == Predicate::Kind::kFalse) {
        p.column.reset();
      }
      return p;
    }
    case LogicalType::kDouble: {
      if (!number) {
        return mismatch("write a number without quotes");
      }
      // Rounded to the nearest double, as DuckDB compares a DOUBLE column with a number; a decimal
      // literal is DuckDB's DECIMAL, converted as DuckDB converts one (ADR 0021 rule 8).
      const auto value = ParseDoubleLiteral(lit.text, lit.negative);
      if (!value.has_value()) {
        return BindError("invalid number " + Clip(lit.text), lit.span);
      }
      p.constant.value = *value;
      if (const auto decimal = ParseDecimalLiteral(lit.text, lit.negative)) {
        p.constant.value =
            DuckDbDecimalToDouble(decimal->unscaled, decimal->type.width(), decimal->type.scale());
      }
      // A FLOAT column: DuckDB casts an integer or DECIMAL literal to FLOAT and compares in FLOAT.
      // Widening that float to double is exact and keeps the order, so comparing the widened
      // column with it gives DuckDB's answer.
      if (!as_double && stored_as_float) {
        if (const auto f = DuckDbFloatOf(lit.text, lit.negative)) {
          p.constant.value = static_cast<double>(*f);
        }
      }
      return p;
    }
    case LogicalType::kVarchar:
      if (lit.kind != sql::Literal::Kind::kString) {
        return mismatch("write a string literal ('...')");
      }
      p.constant.value = lit.text;
      return p;
    case LogicalType::kBoolean:
      return UnsupportedError("comparing a condition is not supported", cmp.span);
    case LogicalType::kTimestamp: {
      // A string or TIMESTAMP literal is the timestamp it spells, a DATE literal its midnight (as
      // DuckDB casts them); compared exactly.
      if (number) {
        return mismatch("write a timestamp as TIMESTAMP 'YYYY-MM-DD HH:MM:SS'");
      }
      std::optional<int64_t> micros;
      if (lit.kind == sql::Literal::Kind::kDate) {
        if (const auto days = ParseDate(lit.text)) {
          micros = int64_t{*days} * 86'400'000'000;
        }
      } else {
        micros = ParseTimestamp(lit.text);
      }
      if (!micros.has_value()) {
        return BindError(
            std::string(kInvalidTimestamp) + " '" + Clip(lit.text) + "': expected " +
                std::string(lit.kind == sql::Literal::Kind::kDate ? "YYYY-MM-DD" : kTimestampForm),
            lit.span);
      }
      p.constant.value = Int128{*micros};
      return p;
    }
    case LogicalType::kDate: {
      if (number) {
        return mismatch("write a date as DATE 'YYYY-MM-DD'");
      }
      if (lit.kind == sql::Literal::Kind::kTimestamp) {
        return UnsupportedError(std::format("comparing the DATE '{}' with a TIMESTAMP literal is "
                                            "not supported",
                                            Clip(column.name)),
                                lit.span);
      }
      const auto days = ParseDate(lit.text);
      if (!days.has_value()) {
        return BindError("invalid date '" + Clip(lit.text) + "': expected YYYY-MM-DD", lit.span);
      }
      p.constant.value = Int128{*days};
      return p;
    }
  }
  return p;
}

// What a select item is: a plain column, an aggregate call, a constant or any other expression.
enum class ItemKind : std::uint8_t { kColumn, kAggregate, kConstant, kExpression };

struct SelectList {
  std::vector<BoundColumn> columns;       // SELECT * or plain columns, in select order
  std::vector<AggregateCall> aggregates;  // aggregates, in select order (hidden ones appended)
  std::vector<Constant> constants;        // constants, in select order
  std::vector<const sql::Expr*> exprs;    // other expressions, in select order
  std::vector<BoundColumn> expr_columns;  // per entry of `exprs`: its column (once bound)
  std::vector<OutputColumn> output;
  // Per output column: its kind and an index into `columns`, `aggregates`, `constants` or `exprs`.
  std::vector<std::pair<ItemKind, std::size_t>> items;
  std::vector<SourceSpan> column_spans;     // per entry of `columns`: what the error points at
  std::vector<std::string> column_written;  // per entry of `columns`: the name as written
  std::vector<std::optional<std::string>> aliases;  // per output column
  SourceSpan span;
};

// A constant select item with DuckDB's type and result name: an integer is INTEGER, BIGINT or
// HUGEINT by its value (see below) and named by it ("-5"); a string is VARCHAR named with its
// quotes ('it''s'); a date is DATE named CAST('2020-01-01' AS "DATE"); a decimal is
// DECIMAL(digits, fraction digits) named by its value (ADR 0021 rule 3). A number DuckDB types as
// DOUBLE is not supported.
arrow::Result<std::pair<Constant, std::string>> BindConstant(const sql::Literal& lit) {
  switch (lit.kind) {
    case sql::Literal::Kind::kInteger: {
      const auto exact =
          IsApproximateNumber(lit.text) ? std::nullopt : ParseExactNumber(lit.text, lit.negative);
      const IntegerRange hugeint = RangeOf(LogicalType::kHugeInt);
      if (!exact.has_value() || exact->huge || exact->magnitude > hugeint.max) {
        return UnsupportedError(
            "integer constants outside HUGEINT's range (38 digits) are not supported", lit.span);
      }
      // DuckDB types the magnitude as INTEGER when it fits (so -2147483648 is not an INTEGER),
      // else the signed value as BIGINT or HUGEINT, and names the constant by its value (007: 7).
      const Int128 value = exact->negative ? -exact->magnitude : exact->magnitude;
      LogicalType type = LogicalType::kHugeInt;
      const IntegerRange bigint = RangeOf(LogicalType::kBigInt);
      if (exact->magnitude <= RangeOf(LogicalType::kInteger).max) {
        type = LogicalType::kInteger;
      } else if (value >= bigint.min && value <= bigint.max) {
        type = LogicalType::kBigInt;
      }
      return std::pair(Constant{.type = type, .value = value}, Int128ToString(value));
    }
    case sql::Literal::Kind::kDecimal: {
      // DECIMAL(digits, fraction digits) (ADR 0021 rule 3), named by its value.
      const auto decimal = ParseDecimalLiteral(lit.text, lit.negative);
      if (!decimal.has_value()) {
        return UnsupportedError(
            "a number with an exponent or more than 38 digits as a constant is not supported "
            "(DuckDB types it DOUBLE)",
            lit.span);
      }
      return std::pair(Constant{.type = decimal->type, .value = decimal->unscaled},
                       LiteralName(lit));
    }
    case sql::Literal::Kind::kString: {
      std::string name = "'";
      for (const char c : lit.text) {
        name += c;
        if (c == '\'') {
          name += '\'';
        }
      }
      return std::pair(Constant{.type = LogicalType::kVarchar, .value = lit.text}, name + "'");
    }
    case sql::Literal::Kind::kDate: {
      const auto days = ParseDate(lit.text);
      if (!days.has_value()) {
        return BindError("invalid date '" + Clip(lit.text) + "': expected YYYY-MM-DD", lit.span);
      }
      return std::pair(Constant{.type = LogicalType::kDate, .value = Int128{*days}},
                       "CAST('" + lit.text + "' AS \"DATE\")");
    }
    case sql::Literal::Kind::kTimestamp: {
      const auto micros = ParseTimestamp(lit.text);
      if (!micros.has_value()) {
        return BindError(std::string(kInvalidTimestamp) + " '" + Clip(lit.text) + "': expected " +
                             std::string(kTimestampForm),
                         lit.span);
      }
      return std::pair(Constant{.type = LogicalType::kTimestamp, .value = Int128{*micros}},
                       "CAST('" + lit.text + "' AS TIMESTAMP)");
    }
  }
  return UnsupportedError("this constant is not supported", lit.span);
}

// ---- scalar expressions ----

// Whether the expression contains an aggregate call.
bool ContainsAggregate(const sql::Expr& expr);

struct ContainsAggregateOf {
  bool operator()(const sql::ColumnRef& /*column*/) const { return false; }
  bool operator()(const sql::Literal& /*lit*/) const { return false; }
  bool operator()(const sql::AggregateCall& /*call*/) const { return true; }
  bool operator()(const sql::UnaryExpr& unary) const { return ContainsAggregate(*unary.operand); }
  bool operator()(const sql::BinaryExpr& binary) const {
    return ContainsAggregate(*binary.left) || ContainsAggregate(*binary.right);
  }
  // A LIKE pattern and the values of an IN list are literals (RejectCondition).
  bool operator()(const sql::LikeExpr& like) const { return ContainsAggregate(*like.operand); }
  bool operator()(const sql::InExpr& in) const { return ContainsAggregate(*in.operand); }
  bool operator()(const sql::BetweenExpr& between) const {
    return ContainsAggregate(*between.operand) || ContainsAggregate(*between.low) ||
           ContainsAggregate(*between.high);
  }
  bool operator()(const sql::FunctionCall& call) const {
    return std::ranges::any_of(call.args, ContainsAggregate);
  }
  bool operator()(const sql::CaseExpr& c) const {
    return (c.operand.has_value() && ContainsAggregate(**c.operand)) ||
           (c.otherwise.has_value() && ContainsAggregate(**c.otherwise)) ||
           std::ranges::any_of(c.branches, [](const sql::CaseBranch& b) {
             return ContainsAggregate(*b.when) || ContainsAggregate(*b.then);
           });
  }
  bool operator()(const sql::ExtractExpr& e) const { return ContainsAggregate(*e.source); }
  bool operator()(const sql::CastExpr& cast) const { return ContainsAggregate(*cast.operand); }
};

bool ContainsAggregate(const sql::Expr& expr) {
  return std::visit(ContainsAggregateOf{}, static_cast<const sql::ExprNode&>(expr));
}

// An expression of the binder's subset, typed, with what DuckDB's typing needs to know of it.
struct Typed {
  ExprPtr expr;
  bool literal = false;          // an integer or decimal literal: DuckDB types it by its context
  bool decimal = false;          // a decimal literal: DuckDB's DECIMAL
  bool stored_as_float = false;  // a FLOAT column (read as DOUBLE, divergence D11)
};

Typed Leaf(Expr expr, bool stored_as_float = false) {
  return Typed{.expr = std::make_shared<const Expr>(std::move(expr)),
               .literal = false,
               .decimal = false,
               .stored_as_float = stored_as_float};
}

Typed ColumnLeaf(ColumnId id, LogicalType type, std::string name, bool stored_as_float) {
  return Leaf(Expr{.node = ColumnExpr{.id = id}, .type = type, .name = std::move(name)},
              stored_as_float);
}

// The rank of an integer type among the signed ones (USMALLINT: none).
int SignedRank(LogicalType type) {
  switch (type.id()) {
    case LogicalType::kSmallInt:
      return 1;
    case LogicalType::kInteger:
      return 2;
    case LogicalType::kBigInt:
      return 3;
    case LogicalType::kHugeInt:
      return 4;
    default:
      return 0;
  }
}

// The type DuckDB computes two integer operands in (neither of them a literal that fits the
// other's type): the wider signed type, where USMALLINT with SMALLINT is BIGINT and with a wider
// type that type.
LogicalType CombineIntegers(LogicalType a, LogicalType b) {
  if (a == b) {
    return a;
  }
  if (a == LogicalType::kUSmallInt || b == LogicalType::kUSmallInt) {
    const LogicalType other = a == LogicalType::kUSmallInt ? b : a;
    return other == LogicalType::kSmallInt ? LogicalType::kBigInt : other;
  }
  return SignedRank(a) >= SignedRank(b) ? a : b;
}

bool IsSignedInteger(LogicalType t) {
  return t == LogicalType::kSmallInt || t == LogicalType::kInteger || t == LogicalType::kBigInt;
}

// The value of a constant integer expression (integer literals under + - * and unary -), as DuckDB
// folds it; std::nullopt for anything else or on an overflow.
std::optional<Int128> ConstantValue(const sql::Expr& expr) {
  if (const auto* lit = std::get_if<sql::Literal>(&expr)) {
    if (lit->kind != sql::Literal::Kind::kInteger || IsApproximateNumber(lit->text)) {
      return std::nullopt;
    }
    const auto exact = ParseExactNumber(lit->text, lit->negative);
    if (!exact.has_value() || exact->huge) {
      return std::nullopt;
    }
    return exact->negative ? -exact->magnitude : exact->magnitude;
  }
  if (const auto* unary = std::get_if<sql::UnaryExpr>(&expr);
      unary != nullptr && unary->op == sql::UnaryOp::kNegate) {
    const auto v = ConstantValue(*unary->operand);
    Int128 out = 0;
    return v.has_value() && !__builtin_sub_overflow(Int128{0}, *v, &out) ? std::optional(out)
                                                                         : std::nullopt;
  }
  const auto* binary = std::get_if<sql::BinaryExpr>(&expr);
  if (binary == nullptr) {
    return std::nullopt;
  }
  const auto l = ConstantValue(*binary->left);
  const auto r = ConstantValue(*binary->right);
  if (!l.has_value() || !r.has_value()) {
    return std::nullopt;
  }
  Int128 out = 0;
  bool overflow = true;
  switch (binary->op) {
    case sql::BinaryOp::kAdd:
      overflow = __builtin_add_overflow(*l, *r, &out);
      break;
    case sql::BinaryOp::kSubtract:
      overflow = __builtin_sub_overflow(*l, *r, &out);
      break;
    case sql::BinaryOp::kMultiply:
      overflow = __builtin_mul_overflow(*l, *r, &out);
      break;
    default:
      break;
  }
  return overflow ? std::nullopt : std::optional(out);
}

// A constant integer expression (integer literals under unary -, +, - and *) folded as DuckDB
// types it: a literal INTEGER, BIGINT or HUGEINT by its value, an operation in the wider type of
// its operands. std::nullopt for any other expression; an error for a value outside its type.
constexpr std::string_view kHugeIntConstants =
    "integer constants outside HUGEINT's range (38 digits) are not supported";

struct TypedConstant {
  Int128 value = 0;
  LogicalType type = LogicalType::kInteger;
};

std::optional<arrow::Result<TypedConstant>> FoldTyped(const sql::Expr& expr) {
  const auto rank = [](LogicalType t) {
    if (t == LogicalType::kInteger) {
      return 0;
    }
    return t == LogicalType::kBigInt ? 1 : 2;
  };
  if (const auto* lit = std::get_if<sql::Literal>(&expr)) {
    if (lit->kind != sql::Literal::Kind::kInteger || IsApproximateNumber(lit->text)) {
      return std::nullopt;
    }
    const auto exact = ParseExactNumber(lit->text, lit->negative);
    if (!exact.has_value() || exact->fraction) {
      return std::nullopt;
    }
    if (exact->huge || exact->magnitude > RangeOf(LogicalType::kHugeInt).max) {
      return arrow::Result<TypedConstant>(
          UnsupportedError(std::string(kHugeIntConstants), lit->span));
    }
    // As BindConstant: INTEGER by the magnitude, else BIGINT or HUGEINT by the signed value.
    const Int128 value = exact->negative ? -exact->magnitude : exact->magnitude;
    const IntegerRange bigint = RangeOf(LogicalType::kBigInt);
    LogicalType type = LogicalType::kHugeInt;
    if (exact->magnitude <= RangeOf(LogicalType::kInteger).max) {
      type = LogicalType::kInteger;
    } else if (value >= bigint.min && value <= bigint.max) {
      type = LogicalType::kBigInt;
    }
    return arrow::Result<TypedConstant>(TypedConstant{.value = value, .type = type});
  }
  // DuckDB's HUGEINT reaches 2^127 - 1, antb1's 38 digits: a HUGEINT result outside them is
  // unsupported rather than an overflow.
  const auto overflow_error = [](LogicalType type, std::string_view what,
                                 SourceSpan span) -> arrow::Status {
    if (type == LogicalType::kHugeInt) {
      return UnsupportedError(std::string(kHugeIntConstants), span);
    }
    return BindError(std::format("Overflow in {} of {}", what, ToString(type)), span);
  };
  const auto fits = [&](Int128 value, LogicalType type, std::string_view what,
                        SourceSpan span) -> arrow::Result<TypedConstant> {
    const IntegerRange range = RangeOf(type);
    if (value < range.min || value > range.max) {
      return overflow_error(type, what, span);
    }
    return TypedConstant{.value = value, .type = type};
  };
  if (const auto* unary = std::get_if<sql::UnaryExpr>(&expr);
      unary != nullptr && unary->op == sql::UnaryOp::kNegate) {
    // DuckDB folds the minuses of a literal into the literal while parsing, so
    // -(-9223372036854775808) is the HUGEINT literal 9223372036854775808, not an overflowing
    // negation, and -(-(-9223372036854775808)) is a BIGINT.
    bool flip = true;
    const sql::Expr* operand = &*unary->operand;
    for (const auto* inner = std::get_if<sql::UnaryExpr>(operand);
         inner != nullptr && inner->op == sql::UnaryOp::kNegate;
         inner = std::get_if<sql::UnaryExpr>(operand)) {
      flip = !flip;
      operand = &*inner->operand;
    }
    if (const auto* lit = std::get_if<sql::Literal>(operand);
        lit != nullptr && lit->kind == sql::Literal::Kind::kInteger) {
      sql::Literal negated = *lit;
      negated.negative = flip != lit->negative;
      negated.span = unary->span;
      return FoldTyped(sql::Expr(std::move(negated)));
    }
    auto inner = FoldTyped(*unary->operand);
    if (!inner.has_value() || !inner->ok()) {
      return inner;
    }
    return fits(-(*inner)->value, (*inner)->type, "negation", unary->span);
  }
  const auto* binary = std::get_if<sql::BinaryExpr>(&expr);
  if (binary == nullptr ||
      (binary->op != sql::BinaryOp::kAdd && binary->op != sql::BinaryOp::kSubtract &&
       binary->op != sql::BinaryOp::kMultiply)) {
    return std::nullopt;
  }
  auto l = FoldTyped(*binary->left);
  auto r = FoldTyped(*binary->right);
  if (!l.has_value() || !r.has_value()) {
    return std::nullopt;
  }
  if (!l->ok()) {
    return l;
  }
  if (!r->ok()) {
    return r;
  }
  const TypedConstant a = **l;
  const TypedConstant b = **r;
  const LogicalType type = rank(a.type) >= rank(b.type) ? a.type : b.type;
  Int128 out = 0;
  bool overflow = false;
  std::string_view what;
  switch (binary->op) {
    case sql::BinaryOp::kAdd:
      overflow = __builtin_add_overflow(a.value, b.value, &out);
      what = "addition";
      break;
    case sql::BinaryOp::kSubtract:
      overflow = __builtin_sub_overflow(a.value, b.value, &out);
      what = "subtraction";
      break;
    default:
      overflow = __builtin_mul_overflow(a.value, b.value, &out);
      what = "multiplication";
      break;
  }
  if (overflow) {
    return arrow::Result<TypedConstant>(overflow_error(type, what, binary->span));
  }
  return fits(out, type, what, binary->span);
}

bool IsConstantInteger(const sql::Expr& expr) {
  return !std::holds_alternative<sql::Literal>(expr) && FoldTyped(expr).has_value();
}

// The literal of a constant integer expression's value, spanning it (IsConstantInteger), or the
// bind error of its overflow.
arrow::Result<sql::Literal> FoldConstant(const sql::Expr& expr) {
  auto folded = FoldTyped(expr);
  ANTB1_CHECK(folded.has_value());
  ARROW_ASSIGN_OR_RAISE(const TypedConstant constant, *std::move(folded));
  return sql::Literal{.kind = sql::Literal::Kind::kInteger,
                      .negative = constant.value < 0,
                      .text = Int128ToString(constant.value < 0 ? -constant.value : constant.value),
                      .span = expr.span()};
}

// Whether an integer constant fits an integer type.
bool Fits(const Expr& constant, LogicalType type) {
  const auto* c = std::get_if<ConstantExpr>(&constant.node);
  const auto* value = c != nullptr ? std::get_if<Int128>(&c->value.value) : nullptr;
  if (value == nullptr) {
    return false;
  }
  const IntegerRange range = RangeOf(type);
  return *value >= range.min && *value <= range.max;
}

std::string DescribeOperand(const Typed& t) {
  return std::format("'{}' is {}", Clip(t.expr->name), ToString(t.expr->type));
}

// An integer type as the DECIMAL DuckDB computes it in next to a DECIMAL (ADR 0021 rule 4): by its
// type, so a literal does not shrink to fit (7 is INTEGER, DECIMAL(10,0)).
LogicalType DecimalOfInteger(LogicalType type) {
  switch (type.id()) {
    case LogicalType::kSmallInt:
    case LogicalType::kUSmallInt:
      return LogicalType::Decimal(5, 0);
    case LogicalType::kInteger:
      return LogicalType::Decimal(10, 0);
    case LogicalType::kBigInt:
      return LogicalType::Decimal(19, 0);
    default:
      break;
  }
  return LogicalType::Decimal(LogicalType::kMaxDecimalWidth, 0);  // HUGEINT
}

// DuckDB's type of `l <op> r` for + - * % with a DECIMAL operand (a decimal literal included) and
// the other a DECIMAL or an integer (ADR 0021 rules 4 to 6 and 9): for + - * the width beyond 18
// digits is capped to 18 while both operands have at most 18 (DuckDB computes them in 64 bits),
// and to 38 beyond that; % takes the common type, and beyond 38 digits it is DOUBLE (DuckDB's fmod
// of the operands converted by rule 8, NULL for a zero divisor).
arrow::Result<LogicalType> DecimalArithType(ArithOp op, const Typed& l, const Typed& r,
                                            SourceSpan span) {
  const auto decimal = [](LogicalType t) {
    return t == LogicalType::kDecimal ? t : DecimalOfInteger(t);
  };
  const LogicalType a = decimal(l.expr->type);
  const LogicalType b = decimal(r.expr->type);
  const int pa = a.width();
  const int pb = b.width();
  const int sa = a.scale();
  const int sb = b.scale();
  constexpr int kMax = LogicalType::kMaxDecimalWidth;
  constexpr int kSmall = 18;  // DuckDB's 64-bit DECIMAL
  int width = 0;
  int scale = 0;
  if (op == ArithOp::kModulo) {
    scale = std::max(sa, sb);
    width = std::max(pa - sa, pb - sb) + scale;
    if (width > kMax) {
      return LogicalType::kDouble;
    }
  } else if (op == ArithOp::kMultiply) {
    scale = sa + sb;
    if (scale > kMax) {
      return BindError(std::format("Needed scale {} to accurately represent the multiplication "
                                   "result, but this is out of range of the DECIMAL type. Max "
                                   "scale is 38; could not perform an accurate multiplication. "
                                   "Either add a cast to DOUBLE, or add an explicit cast to a "
                                   "decimal with a lower scale.",
                                   scale),
                       span);
    }
    width = pa + pb;
    if (width > kSmall && pa <= kSmall && pb <= kSmall && scale < kSmall) {
      width = kSmall;
    }
  } else {
    scale = std::max(sa, sb);
    width = std::max(pa - sa, pb - sb) + scale + 1;
    if (width > kSmall && pa <= kSmall && pb <= kSmall) {
      width = kSmall;
    }
  }
  width = std::min(width, kMax);
  return LogicalType::Decimal(Narrow<std::uint8_t>(width), Narrow<std::uint8_t>(scale));
}

// The type DuckDB gives `l <op> r` (docs/sql-subset.md), or the error for operands it cannot take.
arrow::Result<LogicalType> ArithType(sql::BinaryOp sql_op, ArithOp op, const Typed& l,
                                     const Typed& r, SourceSpan span) {
  for (const Typed* t : {&l, &r}) {
    if (t->expr->type == LogicalType::kDate || t->expr->type == LogicalType::kTimestamp) {
      const std::string type = ToString(t->expr->type);
      return UnsupportedError(std::format("{} arithmetic ('{}' is {}) is not supported", type,
                                          Clip(t->expr->name), type),
                              span);
    }
    if (t->expr->type == LogicalType::kDecimal) {
      continue;
    }
    if (!IsNumeric(t->expr->type)) {
      return BindError(std::format("arithmetic operator '{}' needs numbers, but {}",
                                   sql::ToString(sql_op), DescribeOperand(*t)),
                       span);
    }
    if (t->stored_as_float) {
      return UnsupportedError(
          std::format("arithmetic on the FLOAT column {} is not supported (antb1 reads FLOAT as "
                      "DOUBLE, divergence D11)",
                      Clip(t->expr->name)),
          span);
    }
  }
  // / is DOUBLE; so is anything with a DOUBLE, and // with a DECIMAL (a decimal literal included),
  // whose DECIMAL operands convert as DuckDB converts them (ADR 0021 rule 8).
  const bool any_decimal =
      l.expr->type == LogicalType::kDecimal || r.expr->type == LogicalType::kDecimal;
  if (op == ArithOp::kDivide || l.expr->type == LogicalType::kDouble ||
      r.expr->type == LogicalType::kDouble || (any_decimal && op == ArithOp::kIntegerDivide)) {
    return LogicalType::kDouble;
  }
  if (any_decimal) {
    return DecimalArithType(op, l, r, span);
  }
  LogicalType type = LogicalType::kHugeInt;
  if (l.literal && !r.literal && Fits(*l.expr, r.expr->type)) {
    type = r.expr->type;
  } else if (r.literal && !l.literal && Fits(*r.expr, l.expr->type)) {
    type = l.expr->type;
  } else {
    type = CombineIntegers(l.expr->type, r.expr->type);
  }
  if (type == LogicalType::kHugeInt && (op == ArithOp::kIntegerDivide || op == ArithOp::kModulo)) {
    return UnsupportedError(std::format("'{}' in HUGEINT (integer SUM results) is not supported",
                                        sql::ToString(sql_op)),
                            span);
  }
  return type;
}

std::optional<ArithOp> ArithOpOf(sql::BinaryOp op) {
  switch (op) {
    case sql::BinaryOp::kAdd:
      return ArithOp::kAdd;
    case sql::BinaryOp::kSubtract:
      return ArithOp::kSubtract;
    case sql::BinaryOp::kMultiply:
      return ArithOp::kMultiply;
    case sql::BinaryOp::kDivide:
      return ArithOp::kDivide;
    case sql::BinaryOp::kIntegerDivide:
      return ArithOp::kIntegerDivide;
    case sql::BinaryOp::kModulo:
      return ArithOp::kModulo;
    default:
      return std::nullopt;
  }
}

// `left <op> right` over typed operands.
arrow::Result<Typed> Arith(const sql::BinaryExpr& binary, Typed left, Typed right) {
  const std::optional<ArithOp> op = ArithOpOf(binary.op);
  if (!op.has_value()) {
    return UnsupportedError("this operator is not supported here", binary.op_span);
  }
  ARROW_ASSIGN_OR_RAISE(const LogicalType type,
                        ArithType(binary.op, *op, left, right, binary.op_span));
  std::string name =
      std::format("({} {} {})", left.expr->name, sql::ToString(binary.op), right.expr->name);
  return Leaf(Expr{
      .node = ArithExpr{.op = *op, .left = std::move(left.expr), .right = std::move(right.expr)},
      .type = type,
      .name = std::move(name)});
}

arrow::Result<Typed> Negate(const sql::UnaryExpr& unary, Typed operand) {
  const LogicalType type = operand.expr->type;  // a DECIMAL keeps its type, as in DuckDB
  if (!IsNumeric(type) && type != LogicalType::kDecimal) {
    return BindError(
        std::format("arithmetic operator '-' needs a number, but {}", DescribeOperand(operand)),
        unary.op_span);
  }
  if (type == LogicalType::kUSmallInt || operand.stored_as_float) {
    return UnsupportedError(std::format("negating a {} is not supported",
                                        operand.stored_as_float ? "FLOAT" : ToString(type)),
                            unary.op_span);
  }
  std::string name = "-(" + operand.expr->name + ")";
  return Leaf(Expr{.node = NegateExpr{.operand = std::move(operand.expr)},
                   .type = type,
                   .name = std::move(name)});
}

// A literal as an expression operand: typed by its value (an integer), DECIMAL (a decimal, ADR 0021
// rule 3: flagged), DOUBLE (an exponent or more than 38 digits), VARCHAR or DATE.
arrow::Result<Typed> LiteralOperand(const sql::Literal& lit) {
  // A DATE or TIMESTAMP literal is named as DuckDB names its cast, as in ExprName.
  const std::string name =
      lit.kind == sql::Literal::Kind::kDate || lit.kind == sql::Literal::Kind::kTimestamp
          ? ExprName(sql::Expr(lit))
          : LiteralName(lit);
  const bool number =
      lit.kind == sql::Literal::Kind::kInteger || lit.kind == sql::Literal::Kind::kDecimal;
  if (number && lit.kind == sql::Literal::Kind::kDecimal && !IsApproximateNumber(lit.text)) {
    ARROW_ASSIGN_OR_RAISE(auto constant, BindConstant(lit));
    const LogicalType type = constant.first.type;
    Typed out = Leaf(
        Expr{.node = ConstantExpr{.value = std::move(constant.first)}, .type = type, .name = name});
    out.literal = true;
    out.decimal = true;
    return out;
  }
  if (number && (lit.kind == sql::Literal::Kind::kDecimal || IsApproximateNumber(lit.text))) {
    const auto value = ParseDoubleLiteral(lit.text, lit.negative);
    if (!value.has_value()) {
      return BindError("invalid number " + Clip(lit.text), lit.span);
    }
    Typed out = Leaf(
        Expr{.node = ConstantExpr{.value = Constant{.type = LogicalType::kDouble, .value = *value}},
             .type = LogicalType::kDouble,
             .name = name});
    out.literal = true;
    return out;
  }
  ARROW_ASSIGN_OR_RAISE(auto constant, BindConstant(lit));
  const LogicalType type = constant.first.type;
  Typed out = Leaf(
      Expr{.node = ConstantExpr{.value = std::move(constant.first)}, .type = type, .name = name});
  out.literal = number;
  return out;
}

bool IsConstantExpr(const Expr& expr) { return std::holds_alternative<ConstantExpr>(expr.node); }

// A node that the checks before binding never admit where a binder visits it (programming errors):
// - ConditionAsOperand: [NOT] LIKE and [NOT] IN are conditions. CheckSupported admits them only
//   where BindConditionWith binds them, never where BindInput or BindOutput binds an operand.
// - NotAConditionLeaf: a condition leaf is a comparison, [NOT] LIKE or [NOT] IN. RejectCondition
//   admits no other, and Conjuncts and BindBool take AND, OR and NOT apart before
//   BindConditionWith binds a leaf.
[[noreturn]] void ConditionAsOperand() { ANTB1_CHECK(false); }
[[noreturn]] void NotAConditionLeaf() { ANTB1_CHECK(false); }
// - UnfoldedCast: FoldDateCasts turned the casts the binder answers into DATE literals, and
//   CheckSupported rejected every other one, except in the arguments of a call with the wrong
//   number of them, which BindFunction rejects before it binds or names an argument.
[[noreturn]] void UnfoldedCast() { ANTB1_CHECK(false); }

// The comparison of a binary comparison operator (any other operator is no condition leaf).
sql::CompareOp SqlCompareOp(sql::BinaryOp op) {
  switch (op) {
    case sql::BinaryOp::kEq:
      return sql::CompareOp::kEq;
    case sql::BinaryOp::kNe:
      return sql::CompareOp::kNe;
    case sql::BinaryOp::kLt:
      return sql::CompareOp::kLt;
    case sql::BinaryOp::kLe:
      return sql::CompareOp::kLe;
    case sql::BinaryOp::kGt:
      return sql::CompareOp::kGt;
    case sql::BinaryOp::kGe:
      return sql::CompareOp::kGe;
    default:  // arithmetic, AND, OR
      break;
  }
  NotAConditionLeaf();
}

// The operator with swapped operands: 5 < c  <=>  c > 5.
sql::CompareOp Mirror(sql::CompareOp op) {
  switch (op) {
    case sql::CompareOp::kLt:
      return sql::CompareOp::kGt;
    case sql::CompareOp::kLe:
      return sql::CompareOp::kGe;
    case sql::CompareOp::kGt:
      return sql::CompareOp::kLt;
    case sql::CompareOp::kGe:
      return sql::CompareOp::kLe;
    default:
      return op;
  }
}

// Whether a comparison between the types is one DuckDB and antb1 make alike: numbers with numbers
// (a DECIMAL of any precision and scale included), VARCHAR with VARCHAR, DATE with DATE.
bool Comparable(LogicalType a, LogicalType b) {
  const auto number = [](LogicalType t) { return IsNumeric(t) || t == LogicalType::kDecimal; };
  return (number(a) && number(b)) || a == b;
}

// The last select item with the alias (DuckDB), if any.
std::optional<std::size_t> FindAlias(const SelectList& select, std::string_view name) {
  const std::string wanted = AsciiLower(name);
  for (std::size_t n = select.items.size(); n > 0; --n) {
    const auto& alias = select.aliases[n - 1];
    if (alias.has_value() && AsciiLower(*alias) == wanted) {
      return n - 1;
    }
  }
  return std::nullopt;
}

bool SameCall(const AggregateCall& a, const AggregateCall& b) {
  if (a.kind != b.kind || a.arg.has_value() != b.arg.has_value()) {
    return false;
  }
  return !a.arg.has_value() || a.arg->id == b.arg->id;
}

// The select item (0-based) that a GROUP BY or ORDER BY literal refers to: an integer is a position
// (1-based; out of range, a negative one too, is a bind error, as in DuckDB). Any other literal is
// a constant (std::nullopt), except that DuckDB rejects a number or string in ORDER BY because it
// would order nothing (a DATE literal is a constant expression there).
arrow::Result<std::optional<std::size_t>> PositionOf(const sql::Literal& lit,
                                                     const SelectList& select,
                                                     std::string_view clause) {
  if (lit.kind != sql::Literal::Kind::kInteger || IsApproximateNumber(lit.text)) {
    if (clause == "ORDER BY" && lit.kind != sql::Literal::Kind::kDate &&
        lit.kind != sql::Literal::Kind::kTimestamp) {
      return BindError("ORDER BY a non-integer literal orders nothing", lit.span);
    }
    return std::nullopt;
  }
  const auto exact = ParseExactNumber(lit.text, lit.negative);
  const auto count = static_cast<Int128>(select.items.size());
  if (!exact.has_value() || exact->huge || exact->negative || exact->magnitude < 1 ||
      exact->magnitude > count) {
    return BindError(std::format("{} position {}{} is not between 1 and {}", clause,
                                 lit.negative ? "-" : "", Clip(lit.text), select.items.size()),
                     lit.span);
  }
  return static_cast<std::size_t>(exact->magnitude) - 1;
}

// How the rows of a query are shaped, which decides what ORDER BY may refer to.
enum class Shape : std::uint8_t {
  kProjection,  // no aggregate: any table column
  kGrouped,     // GROUP BY: keys and aggregates (hidden ones are added)
  kGlobal,      // aggregates without GROUP BY: one row, so only aggregates
};

// An aggregate anywhere in the select list, HAVING or ORDER BY makes the query aggregate, and so
// does HAVING itself (DuckDB).
bool IsAggregateQuery(const sql::SelectStatement& stmt) {
  return !stmt.having.empty() || std::ranges::any_of(stmt.items, [](const sql::SelectItem& i) {
    return ContainsAggregate(i.expr);
  }) || std::ranges::any_of(stmt.order_by, [](const sql::OrderItem& i) {
    return ContainsAggregate(i.expr);
  });
}

class Binder {
 public:
  Binder(const sql::SelectStatement& stmt, std::shared_ptr<Table> table, ColumnIdSource& ids)
      : stmt_(stmt),
        table_(std::move(table)),
        schema_(*table_->schema()),
        columns_(schema_, ids),
        ids_(ids) {}

  arrow::Result<LogicalPlan> Bind();

 private:
  // ---- the input scope: the table's columns, and computed columns over them ----

  struct BindInputOf {
    Binder& binder;

    arrow::Result<Typed> operator()(const sql::ColumnRef& ref) const {
      auto resolved = binder.columns_.Resolve(ref);
      if (!resolved.ok() && binder.shape_ == Shape::kProjection) {
        if (auto alias = binder.AliasFallback(ref, resolved.status())) {
          return *std::move(alias);
        }
      }
      ARROW_ASSIGN_OR_RAISE(BoundColumn column, std::move(resolved));
      return ColumnLeaf(column.id, column.type, ArgumentName(ref.name),
                        binder.StoredAsFloat(column.id));
    }
    arrow::Result<Typed> operator()(const sql::Literal& lit) const { return LiteralOperand(lit); }
    arrow::Result<Typed> operator()(const sql::AggregateCall& call) const {
      return BindError("aggregate functions are not allowed here", call.span);
    }
    arrow::Result<Typed> operator()(const sql::UnaryExpr& unary) const {
      ARROW_ASSIGN_OR_RAISE(Typed operand, binder.BindInput(*unary.operand));
      return Negate(unary, std::move(operand));
    }
    arrow::Result<Typed> operator()(const sql::BinaryExpr& binary) const {
      ARROW_ASSIGN_OR_RAISE(Typed left, binder.BindInput(*binary.left));
      ARROW_ASSIGN_OR_RAISE(Typed right, binder.BindInput(*binary.right));
      return Arith(binary, std::move(left), std::move(right));
    }
    arrow::Result<Typed> operator()(const sql::LikeExpr& /*like*/) const { ConditionAsOperand(); }
    arrow::Result<Typed> operator()(const sql::InExpr& /*in*/) const { ConditionAsOperand(); }
    arrow::Result<Typed> operator()(const sql::BetweenExpr& /*between*/) const {
      ConditionAsOperand();
    }
    arrow::Result<Typed> operator()(const sql::FunctionCall& call) const {
      return binder.BindFunction(call, [this](const sql::Expr& e) { return binder.BindInput(e); });
    }
    arrow::Result<Typed> operator()(const sql::CaseExpr& c) const {
      return binder.BindCase(
          c, [this](const sql::Expr& e) { return binder.BindInput(e); }, /*input=*/true);
    }
    arrow::Result<Typed> operator()(const sql::ExtractExpr& e) const {
      return binder.BindExtract(e, [this](const sql::Expr& x) { return binder.BindInput(x); });
    }
    arrow::Result<Typed> operator()(const sql::CastExpr& /*cast*/) const { UnfoldedCast(); }
  };

  arrow::Result<Typed> BindInput(const sql::Expr& expr) {
    return std::visit(BindInputOf{.binder = *this}, static_cast<const sql::ExprNode&>(expr));
  }

  // A function call with its first argument bound by `bind_arg` (the scope's binder).
  template <class BindArg>
  arrow::Result<Typed> BindFunction(const sql::FunctionCall& call, const BindArg& bind_arg) {
    const std::optional<FunctionSpec> spec = FindFunction(call);
    if (!spec.has_value()) {
      return UnsupportedError("this function is not supported", call.name_span);
    }
    if (call.args.size() != spec->args) {
      return BindError(std::format("{}() takes {} argument{}, not {}", spec->name, spec->args,
                                   spec->args == 1 ? "" : "s", call.args.size()),
                       call.span);
    }
    ARROW_ASSIGN_OR_RAISE(Typed value, bind_arg(call.args[spec->value_arg]));
    std::string name = std::string(spec->name) + "(";
    std::vector<ExprPtr> literals;
    for (std::size_t i = 0; i < call.args.size(); ++i) {
      name += i == 0 ? "" : ", ";
      if (i == spec->value_arg) {
        name += value.expr->name;
        continue;
      }
      const auto* lit = std::get_if<sql::Literal>(&call.args[i]);
      if (lit == nullptr || lit->kind != sql::Literal::Kind::kString) {
        return BindError(
            std::format("argument {} of {}() must be a string literal", i + 1, spec->name),
            call.args[i].span());
      }
      name += LiteralName(*lit);
      // date_trunc's unit: the canonical unit its spelling (any case) means.
      const std::optional<DatePart> part =
          spec->function == Function::kDateTrunc ? FindDatePart(lit->text) : std::nullopt;
      const std::string text = part.has_value() ? std::string(part->unit) : lit->text;
      literals.push_back(std::make_shared<const Expr>(Expr{
          .node = ConstantExpr{.value = Constant{.type = LogicalType::kVarchar, .value = text}},
          .type = LogicalType::kVarchar,
          .name = LiteralName(*lit)}));
    }
    name += ')';
    const SourceSpan value_span = call.args[spec->value_arg].span();
    LogicalType type = LogicalType::kVarchar;
    switch (spec->function) {
      case Function::kStrlen:
      case Function::kRegexpReplace:
        if (value.expr->type != LogicalType::kVarchar) {
          return BindError(
              std::format("{}() needs a VARCHAR, but {}", spec->name, DescribeOperand(value)),
              value_span);
        }
        type = spec->function == Function::kStrlen ? LogicalType::kBigInt : LogicalType::kVarchar;
        break;
      case Function::kEpochMs: {
        // toDateTime(t) is epoch_ms(t * 1000): the product is typed and checked as arithmetic.
        if (!IsInteger(value.expr->type) || value.expr->type == LogicalType::kHugeInt) {
          return BindError(std::format("toDateTime() needs an integer (seconds), but {}",
                                       DescribeOperand(value)),
                           value_span);
        }
        const sql::Literal thousand{
            .kind = sql::Literal::Kind::kInteger, .negative = false, .text = "1000", .span = {}};
        const sql::BinaryExpr product{.op = sql::BinaryOp::kMultiply,
                                      .left = sql::Box<sql::Expr>(call.args[0]),
                                      .right = sql::Box<sql::Expr>(sql::Expr(thousand)),
                                      .op_span = call.name_span,
                                      .span = call.span};
        ARROW_ASSIGN_OR_RAISE(Typed literal, LiteralOperand(thousand));
        ARROW_ASSIGN_OR_RAISE(value, Arith(product, std::move(value), std::move(literal)));
        type = LogicalType::kTimestamp;
        break;
      }
      case Function::kDateTrunc:
        if (value.expr->type != LogicalType::kTimestamp && value.expr->type != LogicalType::kDate) {
          return BindError(
              std::format("date_trunc() needs a TIMESTAMP or DATE, but {}", DescribeOperand(value)),
              value_span);
        }
        type = LogicalType::kTimestamp;
        break;
      case Function::kExtract:
        return arrow::Status::Invalid("EXTRACT is no function call");
    }
    std::vector<ExprPtr> args{std::move(value.expr)};
    args.insert(args.end(), literals.begin(), literals.end());
    return Leaf(Expr{.node = FunctionExpr{.function = spec->function, .args = std::move(args)},
                     .type = type,
                     .name = std::move(name)});
  }

  // EXTRACT(field FROM source): BIGINT (epoch: DOUBLE) of a TIMESTAMP or DATE, named as DuckDB
  // names it.
  template <class BindArg>
  arrow::Result<Typed> BindExtract(const sql::ExtractExpr& e, const BindArg& bind_arg) {
    ARROW_ASSIGN_OR_RAISE(Typed source, bind_arg(*e.source));
    if (source.expr->type != LogicalType::kTimestamp && source.expr->type != LogicalType::kDate) {
      return BindError(
          std::format("EXTRACT needs a TIMESTAMP or DATE, but {}", DescribeOperand(source)),
          e.source->span());
    }
    const std::optional<DatePart> part = FindDatePart(e.field);
    if (!part.has_value() || part->field.empty()) {
      return UnsupportedError("this EXTRACT field is not supported", e.field_span);
    }
    const std::string field(part->field);
    std::string name =
        std::format("main.date_part('{}', {})", ExtractFieldName(e.field), source.expr->name);
    auto field_expr = std::make_shared<const Expr>(
        Expr{.node = ConstantExpr{.value = Constant{.type = LogicalType::kVarchar, .value = field}},
             .type = LogicalType::kVarchar,
             .name = "'" + field + "'"});
    return Leaf(Expr{.node = FunctionExpr{.function = Function::kExtract,
                                          .args = {std::move(source.expr), std::move(field_expr)}},
                     .type = field == "epoch" ? LogicalType::kDouble : LogicalType::kBigInt,
                     .name = std::move(name)});
  }

  // The column of an input-scope expression: a table column as is, anything else computed (once):
  // a WHERE operand (`where`) before the WHERE filter on computed columns, anything else after it,
  // so that it is computed only for the rows WHERE keeps.
  BoundColumn InputColumn(const Typed& t, bool where = false) {
    if (const auto* column = std::get_if<ColumnExpr>(&t.expr->node)) {
      // A table field is named as declared; a computed column (a select item) by its expression.
      const std::optional<int> field = columns_.FieldOf(column->id);
      return BoundColumn{.id = column->id,
                         .name = field.has_value() ? schema_.field(*field)->name() : t.expr->name,
                         .type = t.expr->type};
    }
    std::vector<ExprPtr>& exprs = where ? where_exprs_ : input_exprs_;
    std::vector<ColumnId>& ids = where ? where_ids_ : input_ids_;
    const auto same =
        std::ranges::find_if(exprs, [&](const ExprPtr& e) { return SameExpr(*e, *t.expr); });
    const auto k = Narrow<std::size_t>(same - exprs.begin());
    if (same == exprs.end()) {
      exprs.push_back(t.expr);
      ids.push_back(ids_.Next());
    }
    return BoundColumn{.id = ids[k], .name = t.expr->name, .type = t.expr->type};
  }

  // An aggregate call with its argument bound in the input scope (computed when not a column).
  arrow::Result<AggregateCall> BindAggregate(const sql::AggregateCall& call) {
    AggregateCall bound{.kind = AggKind::kCountStar, .span = call.span};
    if (call.kind == sql::AggKind::kCountStar || !call.arg.has_value()) {
      return bound;
    }
    ARROW_ASSIGN_OR_RAISE(const Typed arg, BindInput(**call.arg));
    BoundColumn column = InputColumn(arg);
    if (call.arg_column() == nullptr) {
      column.name = arg.expr->name;
    }
    ARROW_ASSIGN_OR_RAISE(bound.type, AggregateType(call, column));
    bound.kind = call.distinct ? AggKind::kCountDistinct : ToPlan(call.kind);
    bound.arg = std::move(column);
    return bound;
  }

  // ---- the output scope: the aggregation's keys and aggregates ----

  struct SumRewrite {
    const sql::Expr* other = nullptr;    // SUM(other + constant)
    const sql::Expr* counted = nullptr;  // `other` without its `+ constant` layers
    Int128 constant = 0;
  };

  // DuckDB's sum rewriter, in a query without GROUP BY: SUM(other + c) (either order), c an integer
  // constant (literals folded) and the sum and `other` of a signed integer type, is
  // SUM(other) + c * COUNT(other), in HUGEINT: the addition is never computed, so it never
  // overflows. SUM(other) is rewritten again if it has that shape, and COUNT(other) counts `other`
  // without its `+ constant` layers (they add no NULL), as DuckDB does.
  arrow::Result<std::optional<SumRewrite>> RewritableSum(const sql::AggregateCall& call) {
    if (!stmt_.group_by.empty() || call.kind != sql::AggKind::kSum || call.distinct ||
        !call.arg.has_value()) {
      return std::nullopt;
    }
    const auto split =
        [](const sql::Expr& e) -> std::optional<std::pair<const sql::Expr*, Int128>> {
      const auto* add = std::get_if<sql::BinaryExpr>(&e);
      if (add == nullptr || add->op != sql::BinaryOp::kAdd) {
        return std::nullopt;
      }
      if (const auto c = ConstantValue(*add->right); c.has_value() && ReadsColumn(*add->left)) {
        return std::pair(&*add->left, *c);
      }
      if (const auto c = ConstantValue(*add->left); c.has_value() && ReadsColumn(*add->right)) {
        return std::pair(&*add->right, *c);
      }
      return std::nullopt;
    };
    const auto top = split(**call.arg);
    if (!top.has_value()) {
      return std::nullopt;
    }
    SumRewrite rewrite{.other = top->first, .counted = top->first, .constant = top->second};
    while (const auto inner = split(*rewrite.counted)) {
      rewrite.counted = inner->first;
    }
    ARROW_ASSIGN_OR_RAISE(const Typed whole, BindInput(**call.arg));
    ARROW_ASSIGN_OR_RAISE(const Typed other, BindInput(*rewrite.other));
    if (!IsSignedInteger(whole.expr->type) || !IsSignedInteger(other.expr->type)) {
      return std::nullopt;
    }
    return rewrite;
  }

  // The aggregate as an output column (added as a hidden aggregate when the select list lacks it);
  // a rewritten SUM as its expression over SUM and COUNT.
  arrow::Result<Typed> AggregateOutput(const sql::AggregateCall& call) {
    ARROW_ASSIGN_OR_RAISE(const auto rewrite, RewritableSum(call));
    if (rewrite.has_value()) {
      sql::AggregateCall sum{
          .kind = sql::AggKind::kSum, .arg = {}, .distinct = false, .span = call.span};
      sum.arg.emplace(*rewrite->other);
      sql::AggregateCall count{
          .kind = sql::AggKind::kCount, .arg = {}, .distinct = false, .span = call.span};
      count.arg.emplace(*rewrite->counted);
      ARROW_ASSIGN_OR_RAISE(Typed sum_column, AggregateOutput(sum));
      ARROW_ASSIGN_OR_RAISE(Typed count_column, AggregateOutput(count));
      const std::string constant_name = Int128ToString(rewrite->constant);
      auto constant = std::make_shared<const Expr>(
          Expr{.node = ConstantExpr{.value = Constant{.type = LogicalType::kHugeInt,
                                                      .value = rewrite->constant}},
               .type = LogicalType::kHugeInt,
               .name = constant_name});
      auto product = std::make_shared<const Expr>(
          Expr{.node = ArithExpr{.op = ArithOp::kMultiply,
                                 .left = std::move(constant),
                                 .right = std::move(count_column.expr)},
               .type = LogicalType::kHugeInt,
               .name = std::format("({} * {})", constant_name, ResultName(count))});
      return Leaf(Expr{.node = ArithExpr{.op = ArithOp::kAdd,
                                         .left = std::move(sum_column.expr),
                                         .right = std::move(product)},
                       .type = LogicalType::kHugeInt,
                       .name = ResultName(call)});
    }
    ARROW_ASSIGN_OR_RAISE(AggregateCall bound, BindAggregate(call));
    const auto same = std::ranges::find_if(
        select_.aggregates, [&](const AggregateCall& a) { return SameCall(a, bound); });
    const auto index = Narrow<std::size_t>(same - select_.aggregates.begin());
    if (same == select_.aggregates.end()) {
      bound.id = ids_.Next();
      select_.aggregates.push_back(std::move(bound));
    }
    const AggregateCall& agg = select_.aggregates[index];
    return ColumnLeaf(agg.id, agg.type, ResultName(call), FloatResult(agg));
  }

  // Whether column `id` is a table field stored as FLOAT (read as DOUBLE, divergence D11).
  [[nodiscard]] bool StoredAsFloat(ColumnId id) const {
    const std::optional<int> field = columns_.FieldOf(id);
    return field.has_value() && table_->StoredAsFloat(*field);
  }

  // DuckDB's MIN and MAX of a FLOAT column are FLOAT; the other aggregates are not.
  [[nodiscard]] bool FloatResult(const AggregateCall& agg) const {
    return (agg.kind == AggKind::kMin || agg.kind == AggKind::kMax) && agg.arg.has_value() &&
           StoredAsFloat(agg.arg->id);
  }

  // The GROUP BY key that is table column `id`, if one is.
  [[nodiscard]] std::optional<std::size_t> KeyOf(ColumnId id) const {
    for (std::size_t k = 0; k < key_exprs_.size(); ++k) {
      const auto* key = std::get_if<ColumnExpr>(&key_exprs_[k]->node);
      if (key != nullptr && key->id == id) {
        return k;
      }
    }
    return std::nullopt;
  }

  static arrow::Status NotGrouped(std::string_view name, SourceSpan span) {
    return BindError(std::format("column '{}' must appear in the GROUP BY clause or be inside an "
                                 "aggregate function",
                                 name),
                     span);
  }

  // An expression over the aggregation's output that is no GROUP BY key (see BindOutput).
  struct BindOutputOf {
    Binder& binder;

    arrow::Result<Typed> operator()(const sql::ColumnRef& ref) const {
      auto resolved = binder.columns_.Resolve(ref);
      if (!resolved.ok()) {
        if (auto alias = binder.AliasFallback(ref, resolved.status())) {
          return *std::move(alias);
        }
        return resolved.status();
      }
      return NotGrouped(ref.name, ref.span);
    }
    arrow::Result<Typed> operator()(const sql::Literal& lit) const { return LiteralOperand(lit); }
    arrow::Result<Typed> operator()(const sql::AggregateCall& call) const {
      return binder.AggregateOutput(call);
    }
    arrow::Result<Typed> operator()(const sql::UnaryExpr& unary) const {
      ARROW_ASSIGN_OR_RAISE(Typed operand, binder.BindOutput(*unary.operand));
      return Negate(unary, std::move(operand));
    }
    arrow::Result<Typed> operator()(const sql::BinaryExpr& binary) const {
      ARROW_ASSIGN_OR_RAISE(Typed left, binder.BindOutput(*binary.left));
      ARROW_ASSIGN_OR_RAISE(Typed right, binder.BindOutput(*binary.right));
      return Arith(binary, std::move(left), std::move(right));
    }
    arrow::Result<Typed> operator()(const sql::LikeExpr& /*like*/) const { ConditionAsOperand(); }
    arrow::Result<Typed> operator()(const sql::InExpr& /*in*/) const { ConditionAsOperand(); }
    arrow::Result<Typed> operator()(const sql::BetweenExpr& /*between*/) const {
      ConditionAsOperand();
    }
    arrow::Result<Typed> operator()(const sql::FunctionCall& call) const {
      return binder.BindFunction(call, [this](const sql::Expr& e) { return binder.BindOutput(e); });
    }
    arrow::Result<Typed> operator()(const sql::CaseExpr& c) const {
      return binder.BindCase(
          c, [this](const sql::Expr& e) { return binder.BindOutput(e); }, /*input=*/false);
    }
    arrow::Result<Typed> operator()(const sql::ExtractExpr& e) const {
      return binder.BindExtract(e, [this](const sql::Expr& x) { return binder.BindOutput(x); });
    }
    arrow::Result<Typed> operator()(const sql::CastExpr& /*cast*/) const { UnfoldedCast(); }
  };

  // An expression over the aggregation's output: a subexpression equal to a GROUP BY key is that
  // key, an aggregate its column; any other column is a bind error.
  arrow::Result<Typed> BindOutput(const sql::Expr& expr) {
    if (!ContainsAggregate(expr) && !std::holds_alternative<sql::Literal>(expr)) {
      auto input = BindInput(expr);
      if (input.ok()) {
        for (std::size_t k = 0; k < key_exprs_.size(); ++k) {
          if (SameExpr(*input->expr, *key_exprs_[k])) {
            return ColumnLeaf(key_ids_[k], keys_[k].type, input->expr->name,
                              input->stored_as_float);
          }
        }
      }
    }
    return std::visit(BindOutputOf{.binder = *this}, static_cast<const sql::ExprNode&>(expr));
  }

  // The column of an output-scope expression: an output column as is, anything else computed
  // above the aggregation (once).
  BoundColumn OutputColumn(const Typed& t) {
    if (const auto* column = std::get_if<ColumnExpr>(&t.expr->node)) {
      return BoundColumn{.id = column->id, .name = t.expr->name, .type = t.expr->type};
    }
    const auto same =
        std::ranges::find_if(post_exprs_, [&](const ExprPtr& e) { return SameExpr(*e, *t.expr); });
    const auto k = Narrow<std::size_t>(same - post_exprs_.begin());
    if (same == post_exprs_.end()) {
      post_exprs_.push_back(t.expr);
      post_ids_.push_back(ids_.Next());
    }
    return BoundColumn{.id = post_ids_[k], .name = t.expr->name, .type = t.expr->type};
  }

  // An expression of the select list, HAVING or ORDER BY in the query's scope: the input scope of a
  // projection, else the output scope.
  arrow::Result<Typed> BindScoped(const sql::Expr& expr) {
    return shape_ == Shape::kProjection ? BindInput(expr) : BindOutput(expr);
  }

  BoundColumn ScopedColumn(const Typed& t) {
    return shape_ == Shape::kProjection ? InputColumn(t) : OutputColumn(t);
  }

  // ---- clauses ----

  arrow::Status BindSelectList();
  arrow::Status BindWhere();
  arrow::Status BindGroupBy();
  arrow::Status CheckGrouped();
  arrow::Status BindSelectExpressions();
  // A WHERE or HAVING conjunct as a predicate over the scope's columns.
  arrow::Result<Predicate> BindCondition(const sql::Expr& conjunct, bool having);
  // BindCondition with the scope's operand binder `bind` and `column_of`, which gives an operand's
  // column (`input`: the input scope, where constants move). `nested`: inside a boolean
  // expression, where a folded predicate keeps its operand's column (NULL stays NULL).
  template <class BindFn, class ColumnOf>
  arrow::Result<Predicate> BindConditionWith(const sql::Expr& conjunct, bool input,
                                             const BindFn& bind, const ColumnOf& column_of,
                                             bool nested);
  // BindConditionWith's visitor over the condition leaves.
  template <class BindFn, class ColumnOf>
  struct BindConditionOf;
  // A condition as a BOOLEAN expression in the scope `bind` binds operands in: AND, OR and NOT
  // over PredicateExpr leaves (WHERE's forms, folded alike).
  template <class BindFn>
  arrow::Result<Typed> BindBool(const sql::Expr& expr, const BindFn& bind, bool input);
  // CASE with its conditions and values bound in the scope of `bind`, typed as DuckDB types it.
  template <class BindFn>
  arrow::Result<Typed> BindCase(const sql::CaseExpr& c, const BindFn& bind, bool input);
  struct Moved {
    enum class Outcome : std::uint8_t { kCompare, kFalse, kNotNull };
    const sql::Expr* operand = nullptr;
    sql::CompareOp op = sql::CompareOp::kEq;
    Int128 k = 0;
    Outcome outcome = Outcome::kCompare;
  };
  // `bind` binds the operand in its scope; `aggregates`: an aggregate call is an operand too (the
  // output scope of HAVING and of conditions over the aggregation).
  template <class BindFn>
  arrow::Result<std::optional<Moved>> MoveConstants(const sql::Expr& operand, sql::CompareOp op,
                                                    const sql::Literal& literal, const BindFn& bind,
                                                    bool aggregates);
  arrow::Result<std::optional<Typed>> ResolveHavingName(const sql::ColumnRef& ref);
  arrow::Status BindHaving();
  arrow::Status BindOrderBy();

  // Select item i in the query's scope (std::nullopt: a constant).
  arrow::Result<std::optional<Typed>> ItemOutput(std::size_t i);

  // Inside an ORDER BY or HAVING expression a name that is no table column is the last select item
  // with that alias, as in DuckDB (a table column comes first there). std::nullopt: no such alias,
  // or not in such an expression.
  std::optional<arrow::Result<Typed>> AliasFallback(const sql::ColumnRef& ref,
                                                    const arrow::Status& resolved) {
    const auto detail = GetSqlError(resolved);
    if (!alias_fallback_ || detail == nullptr || detail->kind() != SqlErrorDetail::Kind::kBind) {
      return std::nullopt;
    }
    const auto alias = FindAlias(select_, ref.name);
    if (!alias.has_value()) {
      return std::nullopt;
    }
    const auto [kind, index] = select_.items[*alias];
    if (kind == ItemKind::kConstant) {
      const Constant& constant = select_.constants[index];
      return Leaf(Expr{.node = ConstantExpr{.value = constant},
                       .type = constant.type,
                       .name = select_.output[*alias].name});
    }
    if (kind == ItemKind::kExpression) {
      // The item's expression itself (a computed column cannot be read by a computation), bound as
      // in the select list: without the fallback, so that an alias never refers to itself.
      alias_fallback_ = false;
      auto item = BindScoped(*select_.exprs[index]);
      alias_fallback_ = true;
      return item;
    }
    auto item = ItemOutput(*alias);
    if (!item.ok()) {
      return item.status();
    }
    std::optional<Typed> value = *std::move(item);
    if (!value.has_value()) {
      return std::nullopt;
    }
    return *std::move(value);
  }

  bool alias_fallback_ = false;  // binding an ORDER BY or HAVING expression

  LogicalPlan Assemble();

  const sql::SelectStatement& stmt_;
  std::shared_ptr<Table> table_;
  const arrow::Schema& schema_;
  Columns columns_;
  ColumnIdSource& ids_;
  SelectList select_;
  Shape shape_ = Shape::kProjection;
  std::vector<ExprPtr> where_exprs_;     // WHERE operands, computed over the filtered Scan
  std::vector<ColumnId> where_ids_;      // per WHERE operand: its column
  std::vector<ExprPtr> input_exprs_;     // computed after the WHERE filter on computed columns
  std::vector<ColumnId> input_ids_;      // per input expression: its column
  std::vector<Predicate> scan_filter_;   // WHERE over the Scan's columns
  std::vector<Predicate> input_filter_;  // WHERE over computed columns
  std::vector<BoundColumn> keys_;        // GROUP BY keys, as columns of the input
  std::vector<ColumnId> key_ids_;        // per key: its column in the aggregation's output
  std::vector<ExprPtr> key_exprs_;       // per key: its input-scope expression
  std::vector<ExprPtr> post_exprs_;      // computed over the aggregation's output
  std::vector<ColumnId> post_ids_;       // per post expression: its column
  std::vector<Predicate> having_;
  std::vector<SortKey> sort_keys_;
};

arrow::Status Binder::BindSelectList() {
  SelectList& list = select_;
  if (stmt_.star) {
    list.span = stmt_.star_span;
    for (int i = 0; i < schema_.num_fields(); ++i) {
      ARROW_ASSIGN_OR_RAISE(BoundColumn column, columns_.Field(i, stmt_.star_span));
      list.output.push_back(plan::OutputColumn{.name = column.name, .type = column.type});
      list.items.emplace_back(ItemKind::kColumn, list.columns.size());
      list.column_spans.push_back(stmt_.star_span);
      list.column_written.push_back(column.name);
      list.aliases.emplace_back();
      list.columns.push_back(std::move(column));
    }
    if (stmt_.group_by.empty() && IsAggregateQuery(stmt_) && !list.columns.empty()) {
      return BindError(std::format("column '{}' must be inside an aggregate function: a query "
                                   "with aggregates cannot also select plain columns (there is "
                                   "no GROUP BY)",
                                   list.columns.front().name),
                       stmt_.star_span);
    }
    return arrow::Status::OK();
  }
  if (stmt_.items.empty()) {
    return BindError("the select list is empty", stmt_.span);
  }
  list.span = Cover(stmt_.items.front().span, stmt_.items.back().span);
  const sql::ColumnRef* first_column = nullptr;
  for (const sql::SelectItem& item : stmt_.items) {
    list.aliases.push_back(item.alias);
    const auto* call = std::get_if<sql::AggregateCall>(&item.expr);
    bool rewritten = false;
    if (call != nullptr) {
      ARROW_ASSIGN_OR_RAISE(const auto rewrite, RewritableSum(*call));
      rewritten = rewrite.has_value();
    }
    if (call != nullptr && !rewritten) {
      ARROW_ASSIGN_OR_RAISE(AggregateCall bound, BindAggregate(*call));
      list.output.push_back(
          plan::OutputColumn{.name = item.alias.value_or(ResultName(*call)), .type = bound.type});
      list.items.emplace_back(ItemKind::kAggregate, list.aggregates.size());
      bound.id = ids_.Next();
      list.aggregates.push_back(std::move(bound));
      continue;
    }
    if (const auto* lit = std::get_if<sql::Literal>(&item.expr)) {
      ARROW_ASSIGN_OR_RAISE(auto constant, BindConstant(*lit));
      list.output.push_back(plan::OutputColumn{
          .name = item.alias.value_or(std::move(constant.second)), .type = constant.first.type});
      list.items.emplace_back(ItemKind::kConstant, list.constants.size());
      list.constants.push_back(std::move(constant.first));
      continue;
    }
    if (const auto* ref = std::get_if<sql::ColumnRef>(&item.expr)) {
      ARROW_ASSIGN_OR_RAISE(BoundColumn column, columns_.Resolve(*ref));
      // DuckDB names a plain column by its declared name, not as written.
      list.output.push_back(
          plan::OutputColumn{.name = item.alias.value_or(column.name), .type = column.type});
      list.items.emplace_back(ItemKind::kColumn, list.columns.size());
      list.column_spans.push_back(ref->span);
      list.column_written.push_back(ref->name);
      list.columns.push_back(std::move(column));
      if (first_column == nullptr) {
        first_column = ref;
      }
      continue;
    }
    // Bound once the query's shape and keys are known (BindSelectExpressions).
    list.output.push_back(plan::OutputColumn{.name = item.alias.value_or(""), .type = {}});
    list.items.emplace_back(ItemKind::kExpression, list.exprs.size());
    list.exprs.push_back(&item.expr);
  }
  if (stmt_.group_by.empty() && IsAggregateQuery(stmt_) && first_column != nullptr) {
    return BindError(std::format("column '{}' must be inside an aggregate function: a query with "
                                 "aggregates cannot also select plain columns (there is no "
                                 "GROUP BY)",
                                 first_column->name),
                     first_column->span);
  }
  return arrow::Status::OK();
}

// DuckDB's constant moving in a comparison `operand <op> k` (WHERE, HAVING, CASE WHEN, in any
// scope), k an integer literal: for a
// signed integer operand x + c, c + x or x - c (c an integer constant, literals folded) the
// comparison becomes x <op> k - c (or k + c), for c - x it becomes x <mirrored op> c - k, and for
// x * c (c not 0) x <op> k / c when c divides k (the op mirrored for a negative c), where a
// non-dividing k makes = FALSE and <> IS NOT NULL. It repeats on x, and stops where k or the new
// constant is outside the operand's type. So the arithmetic is never computed and never
// overflows, as in DuckDB. std::nullopt: nothing moved.
template <class BindFn>
arrow::Result<std::optional<Binder::Moved>> Binder::MoveConstants(const sql::Expr& operand,
                                                                  sql::CompareOp op,
                                                                  const sql::Literal& literal,
                                                                  const BindFn& bind,
                                                                  bool aggregates) {
  if (literal.kind != sql::Literal::Kind::kInteger || IsApproximateNumber(literal.text)) {
    return std::nullopt;
  }
  const auto exact = ParseExactNumber(literal.text, literal.negative);
  if (!exact.has_value() || exact->huge) {
    return std::nullopt;
  }
  Moved cur{.operand = &operand,
            .op = op,
            .k = exact->negative ? -exact->magnitude : exact->magnitude,
            .outcome = Moved::Outcome::kCompare};
  bool moved = false;
  const bool ordered = op != sql::CompareOp::kEq && op != sql::CompareOp::kNe;
  while (const auto* binary = std::get_if<sql::BinaryExpr>(cur.operand)) {
    ARROW_ASSIGN_OR_RAISE(const Typed whole, bind(*cur.operand));
    const LogicalType type = whole.expr->type;
    if (!IsSignedInteger(type)) {
      break;
    }
    const IntegerRange range = RangeOf(type);
    const auto fits = [&](Int128 v) { return v >= range.min && v <= range.max; };
    if (!fits(cur.k)) {
      break;
    }
    const auto right = ConstantValue(*binary->right);
    const auto left = ConstantValue(*binary->left);
    const bool constant_right = right.has_value() && ReadsColumn(*binary->left, aggregates);
    const bool constant_left = left.has_value() && ReadsColumn(*binary->right, aggregates);
    if (!constant_right && !constant_left) {
      break;
    }
    const sql::Expr* x = constant_right ? &*binary->left : &*binary->right;
    const Int128 c = constant_right ? *right : *left;
    Int128 k = 0;
    sql::CompareOp next = cur.op;
    bool overflow = false;
    switch (binary->op) {
      case sql::BinaryOp::kAdd:
        overflow = __builtin_sub_overflow(cur.k, c, &k);
        break;
      case sql::BinaryOp::kSubtract:
        if (constant_right) {
          overflow = __builtin_add_overflow(cur.k, c, &k);
        } else {
          overflow = __builtin_sub_overflow(c, cur.k, &k);
          next = Mirror(cur.op);
        }
        break;
      case sql::BinaryOp::kMultiply:
        if (c == 0) {
          overflow = true;  // no move
        } else if (cur.k % c != 0) {
          if (ordered) {
            overflow = true;
          } else {
            return Moved{.operand = x,
                         .op = cur.op,
                         .k = 0,
                         .outcome = cur.op == sql::CompareOp::kEq ? Moved::Outcome::kFalse
                                                                  : Moved::Outcome::kNotNull};
          }
        } else {
          k = cur.k / c;
          next = c < 0 ? Mirror(cur.op) : cur.op;
        }
        break;
      default:
        overflow = true;
        break;
    }
    if (overflow || !fits(k)) {
      break;
    }
    cur = Moved{.operand = x, .op = next, .k = k, .outcome = Moved::Outcome::kCompare};
    moved = true;
  }
  if (!moved) {
    return std::nullopt;
  }
  return cur;
}

// A condition of WHERE (the input scope) or HAVING (the output scope, `having`): an operand
// <op> literal is folded exactly into the operand's type as for a column; operand <op> operand
// compares two columns; [NOT] LIKE and [NOT] IN take a literal pattern or list. An operand that is
// not a column of the scope is computed.
arrow::Result<Predicate> Binder::BindCondition(const sql::Expr& conjunct, bool having) {
  const auto bind = [&](const sql::Expr& e) -> arrow::Result<Typed> {
    if (having) {
      if (const auto* ref = std::get_if<sql::ColumnRef>(&e)) {
        ARROW_ASSIGN_OR_RAISE(auto resolved, ResolveHavingName(*ref));
        if (resolved.has_value()) {
          return *std::move(resolved);
        }
      }
      return BindOutput(e);
    }
    return BindInput(e);
  };
  const auto column_of = [&](const Typed& t) {
    return having ? OutputColumn(t) : InputColumn(t, /*where=*/true);
  };
  return BindConditionWith(conjunct, /*input=*/!having, bind, column_of, /*nested=*/false);
}

// BindConditionWith's visitor: the condition leaves that RejectCondition admits ([NOT] LIKE,
// [NOT] IN and the comparisons) as predicates. Any other kind of node is no leaf.
template <class BindFn, class ColumnOf>
struct Binder::BindConditionOf {
  Binder& binder;
  const BindFn& bind;
  const ColumnOf& column_of;
  bool input = false;
  bool nested = false;

  // `operand <op> literal` (and the LIKE and IN forms) through the column binders.
  arrow::Result<Predicate> WithLiteral(const sql::Expr& operand_expr, sql::CompareOp op,
                                       const sql::Literal& literal,
                                       const std::vector<sql::Literal>& list,
                                       SourceSpan span) const {
    ARROW_ASSIGN_OR_RAISE(const Typed operand, bind(operand_expr));
    const BoundColumn column = column_of(operand);
    // The comparison binders read the operand's name and span from a column reference.
    const sql::Comparison cmp{
        .column = sql::ColumnRef{.name = column.name, .quoted = false, .span = operand_expr.span()},
        .op = op,
        .literal = literal,
        .list = list,
        .span = span};
    return BindComparison(cmp, column, operand.stored_as_float);
  }

  arrow::Result<Predicate> operator()(const sql::LikeExpr& like) const {
    return WithLiteral(*like.operand,
                       like.negated ? sql::CompareOp::kNotLike : sql::CompareOp::kLike,
                       std::get<sql::Literal>(*like.pattern), {}, like.span);
  }
  arrow::Result<Predicate> operator()(const sql::InExpr& in) const {
    std::vector<sql::Literal> list;
    list.reserve(in.list.size());
    for (const sql::Expr& value : in.list) {
      list.push_back(std::get<sql::Literal>(value));
    }
    return WithLiteral(*in.operand, in.negated ? sql::CompareOp::kNotIn : sql::CompareOp::kIn,
                       sql::Literal{}, list, in.span);
  }
  arrow::Result<Predicate> operator()(const sql::BinaryExpr& binary) const {
    // A constant integer expression on a side (1 + 2) as the literal of its value, which then
    // folds as any literal; one that overflows its type is a bind error.
    for (const bool left : {true, false}) {
      const sql::Expr& side = left ? *binary.left : *binary.right;
      if (std::holds_alternative<sql::Literal>(side) || !IsConstantInteger(side)) {
        continue;
      }
      ARROW_ASSIGN_OR_RAISE(sql::Literal folded, FoldConstant(side));
      sql::BinaryExpr literal = binary;
      (left ? literal.left : literal.right) = sql::Box<sql::Expr>(sql::Expr(std::move(folded)));
      return (*this)(literal);
    }
    const sql::CompareOp op = SqlCompareOp(binary.op);
    const auto* left_literal = std::get_if<sql::Literal>(&*binary.left);
    const auto* right_literal = std::get_if<sql::Literal>(&*binary.right);
    if (right_literal != nullptr || left_literal != nullptr) {
      const sql::Expr& operand_expr = right_literal != nullptr ? *binary.left : *binary.right;
      const sql::Literal& literal = right_literal != nullptr ? *right_literal : *left_literal;
      const sql::CompareOp oriented = right_literal != nullptr ? op : Mirror(op);
      {
        ARROW_ASSIGN_OR_RAISE(const auto moved,
                              binder.MoveConstants(operand_expr, oriented, literal, bind, !input));
        if (moved.has_value()) {
          if (moved->outcome != Moved::Outcome::kCompare) {
            ARROW_ASSIGN_OR_RAISE(const Typed operand, bind(*moved->operand));
            // A top-level FALSE reads nothing; inside an expression it is NULL for a NULL operand.
            const bool never = moved->outcome == Moved::Outcome::kFalse;
            return Predicate{
                .kind = never ? Predicate::Kind::kFalse : Predicate::Kind::kIsNotNull,
                .column = never && !nested ? std::nullopt : std::optional(column_of(operand)),
                .op = CompareOp::kEq,
                .constant = {},
                .values = {},
                .span = binary.span};
          }
          const sql::Literal k{.kind = sql::Literal::Kind::kInteger,
                               .negative = moved->k < 0,
                               .text = Int128ToString(moved->k < 0 ? -moved->k : moved->k),
                               .span = literal.span};
          return WithLiteral(*moved->operand, moved->op, k, {}, binary.span);
        }
      }
      return WithLiteral(operand_expr, oriented, literal, {}, binary.span);
    }
    ARROW_ASSIGN_OR_RAISE(const Typed left, bind(*binary.left));
    ARROW_ASSIGN_OR_RAISE(const Typed right, bind(*binary.right));
    const auto temporal = [](LogicalType t) {
      return t == LogicalType::kDate || t == LogicalType::kTimestamp;
    };
    if (left.expr->type != right.expr->type && temporal(left.expr->type) &&
        temporal(right.expr->type)) {
      return UnsupportedError("comparing a DATE with a TIMESTAMP is not supported", binary.op_span);
    }
    // A DECIMAL compares with a DECIMAL of any precision and scale and with an integer exactly by
    // value, and with a DOUBLE in DOUBLE after DuckDB's conversion (ADR 0021 rule 11; the
    // executor's comparison of the two columns).
    if (!Comparable(left.expr->type, right.expr->type)) {
      return BindError(
          std::format("cannot compare {} with {}", DescribeOperand(left), DescribeOperand(right)),
          binary.op_span);
    }
    if (left.stored_as_float || right.stored_as_float) {
      return UnsupportedError(
          "comparing a FLOAT column with another expression is not supported (antb1 reads FLOAT as "
          "DOUBLE, divergence D11)",
          binary.op_span);
    }
    return Predicate{.kind = Predicate::Kind::kCompareColumns,
                     .column = column_of(left),
                     .other = column_of(right),
                     .op = ToPlan(op),
                     .constant = {},
                     .values = {},
                     .span = binary.span};
  }
  arrow::Result<Predicate> operator()(const sql::ColumnRef& /*ref*/) const { NotAConditionLeaf(); }
  arrow::Result<Predicate> operator()(const sql::Literal& /*lit*/) const { NotAConditionLeaf(); }
  arrow::Result<Predicate> operator()(const sql::AggregateCall& /*call*/) const {
    NotAConditionLeaf();
  }
  arrow::Result<Predicate> operator()(const sql::UnaryExpr& /*unary*/) const {
    NotAConditionLeaf();
  }
  arrow::Result<Predicate> operator()(const sql::BetweenExpr& /*between*/) const {
    NotAConditionLeaf();
  }
  arrow::Result<Predicate> operator()(const sql::FunctionCall& /*call*/) const {
    NotAConditionLeaf();
  }
  arrow::Result<Predicate> operator()(const sql::CaseExpr& /*c*/) const { NotAConditionLeaf(); }
  arrow::Result<Predicate> operator()(const sql::ExtractExpr& /*e*/) const { NotAConditionLeaf(); }
  arrow::Result<Predicate> operator()(const sql::CastExpr& /*cast*/) const { UnfoldedCast(); }
};

template <class BindFn, class ColumnOf>
arrow::Result<Predicate> Binder::BindConditionWith(const sql::Expr& conjunct, bool input,
                                                   const BindFn& bind, const ColumnOf& column_of,
                                                   bool nested) {
  return std::visit(
      BindConditionOf<BindFn, ColumnOf>{
          .binder = *this, .bind = bind, .column_of = column_of, .input = input, .nested = nested},
      static_cast<const sql::ExprNode&>(conjunct));
}

namespace {

// DuckDB compares x BETWEEN lo AND hi with one common type of the three values, while antb1
// compares x >= lo and x <= hi each in its own. They differ only when that type is DOUBLE (a
// DOUBLE column or an approximate literal, which an exponent or more than 38 digits makes) and a
// pair that antb1 does not compare in DOUBLE would compare otherwise in DOUBLE: a BIGINT or
// HUGEINT value (a column, or a constant of that type), a FLOAT column, a decimal literal against
// an integer value, and a DECIMAL (ADR 0021): a DECIMAL operand against any bound that is not
// DOUBLE (an integer or decimal literal, a constant, a column), or a DECIMAL column or expression
// as a bound. Such a BETWEEN is unsupported, unless the operand is DOUBLE: then both comparisons
// are in DOUBLE already.
template <class BindFn>
arrow::Status CheckBetweenTypes(const sql::BetweenExpr& between, const BindFn& bind) {
  bool any_double = false;
  bool any_wide = false;
  bool any_float = false;
  bool any_decimal = false;  // a decimal literal
  bool any_integer = false;
  bool any_varchar = false;
  bool operand_double = false;     // the operand is DOUBLE: both comparisons are DOUBLE anyway
  bool operand_decimal = false;    // the operand is a DECIMAL (a decimal literal too)
  bool any_exact_bound = false;    // a bound that is not DOUBLE
  bool any_decimal_bound = false;  // a bound that is a DECIMAL column or expression
  std::optional<LogicalType> temporal;
  const auto wide = [](LogicalType t) {
    return t == LogicalType::kBigInt || t == LogicalType::kHugeInt;
  };
  for (const sql::Expr* value : {&*between.operand, &*between.low, &*between.high}) {
    const bool is_operand = value == &*between.operand;
    bool is_double = false;
    bool is_decimal = false;
    if (const auto* lit = std::get_if<sql::Literal>(value)) {
      const bool number =
          lit->kind == sql::Literal::Kind::kInteger || lit->kind == sql::Literal::Kind::kDecimal;
      if (number && IsApproximateNumber(lit->text)) {
        is_double = true;
      } else if (lit->kind == sql::Literal::Kind::kDecimal) {
        any_decimal = true;
        operand_decimal = operand_decimal || is_operand;
      } else if (lit->kind == sql::Literal::Kind::kInteger) {
        const auto exact = ParseExactNumber(lit->text, lit->negative);
        const bool huge = !exact.has_value() || exact->huge;
        // 38 digits or more: HUGEINT or DOUBLE in DuckDB; antb1 treats it as both.
        any_double = any_double || huge;
        any_wide = any_wide || huge || exact->magnitude > RangeOf(LogicalType::kInteger).max;
      }
    } else if (IsConstantInteger(*value)) {
      auto folded = FoldTyped(*value);
      any_wide = any_wide || (folded.has_value() && folded->ok() && wide((*folded)->type));
    } else {
      ARROW_ASSIGN_OR_RAISE(const Typed typed, bind(*value));
      const LogicalType type = typed.expr->type;
      any_varchar = any_varchar || type == LogicalType::kVarchar;
      if (type == LogicalType::kDate || type == LogicalType::kTimestamp) {
        temporal = type;
      }
      if (typed.stored_as_float) {
        any_float = true;
      } else if (type == LogicalType::kDouble) {
        is_double = true;
      } else if (type == LogicalType::kDecimal) {
        is_decimal = true;
        operand_decimal = operand_decimal || is_operand;
      } else if (IsInteger(type)) {
        any_integer = true;
        any_wide = any_wide || wide(type);
      }
    }
    any_double = any_double || is_double;
    operand_double = operand_double || (is_operand && is_double);
    any_exact_bound = any_exact_bound || (!is_operand && !is_double);
    any_decimal_bound = any_decimal_bound || (!is_operand && is_decimal);
  }
  // Pairwise, a string literal compares with a VARCHAR value and a DATE value alike; DuckDB
  // rejects the mix.
  if (any_varchar && temporal.has_value()) {
    return BindError(std::format("Cannot mix values of type VARCHAR and {} in BETWEEN clause",
                                 ToString(*temporal)),
                     between.op_span);
  }
  const bool decimal_pair = (operand_decimal && any_exact_bound) || any_decimal_bound;
  if (any_double && !operand_double &&
      (any_wide || any_float || (any_decimal && any_integer) || decimal_pair)) {
    return UnsupportedError(
        "BETWEEN of a DOUBLE value and a BIGINT, HUGEINT, FLOAT or DECIMAL value, or of a DOUBLE "
        "value, a decimal literal and an integer value, is not supported (DuckDB compares all "
        "three in DOUBLE)",
        between.op_span);
  }
  return arrow::Status::OK();
}

}  // namespace

template <class BindFn>
arrow::Result<Typed> Binder::BindBool(const sql::Expr& expr, const BindFn& bind, bool input) {
  const auto* binary = std::get_if<sql::BinaryExpr>(&expr);
  if (binary != nullptr &&
      (binary->op == sql::BinaryOp::kAnd || binary->op == sql::BinaryOp::kOr)) {
    // A chain of one operator is one node: a AND b AND c.
    std::vector<const sql::Expr*> chain;
    std::vector<const sql::Expr*> pending{&expr};
    while (!pending.empty()) {
      const sql::Expr* e = pending.back();
      pending.pop_back();
      const auto* b = std::get_if<sql::BinaryExpr>(e);
      if (b != nullptr && b->op == binary->op) {
        pending.push_back(&*b->right);
        pending.push_back(&*b->left);
      } else {
        chain.push_back(e);
      }
    }
    BoolExpr node{.op = binary->op == sql::BinaryOp::kAnd ? BoolOp::kAnd : BoolOp::kOr, .args = {}};
    for (const sql::Expr* e : chain) {
      ARROW_ASSIGN_OR_RAISE(Typed arg, BindBool(*e, bind, input));
      node.args.push_back(std::move(arg.expr));
    }
    return Leaf(
        Expr{.node = std::move(node), .type = LogicalType::kBoolean, .name = ExprName(expr)});
  }
  if (const auto* between = std::get_if<sql::BetweenExpr>(&expr)) {
    // operand >= low AND operand <= high, under a NOT when negated; named as DuckDB names it.
    ARROW_RETURN_NOT_OK(CheckBetweenTypes(*between, bind));
    BoolExpr both{.op = BoolOp::kAnd, .args = {}};
    for (const sql::Expr& comparison : BetweenComparisons(*between)) {
      ARROW_ASSIGN_OR_RAISE(Typed arg, BindBool(comparison, bind, input));
      both.args.push_back(std::move(arg.expr));
    }
    if (!between->negated) {
      return Leaf(
          Expr{.node = std::move(both), .type = LogicalType::kBoolean, .name = ExprName(expr)});
    }
    auto inner = std::make_shared<const Expr>(
        Expr{.node = std::move(both), .type = LogicalType::kBoolean, .name = ExprName(expr)});
    return Leaf(Expr{.node = BoolExpr{.op = BoolOp::kNot, .args = {std::move(inner)}},
                     .type = LogicalType::kBoolean,
                     .name = ExprName(expr)});
  }
  if (const auto* unary = std::get_if<sql::UnaryExpr>(&expr);
      unary != nullptr && unary->op == sql::UnaryOp::kNot) {
    ARROW_ASSIGN_OR_RAISE(Typed arg, BindBool(*unary->operand, bind, input));
    return Leaf(Expr{.node = BoolExpr{.op = BoolOp::kNot, .args = {std::move(arg.expr)}},
                     .type = LogicalType::kBoolean,
                     .name = ExprName(expr)});
  }
  std::vector<ExprPtr> operands;
  const auto column_of = [&](const Typed& t) {
    operands.push_back(t.expr);
    return BoundColumn{
        .index = Narrow<int>(operands.size() - 1), .name = t.expr->name, .type = t.expr->type};
  };
  ARROW_ASSIGN_OR_RAISE(Predicate predicate,
                        BindConditionWith(expr, input, bind, column_of, /*nested=*/true));
  if (!predicate.column.has_value()) {
    if (operands.empty()) {
      return arrow::Status::Invalid("a condition without an operand");
    }
    // A folded [NOT] LIKE: NULL for a NULL operand, as the comparison it replaces.
    predicate.column =
        BoundColumn{.index = 0, .name = operands[0]->name, .type = operands[0]->type};
  }
  return Leaf(Expr{
      .node = PredicateExpr{.predicate = std::move(predicate), .operands = std::move(operands)},
      .type = LogicalType::kBoolean,
      .name = ExprName(expr)});
}

namespace {

// The DECIMAL DuckDB gives two CASE values of which one is DECIMAL and the other a DECIMAL or an
// integer type (ADR 0021 rule 10): with e the larger number of integer digits and s the larger
// scale, DECIMAL(e + s, s), or DECIMAL(38, 38 - e) beyond 38 digits (the values are rounded); an
// integer counts by its type's digits (SMALLINT 5, INTEGER 10, BIGINT 19, HUGEINT 38) and keeps
// the DECIMAL's scale, so beyond 38 digits its large values fail to cast.
LogicalType CaseDecimal(LogicalType decimal, LogicalType other) {
  constexpr int kMax = LogicalType::kMaxDecimalWidth;
  const int p = decimal.width();
  const int s = decimal.scale();
  if (other != LogicalType::kDecimal) {
    const int digits = DecimalOfInteger(other).width();
    return digits <= p - s ? decimal
                           : LogicalType::Decimal(Narrow<std::uint8_t>(std::min(kMax, digits + s)),
                                                  Narrow<std::uint8_t>(s));
  }
  const int integers = std::max(p - s, other.width() - other.scale());
  const int scale = std::max<int>(s, other.scale());
  if (integers + scale <= kMax) {
    return LogicalType::Decimal(Narrow<std::uint8_t>(integers + scale),
                                Narrow<std::uint8_t>(scale));
  }
  return LogicalType::Decimal(Narrow<std::uint8_t>(kMax), Narrow<std::uint8_t>(kMax - integers));
}

// The type DuckDB gives two CASE values (a string literal takes the other's type elsewhere).
arrow::Result<LogicalType> CaseCommon(LogicalType a, LogicalType b, SourceSpan span) {
  if (a == b) {
    return a;
  }
  if (a == LogicalType::kDecimal || b == LogicalType::kDecimal) {
    const LogicalType decimal = a == LogicalType::kDecimal ? a : b;
    const LogicalType other = a == LogicalType::kDecimal ? b : a;
    if (other == LogicalType::kDouble) {
      return LogicalType::kDouble;
    }
    if (other == LogicalType::kDecimal || IsInteger(other)) {
      return CaseDecimal(decimal, other);
    }
  }
  if (IsInteger(a) && IsInteger(b)) {
    // Unlike arithmetic, USMALLINT with SMALLINT is INTEGER here.
    const bool mixed = (a == LogicalType::kUSmallInt && b == LogicalType::kSmallInt) ||
                       (a == LogicalType::kSmallInt && b == LogicalType::kUSmallInt);
    return mixed ? LogicalType::kInteger : CombineIntegers(a, b);
  }
  if (IsNumeric(a) && IsNumeric(b)) {
    return LogicalType::kDouble;
  }
  if ((a == LogicalType::kDate && b == LogicalType::kTimestamp) ||
      (a == LogicalType::kTimestamp && b == LogicalType::kDate)) {
    return UnsupportedError("CASE values of types DATE and TIMESTAMP are not supported", span);
  }
  return BindError(
      std::format("cannot mix values of type {} and {} in CASE", ToString(a), ToString(b)), span);
}

bool IsStringLiteral(const sql::Expr& expr) {
  const auto* lit = std::get_if<sql::Literal>(&expr);
  return lit != nullptr && lit->kind == sql::Literal::Kind::kString;
}

// An integer literal as DuckDB's CASE typing sees one: plain, parenthesized or under unary minus
// (DuckDB folds every minus of a literal into it), with its value and its type by value (INTEGER,
// BIGINT or HUGEINT); `1 + 1` and `7 + 0` are none.
std::optional<TypedConstant> CaseIntegerLiteral(const sql::Expr& expr) {
  const sql::Expr* inner = &expr;
  for (const auto* unary = std::get_if<sql::UnaryExpr>(inner);
       unary != nullptr && unary->op == sql::UnaryOp::kNegate;
       unary = std::get_if<sql::UnaryExpr>(inner)) {
    inner = &*unary->operand;
  }
  const auto* lit = std::get_if<sql::Literal>(inner);
  if (lit == nullptr || lit->kind != sql::Literal::Kind::kInteger ||
      IsApproximateNumber(lit->text)) {
    return std::nullopt;
  }
  auto folded = FoldTyped(expr);
  if (!folded.has_value() || !folded->ok()) {
    return std::nullopt;
  }
  return **folded;
}

// DuckDB's type of CASE values of which one is DECIMAL (ADR 0021 rule 10), folded from the ELSE
// value's type as it is (a literal stays one; NULL without an ELSE) through the THEN values in
// written order, so it can depend on their order. An integer literal takes an integer type it fits,
// else the two types' common one, and counts by its own type next to a DECIMAL; a string literal
// takes the type folded so far; next to NULL or another literal either becomes a plain type (the
// string VARCHAR, the pair the integer's type). Values of other types are combined by CaseCommon,
// the type folded so far named first in an error, as by DuckDB.
arrow::Result<LogicalType> DecimalCaseType(const std::vector<Typed>& values,
                                           const std::vector<const sql::Expr*>& exprs,
                                           bool has_else) {
  struct Folded {
    enum class Kind : std::uint8_t { kNull, kPlain, kInteger, kString };
    Kind kind = Kind::kNull;
    LogicalType type = LogicalType::kVarchar;  // kPlain, and kInteger's own type
    Int128 value = 0;                          // kInteger
  };
  std::vector<std::size_t> order;
  if (has_else) {
    order.push_back(values.size() - 1);
  }
  for (std::size_t i = 0; i + (has_else ? 1 : 0) < values.size(); ++i) {
    order.push_back(i);
  }
  using enum Folded::Kind;
  Folded folded;
  const sql::Expr* string_literal = nullptr;
  for (const std::size_t i : order) {
    const sql::Expr& expr = *exprs[i];
    const SourceSpan span = expr.span();
    const bool first = &expr == exprs[order.front()];
    if (values[i].stored_as_float) {
      return UnsupportedError(
          "a FLOAT column as a CASE value is not supported (antb1 reads FLOAT as DOUBLE, "
          "divergence D11)",
          span);
    }
    Folded next{.kind = kPlain, .type = values[i].expr->type, .value = 0};
    if (IsStringLiteral(expr)) {
      next.kind = kString;
      if (string_literal == nullptr) {
        string_literal = &expr;
      }
    } else if (const auto literal = CaseIntegerLiteral(expr)) {
      next = Folded{.kind = kInteger, .type = literal->type, .value = literal->value};
    }
    if (first && has_else) {
      folded = next;
      continue;
    }
    if (folded.kind == kNull || (folded.kind != kPlain && next.kind != kPlain)) {
      // Next to NULL or another literal: a plain type, the string VARCHAR, two literals the
      // integer's type (or VARCHAR for two strings).
      LogicalType plain = next.type;
      if (folded.kind == kInteger && next.kind == kInteger) {
        plain = CombineIntegers(folded.type, next.type);
      } else if (folded.kind == kInteger) {
        plain = folded.type;
      } else if (next.kind == kString) {
        plain = LogicalType::kVarchar;
      }
      folded = Folded{.kind = kPlain, .type = plain, .value = 0};
      continue;
    }
    if (next.kind == kString) {
      continue;  // the string takes the folded type (a literal one stays a literal)
    }
    if (folded.kind == kString) {
      folded = next;  // a plain type, which the string takes
      continue;
    }
    // A plain type and a plain type or an integer literal; the literal takes an integer type it
    // fits.
    if (folded.kind == kInteger || next.kind == kInteger) {
      const Int128 value = folded.kind == kInteger ? folded.value : next.value;
      const LogicalType plain = folded.kind == kInteger ? next.type : folded.type;
      const IntegerRange range = IsInteger(plain) ? RangeOf(plain) : IntegerRange{};
      if (IsInteger(plain) && value >= range.min && value <= range.max) {
        folded = Folded{.kind = kPlain, .type = plain, .value = 0};
        continue;
      }
    }
    ARROW_ASSIGN_OR_RAISE(const LogicalType type, CaseCommon(folded.type, next.type, span));
    folded = Folded{.kind = kPlain, .type = type, .value = 0};
  }
  if (string_literal != nullptr) {
    // DuckDB casts the string to the number.
    return UnsupportedError(std::format("a string literal as a CASE value next to {} values is not "
                                        "supported",
                                        ToString(folded.type)),
                            string_literal->span());
  }
  return folded.type;
}

}  // namespace

template <class BindFn>
arrow::Result<Typed> Binder::BindCase(const sql::CaseExpr& c, const BindFn& bind, bool input) {
  CaseExpr node;
  std::vector<Typed> values;
  std::vector<const sql::Expr*> value_exprs;
  for (const sql::CaseBranch& branch : c.branches) {
    Typed when;
    if (c.operand.has_value()) {
      const sql::Expr equal(sql::BinaryExpr{.op = sql::BinaryOp::kEq,
                                            .left = sql::Box<sql::Expr>(**c.operand),
                                            .right = sql::Box<sql::Expr>(*branch.when),
                                            .op_span = branch.when->span(),
                                            .span = branch.when->span()});
      ARROW_ASSIGN_OR_RAISE(when, BindBool(equal, bind, input));
    } else {
      ARROW_ASSIGN_OR_RAISE(when, BindBool(*branch.when, bind, input));
    }
    node.whens.push_back(std::move(when.expr));
    ARROW_ASSIGN_OR_RAISE(Typed then, bind(*branch.then));
    values.push_back(std::move(then));
    value_exprs.push_back(&*branch.then);
  }
  if (c.otherwise.has_value()) {
    ARROW_ASSIGN_OR_RAISE(Typed otherwise, bind(**c.otherwise));
    values.push_back(std::move(otherwise));
    value_exprs.push_back(&**c.otherwise);
  }
  // With a DECIMAL value (a decimal literal too), DuckDB's fold of rule 10 types the CASE, and each
  // value is cast to it on the rows that take it; a DOUBLE value makes it DOUBLE, the DECIMALs
  // converted as DuckDB converts them (ADR 0021 rules 8 and 10).
  if (std::ranges::any_of(values,
                          [](const Typed& v) { return v.expr->type == LogicalType::kDecimal; })) {
    ARROW_ASSIGN_OR_RAISE(const LogicalType type,
                          DecimalCaseType(values, value_exprs, c.otherwise.has_value()));
    for (std::size_t i = 0; i < c.branches.size(); ++i) {
      node.thens.push_back(std::move(values[i].expr));
    }
    if (c.otherwise.has_value()) {
      node.otherwise = std::move(values.back().expr);
    }
    return Leaf(Expr{.node = std::move(node), .type = type, .name = CaseName(c)});
  }
  // Otherwise DuckDB's type is the values' common type, where an integer literal takes the others'
  // integer type when it fits and a string literal any type (DATE: the date it spells); antb1 takes
  // the non-literal values first, so some orders type differently (divergence D20).
  std::optional<LogicalType> type;
  for (const bool literals : {false, true}) {
    for (std::size_t i = 0; i < values.size(); ++i) {
      const Typed& v = values[i];
      if (v.literal != literals || IsStringLiteral(*value_exprs[i])) {
        continue;
      }
      const SourceSpan span = value_exprs[i]->span();
      if (v.stored_as_float) {
        return UnsupportedError(
            "a FLOAT column as a CASE value is not supported (antb1 reads FLOAT as DOUBLE, "
            "divergence D11)",
            span);
      }
      if (!type.has_value()) {
        type = v.expr->type;
      } else if (!(v.literal && IsInteger(*type) && Fits(*v.expr, *type))) {
        ARROW_ASSIGN_OR_RAISE(type, CaseCommon(*type, v.expr->type, span));
      }
    }
  }
  for (std::size_t i = 0; i < values.size(); ++i) {
    if (!IsStringLiteral(*value_exprs[i])) {
      continue;
    }
    const auto& lit = std::get<sql::Literal>(*value_exprs[i]);
    if (!type.has_value()) {
      type = LogicalType::kVarchar;
    }
    if (*type == LogicalType::kDate) {
      const auto days = ParseDate(lit.text);
      if (!days.has_value()) {
        return BindError("invalid date '" + Clip(lit.text) + "': expected YYYY-MM-DD", lit.span);
      }
      values[i] = Leaf(Expr{.node = ConstantExpr{.value = Constant{.type = LogicalType::kDate,
                                                                   .value = Int128{*days}}},
                            .type = LogicalType::kDate,
                            .name = values[i].expr->name});
    } else if (*type == LogicalType::kTimestamp) {
      const auto micros = ParseTimestamp(lit.text);
      if (!micros.has_value()) {
        return BindError(std::string(kInvalidTimestamp) + " '" + Clip(lit.text) + "': expected " +
                             std::string(kTimestampForm),
                         lit.span);
      }
      values[i] = Leaf(Expr{.node = ConstantExpr{.value = Constant{.type = LogicalType::kTimestamp,
                                                                   .value = Int128{*micros}}},
                            .type = LogicalType::kTimestamp,
                            .name = values[i].expr->name});
    } else if (IsNumeric(*type)) {
      // DuckDB casts the string to the number ('1' next to SMALLINT is 1).
      return UnsupportedError(std::format("a string literal as a CASE value next to {} values is "
                                          "not supported",
                                          ToString(*type)),
                              lit.span);
    } else if (*type != LogicalType::kVarchar) {
      ARROW_RETURN_NOT_OK(CaseCommon(*type, LogicalType::kVarchar, lit.span));
    }
  }
  for (std::size_t i = 0; i < c.branches.size(); ++i) {
    node.thens.push_back(std::move(values[i].expr));
  }
  if (c.otherwise.has_value()) {
    node.otherwise = std::move(values.back().expr);
  }
  const LogicalType result = type.value_or(LogicalType::kVarchar);
  return Leaf(Expr{.node = std::move(node), .type = result, .name = CaseName(c)});
}

namespace {

// A conjunct that is no single predicate (OR, NOT): computed as a BOOLEAN column.
bool IsCompound(const sql::Expr& conjunct) {
  const auto* binary = std::get_if<sql::BinaryExpr>(&conjunct);
  const auto* unary = std::get_if<sql::UnaryExpr>(&conjunct);
  // A plain BETWEEN is two conjuncts (its comparisons), a NOT BETWEEN one compound condition.
  const auto* between = std::get_if<sql::BetweenExpr>(&conjunct);
  return (binary != nullptr && binary->op == sql::BinaryOp::kOr) ||
         (unary != nullptr && unary->op == sql::UnaryOp::kNot) ||
         (between != nullptr && between->negated);
}

Predicate IsTrue(BoundColumn column, SourceSpan span) {
  return Predicate{.kind = Predicate::Kind::kIsTrue,
                   .column = std::move(column),
                   .op = CompareOp::kEq,
                   .constant = {},
                   .values = {},
                   .span = span};
}

}  // namespace

arrow::Status Binder::BindWhere() {
  for (const sql::Expr* conjunct : Conjuncts(stmt_.where)) {
    if (IsCompound(*conjunct)) {
      ARROW_ASSIGN_OR_RAISE(
          const Typed condition,
          BindBool(*conjunct, [this](const sql::Expr& e) { return BindInput(e); }, /*input=*/true));
      input_filter_.push_back(IsTrue(InputColumn(condition, /*where=*/true), conjunct->span()));
      continue;
    }
    // A plain BETWEEN is its two comparisons, each folded and pushed into the scan where it can.
    std::vector<sql::Expr> comparisons;
    if (const auto* between = std::get_if<sql::BetweenExpr>(conjunct)) {
      ARROW_RETURN_NOT_OK(
          CheckBetweenTypes(*between, [this](const sql::Expr& e) { return BindInput(e); }));
      for (sql::Expr& comparison : BetweenComparisons(*between)) {
        comparisons.push_back(std::move(comparison));
      }
    }
    const std::vector<const sql::Expr*> parts =
        comparisons.empty()
            ? std::vector<const sql::Expr*>{conjunct}
            : std::vector<const sql::Expr*>{&comparisons.front(), &comparisons.back()};
    for (const sql::Expr* part : parts) {
      ARROW_ASSIGN_OR_RAISE(Predicate predicate, BindCondition(*part, /*having=*/false));
      const bool on_scan =
          (!predicate.column.has_value() || columns_.FieldOf(predicate.column->id).has_value()) &&
          (!predicate.other.has_value() || columns_.FieldOf(predicate.other->id).has_value());
      (on_scan ? scan_filter_ : input_filter_).push_back(std::move(predicate));
    }
  }
  return arrow::Status::OK();
}

// The GROUP BY keys: an expression over the table's columns (a table column first), or else the
// alias of a select item that is not an aggregate (the last item with that alias, as in DuckDB),
// or a position in the select list; duplicates are dropped. A constant (a constant item, by alias
// or position, or any other literal) is no key: it still makes the query grouped (one group if
// there is a row at all).
arrow::Status Binder::BindGroupBy() {
  const auto add = [&](const Typed& t) {
    const bool duplicate = std::ranges::any_of(
        key_exprs_, [&](const ExprPtr& key) { return SameExpr(*key, *t.expr); });
    if (!duplicate) {
      keys_.push_back(InputColumn(t));
      key_ids_.push_back(ids_.Next());
      key_exprs_.push_back(t.expr);
    }
  };
  // Select item i as a key: its expression, nothing for a constant, an error for an aggregate.
  const auto item_key = [&](std::size_t i, std::string_view what,
                            SourceSpan span) -> arrow::Status {
    const auto [kind, index] = select_.items[i];
    switch (kind) {
      case ItemKind::kAggregate:
        return BindError(std::format("GROUP BY cannot refer to the aggregate {}", what), span);
      case ItemKind::kConstant:
        break;
      case ItemKind::kColumn: {
        const BoundColumn& column = select_.columns[index];
        add(ColumnLeaf(column.id, column.type, column.name, false));
        break;
      }
      case ItemKind::kExpression: {
        const sql::Expr& expr = *select_.exprs[index];
        if (ContainsAggregate(expr)) {
          return BindError(std::format("GROUP BY cannot refer to the aggregate {}", what), span);
        }
        ARROW_ASSIGN_OR_RAISE(const Typed t, BindInput(expr));
        if (!IsConstantExpr(*t.expr)) {
          add(t);
        }
        break;
      }
    }
    return arrow::Status::OK();
  };
  for (const sql::GroupExpr& expr : stmt_.group_by) {
    if (const auto* lit = std::get_if<sql::Literal>(&expr)) {
      ARROW_ASSIGN_OR_RAISE(const auto position, PositionOf(*lit, select_, "GROUP BY"));
      if (position.has_value()) {
        ARROW_RETURN_NOT_OK(
            item_key(*position, std::format("at position {}", *position + 1), lit->span));
      }
      continue;
    }
    if (const auto* ref = std::get_if<sql::ColumnRef>(&expr)) {
      auto column = columns_.Resolve(*ref);
      if (column.ok()) {
        add(ColumnLeaf(column->id, column->type, column->name, false));
        continue;
      }
      const auto detail = GetSqlError(column.status());
      if (detail == nullptr || detail->kind() != SqlErrorDetail::Kind::kBind) {
        return column.status();
      }
      const auto alias = FindAlias(select_, ref->name);
      if (!alias.has_value()) {
        return column.status();
      }
      ARROW_RETURN_NOT_OK(
          item_key(*alias, std::format("'{}'", select_.aliases[*alias].value_or("")), ref->span));
      continue;
    }
    ARROW_ASSIGN_OR_RAISE(const Typed t, BindInput(expr));
    std::vector<ColumnId> reads;
    CollectColumnIds(*t.expr, reads);
    if (reads.empty()) {
      return UnsupportedError("GROUP BY a constant expression is not supported", expr.span());
    }
    add(t);
  }
  return arrow::Status::OK();
}

// Every plain column of a grouped select list must be a key (as in DuckDB).
arrow::Status Binder::CheckGrouped() {
  for (std::size_t i = 0; i < select_.columns.size(); ++i) {
    if (!KeyOf(select_.columns[i].id).has_value()) {
      return BindError(std::format("column '{}' must appear in the GROUP BY clause or be inside an "
                                   "aggregate function",
                                   select_.column_written[i]),
                       select_.column_spans[i]);
    }
  }
  return arrow::Status::OK();
}

// The select items that are neither columns, aggregates nor constants, in the query's scope; each
// gets its column (computed unless it is one already).
arrow::Status Binder::BindSelectExpressions() {
  for (std::size_t i = 0; i < select_.items.size(); ++i) {
    const auto [kind, index] = select_.items[i];
    if (kind != ItemKind::kExpression) {
      continue;
    }
    ARROW_ASSIGN_OR_RAISE(const Typed t, BindScoped(*select_.exprs[index]));
    BoundColumn column = ScopedColumn(t);
    select_.output[i].type = t.expr->type;
    if (!select_.aliases[i].has_value()) {
      select_.output[i].name = t.expr->name;
    }
    column.name = select_.output[i].name;
    select_.expr_columns.push_back(std::move(column));
  }
  return arrow::Status::OK();
}

arrow::Result<std::optional<Typed>> Binder::ItemOutput(std::size_t i) {
  const auto [kind, index] = select_.items[i];
  switch (kind) {
    case ItemKind::kConstant:
      return std::nullopt;
    case ItemKind::kAggregate:
      return std::optional(ColumnLeaf(select_.aggregates[index].id, select_.aggregates[index].type,
                                      select_.output[i].name, false));
    case ItemKind::kExpression: {
      const BoundColumn& column = select_.expr_columns[index];
      return std::optional(ColumnLeaf(column.id, column.type, column.name, false));
    }
    case ItemKind::kColumn:
      break;
  }
  const BoundColumn& column = select_.columns[index];
  if (shape_ == Shape::kProjection) {
    return std::optional(ColumnLeaf(column.id, column.type, column.name, StoredAsFloat(column.id)));
  }
  if (const std::optional<std::size_t> k = KeyOf(column.id)) {
    return std::optional(
        ColumnLeaf(key_ids_[*k], column.type, column.name, StoredAsFloat(column.id)));
  }
  return NotGrouped(select_.column_written[index], select_.column_spans[index]);
}

// A name in HAVING: a table column that is a GROUP BY key first, else the last select item with
// that alias (DuckDB): a key, an aggregate or an expression. std::nullopt: bind it as any other
// operand.
arrow::Result<std::optional<Typed>> Binder::ResolveHavingName(const sql::ColumnRef& ref) {
  auto table_column = columns_.Resolve(ref);
  if (table_column.ok()) {
    if (const std::optional<std::size_t> k = KeyOf(table_column->id)) {
      return std::optional(ColumnLeaf(key_ids_[*k], table_column->type, table_column->name,
                                      StoredAsFloat(table_column->id)));
    }
  }
  const auto alias = FindAlias(select_, ref.name);
  if (!alias.has_value()) {
    return std::nullopt;
  }
  if (select_.items[*alias].first == ItemKind::kConstant) {
    return UnsupportedError("HAVING on a constant select item is not supported", ref.span);
  }
  ARROW_ASSIGN_OR_RAISE(auto item, ItemOutput(*alias));
  if (item.has_value() && select_.items[*alias].first == ItemKind::kAggregate) {
    // DuckDB's MIN and MAX of a FLOAT column are FLOAT.
    item->stored_as_float = FloatResult(select_.aggregates[select_.items[*alias].second]);
  }
  return item;
}

// HAVING conditions as predicates over the aggregation's output (and columns computed over it).
arrow::Status Binder::BindHaving() {
  alias_fallback_ = true;
  // A name binds as in BindCondition: a key or a select alias first. The alias of an expression
  // item is its expression: the condition is computed next to it, over the aggregation.
  const auto bind = [&](const sql::Expr& e) -> arrow::Result<Typed> {
    if (const auto* ref = std::get_if<sql::ColumnRef>(&e)) {
      ARROW_ASSIGN_OR_RAISE(auto resolved, ResolveHavingName(*ref));
      if (resolved.has_value()) {
        const auto* column = std::get_if<ColumnExpr>(&resolved->expr->node);
        const auto post =
            column == nullptr ? post_ids_.end() : std::ranges::find(post_ids_, column->id);
        if (post != post_ids_.end()) {
          resolved->expr = post_exprs_.at(Narrow<std::size_t>(post - post_ids_.begin()));
        }
        return *std::move(resolved);
      }
    }
    return BindOutput(e);
  };
  for (const sql::Expr* conjunct : Conjuncts(stmt_.having)) {
    if (IsCompound(*conjunct)) {
      ARROW_ASSIGN_OR_RAISE(const Typed condition, BindBool(*conjunct, bind, /*input=*/false));
      having_.push_back(IsTrue(OutputColumn(condition), conjunct->span()));
      continue;
    }
    if (const auto* between = std::get_if<sql::BetweenExpr>(conjunct)) {
      ARROW_RETURN_NOT_OK(CheckBetweenTypes(*between, bind));
      for (const sql::Expr& comparison : BetweenComparisons(*between)) {
        ARROW_ASSIGN_OR_RAISE(Predicate predicate, BindCondition(comparison, /*having=*/true));
        having_.push_back(std::move(predicate));
      }
      continue;
    }
    ARROW_ASSIGN_OR_RAISE(Predicate predicate, BindCondition(*conjunct, /*having=*/true));
    having_.push_back(std::move(predicate));
  }
  alias_fallback_ = false;
  return arrow::Status::OK();
}

// ORDER BY items in the query's scope. kGlobal checks the items and returns no key: one row needs
// no sort. A select alias comes before a table column, as in DuckDB, and a qualified name (t.x)
// never names an alias: that guard is for J2b (ADR 0022), since until then CheckSupported rejects
// a qualified ORDER BY item before the binder runs. An unsigned integer is a position in the select
// list; a constant (a constant item, or any other literal) orders nothing; a later key on a column
// already ordered by changes nothing and is dropped.
arrow::Status Binder::BindOrderBy() {
  alias_fallback_ = true;
  for (const sql::OrderItem& item : stmt_.order_by) {
    std::optional<Typed> key;
    if (const auto* lit = std::get_if<sql::Literal>(&item.expr)) {
      ARROW_ASSIGN_OR_RAISE(const auto position, PositionOf(*lit, select_, "ORDER BY"));
      if (position.has_value()) {
        ARROW_ASSIGN_OR_RAISE(key, ItemOutput(*position));
      }
    } else if (const auto* ref = std::get_if<sql::ColumnRef>(&item.expr);
               ref != nullptr && ref->qualifier.empty() &&
               FindAlias(select_, ref->name).has_value()) {  // J2b: t.x never names an alias
      const std::size_t alias = FindAlias(select_, ref->name).value_or(0);
      ARROW_ASSIGN_OR_RAISE(key, ItemOutput(alias));
    } else {
      // A global aggregate's only row needs no sort: its ORDER BY items are checked, and computed
      // or hidden aggregates for them dropped again.
      const std::size_t aggregates = select_.aggregates.size();
      const std::size_t posts = post_exprs_.size();
      ARROW_ASSIGN_OR_RAISE(const Typed t, BindScoped(item.expr));
      if (shape_ == Shape::kGlobal) {
        select_.aggregates.resize(aggregates);
        post_exprs_.resize(posts);
        post_ids_.resize(posts);
        continue;
      }
      std::vector<ColumnId> reads;
      CollectColumnIds(*t.expr, reads);
      if (reads.empty()) {
        return UnsupportedError("ORDER BY a constant expression is not supported",
                                item.expr.span());
      }
      key = t;
    }
    if (!key.has_value() || shape_ == Shape::kGlobal) {
      continue;
    }
    const BoundColumn column = ScopedColumn(*key);
    const bool repeated =
        std::ranges::any_of(sort_keys_, [&](const SortKey& k) { return k.column.id == column.id; });
    if (!repeated) {
      sort_keys_.push_back(SortKey{.column = column,
                                   .descending = item.descending,
                                   .nulls_first = item.nulls == sql::NullsOrder::kFirst});
    }
  }
  return arrow::Status::OK();
}

arrow::Result<LogicalPlan> Binder::Bind() {
  ARROW_RETURN_NOT_OK(BindSelectList());
  ARROW_RETURN_NOT_OK(BindWhere());
  if (!stmt_.group_by.empty()) {
    shape_ = Shape::kGrouped;
    ARROW_RETURN_NOT_OK(BindGroupBy());
    ARROW_RETURN_NOT_OK(CheckGrouped());
  } else if (IsAggregateQuery(stmt_) || !select_.aggregates.empty()) {
    shape_ = Shape::kGlobal;  // an aggregate anywhere or HAVING makes one row, even of constants
  }
  if (stmt_.limit.has_value() && *stmt_.limit < 0) {
    return BindError("LIMIT must not be negative", stmt_.limit_span);
  }
  if (stmt_.offset.has_value() && *stmt_.offset < 0) {
    return BindError("OFFSET must not be negative", stmt_.offset_span);
  }
  ARROW_RETURN_NOT_OK(BindSelectExpressions());
  ARROW_RETURN_NOT_OK(BindHaving());
  ARROW_RETURN_NOT_OK(BindOrderBy());
  return Assemble();
}

LogicalPlan Binder::Assemble() {
  const sql::TableRef& from = stmt_.from.front().table;
  ScanNode scan{
      .table = table_, .table_name = from.name, .fields = {}, .ids = {}, .span = from.span};
  for (int i = 0; i < schema_.num_fields(); ++i) {
    scan.fields.push_back(i);
    scan.ids.push_back(columns_.Id(i));
  }
  LogicalNodePtr node = Make(std::move(scan));
  const SourceSpan where_span = stmt_.where.empty()
                                    ? SourceSpan{}
                                    : Cover(stmt_.where.front().span(), stmt_.where.back().span());
  if (!scan_filter_.empty()) {
    node = Make(FilterNode{
        .input = std::move(node), .predicates = std::move(scan_filter_), .span = where_span});
  }
  if (!where_exprs_.empty()) {
    node = Make(ComputeNode{
        .input = std::move(node), .exprs = where_exprs_, .ids = where_ids_, .span = where_span});
  }
  if (!input_filter_.empty()) {
    node = Make(FilterNode{
        .input = std::move(node), .predicates = std::move(input_filter_), .span = where_span});
  }
  if (!input_exprs_.empty()) {
    node = Make(ComputeNode{
        .input = std::move(node), .exprs = input_exprs_, .ids = input_ids_, .span = select_.span});
  }
  // Over the aggregation: computed columns, HAVING, the Sort, then a Project restores the select
  // order.
  const auto above = [&] {
    if (!post_exprs_.empty()) {
      node = Make(ComputeNode{
          .input = std::move(node), .exprs = post_exprs_, .ids = post_ids_, .span = select_.span});
    }
    if (!having_.empty()) {
      node =
          Make(FilterNode{.input = std::move(node),
                          .predicates = std::move(having_),
                          .span = Cover(stmt_.having.front().span(), stmt_.having.back().span())});
    }
    if (!sort_keys_.empty()) {
      node = Make(SortNode{
          .input = std::move(node), .keys = std::move(sort_keys_), .span = stmt_.order_by_span});
    }
  };
  // The select list over the node below it: a table column (the column that `column_of` gives
  // for it in that node), an aggregate (its call's column), a constant or a (computed) expression.
  // Every output column is a new column.
  const auto project = [&](const auto& column_of) {
    ProjectNode out{
        .input = std::move(node), .columns = {}, .constants = {}, .ids = {}, .span = select_.span};
    for (std::size_t i = 0; i < select_.items.size(); ++i) {
      const auto [kind, index] = select_.items[i];
      std::optional<Constant> constant;
      BoundColumn column{.index = -1,
                         .id = kNoColumnId,
                         .name = select_.output[i].name,
                         .type = select_.output[i].type};
      switch (kind) {
        case ItemKind::kColumn:
          column.id = column_of(select_.columns[index].id);
          column.name = select_.columns[index].name;
          break;
        case ItemKind::kAggregate:
          column.id = select_.aggregates[index].id;
          break;
        case ItemKind::kConstant:
          constant = select_.constants[index];
          break;
        case ItemKind::kExpression:
          column.id = select_.expr_columns[index].id;
          break;
      }
      out.columns.push_back(std::move(column));
      out.constants.push_back(std::move(constant));
      out.ids.push_back(ids_.Next());
      select_.output[i].id = out.ids.back();
    }
    if (select_.constants.empty()) {
      out.constants.clear();
    }
    node = Make(std::move(out));
  };
  switch (shape_) {
    case Shape::kGrouped: {
      // GroupAggregate outputs the keys, then the aggregates (hidden HAVING and ORDER BY ones
      // last).
      node = Make(GroupAggregateNode{.input = std::move(node),
                                     .keys = std::move(keys_),
                                     .key_ids = key_ids_,
                                     .aggregates = select_.aggregates,
                                     .span = stmt_.group_by_span});
      above();
      // A plain column is the key it groups by (CheckGrouped made sure there is one).
      project([&](ColumnId field) { return key_ids_.at(KeyOf(field).value_or(key_ids_.size())); });
      break;
    }
    case Shape::kProjection:
      above();
      project([](ColumnId field) { return field; });
      break;
    case Shape::kGlobal: {
      const bool hidden =
          select_.aggregates.size() >
          static_cast<std::size_t>(std::ranges::count_if(
              select_.items, [](const auto& item) { return item.first == ItemKind::kAggregate; }));
      node = Make(AggregateNode{
          .input = std::move(node), .aggregates = select_.aggregates, .span = select_.span});
      above();
      // A Project restores the select list when the Aggregate's output is not it (constants,
      // expressions, hidden aggregates, or columns computed for HAVING).
      if (!select_.constants.empty() || !select_.exprs.empty() || hidden || !post_exprs_.empty()) {
        project([](ColumnId column) { return column; });
      } else {
        for (std::size_t i = 0; i < select_.items.size(); ++i) {
          select_.output[i].id = select_.aggregates[select_.items[i].second].id;  // the call itself
        }
      }
      break;
    }
  }
  const int64_t offset = stmt_.offset.value_or(0);
  if (stmt_.limit.has_value() || offset > 0) {
    SourceSpan span = stmt_.limit.has_value() ? stmt_.limit_span : stmt_.offset_span;
    if (stmt_.limit.has_value() && stmt_.offset.has_value()) {
      span = stmt_.limit_span.offset < stmt_.offset_span.offset
                 ? Cover(stmt_.limit_span, stmt_.offset_span)
                 : Cover(stmt_.offset_span, stmt_.limit_span);
    }
    node = Make(
        LimitNode{.input = std::move(node), .limit = stmt_.limit, .offset = offset, .span = span});
  }
  // The output ids were recorded where the columns were made (the Project, or the Aggregate's
  // calls); ResolvePositions checks them against what the root outputs.
  return LogicalPlan{.root = std::move(node), .output = std::move(select_.output)};
}

// CAST('YYYY-MM-DD' AS DATE) and 'YYYY-MM-DD'::DATE are the literal DATE 'YYYY-MM-DD' in DuckDB,
// with the same name and value: the literal, spanning the cast. std::nullopt for any other cast,
// which CheckSupported rejects.
std::optional<sql::Literal> DateLiteralOf(const sql::CastExpr& cast) {
  const auto* lit = std::get_if<sql::Literal>(&*cast.operand);
  if (cast.try_cast || cast.type != "DATE" || !cast.type_params.empty() || lit == nullptr ||
      lit->kind != sql::Literal::Kind::kString) {
    return std::nullopt;
  }
  return sql::Literal{
      .kind = sql::Literal::Kind::kDate, .negative = false, .text = lit->text, .span = cast.span};
}

void FoldDateCasts(sql::Expr& expr);

struct FoldDateCastsOf {
  void operator()(sql::ColumnRef& /*column*/) const {}
  void operator()(sql::Literal& /*lit*/) const {}
  void operator()(sql::AggregateCall& call) const {
    if (call.arg.has_value()) {
      FoldDateCasts(**call.arg);
    }
  }
  void operator()(sql::UnaryExpr& unary) const { FoldDateCasts(*unary.operand); }
  void operator()(sql::BinaryExpr& binary) const {
    FoldDateCasts(*binary.left);
    FoldDateCasts(*binary.right);
  }
  void operator()(sql::LikeExpr& like) const {
    FoldDateCasts(*like.operand);
    FoldDateCasts(*like.pattern);
  }
  void operator()(sql::InExpr& in) const {
    FoldDateCasts(*in.operand);
    for (sql::Expr& value : in.list) {
      FoldDateCasts(value);
    }
  }
  void operator()(sql::BetweenExpr& between) const {
    FoldDateCasts(*between.operand);
    FoldDateCasts(*between.low);
    FoldDateCasts(*between.high);
  }
  void operator()(sql::FunctionCall& call) const {
    for (sql::Expr& arg : call.args) {
      FoldDateCasts(arg);
    }
  }
  void operator()(sql::CaseExpr& c) const {
    if (c.operand.has_value()) {
      FoldDateCasts(**c.operand);
    }
    for (sql::CaseBranch& branch : c.branches) {
      FoldDateCasts(*branch.when);
      FoldDateCasts(*branch.then);
    }
    if (c.otherwise.has_value()) {
      FoldDateCasts(**c.otherwise);
    }
  }
  void operator()(sql::ExtractExpr& e) const { FoldDateCasts(*e.source); }
  void operator()(sql::CastExpr& cast) const { FoldDateCasts(*cast.operand); }
};

// Every date cast of a string literal in `expr` as its DATE literal, innermost first (a cast of
// that literal again stays a cast).
void FoldDateCasts(sql::Expr& expr) {
  std::visit(FoldDateCastsOf{}, static_cast<sql::ExprNode&>(expr));
  if (const auto* cast = std::get_if<sql::CastExpr>(&expr)) {
    if (std::optional<sql::Literal> date = DateLiteralOf(*cast)) {
      expr = sql::Expr(*std::move(date));
    }
  }
}

sql::SelectStatement FoldDateCasts(sql::SelectStatement stmt) {
  for (sql::SelectItem& item : stmt.items) {
    FoldDateCasts(item.expr);
  }
  for (std::vector<sql::Expr>* clause : {&stmt.where, &stmt.group_by, &stmt.having}) {
    for (sql::Expr& expr : *clause) {
      FoldDateCasts(expr);
    }
  }
  for (sql::OrderItem& item : stmt.order_by) {
    FoldDateCasts(item.expr);
  }
  return stmt;
}

// The FROM list the binder answers: one table or path without an alias. The others parse (ADR
// 0022) and are answered from J2b on: the first item's alias, else the second item's connector.
std::optional<Rejection> RejectFromList(const std::vector<sql::FromItem>& from) {
  ANTB1_CHECK(!from.empty());  // the parser makes no statement without a FROM item
  if (const sql::FromItem& first = from.front(); first.alias.has_value()) {
    return Rejection{.span = first.alias_span, .message = "table aliases are not supported"};
  }
  if (from.size() == 1) {
    return std::nullopt;
  }
  const sql::FromItem& second = from[1];
  std::string message;
  switch (second.connector) {
    case sql::Connector::kComma:
    case sql::Connector::kFirst:  // only the first item has it
      message = "a FROM list of several tables is not supported";
      break;
    case sql::Connector::kCross:
      message = "CROSS JOIN is not supported";
      break;
    case sql::Connector::kInner:
      message = "JOIN ... ON is not supported";
      break;
    case sql::Connector::kLeft:
      message = "LEFT JOIN is not supported";
      break;
  }
  return Rejection{.span = second.connector_span, .message = std::move(message)};
}

// Expressions and FROM lists the binder does not answer yet are kUnsupported, reported (like the
// parser's own kUnsupported errors) before any name is resolved, so also over tables that do not
// exist, at the first one in query order.
arrow::Status CheckSupported(const sql::SelectStatement& stmt) {
  for (const sql::SelectItem& item : stmt.items) {
    ARROW_RETURN_NOT_OK(CheckValue(item.expr));
  }
  if (auto r = RejectFromList(stmt.from)) {
    return Reject(*r);
  }
  for (const sql::Expr& conjunct : stmt.where) {
    if (auto r = RejectCondition(conjunct, /*having=*/false)) {
      return Reject(*r);
    }
  }
  for (const sql::Expr& expr : stmt.group_by) {
    ARROW_RETURN_NOT_OK(CheckValue(expr));
  }
  for (const sql::Expr& conjunct : stmt.having) {
    if (auto r = RejectCondition(conjunct, /*having=*/true)) {
      return Reject(*r);
    }
  }
  for (const sql::OrderItem& item : stmt.order_by) {
    ARROW_RETURN_NOT_OK(CheckValue(item.expr));
  }
  return arrow::Status::OK();
}

}  // namespace

arrow::Result<LogicalPlan> Bind(const sql::SelectStatement& stmt, const Catalog& catalog) {
  const sql::SelectStatement folded = FoldDateCasts(stmt);  // the binder refers to it
  ARROW_RETURN_NOT_OK(CheckSupported(folded));
  ANTB1_CHECK(folded.from.size() == 1);  // CheckSupported rejected the others
  ARROW_ASSIGN_OR_RAISE(auto table, ResolveTable(folded.from.front().table, catalog));
  ColumnIdSource ids;
  Binder binder(folded, std::move(table), ids);
  ARROW_ASSIGN_OR_RAISE(LogicalPlan plan, binder.Bind());
  return ResolvePositions(plan);  // the binder refers to columns by id (ADR 0022)
}

}  // namespace antb1::plan
