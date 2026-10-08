#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include <arrow/result.h>

#include "antb1/common/source_span.h"
#include "antb1/plan/logical_plan.h"
#include "antb1/plan/table.h"
#include "antb1/plan/types.h"
#include "antb1/sql/ast.h"

// The binder's name scopes (docs/adr/0022-joins-and-query-blocks.md, "Scopes of bindings"). A
// binding is one FROM item: its name, its source and its columns. A scope holds the bindings of one
// query block, in FROM order, and links to the scope of the enclosing block. Private to the plan
// module: the binder and its tests include it.

namespace antb1::plan {

// Mints the ids of a query's columns (plan::ColumnId): from 1, in binding order. One per query,
// shared by its bindings and its binder; not thread-safe.
class ColumnIdSource {
 public:
  ColumnId Next() { return ColumnId{++last_}; }

 private:
  std::uint32_t last_ = 0;
};

// A table or a path in FROM, as the Scan of its binding shows it.
struct TableSource {
  std::shared_ptr<Table> table;  // not null
  std::string table_name;        // ScanNode::table_name: the FROM reference as written, or the path
  SourceSpan span;               // ScanNode::span: the FROM reference
};

// A bound sub-plan in FROM (a derived table or a CTE reference, from roadmap PR J4 on): its root
// outputs the binding's columns, in order.
struct PlanSource {
  LogicalNodePtr root;
};

using BindingSource = std::variant<TableSource, PlanSource>;

struct BindingColumn {
  std::string name;  // as declared
  ColumnId id = kNoColumnId;
  // Empty for a column without an engine type, which is kUnsupported where it is referenced and
  // only there; `unsupported_type` is then its Arrow type's text.
  std::optional<LogicalType> type;
  std::string unsupported_type;
  bool stored_as_float = false;  // a DOUBLE stored as FLOAT (Table::StoredAsFloat, divergence D11)
};

// The columns of a binding that a name matches: the first two, in column order.
struct NameMatches {
  std::optional<std::size_t> first;
  std::optional<std::size_t> second;
};

// One FROM item; immutable.
class Binding {
 public:
  // Column i is field i of the table, which must not be null, with an id minted from `ids`, in
  // schema order. A field without an engine type is no error here: it keeps its column and id
  // (BindingColumn::type).
  static Binding OfTable(std::string name, TableSource source, ColumnIdSource& ids);
  // The columns as given. Every column must have an id and a type, and the root must output
  // exactly the columns' ids, in order: anything else is a programming error.
  static Binding OfPlan(std::string name, LogicalNodePtr root, std::vector<BindingColumn> columns);

  // Until roadmap PR J2b, which names bindings by rule 1 of ADR 0022, the FROM reference as
  // written; nothing consults it yet.
  [[nodiscard]] const std::string& name() const { return name_; }
  [[nodiscard]] const BindingSource& source() const { return source_; }
  [[nodiscard]] const std::vector<BindingColumn>& columns() const { return columns_; }

  // The columns `name` names: ASCII case-insensitively, quoted or not (DuckDB).
  [[nodiscard]] NameMatches Match(std::string_view name) const;

  // What reads the binding: a Scan of every field of the table under the columns' ids, or the
  // sub-plan's root.
  [[nodiscard]] LogicalNodePtr Node() const;

 private:
  Binding(std::string name, BindingSource source, std::vector<BindingColumn> columns);

  std::string name_;
  BindingSource source_;
  std::vector<BindingColumn> columns_;
  std::vector<std::string> lower_;  // per column: its name in ASCII lower case
};

// Column `column` of binding `binding` of a scope.
struct ColumnLocation {
  std::size_t binding = 0;
  std::size_t column = 0;

  friend bool operator==(const ColumnLocation&, const ColumnLocation&) = default;
};

// The bindings of one query block, in FROM order; immutable. Neither copied nor moved, since the
// scope of an inner block may point at it: a container of scopes holds them through
// std::unique_ptr.
class Scope {
 public:
  // `outer` is the scope of the enclosing block (nullptr: none), which must outlive this one.
  // Nothing consults it yet: rule 9 of ADR 0022 (a name found only in an enclosing block is a
  // correlation, ADR 0023) comes with roadmap PR J5.
  explicit Scope(std::vector<Binding> bindings, const Scope* outer = nullptr);
  Scope(const Scope&) = delete;
  Scope& operator=(const Scope&) = delete;
  Scope(Scope&&) = delete;
  Scope& operator=(Scope&&) = delete;

  [[nodiscard]] const std::vector<Binding>& bindings() const { return bindings_; }
  [[nodiscard]] const Scope* outer() const { return outer_; }

  // The column an unqualified name refers to, by its id (plan::ResolvePositions sets its position
  // at the end): a bind error when two columns match it (reported first) or none does, else as
  // Reference. Until roadmap PR J2b a scope resolves names in its one binding: a qualified name or
  // another number of bindings is a programming error. J2b also limits a name in an ON to the
  // bindings up to and including its own JOIN (rule 10 of ADR 0022).
  [[nodiscard]] arrow::Result<BoundColumn> Resolve(const sql::ColumnRef& ref) const;

  // The column at `where`, referred to at `span`: kUnsupported there when it has no engine type.
  [[nodiscard]] arrow::Result<BoundColumn> Reference(ColumnLocation where, SourceSpan span) const;

  // Where column `id` is, if it is a binding's column (and not a computed one).
  [[nodiscard]] std::optional<ColumnLocation> Find(ColumnId id) const;

  // The column at `where`, which must be in range.
  [[nodiscard]] const BindingColumn& column(ColumnLocation where) const;

  // Whether column `id` is a binding's column stored as FLOAT (false for a computed column).
  [[nodiscard]] bool StoredAsFloat(ColumnId id) const;

 private:
  std::vector<Binding> bindings_;
  const Scope* outer_ = nullptr;
};

}  // namespace antb1::plan
