#include "../scope.h"

#include <cstddef>
#include <cstdint>
#include <format>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <arrow/api.h>
#include <gtest/gtest.h>

#include "antb1/common/source_span.h"
#include "antb1/plan/catalog.h"
#include "antb1/plan/explain.h"
#include "antb1/plan/logical_plan.h"
#include "antb1/plan/sql_status.h"
#include "antb1/plan/types.h"
#include "antb1/sql/ast.h"

#include "plan_test_util.h"

namespace antb1::plan {
namespace {

using testing::AllTypesSchema;
using testing::BindSql;
using testing::FakeTable;
using testing::MakeCatalog;
using testing::Nth;

constexpr SqlErrorDetail::Kind kBind = SqlErrorDetail::Kind::kBind;
constexpr SqlErrorDetail::Kind kUnsupported = SqlErrorDetail::Kind::kUnsupported;

// The binding `name` of a table with `schema`, whose fields `float_fields` are stored as FLOAT.
Binding TableBinding(const std::string& name, std::shared_ptr<arrow::Schema> schema,
                     ColumnIdSource& ids, std::vector<int> float_fields = {}) {
  auto table = std::make_shared<FakeTable>(std::move(schema), 1, std::move(float_fields));
  return Binding::OfTable(
      name, TableSource{.table = std::move(table), .table_name = name, .span = {}}, ids);
}

// "ok" of MakeCatalog: i16 SMALLINT, s VARCHAR.
std::shared_ptr<arrow::Schema> OkSchema() {
  return arrow::schema({arrow::field("i16", arrow::int16()), arrow::field("s", arrow::binary())});
}

// An unqualified column reference, as the parser makes one.
sql::ColumnRef Ref(std::string name, SourceSpan span = {}, bool quoted = false) {
  return sql::ColumnRef{.name = std::move(name),
                        .quoted = quoted,
                        .span = span,
                        .qualifier = {},
                        .qualifier_quoted = false};
}

std::string UnsupportedListMessage(std::string_view column) {
  return std::format("column '{}' has the unsupported type {}", column,
                     arrow::list(arrow::int32())->ToString());
}

// `status` is a SQL error of `kind` with exactly `message`, at `span`.
void ExpectSqlError(const arrow::Status& status, SqlErrorDetail::Kind kind,
                    std::string_view message, SourceSpan span) {
  const auto detail = GetSqlError(status);
  ASSERT_NE(detail, nullptr) << status.ToString();
  EXPECT_EQ(detail->kind(), kind) << status.ToString();
  EXPECT_EQ(status.message(), message);
  EXPECT_EQ(detail->span(), span) << status.ToString();
  // kUnsupported is NotImplemented (exit code 4); kBind is Invalid (exit code 1).
  EXPECT_EQ(status.IsNotImplemented(), kind == kUnsupported) << status.ToString();
  EXPECT_EQ(status.IsInvalid(), kind == kBind) << status.ToString();
}

TEST(BindingTest, OfTableMintsOneIdPerFieldInSchemaOrder) {
  ColumnIdSource ids;
  const Binding t = TableBinding("t", AllTypesSchema(), ids);
  const Binding u = TableBinding("u", AllTypesSchema(), ids);
  ASSERT_EQ(t.columns().size(), 11U);
  ASSERT_EQ(u.columns().size(), 11U);
  for (std::uint32_t i = 0; i < 11; ++i) {
    EXPECT_EQ(t.columns()[i].id, ColumnId{i + 1}) << i;
    EXPECT_EQ(u.columns()[i].id, ColumnId{i + 12}) << i;  // the second binding continues
  }
  EXPECT_EQ(ids.Next(), ColumnId{23});  // one id per field, and no more
}

// A field without an engine type keeps its column and id, with its Arrow type's text: binding it
// is no error.
TEST(BindingTest, OfTableKeepsDeclaredNamesTypesAndUnsupportedFields) {
  ColumnIdSource ids;
  const Binding t = TableBinding("t", AllTypesSchema(), ids);
  const std::vector<std::pair<std::string, std::optional<LogicalType>>> expected = {
      {"i16", LogicalType::kSmallInt},
      {"i32", LogicalType::kInteger},
      {"i64", LogicalType::kBigInt},
      {"u16", LogicalType::kUSmallInt},
      {"h", LogicalType::Decimal(38, 0)},
      {"d", LogicalType::kDouble},
      {"s", LogicalType::kVarchar},
      {"dt", LogicalType::kDate},
      {"bad", std::nullopt},
      {"Mixed Case", LogicalType::kInteger},
      {"from", LogicalType::kInteger}};
  ASSERT_EQ(t.columns().size(), expected.size());
  for (std::size_t i = 0; i < expected.size(); ++i) {
    const BindingColumn& column = t.columns()[i];
    EXPECT_EQ(column.name, expected[i].first) << i;
    EXPECT_EQ(column.type, expected[i].second) << i;
    EXPECT_EQ(column.unsupported_type.empty(), expected[i].second.has_value()) << i;
    EXPECT_FALSE(column.stored_as_float) << i;
  }
  EXPECT_EQ(t.columns()[8].id, ColumnId{9});
  EXPECT_EQ(t.columns()[8].unsupported_type, arrow::list(arrow::int32())->ToString());
}

// The FLOAT flag comes from the table (Table::StoredAsFloat), not from the Arrow type: io shows a
// FLOAT column as float64 and flags it, and a float32 field that the table does not flag is no
// FLOAT column.
TEST(BindingTest, OfTableTakesTheFloatFlagFromTheTable) {
  ColumnIdSource ids;
  const Binding b = TableBinding(
      "tf",
      arrow::schema({arrow::field("f", arrow::float64()), arrow::field("g", arrow::float32()),
                     arrow::field("k", arrow::int32())}),
      ids, {0});
  ASSERT_EQ(b.columns().size(), 3U);
  EXPECT_EQ(b.columns()[0].type, LogicalType(LogicalType::kDouble));
  EXPECT_TRUE(b.columns()[0].stored_as_float);
  EXPECT_EQ(b.columns()[1].type, LogicalType(LogicalType::kDouble));
  EXPECT_FALSE(b.columns()[1].stored_as_float);
  EXPECT_FALSE(b.columns()[2].stored_as_float);
}

TEST(BindingTest, MatchIsCaseInsensitiveAndReportsTheFirstTwoMatches) {
  ColumnIdSource ids;
  const Binding b = TableBinding(
      "m",
      arrow::schema({arrow::field("Ab", arrow::int32()), arrow::field("x", arrow::int32()),
                     arrow::field("aB", arrow::int32()), arrow::field("AB", arrow::int32())}),
      ids);
  const NameMatches ab = b.Match("ab");
  EXPECT_EQ(ab.first, std::size_t{0});
  EXPECT_EQ(ab.second, std::size_t{2});  // the first two matches, not the last two
  const NameMatches x = b.Match("X");
  EXPECT_EQ(x.first, std::size_t{1});
  EXPECT_EQ(x.second, std::nullopt);
  const NameMatches none = b.Match("nope");
  EXPECT_EQ(none.first, std::nullopt);
  EXPECT_EQ(none.second, std::nullopt);
}

// A table's binding reads every field under its columns' ids. Its Scan shows the FROM reference
// (TableSource::table_name), never the binding's name: binding "trips" over a path.
TEST(BindingTest, NodeOfATableScansEveryFieldUnderTheBindingIds) {
  ColumnIdSource ids;
  const Binding first = TableBinding("ok", OkSchema(), ids);  // ids 1 and 2
  const auto table = std::make_shared<FakeTable>(AllTypesSchema(), 1);
  const SourceSpan span{.offset = 14, .length = 19};
  const Binding trips = Binding::OfTable(
      "trips", TableSource{.table = table, .table_name = "dir/trips.parquet", .span = span}, ids);
  EXPECT_EQ(trips.name(), "trips");
  const LogicalNodePtr node = trips.Node();
  ASSERT_NE(node, nullptr);
  const auto* scan = std::get_if<ScanNode>(node.get());
  ASSERT_NE(scan, nullptr);
  EXPECT_EQ(scan->table, table);
  EXPECT_EQ(scan->table_name, "dir/trips.parquet");
  EXPECT_EQ(scan->span, span);
  EXPECT_EQ(scan->fields, (std::vector<int>{0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10}));
  std::vector<ColumnId> columns;
  for (const BindingColumn& column : trips.columns()) {
    columns.push_back(column.id);
  }
  EXPECT_EQ(scan->ids, columns);
  EXPECT_EQ(scan->ids.front(), ColumnId{3});
  EXPECT_TRUE(ExplainNode(*node).starts_with("Scan table=dir/trips.parquet "))
      << ExplainNode(*node);
}

// A sub-plan's binding (roadmap PR J4) takes its columns as given, their FLOAT flags included, and
// is read by the sub-plan's root itself.
TEST(BindingTest, ASubPlanBindingTakesItsColumnsAsGiven) {
  const auto table = std::make_shared<FakeTable>(
      arrow::schema({arrow::field("a", arrow::int32()), arrow::field("f", arrow::float64())}), 1);
  const LogicalNodePtr root = std::make_shared<const LogicalNode>(ScanNode{
      .table = table, .table_name = "x", .fields = {0, 1}, .ids = {ColumnId{7}, ColumnId{8}}});
  const Binding sub =
      Binding::OfPlan("sub", root,
                      {BindingColumn{.name = "a", .id = ColumnId{7}, .type = LogicalType::kInteger},
                       BindingColumn{.name = "F",
                                     .id = ColumnId{8},
                                     .type = LogicalType::kDouble,
                                     .unsupported_type = {},
                                     .stored_as_float = true}});
  EXPECT_EQ(sub.name(), "sub");
  EXPECT_TRUE(std::holds_alternative<PlanSource>(sub.source()));
  EXPECT_EQ(sub.Node(), root);
  ASSERT_EQ(sub.columns().size(), 2U);
  EXPECT_EQ(sub.columns()[1].name, "F");

  const Scope scope({sub});
  const auto f = scope.Resolve(Ref("f"));
  ASSERT_TRUE(f.ok()) << f.status().ToString();
  EXPECT_EQ(f->id, ColumnId{8});
  EXPECT_EQ(f->name, "F");
  EXPECT_EQ(f->type, LogicalType::kDouble);
  EXPECT_TRUE(scope.StoredAsFloat(ColumnId{8}));  // as given: the table flags no field
  EXPECT_FALSE(scope.StoredAsFloat(ColumnId{7}));
}

// A name resolves to its column by id, ASCII case-insensitively and quoted or not, with the
// declared name; ResolvePositions sets the position later, and the column of a scope's only binding
// has no qualifier.
TEST(ScopeTest, ResolveMatchesAsciiCaseInsensitivelyQuotedOrNot) {
  ColumnIdSource ids;
  const Scope scope({TableBinding("t", AllTypesSchema(), ids)});
  struct Case {
    sql::ColumnRef ref;
    std::string name;
    ColumnId id;
    LogicalType type;
  };
  const std::vector<Case> cases = {
      {.ref = Ref("I16"), .name = "i16", .id = ColumnId{1}, .type = LogicalType::kSmallInt},
      {.ref = Ref("H"), .name = "h", .id = ColumnId{5}, .type = LogicalType::Decimal(38, 0)},
      {.ref = Ref("MIXED CASE", {}, /*quoted=*/true),
       .name = "Mixed Case",
       .id = ColumnId{10},
       .type = LogicalType::kInteger},
      {.ref = Ref("FROM", {}, /*quoted=*/true),
       .name = "from",
       .id = ColumnId{11},
       .type = LogicalType::kInteger},
  };
  for (const Case& c : cases) {
    const auto column = scope.Resolve(c.ref);
    ASSERT_TRUE(column.ok()) << c.ref.name << ": " << column.status().ToString();
    EXPECT_EQ(column->id, c.id) << c.ref.name;
    EXPECT_EQ(column->name, c.name) << c.ref.name;
    EXPECT_EQ(column->type, c.type) << c.ref.name;
    EXPECT_EQ(column->index, 0) << c.ref.name;
    EXPECT_EQ(column->qualifier, "") << c.ref.name;
  }
}

TEST(ScopeTest, AnUnknownColumnIsABindErrorAsWritten) {
  ColumnIdSource ids;
  const Scope scope({TableBinding("t", AllTypesSchema(), ids)});
  const SourceSpan span{.offset = 7, .length = 4};
  ExpectSqlError(scope.Resolve(Ref("NoPe", span)).status(), kBind, "column 'NoPe' does not exist",
                 span);
}

TEST(ScopeTest, AmbiguityNamesTheFirstTwoMatchesInColumnOrder) {
  ColumnIdSource ids;
  const Scope scope({TableBinding(
      "m",
      arrow::schema({arrow::field("Ab", arrow::int32()), arrow::field("x", arrow::int32()),
                     arrow::field("aB", arrow::int32()), arrow::field("AB", arrow::int32())}),
      ids)});
  const SourceSpan span{.offset = 3, .length = 2};
  ExpectSqlError(scope.Resolve(Ref("AB", span)).status(), kBind,
                 "column name 'AB' is ambiguous: it matches the columns 'Ab' and 'aB', which "
                 "differ only in case",
                 span);
}

// Two matches are ambiguous whatever their types: the unsupported type of one of them is reported
// only for a name that matches it alone.
TEST(ScopeTest, AmbiguityComesBeforeAnUnsupportedType) {
  ColumnIdSource ids;
  const Scope both({TableBinding("vv",
                                 arrow::schema({arrow::field("v", arrow::list(arrow::int32())),
                                                arrow::field("V", arrow::int32())}),
                                 ids)});
  ExpectSqlError(both.Resolve(Ref("v")).status(), kBind,
                 "column name 'v' is ambiguous: it matches the columns 'v' and 'V', which differ "
                 "only in case",
                 {});
  const Scope alone(
      {TableBinding("v1", arrow::schema({arrow::field("v", arrow::list(arrow::int32()))}), ids)});
  ExpectSqlError(alone.Resolve(Ref("v")).status(), kUnsupported, UnsupportedListMessage("v"), {});
}

// A column without an engine type is kUnsupported where a query refers to it, by name or as part
// of SELECT *, with its declared name; the binding's other columns stay usable.
TEST(ScopeTest, AnUnsupportedTypeFailsOnlyWhereItIsReferenced) {
  ColumnIdSource ids;
  const Scope scope({TableBinding("t", AllTypesSchema(), ids)});
  EXPECT_TRUE(scope.Resolve(Ref("i16")).ok());
  const SourceSpan ref_span{.offset = 7, .length = 3};
  ExpectSqlError(scope.Resolve(Ref("BAD", ref_span)).status(), kUnsupported,
                 UnsupportedListMessage("bad"), ref_span);
  const SourceSpan star{.offset = 7, .length = 1};
  ExpectSqlError(scope.Reference(ColumnLocation{.binding = 0, .column = 8}, star).status(),
                 kUnsupported, UnsupportedListMessage("bad"), star);
  for (std::size_t c = 0; c < scope.bindings().front().columns().size(); ++c) {
    EXPECT_EQ(scope.Reference(ColumnLocation{.binding = 0, .column = c}, star).ok(), c != 8) << c;
  }
}

TEST(ScopeTest, FindLocatesBindingColumnsAcrossBindings) {
  ColumnIdSource ids;
  const Scope scope(
      {TableBinding("t", AllTypesSchema(), ids), TableBinding("ok", OkSchema(), ids)});
  EXPECT_EQ(scope.Find(ColumnId{1}), (ColumnLocation{.binding = 0, .column = 0}));
  EXPECT_EQ(scope.Find(ColumnId{5}), (ColumnLocation{.binding = 0, .column = 4}));
  EXPECT_EQ(scope.Find(ColumnId{12}), (ColumnLocation{.binding = 1, .column = 0}));
  EXPECT_EQ(scope.Find(ColumnId{13}), (ColumnLocation{.binding = 1, .column = 1}));
  EXPECT_EQ(scope.column(ColumnLocation{.binding = 1, .column = 1}).name, "s");
  EXPECT_EQ(scope.Find(ids.Next()), std::nullopt);  // a computed column's
  EXPECT_EQ(scope.Find(kNoColumnId), std::nullopt);
}

// The FLOAT flag of a column is found by its id: with a FLOAT field first in the first binding and
// second in the second, a lookup by position would answer for other columns.
TEST(ScopeTest, StoredAsFloatIsLookedUpById) {
  ColumnIdSource ids;
  const Scope scope({TableBinding("a",
                                  arrow::schema({arrow::field("f", arrow::float64()),
                                                 arrow::field("g", arrow::float32()),
                                                 arrow::field("k", arrow::int32())}),
                                  ids, {0}),
                     TableBinding("b",
                                  arrow::schema({arrow::field("k2", arrow::int32()),
                                                 arrow::field("f2", arrow::float64())}),
                                  ids, {1})});
  EXPECT_TRUE(scope.StoredAsFloat(ColumnId{1}));
  EXPECT_FALSE(scope.StoredAsFloat(ColumnId{2}));
  EXPECT_FALSE(scope.StoredAsFloat(ColumnId{3}));
  EXPECT_FALSE(scope.StoredAsFloat(ColumnId{4}));
  EXPECT_TRUE(scope.StoredAsFloat(ColumnId{5}));
  EXPECT_FALSE(scope.StoredAsFloat(ids.Next()));  // a computed column's
  EXPECT_FALSE(scope.StoredAsFloat(kNoColumnId));
}

// A scope keeps the enclosing block's scope, and a name resolves in the innermost block that has
// it (rule 9 of ADR 0022). A name found only outside is a correlation (ADR 0023), which this test
// does not pin.
TEST(ScopeTest, TheOuterScopeIsKeptAndTheInnerBlockResolvesFirst) {
  ColumnIdSource ids;
  const Scope outer({TableBinding("o", arrow::schema({arrow::field("k", arrow::int32())}), ids)});
  const Scope inner({TableBinding("i", arrow::schema({arrow::field("K", arrow::int64())}), ids)},
                    &outer);
  EXPECT_EQ(outer.outer(), nullptr);
  EXPECT_EQ(inner.outer(), &outer);
  const auto k = inner.Resolve(Ref("k"));
  ASSERT_TRUE(k.ok()) << k.status().ToString();
  EXPECT_EQ(k->id, ColumnId{2});
  EXPECT_EQ(k->name, "K");
}

// The binder numbers the binding's columns first (the Scan's ids, in schema order), then what it
// computes and the select list.
TEST(BinderIdsTest, TheBindingColumnsAreNumberedFirst) {
  const Catalog catalog = MakeCatalog();
  auto plan = BindSql("SELECT i16 + 1 FROM t WHERE i32 > 1", catalog);
  ASSERT_TRUE(plan.ok()) << plan.status().ToString();
  const auto& project = std::get<ProjectNode>(Nth(*plan, 0));
  const auto& compute = std::get<ComputeNode>(Nth(*plan, 1));
  const auto& filter = std::get<FilterNode>(Nth(*plan, 2));
  const auto& scan = std::get<ScanNode>(Nth(*plan, 3));
  std::vector<ColumnId> fields;
  for (std::uint32_t i = 1; i <= 11; ++i) {
    fields.push_back(ColumnId{i});
  }
  EXPECT_EQ(scan.ids, fields);
  ASSERT_EQ(filter.predicates.size(), 1U);
  EXPECT_EQ(filter.predicates[0].column.value_or(BoundColumn{}).id, ColumnId{2});  // i32
  EXPECT_EQ(compute.ids, (std::vector<ColumnId>{ColumnId{12}}));
  EXPECT_EQ(project.ids, (std::vector<ColumnId>{ColumnId{13}}));
}

// ---- several bindings: the name rules of ADR 0022 (rules 1-5 and 10) ----

// The tables of the rule probes (DuckDB 1.5.6 probes of the J2b design, re-modelled): t(a, b, c),
// u(a BIGINT, d), w(c, g) and v(e, f), INTEGER but for u.a.
std::shared_ptr<arrow::Schema> IntegerSchema(const std::vector<std::string>& names) {
  arrow::FieldVector fields;
  for (const std::string& name : names) {
    fields.push_back(arrow::field(name, arrow::int32()));
  }
  return arrow::schema(std::move(fields));
}
std::shared_ptr<arrow::Schema> T() { return IntegerSchema({"a", "b", "c"}); }
std::shared_ptr<arrow::Schema> U() {
  return arrow::schema({arrow::field("a", arrow::int64()), arrow::field("d", arrow::int32())});
}
std::shared_ptr<arrow::Schema> W() { return IntegerSchema({"c", "g"}); }
std::shared_ptr<arrow::Schema> V() { return IntegerSchema({"e", "f"}); }

// The binding `name` of a table referred to in FROM as `table_name`, a table's name or a path: an
// alias when `name` is not the name the reference gives it (rule 1).
Binding Aliased(const std::string& name, const std::string& table_name,
                std::shared_ptr<arrow::Schema> schema, ColumnIdSource& ids,
                sql::TableRef::Kind kind = sql::TableRef::Kind::kName) {
  auto table = std::make_shared<FakeTable>(std::move(schema), 1);
  return Binding::OfTable(
      name,
      TableSource{.table = std::move(table), .table_name = table_name, .span = {}, .kind = kind},
      ids);
}

sql::ColumnRef QRef(std::string qualifier, std::string name, SourceSpan span = {}) {
  sql::ColumnRef ref = Ref(std::move(name), span);
  ref.qualifier = std::move(qualifier);
  return ref;
}

// `ref` resolves in `scope` to column `column` of binding `binding` (by id), with `qualifier`.
void ExpectResolves(const Scope& scope, const sql::ColumnRef& ref, Visibility visible,
                    std::size_t binding, std::size_t column, std::string_view qualifier) {
  const auto resolved = scope.Resolve(ref, visible);
  ASSERT_TRUE(resolved.ok()) << ref.qualifier << "." << ref.name << ": "
                             << resolved.status().ToString();
  const ColumnLocation where{.binding = binding, .column = column};
  EXPECT_EQ(resolved->id, scope.column(where).id) << ref.qualifier << "." << ref.name;
  EXPECT_EQ(resolved->name, scope.column(where).name);
  EXPECT_EQ(resolved->qualifier, qualifier);
  const Lookup lookup = scope.LookUp(ref, visible);
  EXPECT_EQ(lookup.outcome, Lookup::Outcome::kFound);
  EXPECT_EQ(lookup.where, where);
}

// Rule 1: a path without an alias binds as its file name up to the first dot, leading dots and
// empty parts skipped, and a path with a glob character as its whole text (DuckDB 1.5.6 probes:
// `SELECT lead.x FROM 'S/..lead.dots.parquet'`, `SELECT "S/a*.parquet".x FROM 'S/a*.parquet'`).
TEST(ScopeTest, PathBindingNames) {
  const std::vector<std::pair<std::string, std::string>> cases = {
      {"dir/a.b.parquet", "a"},
      {"..x.parquet", "x"},
      {"S/..lead.dots.parquet", "lead"},
      {"nodot", "nodot"},
      {"dir//a.parquet", "a"},
      {"./a.parquet", "a"},
      {"dir/", "dir"},
      {".", "."},
      {"a/..", ".."},
      {"/", "/"},
      {"", ""},
      {"dir/t*.parquet", "dir/t*.parquet"},
      {"dir/?.parquet", "dir/?.parquet"},
      {"[ab].parquet", "[ab].parquet"},
      {"dir/{a,b}.parquet", "{a,b}"},  // braces are no glob in DuckDB
  };
  for (const auto& [path, name] : cases) {
    EXPECT_EQ(PathBindingName(path), name) << path;
  }
}

// Rules 2 and 3: a qualified name resolves among the bindings of that name, ASCII
// case-insensitively and quoted or not; a binding without the column, or no binding of the name,
// is a bind error at the name.
TEST(ScopeTest, QualifiedNamesResolveInTheBindingsTheyName) {
  ColumnIdSource ids;
  const Scope scope({TableBinding("t", T(), ids), TableBinding("u", U(), ids)});
  ExpectResolves(scope, QRef("t", "a"), {}, 0, 0, "t");
  ExpectResolves(scope, QRef("U", "A"), {}, 1, 0, "u");
  sql::ColumnRef quoted = QRef("T", "C");
  quoted.qualifier_quoted = true;
  quoted.quoted = true;
  ExpectResolves(scope, quoted, {}, 0, 2, "t");
  const SourceSpan span{.offset = 7, .length = 3};
  ExpectSqlError(scope.Resolve(QRef("t", "d", span)).status(), kBind, "'t' has no column 'd'",
                 span);
  ExpectSqlError(scope.Resolve(QRef("x", "a", span)).status(), kBind, "no FROM item is named 'x'",
                 span);
  EXPECT_EQ(scope.LookUp(QRef("x", "a")).outcome, Lookup::Outcome::kMissing);
}

// Rule 2: an alias hides the name its FROM reference gives a binding without one, a table's name as
// written or a path's PathBindingName. A path without a glob character is named by its file name up
// to the first dot, so its whole text names nothing, aliased or not; a glob path's name is its
// whole text, which an alias then hides. A table's name is never taken for a path: FROM "a.b" AS x
// hides "a.b", not a.
TEST(ScopeTest, AnAliasHidesItsTablesName) {
  ColumnIdSource ids;
  const Scope aliased({Aliased("x", "t", T(), ids), TableBinding("u", U(), ids)});
  ExpectResolves(aliased, QRef("x", "b"), {}, 0, 1, "x");
  ExpectSqlError(aliased.Resolve(QRef("t", "b")).status(), kBind,
                 "no FROM item is named 't' (the alias 'x' hides it)", {});
  constexpr sql::TableRef::Kind kPath = sql::TableRef::Kind::kPath;
  const Scope paths({Aliased("x", "F/trips.parquet", T(), ids, kPath),
                     Aliased("zones", "F/zones.parquet", U(), ids, kPath)});
  ExpectResolves(paths, QRef("zones", "d"), {}, 1, 1, "zones");
  ExpectSqlError(paths.Resolve(QRef("trips", "a")).status(), kBind,
                 "no FROM item is named 'trips' (the alias 'x' hides it)", {});
  ExpectSqlError(paths.Resolve(QRef("F/trips.parquet", "a")).status(), kBind,
                 "no FROM item is named 'F/trips.parquet'", {});
  ExpectSqlError(paths.Resolve(QRef("F/zones.parquet", "d")).status(), kBind,
                 "no FROM item is named 'F/zones.parquet'", {});
  // A glob path is named by its whole text: unaliased it resolves under it, and an alias hides it.
  const Scope globs({Aliased("x", "F/t*.parquet", T(), ids, kPath),
                     Aliased("F/z?.parquet", "F/z?.parquet", U(), ids, kPath)});
  ExpectResolves(globs, QRef("F/z?.parquet", "d"), {}, 1, 1, "F/z?.parquet");
  ExpectSqlError(globs.Resolve(QRef("F/t*.parquet", "a")).status(), kBind,
                 "no FROM item is named 'F/t*.parquet' (the alias 'x' hides it)", {});
  const Scope dotted({Aliased("x", "a.b", T(), ids), TableBinding("u", U(), ids)});
  ExpectSqlError(dotted.Resolve(QRef("a", "b")).status(), kBind, "no FROM item is named 'a'", {});
  ExpectSqlError(dotted.Resolve(QRef("A.B", "b")).status(), kBind,
                 "no FROM item is named 'A.B' (the alias 'x' hides it)", {});
}

// Rule 3: two bindings may share a name; a qualified name resolves to the one that has the column,
// and is ambiguous when both have it (DuckDB: `FROM t AS x, u AS X WHERE x.b = X.d` answers).
TEST(ScopeTest, BindingsMayShareAName) {
  ColumnIdSource ids;
  const Scope scope({Aliased("x", "t", T(), ids), Aliased("X", "u", U(), ids)});
  ExpectResolves(scope, QRef("x", "b"), {}, 0, 1, "x");
  ExpectResolves(scope, QRef("X", "d"), {}, 1, 1, "X");
  const SourceSpan span{.offset = 2, .length = 3};
  ExpectSqlError(scope.Resolve(QRef("x", "a", span)).status(), kBind,
                 "'x.a' is ambiguous: two FROM items named 'x' have a column 'a'", span);
  const Lookup lookup = scope.LookUp(QRef("X", "A"));
  EXPECT_EQ(lookup.outcome, Lookup::Outcome::kAmbiguous);
  EXPECT_EQ(lookup.where, (ColumnLocation{.binding = 0, .column = 0}));
  EXPECT_EQ(lookup.other, (ColumnLocation{.binding = 1, .column = 0}));
}

// Rule 4: an unqualified name that two bindings have is ambiguous, and the error names the
// qualified candidates (DuckDB: `SELECT a FROM t, u WHERE t.a = u.a`); two columns of one binding
// whose names differ only in case keep the single-table message.
TEST(ScopeTest, AnUnqualifiedNameInTwoBindingsIsAmbiguous) {
  ColumnIdSource ids;
  const Scope scope({TableBinding("t", T(), ids), TableBinding("u", U(), ids)});
  const SourceSpan span{.offset = 7, .length = 1};
  ExpectSqlError(scope.Resolve(Ref("A", span)).status(), kBind,
                 "column name 'A' is ambiguous: it matches t.a and u.a", span);
  ExpectResolves(scope, Ref("b"), {}, 0, 1, "t");
  ExpectResolves(scope, Ref("D"), {}, 1, 1, "u");
  ExpectSqlError(scope.Resolve(Ref("e", span)).status(), kBind, "column 'e' does not exist", span);
  const Scope cased({TableBinding("m",
                                  arrow::schema({arrow::field("Ab", arrow::int32()),
                                                 arrow::field("aB", arrow::int32())}),
                                  ids),
                     TableBinding("n", arrow::schema({arrow::field("ab", arrow::int32())}), ids)});
  ExpectSqlError(cased.Resolve(Ref("AB")).status(), kBind,
                 "column name 'AB' is ambiguous: it matches the columns 'Ab' and 'aB', which "
                 "differ only in case",
                 {});
  ExpectResolves(cased, QRef("n", "AB"), {}, 1, 0, "n");
}

// Rule 10 in two levels, as in DuckDB 1.5.6: an ON resolves a name in its own join group first
// (the items from the last comma up to its JOIN; CROSS JOIN continues a group), and only then in
// the earlier comma siblings. WHERE sees every binding in one level.
TEST(ScopeTest, AnOnResolvesInItsJoinGroupFirst) {
  ColumnIdSource ids;
  // FROM t, u JOIN w ON c = u.d: w.c, not ambiguous with t.c (DuckDB answers 4, 10).
  const Scope comma(
      {TableBinding("t", T(), ids), TableBinding("u", U(), ids), TableBinding("w", W(), ids)});
  const Visibility on_w{.inner_begin = 1, .end = 3};
  ExpectResolves(comma, Ref("c"), on_w, 2, 0, "w");
  ExpectResolves(comma, QRef("u", "d"), on_w, 1, 1, "u");
  // FROM t, u JOIN w ON b = w.g: b only in the earlier comma sibling t.
  ExpectResolves(comma, Ref("b"), on_w, 0, 1, "t");
  // ... WHERE c = 3: one level, so t.c and w.c are ambiguous.
  ExpectSqlError(comma.Resolve(Ref("c")).status(), kBind,
                 "column name 'c' is ambiguous: it matches t.c and w.c", {});
  // FROM t CROSS JOIN u JOIN w ON c = u.d: one group, ambiguous.
  ExpectSqlError(comma.Resolve(Ref("c"), Visibility{.inner_begin = 0, .end = 3}).status(), kBind,
                 "column name 'c' is ambiguous: it matches t.c and w.c", {});
  // FROM t, w, u JOIN v ON c = v.e: c only in the outer level, where t.c and w.c are ambiguous.
  const Scope outer({TableBinding("t", T(), ids), TableBinding("w", W(), ids),
                     TableBinding("u", U(), ids), TableBinding("v", V(), ids)});
  ExpectSqlError(outer.Resolve(Ref("c"), Visibility{.inner_begin = 2, .end = 4}).status(), kBind,
                 "column name 'c' is ambiguous: it matches t.c and w.c", {});
  // FROM t AS x, u JOIN w AS x ON x.c = u.d: the inner x (w); ON x.b = u.d: the outer x (t).
  const Scope shadow(
      {Aliased("x", "t", T(), ids), TableBinding("u", U(), ids), Aliased("x", "w", W(), ids)});
  ExpectResolves(shadow, QRef("x", "c"), on_w, 2, 0, "x");
  ExpectResolves(shadow, QRef("x", "b"), on_w, 0, 1, "x");
  // In WHERE both x have c.
  ExpectSqlError(shadow.Resolve(QRef("x", "c")).status(), kBind,
                 "'x.c' is ambiguous: two FROM items named 'x' have a column 'c'", {});
}

// Rule 10: an ON never sees a later FROM item. A name only a later item has is a bind error, whose
// message says so, and a later item's column never makes a name ambiguous.
TEST(ScopeTest, AnOnDoesNotSeeLaterItems) {
  ColumnIdSource ids;
  // FROM t JOIN u ON t.a = u.a AND d = e JOIN v ON e = d: DuckDB's "Referenced column "e" not
  // found" in the first ON; the second ON sees v.
  const Scope chain(
      {TableBinding("t", T(), ids), TableBinding("u", U(), ids), TableBinding("v", V(), ids)});
  const Visibility first_on{.inner_begin = 0, .end = 2};
  const SourceSpan span{.offset = 9, .length = 1};
  ExpectSqlError(chain.Resolve(Ref("e", span), first_on).status(), kBind,
                 "column 'e' does not exist (an ON sees only the FROM items up to its JOIN)", span);
  ExpectResolves(chain, Ref("e"), Visibility{.inner_begin = 0, .end = 3}, 2, 0, "v");
  ExpectSqlError(chain.Resolve(QRef("v", "e"), first_on).status(), kBind,
                 "no FROM item is named 'v' (an ON sees only the FROM items up to its JOIN)", {});
  ExpectSqlError(chain.Resolve(Ref("zz"), first_on).status(), kBind, "column 'zz' does not exist",
                 {});
  // FROM t JOIN w ON c = w.g JOIN u ...: u.a is later, so a is t.a alone.
  const Scope later(
      {TableBinding("t", T(), ids), TableBinding("w", W(), ids), TableBinding("u", U(), ids)});
  ExpectResolves(later, Ref("a"), first_on, 0, 0, "t");
  // FROM t AS q JOIN u ON q.d = u.d JOIN u AS q ...: the later q has d, the visible one does not.
  const Scope same_name(
      {Aliased("q", "t", T(), ids), TableBinding("u", U(), ids), Aliased("q", "u", U(), ids)});
  ExpectSqlError(same_name.Resolve(QRef("q", "d"), first_on).status(), kBind,
                 "'q' has no column 'd' (an ON sees only the FROM items up to its JOIN)", {});
  ExpectSqlError(same_name.Resolve(QRef("q", "zz"), first_on).status(), kBind,
                 "'q' has no column 'zz'", {});
  // An end beyond the bindings is every binding.
  ExpectResolves(chain, Ref("f"), Visibility{.inner_begin = 1, .end = 9}, 2, 1, "v");
  // FROM t JOIN u ON w.a = u.a JOIN w AS v ...: the later item's alias hides the name the ON uses,
  // so the hint it needs is that it cannot see that item, not that an alias hides the name.
  const Scope hidden_later(
      {TableBinding("t", T(), ids), TableBinding("u", U(), ids), Aliased("v", "w", W(), ids)});
  ExpectSqlError(hidden_later.Resolve(QRef("w", "a"), first_on).status(), kBind,
                 "no FROM item is named 'w' (an ON sees only the FROM items up to its JOIN)", {});
  // FROM t AS w JOIN u ON w2.a = u.a JOIN w2 ...: both facts hold, so both hints come, the later
  // item first. (A visible alias hides 'w2' and a later item is named by it.)
  const Scope both({Aliased("alias", "w2", T(), ids), TableBinding("u", U(), ids),
                    TableBinding("w2", W(), ids)});
  ExpectSqlError(both.Resolve(QRef("w2", "a"), first_on).status(), kBind,
                 "no FROM item is named 'w2' (an ON sees only the FROM items up to its JOIN)"
                 " (the alias 'alias' hides it)",
                 {});
}

// Rule 10's visibility of an ON, from the FROM list alone: the items up to and including its own,
// its join group the inner level. A comma starts a group and so does the first item; JOIN and
// CROSS JOIN continue the group they join into.
TEST(ScopeTest, OnVisibilityFollowsTheJoinGroups) {
  using sql::Connector;
  const auto from = [](const std::vector<Connector>& connectors) {
    std::vector<sql::FromItem> items;
    items.reserve(connectors.size());
    for (const Connector connector : connectors) {
      items.push_back(sql::FromItem{.connector = connector});
    }
    return items;
  };
  const auto visibility = [](const std::vector<sql::FromItem>& items, std::size_t item) {
    const Visibility v = OnVisibility(items, item);
    return std::pair{v.inner_begin, v.end};
  };
  // FROM t, u JOIN w ON ...: w's group is u and w.
  const std::vector<sql::FromItem> comma =
      from({Connector::kFirst, Connector::kComma, Connector::kInner});
  EXPECT_EQ(visibility(comma, 2), (std::pair{std::size_t{1}, std::size_t{3}}));
  // FROM t CROSS JOIN u JOIN w ON ...: one group, from the first item.
  const std::vector<sql::FromItem> cross =
      from({Connector::kFirst, Connector::kCross, Connector::kInner});
  EXPECT_EQ(visibility(cross, 2), (std::pair{std::size_t{0}, std::size_t{3}}));
  // FROM t, w, u JOIN v ON ...: v's group is u and v.
  const std::vector<sql::FromItem> outer =
      from({Connector::kFirst, Connector::kComma, Connector::kComma, Connector::kInner});
  EXPECT_EQ(visibility(outer, 3), (std::pair{std::size_t{2}, std::size_t{4}}));
  // FROM s JOIN d ON ..., t: the ON of item 1 never sees t.
  const std::vector<sql::FromItem> trailing =
      from({Connector::kFirst, Connector::kInner, Connector::kComma});
  EXPECT_EQ(visibility(trailing, 1), (std::pair{std::size_t{0}, std::size_t{2}}));
  // A comma item of its own is its whole group.
  EXPECT_EQ(visibility(comma, 1), (std::pair{std::size_t{1}, std::size_t{2}}));
  EXPECT_EQ(visibility(comma, 0), (std::pair{std::size_t{0}, std::size_t{1}}));
}

// A column's qualifier is its binding's name (as declared) only in a scope of two or more
// bindings, so that EXPLAIN of a single table does not change.
TEST(ScopeTest, QualifiersOnlyWithSeveralBindings) {
  ColumnIdSource ids;
  const Scope one({TableBinding("t", T(), ids)});
  EXPECT_EQ(one.Qualifier(0), "");
  ExpectResolves(one, QRef("T", "a"), {}, 0, 0, "");
  const Scope two({Aliased("Big", "t", T(), ids), TableBinding("u", U(), ids)});
  EXPECT_EQ(two.Qualifier(0), "Big");
  EXPECT_EQ(two.Qualifier(1), "u");
  const auto star = two.Reference(ColumnLocation{.binding = 1, .column = 1}, {});
  ASSERT_TRUE(star.ok()) << star.status().ToString();
  EXPECT_EQ(star->qualifier, "u");
  EXPECT_EQ(star->name, "d");
}

// Rule 5: SELECT * is a bind error only when two bindings of the same name (ASCII
// case-insensitively) share a column name (DuckDB: `FROM t AS x, u AS x` errors, `FROM t AS x,
// v AS x` answers); the first such pair in FROM order is reported, with its first shared column.
TEST(ScopeTest, StarConflicts) {
  ColumnIdSource ids;
  const auto conflict = [](const Scope& scope) {
    return scope.FindStarConflict().value_or(Scope::StarConflict{.first = 9, .second = 9});
  };
  const Scope same({Aliased("x", "t", T(), ids), Aliased("X", "u", U(), ids)});
  EXPECT_EQ(conflict(same).first, 0U);
  EXPECT_EQ(conflict(same).second, 1U);
  EXPECT_EQ(conflict(same).column, "a");
  const Scope twice({TableBinding("t", T(), ids), TableBinding("t", T(), ids)});  // FROM t, t
  EXPECT_EQ(conflict(twice).column, "a");
  const Scope disjoint({Aliased("x", "t", T(), ids), Aliased("x", "v", V(), ids)});
  EXPECT_EQ(disjoint.FindStarConflict(), std::nullopt);
  const Scope named_apart({TableBinding("t", T(), ids), TableBinding("u", U(), ids)});
  EXPECT_EQ(named_apart.FindStarConflict(), std::nullopt);
  const Scope later(
      {TableBinding("t", T(), ids), Aliased("q", "w", W(), ids), Aliased("q", "t", T(), ids)});
  EXPECT_EQ(conflict(later).first, 1U);
  EXPECT_EQ(conflict(later).second, 2U);
  EXPECT_EQ(conflict(later).column, "c");
}

// Ambiguity across bindings comes before an unsupported type, which a qualified name reaches like
// an unqualified one.
TEST(ScopeTest, UnsupportedTypesAcrossBindings) {
  ColumnIdSource ids;
  const Scope scope({TableBinding("t", AllTypesSchema(), ids),
                     TableBinding("l", arrow::schema({arrow::field("bad", arrow::int32())}), ids)});
  ExpectSqlError(scope.Resolve(Ref("bad")).status(), kBind,
                 "column name 'bad' is ambiguous: it matches t.bad and l.bad", {});
  const SourceSpan span{.offset = 3, .length = 5};
  ExpectSqlError(scope.Resolve(QRef("t", "bad", span)).status(), kUnsupported,
                 UnsupportedListMessage("bad"), span);
  ExpectResolves(scope, QRef("l", "bad"), {}, 1, 0, "l");
}

TEST(ScopeDeathTest, ALocationOutOfRangeAborts) {
  ColumnIdSource ids;
  const Scope scope({TableBinding("ok", OkSchema(), ids)});
  EXPECT_DEATH((void)scope.column(ColumnLocation{.binding = 1, .column = 0}), "where.binding");
  EXPECT_DEATH((void)scope.column(ColumnLocation{.binding = 0, .column = 2}), "where.column");
}

// A column id belongs to one column of a scope (checked in debug builds).
TEST(ScopeDeathTest, ColumnIdsAreUniqueInAScope) {
  ColumnIdSource ids;
  const Binding ok = TableBinding("ok", OkSchema(), ids);
  EXPECT_DEBUG_DEATH({ const Scope twice({ok, ok}); }, "IdsAreUnique");
}

// A table's binding needs the table.
TEST(BindingDeathTest, OfTableNeedsATable) {
  ColumnIdSource ids;
  EXPECT_DEATH((void)Binding::OfTable(
                   "x", TableSource{.table = nullptr, .table_name = "x", .span = {}}, ids),
               "source.table != nullptr");
}

// A sub-plan's binding needs a root that outputs exactly its columns' ids, in order, and columns
// with ids and types.
TEST(BindingDeathTest, OfPlanChecksItsRootAndColumns) {
  const auto table =
      std::make_shared<FakeTable>(arrow::schema({arrow::field("a", arrow::int32())}), 1);
  const LogicalNodePtr root = std::make_shared<const LogicalNode>(
      ScanNode{.table = table, .table_name = "x", .fields = {0}, .ids = {ColumnId{7}}});
  const BindingColumn a{.name = "a", .id = ColumnId{7}, .type = LogicalType::kInteger};
  EXPECT_DEATH((void)Binding::OfPlan("x", nullptr, {a}), "root != nullptr");
  BindingColumn untyped = a;
  untyped.type.reset();
  EXPECT_DEATH((void)Binding::OfPlan("x", root, {untyped}), "type.has_value");
  BindingColumn without_id = a;
  without_id.id = kNoColumnId;
  EXPECT_DEATH((void)Binding::OfPlan("x", root, {without_id}), "kNoColumnId");
  BindingColumn other = a;
  other.id = ColumnId{8};
  EXPECT_DEATH((void)Binding::OfPlan("x", root, {other}), "OutputIds");
  EXPECT_DEATH((void)Binding::OfPlan("x", root, {}), "OutputIds");
}

}  // namespace
}  // namespace antb1::plan
