#include "scope.h"

#include <algorithm>
#include <cstddef>
#include <format>
#include <memory>
#include <optional>
#include <span>
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

// The parts of `text` between the separators, empty ones skipped (DuckDB's StringUtil::Split).
std::vector<std::string_view> NonEmptyParts(std::string_view text, char separator) {
  std::vector<std::string_view> parts;
  std::size_t begin = 0;
  while (begin <= text.size()) {
    const std::size_t end = std::min(text.find(separator, begin), text.size());
    if (end > begin) {
      parts.push_back(text.substr(begin, end - begin));
    }
    begin = end + 1;
  }
  return parts;
}

// Where `ref` resolves in bindings [begin, end) of `bindings`: the first two matching columns.
Lookup LookUpIn(const std::vector<Binding>& bindings, const sql::ColumnRef& ref, std::size_t begin,
                std::size_t end) {
  std::vector<ColumnLocation> matches;
  for (std::size_t b = begin; b < end && matches.size() < 2; ++b) {
    if (!ref.qualifier.empty() && !bindings[b].Named(ref.qualifier)) {
      continue;
    }
    const NameMatches columns = bindings[b].Match(ref.name);
    for (const std::optional<std::size_t>& column : {columns.first, columns.second}) {
      if (column.has_value()) {
        matches.push_back(ColumnLocation{.binding = b, .column = *column});
      }
    }
  }
  if (matches.empty()) {
    return {};
  }
  if (matches.size() == 1) {
    return {.outcome = Lookup::Outcome::kFound, .where = matches[0], .other = {}};
  }
  return {.outcome = Lookup::Outcome::kAmbiguous, .where = matches[0], .other = matches[1]};
}

// A binding's name and the declared name of one of its columns, for a message: t.x.
std::string QualifiedText(const std::vector<Binding>& bindings, ColumnLocation where) {
  const Binding& binding = bindings[where.binding];
  return binding.name() + "." + binding.columns()[where.column].name;
}

// Whether a binding of a table is named by an alias that hides the table's name `qualifier`: its
// FROM reference is `qualifier`, as a table name or as a path's name, but the binding is not
// named so.
bool AliasHides(const Binding& binding, std::string_view qualifier) {
  const auto* table = std::get_if<TableSource>(&binding.source());
  if (table == nullptr || binding.Named(qualifier)) {
    return false;
  }
  const std::string written = AsciiLower(table->table_name);
  const std::string path_name = AsciiLower(PathBindingName(table->table_name));
  const std::string wanted = AsciiLower(qualifier);
  const bool aliased = !binding.Named(written) && !binding.Named(path_name);
  return aliased && (wanted == written || wanted == path_name);
}

// Why `ref` resolves nowhere in the bindings before `end`, for its bind error: the name it lacks,
// and the hints that a later FROM item, which an ON does not see, has it, or that an alias hides a
// table's name.
std::string MissingMessage(const std::vector<Binding>& bindings, const sql::ColumnRef& ref,
                           std::size_t end) {
  const std::span<const Binding> visible = std::span(bindings).first(end);
  const std::span<const Binding> later = std::span(bindings).subspan(end);
  const auto has_column = [&ref](const Binding& b) { return b.Match(ref.name).first.has_value(); };
  const auto named = [&ref](const Binding& b) { return b.Named(ref.qualifier); };
  constexpr std::string_view kLaterHint = " (an ON sees only the FROM items up to its JOIN)";
  if (ref.qualifier.empty()) {
    std::string message = std::format("column '{}' does not exist", ref.name);
    if (std::ranges::any_of(later, has_column)) {
      message += kLaterHint;
    }
    return message;
  }
  if (std::ranges::any_of(visible, named)) {
    std::string message = std::format("'{}' has no column '{}'", ref.qualifier, ref.name);
    if (std::ranges::any_of(later, [&](const Binding& b) { return named(b) && has_column(b); })) {
      message += kLaterHint;
    }
    return message;
  }
  std::string message = std::format("no FROM item is named '{}'", ref.qualifier);
  if (std::ranges::any_of(later, named)) {
    message += kLaterHint;
  }
  const auto hiding = std::ranges::find_if(
      bindings, [&ref](const Binding& b) { return AliasHides(b, ref.qualifier); });
  if (hiding != bindings.end()) {
    message += std::format(" (the alias '{}' hides it)", hiding->name());
  }
  return message;
}

// The bind error of an ambiguous `ref` (Lookup::Outcome::kAmbiguous).
std::string AmbiguityMessage(const std::vector<Binding>& bindings, const sql::ColumnRef& ref,
                             const Lookup& lookup) {
  if (lookup.where.binding == lookup.other.binding) {
    const std::vector<BindingColumn>& columns = bindings[lookup.where.binding].columns();
    return std::format(
        "column name '{}' is ambiguous: it matches the columns '{}' and '{}', which differ only "
        "in case",
        ref.name, columns[lookup.where.column].name, columns[lookup.other.column].name);
  }
  if (!ref.qualifier.empty()) {
    return std::format("'{}.{}' is ambiguous: two FROM items named '{}' have a column '{}'",
                       ref.qualifier, ref.name, ref.qualifier, ref.name);
  }
  return std::format("column name '{}' is ambiguous: it matches {} and {}", ref.name,
                     QualifiedText(bindings, lookup.where), QualifiedText(bindings, lookup.other));
}

}  // namespace

std::string PathBindingName(std::string_view path) {
  if (path.find_first_of("*?[") != std::string_view::npos) {
    return std::string(path);
  }
  const std::vector<std::string_view> components = NonEmptyParts(path, '/');
  if (components.empty()) {
    return std::string(path);
  }
  const std::string_view file = components.back();
  const std::vector<std::string_view> pieces = NonEmptyParts(file, '.');
  return std::string(pieces.empty() ? file : pieces.front());
}

Binding::Binding(std::string name, BindingSource source, std::vector<BindingColumn> columns)
    : name_(std::move(name)),
      lower_name_(AsciiLower(name_)),
      source_(std::move(source)),
      columns_(std::move(columns)) {
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

bool Binding::Named(std::string_view qualifier) const {
  return AsciiLower(qualifier) == lower_name_;
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

Lookup Scope::LookUp(const sql::ColumnRef& ref, Visibility visible) const {
  const std::size_t end = std::min(visible.end, bindings_.size());
  const std::size_t inner = std::min(visible.inner_begin, end);
  Lookup lookup = LookUpIn(bindings_, ref, inner, end);
  if (lookup.outcome == Lookup::Outcome::kMissing) {
    lookup = LookUpIn(bindings_, ref, 0, inner);
  }
  return lookup;
}

arrow::Result<BoundColumn> Scope::Resolve(const sql::ColumnRef& ref, Visibility visible) const {
  const Lookup lookup = LookUp(ref, visible);
  switch (lookup.outcome) {
    case Lookup::Outcome::kFound:
      return Reference(lookup.where, ref.span);
    case Lookup::Outcome::kAmbiguous:
      return BindError(AmbiguityMessage(bindings_, ref, lookup), ref.span);
    case Lookup::Outcome::kMissing:
      break;
  }
  return BindError(MissingMessage(bindings_, ref, std::min(visible.end, bindings_.size())),
                   ref.span);
}

arrow::Result<BoundColumn> Scope::Reference(ColumnLocation where, SourceSpan span) const {
  const BindingColumn& c = column(where);
  if (!c.type.has_value()) {
    return UnsupportedError(
        std::format("column '{}' has the unsupported type {}", c.name, c.unsupported_type), span);
  }
  return BoundColumn{
      .id = c.id, .name = c.name, .type = *c.type, .qualifier = Qualifier(where.binding)};
}

std::string Scope::Qualifier(std::size_t binding) const {
  ANTB1_CHECK(binding < bindings_.size());
  return bindings_.size() >= 2 ? bindings_[binding].name() : std::string();
}

std::optional<Scope::StarConflict> Scope::FindStarConflict() const {
  for (std::size_t first = 0; first < bindings_.size(); ++first) {
    const Binding& a = bindings_[first];
    for (std::size_t second = first + 1; second < bindings_.size(); ++second) {
      const Binding& b = bindings_[second];
      if (!b.Named(a.name())) {
        continue;
      }
      for (const BindingColumn& column : a.columns()) {
        if (b.Match(column.name).first.has_value()) {
          return StarConflict{.first = first, .second = second, .column = column.name};
        }
      }
    }
  }
  return std::nullopt;
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
