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
// declared name; ResolvePositions sets the position later, and nothing sets a qualifier yet.
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

// CheckSupported rejects every qualified name and every FROM list of several items before a scope
// resolves a name; roadmap PR J2b lifts both checks.
TEST(ScopeDeathTest, QualifiedNamesWaitForJ2b) {
  ColumnIdSource ids;
  const Scope scope({TableBinding("t", AllTypesSchema(), ids)});
  sql::ColumnRef ref = Ref("i16");
  ref.qualifier = "t";
  EXPECT_DEATH((void)scope.Resolve(ref), "qualifier.empty");
}

TEST(ScopeDeathTest, NamesResolveInOneBindingUntilJ2b) {
  ColumnIdSource ids;
  const Scope scope(
      {TableBinding("t", AllTypesSchema(), ids), TableBinding("ok", OkSchema(), ids)});
  EXPECT_DEATH((void)scope.Resolve(Ref("dt")), "bindings_.size");
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
