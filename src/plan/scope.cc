#include "scope.h"

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
#include "antb1/common/narrow.h"
#include "antb1/common/source_span.h"
#include "antb1/plan/catalog.h"
#include "antb1/plan/logical_plan.h"
#include "antb1/plan/sql_status.h"
#include "antb1/plan/types.h"
#include "antb1/sql/ast.h"

namespace antb1::plan {
namespace {

// Whether every column of the bindings has an id of its own (checked in debug builds).
[[maybe_unused]] bool IdsAreUnique(const std::vector<Binding>& bindings) {
  std::vector<ColumnId> ids;
  for (const Binding& binding : bindings) {
    for (const BindingColumn& column : binding.columns()) {
      ids.push_back(column.id);
    }
  }
  std::ranges::sort(ids);
  return std::ranges::adjacent_find(ids) == ids.end();
}

}  // namespace

Binding::Binding(std::string name, BindingSource source, std::vector<BindingColumn> columns)
    : name_(std::move(name)), source_(std::move(source)), columns_(std::move(columns)) {
  lower_.reserve(columns_.size());
  for (const BindingColumn& column : columns_) {
    lower_.push_back(AsciiLower(column.name));
  }
}

Binding Binding::OfTable(std::string name, TableSource source, ColumnIdSource& ids) {
  ANTB1_CHECK(source.table != nullptr);
  const arrow::Schema& schema = *source.table->schema();
  std::vector<BindingColumn> columns;
  columns.reserve(Narrow<std::size_t>(schema.num_fields()));
  for (int i = 0; i < schema.num_fields(); ++i) {
    const arrow::Field& field = *schema.field(i);
    BindingColumn column{.name = field.name(),
                         .id = ids.Next(),
                         .type = std::nullopt,
                         .unsupported_type = {},
                         .stored_as_float = source.table->StoredAsFloat(i)};
    // A field without an engine type is an error only where a query refers to it (Reference).
    if (arrow::Result<LogicalType> type = FromArrow(*field.type()); type.ok()) {
      column.type = *type;
    } else {
      column.unsupported_type = field.type()->ToString();
    }
    columns.push_back(std::move(column));
  }
  return {std::move(name), std::move(source), std::move(columns)};
}

Binding Binding::OfPlan(std::string name, LogicalNodePtr root, std::vector<BindingColumn> columns) {
  ANTB1_CHECK(root != nullptr);
  std::vector<ColumnId> ids;
  ids.reserve(columns.size());
  for (const BindingColumn& column : columns) {
    ANTB1_CHECK(column.id != kNoColumnId);
    ANTB1_CHECK(column.type.has_value());
    ids.push_back(column.id);
  }
  ANTB1_CHECK(OutputIds(*root) == ids);
  return {std::move(name), PlanSource{.root = std::move(root)}, std::move(columns)};
}

NameMatches Binding::Match(std::string_view name) const {
  const std::string wanted = AsciiLower(name);
  NameMatches matches;
  for (std::size_t i = 0; i < lower_.size(); ++i) {
    if (lower_[i] != wanted) {
      continue;
    }
    if (matches.first.has_value()) {
      matches.second = i;
      break;
    }
    matches.first = i;
  }
  return matches;
}

LogicalNodePtr Binding::Node() const {
  // One overload per source: a new kind of source fails to compile until it is handled.
  struct NodeOf {
    const std::vector<BindingColumn>& columns;

    LogicalNodePtr operator()(const TableSource& table) const {
      ScanNode scan{.table = table.table,
                    .table_name = table.table_name,
                    .fields = {},
                    .ids = {},
                    .span = table.span};
      scan.fields.reserve(columns.size());
      scan.ids.reserve(columns.size());
      for (std::size_t i = 0; i < columns.size(); ++i) {
        scan.fields.push_back(Narrow<int>(i));
        scan.ids.push_back(columns[i].id);
      }
      return std::make_shared<const LogicalNode>(std::move(scan));
    }
    LogicalNodePtr operator()(const PlanSource& plan) const { return plan.root; }
  };
  return std::visit(NodeOf{.columns = columns_}, source_);
}

Scope::Scope(std::vector<Binding> bindings, const Scope* outer)
    : bindings_(std::move(bindings)), outer_(outer) {
  ANTB1_DCHECK(IdsAreUnique(bindings_));
}

arrow::Result<BoundColumn> Scope::Resolve(const sql::ColumnRef& ref) const {
  // CheckSupported rejected every qualified name, except in the arguments of a call with the
  // wrong number of them, which BindFunction rejects before it binds an argument.
  ANTB1_CHECK(ref.qualifier.empty());
  // CheckSupported rejected every FROM list of several items; rules 2-4 of ADR 0022 come with J2b.
  ANTB1_CHECK(bindings_.size() == 1);
  const Binding& binding = bindings_.front();
  const NameMatches matches = binding.Match(ref.name);
  if (matches.first.has_value() && matches.second.has_value()) {
    return BindError(std::format("column name '{}' is ambiguous: it matches the columns '{}' "
                                 "and '{}', which differ only in case",
                                 ref.name, binding.columns()[*matches.first].name,
                                 binding.columns()[*matches.second].name),
                     ref.span);
  }
  if (!matches.first.has_value()) {
    return BindError(std::format("column '{}' does not exist", ref.name), ref.span);
  }
  return Reference(ColumnLocation{.binding = 0, .column = *matches.first}, ref.span);
}

arrow::Result<BoundColumn> Scope::Reference(ColumnLocation where, SourceSpan span) const {
  const BindingColumn& c = column(where);
  if (!c.type.has_value()) {
    return UnsupportedError(
        std::format("column '{}' has the unsupported type {}", c.name, c.unsupported_type), span);
  }
  return BoundColumn{.id = c.id, .name = c.name, .type = *c.type};
}

std::optional<ColumnLocation> Scope::Find(ColumnId id) const {
  for (std::size_t b = 0; b < bindings_.size(); ++b) {
    const std::vector<BindingColumn>& columns = bindings_[b].columns();
    for (std::size_t c = 0; c < columns.size(); ++c) {
      if (columns[c].id == id) {
        return ColumnLocation{.binding = b, .column = c};
      }
    }
  }
  return std::nullopt;
}

const BindingColumn& Scope::column(ColumnLocation where) const {
  ANTB1_CHECK(where.binding < bindings_.size());
  const std::vector<BindingColumn>& columns = bindings_[where.binding].columns();
  ANTB1_CHECK(where.column < columns.size());
  return columns[where.column];
}

bool Scope::StoredAsFloat(ColumnId id) const {
  const std::optional<ColumnLocation> where = Find(id);
  return where.has_value() && column(*where).stored_as_float;
}

}  // namespace antb1::plan
