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

// count_star(), count(x), sum(x), avg(x), min(x), max(x).
std::string ResultName(const sql::AggregateCall& call) {
  if (call.kind == sql::AggKind::kCountStar || !call.arg.has_value()) {
    return "count_star()";
  }
  return AsciiLower(ToString(ToPlan(call.kind))) + "(" + ArgumentName(call.arg->name) + ")";
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
  bound.kind = ToPlan(call.kind);
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

// `column <op> literal`, with the literal folded exactly into the column's type.
arrow::Result<Predicate> BindComparison(const sql::Comparison& cmp, const BoundColumn& column,
                                        const Table& table) {
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
      if (IsApproximateNumber(lit.text)) {
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
      if (table.StoredAsFloat(column.index)) {
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

struct SelectList {
  std::vector<BoundColumn> columns;       // SELECT * or plain columns, in select order
  std::vector<AggregateCall> aggregates;  // aggregates, in select order
  std::vector<OutputColumn> output;
  // Per output column: an index into `aggregates` (true) or `columns` (false).
  std::vector<std::pair<bool, std::size_t>> items;
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

arrow::Result<SelectList> BindSelectList(const sql::SelectStatement& stmt,
                                         const arrow::Schema& schema, const Columns& columns) {
  SelectList list;
  if (stmt.star) {
    list.span = stmt.star_span;
    for (int i = 0; i < schema.num_fields(); ++i) {
      ARROW_ASSIGN_OR_RAISE(BoundColumn column, columns.Field(i, stmt.star_span));
      list.output.push_back(OutputColumn{.name = column.name, .type = column.type});
      list.items.emplace_back(false, list.columns.size());
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
    if (const auto* call = std::get_if<sql::AggregateCall>(&item.expr)) {
      ARROW_ASSIGN_OR_RAISE(AggregateCall bound, BindAggregate(*call, columns));
      list.output.push_back(
          OutputColumn{.name = item.alias.value_or(ResultName(*call)), .type = bound.type});
      list.items.emplace_back(true, list.aggregates.size());
      list.aliases.push_back(item.alias);
      list.aggregates.push_back(std::move(bound));
      continue;
    }
    const auto& ref = std::get<sql::ColumnRef>(item.expr);
    ARROW_ASSIGN_OR_RAISE(BoundColumn column, columns.Resolve(ref));
    // DuckDB names a plain column by its declared name, not as written.
    list.output.push_back(
        OutputColumn{.name = item.alias.value_or(column.name), .type = column.type});
    list.items.emplace_back(false, list.columns.size());
    list.column_spans.push_back(ref.span);
    list.column_written.push_back(ref.name);
    list.aliases.push_back(item.alias);
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

// Syntax the parser accepts but the engine does not answer yet (docs/sql-subset.md); checked before
// anything else, so such a query is always kUnsupported (exit code 4), never a bind error.
arrow::Status CheckNotYetSupported(const sql::SelectStatement& stmt) {
  const auto distinct = [](const sql::SelectExpr& expr) -> const sql::AggregateCall* {
    const auto* agg = std::get_if<sql::AggregateCall>(&expr);
    return agg != nullptr && agg->distinct ? agg : nullptr;
  };
  for (const sql::SelectItem& item : stmt.items) {
    if (const auto* agg = distinct(item.expr)) {
      return UnsupportedError("COUNT(DISTINCT ...) is not supported yet", agg->span);
    }
  }
  for (const sql::OrderItem& item : stmt.order_by) {
    if (const auto* agg = distinct(item.expr)) {
      return UnsupportedError("COUNT(DISTINCT ...) is not supported yet", agg->span);
    }
  }
  return arrow::Status::OK();
}

// The GROUP BY keys: a table column, or else the alias of a plain column in the select list (the
// last item with that alias, as in DuckDB); duplicates are dropped.
arrow::Result<std::vector<BoundColumn>> BindGroupBy(const sql::SelectStatement& stmt,
                                                    const Columns& columns,
                                                    const SelectList& select) {
  std::vector<BoundColumn> keys;
  for (const sql::ColumnRef& ref : stmt.group_by) {
    auto column = columns.Resolve(ref);
    if (!column.ok()) {
      const auto detail = GetSqlError(column.status());
      const std::string wanted = AsciiLower(ref.name);
      // The last select item with the alias wins, as in DuckDB.
      for (std::size_t n = select.items.size();
           n > 0 && detail != nullptr && detail->kind() == SqlErrorDetail::Kind::kBind; --n) {
        const std::size_t i = n - 1;
        const auto& alias = select.aliases[i];
        if (!alias.has_value() || AsciiLower(*alias) != wanted) {
          continue;
        }
        const auto [aggregate, index] = select.items[i];
        if (aggregate) {
          return BindError(std::format("GROUP BY cannot refer to the aggregate '{}'", *alias),
                           ref.span);
        }
        column = select.columns[index];
        break;
      }
    }
    ARROW_RETURN_NOT_OK(column.status());
    const bool duplicate = std::ranges::any_of(
        keys, [&](const BoundColumn& key) { return key.index == column->index; });
    if (!duplicate) {
      keys.push_back(*std::move(column));
    }
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
// sort. A select alias comes before a table column, as in DuckDB; a later key on a column already
// ordered by changes nothing and is dropped.
arrow::Result<std::vector<SortKey>> BindOrderBy(const sql::SelectStatement& stmt,
                                                const Columns& columns, const SelectList& select,
                                                Shape shape, const std::vector<BoundColumn>& keys,
                                                std::vector<AggregateCall>& aggregates) {
  const auto not_grouped = [](const sql::ColumnRef& ref) {
    return BindError(std::format("column '{}' must appear in the GROUP BY clause or be inside an "
                                 "aggregate function",
                                 ref.name),
                     ref.span);
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
    } else {
      const auto& ref = std::get<sql::ColumnRef>(item.expr);
      const auto alias = FindAlias(select, ref.name);
      if (alias.has_value() && select.items[*alias].first) {
        // An aggregate of the select list (kGlobal: its only row needs no sort).
        if (shape == Shape::kGrouped) {
          column = aggregate_column(select.items[*alias].second, select.output[*alias].name);
        }
      } else {
        BoundColumn table_column;
        if (alias.has_value()) {
          table_column = select.columns[select.items[*alias].second];
        } else {
          ARROW_ASSIGN_OR_RAISE(table_column, columns.Resolve(ref));
        }
        if (shape == Shape::kGlobal) {
          return not_grouped(ref);
        }
        if (shape == Shape::kGrouped) {
          const auto key = key_of(table_column);
          if (!key.has_value()) {
            return not_grouped(ref);
          }
          table_column.index = *key;
        }
        column = std::move(table_column);
      }
    }
    if (!column.has_value()) {
      continue;  // kGlobal
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
  ARROW_RETURN_NOT_OK(CheckNotYetSupported(stmt));
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
  if (!keys.empty()) {
    shape = Shape::kGrouped;
  } else if (select.aggregates.empty()) {
    shape = Shape::kProjection;
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
  switch (shape) {
    case Shape::kGrouped: {
      // GroupAggregate outputs the keys, then the aggregates (hidden ORDER BY ones last); a Project
      // restores the select order.
      std::vector<BoundColumn> projected;
      for (std::size_t i = 0; i < select.items.size(); ++i) {
        const auto [aggregate, index] = select.items[i];
        if (aggregate) {
          projected.push_back(BoundColumn{.index = Narrow<int>(keys.size() + index),
                                          .name = select.output[i].name,
                                          .type = select.aggregates[index].type});
          continue;
        }
        const BoundColumn& column = select.columns[index];
        const auto key = std::ranges::find_if(
            keys, [&](const BoundColumn& k) { return k.index == column.index; });
        projected.push_back(BoundColumn{
            .index = Narrow<int>(key - keys.begin()), .name = column.name, .type = column.type});
      }
      node = Make(GroupAggregateNode{.input = std::move(node),
                                     .keys = std::move(keys),
                                     .aggregates = std::move(select.aggregates),
                                     .span = stmt.group_by_span});
      sort();
      node = Make(ProjectNode{
          .input = std::move(node), .columns = std::move(projected), .span = select.span});
      break;
    }
    case Shape::kProjection:
      sort();
      node = Make(ProjectNode{
          .input = std::move(node), .columns = std::move(select.columns), .span = select.span});
      break;
    case Shape::kGlobal:
      node = Make(AggregateNode{.input = std::move(node),
                                .aggregates = std::move(select.aggregates),
                                .span = select.span});
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
