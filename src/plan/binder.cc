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
#include "antb1/sql/parser.h"

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

// count_star(), count(x), count(DISTINCT x), sum(x), avg(x), min(x), max(x).
std::string ResultName(const sql::AggregateCall& call) {
  if (call.kind == sql::AggKind::kCountStar || !call.arg.has_value()) {
    return "count_star()";
  }
  return AsciiLower(ToString(ToPlan(call.kind))) + "(" + (call.distinct ? "DISTINCT " : "") +
         ArgumentName(call.arg->name) + ")";
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

arrow::Result<AggregateCall> BindAggregate(const sql::AggregateCall& call, const Columns& columns) {
  AggregateCall bound{.kind = AggKind::kCountStar, .span = call.span};
  if (call.kind == sql::AggKind::kCountStar || !call.arg.has_value()) {
    return bound;
  }
  ARROW_ASSIGN_OR_RAISE(BoundColumn arg, columns.Resolve(*call.arg));
  ARROW_ASSIGN_OR_RAISE(bound.type, AggregateType(call, arg));
  bound.kind = call.distinct ? AggKind::kCountDistinct : ToPlan(call.kind);
  bound.arg = std::move(arg);
  return bound;
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
                                      const Table& table, bool as_double = false);

// `column [NOT] IN (v1, ...)`: each value is bound as `column = v` (the same typing and exact
// folding); a value no column value can equal is dropped. With no value left, IN is FALSE and
// NOT IN is IS NOT NULL (NULL still rejects the row). DuckDB gives the list one type: with a
// number it types as DOUBLE (an exponent, or more than 38 digits) every number is a double, so
// each value is then bound as that DOUBLE would be (integer columns: the nearest double, folded
// exactly, as divergence D7; a FLOAT column: no FLOAT literals).
arrow::Result<Predicate> BindIn(const sql::Comparison& cmp, const BoundColumn& column,
                                const Table& table) {
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
    ARROW_ASSIGN_OR_RAISE(const Predicate one, BindEquality(equal, column, table, as_double));
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

arrow::Result<Predicate> BindComparison(const sql::Comparison& cmp, const BoundColumn& column,
                                        const Table& table) {
  if (cmp.op == sql::CompareOp::kLike || cmp.op == sql::CompareOp::kNotLike) {
    return BindLike(cmp, column);
  }
  if (cmp.op == sql::CompareOp::kIn || cmp.op == sql::CompareOp::kNotIn) {
    return BindIn(cmp, column, table);
  }
  return BindEquality(cmp, column, table);
}

// `column <op> literal`, with the literal folded exactly into the column's type. `as_double`: a
// number is read as DuckDB reads a DOUBLE-typed one, even when it is not written that way (an IN
// list with such a number).
arrow::Result<Predicate> BindEquality(const sql::Comparison& cmp, const BoundColumn& column,
                                      const Table& table, bool as_double) {
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
      if (!as_double && table.StoredAsFloat(column.index)) {
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

// What a select item is: a plain column, an aggregate call or a constant.
enum class ItemKind : std::uint8_t { kColumn, kAggregate, kConstant };

struct SelectList {
  std::vector<BoundColumn> columns;       // SELECT * or plain columns, in select order
  std::vector<AggregateCall> aggregates;  // aggregates, in select order
  std::vector<Constant> constants;        // constants, in select order
  std::vector<OutputColumn> output;
  // Per output column: its kind and an index into `columns`, `aggregates` or `constants`.
  std::vector<std::pair<ItemKind, std::size_t>> items;
  std::vector<SourceSpan> column_spans;     // per entry of `columns`: what the error points at
  std::vector<std::string> column_written;  // per entry of `columns`: the name as written
  std::vector<std::optional<std::string>> aliases;  // per output column
  SourceSpan span;
};

// An ORDER BY aggregate makes the query aggregate, like one in the select list (DuckDB).
bool OrdersByAggregate(const sql::SelectStatement& stmt) {
  return std::ranges::any_of(stmt.order_by, [](const sql::OrderItem& item) {
    return std::holds_alternative<sql::AggregateCall>(item.expr);
  });
}

// A constant select item with DuckDB's type and result name: an integer is INTEGER, BIGINT or
// HUGEINT by its value and named as written ("-5"); a string is VARCHAR named with its quotes
// ('it''s'); a date is DATE named CAST('2020-01-01' AS "DATE"). A decimal (DuckDB's DECIMAL) and a
// number DuckDB types as DOUBLE are not supported.
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
      const Int128 value = exact->negative ? -exact->magnitude : exact->magnitude;
      LogicalType type = LogicalType::kHugeInt;
      for (const LogicalType narrow : {LogicalType::kInteger, LogicalType::kBigInt}) {
        const IntegerRange range = RangeOf(narrow);
        if (value >= range.min && value <= range.max) {
          type = narrow;
          break;
        }
      }
      return std::pair(Constant{.type = type, .value = value},
                       (lit.negative ? "-" : "") + lit.text);
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

arrow::Result<SelectList> BindSelectList(const sql::SelectStatement& stmt,
                                         const arrow::Schema& schema, const Columns& columns) {
  SelectList list;
  if (stmt.star) {
    list.span = stmt.star_span;
    for (int i = 0; i < schema.num_fields(); ++i) {
      ARROW_ASSIGN_OR_RAISE(BoundColumn column, columns.Field(i, stmt.star_span));
      list.output.push_back(OutputColumn{.name = column.name, .type = column.type});
      list.items.emplace_back(ItemKind::kColumn, list.columns.size());
      list.column_spans.push_back(stmt.star_span);
      list.column_written.push_back(column.name);
      list.aliases.emplace_back();
      list.columns.push_back(std::move(column));
    }
    if (stmt.group_by.empty() && OrdersByAggregate(stmt) && !list.columns.empty()) {
      return BindError(std::format("column '{}' must be inside an aggregate function: a query "
                                   "with aggregates cannot also select plain columns (there is "
                                   "no GROUP BY)",
                                   list.columns.front().name),
                       stmt.star_span);
    }
    return list;
  }
  if (stmt.items.empty()) {
    return BindError("the select list is empty", stmt.span);
  }
  list.span = Cover(stmt.items.front().span, stmt.items.back().span);
  const sql::ColumnRef* first_column = nullptr;
  for (const sql::SelectItem& item : stmt.items) {
    list.aliases.push_back(item.alias);
    if (const auto* call = std::get_if<sql::AggregateCall>(&item.expr)) {
      ARROW_ASSIGN_OR_RAISE(AggregateCall bound, BindAggregate(*call, columns));
      list.output.push_back(
          OutputColumn{.name = item.alias.value_or(ResultName(*call)), .type = bound.type});
      list.items.emplace_back(ItemKind::kAggregate, list.aggregates.size());
      list.aggregates.push_back(std::move(bound));
      continue;
    }
    if (const auto* lit = std::get_if<sql::Literal>(&item.expr)) {
      ARROW_ASSIGN_OR_RAISE(auto constant, BindConstant(*lit));
      list.output.push_back(OutputColumn{.name = item.alias.value_or(std::move(constant.second)),
                                         .type = constant.first.type});
      list.items.emplace_back(ItemKind::kConstant, list.constants.size());
      list.constants.push_back(std::move(constant.first));
      continue;
    }
    const auto& ref = std::get<sql::ColumnRef>(item.expr);
    ARROW_ASSIGN_OR_RAISE(BoundColumn column, columns.Resolve(ref));
    // DuckDB names a plain column by its declared name, not as written.
    list.output.push_back(
        OutputColumn{.name = item.alias.value_or(column.name), .type = column.type});
    list.items.emplace_back(ItemKind::kColumn, list.columns.size());
    list.column_spans.push_back(ref.span);
    list.column_written.push_back(ref.name);
    list.columns.push_back(std::move(column));
    if (first_column == nullptr) {
      first_column = &ref;
    }
  }
  if (stmt.group_by.empty() && (!list.aggregates.empty() || OrdersByAggregate(stmt)) &&
      first_column != nullptr) {
    return BindError(std::format("column '{}' must be inside an aggregate function: a query with "
                                 "aggregates cannot also select plain columns (there is no "
                                 "GROUP BY)",
                                 first_column->name),
                     first_column->span);
  }
  return list;
}

}  // namespace

namespace {

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

// The GROUP BY keys: a table column, or else the alias of a plain column in the select list (the
// last item with that alias, as in DuckDB), or a position in the select list; duplicates are
// dropped. A constant (a constant item, by alias or position, or any other literal) is no key: it
// still makes the query grouped (one group if there is a row at all).
arrow::Result<std::vector<BoundColumn>> BindGroupBy(const sql::SelectStatement& stmt,
                                                    const Columns& columns,
                                                    const SelectList& select) {
  std::vector<BoundColumn> keys;
  const auto add = [&](BoundColumn column) {
    const bool duplicate = std::ranges::any_of(
        keys, [&](const BoundColumn& key) { return key.index == column.index; });
    if (!duplicate) {
      keys.push_back(std::move(column));
    }
  };
  // Select item i as a key: its column, nothing for a constant, an error for an aggregate.
  const auto item_key = [&](std::size_t i, std::string_view what,
                            SourceSpan span) -> arrow::Status {
    const auto [kind, index] = select.items[i];
    switch (kind) {
      case ItemKind::kAggregate:
        return BindError(std::format("GROUP BY cannot refer to the aggregate {}", what), span);
      case ItemKind::kConstant:
        break;
      case ItemKind::kColumn:
        add(select.columns[index]);
        break;
    }
    return arrow::Status::OK();
  };
  for (const sql::GroupExpr& expr : stmt.group_by) {
    if (const auto* lit = std::get_if<sql::Literal>(&expr)) {
      ARROW_ASSIGN_OR_RAISE(const auto position, PositionOf(*lit, select, "GROUP BY"));
      if (position.has_value()) {
        ARROW_RETURN_NOT_OK(
            item_key(*position, std::format("at position {}", *position + 1), lit->span));
      }
      continue;
    }
    const auto& ref = std::get<sql::ColumnRef>(expr);
    auto column = columns.Resolve(ref);
    if (column.ok()) {
      add(*std::move(column));
      continue;
    }
    const auto detail = GetSqlError(column.status());
    if (detail == nullptr || detail->kind() != SqlErrorDetail::Kind::kBind) {
      return column.status();
    }
    // The last select item with the alias wins, as in DuckDB.
    const std::string wanted = AsciiLower(ref.name);
    std::optional<std::size_t> alias;
    for (std::size_t n = select.items.size(); n > 0 && !alias.has_value(); --n) {
      const auto& name = select.aliases[n - 1];
      if (name.has_value() && AsciiLower(*name) == wanted) {
        alias = n - 1;
      }
    }
    if (!alias.has_value()) {
      return column.status();
    }
    ARROW_RETURN_NOT_OK(item_key(*alias, std::format("'{}'", *select.aliases[*alias]), ref.span));
  }
  return keys;
}

// Every plain column of a grouped select list must be a key (as in DuckDB).
arrow::Status CheckGrouped(const SelectList& select, const std::vector<BoundColumn>& keys) {
  for (std::size_t i = 0; i < select.columns.size(); ++i) {
    const bool key = std::ranges::any_of(
        keys, [&](const BoundColumn& k) { return k.index == select.columns[i].index; });
    if (!key) {
      return BindError(std::format("column '{}' must appear in the GROUP BY clause or be inside an "
                                   "aggregate function",
                                   select.column_written[i]),
                       select.column_spans[i]);
    }
  }
  return arrow::Status::OK();
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
  return !a.arg.has_value() || a.arg->index == b.arg->index;
}

// How the rows of a query are shaped, which decides what ORDER BY may refer to.
enum class Shape : std::uint8_t {
  kProjection,  // no aggregate: any table column
  kGrouped,     // GROUP BY: keys and aggregates (hidden ones are added)
  kGlobal,      // aggregates without GROUP BY: one row, so only aggregates
};

// ORDER BY items resolved against the node below the Sort: the table's columns (kProjection), or
// the GroupAggregate's keys and then its aggregates (kGrouped, where `aggregates` gains the calls
// that are not in the select list). kGlobal checks the items and returns no key: one row needs no
// sort. A select alias comes before a table column, as in DuckDB; an unsigned integer is a
// position in the select list; a constant (a constant item, or any other literal) orders nothing;
// a later key on a column already ordered by changes nothing and is dropped.
arrow::Result<std::vector<SortKey>> BindOrderBy(const sql::SelectStatement& stmt,
                                                const Columns& columns, const SelectList& select,
                                                Shape shape, const std::vector<BoundColumn>& keys,
                                                std::vector<AggregateCall>& aggregates) {
  const auto not_grouped = [](std::string_view name, SourceSpan span) {
    return BindError(std::format("column '{}' must appear in the GROUP BY clause or be inside an "
                                 "aggregate function",
                                 name),
                     span);
  };
  const auto key_of = [&](const BoundColumn& column) -> std::optional<int> {
    const auto key =
        std::ranges::find_if(keys, [&](const BoundColumn& k) { return k.index == column.index; });
    return key == keys.end() ? std::nullopt : std::optional(Narrow<int>(key - keys.begin()));
  };
  const auto aggregate_column = [&](std::size_t index, std::string name) {
    return BoundColumn{.index = Narrow<int>(keys.size() + index),
                       .name = std::move(name),
                       .type = aggregates[index].type};
  };
  // A table column as a sort key of this shape.
  const auto table_key = [&](BoundColumn column, std::string_view name,
                             SourceSpan span) -> arrow::Result<std::optional<BoundColumn>> {
    if (shape == Shape::kGlobal) {
      return not_grouped(name, span);
    }
    if (shape == Shape::kGrouped) {
      const auto key = key_of(column);
      if (!key.has_value()) {
        return not_grouped(name, span);
      }
      column.index = *key;
    }
    return std::optional(std::move(column));
  };
  // Select item i as a sort key (std::nullopt: it orders nothing).
  const auto item_key = [&](std::size_t i,
                            SourceSpan span) -> arrow::Result<std::optional<BoundColumn>> {
    const auto [kind, index] = select.items[i];
    switch (kind) {
      case ItemKind::kAggregate:  // kGlobal: its only row needs no sort
        if (shape == Shape::kGrouped) {
          return std::optional(aggregate_column(index, select.output[i].name));
        }
        return std::nullopt;
      case ItemKind::kConstant:
        return std::nullopt;
      case ItemKind::kColumn:
        break;
    }
    return table_key(select.columns[index], select.column_written[index], span);
  };
  std::vector<SortKey> sort_keys;
  for (const sql::OrderItem& item : stmt.order_by) {
    std::optional<BoundColumn> column;
    if (const auto* call = std::get_if<sql::AggregateCall>(&item.expr)) {
      ARROW_ASSIGN_OR_RAISE(AggregateCall bound, BindAggregate(*call, columns));
      if (shape == Shape::kGrouped) {
        const auto same = std::ranges::find_if(
            aggregates, [&](const AggregateCall& a) { return SameCall(a, bound); });
        const auto index = Narrow<std::size_t>(same - aggregates.begin());
        if (same == aggregates.end()) {
          aggregates.push_back(std::move(bound));  // a hidden aggregate
        }
        column = aggregate_column(index, ResultName(*call));
      }
    } else if (const auto* lit = std::get_if<sql::Literal>(&item.expr)) {
      ARROW_ASSIGN_OR_RAISE(const auto position, PositionOf(*lit, select, "ORDER BY"));
      if (position.has_value()) {
        ARROW_ASSIGN_OR_RAISE(column, item_key(*position, lit->span));
      }
    } else {
      const auto& ref = std::get<sql::ColumnRef>(item.expr);
      if (const auto alias = FindAlias(select, ref.name)) {
        ARROW_ASSIGN_OR_RAISE(column, item_key(*alias, ref.span));
      } else {
        ARROW_ASSIGN_OR_RAISE(BoundColumn table_column, columns.Resolve(ref));
        ARROW_ASSIGN_OR_RAISE(column, table_key(std::move(table_column), ref.name, ref.span));
      }
    }
    if (!column.has_value()) {
      continue;
    }
    const bool repeated = std::ranges::any_of(
        sort_keys, [&](const SortKey& k) { return k.column.index == column->index; });
    if (!repeated) {
      sort_keys.push_back(SortKey{.column = *std::move(column),
                                  .descending = item.descending,
                                  .nulls_first = item.nulls == sql::NullsOrder::kFirst});
    }
  }
  return sort_keys;
}

}  // namespace

arrow::Result<LogicalPlan> Bind(const sql::SelectStatement& stmt, const Catalog& catalog) {
  ARROW_ASSIGN_OR_RAISE(auto table, ResolveTable(stmt.from, catalog));
  const arrow::Schema& schema = *table->schema();
  const Columns columns(schema);
  ARROW_ASSIGN_OR_RAISE(SelectList select, BindSelectList(stmt, schema, columns));

  std::vector<Predicate> predicates;
  for (const sql::Comparison& cmp : stmt.where) {
    ARROW_ASSIGN_OR_RAISE(BoundColumn column, columns.Resolve(cmp.column));
    ARROW_ASSIGN_OR_RAISE(Predicate predicate, BindComparison(cmp, column, *table));
    predicates.push_back(std::move(predicate));
  }
  std::vector<BoundColumn> keys;
  if (!stmt.group_by.empty()) {
    ARROW_ASSIGN_OR_RAISE(keys, BindGroupBy(stmt, columns, select));
    ARROW_RETURN_NOT_OK(CheckGrouped(select, keys));
  }
  if (stmt.limit.has_value() && *stmt.limit < 0) {
    return BindError("LIMIT must not be negative", stmt.limit_span);
  }
  if (stmt.offset.has_value() && *stmt.offset < 0) {
    return BindError("OFFSET must not be negative", stmt.offset_span);
  }
  Shape shape = Shape::kGlobal;
  if (!stmt.group_by.empty()) {
    shape = Shape::kGrouped;
  } else if (select.aggregates.empty() && !OrdersByAggregate(stmt)) {
    shape = Shape::kProjection;  // an ORDER BY aggregate makes one row, even of constants only
  }
  ARROW_ASSIGN_OR_RAISE(std::vector<SortKey> sort_keys,
                        BindOrderBy(stmt, columns, select, shape, keys, select.aggregates));

  ScanNode scan{.table = table, .table_name = stmt.from.name, .fields = {}, .span = stmt.from.span};
  for (int i = 0; i < schema.num_fields(); ++i) {
    scan.fields.push_back(i);
  }
  LogicalNodePtr node = Make(std::move(scan));
  if (!predicates.empty()) {
    node = Make(FilterNode{.input = std::move(node),
                           .predicates = std::move(predicates),
                           .span = Cover(stmt.where.front().span, stmt.where.back().span)});
  }
  const auto sort = [&] {
    if (!sort_keys.empty()) {
      node = Make(SortNode{
          .input = std::move(node), .keys = std::move(sort_keys), .span = stmt.order_by_span});
    }
  };
  // The select list over the node below it: a column (at `column_index` in that node), an
  // aggregate (at `first_aggregate` + its index) or a constant, per item.
  const auto project = [&](const auto& column_index, std::size_t first_aggregate) {
    ProjectNode out{.input = std::move(node), .columns = {}, .constants = {}, .span = select.span};
    for (std::size_t i = 0; i < select.items.size(); ++i) {
      const auto [kind, index] = select.items[i];
      std::optional<Constant> constant;
      BoundColumn column{.index = -1, .name = select.output[i].name, .type = select.output[i].type};
      switch (kind) {
        case ItemKind::kColumn:
          column.index = column_index(select.columns[index]);
          column.name = select.columns[index].name;
          break;
        case ItemKind::kAggregate:
          column.index = Narrow<int>(first_aggregate + index);
          break;
        case ItemKind::kConstant:
          constant = select.constants[index];
          break;
      }
      out.columns.push_back(std::move(column));
      out.constants.push_back(std::move(constant));
    }
    if (select.constants.empty()) {
      out.constants.clear();
    }
    node = Make(std::move(out));
  };
  switch (shape) {
    case Shape::kGrouped: {
      // GroupAggregate outputs the keys, then the aggregates (hidden ORDER BY ones last); a Project
      // restores the select order.
      const std::size_t key_count = keys.size();
      const std::vector<BoundColumn> key_columns = keys;
      node = Make(GroupAggregateNode{.input = std::move(node),
                                     .keys = std::move(keys),
                                     .aggregates = std::move(select.aggregates),
                                     .span = stmt.group_by_span});
      sort();
      project(
          [&](const BoundColumn& column) {
            const auto key = std::ranges::find_if(
                key_columns, [&](const BoundColumn& k) { return k.index == column.index; });
            return Narrow<int>(key - key_columns.begin());
          },
          key_count);
      break;
    }
    case Shape::kProjection:
      sort();
      project([](const BoundColumn& column) { return column.index; }, 0);
      break;
    case Shape::kGlobal:
      node = Make(AggregateNode{.input = std::move(node),
                                .aggregates = std::move(select.aggregates),
                                .span = select.span});
      if (!select.constants.empty()) {
        project([](const BoundColumn& column) { return column.index; }, 0);
      }
      break;
  }
  const int64_t offset = stmt.offset.value_or(0);
  if (stmt.limit.has_value() || offset > 0) {
    SourceSpan span = stmt.limit.has_value() ? stmt.limit_span : stmt.offset_span;
    if (stmt.limit.has_value() && stmt.offset.has_value()) {
      span = stmt.limit_span.offset < stmt.offset_span.offset
                 ? Cover(stmt.limit_span, stmt.offset_span)
                 : Cover(stmt.offset_span, stmt.limit_span);
    }
    node = Make(
        LimitNode{.input = std::move(node), .limit = stmt.limit, .offset = offset, .span = span});
  }
  return LogicalPlan{.root = std::move(node), .output = std::move(select.output)};
}

}  // namespace antb1::plan
