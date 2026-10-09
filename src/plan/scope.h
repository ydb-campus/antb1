#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
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

// The name of the binding of a path in FROM without an alias, as DuckDB names it (rule 1 of ADR
// 0022): the file name up to its first dot, leading dots skipped (`dir/a.b.parquet` is `a`,
// `..x.parquet` is `x`, `nodot` is itself), and the whole text for a path with a glob character
// (`*`, `?` or `[`). Like DuckDB it skips empty parts: a file name of dots only names itself, and a
// path without a part (only slashes, or empty) is its whole text.
std::string PathBindingName(std::string_view path);

// A table or a path in FROM, as the Scan of its binding shows it.
struct TableSource {
  std::shared_ptr<Table> table;  // not null
  std::string table_name;        // ScanNode::table_name: the FROM reference as written, or the path
  SourceSpan span;               // ScanNode::span: the FROM reference
  // Whether the reference is a table's name or a path. Without an alias a table's binding is named
  // by the name as written, a path's by its PathBindingName (rule 1 of ADR 0022): the name that a
  // bind error says an alias hides.
  sql::TableRef::Kind kind = sql::TableRef::Kind::kName;
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

  // The name a qualified column reference uses (rule 1 of ADR 0022): the FROM item's alias, else
  // its table name as written, else its path's PathBindingName. Two bindings may share it (rule 3).
  [[nodiscard]] const std::string& name() const { return name_; }
  [[nodiscard]] const BindingSource& source() const { return source_; }
  [[nodiscard]] const std::vector<BindingColumn>& columns() const { return columns_; }

  // Whether `qualifier` names the binding: ASCII case-insensitively, quoted or not (DuckDB).
  [[nodiscard]] bool Named(std::string_view qualifier) const;

  // The columns `name` names: ASCII case-insensitively, quoted or not (DuckDB).
  [[nodiscard]] NameMatches Match(std::string_view name) const;

  // What reads the binding: a Scan of every field of the table under the columns' ids, or the
  // sub-plan's root.
  [[nodiscard]] LogicalNodePtr Node() const;

 private:
  Binding(std::string name, BindingSource source, std::vector<BindingColumn> columns);

  std::string name_;
  std::string lower_name_;  // name_ in ASCII lower case
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

// The bindings of a scope that a name may resolve in (rule 10 of ADR 0022, in two levels as in
// DuckDB): those before `end`. A name resolves in the inner level, [inner_begin, end), and only
// when no binding there has it in the outer level, [0, inner_begin). For an ON, `end` follows its
// own JOIN's item and the inner level is its join group, the items from the last comma up to it (a
// CROSS JOIN continues a group): its earlier comma siblings form the outer level, and later items
// are invisible. The default, for every other clause, is every binding in one level.
struct Visibility {
  std::size_t inner_begin = 0;
  std::size_t end = std::numeric_limits<std::size_t>::max();
};

// Where a name resolves (Scope::LookUp). Matches count in FROM order, and within a binding in
// column order.
struct Lookup {
  enum class Outcome : std::uint8_t { kFound, kMissing, kAmbiguous };
  Outcome outcome = Outcome::kMissing;
  ColumnLocation where;  // kFound: the column; kAmbiguous: the first match
  ColumnLocation other;  // kAmbiguous: the second match
};

// The bindings of one query block, in FROM order; immutable. Neither copied nor moved, since the
// scope of an inner block may point at it: a container of scopes holds them through
// std::unique_ptr.
class Scope {
 public:
  // `outer` is the scope of the enclosing block (nullptr: none), which must outlive this one.
  // Nothing consults it yet: rule 9 of ADR 0022 (a name found only in an enclosing block is a
  // correlation, ADR 0023) comes with roadmap PR J5, and a Visibility limits this block's bindings
  // only.
  explicit Scope(std::vector<Binding> bindings, const Scope* outer = nullptr);
  Scope(const Scope&) = delete;
  Scope& operator=(const Scope&) = delete;
  Scope(Scope&&) = delete;
  Scope& operator=(Scope&&) = delete;

  [[nodiscard]] const std::vector<Binding>& bindings() const { return bindings_; }
  [[nodiscard]] const Scope* outer() const { return outer_; }

  // Where `ref` resolves among the `visible` bindings, level by level (Visibility): a qualified
  // name among the bindings it names (rules 2 and 3 of ADR 0022), an unqualified one among all of
  // them. A level with two or more matching columns is ambiguous (two bindings that have the name,
  // or two columns of one binding whose names differ only in case); with none, the next level is
  // searched.
  [[nodiscard]] Lookup LookUp(const sql::ColumnRef& ref, Visibility visible = {}) const;

  // The column `ref` refers to (LookUp), by its id (plan::ResolvePositions sets its position at the
  // end), as Reference gives it. A bind error at the name when it is ambiguous (rules 3 and 4: the
  // error names the qualified candidates) or found nowhere; the message then says when a later
  // FROM item, which an ON does not see, has the name, and when an alias hides the name of a
  // binding's table (rule 2).
  [[nodiscard]] arrow::Result<BoundColumn> Resolve(const sql::ColumnRef& ref,
                                                   Visibility visible = {}) const;

  // The column at `where`, referred to at `span`, with its binding's Qualifier: kUnsupported there
  // when it has no engine type.
  [[nodiscard]] arrow::Result<BoundColumn> Reference(ColumnLocation where, SourceSpan span) const;

  // The qualifier of binding `binding`'s columns (BoundColumn::qualifier, which EXPLAIN shows): its
  // name in a scope of two or more bindings, else none, so that a single table's plans do not
  // change.
  [[nodiscard]] std::string Qualifier(std::size_t binding) const;

  // Two bindings of the same name that share a column name, which make SELECT * a bind error (rule
  // 5 of ADR 0022): the first such pair in FROM order, with the first column (in the first
  // binding's column order) that the second binding has too.
  struct StarConflict {
    std::size_t first = 0;
    std::size_t second = 0;
    std::string column;  // as the first binding declares it
  };
  [[nodiscard]] std::optional<StarConflict> FindStarConflict() const;

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
