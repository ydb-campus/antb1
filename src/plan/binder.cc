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
#include "antb1/sql/unparse.h"

// The binding rules are documented in docs/sql-subset.md and
// docs/adr/0004-types-null-overflow-semantics.md.

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

// DuckDB's name of a literal inside an expression: numbers as their value (a decimal as written),
// strings quoted.
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
      break;
    case sql::Literal::Kind::kString:
    case sql::Literal::Kind::kDate:
      return "'" + lit.text + "'";
  }
  return (lit.negative ? "-" : "") + lit.text;
}

std::string ResultName(const sql::AggregateCall& call);

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

// DuckDB's name of an expression of the binder's subset: (a + 1), -(a), sum((a + 1)); conditions
// as ((a = 1) OR (b != 2) OR (c IN (1, 2))).
std::string ExprName(const sql::Expr& expr) {
  if (const auto* ref = std::get_if<sql::ColumnRef>(&expr)) {
    return ArgumentName(ref->name);
  }
  if (const auto* lit = std::get_if<sql::Literal>(&expr)) {
    return lit->kind == sql::Literal::Kind::kDate ? "CAST('" + lit->text + "' AS \"DATE\")"
                                                  : LiteralName(*lit);
  }
  if (const auto* call = std::get_if<sql::AggregateCall>(&expr)) {
    return ResultName(*call);
  }
  if (const auto* binary = std::get_if<sql::BinaryExpr>(&expr);
      binary != nullptr &&
      (binary->op == sql::BinaryOp::kAnd || binary->op == sql::BinaryOp::kOr)) {
    // A chain of one operator is one list: ((a) OR (b) OR (c)).
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
    std::string out;
    for (const sql::Expr* e : chain) {
      out += (out.empty() ? "(" : std::format(" {} ", sql::ToString(binary->op))) + ExprName(*e);
    }
    return out + ")";
  }
  if (const auto* binary = std::get_if<sql::BinaryExpr>(&expr)) {
    return std::format("({} {} {})", ExprName(*binary->left), OperatorName(binary->op),
                       ExprName(*binary->right));
  }
  if (const auto* unary = std::get_if<sql::UnaryExpr>(&expr)) {
    return unary->op == sql::UnaryOp::kNot ? NotName(*unary->operand)
                                           : "-(" + ExprName(*unary->operand) + ")";
  }
  if (const auto* in = std::get_if<sql::InExpr>(&expr)) {
    return InName(*in, in->negated);
  }
  if (const auto* e = std::get_if<sql::ExtractExpr>(&expr)) {
    return std::format("main.date_part('{}', {})", AsciiLower(e->field), ExprName(*e->source));
  }
  if (const auto* call = std::get_if<sql::FunctionCall>(&expr)) {
    std::string args;
    for (const sql::Expr& arg : call->args) {
      args += (args.empty() ? "" : ", ") + ExprName(arg);
    }
    return AsciiLower(call->name) + "(" + args + ")";
  }
  if (const auto* c = std::get_if<sql::CaseExpr>(&expr)) {
    return CaseName(*c);
  }
  if (const auto* like = std::get_if<sql::LikeExpr>(&expr)) {
    return std::format("({} {} {})", ExprName(*like->operand), like->negated ? "!~~" : "~~",
                       ExprName(*like->pattern));
  }
  return sql::ToSql(expr);
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

// The date_trunc units and EXTRACT fields antb1 answers (DuckDB has more).
constexpr auto kTruncUnits = std::to_array<std::string_view>(
    {"year", "quarter", "month", "week", "day", "hour", "minute", "second"});
constexpr auto kExtractFields =
    std::to_array<std::string_view>({"year", "month", "day", "hour", "minute", "second"});

std::optional<FunctionSpec> FindFunction(const sql::FunctionCall& call) {
  const std::string name = AsciiLower(call.name);
  const auto* spec = std::ranges::find(kFunctions, name, &FunctionSpec::name);
  return spec == kFunctions.end() ? std::nullopt : std::optional(*spec);
}

bool ReadsColumn(const sql::Expr& expr, bool aggregates = false) {
  if (std::holds_alternative<sql::ColumnRef>(expr)) {
    return true;
  }
  if (const auto* call = std::get_if<sql::FunctionCall>(&expr)) {
    return std::ranges::any_of(call->args,
                               [&](const sql::Expr& a) { return ReadsColumn(a, aggregates); });
  }
  if (std::holds_alternative<sql::AggregateCall>(expr)) {
    return aggregates;
  }
  if (const auto* binary = std::get_if<sql::BinaryExpr>(&expr)) {
    return ReadsColumn(*binary->left, aggregates) || ReadsColumn(*binary->right, aggregates);
  }
  if (const auto* unary = std::get_if<sql::UnaryExpr>(&expr)) {
    return ReadsColumn(*unary->operand, aggregates);
  }
  if (const auto* like = std::get_if<sql::LikeExpr>(&expr)) {
    return ReadsColumn(*like->operand, aggregates) || ReadsColumn(*like->pattern, aggregates);
  }
  if (const auto* in = std::get_if<sql::InExpr>(&expr)) {
    return ReadsColumn(*in->operand, aggregates);
  }
  if (const auto* e = std::get_if<sql::ExtractExpr>(&expr)) {
    return ReadsColumn(*e->source, aggregates);
  }
  if (const auto* c = std::get_if<sql::CaseExpr>(&expr)) {
    const auto reads = [&](const sql::Expr& e) { return ReadsColumn(e, aggregates); };
    return (c->operand.has_value() && reads(**c->operand)) ||
           (c->otherwise.has_value() && reads(**c->otherwise)) ||
           std::ranges::any_of(c->branches, [&](const sql::CaseBranch& b) {
             return reads(*b.when) || reads(*b.then);
           });
  }
  return false;
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
  std::optional<Rejection> operator()(const sql::ColumnRef& /*column*/) const {
    return std::nullopt;
  }
  std::optional<Rejection> operator()(const sql::Literal& /*lit*/) const { return std::nullopt; }
  std::optional<Rejection> operator()(const sql::AggregateCall& call) const {
    if (!call.arg.has_value() || call.arg_column() != nullptr) {
      return std::nullopt;
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
          unit->kind == sql::Literal::Kind::kString &&
          std::ranges::find(kTruncUnits, AsciiLower(unit->text)) == kTruncUnits.end()) {
        return Rejection{.span = unit->span,
                         .message = std::format("date_trunc() with the unit '{}' is not supported "
                                                "(only year, quarter, month, week, day, hour, "
                                                "minute and second)",
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
    if (std::ranges::find(kExtractFields, AsciiLower(e.field)) == kExtractFields.end()) {
      return Rejection{.span = e.field_span,
                       .message = std::format("EXTRACT of {} is not supported (only year, month, "
                                              "day, hour, minute and second)",
                                              Clip(e.field))};
    }
    return FirstUnsupported(*e.source);
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

// Why a WHERE (or, with `having`, HAVING) condition is not one the binder answers, or std::nullopt
// when it is (AsComparison / AsHavingComparison accept it, or a conjunction of such).
std::optional<Rejection> RejectCondition(const sql::Expr& expr, bool having) {
  // An operand reads a column (in HAVING, or calls an aggregate); the other side of a comparison
  // is then an operand too, or a literal.
  const auto is_operand = [having](const sql::Expr& e) { return ReadsColumn(e, having); };
  const std::string_view operand_kind = having ? "a column or an aggregate" : "a column";
  if (const auto* binary = std::get_if<sql::BinaryExpr>(&expr)) {
    switch (binary->op) {
      case sql::BinaryOp::kAnd:
        if (auto r = RejectCondition(*binary->left, having)) {
          return r;
        }
        return RejectCondition(*binary->right, having);
      case sql::BinaryOp::kOr:
        if (auto r = RejectCondition(*binary->left, having)) {
          return r;
        }
        return RejectCondition(*binary->right, having);
      case sql::BinaryOp::kEq:
      case sql::BinaryOp::kNe:
      case sql::BinaryOp::kLt:
      case sql::BinaryOp::kLe:
      case sql::BinaryOp::kGt:
      case sql::BinaryOp::kGe: {
        if (auto r = FirstUnsupported(*binary->left)) {
          return r;
        }
        if (auto r = FirstUnsupported(*binary->right)) {
          return r;
        }
        const bool left = is_operand(*binary->left);
        const bool right = is_operand(*binary->right);
        if (!left && !right) {
          return Rejection{.span = binary->right->span(),
                           .message = "comparisons between two literals are not supported"};
        }
        for (const sql::Expr* side : {&*binary->left, &*binary->right}) {
          if (!is_operand(*side) && !std::holds_alternative<sql::Literal>(*side)) {
            return Rejection{.span = side->span(),
                             .message =
                                 "a constant expression in a comparison is not supported "
                                 "(write it as a literal)"};
          }
        }
        return std::nullopt;
      }
      default:
        break;
    }
  }
  if (const auto* unary = std::get_if<sql::UnaryExpr>(&expr);
      unary != nullptr && unary->op == sql::UnaryOp::kNot) {
    return RejectCondition(*unary->operand, having);
  }
  if (const auto* like = std::get_if<sql::LikeExpr>(&expr)) {
    if (auto r = FirstUnsupported(*like->operand)) {
      return r;
    }
    if (!is_operand(*like->operand)) {
      return Rejection{.span = like->operand->span(),
                       .message = std::format("LIKE needs {} on the left", operand_kind)};
    }
    if (std::holds_alternative<sql::Literal>(*like->pattern)) {
      return std::nullopt;
    }
    if (auto r = FirstUnsupported(*like->pattern)) {
      return r;
    }
    return Rejection{.span = like->pattern->span(),
                     .message =
                         "LIKE with a column or an aggregate as the pattern is not "
                         "supported"};
  }
  if (const auto* in = std::get_if<sql::InExpr>(&expr)) {
    if (auto r = FirstUnsupported(*in->operand)) {
      return r;
    }
    if (!is_operand(*in->operand)) {
      return Rejection{.span = in->operand->span(),
                       .message = std::format("IN needs {} on the left", operand_kind)};
    }
    if (auto r = FirstUnsupportedIn(in->list)) {
      return r;
    }
    for (const sql::Expr& value : in->list) {
      if (!std::holds_alternative<sql::Literal>(value)) {
        return Rejection{.span = value.span(),
                         .message = "only literals are supported in an IN list"};
      }
    }
    return std::nullopt;
  }
  if (auto r = FirstUnsupported(expr)) {
    return r;
  }
  if (std::holds_alternative<sql::ColumnRef>(expr) || std::holds_alternative<sql::Literal>(expr) ||
      std::holds_alternative<sql::AggregateCall>(expr) ||
      std::holds_alternative<sql::BinaryExpr>(expr) ||
      std::holds_alternative<sql::UnaryExpr>(expr)) {
    return Rejection{.span = expr.span(),
                     .message =
                         "conditions other than comparisons, [NOT] LIKE, [NOT] IN, AND, OR "
                         "and NOT are not supported"};
  }
  return Rejection{.span = expr.span(), .message = "this condition is not supported"};
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

// Resolves column names of one table: ASCII case-insensitively, quoted names too (DuckDB).
class Columns {
 public:
  explicit Columns(const arrow::Schema& schema) : schema_(schema) {
    lower_.reserve(Narrow<std::size_t>(schema.num_fields()));
    for (const auto& field : schema.fields()) {
      lower_.push_back(AsciiLower(field->name()));
    }
  }

  // The column as an index into the Scan's output (the binder's Scan reads every field in schema
  // order, so this is the field index).
  [[nodiscard]] arrow::Result<BoundColumn> Resolve(const sql::ColumnRef& ref) const {
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
    return BoundColumn{.index = index, .name = field->name(), .type = *type};
  }

 private:
  const arrow::Schema& schema_;
  std::vector<std::string> lower_;
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

std::string_view LiteralKind(const sql::Literal& lit) {
  switch (lit.kind) {
    case sql::Literal::Kind::kInteger:
    case sql::Literal::Kind::kDecimal:
      return "a number";
    case sql::Literal::Kind::kString:
      return "a string";
    case sql::Literal::Kind::kDate:
      return "a DATE literal";
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
// number it types as DOUBLE (an exponent, or more than 38 digits) every number is a double, so
// each value is then bound as that DOUBLE would be (integer columns: the nearest double, folded
// exactly, as divergence D7; a FLOAT column: no FLOAT literals).
arrow::Result<Predicate> BindIn(const sql::Comparison& cmp, const BoundColumn& column,
                                bool stored_as_float) {
  const bool negated = cmp.op == sql::CompareOp::kNotIn;
  Predicate p{.kind = negated ? Predicate::Kind::kNotIn : Predicate::Kind::kIn,
              .column = column,
              .op = CompareOp::kEq,
              .constant = {},
              .values = {},
              .span = cmp.span};
  const bool as_double = std::ranges::any_of(cmp.list, [](const sql::Literal& value) {
    return (value.kind == sql::Literal::Kind::kInteger ||
            value.kind == sql::Literal::Kind::kDecimal) &&
           IsApproximateNumber(value.text);
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
// list with such a number).
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
  switch (column.type) {
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
    case LogicalType::kDouble: {
      if (!number) {
        return mismatch("write a number without quotes");
      }
      // Rounded to the nearest double, as DuckDB compares a DOUBLE column with a number.
      const auto value = ParseDoubleLiteral(lit.text, lit.negative);
      if (!value.has_value()) {
        return BindError("invalid number " + Clip(lit.text), lit.span);
      }
      p.constant.value = *value;
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
    case LogicalType::kTimestamp:
      return UnsupportedError(
          std::format("comparing the TIMESTAMP '{}' with a literal is not supported",
                      Clip(column.name)),
          lit.span);
    case LogicalType::kDate: {
      if (number) {
        return mismatch("write a date as DATE 'YYYY-MM-DD'");
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
// quotes ('it''s'); a date is DATE named CAST('2020-01-01' AS "DATE"). A decimal (DuckDB's
// DECIMAL) and a number DuckDB types as DOUBLE are not supported.
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
    case sql::Literal::Kind::kDecimal:
      return UnsupportedError(
          "decimal constants are not supported (DuckDB types them as DECIMAL, which antb1 lacks)",
          lit.span);
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
  }
  return UnsupportedError("this constant is not supported", lit.span);
}

// ---- scalar expressions ----

// Whether the expression contains an aggregate call.
bool ContainsAggregate(const sql::Expr& expr) {
  if (std::holds_alternative<sql::AggregateCall>(expr)) {
    return true;
  }
  if (const auto* call = std::get_if<sql::FunctionCall>(&expr)) {
    return std::ranges::any_of(call->args, ContainsAggregate);
  }
  if (const auto* binary = std::get_if<sql::BinaryExpr>(&expr)) {
    return ContainsAggregate(*binary->left) || ContainsAggregate(*binary->right);
  }
  if (const auto* unary = std::get_if<sql::UnaryExpr>(&expr)) {
    return ContainsAggregate(*unary->operand);
  }
  if (const auto* like = std::get_if<sql::LikeExpr>(&expr)) {
    return ContainsAggregate(*like->operand);
  }
  if (const auto* in = std::get_if<sql::InExpr>(&expr)) {
    return ContainsAggregate(*in->operand);
  }
  if (const auto* e = std::get_if<sql::ExtractExpr>(&expr)) {
    return ContainsAggregate(*e->source);
  }
  if (const auto* c = std::get_if<sql::CaseExpr>(&expr)) {
    return (c->operand.has_value() && ContainsAggregate(**c->operand)) ||
           (c->otherwise.has_value() && ContainsAggregate(**c->otherwise)) ||
           std::ranges::any_of(c->branches, [](const sql::CaseBranch& b) {
             return ContainsAggregate(*b.when) || ContainsAggregate(*b.then);
           });
  }
  return false;
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

Typed ColumnLeaf(int index, LogicalType type, std::string name, bool stored_as_float) {
  return Leaf(Expr{.node = ColumnExpr{.index = index}, .type = type, .name = std::move(name)},
              stored_as_float);
}

// The rank of an integer type among the signed ones (USMALLINT: none).
int SignedRank(LogicalType type) {
  switch (type) {
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

// The type DuckDB gives `l <op> r` (docs/sql-subset.md), or the error for operands it cannot take.
arrow::Result<LogicalType> ArithType(sql::BinaryOp sql_op, ArithOp op, const Typed& l,
                                     const Typed& r, SourceSpan span) {
  for (const Typed* t : {&l, &r}) {
    if (t->expr->type == LogicalType::kDate || t->expr->type == LogicalType::kTimestamp) {
      const std::string_view type = ToString(t->expr->type);
      return UnsupportedError(std::format("{} arithmetic ('{}' is {}) is not supported", type,
                                          Clip(t->expr->name), type),
                              span);
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
  if (op == ArithOp::kDivide) {
    return LogicalType::kDouble;
  }
  // A decimal literal is DuckDB's DECIMAL, not DOUBLE (though antb1 holds its value as a double).
  const bool any_double = (l.expr->type == LogicalType::kDouble && !l.decimal) ||
                          (r.expr->type == LogicalType::kDouble && !r.decimal);
  if ((l.decimal || r.decimal) && !any_double && op != ArithOp::kIntegerDivide) {
    return UnsupportedError(
        "arithmetic of a decimal literal with an integer is not supported (DuckDB computes it in "
        "DECIMAL, which antb1 lacks)",
        span);
  }
  if (any_double || l.decimal || r.decimal) {
    return LogicalType::kDouble;
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
  const LogicalType type = operand.expr->type;
  if (!IsNumeric(type)) {
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

// A literal as an expression operand: typed by its value (an integer), DOUBLE (a number DuckDB
// types as DOUBLE, or a decimal, which DuckDB types DECIMAL: flagged), VARCHAR or DATE.
arrow::Result<Typed> LiteralOperand(const sql::Literal& lit) {
  const std::string name = LiteralName(lit);
  const bool number =
      lit.kind == sql::Literal::Kind::kInteger || lit.kind == sql::Literal::Kind::kDecimal;
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
    out.decimal = !IsApproximateNumber(lit.text);
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

// The comparison of a binary comparison operator (the caller checked that it is one).
sql::CompareOp SqlCompareOp(sql::BinaryOp op) {
  switch (op) {
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
    default:
      return sql::CompareOp::kEq;
  }
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

// Whether a comparison between the types is one DuckDB and antb1 make alike: numbers with numbers,
// VARCHAR with VARCHAR, DATE with DATE.
bool Comparable(LogicalType a, LogicalType b) { return (IsNumeric(a) && IsNumeric(b)) || a == b; }

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
  return !a.arg.has_value() || a.arg->index == b.arg->index;
}

// The select item (0-based) that a GROUP BY or ORDER BY literal refers to: an integer is a position
// (1-based; out of range, a negative one too, is a bind error, as in DuckDB). Any other literal is
// a constant (std::nullopt), except that DuckDB rejects a number or string in ORDER BY because it
// would order nothing (a DATE literal is a constant expression there).
arrow::Result<std::optional<std::size_t>> PositionOf(const sql::Literal& lit,
                                                     const SelectList& select,
                                                     std::string_view clause) {
  if (lit.kind != sql::Literal::Kind::kInteger || IsApproximateNumber(lit.text)) {
    if (clause == "ORDER BY" && lit.kind != sql::Literal::Kind::kDate) {
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

// Computed columns are numbered from these bases until the plan is assembled: those of the Compute
// after the WHERE filter (from kPreBase; the WHERE operands' Compute comes first) and those of the
// Compute above the aggregation (from kPostBase), whose inputs' widths are only known then.
constexpr int kPreBase = 4 * 1024 * 1024;
constexpr int kPostBase = 16 * 1024 * 1024;

class Binder {
 public:
  Binder(const sql::SelectStatement& stmt, std::shared_ptr<Table> table)
      : stmt_(stmt),
        table_(std::move(table)),
        schema_(*table_->schema()),
        columns_(schema_),
        width_(schema_.num_fields()) {}

  arrow::Result<LogicalPlan> Bind();

 private:
  // ---- the input scope: the table's columns, and computed columns over them ----

  arrow::Result<Typed> BindInput(const sql::Expr& expr) {
    if (const auto* ref = std::get_if<sql::ColumnRef>(&expr)) {
      auto resolved = columns_.Resolve(*ref);
      if (!resolved.ok() && shape_ == Shape::kProjection) {
        if (auto alias = AliasFallback(*ref, resolved.status())) {
          return *std::move(alias);
        }
      }
      ARROW_ASSIGN_OR_RAISE(BoundColumn column, std::move(resolved));
      return ColumnLeaf(column.index, column.type, ArgumentName(ref->name),
                        table_->StoredAsFloat(column.index));
    }
    if (const auto* lit = std::get_if<sql::Literal>(&expr)) {
      return LiteralOperand(*lit);
    }
    if (const auto* binary = std::get_if<sql::BinaryExpr>(&expr)) {
      ARROW_ASSIGN_OR_RAISE(Typed left, BindInput(*binary->left));
      ARROW_ASSIGN_OR_RAISE(Typed right, BindInput(*binary->right));
      return Arith(*binary, std::move(left), std::move(right));
    }
    if (const auto* unary = std::get_if<sql::UnaryExpr>(&expr)) {
      ARROW_ASSIGN_OR_RAISE(Typed operand, BindInput(*unary->operand));
      return Negate(*unary, std::move(operand));
    }
    if (const auto* call = std::get_if<sql::AggregateCall>(&expr)) {
      return BindError("aggregate functions are not allowed here", call->span);
    }
    if (const auto* call = std::get_if<sql::FunctionCall>(&expr)) {
      return BindFunction(*call, [this](const sql::Expr& e) { return BindInput(e); });
    }
    if (const auto* c = std::get_if<sql::CaseExpr>(&expr)) {
      return BindCase(*c, [this](const sql::Expr& e) { return BindInput(e); }, /*input=*/true);
    }
    if (const auto* e = std::get_if<sql::ExtractExpr>(&expr)) {
      return BindExtract(*e, [this](const sql::Expr& x) { return BindInput(x); });
    }
    return UnsupportedError("this expression is not supported", expr.span());
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
      // date_trunc's unit matches case-insensitively.
      const std::string text =
          spec->function == Function::kDateTrunc ? AsciiLower(lit->text) : lit->text;
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

  // EXTRACT(field FROM source): BIGINT of a TIMESTAMP or DATE, named as DuckDB names it.
  template <class BindArg>
  arrow::Result<Typed> BindExtract(const sql::ExtractExpr& e, const BindArg& bind_arg) {
    ARROW_ASSIGN_OR_RAISE(Typed source, bind_arg(*e.source));
    if (source.expr->type != LogicalType::kTimestamp && source.expr->type != LogicalType::kDate) {
      return BindError(
          std::format("EXTRACT needs a TIMESTAMP or DATE, but {}", DescribeOperand(source)),
          e.source->span());
    }
    const std::string field = AsciiLower(e.field);
    std::string name = std::format("main.date_part('{}', {})", field, source.expr->name);
    auto field_expr = std::make_shared<const Expr>(
        Expr{.node = ConstantExpr{.value = Constant{.type = LogicalType::kVarchar, .value = field}},
             .type = LogicalType::kVarchar,
             .name = "'" + field + "'"});
    return Leaf(Expr{.node = FunctionExpr{.function = Function::kExtract,
                                          .args = {std::move(source.expr), std::move(field_expr)}},
                     .type = LogicalType::kBigInt,
                     .name = std::move(name)});
  }

  // The column of an input-scope expression: a table column as is, anything else computed (once):
  // a WHERE operand (`where`) before the WHERE filter on computed columns, anything else after it,
  // so that it is computed only for the rows WHERE keeps.
  BoundColumn InputColumn(const Typed& t, bool where = false) {
    if (const auto* column = std::get_if<ColumnExpr>(&t.expr->node)) {
      // A table field is named as declared; a computed column (a select item) by its expression.
      return BoundColumn{
          .index = column->index,
          .name = column->index < width_ ? schema_.field(column->index)->name() : t.expr->name,
          .type = t.expr->type};
    }
    std::vector<ExprPtr>& exprs = where ? where_exprs_ : input_exprs_;
    const auto same =
        std::ranges::find_if(exprs, [&](const ExprPtr& e) { return SameExpr(*e, *t.expr); });
    const auto k = Narrow<int>(same - exprs.begin());
    if (same == exprs.end()) {
      exprs.push_back(t.expr);
    }
    return BoundColumn{
        .index = (where ? width_ : kPreBase) + k, .name = t.expr->name, .type = t.expr->type};
  }

  // An aggregate call with its argument bound in the input scope (computed when not a column).
  arrow::Result<AggregateCall> BindAggregate(const sql::AggregateCall& call) {
    AggregateCall bound{.kind = AggKind::kCountStar, .span = call.span};
    if (call.kind == sql::AggKind::kCountStar || !call.arg.has_value()) {
      return bound;
    }
    ARROW_ASSIGN_OR_RAISE(const Typed arg, BindInput(**call.arg));
    if (arg.stored_as_float) {
      float_args_.push_back(InputColumn(arg).index);
    }
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
      select_.aggregates.push_back(std::move(bound));
    }
    const AggregateCall& agg = select_.aggregates[index];
    // DuckDB's MIN and MAX of a FLOAT column are FLOAT; the other aggregates are not.
    const bool is_float = (agg.kind == AggKind::kMin || agg.kind == AggKind::kMax) &&
                          agg.arg.has_value() &&
                          std::ranges::find(float_args_, agg.arg->index) != float_args_.end();
    return ColumnLeaf(Narrow<int>(keys_.size() + index), agg.type, ResultName(call), is_float);
  }

  static arrow::Status NotGrouped(std::string_view name, SourceSpan span) {
    return BindError(std::format("column '{}' must appear in the GROUP BY clause or be inside an "
                                 "aggregate function",
                                 name),
                     span);
  }

  // An expression over the aggregation's output: a subexpression equal to a GROUP BY key is that
  // key, an aggregate its column; any other column is a bind error.
  arrow::Result<Typed> BindOutput(const sql::Expr& expr) {
    if (!ContainsAggregate(expr) && !std::holds_alternative<sql::Literal>(expr)) {
      auto input = BindInput(expr);
      if (input.ok()) {
        for (std::size_t k = 0; k < key_exprs_.size(); ++k) {
          if (SameExpr(*input->expr, *key_exprs_[k])) {
            return ColumnLeaf(Narrow<int>(k), keys_[k].type, input->expr->name,
                              input->stored_as_float);
          }
        }
      }
    }
    if (const auto* call = std::get_if<sql::AggregateCall>(&expr)) {
      return AggregateOutput(*call);
    }
    if (const auto* ref = std::get_if<sql::ColumnRef>(&expr)) {
      auto resolved = columns_.Resolve(*ref);
      if (!resolved.ok()) {
        if (auto alias = AliasFallback(*ref, resolved.status())) {
          return *std::move(alias);
        }
        return resolved.status();
      }
      return NotGrouped(ref->name, ref->span);
    }
    if (const auto* lit = std::get_if<sql::Literal>(&expr)) {
      return LiteralOperand(*lit);
    }
    if (const auto* binary = std::get_if<sql::BinaryExpr>(&expr)) {
      ARROW_ASSIGN_OR_RAISE(Typed left, BindOutput(*binary->left));
      ARROW_ASSIGN_OR_RAISE(Typed right, BindOutput(*binary->right));
      return Arith(*binary, std::move(left), std::move(right));
    }
    if (const auto* unary = std::get_if<sql::UnaryExpr>(&expr)) {
      ARROW_ASSIGN_OR_RAISE(Typed operand, BindOutput(*unary->operand));
      return Negate(*unary, std::move(operand));
    }
    if (const auto* call = std::get_if<sql::FunctionCall>(&expr)) {
      return BindFunction(*call, [this](const sql::Expr& e) { return BindOutput(e); });
    }
    if (const auto* c = std::get_if<sql::CaseExpr>(&expr)) {
      return BindCase(*c, [this](const sql::Expr& e) { return BindOutput(e); }, /*input=*/false);
    }
    if (const auto* e = std::get_if<sql::ExtractExpr>(&expr)) {
      return BindExtract(*e, [this](const sql::Expr& x) { return BindOutput(x); });
    }
    return UnsupportedError("this expression is not supported", expr.span());
  }

  // The column of an output-scope expression: an output column as is, anything else computed
  // above the aggregation (once).
  BoundColumn OutputColumn(const Typed& t) {
    if (const auto* column = std::get_if<ColumnExpr>(&t.expr->node)) {
      return BoundColumn{.index = column->index, .name = t.expr->name, .type = t.expr->type};
    }
    const auto same =
        std::ranges::find_if(post_exprs_, [&](const ExprPtr& e) { return SameExpr(*e, *t.expr); });
    const auto k = Narrow<int>(same - post_exprs_.begin());
    if (same == post_exprs_.end()) {
      post_exprs_.push_back(t.expr);
    }
    return BoundColumn{.index = kPostBase + k, .name = t.expr->name, .type = t.expr->type};
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

  // The column of an output reference once the aggregation's output width is known.
  static void Place(BoundColumn& column, int width) {
    if (column.index >= kPostBase) {
      column.index = width + (column.index - kPostBase);
    }
  }

  LogicalPlan Assemble();

  const sql::SelectStatement& stmt_;
  std::shared_ptr<Table> table_;
  const arrow::Schema& schema_;
  Columns columns_;
  int width_;  // the Scan's output: every table field
  SelectList select_;
  Shape shape_ = Shape::kProjection;
  std::vector<ExprPtr> where_exprs_;     // WHERE operands, computed over the filtered Scan
  std::vector<ExprPtr> input_exprs_;     // computed after the WHERE filter on computed columns
  std::vector<int> float_args_;          // input columns holding FLOAT values
  std::vector<Predicate> scan_filter_;   // WHERE over the Scan's columns
  std::vector<Predicate> input_filter_;  // WHERE over computed columns
  std::vector<BoundColumn> keys_;        // GROUP BY keys, as columns of the input
  std::vector<ExprPtr> key_exprs_;       // per key: its input-scope expression
  std::vector<ExprPtr> post_exprs_;      // computed over the aggregation's output
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

template <class BindFn, class ColumnOf>
arrow::Result<Predicate> Binder::BindConditionWith(const sql::Expr& conjunct, bool input,
                                                   const BindFn& bind, const ColumnOf& column_of,
                                                   bool nested) {
  // `operand <op> literal` (and the LIKE and IN forms) through the column binders.
  const auto with_literal = [&](const sql::Expr& operand_expr, sql::CompareOp op,
                                const sql::Literal& literal, const std::vector<sql::Literal>& list,
                                SourceSpan span) -> arrow::Result<Predicate> {
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
  };
  if (const auto* like = std::get_if<sql::LikeExpr>(&conjunct)) {
    return with_literal(*like->operand,
                        like->negated ? sql::CompareOp::kNotLike : sql::CompareOp::kLike,
                        std::get<sql::Literal>(*like->pattern), {}, like->span);
  }
  if (const auto* in = std::get_if<sql::InExpr>(&conjunct)) {
    std::vector<sql::Literal> list;
    list.reserve(in->list.size());
    for (const sql::Expr& value : in->list) {
      list.push_back(std::get<sql::Literal>(value));
    }
    return with_literal(*in->operand, in->negated ? sql::CompareOp::kNotIn : sql::CompareOp::kIn,
                        sql::Literal{}, list, in->span);
  }
  const auto& binary = std::get<sql::BinaryExpr>(conjunct);
  const sql::CompareOp op = SqlCompareOp(binary.op);
  const auto* left_literal = std::get_if<sql::Literal>(&*binary.left);
  const auto* right_literal = std::get_if<sql::Literal>(&*binary.right);
  if (right_literal != nullptr || left_literal != nullptr) {
    const sql::Expr& operand_expr = right_literal != nullptr ? *binary.left : *binary.right;
    const sql::Literal& literal = right_literal != nullptr ? *right_literal : *left_literal;
    const sql::CompareOp oriented = right_literal != nullptr ? op : Mirror(op);
    {
      ARROW_ASSIGN_OR_RAISE(const auto moved,
                            MoveConstants(operand_expr, oriented, literal, bind, !input));
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
        return with_literal(*moved->operand, moved->op, k, {}, binary.span);
      }
    }
    return with_literal(operand_expr, oriented, literal, {}, binary.span);
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

// The type DuckDB gives two CASE values (a string literal takes the other's type elsewhere).
arrow::Result<LogicalType> CaseCommon(LogicalType a, LogicalType b, SourceSpan span) {
  if (a == b) {
    return a;
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
  // DuckDB's type: the values' common type, where an integer literal takes the others' integer
  // type when it fits and a string literal any type (DATE: the date it spells).
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
      if (v.decimal && type != LogicalType::kDouble) {
        // With a DOUBLE value DuckDB's CASE is DOUBLE; else DECIMAL.
        return UnsupportedError(
            "a decimal literal as a CASE value is not supported (DuckDB types it DECIMAL, which "
            "antb1 lacks)",
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
    } else if (IsNumeric(*type) || *type == LogicalType::kTimestamp) {
      // DuckDB casts the string to the number ('1' next to SMALLINT is 1) or the TIMESTAMP.
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
  return (binary != nullptr && binary->op == sql::BinaryOp::kOr) ||
         (unary != nullptr && unary->op == sql::UnaryOp::kNot);
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
    ARROW_ASSIGN_OR_RAISE(Predicate predicate, BindCondition(*conjunct, /*having=*/false));
    const bool on_scan = (!predicate.column.has_value() || predicate.column->index < width_) &&
                         (!predicate.other.has_value() || predicate.other->index < width_);
    (on_scan ? scan_filter_ : input_filter_).push_back(std::move(predicate));
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
        add(ColumnLeaf(column.index, column.type, column.name, false));
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
        add(ColumnLeaf(column->index, column->type, column->name, false));
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
    std::vector<int> reads;
    CollectColumns(*t.expr, reads);
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
    const Expr column{.node = ColumnExpr{.index = select_.columns[i].index},
                      .type = select_.columns[i].type,
                      .name = {}};
    const bool key =
        std::ranges::any_of(key_exprs_, [&](const ExprPtr& k) { return SameExpr(*k, column); });
    if (!key) {
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
      return std::optional(ColumnLeaf(Narrow<int>(keys_.size() + index),
                                      select_.aggregates[index].type, select_.output[i].name,
                                      false));
    case ItemKind::kExpression: {
      const BoundColumn& column = select_.expr_columns[index];
      return std::optional(ColumnLeaf(column.index, column.type, column.name, false));
    }
    case ItemKind::kColumn:
      break;
  }
  const BoundColumn& column = select_.columns[index];
  if (shape_ == Shape::kProjection) {
    return std::optional(
        ColumnLeaf(column.index, column.type, column.name, table_->StoredAsFloat(column.index)));
  }
  for (std::size_t k = 0; k < key_exprs_.size(); ++k) {
    const auto* key = std::get_if<ColumnExpr>(&key_exprs_[k]->node);
    if (key != nullptr && key->index == column.index) {
      return std::optional(ColumnLeaf(Narrow<int>(k), column.type, column.name,
                                      table_->StoredAsFloat(column.index)));
    }
  }
  return NotGrouped(select_.column_written[index], select_.column_spans[index]);
}

// A name in HAVING: a table column that is a GROUP BY key first, else the last select item with
// that alias (DuckDB): a key, an aggregate or an expression. std::nullopt: bind it as any other
// operand.
arrow::Result<std::optional<Typed>> Binder::ResolveHavingName(const sql::ColumnRef& ref) {
  auto table_column = columns_.Resolve(ref);
  if (table_column.ok()) {
    for (std::size_t k = 0; k < key_exprs_.size(); ++k) {
      const auto* key = std::get_if<ColumnExpr>(&key_exprs_[k]->node);
      if (key != nullptr && key->index == table_column->index) {
        return std::optional(ColumnLeaf(Narrow<int>(k), table_column->type, table_column->name,
                                        table_->StoredAsFloat(table_column->index)));
      }
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
    const AggregateCall& agg = select_.aggregates[select_.items[*alias].second];
    item->stored_as_float = (agg.kind == AggKind::kMin || agg.kind == AggKind::kMax) &&
                            agg.arg.has_value() &&
                            std::ranges::find(float_args_, agg.arg->index) != float_args_.end();
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
        if (column != nullptr && column->index >= kPostBase) {
          resolved->expr = post_exprs_.at(Narrow<std::size_t>(column->index - kPostBase));
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
    ARROW_ASSIGN_OR_RAISE(Predicate predicate, BindCondition(*conjunct, /*having=*/true));
    having_.push_back(std::move(predicate));
  }
  alias_fallback_ = false;
  return arrow::Status::OK();
}

// ORDER BY items in the query's scope. kGlobal checks the items and returns no key: one row needs
// no sort. A select alias comes before a table column, as in DuckDB; an unsigned integer is a
// position in the select list; a constant (a constant item, or any other literal) orders nothing;
// a later key on a column already ordered by changes nothing and is dropped.
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
               ref != nullptr && FindAlias(select_, ref->name).has_value()) {
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
        continue;
      }
      std::vector<int> reads;
      CollectColumns(*t.expr, reads);
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
    const bool repeated = std::ranges::any_of(
        sort_keys_, [&](const SortKey& k) { return k.column.index == column.index; });
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
  ScanNode scan{
      .table = table_, .table_name = stmt_.from.name, .fields = {}, .span = stmt_.from.span};
  for (int i = 0; i < width_; ++i) {
    scan.fields.push_back(i);
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
    node = Make(ComputeNode{.input = std::move(node), .exprs = where_exprs_, .span = where_span});
  }
  if (!input_filter_.empty()) {
    node = Make(FilterNode{
        .input = std::move(node), .predicates = std::move(input_filter_), .span = where_span});
  }
  // The computed columns after the WHERE filter follow the WHERE operands'.
  const int pre = width_ + Narrow<int>(where_exprs_.size());
  const auto place_pre = [pre](BoundColumn& column) {
    if (column.index >= kPreBase && column.index < kPostBase) {
      column.index = pre + (column.index - kPreBase);
    }
  };
  for (AggregateCall& call : select_.aggregates) {
    if (call.arg.has_value()) {
      place_pre(*call.arg);
    }
  }
  for (BoundColumn& key : keys_) {
    place_pre(key);
  }
  for (BoundColumn& column : select_.expr_columns) {
    place_pre(column);
  }
  for (SortKey& key : sort_keys_) {
    place_pre(key.column);
  }
  if (!input_exprs_.empty()) {
    node = Make(ComputeNode{.input = std::move(node), .exprs = input_exprs_, .span = select_.span});
  }
  // Over the aggregation: computed columns, HAVING, the Sort, then a Project restores the select
  // order; `width` is the aggregation's output.
  const auto above = [&](int width) {
    for (Predicate& p : having_) {
      if (p.column.has_value()) {
        Place(*p.column, width);
      }
      if (p.other.has_value()) {
        Place(*p.other, width);
      }
    }
    for (SortKey& key : sort_keys_) {
      Place(key.column, width);
    }
    for (BoundColumn& column : select_.expr_columns) {
      Place(column, width);
    }
    if (!post_exprs_.empty()) {
      node =
          Make(ComputeNode{.input = std::move(node), .exprs = post_exprs_, .span = select_.span});
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
  // The select list over the node below it: a column (at `column_index` in that node), an
  // aggregate (at `first_aggregate` + its index), a constant or a (computed) expression.
  const auto project = [&](const auto& column_index, std::size_t first_aggregate) {
    ProjectNode out{.input = std::move(node), .columns = {}, .constants = {}, .span = select_.span};
    for (std::size_t i = 0; i < select_.items.size(); ++i) {
      const auto [kind, index] = select_.items[i];
      std::optional<Constant> constant;
      BoundColumn column{
          .index = -1, .name = select_.output[i].name, .type = select_.output[i].type};
      switch (kind) {
        case ItemKind::kColumn:
          column.index = column_index(select_.columns[index]);
          column.name = select_.columns[index].name;
          break;
        case ItemKind::kAggregate:
          column.index = Narrow<int>(first_aggregate + index);
          break;
        case ItemKind::kConstant:
          constant = select_.constants[index];
          break;
        case ItemKind::kExpression:
          column.index = select_.expr_columns[index].index;
          break;
      }
      out.columns.push_back(std::move(column));
      out.constants.push_back(std::move(constant));
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
      const std::size_t key_count = keys_.size();
      const std::vector<ExprPtr> key_exprs = key_exprs_;
      const int width = Narrow<int>(keys_.size() + select_.aggregates.size());
      node = Make(GroupAggregateNode{.input = std::move(node),
                                     .keys = std::move(keys_),
                                     .aggregates = select_.aggregates,
                                     .span = stmt_.group_by_span});
      above(width);
      project(
          [&](const BoundColumn& column) {
            const auto key = std::ranges::find_if(key_exprs, [&](const ExprPtr& k) {
              const auto* c = std::get_if<ColumnExpr>(&k->node);
              return c != nullptr && c->index == column.index;
            });
            return Narrow<int>(key - key_exprs.begin());
          },
          key_count);
      break;
    }
    case Shape::kProjection:
      above(0);
      project([](const BoundColumn& column) { return column.index; }, 0);
      break;
    case Shape::kGlobal: {
      const bool hidden =
          select_.aggregates.size() >
          static_cast<std::size_t>(std::ranges::count_if(
              select_.items, [](const auto& item) { return item.first == ItemKind::kAggregate; }));
      const int width = Narrow<int>(select_.aggregates.size());
      node = Make(AggregateNode{
          .input = std::move(node), .aggregates = select_.aggregates, .span = select_.span});
      above(width);
      // A Project restores the select list when the Aggregate's output is not it (constants,
      // expressions, hidden aggregates, or columns computed for HAVING).
      if (!select_.constants.empty() || !select_.exprs.empty() || hidden || !post_exprs_.empty()) {
        project([](const BoundColumn& column) { return column.index; }, 0);
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
  return LogicalPlan{.root = std::move(node), .output = std::move(select_.output)};
}

// Expressions the binder does not answer yet are kUnsupported, reported (like the parser's own
// kUnsupported errors) before any name is resolved, at the first one in query order.
arrow::Status CheckSupported(const sql::SelectStatement& stmt) {
  for (const sql::SelectItem& item : stmt.items) {
    ARROW_RETURN_NOT_OK(CheckValue(item.expr));
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
  ARROW_RETURN_NOT_OK(CheckSupported(stmt));
  ARROW_ASSIGN_OR_RAISE(auto table, ResolveTable(stmt.from, catalog));
  Binder binder(stmt, std::move(table));
  return binder.Bind();
}

}  // namespace antb1::plan
