#include "antb1/plan/binder.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <limits>
#include <memory>
#include <optional>
#include <ostream>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <arrow/api.h>
#include <gtest/gtest.h>

#include "antb1/common/int128.h"
#include "antb1/common/narrow.h"
#include "antb1/plan/explain.h"
#include "antb1/plan/logical_plan.h"
#include "antb1/plan/optimizer.h"
#include "antb1/plan/sql_status.h"
#include "antb1/sql/parser.h"

#include "plan_test_util.h"

namespace antb1::plan {
namespace {

using testing::BindSql;
using testing::FakeTable;
using testing::MakeCatalog;
using testing::Nth;
using testing::SpanText;

struct ErrorCase {
  std::string_view sql;
  SqlErrorDetail::Kind kind = SqlErrorDetail::Kind::kBind;
  std::string_view span;     // the query text the error points at
  std::string_view message;  // a part of the message
};

void PrintTo(const ErrorCase& c, std::ostream* os) { *os << c.sql; }

class BindErrorTest : public ::testing::TestWithParam<ErrorCase> {};

TEST_P(BindErrorTest, ReportsKindSpanAndMessage) {
  const ErrorCase& c = GetParam();
  const Catalog catalog = MakeCatalog();
  auto plan = BindSql(c.sql, catalog);
  ASSERT_FALSE(plan.ok()) << c.sql;
  const auto detail = GetSqlError(plan.status());
  ASSERT_NE(detail, nullptr) << plan.status().ToString();
  EXPECT_EQ(detail->kind(), c.kind) << plan.status().ToString();
  EXPECT_EQ(SpanText(c.sql, plan.status()), c.span) << plan.status().ToString();
  EXPECT_NE(plan.status().message().find(c.message), std::string::npos) << plan.status().message();
  // kUnsupported is NotImplemented (exit code 4); kBind is Invalid (exit code 1).
  EXPECT_EQ(plan.status().IsNotImplemented(), c.kind == SqlErrorDetail::Kind::kUnsupported);
}

constexpr auto kBind = SqlErrorDetail::Kind::kBind;
constexpr auto kUnsupported = SqlErrorDetail::Kind::kUnsupported;

INSTANTIATE_TEST_SUITE_P(
    Binder, BindErrorTest,
    ::testing::Values(
        // [NOT] IN: each value is typed like `column = value`.
        ErrorCase{"SELECT COUNT(*) FROM t WHERE i16 IN (1, '2')", kBind, "'2'",
                  "cannot compare SMALLINT column 'i16' with a string"},
        ErrorCase{"SELECT COUNT(*) FROM t WHERE s NOT IN ('a', 1)", kBind, "1",
                  "cannot compare VARCHAR column 's' with a number"},
        ErrorCase{"SELECT COUNT(*) FROM t WHERE dt IN (DATE '2013-02-30')", kBind,
                  "DATE '2013-02-30'", "invalid date"},
        // Constants and positions.
        ErrorCase{"SELECT 1e3 FROM t", kUnsupported, "1e3",
                  "a number with an exponent or more than 38 digits as a constant"},
        ErrorCase{"SELECT 100000000000000000000000000000000000000 FROM t", kUnsupported,
                  "100000000000000000000000000000000000000", "outside HUGEINT's range"},
        ErrorCase{"SELECT DATE '2020-02-30' FROM t", kBind, "DATE '2020-02-30'", "invalid date"},
        ErrorCase{"SELECT i16 FROM t ORDER BY 2", kBind, "2",
                  "ORDER BY position 2 is not between 1 and 1"},
        ErrorCase{"SELECT i16 FROM t ORDER BY -1", kBind, "-1", "ORDER BY position -1"},
        // A minus over a number is part of the literal, as DuckDB's parser folds it: each is the
        // error of the literal spelling, and one at the number spans from the minus to the number,
        // as a negation's did.
        ErrorCase{"SELECT i16 FROM t ORDER BY -(1)", kBind, "-(1",
                  "ORDER BY position -1 is not between 1 and 1"},
        ErrorCase{"SELECT i16, COUNT(*) FROM t GROUP BY -(-2)", kBind, "-(-2",
                  "GROUP BY cannot refer to the aggregate at position 2"},
        ErrorCase{"SELECT -(1e3) FROM t", kUnsupported, "-(1e3",
                  "a number with an exponent or more than 38 digits as a constant"},
        ErrorCase{"SELECT CAST(-(1e3) AS INTEGER) FROM t", kUnsupported, "CAST(-(1e3) AS INTEGER)",
                  "CAST of the number -1e3 to INTEGER is not supported"},
        ErrorCase{"SELECT CAST(- -1e3 AS INTEGER) FROM t", kUnsupported, "CAST(- -1e3 AS INTEGER)",
                  "CAST of the number 1e3 to INTEGER is not supported"},
        ErrorCase{"SELECT SUM(-(1)) FROM t", kUnsupported, "-(1",
                  "constant aggregate arguments are not supported"},
        ErrorCase{"SELECT -(1) AS k, COUNT(*) FROM t GROUP BY i16 HAVING k < 0", kUnsupported, "k",
                  "HAVING on a constant select item is not supported"},
        ErrorCase{"SELECT i16 FROM t ORDER BY 'x'", kBind, "'x'",
                  "ORDER BY a non-integer literal orders nothing"},
        ErrorCase{"SELECT i16, COUNT(*) FROM t GROUP BY 0", kBind, "0",
                  "GROUP BY position 0 is not between 1 and 2"},
        ErrorCase{"SELECT i16, COUNT(*) FROM t GROUP BY 2", kBind, "2",
                  "GROUP BY cannot refer to the aggregate at position 2"},
        ErrorCase{"SELECT COUNT(*) AS n FROM t GROUP BY n", kBind, "n",
                  "GROUP BY cannot refer to the aggregate 'n'"},
        ErrorCase{"SELECT 1, i16 FROM t GROUP BY 1", kBind, "i16",
                  "column 'i16' must appear in the GROUP BY clause"},
        // [NOT] LIKE: a VARCHAR column and a string pattern (DuckDB rejects LIKE on numbers).
        ErrorCase{"SELECT COUNT(*) FROM t WHERE i16 LIKE '1%'", kBind, "i16",
                  "LIKE needs a VARCHAR column, but 'i16' is SMALLINT"},
        ErrorCase{"SELECT COUNT(*) FROM t WHERE dt NOT LIKE '2013%'", kBind, "dt",
                  "NOT LIKE needs a VARCHAR column"},
        ErrorCase{"SELECT COUNT(*) FROM t WHERE s LIKE 5", kBind, "5",
                  "the pattern of LIKE must be a string literal"},
        ErrorCase{"SELECT COUNT(*) FROM t WHERE nope LIKE 'x'", kBind, "nope", "does not exist"},
        // COUNT(DISTINCT col): the column must exist and have an engine type; it is an aggregate.
        ErrorCase{"SELECT COUNT(DISTINCT nope) FROM t", kBind, "nope",
                  "column 'nope' does not exist"},
        ErrorCase{"SELECT COUNT(DISTINCT bad) FROM t", kUnsupported, "bad", "unsupported type"},
        ErrorCase{"SELECT i16, COUNT(DISTINCT i32) FROM t", kBind, "i16",
                  "column 'i16' must be inside an aggregate function"},
        ErrorCase{"SELECT i16 FROM t ORDER BY COUNT(DISTINCT i32)", kBind, "i16",
                  "must be inside an aggregate function"},
        // ORDER BY: a select alias (the last one), else a table column; an aggregate makes the
        // query aggregate; a grouped query orders by keys and aggregates only.
        ErrorCase{"SELECT i16 FROM t ORDER BY nope", kBind, "nope", "column 'nope' does not exist"},
        ErrorCase{"SELECT COUNT(*) FROM t ORDER BY SUM(s)", kBind, "SUM(s)", "SUM needs a numeric"},
        ErrorCase{"SELECT i16 FROM t ORDER BY COUNT(*)", kBind, "i16",
                  "column 'i16' must be inside an aggregate function"},
        ErrorCase{"SELECT * FROM ok ORDER BY MAX(i16)", kBind, "*",
                  "column 'i16' must be inside an aggregate function"},
        ErrorCase{"SELECT COUNT(*) FROM t ORDER BY i16", kBind, "i16",
                  "column 'i16' must appear in the GROUP BY clause or be inside an aggregate"},
        ErrorCase{"SELECT i16 AS x, COUNT(*) FROM t GROUP BY i16 ORDER BY i32 DESC", kBind, "i32",
                  "column 'i32' must appear in the GROUP BY clause"},
        ErrorCase{"SELECT s AS i32, i16 AS i32 FROM t GROUP BY s, i16 ORDER BY I32, i64", kBind,
                  "i64", "column 'i64' must appear in the GROUP BY clause"},
        ErrorCase{"SELECT i16 FROM t GROUP BY i16 ORDER BY COUNT(bad)", kUnsupported, "bad",
                  "unsupported type"},
        // GROUP BY: keys are table columns or aliases of plain select columns; every plain select
        // column must be a key.
        ErrorCase{"SELECT i16, i32, COUNT(*) FROM t GROUP BY i16", kBind, "i32",
                  "column 'i32' must appear in the GROUP BY clause"},
        ErrorCase{"SELECT * FROM ok GROUP BY i16", kBind, "*",
                  "column 's' must appear in the GROUP BY clause"},
        ErrorCase{"SELECT COUNT(*) FROM t GROUP BY nope", kBind, "nope",
                  "column 'nope' does not exist"},
        ErrorCase{"SELECT SUM(i16) AS total FROM t GROUP BY total", kBind, "total",
                  "GROUP BY cannot refer to the aggregate 'total'"},
        // A repeated alias means its last select item (DuckDB).
        ErrorCase{"SELECT s AS k, COUNT(*) AS k FROM t GROUP BY k", kBind, "k",
                  "GROUP BY cannot refer to the aggregate 'k'"},
        ErrorCase{"SELECT nope FROM missing GROUP BY nope", kBind, "missing",
                  "table 'missing' does not exist"},
        ErrorCase{"SELECT i16 FROM t WHERE nope = 1 GROUP BY nope", kBind, "nope",
                  "column 'nope' does not exist"},
        // OR factoring (ADR 0022's J3) drops an OR its shared conjuncts imply, but binds it first,
        // so an error in the branch it drops is still that error and not an answer.
        ErrorCase{"SELECT COUNT(*) FROM t WHERE i16 = 1 OR i16 = 1 AND nope > 1", kBind, "nope",
                  "column 'nope' does not exist"},
        ErrorCase{"SELECT COUNT(*) FROM t WHERE i16 = 1 OR i16 = 1 AND strlen(s, 1) > 1", kBind,
                  "strlen(s, 1)", "strlen() takes 1 argument, not 2"},
        ErrorCase{"SELECT COUNT(*) FROM t WHERE i16 = 1 OR i16 = 1 AND s > 1", kBind, "1",
                  "cannot compare VARCHAR column 's' with a number"},
        // The same for an inner ON, which the factoring treats exactly as a WHERE element.
        ErrorCase{"SELECT COUNT(*) FROM t JOIN ok ON t.i16 = ok.i16 OR t.i16 = ok.i16 AND "
                  "ok.nope > 1",
                  kBind, "ok.nope", "'ok' has no column 'nope'"},
        // A dropped OR never hides an error written before it: the element reports its first.
        ErrorCase{"SELECT COUNT(*) FROM t WHERE (nope = 1 AND (i16 = 1 OR i16 = 1 AND i32 = 2))",
                  kBind, "nope", "column 'nope' does not exist"},
        // HAVING: aggregates, GROUP BY keys and select aliases of keys or aggregates; HAVING makes
        // the query aggregate.
        ErrorCase{"SELECT i16 FROM t GROUP BY i16 HAVING i32 > 1", kBind, "i32",
                  "column 'i32' must appear in the GROUP BY clause"},
        ErrorCase{"SELECT i16 FROM t HAVING i16 > 1", kBind, "i16",
                  "column 'i16' must be inside an aggregate function"},
        ErrorCase{"SELECT * FROM ok HAVING COUNT(*) > 1", kBind, "*",
                  "must be inside an aggregate function"},
        ErrorCase{"SELECT COUNT(*) FROM t HAVING i16 > 1", kBind, "i16",
                  "column 'i16' must appear in the GROUP BY clause"},
        ErrorCase{"SELECT COUNT(*) FROM t GROUP BY i16 HAVING nope > 1", kBind, "nope",
                  "column 'nope' does not exist"},
        ErrorCase{"SELECT 5 AS k, COUNT(*) FROM t GROUP BY i16 HAVING k > 1", kUnsupported, "k",
                  "HAVING on a constant select item is not supported"},
        ErrorCase{"SELECT COUNT(*) FROM t GROUP BY i16 HAVING COUNT(*) > 'x'", kBind, "'x'",
                  "cannot compare BIGINT column 'count_star()' with a string"},
        ErrorCase{"SELECT COUNT(*) AS n FROM t GROUP BY i16 HAVING n = DATE '2020-01-01'", kBind,
                  "DATE '2020-01-01'", "cannot compare BIGINT column 'n'"},
        ErrorCase{"SELECT COUNT(*) FROM t GROUP BY i16 HAVING SUM(s) > 1", kBind, "SUM(s)",
                  "SUM needs a numeric column"},
        ErrorCase{"SELECT COUNT(*) FROM t GROUP BY i16 HAVING COUNT(*) LIKE 'x'", kBind, "COUNT(*)",
                  "LIKE needs a VARCHAR column, but 'count_star()' is BIGINT"},
        ErrorCase{"SELECT COUNT(*) FROM t GROUP BY i16 HAVING MAX(s) NOT LIKE 1", kBind, "1",
                  "the pattern of NOT LIKE must be a string literal"},
        ErrorCase{"SELECT COUNT(*) FROM t GROUP BY i16 HAVING MAX(dt) IN (1)", kBind, "1",
                  "cannot compare DATE column 'max(dt)' with a number"},
        ErrorCase{"SELECT COUNT(*) FROM t GROUP BY i16 HAVING COUNT(bad) > 1", kUnsupported, "bad",
                  "unsupported type"},
        // Tables.
        ErrorCase{"SELECT COUNT(*) FROM nope", kBind, "nope", "table 'nope' does not exist"},
        ErrorCase{"SELECT COUNT(*) FROM \"nope\"", kBind, "\"nope\"", "does not exist"},
        ErrorCase{"SELECT COUNT(*) FROM 'x.parquet'", kUnsupported, "'x.parquet'", "not enabled"},
        // Columns: unknown, ambiguous, of an unsupported type; in every clause.
        ErrorCase{"SELECT nope FROM t", kBind, "nope", "column 'nope' does not exist"},
        ErrorCase{"SELECT i16, \"No pe\" FROM t", kBind, "\"No pe\"",
                  "column 'No pe' does not exist"},
        ErrorCase{"SELECT SUM(nope) FROM t", kBind, "nope", "does not exist"},
        ErrorCase{"SELECT COUNT(*) FROM t WHERE nope = 1", kBind, "nope", "does not exist"},
        ErrorCase{"SELECT a FROM dup", kBind, "a", "ambiguous"},
        ErrorCase{"SELECT COUNT(\"A\") FROM dup", kBind, "\"A\"", "'a' and 'A'"},
        ErrorCase{"SELECT COUNT(*) FROM dup WHERE A > 1", kBind, "A", "differ only in case"},
        ErrorCase{"SELECT bad FROM t", kUnsupported, "bad",
                  "column 'bad' has the unsupported type"},
        ErrorCase{"SELECT COUNT(bad) FROM t", kUnsupported, "bad", "unsupported type"},
        ErrorCase{"SELECT COUNT(*) FROM t WHERE bad = 1", kUnsupported, "bad", "unsupported"},
        ErrorCase{"SELECT * FROM t", kUnsupported, "*", "column 'bad' has the unsupported type"},
        ErrorCase{"SELECT  *  FROM u LIMIT 1", kUnsupported, "*", "'bad'"},
        // Aggregates mixed with plain columns (either order); SUM and AVG of non-numbers.
        ErrorCase{"SELECT i16, COUNT(*) FROM t", kBind, "i16",
                  "column 'i16' must be inside an aggregate function"},
        ErrorCase{"SELECT COUNT(*), I16 AS x, i32 FROM t", kBind, "I16", "no GROUP BY"},
        ErrorCase{"SELECT SUM(s) FROM t", kBind, "SUM(s)",
                  "SUM needs a numeric column, but 's' is VARCHAR"},
        ErrorCase{"SELECT sum ( DT ) FROM t", kBind, "sum ( DT )", "'dt' is DATE"},
        ErrorCase{"SELECT COUNT(*), AVG(s) AS a FROM t", kBind, "AVG(s)", "AVG needs a numeric"},
        ErrorCase{"SELECT SUM(toDateTime(i64)) FROM t", kBind, "SUM(toDateTime(i64))",
                  "SUM needs a numeric column"},
        // Literals of the wrong type: the span is the literal.
        ErrorCase{"SELECT COUNT(*) FROM t WHERE i16 = '1'", kBind, "'1'",
                  "cannot compare SMALLINT column 'i16' with a string; write a number"},
        ErrorCase{"SELECT COUNT(*) FROM t WHERE i64 > DATE '2013-07-01'", kBind,
                  "DATE '2013-07-01'", "with a DATE literal"},
        ErrorCase{"SELECT COUNT(*) FROM t WHERE '2' < u16", kBind, "'2'", "USMALLINT"},
        ErrorCase{"SELECT COUNT(*) FROM t WHERE h = '1'", kBind, "'1'", "DECIMAL(38,0)"},
        ErrorCase{"SELECT COUNT(*) FROM t WHERE d = '1.5'", kBind, "'1.5'",
                  "cannot compare DOUBLE column 'd' with a string"},
        ErrorCase{"SELECT COUNT(*) FROM t WHERE d <> DATE '2013-07-01'", kBind, "DATE '2013-07-01'",
                  "DATE literal"},
        ErrorCase{"SELECT COUNT(*) FROM t WHERE s = 1", kBind, "1",
                  "cannot compare VARCHAR column 's' with a number; write a string literal"},
        ErrorCase{"SELECT COUNT(*) FROM t WHERE s = -1.5", kBind, "-1.5", "a number"},
        ErrorCase{"SELECT COUNT(*) FROM t WHERE s = DATE '2013-07-01'", kBind, "DATE '2013-07-01'",
                  "with a DATE literal"},
        ErrorCase{"SELECT COUNT(*) FROM t WHERE dt = 15887", kBind, "15887",
                  "cannot compare DATE column 'dt' with a number; write a date as DATE"},
        ErrorCase{"SELECT COUNT(*) FROM t WHERE dt > 1.5", kBind, "1.5", "a number"},
        ErrorCase{"SELECT COUNT(*) FROM t WHERE dt = '2013-7-1'", kBind, "'2013-7-1'",
                  "invalid date '2013-7-1': expected YYYY-MM-DD"},
        ErrorCase{"SELECT COUNT(*) FROM t WHERE dt = DATE '2013-02-29'", kBind, "DATE '2013-02-29'",
                  "invalid date"},
        ErrorCase{"SELECT COUNT(*) FROM t WHERE dt = ''", kBind, "''", "invalid date ''"},
        ErrorCase{"SELECT COUNT(*) FROM t WHERE dt = '2013-07-01 00:00:00'", kBind,
                  "'2013-07-01 00:00:00'", "expected YYYY-MM-DD"},
        // Long literals are clipped to 32 bytes in the message.
        ErrorCase{"SELECT COUNT(*) FROM t WHERE dt = '0123456789012345678901234567890123456789'",
                  kBind, "'0123456789012345678901234567890123456789'",
                  "'01234567890123456789012345678901...'"},
        // The first error in query order wins (after the table): select list, WHERE, LIMIT.
        ErrorCase{"SELECT nope FROM t WHERE s = 1", kBind, "nope", "does not exist"},
        ErrorCase{"SELECT i16 FROM t WHERE s = 1 AND nope = 2", kBind, "1", "VARCHAR"},
        ErrorCase{"SELECT COUNT(*) FROM t WHERE i16 = 1 AND nope = 2", kBind, "nope",
                  "does not exist"}));

// Expressions the parser accepts but the binder does not answer yet: kUnsupported at the first
// offending token, before any name is resolved.
INSTANTIATE_TEST_SUITE_P(
    Expressions, BindErrorTest,
    ::testing::Values(
        ErrorCase{R"(SELECT "md5"(url) FROM t)", kUnsupported, R"("md5")",
                  "function md5() is not supported"},
        ErrorCase{"SELECT a FROM t GROUP BY year(d)", kUnsupported, "year",
                  "function year() is not supported (only COUNT, SUM, AVG, MIN, MAX, STRLEN, "
                  "REGEXP_REPLACE, TODATETIME, DATE_TRUNC, SUBSTRING, ROUND, ABS, UPPER, LOWER, "
                  "CONCAT and "
                  "COALESCE)"},
        ErrorCase{"SELECT a FROM t GROUP BY a HAVING 1 < 2", kUnsupported, "2",
                  "comparisons between two literals are not supported"},
        ErrorCase{"SELECT a FROM t GROUP BY a HAVING COUNT(*)", kUnsupported, "COUNT(*)",
                  "conditions other than comparisons, [NOT] LIKE, [NOT] IN, [NOT] BETWEEN, AND, OR "
                  "and NOT are "
                  "not supported"},
        ErrorCase{"SELECT a FROM t GROUP BY a HAVING MIN(s) LIKE MAX(s)", kUnsupported, "MAX(s)",
                  "LIKE with a column or an aggregate as the pattern is not supported"},
        ErrorCase{"SELECT a FROM t GROUP BY a HAVING a IN (1, COUNT(*))", kUnsupported, "COUNT(*)",
                  "only literals, constant integer expressions, quotients of numbers and "
                  "subquery values that read no column are supported in an IN list"},
        ErrorCase{"SELECT a FROM t GROUP BY a HAVING 'x' LIKE 'y'", kUnsupported, "'x'",
                  "LIKE needs a column or an aggregate on the left"},
        ErrorCase{"SELECT a FROM t GROUP BY a HAVING 1 IN (1)", kUnsupported, "1",
                  "IN needs a column or an aggregate on the left"},
        ErrorCase{"SELECT a FROM t WHERE url LIKE title", kUnsupported, "title",
                  "LIKE with a column or an aggregate as the pattern is not supported"},
        ErrorCase{"SELECT a FROM t WHERE 'x' LIKE url", kUnsupported, "'x'",
                  "LIKE needs a column on the left"},
        ErrorCase{"SELECT url LIKE '%x%' FROM t", kUnsupported, "LIKE",
                  "LIKE is only supported in conditions (WHERE, HAVING and CASE WHEN)"},
        ErrorCase{"SELECT a FROM t WHERE region IN (1, b)", kUnsupported, "b",
                  "only literals, constant integer expressions, quotients of numbers and "
                  "subquery values that read no column are supported in an IN list"},
        ErrorCase{"SELECT a FROM t WHERE 1 IN (a)", kUnsupported, "1",
                  "IN needs a column on the left"},
        ErrorCase{"SELECT region IN ('a') FROM t", kUnsupported, "IN",
                  "IN is only supported in conditions (WHERE, HAVING and CASE WHEN)"},
        ErrorCase{"SELECT toDateTime(d) FROM t", kBind, "d",
                  "toDateTime() needs an integer (seconds), but 'd' is DOUBLE"},
        ErrorCase{"SELECT toDateTime(h) FROM t", kBind, "h", "toDateTime() needs an integer"},
        ErrorCase{"SELECT toDateTime(i64, 1) FROM t", kBind, "toDateTime(i64, 1)",
                  "todatetime() takes 1 argument, not 2"},
        ErrorCase{"SELECT date_trunc(s, dt) FROM t", kUnsupported, "s",
                  "the unit of date_trunc() must be a string literal"},
        ErrorCase{"SELECT date_trunc('fortnight', dt) FROM t", kUnsupported, "'fortnight'",
                  "date_trunc() with the unit 'fortnight' is not supported"},
        ErrorCase{"SELECT date_trunc('day', s) FROM t", kBind, "s",
                  "date_trunc() needs a TIMESTAMP or DATE, but 's' is VARCHAR"},
        ErrorCase{"SELECT EXTRACT(julian FROM dt) FROM t", kUnsupported, "julian",
                  "EXTRACT of julian is not supported"},
        ErrorCase{"SELECT EXTRACT(dec FROM dt) FROM t", kUnsupported, "dec",
                  "EXTRACT of dec is not supported"},
        ErrorCase{"SELECT EXTRACT(minute FROM i64) FROM t", kBind, "i64",
                  "EXTRACT needs a TIMESTAMP or DATE, but 'i64' is BIGINT"},
        ErrorCase{"SELECT COUNT(*) FROM t WHERE toDateTime(i64) > 5", kBind, "5",
                  "write a timestamp as TIMESTAMP 'YYYY-MM-DD HH:MM:SS'"},
        ErrorCase{"SELECT COUNT(*) FROM t WHERE toDateTime(i64) > '2013-7-15'", kBind,
                  "'2013-7-15'", "invalid timestamp '2013-7-15'"},
        ErrorCase{"SELECT CASE WHEN i16 = 1 THEN toDateTime(i64) ELSE '2013-7-15' END FROM t",
                  kBind, "'2013-7-15'", "invalid timestamp '2013-7-15'"},
        ErrorCase{"SELECT TIMESTAMP '2000-01-01 10:00' + 1 FROM t", kUnsupported, "+",
                  "TIMESTAMP arithmetic ('CAST('2000-01-01 10:00' AS TIMES"},
        ErrorCase{"SELECT TIMESTAMP '2013-07-15 24:00:00' FROM t", kBind,
                  "TIMESTAMP '2013-07-15 24:00:00'", "invalid timestamp"},
        ErrorCase{"SELECT COUNT(*) FROM t WHERE dt > TIMESTAMP '2013-07-15 12:00:00'", kUnsupported,
                  "TIMESTAMP '2013-07-15 12:00:00'",
                  "comparing the DATE 'dt' with a TIMESTAMP literal is not supported"},
        ErrorCase{"SELECT COUNT(*) FROM t WHERE toDateTime(i64) > DATE '2013-02-30'", kBind,
                  "DATE '2013-02-30'", "invalid timestamp '2013-02-30': expected YYYY-MM-DD"},
        ErrorCase{"SELECT COUNT(*) FROM t WHERE toDateTime(i64) > dt", kUnsupported, ">",
                  "comparing a DATE with a TIMESTAMP is not supported"},
        ErrorCase{"SELECT toDateTime(i64) + 1 FROM t", kUnsupported, "+", "TIMESTAMP arithmetic"},
        ErrorCase{"SELECT CASE WHEN i16 = 1 THEN toDateTime(i64) ELSE dt END FROM t", kUnsupported,
                  "dt", "CASE values of types DATE and TIMESTAMP are not supported"},
        ErrorCase{"SELECT CASE WHEN i16 = 1 THEN s ELSE 1 END FROM t", kBind, "1",
                  "cannot mix values of type VARCHAR and INTEGER in CASE"},
        ErrorCase{"SELECT CASE WHEN i16 = 1 THEN dt ELSE 2 END FROM t", kBind, "2",
                  "cannot mix values of type DATE and INTEGER in CASE"},
        ErrorCase{"SELECT CASE WHEN i16 = 1 THEN dt ELSE '2020-13-01' END FROM t", kBind,
                  "'2020-13-01'", "invalid date '2020-13-01'"},
        ErrorCase{"SELECT CASE WHEN i16 = 1 THEN '1' ELSE i16 END FROM t", kUnsupported, "'1'",
                  "a string literal as a CASE value next to SMALLINT values is not supported"},
        ErrorCase{"SELECT CASE WHEN i16 = 1 THEN s = 'a' END FROM t", kUnsupported, "=",
                  "comparisons are only supported in conditions"},
        ErrorCase{"SELECT CASE WHEN 1 = 1 THEN i16 END FROM t", kUnsupported, "1",
                  "comparisons between two literals are not supported"},
        ErrorCase{"SELECT CASE WHEN i16 = 1 THEN i16 END, COUNT(*) FROM t", kBind, "i16",
                  "must appear in the GROUP BY clause"},
        ErrorCase{"SELECT COUNT(*) FROM t WHERE i16 = 1 OR s", kUnsupported, "s",
                  "conditions other than comparisons"},
        ErrorCase{"SELECT COUNT(*) FROM t WHERE i16 = 1 OR s = 1", kBind, "1",
                  "cannot compare VARCHAR column 's' with"},
        ErrorCase{"SELECT a FROM t WHERE a = CASE WHEN b THEN 1 END", kUnsupported, "b",
                  "conditions other than comparisons, [NOT] LIKE, [NOT] IN, [NOT] BETWEEN, AND, OR "
                  "and NOT are not "
                  "supported"},
        ErrorCase{"SELECT a FROM t WHERE flag or b = 2", kUnsupported, "flag",
                  "conditions other than comparisons, [NOT] LIKE, [NOT] IN, [NOT] BETWEEN, AND, OR "
                  "and NOT are not "
                  "supported"},
        ErrorCase{"SELECT i32 FROM t WHERE 1 // 1 = i32", kUnsupported, "1 // 1",
                  "a constant expression in a comparison is not supported (only integer literals "
                  "under +, - and * and a quotient of two numbers are folded)"},
        ErrorCase{"SELECT i32 FROM t WHERE i32 > 2 * 1.5", kUnsupported, "2 * 1.5",
                  "a constant expression in a comparison is not supported"},
        // Constant integer arithmetic folds in DuckDB's types; an overflow is a bind error at
        // the operation (DuckDB raises it at execution: divergence D17).
        ErrorCase{"SELECT i32 FROM t WHERE i32 > 2147483647 + 1", kBind, "2147483647 + 1",
                  "Overflow in addition of INTEGER"},
        ErrorCase{"SELECT i32 FROM t WHERE i32 BETWEEN 1 AND 3 * 1000000000", kBind,
                  "3 * 1000000000", "Overflow in multiplication of INTEGER"},
        ErrorCase{"SELECT i32 FROM t WHERE i32 < -(-9223372036854775807 - 1) - 1", kBind,
                  "-(-9223372036854775807 - 1", "Overflow in negation of BIGINT"},
        // HUGEINT constants stop at 38 digits (DuckDB's at 2^127 - 1): exit code 4, not an
        // overflow.
        ErrorCase{"SELECT i32 FROM t WHERE h > 99999999999999999999999999999999999999 + 1",
                  kUnsupported, "99999999999999999999999999999999999999 + 1",
                  "integer constants outside HUGEINT's range (38 digits) are not supported"},
        ErrorCase{"SELECT i32 FROM t WHERE i32 > 100000000000000000000000000000000000000 - 1",
                  kUnsupported, "100000000000000000000000000000000000000",
                  "integer constants outside HUGEINT's range (38 digits) are not supported"},
        // The same in an IN list: a constant that does not fold stays unsupported, an overflow
        // is the comparison's bind error (D17) and a HUGEINT past 38 digits is unsupported.
        ErrorCase{"SELECT i32 FROM t WHERE i32 IN (1, 1 // 1)", kUnsupported, "1 // 1",
                  "only literals, constant integer expressions, quotients of numbers and "
                  "subquery values that read no column are supported in an IN list"},
        ErrorCase{"SELECT i32 FROM t WHERE i32 NOT IN (2 * 1.5, 1)", kUnsupported, "2 * 1.5",
                  "only literals, constant integer expressions, quotients of numbers and "
                  "subquery values that read no column are supported in an IN list"},
        ErrorCase{"SELECT i32 FROM t WHERE i32 IN (1, 2147483647 + 1)", kBind, "2147483647 + 1",
                  "Overflow in addition of INTEGER"},
        ErrorCase{"SELECT i32 FROM t WHERE h IN (99999999999999999999999999999999999999 + 1)",
                  kUnsupported, "99999999999999999999999999999999999999 + 1",
                  "integer constants outside HUGEINT's range (38 digits) are not supported"},
        // DuckDB compares a DOUBLE and a BIGINT BETWEEN all in DOUBLE, antb1 pairwise.
        ErrorCase{"SELECT i32 FROM t WHERE i64 BETWEEN 1e0 AND 9223372036854775806", kUnsupported,
                  "BETWEEN",
                  "BETWEEN of a DOUBLE value and a BIGINT, HUGEINT, FLOAT or DECIMAL value"},
        ErrorCase{"SELECT i32 FROM t WHERE i64 NOT BETWEEN d AND 5", kUnsupported, "NOT BETWEEN",
                  "BETWEEN of a DOUBLE value and a BIGINT, HUGEINT, FLOAT or DECIMAL value"},
        ErrorCase{"SELECT i32 FROM t WHERE i64 BETWEEN 1 AND d", kUnsupported, "BETWEEN",
                  "BETWEEN of a DOUBLE value and a BIGINT, HUGEINT, FLOAT or DECIMAL value"},
        ErrorCase{"SELECT i32 FROM t WHERE i32 BETWEEN 1e0 AND 2147483648 * 2", kUnsupported,
                  "BETWEEN",
                  "BETWEEN of a DOUBLE value and a BIGINT, HUGEINT, FLOAT or DECIMAL value"},
        // A decimal literal against an integer value is exact on its own, rounded in DOUBLE.
        ErrorCase{"SELECT i32 FROM t WHERE i32 BETWEEN 1.00000000000000000001 AND 1e1",
                  kUnsupported, "BETWEEN", "a decimal literal and an integer value"},
        ErrorCase{"SELECT i32 FROM t WHERE i32 BETWEEN 1.5 AND d", kUnsupported, "BETWEEN",
                  "a decimal literal and an integer value"},
        // An integer literal of 39 digits is HUGEINT or DOUBLE in DuckDB.
        ErrorCase{"SELECT i32 FROM t WHERE i32 BETWEEN -1000000000000000000000000000000000000000 "
                  "AND 5",
                  kUnsupported, "BETWEEN", "BETWEEN of a DOUBLE value"},
        ErrorCase{"SELECT i16 FROM t GROUP BY i16 HAVING SUM(i64) BETWEEN 0 AND 1e0", kUnsupported,
                  "BETWEEN",
                  "BETWEEN of a DOUBLE value and a BIGINT, HUGEINT, FLOAT or DECIMAL value"},
        // A DECIMAL in a BETWEEN with a DOUBLE value: DuckDB compares all three in DOUBLE, so a
        // pair that antb1 compares exactly differs (ADR 0021): a DECIMAL operand with any bound
        // that is not DOUBLE, or a DECIMAL column or expression as a bound.
        ErrorCase{"SELECT COUNT(*) FROM dec WHERE p BETWEEN 1e0 AND 5", kUnsupported, "BETWEEN",
                  "or DECIMAL value"},
        ErrorCase{"SELECT COUNT(*) FROM dec WHERE p BETWEEN 2 AND 1e1", kUnsupported, "BETWEEN",
                  "or DECIMAL value"},
        ErrorCase{"SELECT COUNT(*) FROM dec WHERE p BETWEEN 1e0 AND 2 + 3", kUnsupported, "BETWEEN",
                  "or DECIMAL value"},
        ErrorCase{"SELECT COUNT(*) FROM dec WHERE p BETWEEN -(-2) AND 1e1", kUnsupported, "BETWEEN",
                  "or DECIMAL value"},
        ErrorCase{"SELECT COUNT(*) FROM dec WHERE p BETWEEN 1.5 AND 1e1", kUnsupported, "BETWEEN",
                  "or DECIMAL value"},
        ErrorCase{"SELECT COUNT(*) FROM dec WHERE p BETWEEN 1e0 AND i", kUnsupported, "BETWEEN",
                  "or DECIMAL value"},
        ErrorCase{"SELECT COUNT(*) FROM dec WHERE i BETWEEN 1e0 AND p", kUnsupported, "BETWEEN",
                  "or DECIMAL value"},
        ErrorCase{"SELECT COUNT(*) FROM dec WHERE 5 BETWEEN p AND f", kUnsupported, "BETWEEN",
                  "or DECIMAL value"},
        ErrorCase{"SELECT COUNT(*) FROM dec WHERE f > 0 OR p NOT BETWEEN q AND 1e1", kUnsupported,
                  "NOT BETWEEN", "or DECIMAL value"},
        ErrorCase{"SELECT CASE WHEN p BETWEEN 1e0 AND r THEN 1 END FROM dec", kUnsupported,
                  "BETWEEN", "or DECIMAL value"},
        ErrorCase{"SELECT i FROM dec GROUP BY i HAVING SUM(p) BETWEEN 1e0 AND 5", kUnsupported,
                  "BETWEEN", "or DECIMAL value"},
        // Pairwise, the string compares with both; DuckDB rejects the mix when binding.
        ErrorCase{"SELECT i32 FROM t WHERE '2020-01-01' BETWEEN s AND dt", kBind, "BETWEEN",
                  "Cannot mix values of type VARCHAR and DATE in BETWEEN clause"},
        ErrorCase{"SELECT i32 FROM t WHERE '2020-01-01' NOT BETWEEN dt AND s", kBind, "NOT BETWEEN",
                  "Cannot mix values of type VARCHAR and DATE in BETWEEN clause"},
        // DuckDB folds every minus of a literal into it: -(-(-9223372036854775808)) is a BIGINT.
        ErrorCase{"SELECT i32 FROM t WHERE i64 = -(-(-9223372036854775808)) - 1", kBind,
                  "-(-(-9223372036854775808)) - 1", "Overflow in subtraction of BIGINT"},
        ErrorCase{"SELECT i32 FROM t WHERE 1 BETWEEN 0 AND 2", kUnsupported, "0",
                  "comparisons between two literals are not supported"},
        ErrorCase{"SELECT i32 FROM t WHERE i32 BETWEEN i16 AND s", kBind, "BETWEEN",
                  "cannot compare"},
        ErrorCase{"SELECT i32 BETWEEN 1 AND 2 FROM t", kUnsupported, "BETWEEN",
                  "BETWEEN is only supported in conditions"},
        // Arithmetic: numbers only, no DATE arithmetic, no // or % in HUGEINT.
        ErrorCase{"SELECT s + 1 FROM t", kBind, "+",
                  "arithmetic operator '+' needs numbers, but 's' is VARCHAR"},
        ErrorCase{"SELECT -s FROM t", kBind, "-", "needs a number, but 's' is VARCHAR"},
        ErrorCase{"SELECT dt - dt FROM t", kUnsupported, "-", "DATE arithmetic"},
        ErrorCase{"SELECT dt + i64 FROM t", kBind, "+",
                  "No function matches the given name and argument types '+(DATE, BIGINT)'"},
        ErrorCase{"SELECT 1 - dt FROM t", kBind, "-",
                  "No function matches the given name and argument types '-(INTEGER, DATE)'"},
        ErrorCase{"SELECT -u16 FROM t", kUnsupported, "-", "negating a USMALLINT is not supported"},
        ErrorCase{"SELECT SUM(i64) % 2 FROM t", kUnsupported, "%", "'%' in HUGEINT"},
        // DECIMAL (ADR 0021).
        ErrorCase{"SELECT p + 'x' FROM dec", kBind, "+", "needs numbers"},
        ErrorCase{"SELECT z * z * z * z FROM dec", kBind, "*",
                  "Needed scale 40 to accurately represent the multiplication result"},
        // A DECIMAL against a VARCHAR or DATE operand, as any number (divergence D4: DuckDB casts
        // the VARCHAR for = and <>, and fails on a DATE only when a row is compared).
        ErrorCase{"SELECT COUNT(*) FROM t WHERE h = s", kBind, "=",
                  "cannot compare 'h' is DECIMAL(38,0) with 's' is VARCHAR"},
        ErrorCase{"SELECT COUNT(*) FROM t WHERE h <> s", kBind, "<>", "cannot compare"},
        ErrorCase{"SELECT COUNT(*) FROM t WHERE s < h", kBind, "<", "cannot compare"},
        ErrorCase{"SELECT COUNT(*) FROM t WHERE h = dt", kBind, "=",
                  "cannot compare 'h' is DECIMAL(38,0) with 'dt' is DATE"},
        ErrorCase{"SELECT COUNT(*) FROM t WHERE h = toDateTime(i64)", kBind, "=",
                  "cannot compare 'h' is DECIMAL(38,0) with 'todatetime(i64)' is TIMESTAMP"},
        ErrorCase{"SELECT i16 FROM t GROUP BY i16 HAVING MIN(h) = MIN(s)", kBind, "=",
                  "cannot compare"},
        // DECIMAL CASE values fold from the ELSE through the THENs (ADR 0021 rule 10): the type
        // folded so far is named first; a string literal that a DECIMAL type takes is unsupported
        // (DuckDB casts it), one next to NULL (no ELSE) or another literal is VARCHAR.
        ErrorCase{"SELECT CASE WHEN i32 > 0 THEN h ELSE dt END FROM t", kBind, "h",
                  "cannot mix values of type DATE and DECIMAL(38,0) in CASE"},
        ErrorCase{"SELECT CASE WHEN i32 > 0 THEN h WHEN i32 > 1 THEN dt END FROM t", kBind, "dt",
                  "cannot mix values of type DECIMAL(38,0) and DATE in CASE"},
        ErrorCase{"SELECT CASE WHEN i32 > 0 THEN 'abc' WHEN i32 > 1 THEN h END FROM t", kBind, "h",
                  "cannot mix values of type VARCHAR and DECIMAL(38,0) in CASE"},
        ErrorCase{"SELECT CASE WHEN i32 > 0 THEN 7 WHEN i32 > 1 THEN h ELSE s END FROM t", kBind,
                  "7", "cannot mix values of type VARCHAR and INTEGER in CASE"},
        ErrorCase{"SELECT CASE WHEN i32 > 0 THEN h ELSE 'abc' END FROM t", kUnsupported, "'abc'",
                  "a string literal as a CASE value next to DECIMAL(38,0) values is not supported"},
        ErrorCase{"SELECT CASE WHEN i32 > 0 THEN 7 WHEN i32 > 1 THEN h ELSE '5' END FROM t",
                  kUnsupported, "'5'", "a string literal as a CASE value next to DECIMAL"},
        ErrorCase{"SELECT COUNT(*) FROM dec WHERE p = 'x'", kBind, "'x'",
                  "cannot compare DECIMAL(15,2) column 'p' with a string"},
        ErrorCase{"SELECT SUM(i16) // 2 FROM t", kUnsupported, "//", "'//' in HUGEINT"},
        // String functions: a VARCHAR first argument, literal strings after it.
        ErrorCase{"SELECT strlen(i16) FROM t", kBind, "i16",
                  "strlen() needs a VARCHAR, but 'i16' is SMALLINT"},
        ErrorCase{"SELECT strlen(s, s) FROM t", kBind, "strlen(s, s)",
                  "strlen() takes 1 argument, not 2"},
        ErrorCase{"SELECT regexp_replace(s, 'a') FROM t", kBind, "regexp_replace(s, 'a')",
                  "takes 3 arguments, not 2"},
        ErrorCase{"SELECT regexp_replace(s, s, 'x') FROM t", kUnsupported, "s",
                  "the arguments of regexp_replace() after the first must be string literals"},
        ErrorCase{"SELECT regexp_replace(s, '(a)(b)', '\\8') FROM t", kUnsupported, "'\\8'",
                  "regexp_replace() with \\8 in the replacement is not supported"},
        ErrorCase{"SELECT regexp_replace(s, '\\Qa', 'x') FROM t", kUnsupported, "'\\Qa'",
                  "regexp_replace() with \\Q in the pattern is not supported"},
        // substring: a VARCHAR value and integer positions, two or three arguments. DuckDB is a
        // binder error for the same value and position types, except a VARCHAR position that parses
        // as a number, which it casts and antb1 refuses (docs/sql-subset.md).
        ErrorCase{"SELECT substring(i16, 1, 2) FROM t", kBind, "i16",
                  "substring() needs a VARCHAR, but 'i16' is SMALLINT"},
        ErrorCase{"SELECT substring(s, s, 2) FROM t", kBind, "s",
                  "argument 2 of substring() must be an integer, but 's' is VARCHAR"},
        ErrorCase{"SELECT substring(s, 1, d) FROM t", kBind, "d",
                  "argument 3 of substring() must be an integer, but 'd' is DOUBLE"},
        ErrorCase{"SELECT substring(s, 1, h) FROM t", kBind, "h",
                  "argument 3 of substring() must be an integer, but 'h' is DECIMAL(38,0)"},
        ErrorCase{"SELECT substring(s) FROM t", kBind, "substring(s)",
                  "substring() takes 2 or 3 arguments, not 1"},
        ErrorCase{"SELECT substring(s, 1, 2, 3) FROM t", kBind, "substring(s, 1, 2, 3)",
                  "substring() takes 2 or 3 arguments, not 4"},
        ErrorCase{"SELECT substr(s, 1, 2, 3) FROM t", kBind, "substr(s, 1, 2, 3)",
                  "substr() takes 2 or 3 arguments, not 4"},
        ErrorCase{"SELECT regexp_replace(s, 'a', 1) FROM t", kBind, "1",
                  "argument 3 of regexp_replace() must be a string literal"},
        ErrorCase{"SELECT regexp_replace(s, 'a', 'b', 'g') FROM t", kUnsupported, "'g'",
                  "regexp_replace() with options (a fourth argument) is not supported"},
        ErrorCase{"SELECT regexp_replace(s, 'a', 'b', 'g', 'x') FROM t", kBind,
                  "regexp_replace(s, 'a', 'b', 'g', 'x')", "takes 3 arguments, not 5"},
        ErrorCase{"SELECT strlen(md5(s)) FROM t", kUnsupported, "md5",
                  "function md5() is not supported"},
        ErrorCase{"SELECT SUM(i16) + 1 AS total FROM t GROUP BY total", kBind, "total",
                  "GROUP BY cannot refer to the aggregate 'total'"},
        ErrorCase{"SELECT s FROM t GROUP BY s HAVING SUM(i16) + 1 > s", kBind, ">",
                  "cannot compare"},
        ErrorCase{"SELECT i16 FROM t WHERE i16 + 1 = s", kBind, "=", "cannot compare"},
        ErrorCase{"SELECT i16 FROM t GROUP BY 1 + 1", kUnsupported, "1 + 1",
                  "GROUP BY a constant expression is not supported"},
        ErrorCase{"SELECT i16 FROM t ORDER BY 1 + 1", kUnsupported, "1 + 1",
                  "ORDER BY a constant expression is not supported"},
        ErrorCase{"SELECT SUM(1 + 2) FROM t", kUnsupported, "1 + 2",
                  "constant aggregate arguments are not supported"},
        ErrorCase{"SELECT md5(url) FROM t", kUnsupported, "md5",
                  "function md5() is not supported (only COUNT, SUM, AVG, MIN, MAX, STRLEN, "
                  "REGEXP_REPLACE, TODATETIME, DATE_TRUNC, SUBSTRING, ROUND, ABS, UPPER, LOWER, "
                  "CONCAT and "
                  "COALESCE)"},
        ErrorCase{"SELECT a FROM t WHERE length(url) > 5", kUnsupported, "length",
                  "function length() is not supported (only COUNT, SUM, AVG, MIN, MAX, STRLEN, "
                  "REGEXP_REPLACE, TODATETIME, DATE_TRUNC, SUBSTRING, ROUND, ABS, UPPER, LOWER, "
                  "CONCAT and "
                  "COALESCE)"},
        ErrorCase{"SELECT a FROM t WHERE d > now()", kUnsupported, "now",
                  "function now() is not supported (only COUNT, SUM, AVG, MIN, MAX, STRLEN, "
                  "REGEXP_REPLACE, TODATETIME, DATE_TRUNC, SUBSTRING, ROUND, ABS, UPPER, LOWER, "
                  "CONCAT and "
                  "COALESCE)"},
        ErrorCase{"SELECT SUM(sqrt(a)) FROM t", kUnsupported, "sqrt",
                  "function sqrt() is not supported (only COUNT, SUM, AVG, MIN, MAX, STRLEN, "
                  "REGEXP_REPLACE, TODATETIME, DATE_TRUNC, SUBSTRING, ROUND, ABS, UPPER, LOWER, "
                  "CONCAT and "
                  "COALESCE)"},
        ErrorCase{"SELECT left(url, 3) FROM t", kUnsupported, "left",
                  "function left() is not supported (only COUNT, SUM, AVG, MIN, MAX, STRLEN, "
                  "REGEXP_REPLACE, TODATETIME, DATE_TRUNC, SUBSTRING, ROUND, ABS, UPPER, LOWER, "
                  "CONCAT and "
                  "COALESCE)"},
        ErrorCase{"SELECT COUNT(1) FROM t", kUnsupported, "1",
                  "constant aggregate arguments are not supported (use COUNT(*))"},
        ErrorCase{"SELECT SUM('x') FROM t", kUnsupported, "'x'",
                  "constant aggregate arguments are not supported"},
        ErrorCase{"SELECT a = 1 FROM t", kUnsupported, "=",
                  "comparisons are only supported in conditions (WHERE, HAVING and CASE WHEN)"},
        ErrorCase{"SELECT a AND b FROM t", kUnsupported, "AND",
                  "AND is only supported in conditions (WHERE, HAVING and CASE WHEN)"},
        ErrorCase{"SELECT a FROM t WHERE 1 = 1", kUnsupported, "1",
                  "comparisons between two literals are not supported"},
        ErrorCase{"SELECT a FROM t WHERE flag", kUnsupported, "flag",
                  "conditions other than comparisons, [NOT] LIKE, [NOT] IN, [NOT] BETWEEN, AND, OR "
                  "and NOT are not "
                  "supported"},
        ErrorCase{"SELECT a FROM t WHERE flag AND a = 1", kUnsupported, "flag",
                  "conditions other than comparisons, [NOT] LIKE, [NOT] IN, [NOT] BETWEEN, AND, OR "
                  "and NOT are not "
                  "supported"},
        ErrorCase{"SELECT a FROM t WHERE 1 LIMIT 5", kUnsupported, "1",
                  "conditions other than comparisons, [NOT] LIKE, [NOT] IN, [NOT] BETWEEN, AND, OR "
                  "and NOT are not "
                  "supported"},
        ErrorCase{"SELECT a FROM t WHERE flag GROUP BY a", kUnsupported, "flag",
                  "conditions other than comparisons, [NOT] LIKE, [NOT] IN, [NOT] BETWEEN, AND, OR "
                  "and NOT are not "
                  "supported"},
        // Each kind of expression that is no condition, also under AND and OR, and what the binder
        // does not answer inside a condition's operands.
        ErrorCase{"SELECT a FROM t WHERE (flag AND a = 1) OR a = 2", kUnsupported, "flag",
                  "conditions other than comparisons, [NOT] LIKE, [NOT] IN, [NOT] BETWEEN, AND, OR "
                  "and NOT are not "
                  "supported"},
        ErrorCase{"SELECT a FROM t WHERE a + 1", kUnsupported, "a + 1",
                  "conditions other than comparisons, [NOT] LIKE, [NOT] IN, [NOT] BETWEEN, AND, OR "
                  "and NOT are not "
                  "supported"},
        ErrorCase{"SELECT a FROM t WHERE -a", kUnsupported, "-a",
                  "conditions other than comparisons, [NOT] LIKE, [NOT] IN, [NOT] BETWEEN, AND, OR "
                  "and NOT are not "
                  "supported"},
        ErrorCase{"SELECT a FROM t WHERE strlen(url)", kUnsupported, "strlen(url)",
                  "this condition is not supported"},
        ErrorCase{"SELECT a FROM t WHERE CASE WHEN a = 1 THEN b END", kUnsupported,
                  "CASE WHEN a = 1 THEN b END", "this condition is not supported"},
        ErrorCase{"SELECT a FROM t WHERE EXTRACT(year FROM dt)", kUnsupported,
                  "EXTRACT(year FROM dt)", "this condition is not supported"},
        ErrorCase{"SELECT a FROM t WHERE md5(url)", kUnsupported, "md5",
                  "function md5() is not supported"},
        ErrorCase{"SELECT a FROM t WHERE md5(url) LIKE 'x%'", kUnsupported, "md5",
                  "function md5() is not supported"},
        ErrorCase{"SELECT a FROM t WHERE url LIKE md5(title)", kUnsupported, "md5",
                  "function md5() is not supported"},
        ErrorCase{"SELECT a FROM t WHERE md5(url) IN ('x')", kUnsupported, "md5",
                  "function md5() is not supported"},
        ErrorCase{"SELECT a FROM t WHERE url IN ('x', md5(title))", kUnsupported, "md5",
                  "function md5() is not supported"}));

// Casts: CAST('YYYY-MM-DD' AS DATE) and 'YYYY-MM-DD'::DATE are DATE literals spanning the cast,
// with their bind errors (D4, D5). A cast to a type antb1 has no type for is kUnsupported before
// any name is resolved; one to a type it has is bound (ADR 0037's CT1), and kUnsupported there
// unless it is an identity, an exact widening or a constant the type holds.
INSTANTIATE_TEST_SUITE_P(
    Casts, BindErrorTest,
    ::testing::Values(
        ErrorCase{"SELECT CAST('2020-02-30' AS DATE) FROM t", kBind, "CAST('2020-02-30' AS DATE)",
                  "invalid date '2020-02-30'"},
        ErrorCase{"SELECT COUNT(*) FROM t WHERE dt = '2013-7-1'::date", kBind, "'2013-7-1'::date",
                  "invalid date '2013-7-1': expected YYYY-MM-DD"},
        ErrorCase{"SELECT COUNT(*) FROM t WHERE dt IN ('2013-07-01 00:00:00'::DATE)", kBind,
                  "'2013-07-01 00:00:00'::DATE", "expected YYYY-MM-DD"},
        ErrorCase{"SELECT COUNT(*) FROM t WHERE s = CAST('2013-07-01' AS DATE)", kBind,
                  "CAST('2013-07-01' AS DATE)",
                  "cannot compare VARCHAR column 's' with a DATE literal"},
        ErrorCase{"SELECT COUNT(*) FROM t WHERE '2013-07-01'::DATE < i64", kBind,
                  "'2013-07-01'::DATE", "cannot compare BIGINT column 'i64' with a DATE literal"},
        ErrorCase{"SELECT CAST('2013-07-01' AS DATE) * 2 FROM t", kUnsupported, "*",
                  "DATE arithmetic"},
        ErrorCase{"SELECT COUNT(*) FROM t WHERE '2013-07-01'::DATE", kUnsupported,
                  "'2013-07-01'::DATE", "conditions other than comparisons"},
        ErrorCase{"SELECT -CAST('2013-07-01' AS DATE) FROM t", kBind, "-",
                  "arithmetic operator '-' needs a number, but 'CAST('2013-07-01' AS \"DATE\")' is "
                  "DATE"},
        ErrorCase{"SELECT COUNT(*) FROM t WHERE s LIKE CAST('2013-07-01' AS DATE)", kBind,
                  "CAST('2013-07-01' AS DATE)", "the pattern of LIKE must be a string literal"},
        ErrorCase{"SELECT COUNT(*) FROM t WHERE '2013-07-01'::DATE LIKE 'x'", kUnsupported,
                  "'2013-07-01'::DATE", "LIKE needs a column on the left"},
        ErrorCase{"SELECT COUNT(*) FROM t WHERE '2013-07-01'::DATE IN (dt)", kUnsupported,
                  "'2013-07-01'::DATE", "IN needs a column on the left"},
        // Every other cast, at its first unsupported part in source order.
        ErrorCase{"SELECT TRY_CAST('2013-07-01' AS DATE) FROM t", kUnsupported, "TRY_CAST",
                  "TRY_CAST is not supported"},
        ErrorCase{"SELECT try_cast(md5(nope) AS INT) FROM t", kUnsupported, "try_cast",
                  "TRY_CAST is not supported"},
        ErrorCase{"SELECT CAST(i16 AS FLOAT) FROM t", kUnsupported, "FLOAT",
                  "CAST to FLOAT is not supported"},
        ErrorCase{"SELECT CAST(nope AS INTERVAL) FROM t", kUnsupported, "INTERVAL",
                  "CAST to INTERVAL is not supported"},
        ErrorCase{"SELECT nope::varchar FROM t", kBind, "nope", "column 'nope' does not exist"},
        ErrorCase{"SELECT CAST(i16 AS DECIMAL(40, 2)) FROM t", kBind, "DECIMAL(40, 2)",
                  "DECIMAL type width must be between 1 and 38"},
        ErrorCase{"SELECT CAST(i16 AS DECIMAL(4, 5)) FROM t", kBind, "DECIMAL(4, 5)",
                  "DECIMAL type scale cannot be greater than width"},
        ErrorCase{"SELECT CAST('2013-07-01' AS DATE(3)) FROM t", kUnsupported, "DATE(3)",
                  "CAST to DATE(3) is not supported"},
        ErrorCase{"SELECT CAST(20130701 AS DATE) FROM t", kUnsupported, "CAST(20130701 AS DATE)",
                  "CAST of INTEGER values to DATE is not supported"},
        ErrorCase{"SELECT CAST(70000 AS SMALLINT) FROM t", kUnsupported, "CAST(70000 AS SMALLINT)",
                  "CAST of INTEGER values to SMALLINT is not supported"},
        ErrorCase{"SELECT CAST(9.95 AS DECIMAL(2,1)) FROM t", kUnsupported,
                  "CAST(9.95 AS DECIMAL(2,1))",
                  "CAST of DECIMAL(3,2) values to DECIMAL(2,1) is not supported"},
        ErrorCase{"SELECT CAST(d AS DECIMAL(5,1)) FROM t", kUnsupported, "CAST(d AS DECIMAL(5,1))",
                  "CAST of DOUBLE values to DECIMAL(5,1) is not supported"},
        ErrorCase{"SELECT CAST(md5(s) AS INT) FROM t", kUnsupported, "md5",
                  "function md5() is not supported"},
        ErrorCase{"SELECT (i16 = 1)::VARCHAR FROM t", kUnsupported, "=",
                  "comparisons are only supported in conditions"},
        ErrorCase{"SELECT COUNT(*) FROM t WHERE CAST(i16 AS VARCHAR) = '1'", kUnsupported,
                  "CAST(i16 AS VARCHAR)", "CAST of SMALLINT values to VARCHAR is not supported"},
        ErrorCase{"SELECT COUNT(*) FROM t WHERE i16 IN (1, 2::INT)", kUnsupported, "2::INT",
                  "only literals, constant integer expressions, quotients of numbers and "
                  "subquery values that read no column are supported in an IN list"},
        ErrorCase{"SELECT COUNT(*) FROM t WHERE s LIKE 'x%'::VARCHAR", kUnsupported,
                  "'x%'::VARCHAR", "LIKE with a column or an aggregate as the pattern"},
        ErrorCase{"SELECT i16 FROM t GROUP BY i16::INT", kBind, "i16",
                  "must appear in the GROUP BY clause"},
        ErrorCase{"SELECT i16 FROM t GROUP BY CAST(i16 AS SMALLINT)", kBind, "i16",
                  "must appear in the GROUP BY clause"},
        ErrorCase{"SELECT CAST(i16 AS SMALLINT) AS z, i16 FROM t GROUP BY z", kBind, "i16",
                  "must appear in the GROUP BY clause"},
        ErrorCase{"SELECT CAST(1e3 AS DOUBLE) FROM t", kUnsupported, "CAST(1e3 AS DOUBLE)",
                  "CAST of the number 1e3 to DOUBLE is not supported"},
        ErrorCase{"SELECT CAST('2020-1-2' AS TIMESTAMP) FROM t", kBind, "'2020-1-2'",
                  "invalid timestamp '2020-1-2'"},
        ErrorCase{"SELECT CAST(d AS HUGEINT) FROM t", kUnsupported, "CAST(d AS HUGEINT)",
                  "CAST of DOUBLE values to HUGEINT is not supported"},
        ErrorCase{"SELECT COUNT(*) FROM t WHERE CAST(s AS INT) > 1", kUnsupported, "CAST(s AS INT)",
                  "CAST of VARCHAR values to INTEGER is not supported"},
        ErrorCase{"SELECT EXTRACT(year FROM CAST(i64 AS TIMESTAMP)) FROM t", kUnsupported,
                  "CAST(i64 AS TIMESTAMP)", "CAST of BIGINT values to TIMESTAMP is not supported"},
        ErrorCase{"SELECT date_trunc('day'::VARCHAR, dt) FROM t", kUnsupported, "'day'::VARCHAR",
                  "the unit of date_trunc() must be a string literal"},
        // A call with the wrong number of arguments is a bind error whatever its arguments
        // (CheckSupported does not look into them): the binder still sees the casts in them.
        ErrorCase{"SELECT strlen(s::VARCHAR, 1) FROM t", kBind, "strlen(s::VARCHAR, 1)",
                  "strlen() takes 1 argument, not 2"},
        ErrorCase{"SELECT i16, strlen(MAX(s)::VARCHAR, 1) FROM t", kBind, "i16",
                  "column 'i16' must be inside an aggregate function"},
        ErrorCase{"SELECT SUM(strlen(s::VARCHAR, 1)) FROM t", kBind, "strlen(s::VARCHAR, 1)",
                  "strlen() takes 1 argument, not 2"},
        ErrorCase{"SELECT SUM(strlen('x'::VARCHAR, 1)) FROM t", kUnsupported,
                  "strlen('x'::VARCHAR, 1)", "constant aggregate arguments are not supported"},
        ErrorCase{"SELECT COUNT(*) FROM t WHERE strlen(s::VARCHAR, 1) > 1", kBind,
                  "strlen(s::VARCHAR, 1)", "strlen() takes 1 argument, not 2"},
        ErrorCase{"SELECT COUNT(*) FROM t GROUP BY i16 HAVING strlen(MAX(s)::VARCHAR, 1) > 1",
                  kBind, "strlen(MAX(s)::VARCHAR, 1)", "strlen() takes 1 argument, not 2"},
        ErrorCase{"SELECT CASE WHEN i16 = 1 THEN strlen(s::VARCHAR, 1) END FROM t", kBind,
                  "strlen(s::VARCHAR, 1)", "strlen() takes 1 argument, not 2"},
        ErrorCase{"SELECT i16 FROM t ORDER BY strlen(s::VARCHAR, 1)", kBind,
                  "strlen(s::VARCHAR, 1)", "strlen() takes 1 argument, not 2"}));

// FROM lists and joins parse (ADR 0022) and are kUnsupported until the binder answers them: before
// any name is resolved, so also over tables that do not exist, at the first one in query order
// (select list, FROM, WHERE, GROUP BY, HAVING, ORDER BY). Aliases and qualified names bind over one
// FROM item from J2b-2 on, so they appear here only where the name itself is the error;
// QualifiedNamesAndAliasesBind covers the ones that answer.
INSTANTIATE_TEST_SUITE_P(
    FromLists, BindErrorTest,
    ::testing::Values(
        // A column both relations have is ambiguous in the select list, which binds before the
        // join order (rule 3, and the documented error order).
        ErrorCase{"SELECT i16 FROM t, u", kBind, "i16",
                  "column name 'i16' is ambiguous: it matches t.i16 and u.i16"},
        // Every item's table is resolved, in FROM order, before any conjunct binds.
        ErrorCase{"SELECT COUNT(*) FROM nope, missing", kBind, "nope",
                  "table 'nope' does not exist"},
        ErrorCase{"SELECT COUNT(*) FROM t, missing", kBind, "missing",
                  "table 'missing' does not exist"},
        // An unqualified name an ON sees in both its relations is ambiguous (rule 3).
        ErrorCase{"SELECT i16 FROM t inner join u ON i16 = 1", kBind, "i16",
                  "column name 'i16' is ambiguous: it matches t.i16 and u.i16"},
        // A LEFT JOIN's ON sees its own two inputs only (rule 10). An earlier comma sibling is
        // visible, as an inner ON's outer level, but is no input of this join: DuckDB refuses such
        // an ON too ("Non-inner join on correlated columns not supported").
        ErrorCase{"SELECT u.i16 FROM t, u LEFT JOIN ok ON ok.i16 = u.i16 AND t.i32 > 1",
                  kUnsupported, "t.i32", "a LEFT JOIN's ON must read its own two inputs only"},
        ErrorCase{"SELECT u.i16 FROM t, u FULL JOIN ok ON ok.i16 = u.i16 AND t.i32 > 1",
                  kUnsupported, "t.i32", "a FULL JOIN's ON must read its own two inputs only"},
        // An inner join's ON inside a FULL JOIN's left input binds inside that input, which the
        // FULL JOIN pads, so a conjunct of it that would bind above every join of the block -- a
        // correlated subquery, a column of an enclosing block, a comma sibling before the join
        // group -- would drop the padded rows: exit 4. Under a LEFT JOIN, which never pads its left
        // input, those are answered; an uncorrelated subquery stands in that input's own slot
        // (ADR 0041's SQ1).
        ErrorCase{
            "SELECT t.i16 FROM t JOIN u ON t.i16 = u.i16 AND EXISTS (SELECT 1 FROM dec WHERE "
            "dec.i = t.i32) FULL JOIN ok ON u.i16 = ok.i16",
            kUnsupported, "EXISTS",
            "a correlated subquery in the ON of a join inside a FULL JOIN's left input is not "
            "supported"},
        ErrorCase{"SELECT t.i16 FROM t WHERE EXISTS (SELECT * FROM u JOIN dec ON u.i32 = dec.i AND "
                  "dec.b = t.i64 FULL JOIN ok ON u.i16 = ok.i16)",
                  kUnsupported, "dec.b = t.i64",
                  "a column of an enclosing block in the ON of a join inside a FULL JOIN's left "
                  "input is not supported"},
        ErrorCase{"SELECT t.i16 FROM dec, t JOIN u ON t.i16 = u.i16 AND dec.i = t.i32 FULL JOIN ok "
                  "ON u.i16 = ok.i16",
                  kUnsupported, "dec.i = t.i32",
                  "the ON of a join inside a FULL JOIN's left input must read that input only"},
        // So must a conjunct with a subquery, which filters in that input's own slot (ADR 0041's
        // SQ1): an IN, a value and an EXISTS under OR, each beside a comma sibling's column.
        ErrorCase{
            "SELECT t.i16 FROM dec, t JOIN u ON t.i16 = u.i16 AND dec.i IN (SELECT ok.i16 FROM "
            "ok) FULL JOIN ok ON u.i16 = ok.i16",
            kUnsupported, "dec.i IN (SELECT ok.i16 FROM ok)",
            "the ON of a join inside a FULL JOIN's left input must read that input only"},
        ErrorCase{
            "SELECT t.i16 FROM dec, t JOIN u ON t.i16 = u.i16 AND dec.i > (SELECT MIN(ok.i16) "
            "FROM ok) FULL JOIN ok ON u.i16 = ok.i16",
            kUnsupported, "dec.i > (SELECT MIN(ok.i16) FROM ok)",
            "the ON of a join inside a FULL JOIN's left input must read that input only"},
        ErrorCase{
            "SELECT t.i16 FROM dec, t JOIN u ON t.i16 = u.i16 AND (dec.i > 0 OR EXISTS (SELECT "
            "1 FROM ok)) FULL JOIN ok ON u.i16 = ok.i16",
            kUnsupported, "dec.i > 0 OR EXISTS (SELECT 1 FROM ok)",
            "the ON of a join inside a FULL JOIN's left input must read that input only"},
        // The missing tables are still missing, and that bind error comes first, as in DuckDB.
        ErrorCase{"SELECT i16 FROM nope LEFT JOIN missing ON a = b", kBind, "nope",
                  "table 'nope' does not exist"},
        // An alias no longer hides the reference it renames: the name behind it is resolved, and
        // fails where it would without one.
        ErrorCase{"SELECT COUNT(*) FROM nope n", kBind, "nope", "table 'nope' does not exist"},
        ErrorCase{"SELECT COUNT(*) FROM 'x.parquet' AS 'p'", kUnsupported, "'x.parquet'",
                  "file paths in FROM are not enabled in this session"},
        // A qualified name resolves among the bindings it names (rules 2 and 3), so it is an error
        // only where the name is: a binding without the column, or no binding of the name.
        ErrorCase{"SELECT u.i16 FROM t", kBind, "u.i16", "no FROM item is named 'u'"},
        ErrorCase{"SELECT t.nope FROM t", kBind, "t.nope", "'t' has no column 'nope'"},
        ErrorCase{"SELECT i16 FROM t AS a WHERE t.i16 = 1", kBind, "t.i16",
                  "no FROM item is named 't' (the alias 'a' hides it)"},
        // A select alias is no binding, so a qualified name never finds one, in ORDER BY, GROUP BY
        // or HAVING (DuckDB errors there too).
        ErrorCase{"SELECT i16 AS x FROM t ORDER BY t.x", kBind, "t.x", "'t' has no column 'x'"},
        ErrorCase{"SELECT i16 AS x FROM t GROUP BY t.x", kBind, "t.x", "'t' has no column 'x'"},
        ErrorCase{"SELECT MAX(i16) AS x FROM t HAVING t.x > 1", kBind, "t.x",
                  "'t' has no column 'x'"},
        // A qualifier no longer masks the rule that does reject the expression: LIKE's pattern and
        // an IN list's elements must be literals, qualified or not.
        ErrorCase{"SELECT COUNT(*) FROM t WHERE s LIKE t.s", kUnsupported, "t.s",
                  "LIKE with a column or an aggregate as the pattern is not supported"},
        ErrorCase{"SELECT COUNT(*) FROM t WHERE i16 IN (1, t.i16)", kUnsupported, "t.i16",
                  "only literals, constant integer expressions, quotients of numbers and "
                  "subquery values that read no column are supported in an IN list"},
        // A call with the wrong number of arguments stays a bind error whatever its arguments
        // (CheckSupported does not look into them, and the binder reports the arity before it
        // binds one).
        ErrorCase{"SELECT strlen(t.s, 1) FROM t", kBind, "strlen(t.s, 1)",
                  "strlen() takes 1 argument, not 2"},
        ErrorCase{"SELECT strlen(MAX(t.s), 1) FROM t", kBind, "strlen(MAX(t.s), 1)",
                  "strlen() takes 1 argument, not 2"},
        ErrorCase{"SELECT COUNT(*) FROM t WHERE strlen(t.s, 1) > 1", kBind, "strlen(t.s, 1)",
                  "strlen() takes 1 argument, not 2"},
        // Query order.
        ErrorCase{"SELECT md5(s), t.i16 FROM t, u", kUnsupported, "md5",
                  "function md5() is not supported"},
        ErrorCase{"SELECT nope FROM t, u WHERE md5(s) = 'x'", kUnsupported, "md5",
                  "function md5() is not supported"},
        // The select list beats the connectivity check, and an alias names the binding in it.
        ErrorCase{"SELECT i16 FROM t AS a, u", kBind, "i16",
                  "column name 'i16' is ambiguous: it matches a.i16 and u.i16"},
        // An ON is checked with the FROM list, before WHERE: its unsupported function comes first.
        ErrorCase{"SELECT i16 FROM t JOIN u ON md5(t.s) = u.s WHERE md5(s) = 'x'", kUnsupported,
                  "md5", "function md5() is not supported"},
        ErrorCase{"SELECT i16 FROM t WHERE md5(s) = 'x' GROUP BY t.i16", kUnsupported, "md5",
                  "function md5() is not supported"}));

// Derived tables and WITH lists bind as sub-plans (ADR 0022's J4), so what is left here is what a
// sub-plan does not change: an unsupported construct anywhere in the statement is still reported
// before any name resolves, because CheckSupported walks every block the statement reaches; a
// missing table inside a sub-plan is that sub-plan's bind error; and a sub-plan's columns take part
// in rules 4 and 7 like any other binding's.
INSTANTIATE_TEST_SUITE_P(
    NestedQueries, BindErrorTest,
    ::testing::Values(
        ErrorCase{"SELECT md5(s) FROM (SELECT s FROM t)", kUnsupported, "md5",
                  "function md5() is not supported"},
        // Beside a sub-plan, and inside a block a CTE reaches: the walk finds both.
        ErrorCase{"SELECT nope FROM (SELECT i16 FROM t) WHERE md5(s) = 'x'", kUnsupported, "md5",
                  "function md5() is not supported"},
        ErrorCase{"WITH c AS (SELECT i16 FROM t) SELECT md5(s) FROM (SELECT s FROM c) AS a",
                  kUnsupported, "md5", "function md5() is not supported"},
        // A missing table inside a sub-plan is the sub-plan's bind error, at its own name.
        ErrorCase{"SELECT COUNT(*) FROM (SELECT x FROM nope) AS n(y)", kBind, "nope",
                  "table 'nope' does not exist"},
        ErrorCase{"SELECT * FROM (WITH c AS (SELECT a FROM nope) SELECT a FROM c) s", kBind, "nope",
                  "table 'nope' does not exist"},
        // An unreferenced CTE is never bound, as DuckDB never binds one, so the missing table in
        // its body is not the error -- the outer one is.
        ErrorCase{"with c(x) AS (SELECT a FROM nope) SELECT x FROM missing", kBind, "missing",
                  "table 'missing' does not exist"},
        // Rule 4: a sub-plan's column makes an unqualified name ambiguous like any other binding's.
        ErrorCase{"SELECT i16 FROM t, (SELECT i16 FROM u)", kBind, "i16",
                  "column name 'i16' is ambiguous"},
        ErrorCase{"SELECT i16 FROM t JOIN (SELECT i16 FROM u) v ON t.i16 = v.i16", kBind, "i16",
                  "column name 'i16' is ambiguous"},
        // Rule 7: more names than the block has columns is a bind error for a derived table.
        ErrorCase{"SELECT COUNT(*) FROM (SELECT i16 FROM t) AS g(a, b)", kBind, "(a, b)",
                  "table \"g\" has 1 columns available but 2 columns specified"},
        // A call with the wrong number of arguments is a bind error once the FROM list binds.
        ErrorCase{"SELECT strlen(s, 1) FROM (SELECT s FROM t)", kBind, "strlen(s, 1)",
                  "strlen() takes 1 argument, not 2"},
        // Rule 1's counter numbers the unaliased derived tables only, so an aliased one beside an
        // unaliased one leaves the latter `unnamed_subquery`, with no `unnamed_subquery2`.
        ErrorCase{"SELECT unnamed_subquery2.s FROM (SELECT i16 FROM t) AS g, (SELECT s FROM u) "
                  "WHERE g.i16 = u.i16",
                  kBind, "unnamed_subquery2.s", "no FROM item is named 'unnamed_subquery2'"}));

// What stays unsupported of UNION ALL (ADR 0036): its own ORDER BY, LIMIT and OFFSET (X5), and a
// subquery whose query it is (X6) -- statically, so over tables that do not exist too. Its tail is
// refused at its first clause, after any construct of its blocks, which come before it in the
// query; a subquery at its operator, once the conjunct's other side is checked, and before the
// checks that read a block's clauses, so a scalar subquery over it is not called a non-aggregate. A
// CTE that nothing references is never checked or bound, a UNION ALL in it, tail and all, too.
INSTANTIATE_TEST_SUITE_P(
    UnionAllRefusals, BindErrorTest,
    ::testing::Values(
        ErrorCase{"SELECT i16 FROM t UNION ALL SELECT i16 FROM u UNION ALL SELECT i16 FROM t "
                  "ORDER BY 1",
                  kUnsupported, "ORDER BY 1", "ORDER BY on a UNION ALL is not supported"},
        ErrorCase{"SELECT i16 FROM t UNION ALL SELECT i16 FROM u LIMIT 1", kUnsupported, "LIMIT 1",
                  "LIMIT on a UNION ALL is not supported"},
        ErrorCase{"SELECT i16 FROM t UNION ALL SELECT i16 FROM u OFFSET 2 LIMIT 1", kUnsupported,
                  "OFFSET 2", "OFFSET on a UNION ALL is not supported"},
        ErrorCase{"SELECT x FROM (SELECT i16 AS x FROM t UNION ALL SELECT i16 FROM u LIMIT 3) AS d",
                  kUnsupported, "LIMIT 3", "LIMIT on a UNION ALL is not supported"},
        ErrorCase{"WITH c AS (SELECT i16 FROM t UNION ALL SELECT i16 FROM u ORDER BY 1) SELECT i16 "
                  "FROM c",
                  kUnsupported, "ORDER BY 1", "ORDER BY on a UNION ALL is not supported"},
        ErrorCase{"SELECT a FROM nope UNION ALL SELECT b FROM nada ORDER BY 1", kUnsupported,
                  "ORDER BY 1", "ORDER BY on a UNION ALL is not supported"},
        ErrorCase{"SELECT i16 FROM t UNION ALL SELECT md5(s) FROM t ORDER BY 1", kUnsupported,
                  "md5", "function md5() is not supported"},
        // A scalar's, an EXISTS's and an IN's UNION ALL is answered where no arm reads an enclosing
        // column (ADR 0041's SQ4a and SQ7b); a correlated arm is SQ15's, refused at the subquery.
        ErrorCase{"SELECT i16 FROM t WHERE i16 IN (SELECT i16 FROM u WHERE u.i32 = t.i32 UNION ALL "
                  "SELECT i16 FROM t)",
                  kUnsupported, "IN", "a correlated subquery over a UNION ALL is not supported"},
        ErrorCase{"SELECT i16 FROM t WHERE i16 > (SELECT MAX(i16) FROM u WHERE u.i32 = t.i32 UNION "
                  "ALL SELECT MAX(i16) FROM t)",
                  kUnsupported, "(", "a correlated subquery over a UNION ALL is not supported"},
        ErrorCase{"SELECT i16 FROM t WHERE EXISTS (SELECT i16 FROM u WHERE u.i32 = t.i32 UNION ALL "
                  "SELECT i16 FROM t)",
                  kUnsupported, "EXISTS",
                  "a correlated subquery over a UNION ALL is not supported"},
        ErrorCase{"SELECT i16 FROM t WHERE NOT EXISTS (SELECT i16 FROM u UNION ALL SELECT i16 "
                  "FROM u WHERE u.i32 = t.i32)",
                  kUnsupported, "EXISTS",
                  "a correlated subquery over a UNION ALL is not supported"},
        ErrorCase{"SELECT i16 FROM t GROUP BY i16 HAVING COUNT(*) > (SELECT MAX(i16) FROM u WHERE "
                  "u.i16 = t.i16 UNION ALL SELECT MAX(i16) FROM t)",
                  kUnsupported, "(", "a correlated subquery over a UNION ALL is not supported"},
        ErrorCase{"SELECT i16 FROM t WHERE md5(s) IN (SELECT s FROM u UNION ALL SELECT s FROM t)",
                  kUnsupported, "md5", "function md5() is not supported"},
        ErrorCase{"SELECT i16 FROM t WHERE i16 IN (SELECT md5(s) FROM u UNION ALL SELECT s FROM "
                  "t)",
                  kUnsupported, "md5", "function md5() is not supported"},
        ErrorCase{"WITH c AS (SELECT a FROM nope UNION ALL SELECT b FROM nada ORDER BY 1) SELECT x "
                  "FROM missing",
                  kBind, "missing", "table 'missing' does not exist"}));

// Every set operator is answered (ADR 0039's X4b to X4d); what stays refused is a compound's own
// tail, which X5 keeps at any depth -- a parenthesized compound's own tail too (ADR 0039's X4a
// update) -- inside an IN's set too -- and a compound with a correlated arm under a scalar, an
// EXISTS or an IN (SQ15). Each names the compound by its root operator.
INSTANTIATE_TEST_SUITE_P(
    SetOperatorRefusals, BindErrorTest,
    ::testing::Values(
        ErrorCase{"SELECT i16 FROM t UNION SELECT i16 FROM u ORDER BY 1", kUnsupported,
                  "ORDER BY 1", "ORDER BY on a UNION is not supported"},
        ErrorCase{"SELECT i16 FROM t UNION DISTINCT SELECT i16 FROM u LIMIT 2", kUnsupported,
                  "LIMIT 2", "LIMIT on a UNION is not supported"},
        ErrorCase{"SELECT i16 FROM t UNION SELECT i16 FROM u UNION ALL SELECT i16 FROM t OFFSET 1",
                  kUnsupported, "OFFSET 1", "OFFSET on a UNION ALL is not supported"},
        ErrorCase{"SELECT i16 FROM t WHERE i16 IN (SELECT i16 FROM u UNION SELECT i16 FROM t ORDER "
                  "BY 1)",
                  kUnsupported, "ORDER BY 1", "ORDER BY on a UNION is not supported"},
        ErrorCase{"SELECT i16 FROM t WHERE EXISTS (SELECT i16 FROM u WHERE u.i16 = t.i16 EXCEPT "
                  "SELECT i16 FROM t)",
                  kUnsupported, "EXISTS", "a correlated subquery over an EXCEPT is not supported"},
        ErrorCase{"SELECT i16 FROM t INTERSECT SELECT i16 FROM u LIMIT 1", kUnsupported, "LIMIT 1",
                  "LIMIT on an INTERSECT is not supported"},
        ErrorCase{"SELECT i16 FROM t WHERE i16 IN (SELECT i16 FROM u INTERSECT ALL SELECT i16 "
                  "FROM ok WHERE ok.i16 = t.i32)",
                  kUnsupported, "IN",
                  "a correlated subquery over an INTERSECT ALL is not supported"},
        ErrorCase{"SELECT i16 FROM t EXCEPT ALL SELECT i16 FROM u OFFSET 3", kUnsupported,
                  "OFFSET 3", "OFFSET on an EXCEPT ALL is not supported"},
        ErrorCase{"SELECT i16 FROM t UNION ALL SELECT i16 FROM u EXCEPT SELECT i16 FROM t "
                  "INTERSECT SELECT i16 FROM u ORDER BY 1",
                  kUnsupported, "ORDER BY 1", "ORDER BY on an EXCEPT is not supported"},
        ErrorCase{"SELECT i16 FROM t UNION ALL (SELECT i16 FROM u EXCEPT ALL SELECT i16 FROM t "
                  "LIMIT 2)",
                  kUnsupported, "LIMIT 2", "LIMIT on an EXCEPT ALL is not supported"},
        ErrorCase{"SELECT i16 FROM t UNION ALL (SELECT i16 FROM u UNION ALL SELECT i16 FROM t "
                  "LIMIT 1)",
                  kUnsupported, "LIMIT 1", "LIMIT on a UNION ALL is not supported"},
        ErrorCase{"(SELECT i16 FROM t UNION ALL SELECT i16 FROM u) ORDER BY 1", kUnsupported,
                  "ORDER BY 1", "ORDER BY on a UNION ALL is not supported"}));

// A UNION ALL's binding (ADR 0036), as in DuckDB: every block binds before the column counts are
// compared, and those before the types; a count that differs is a bind error at the UNION ALL
// before the block. A block binds against the scope the UNION ALL has -- a name that only an
// enclosing FROM item has is a lateral reference -- and never sees another block's names.
INSTANTIATE_TEST_SUITE_P(
    UnionAllBinding, BindErrorTest,
    ::testing::Values(
        ErrorCase{"SELECT a FROM nope UNION ALL SELECT b FROM nada", kBind, "nope",
                  "table 'nope' does not exist"},
        ErrorCase{"SELECT i16 FROM t UNION ALL SELECT i16, i32 FROM u", kBind, "UNION ALL",
                  "Set operations can only apply to expressions with the same number of result "
                  "columns"},
        ErrorCase{"SELECT i16 FROM t UNION ALL SELECT i16, i32 FROM u UNION ALL SELECT nope FROM t",
                  kBind, "nope", "nope"},
        ErrorCase{"SELECT i16 FROM t UNION ALL SELECT s, i32 FROM u", kBind, "UNION ALL",
                  "Set operations can only apply to expressions with the same number of result "
                  "columns"},
        ErrorCase{"SELECT i32 FROM t UNION ALL SELECT i32 FROM ok", kBind, "i32", "i32"},
        ErrorCase{"SELECT d.i16 FROM t AS a, (SELECT i16 FROM u WHERE u.i32 = a.i32 UNION ALL "
                  "SELECT i16 FROM u) AS d",
                  kUnsupported, "a.i32",
                  "a lateral qualified column of an enclosing query block is not supported"}));

// A UNION ALL's types (ADR 0036): a left fold in written order over each block's own type, which
// has to widen exactly to the column's type; where it cannot, exit code 4 at the first select item
// in the query that fails -- a later block's at the fold, any block's at the widening -- or at the
// `*` it was expanded from.
INSTANTIATE_TEST_SUITE_P(
    UnionAllTypes, BindErrorTest,
    ::testing::Values(
        ErrorCase{"SELECT i32 FROM t UNION ALL SELECT s FROM t", kUnsupported, "s",
                  "UNION ALL of INTEGER and VARCHAR values is not supported"},
        ErrorCase{"SELECT i64 FROM t UNION ALL SELECT d FROM t", kUnsupported, "d",
                  "UNION ALL of BIGINT and DOUBLE values is not supported"},
        ErrorCase{"SELECT dt FROM t UNION ALL SELECT toDateTime(i64) FROM t", kUnsupported,
                  "toDateTime(i64)", "UNION ALL of DATE and TIMESTAMP values is not supported"},
        ErrorCase{"SELECT 1 FROM t UNION ALL SELECT '7' FROM t", kUnsupported, "'7'",
                  "UNION ALL of INTEGER and VARCHAR values is not supported"},
        ErrorCase{"SELECT dt FROM t UNION ALL SELECT '2020-01-01' FROM t", kUnsupported,
                  "'2020-01-01'", "UNION ALL of DATE and VARCHAR values is not supported"},
        ErrorCase{"SELECT SUM(i32) FROM t UNION ALL SELECT p FROM dec", kUnsupported, "SUM(i32)",
                  "UNION ALL of DECIMAL(38,2) and HUGEINT values is not supported (HUGEINT does "
                  "not widen exactly to DECIMAL(38,2))"},
        ErrorCase{"SELECT h FROM t UNION ALL SELECT p FROM dec", kUnsupported, "p",
                  "UNION ALL of DECIMAL(38,0) and DECIMAL(15,2) values is not supported "
                  "(DECIMAL(15,2) does not widen exactly to DECIMAL(38,0))"},
        ErrorCase{"SELECT i32, i16 FROM t UNION ALL SELECT * FROM ok", kUnsupported, "*",
                  "UNION ALL of SMALLINT and VARCHAR values is not supported"},
        ErrorCase{"SELECT i32, i16 FROM t UNION ALL SELECT s, dt FROM t", kUnsupported, "s",
                  "UNION ALL of INTEGER and VARCHAR values is not supported"},
        ErrorCase{"SELECT SUM(i32), MIN(i32) FROM t UNION ALL SELECT p, 'x' FROM dec", kUnsupported,
                  "SUM(i32)", "HUGEINT does not widen exactly to DECIMAL(38,2)"}));

// The walks reach a UNION ALL's last block (ADR 0036's matrix): each construct stands only in the
// last of three, where a walk that skipped the blocks would miss it -- a refused call, also in a
// derived table, a CTE body or a subquery that only the last block reads, a name that does not
// resolve, a type that does not fold and a column count that differs.
INSTANTIATE_TEST_SUITE_P(
    UnionAllLastBlock, BindErrorTest,
    ::testing::Values(
        ErrorCase{"SELECT i16 FROM t UNION ALL SELECT i16 FROM u UNION ALL SELECT nope FROM t",
                  kBind, "nope", "nope"},
        ErrorCase{"SELECT i16 FROM t UNION ALL SELECT i16 FROM u UNION ALL SELECT s FROM t",
                  kUnsupported, "s", "UNION ALL of SMALLINT and VARCHAR values is not supported"},
        ErrorCase{"SELECT i16 FROM t UNION ALL SELECT i16 FROM u UNION ALL SELECT i16, i32 FROM t",
                  kBind, "UNION ALL",
                  "Set operations can only apply to expressions with the same number of result "
                  "columns"},
        ErrorCase{"SELECT i16 FROM t UNION ALL SELECT i16 FROM u UNION ALL SELECT md5(s) FROM t",
                  kUnsupported, "md5", "function md5() is not supported"},
        ErrorCase{"SELECT i16 FROM t UNION ALL SELECT i16 FROM u UNION ALL SELECT x FROM (SELECT "
                  "md5(s) AS x FROM t) AS d",
                  kUnsupported, "md5", "function md5() is not supported"},
        ErrorCase{"WITH c AS (SELECT md5(s) AS x FROM t) SELECT i16 FROM t UNION ALL SELECT i16 "
                  "FROM u UNION ALL SELECT x FROM c",
                  kUnsupported, "md5", "function md5() is not supported"},
        ErrorCase{"SELECT i16 FROM t UNION ALL SELECT i16 FROM u UNION ALL SELECT i16 FROM t "
                  "WHERE i16 IN (SELECT md5(s) FROM u)",
                  kUnsupported, "md5", "function md5() is not supported"},
        ErrorCase{"SELECT i16 FROM t UNION ALL SELECT i16 FROM u UNION ALL SELECT x FROM (SELECT "
                  "i16 AS x FROM t UNION ALL SELECT md5(s) FROM t) AS d",
                  kUnsupported, "md5", "function md5() is not supported"}));

// Subqueries in expressions (ADR 0022): the grammar parses all three forms from roadmap PR S4b on.
// Roadmap PR J5a answers IN and NOT IN as a top-level conjunct of WHERE, of an inner join's ON or
// of HAVING, so what is left here is the scalar form (J5b), EXISTS (ADR 0023's U-series), and what
// an answered IN still refuses: a key no type holds, a column count other than one, and a
// correlation.
INSTANTIATE_TEST_SUITE_P(
    Subqueries, BindErrorTest,
    ::testing::Values(
        // A value is answered in every clause of a block (ADR 0041's SQ2) but as an aggregate's
        // whole argument, which waits for constant aggregate arguments.
        ErrorCase{"SELECT SUM((SELECT COUNT(*) FROM u)) FROM t", kUnsupported,
                  "(SELECT COUNT(*) FROM u)",
                  "an aggregate of subquery values alone is not supported"},
        // An uncorrelated scalar subquery of any shape is its single value (ADR 0041's SQ4a). A
        // correlated one is ADR 0023's Path 2 only over an aggregate over every row; any other
        // shape needs the single join (SQ4b).
        ErrorCase{"SELECT i16 FROM t WHERE i32 = (SELECT i16 FROM u WHERE u.i64 = t.i64)",
                  kUnsupported, "(", "other than an aggregate over every row"},
        ErrorCase{"SELECT i16 FROM t WHERE i32 = (SELECT COUNT(*) FROM u WHERE u.i64 = t.i64 "
                  "LIMIT 1)",
                  kUnsupported, "(", "other than an aggregate over every row"},
        ErrorCase{"SELECT i16 FROM t WHERE i32 = (SELECT COUNT(*) FROM u WHERE u.i64 = t.i64 "
                  "OFFSET 1)",
                  kUnsupported, "(", "other than an aggregate over every row"},
        ErrorCase{"SELECT i16 FROM t WHERE i32 = (SELECT COUNT(*) FROM u WHERE u.i64 = t.i64 "
                  "GROUP BY i16)",
                  kUnsupported, "(", "other than an aggregate over every row"},
        ErrorCase{"SELECT i16 FROM t WHERE i32 = (SELECT COUNT(*) FROM u WHERE u.i64 = t.i64 "
                  "HAVING COUNT(*) > 1)",
                  kUnsupported, "(", "other than an aggregate over every row"},
        // The other side is checked as any comparison's is: an operand, a literal, or a constant
        // integer expression.
        ErrorCase{"SELECT i16 FROM t WHERE 1 + 2.5 = (SELECT COUNT(*) FROM u)", kUnsupported,
                  "1 + 2.5", "a constant expression in a comparison is not supported"},
        // A constant integer expression that overflows is a bind error (divergence D17), as it is
        // without a subquery: the fold runs, and the comparison's own side has to survive the copy
        // it makes (sql::Box deep-copies, so the subquery moves).
        ErrorCase{"SELECT i16 FROM t WHERE 2147483647 + 1 = (SELECT COUNT(*) FROM u)", kBind,
                  "2147483647 + 1", "Overflow in addition of INTEGER"},
        ErrorCase{"SELECT i16 FROM t WHERE i32 = (SELECT COUNT(*) FROM u, v)", kBind, "v",
                  "table 'v' does not exist"},
        // One column, as in DuckDB, which reports a bind error for two, whatever the shape, a
        // correlated one's too: the column count is checked first.
        ErrorCase{"SELECT i16 FROM t WHERE i32 = (SELECT i16, i32 FROM u)", kBind, "(",
                  "a subquery of a comparison must have one column, not 2"},
        ErrorCase{"SELECT i16 FROM t WHERE i32 = (SELECT i16, i32 FROM u WHERE u.i64 = t.i64)",
                  kBind, "(", "a subquery of a comparison must have one column, not 2"},
        // A value inside an expression is no comparison's operand (ADR 0041's SQ2a message).
        ErrorCase{"SELECT i16 FROM t WHERE i32 > 1 + (SELECT i16, i32 FROM u)", kBind, "(",
                  "a scalar subquery must have one column, not 2"},
        ErrorCase{"SELECT i16 FROM t GROUP BY i16 HAVING COALESCE((SELECT i16, i32 FROM u), 1) > 0",
                  kBind, "(", "a scalar subquery must have one column, not 2"},
        // A scalar subquery's refusals that depend on its shape come once its block is bound: the
        // enclosing select list's and the block's own bind errors come first, as in DuckDB, which
        // answers the subquery.
        ErrorCase{"SELECT nope FROM t WHERE i32 = (SELECT i16 FROM u WHERE u.i64 = t.i64)", kBind,
                  "nope", "column 'nope' does not exist"},
        ErrorCase{"SELECT i16 FROM t WHERE i32 = (SELECT nope FROM u WHERE u.i64 = t.i64 GROUP BY "
                  "i16)",
                  kBind, "nope", "column 'nope' does not exist"},
        ErrorCase{"SELECT i16 FROM t WHERE i32 = (SELECT MIN(i16), MAX(i32) FROM u)", kBind, "(",
                  "a subquery of a comparison must have one column, not 2"},
        // A top-level conjunct is the one placement a correlated EXISTS is answered in (ADR 0023's
        // U2), refused elsewhere once its block binds; an uncorrelated one is answered under OR,
        // NOT and CASE too (ADR 0041's SQ1). Outside a condition every EXISTS keeps the static
        // refusal, before any name resolves.
        ErrorCase{"SELECT EXISTS (SELECT i16 FROM u) FROM t", kUnsupported, "EXISTS",
                  "EXISTS (subqueries) is not supported here"},
        ErrorCase{"SELECT i16 FROM t WHERE i32 = 1 OR EXISTS (SELECT u.i16 FROM u WHERE "
                  "u.i32 = t.i32)",
                  kUnsupported, "EXISTS",
                  "a correlated EXISTS is supported only as a top-level conjunct of WHERE or an "
                  "inner ON"},
        ErrorCase{"SELECT i16 FROM t WHERE (EXISTS (SELECT i16 FROM u)) IS NOT NULL", kUnsupported,
                  "EXISTS", "EXISTS (subqueries) is not supported here"},
        // A correlated value elsewhere than a direct operand of a top-level comparison waits for
        // ADR 0041's SQ5, refused once its block binds.
        ErrorCase{"SELECT i16 FROM t WHERE i32 = 1 OR i32 > (SELECT MIN(u.i16) FROM u WHERE "
                  "u.i64 = t.i64)",
                  kUnsupported, "(",
                  "a correlated scalar subquery is supported only as a direct operand of a "
                  "comparison"},
        ErrorCase{"SELECT i16 FROM t WHERE i32 > (SELECT MIN(u.i16) FROM u WHERE u.i64 = t.i64) "
                  "+ 1",
                  kUnsupported, "(",
                  "a correlated scalar subquery is supported only as a direct operand of a "
                  "comparison"},
        ErrorCase{"SELECT i16 FROM t ORDER BY EXISTS (SELECT i16 FROM u)", kUnsupported, "EXISTS",
                  "EXISTS (subqueries) is not supported here"},
        // Path 1 takes a plain block only: every clause that would have to act per binding is
        // refused at the EXISTS before any clause of its block binds (the binder's gate), even one
        // that would be a bind error; an uncorrelated EXISTS reads any block.
        ErrorCase{"SELECT i16 FROM t WHERE EXISTS (SELECT u.i32 FROM u WHERE u.i32 = t.i32 "
                  "GROUP BY u.i16)",
                  kUnsupported, "EXISTS", "EXISTS over a subquery with GROUP BY is not supported"},
        ErrorCase{"SELECT nope FROM t WHERE EXISTS (SELECT u.i16 FROM u WHERE u.i32 = t.i32 "
                  "GROUP BY u.i16)",
                  kBind, "nope", "column 'nope' does not exist"},
        ErrorCase{"SELECT i16 FROM t WHERE EXISTS (SELECT u.i16 FROM u WHERE u.i32 = t.i32 "
                  "GROUP BY u.i16)",
                  kUnsupported, "EXISTS", "EXISTS over a subquery with GROUP BY is not supported"},
        ErrorCase{"SELECT i16 FROM t WHERE EXISTS (SELECT u.i16 FROM u WHERE u.i32 = t.i32 "
                  "LIMIT 1)",
                  kUnsupported, "EXISTS", "EXISTS over a subquery with LIMIT is not supported"},
        ErrorCase{"SELECT i16 FROM t WHERE EXISTS (SELECT MIN(u.i16) FROM u WHERE u.i32 = t.i32)",
                  kUnsupported, "EXISTS",
                  "EXISTS over a subquery with an aggregate is not supported"},
        ErrorCase{"SELECT i32, COUNT(*) FROM t GROUP BY i32 HAVING EXISTS "
                  "(SELECT u.i16 FROM u WHERE u.i32 = t.i32)",
                  kUnsupported, "EXISTS", "EXISTS in HAVING is not supported"},
        // The gate comes before the block's own bind errors, which binding it would meet first...
        ErrorCase{"SELECT i16 FROM t WHERE EXISTS (SELECT nope FROM u WHERE u.i32 = t.i32 "
                  "GROUP BY u.i16)",
                  kUnsupported, "EXISTS", "EXISTS over a subquery with GROUP BY is not supported"},
        // ... and after the enclosing block's grouping errors, which come before its HAVING binds,
        // as DuckDB, which answers the EXISTS, reports them.
        ErrorCase{"SELECT i16, COUNT(*) FROM t GROUP BY i32 HAVING EXISTS "
                  "(SELECT u.i16 FROM u WHERE u.i32 = t.i32)",
                  kBind, "i16", "column 'i16' must appear in the GROUP BY clause"},
        // An uncorrelated EXISTS reads any block (ADR 0041's SQ4a), and the stars of its select
        // list are checked, not expanded: a grouped or aggregate block's star is DuckDB's bind
        // error, for the star's first ungrouped column, in select order.
        ErrorCase{"SELECT i16 FROM t WHERE EXISTS (SELECT * FROM u GROUP BY i16)", kBind, "*",
                  "column 'i32' must appear in the GROUP BY clause"},
        ErrorCase{"SELECT i16 FROM t WHERE EXISTS (SELECT ok.* FROM ok GROUP BY ok.i16)", kBind,
                  "ok.*", "column 's' must appear in the GROUP BY clause"},
        ErrorCase{"SELECT i16 FROM t WHERE EXISTS (SELECT s, * FROM ok GROUP BY i16)", kBind, "s",
                  "column 's' must appear in the GROUP BY clause"},
        ErrorCase{"SELECT i16 FROM t WHERE EXISTS (SELECT *, s FROM ok GROUP BY i16)", kBind, "*",
                  "column 's' must appear in the GROUP BY clause"},
        ErrorCase{"SELECT i16 FROM t WHERE EXISTS (SELECT *, COUNT(*) FROM u)", kBind, "*",
                  "column 'i16' must be inside an aggregate function"},
        // A GROUP BY position that falls in such a star exits 4, as an ORDER BY one does.
        ErrorCase{"SELECT i16 FROM t WHERE EXISTS (SELECT * FROM ok GROUP BY 1, 2)", kUnsupported,
                  "1", "GROUP BY position 1 over a '*' of an EXISTS subquery is not supported"},
        // What binds before the gate: the EXISTS block's FROM list, and the enclosing conjuncts
        // written before the EXISTS; a later conjunct's bind error comes after its refusal.
        ErrorCase{
            "SELECT i16 FROM t WHERE EXISTS (SELECT 1 FROM nope WHERE nope.x = t.i16 LIMIT 1)",
            kBind, "nope", "table 'nope' does not exist"},
        ErrorCase{"SELECT i16 FROM t WHERE nope = 1 AND EXISTS (SELECT 1 FROM u WHERE u.i16 = "
                  "t.i16 LIMIT 1)",
                  kBind, "nope", "column 'nope' does not exist"},
        ErrorCase{"SELECT i16 FROM t WHERE EXISTS (SELECT 1 FROM u WHERE u.i16 = t.i16 LIMIT 1) "
                  "AND nope = 1",
                  kUnsupported, "EXISTS", "EXISTS over a subquery with LIMIT is not supported"},
        // A DOUBLE has no join key, by decision (ADR 0022), and a DECIMAL(38,0) with a
        // DECIMAL(38,10) would need 48 digits: a semi join needs one type for its key, so neither
        // exits past 4.
        ErrorCase{"SELECT i16 FROM t WHERE d IN (SELECT d FROM u)", kUnsupported, "IN",
                  "IN (subquery) over a DOUBLE is not supported"},
        ErrorCase{"SELECT i16 FROM t WHERE h IN (SELECT z FROM dec)", kUnsupported, "IN",
                  "IN (subquery) of DECIMAL(38,0) and DECIMAL(38,10) is not supported"},
        // DuckDB's bind error for a VARCHAR or a DATE against another family (ADR 0041's SQ7b).
        ErrorCase{"SELECT i16 FROM t WHERE s IN (SELECT dt FROM u)", kBind, "IN",
                  "cannot compare values of type VARCHAR and DATE in IN (subquery)"},
        ErrorCase{"SELECT i16 FROM t WHERE i16 < 0 OR dt NOT IN (SELECT i32 FROM u)", kBind,
                  "NOT IN", "cannot compare values of type DATE and INTEGER in IN (subquery)"},
        ErrorCase{"SELECT i16 FROM t WHERE i32 = ANY (SELECT s FROM u)", kBind, "= ANY",
                  "cannot compare values of type INTEGER and VARCHAR in IN (subquery)"},
        ErrorCase{"SELECT i16 FROM t WHERE i32 IN (SELECT date_trunc('month', dt) FROM u)", kBind,
                  "IN", "cannot compare values of type INTEGER and TIMESTAMP in IN (subquery)"},
        // A DATE against a TIMESTAMP, which DuckDB answers, has no join key yet.
        ErrorCase{"SELECT i16 FROM t WHERE dt IN (SELECT date_trunc('month', dt) FROM u)",
                  kUnsupported, "IN", "IN (subquery) of DATE and TIMESTAMP is not supported"},
        // Under an OR branch that OR factoring drops: bound for its errors, the type's among them.
        ErrorCase{"SELECT i16 FROM t WHERE (i16 = 2 AND dt IN (SELECT i32 FROM u)) OR i16 = 2",
                  kBind, "IN", "cannot compare values of type DATE and INTEGER in IN (subquery)"},
        // A string literal probe against a number set is divergence D3's bind error.
        ErrorCase{"SELECT i16 FROM t WHERE '1' IN (SELECT i32 FROM u)", kBind, "'1'",
                  "cannot compare the INTEGER values of an IN subquery with a string"},
        // One column, as in DuckDB, which reports a bind error for two.
        ErrorCase{"SELECT i16 FROM t WHERE i32 IN (SELECT i16, i32 FROM u)", kBind, "IN",
                  "a subquery of IN must have one column, not 2"},
        // A correlated IN is refused by its shape, at the IN, not at the reference: its NULL
        // semantics need the set's NULL and empty flags for each binding (ADR 0023).
        ErrorCase{"SELECT i16 FROM t WHERE i32 IN (SELECT i16 FROM u WHERE i64 = t.i64)",
                  kUnsupported, "IN", "a correlated subquery of IN is not supported"},
        ErrorCase{"SELECT i16 FROM t WHERE i32 NOT IN (SELECT i16 FROM u WHERE i64 = t.i64)",
                  kUnsupported, "NOT IN", "a correlated subquery of NOT IN is not supported"},
        ErrorCase{"SELECT i16 FROM t WHERE i32 IN (SELECT i16 FROM ok WHERE i16 = i64)",
                  kUnsupported, "IN", "a correlated subquery of IN is not supported"},
        // ADR 0023 answers an outer reference only in a WHERE conjunct of its own block, an inner
        // join's ON included, and only one block out. Every other placement exits 4 at the
        // reference, with the rule it broke, and never reports a missing column.
        ErrorCase{"SELECT i16 FROM t WHERE i32 IN (SELECT t.i64 FROM u)", kUnsupported, "t.i64",
                  "a qualified column of an enclosing query block is supported only in WHERE"},
        ErrorCase{"SELECT i16 FROM t WHERE i32 IN (SELECT i16 FROM u GROUP BY i16, t.i64)",
                  kUnsupported, "t.i64",
                  "a qualified column of an enclosing query block is supported only in WHERE"},
        ErrorCase{"SELECT i16 FROM t WHERE i32 IN (SELECT i16 FROM u ORDER BY t.i64)", kUnsupported,
                  "t.i64",
                  "a qualified column of an enclosing query block is supported only in WHERE"},
        ErrorCase{"SELECT i16 FROM t WHERE i32 IN (SELECT i16 FROM u GROUP BY i16 HAVING "
                  "MIN(i16) > t.i64)",
                  kUnsupported, "t.i64",
                  "a qualified column of an enclosing query block is supported only in WHERE"},
        ErrorCase{"SELECT i16 FROM t WHERE i32 IN (SELECT u.i16 FROM u LEFT JOIN ok ON "
                  "u.i16 = ok.i16 AND ok.i16 = t.i64)",
                  kUnsupported, "t.i64",
                  "a qualified column of an enclosing query block is supported only in WHERE"},
        // An inner join's ON is a WHERE conjunct (rule 10), so the reference is answered and the
        // refusal is the correlated IN's, at the IN.
        ErrorCase{"SELECT i16 FROM t WHERE i32 IN (SELECT u.i16 FROM u JOIN ok ON "
                  "u.i16 = ok.i16 AND ok.i16 = t.i64)",
                  kUnsupported, "IN", "a correlated subquery of IN is not supported"},
        // Two blocks out: the subquery may name only the block that directly encloses it.
        ErrorCase{"SELECT i16 FROM t WHERE i32 IN (SELECT i16 FROM u WHERE i32 IN "
                  "(SELECT i16 FROM ok WHERE i16 = t.i64))",
                  kUnsupported, "t.i64",
                  "a qualified column of a query block further out than the enclosing one"},
        // Two shapes that aborted the process until the binder refused them (ADR 0023). Both
        // became reachable when U1 made an outer reference resolve instead of refusing at once.
        // A correlated subquery in HAVING: the clause gate cannot catch it, since the reference
        // sits in the WHERE of the subquery's own block while this block's HAVING holds the
        // subquery, and the dependent join would be stacked above the aggregation, whose output
        // does not define the free variable.
        ErrorCase{"SELECT i32 FROM t GROUP BY i32 HAVING MIN(i16) = "
                  "(SELECT MIN(u.i16) FROM u WHERE u.i32 = t.i32)",
                  kUnsupported, "(", "a correlated subquery in HAVING is not supported"},
        // ADR 0041's decision 23: an aggregate in a WHERE or ON inside a subquery that a HAVING
        // encloses belongs to the nearest block whose columns it reads. The enclosing block's,
        // through its HAVING, is an outer aggregate (SQ13); one of the subquery's own columns, of
        // no column, a mix, or one owned through a WHERE nearer in, is DuckDB's bind error.
        ErrorCase{"SELECT i16 FROM t GROUP BY i16 HAVING (SELECT COUNT(*) FROM u WHERE u.i32 < "
                  "SUM(t.i32)) > 3",
                  kUnsupported, "SUM(t.i32)",
                  "an aggregate of an enclosing query block's columns inside a subquery is not "
                  "supported"},
        ErrorCase{"SELECT i16 FROM t GROUP BY i16 HAVING (SELECT COUNT(*) FROM u WHERE u.i32 < "
                  "(SELECT MAX(ok.i16) FROM ok WHERE ok.i16 > SUM(t.i32))) > 3",
                  kUnsupported, "SUM(t.i32)",
                  "an aggregate of an enclosing query block's columns inside a subquery is not "
                  "supported"},
        ErrorCase{"SELECT i16 FROM t GROUP BY i16 HAVING (SELECT COUNT(*) FROM u WHERE u.i32 < "
                  "SUM(u.i32)) > 3",
                  kBind, "SUM(u.i32)", "WHERE clause cannot contain aggregates!"},
        ErrorCase{"SELECT i16 FROM t GROUP BY i16 HAVING (SELECT COUNT(*) FROM u WHERE u.i32 < "
                  "SUM(u.i32 + t.i32)) > 3",
                  kBind, "SUM(u.i32 + t.i32)", "WHERE clause cannot contain aggregates!"},
        ErrorCase{"SELECT i16 FROM t GROUP BY i16 HAVING (SELECT COUNT(*) FROM u WHERE u.i32 < "
                  "COUNT(*)) > 3",
                  kBind, "COUNT(*)", "WHERE clause cannot contain aggregates!"},
        ErrorCase{"SELECT i16 FROM t GROUP BY i16 HAVING COUNT(*) > (SELECT COUNT(*) FROM u WHERE "
                  "SUM(u.i32) IN (SELECT ok.i16 FROM ok))",
                  kBind, "SUM(u.i32)", "WHERE clause cannot contain aggregates!"},
        ErrorCase{"SELECT i16 FROM t GROUP BY i16 HAVING COUNT(*) > (SELECT COUNT(*) FROM u WHERE "
                  "SUM(u.i32 + t.i32) IN (SELECT ok.i16 FROM ok))",
                  kBind, "SUM(u.i32 + t.i32)", "WHERE clause cannot contain aggregates!"},
        ErrorCase{"SELECT i16 FROM t GROUP BY i16 HAVING COUNT(*) > (SELECT COUNT(*) FROM u WHERE "
                  "SUM(t.i32) IN (SELECT ok.i16 FROM ok))",
                  kUnsupported, "SUM(t.i32)",
                  "an aggregate of an enclosing query block's columns inside a subquery is not "
                  "supported"},
        ErrorCase{"SELECT i16 FROM t GROUP BY i16 HAVING COUNT(*) > (SELECT COUNT(*) FROM u WHERE "
                  "u.i32 < (SELECT MAX(ok.i16) FROM ok WHERE ok.i16 > SUM(u.i32)))",
                  kBind, "SUM(u.i32)", "WHERE clause cannot contain aggregates!"},
        // SQ2b: under a subquery in the select list or ORDER BY too, the owner's place deciding:
        // its select list or ORDER BY an outer aggregate, its GROUP BY or an aggregate's argument
        // DuckDB's bind error of each.
        ErrorCase{"SELECT (SELECT COUNT(*) FROM u WHERE u.i32 < MAX(t.i32)) FROM t", kUnsupported,
                  "MAX(t.i32)",
                  "an aggregate of an enclosing query block's columns inside a subquery is not "
                  "supported"},
        ErrorCase{"SELECT i16 FROM t GROUP BY i16 ORDER BY (SELECT COUNT(*) FROM u WHERE u.i32 < "
                  "MAX(t.i32))",
                  kUnsupported, "MAX(t.i32)",
                  "an aggregate of an enclosing query block's columns inside a subquery is not "
                  "supported"},
        ErrorCase{"SELECT (SELECT COUNT(*) FROM u WHERE u.i32 < MAX(t.i32)) AS n FROM t GROUP BY n",
                  kBind, "MAX(t.i32)", "GROUP BY clause cannot contain aggregates!"},
        ErrorCase{"SELECT i16, (SELECT SUM(u.i32 * (SELECT COUNT(*) FROM ok WHERE ok.i16 < "
                  "MAX(u.i16))) FROM u) FROM t",
                  kBind, "MAX(u.i16)", "aggregate function calls cannot be nested"},
        ErrorCase{"SELECT i16, (SELECT MIN(u.i32 * (SELECT COUNT(*) FROM ok WHERE ok.i16 < "
                  "MAX(u.i16))) FROM u) FROM t",
                  kBind, "MAX(u.i16)", "aggregate function calls cannot be nested"},
        ErrorCase{"SELECT (SELECT COUNT(*) FROM u WHERE u.i32 < SUM(1)) FROM t", kBind, "SUM(1)",
                  "WHERE clause cannot contain aggregates!"},
        // An IN conjunct whose probe reads an enclosing block's column: its semi join would stand
        // above this block's joins with the probe's free variable on its outer side, which neither
        // unnesting path of ADR 0023 takes (a value in such a conjunct is a residual: ADR 0041's
        // decision 8, ValuePlacements).
        ErrorCase{"SELECT i16 FROM t WHERE i32 = (SELECT MIN(u.i16) FROM u WHERE "
                  "t.i64 IN (SELECT ok.i16 FROM ok))",
                  kUnsupported, "IN",
                  "an IN subquery whose probe reads a column of an enclosing query block"},
        // A correlated subquery in a conjunct that reads the enclosing block itself (decision 8
        // answers an uncorrelated value there), two correlated subqueries compared (SQ5), and a
        // correlated value inside a FULL JOIN's left input, each named for what refuses it.
        ErrorCase{"SELECT i16 FROM t WHERE EXISTS (SELECT 1 FROM u WHERE u.i16 = t.i16 AND t.i32 > "
                  "(SELECT MIN(ok.i16) FROM ok WHERE ok.i16 = u.i16))",
                  kUnsupported, "(",
                  "a correlated subquery in a conjunct that also reads a column of an enclosing "
                  "query block is not supported"},
        ErrorCase{"SELECT i16 FROM t WHERE (SELECT MIN(u.i16) FROM u WHERE u.i64 = t.i64) < "
                  "(SELECT MAX(ok.i16) FROM ok WHERE ok.i16 = t.i16)",
                  kUnsupported, "(",
                  "a comparison of two correlated scalar subqueries is not supported"},
        ErrorCase{"SELECT t.i16 FROM t JOIN u ON t.i16 = u.i16 AND u.i32 > (SELECT MIN(dec.i) FROM "
                  "dec WHERE dec.i = u.i32) + 1 FULL JOIN ok ON u.i16 = ok.i16",
                  kUnsupported, "(",
                  "a correlated subquery in the ON of a join inside a FULL JOIN's left input is "
                  "not supported"},
        // The same for a correlated subquery compared directly with a value there, and the second
        // of two subqueries compared binds before the pair is refused, so its bind error wins, as
        // in DuckDB.
        ErrorCase{"SELECT t.i16 FROM t JOIN u ON t.i16 = u.i16 AND (SELECT MIN(ok.i16) FROM ok) < "
                  "(SELECT MIN(dec.i) FROM dec WHERE dec.i = u.i32) FULL JOIN ok ON u.i16 = ok.i16",
                  kUnsupported, "(",
                  "a correlated subquery in the ON of a join inside a FULL JOIN's left input is "
                  "not supported"},
        ErrorCase{"SELECT i16 FROM t WHERE (SELECT MIN(u.i16) FROM u WHERE u.i64 = t.i64) < "
                  "(SELECT MAX(nope) FROM ok WHERE ok.i16 = t.i16)",
                  kBind, "nope", "column 'nope' does not exist"},
        // A name this block has twice keeps its own bind error: an enclosing block that happens to
        // have it must not turn an ambiguous name into a refusal (nor, once a correlated subquery
        // binds, make it read the outer column).
        ErrorCase{"SELECT i16 FROM t WHERE i32 IN (SELECT i16 FROM u, ok)", kBind, "i16",
                  "column name 'i16' is ambiguous: it matches u.i16 and ok.i16"},
        // An answered subquery no longer masks a name: the static walk accepts the form, so the
        // first error of the statement is the one a resolution finds, as in DuckDB.
        ErrorCase{"SELECT nope FROM t WHERE i32 IN (SELECT i16 FROM u)", kBind, "nope",
                  "column 'nope' does not exist"},
        ErrorCase{"SELECT i16 FROM t WHERE i32 IN (SELECT x FROM missing)", kBind, "missing",
                  "table 'missing' does not exist"}));

// Where a subquery is refused, placement by placement. Roadmap PR J5 answers the three IN
// spellings and a scalar subquery as a top-level conjunct of WHERE or HAVING, and nothing else:
// every case here must still exit 4 after it, with a message and not an abort -- the binder's
// expression walks reach a subquery only through a placement the static check accepted, so a
// placement that slips through is an ANTB1_CHECK, which is why each one is pinned before the
// acceptance line moves.
INSTANTIATE_TEST_SUITE_P(
    SubqueryPlacements, BindErrorTest,
    ::testing::Values(
        // Clauses that are no conjunct list.
        ErrorCase{"SELECT i16 FROM t GROUP BY ROLLUP (i16, (SELECT COUNT(*) FROM u))", kUnsupported,
                  "(", "a subquery inside ROLLUP, CUBE or GROUPING SETS is not supported"},
        ErrorCase{"SELECT i16 FROM t GROUP BY GROUPING SETS ((i16), (i16 + (SELECT MIN(i16) FROM "
                  "u)))",
                  kUnsupported, "(",
                  "a subquery inside ROLLUP, CUBE or GROUPING SETS is not supported"},
        ErrorCase{"SELECT COUNT((SELECT COUNT(*) FROM u) * 2) FROM t", kUnsupported,
                  "(SELECT COUNT(*) FROM u) * 2",
                  "an aggregate of subquery values alone is not supported"},
        // A BETWEEN's subquery bound is a value (ADR 0041's SQ1); a correlated one waits for SQ5.
        ErrorCase{"SELECT i16 FROM t WHERE i32 BETWEEN (SELECT MIN(i16) FROM u WHERE u.i64 = "
                  "t.i64) AND 10",
                  kUnsupported, "(",
                  "a correlated scalar subquery is supported only as a direct operand of a "
                  "comparison"},
        // A subquery bound is typed as its column, with DuckDB's rules for the three values.
        ErrorCase{"SELECT i16 FROM t WHERE i64 BETWEEN (SELECT MIN(d) FROM u) AND 5", kUnsupported,
                  "BETWEEN", "BETWEEN of a DOUBLE value and a BIGINT"},
        ErrorCase{"SELECT i16 FROM t WHERE dt BETWEEN (SELECT s FROM ok LIMIT 1) AND dt", kBind,
                  "BETWEEN", "Cannot mix values of type VARCHAR and DATE in BETWEEN clause"},
        // An outer join's ON keeps every subquery refused (ADR 0041's follow-ups).
        ErrorCase{"SELECT t.i16 FROM t LEFT JOIN ok ON t.i16 = ok.i16 AND ok.i16 BETWEEN (SELECT "
                  "MIN(i16) FROM u) AND 5",
                  kUnsupported, "(", "a scalar subquery is not supported"},
        ErrorCase{"SELECT i16 FROM t WHERE s LIKE (SELECT s FROM u)", kUnsupported, "(",
                  "a scalar subquery is not supported"},
        // A correlated one over a block's aggregates names the place (ADR 0041's SQ2b).
        ErrorCase{"SELECT i16 FROM t GROUP BY i16 ORDER BY (SELECT MIN(u.i32) FROM u WHERE u.i16 = "
                  "t.i16)",
                  kUnsupported, "(",
                  "a correlated subquery over a query block's aggregates is not supported"},
        ErrorCase{
            "SELECT i16, CASE WHEN EXISTS (SELECT 1 FROM u WHERE u.i16 = t.i16) THEN 1 ELSE 0 "
            "END FROM t GROUP BY i16",
            kUnsupported, "EXISTS",
            "a correlated EXISTS over a query block's aggregates is not supported"},
        // An IN list's values read no column: a subquery's value among literals (ADR 0041's SQ1),
        // typed as DuckDB types the list, all its values in one type.
        ErrorCase{"SELECT i16 FROM t WHERE i32 IN ((SELECT COUNT(*) FROM u), i16)", kUnsupported,
                  "i16",
                  "only literals, constant integer expressions, quotients of numbers and "
                  "subquery values that read no column are supported in an IN list"},
        ErrorCase{"SELECT i16 FROM t WHERE i64 IN (1.5e0, (SELECT MIN(i64) FROM u))", kUnsupported,
                  "IN", "IN of a DOUBLE value and a BIGINT"},
        // Under an OR, NOT or CASE an IN is a mark (ADR 0041's SQ7a), with a top-level IN's
        // probe and set: a NULL literal probe, a correlated set (SQ8, SQ12) and a DOUBLE key stay
        // refused.
        ErrorCase{"SELECT i16 FROM t WHERE i16 < 10 OR NULL IN (SELECT i16 FROM u)", kUnsupported,
                  "NULL", "NULL literals are not supported"},
        ErrorCase{"SELECT i16 FROM t WHERE i16 < 10 OR i32 IN (SELECT u.i16 FROM u WHERE u.i64 = "
                  "t.i64)",
                  kUnsupported, "IN", "a correlated subquery of IN is not supported"},
        ErrorCase{"SELECT i16 FROM t WHERE i16 < 10 OR i32 NOT IN (SELECT u.i16 FROM u WHERE "
                  "u.i64 = t.i64)",
                  kUnsupported, "NOT IN", "a correlated subquery of NOT IN is not supported"},
        ErrorCase{"SELECT i16 FROM t WHERE i16 < 10 OR d IN (SELECT i16 FROM u)", kUnsupported,
                  "IN", "IN (subquery) over a DOUBLE is not supported"},
        // A mark as a BOOLEAN value, here IS NULL's operand (ADR 0041's decision 3).
        ErrorCase{"SELECT i16 FROM t WHERE (i32 IN (SELECT i16 FROM u)) IS NULL", kUnsupported,
                  "IN", "IN (subquery) is not supported"},
        // A mark whose probe reads an enclosing block would stand below that block's correlated
        // filter with a free variable, which neither unnesting path takes.
        ErrorCase{"SELECT i16 FROM t WHERE EXISTS (SELECT 1 FROM u WHERE u.i16 = t.i16 AND (u.i32 "
                  "< 0 OR t.i32 IN (SELECT i16 FROM ok)))",
                  kUnsupported, "IN",
                  "an IN subquery whose probe reads a column of an enclosing query block"},
        // A mark above a correlated block's aggregation, where ADR 0023's Path 2 does not look.
        ErrorCase{"SELECT i16 FROM t WHERE i32 > (SELECT CASE WHEN COUNT(*) IN (SELECT i16 FROM "
                  "ok) THEN 1 ELSE 0 END FROM u WHERE u.i64 = t.i64)",
                  kUnsupported, "IN",
                  "an IN subquery over the aggregate of a correlated subquery is not supported"},
        // The probe binds before the memo is read: one text that an inner ON resolves and WHERE
        // finds ambiguous is that bind error in WHERE, not the ON's mark.
        ErrorCase{"SELECT 1 FROM t AS a JOIN u ON u.i16 = a.i16 AND (i32 IN (SELECT i16 FROM ok) "
                  "OR u.i64 = 1), t AS b WHERE i32 IN (SELECT i16 FROM ok) OR b.i16 = 1",
                  kBind, "i32", "ambiguous"},
        // A LEFT JOIN's ON, which rule 11 does not let a semi join filter (ADR 0022).
        ErrorCase{"SELECT t.i16 FROM t LEFT JOIN u ON t.i32 IN (SELECT i16 FROM ok)", kUnsupported,
                  "IN", "IN (subquery) is not supported"},
        // An IN list holding one scalar subquery, which is what the grammar reads from a second
        // pair of parentheses (divergence D21); DuckDB reads an IN subquery.
        ErrorCase{"SELECT i16 FROM t WHERE i32 IN ((SELECT i16 FROM u))", kUnsupported, "(",
                  "an IN list of one subquery is not supported"},
        // The probe of an accepted IN is checked like the left-hand side of an IN list: an
        // unsupported construct in it comes first, a nested subquery among them. Without that
        // check the conjunct would be accepted and the binder would meet a construct its walks
        // assume away, which is an abort rather than an error.
        ErrorCase{"SELECT i16 FROM t WHERE TRY_CAST(i32 AS BIGINT) IN (SELECT i16 FROM u)",
                  kUnsupported, "TRY_CAST", "TRY_CAST is not supported"},
        ErrorCase{"SELECT i16 FROM t WHERE (SELECT MIN(u.i16) FROM u WHERE u.i64 = t.i64) IN "
                  "(SELECT i16 FROM ok)",
                  kUnsupported, "(", "a correlated scalar subquery"},
        ErrorCase{"SELECT i16 FROM t GROUP BY i16 HAVING TRY_CAST(COUNT(*) AS BIGINT) IN "
                  "(SELECT i16 FROM u)",
                  kUnsupported, "TRY_CAST", "TRY_CAST is not supported"},
        // The query of an accepted IN is a block CheckSupported reaches, so an unsupported
        // construct inside it is reported before any name resolves -- including through a CTE that
        // only the subquery references.
        ErrorCase{"WITH c AS (SELECT TRY_CAST(i16 AS BIGINT) AS a FROM u) "
                  "SELECT i16 FROM t WHERE i32 IN (SELECT a FROM c)",
                  kUnsupported, "TRY_CAST", "TRY_CAST is not supported"}));

// ADR 0041's SQ2a answers an uncorrelated value in the select items and ORDER BY of a block that
// does not aggregate. What it leaves refused, each with a message: a value's own bind errors, which
// come as DuckDB's, an EXISTS block's dropped list included; and the placements other PRs answer.
INSTANTIATE_TEST_SUITE_P(
    SelectValues, BindErrorTest,
    ::testing::Values(
        // One column, as in DuckDB, which reports a bind error for two.
        ErrorCase{"SELECT (SELECT i16, i32 FROM u) FROM t", kBind, "(",
                  "a scalar subquery must have one column, not 2"},
        ErrorCase{"SELECT i16 FROM t ORDER BY (SELECT i16, i32 FROM u)", kBind, "(",
                  "a scalar subquery must have one column, not 2"},
        // An EXISTS block's list is dropped, but bound: its bind errors and refusals stay, a
        // correlated value's shape that ADR 0023's Path 2 does not take among them.
        ErrorCase{"SELECT i16 FROM t WHERE EXISTS (SELECT (SELECT nope FROM u) FROM u)", kBind,
                  "nope", "column 'nope' does not exist"},
        ErrorCase{"SELECT i16 FROM t WHERE EXISTS (SELECT (SELECT ok.i16 FROM ok WHERE ok.i16 = "
                  "u.i32) FROM u)",
                  kUnsupported, "(", "other than an aggregate over every row"},
        // A correlated value or EXISTS outside a condition (ADR 0041's SQ5 and SQ13).
        ErrorCase{"SELECT (SELECT MIN(u.i16) FROM u WHERE u.i64 = t.i64) FROM t", kUnsupported, "(",
                  "a correlated scalar subquery"},
        ErrorCase{"SELECT i16 FROM t ORDER BY (SELECT MIN(u.i16) FROM u WHERE u.i64 = t.i64)",
                  kUnsupported, "(", "a correlated scalar subquery"},
        ErrorCase{"SELECT CASE WHEN EXISTS (SELECT 1 FROM u WHERE u.i64 = t.i64) THEN 1 ELSE 0 END "
                  "FROM t",
                  kUnsupported, "EXISTS", "a correlated EXISTS"},
        // An IN subquery outside a condition, a BOOLEAN value (ADR 0041's decision 3); in a CASE
        // WHEN it is a mark, of any probe but a NULL literal.
        ErrorCase{"SELECT i16 IN (SELECT i16 FROM u) FROM t", kUnsupported, "IN",
                  "IN (subquery) is not supported"},
        ErrorCase{"SELECT CASE WHEN NULL IN (SELECT i16 FROM u) THEN 1 ELSE 0 END FROM t",
                  kUnsupported, "NULL", "NULL literals are not supported"},
        // A value over the aggregate of a correlated scalar subquery, where ADR 0023's Path 2 does
        // not look (SQ5).
        ErrorCase{
            "SELECT i16 FROM t WHERE i32 > (SELECT SUM(u.i32) + (SELECT MIN(i16) FROM ok) FROM "
            "u WHERE u.i64 = t.i64)",
            kUnsupported, "(",
            "a subquery's value over the aggregate of a correlated subquery is not supported"},
        ErrorCase{
            "SELECT i16 FROM t WHERE i32 < (SELECT SUM(u.i32) FROM u WHERE u.i64 = t.i64 HAVING "
            "SUM(u.i32) > (SELECT MIN(i16) FROM ok))",
            kUnsupported, "(",
            "a subquery's value over the aggregate of a correlated subquery is not supported"},
        // A compound's own ORDER BY (ADR 0039's X5).
        ErrorCase{"SELECT i16 FROM t UNION ALL SELECT i16 FROM u ORDER BY (SELECT MIN(i16) FROM "
                  "ok)",
                  kUnsupported, "ORDER BY (SELECT MIN(i16) FROM ok)",
                  "ORDER BY on a UNION ALL is not supported"}));

// The relation limit comes before every other check (ADR 0022), and it counts a subquery's query
// too: the doubling chain of CTEs referenced only from a subquery in WHERE would otherwise expand
// to 2^11 relations from eleven lines of SQL. A subquery has no FROM item of the outer block to
// report at, so the limit reports at its operator -- where the subquery's own refusal points too.
TEST(BinderTest, TheRelationLimitCountsAChainBehindASubquery) {
  std::string sql = "WITH c0 AS (SELECT i16 FROM t)";
  for (int i = 1; i <= 10; ++i) {
    sql += std::format(", c{0} AS (SELECT x.i16 FROM c{1} AS x, c{1} AS y WHERE x.i16 = y.i16)", i,
                       i - 1);
  }
  sql += " SELECT COUNT(*) FROM u WHERE i32 IN (SELECT i16 FROM c10)";
  const Catalog catalog = MakeCatalog();
  auto plan = BindSql(sql, catalog);
  ASSERT_FALSE(plan.ok());
  const auto detail = GetSqlError(plan.status());
  ASSERT_NE(detail, nullptr) << plan.status().ToString();
  EXPECT_EQ(detail->kind(), kUnsupported) << plan.status().ToString();
  EXPECT_EQ(SpanText(sql, plan.status()), "IN") << plan.status().ToString();
  EXPECT_NE(plan.status().message().find("relations is not supported"), std::string::npos)
      << plan.status().message();
}

// A join graph that no key connects is a cross product (ADR 0040): its parts join in cross steps,
// inner joins without keys -- nested loops -- that a residual between them decides where there is
// one. A relation of one row that something reads, or one that may have no row or many, joins so
// too, and one that nothing reads still joins as one row, above the rest.
TEST(BinderTest, CrossProductsJoinAsNestedLoops) {
  const Catalog catalog = MakeCatalog();
  const auto explain = [&catalog](std::string_view sql) {
    auto plan = BindSql(sql, catalog);
    EXPECT_TRUE(plan.ok()) << sql << ": " << plan.status().ToString();
    return plan.ok() ? Explain(*plan) : std::string();
  };
  const auto count = [](const std::string& text, std::string_view what) {
    std::size_t n = 0;
    for (std::size_t at = text.find(what); at != std::string::npos; at = text.find(what, at + 1)) {
      ++n;
    }
    return n;
  };
  for (const std::string_view sql : {
           "SELECT COUNT(*) FROM t, u",
           "SELECT COUNT(*) FROM t CROSS JOIN u",
           "SELECT COUNT(*) FROM t AS a CROSS JOIN u",
           "SELECT t.i16 FROM t, (SELECT MAX(i32) AS m FROM u HAVING MAX(i32) > 0) AS b",
           "SELECT t.i16 FROM t, (SELECT MAX(i32) AS m FROM u LIMIT 1) AS b",
           "SELECT t.i16 FROM t, (SELECT i16, COUNT(*) AS n FROM u GROUP BY i16) AS b",
           "SELECT t.i16 FROM t, (SELECT MAX(i32) AS m FROM u UNION ALL SELECT MIN(i32) FROM u) b",
           "SELECT COUNT(*) FROM (SELECT i16 FROM t) AS g, (SELECT s FROM u)",
       }) {
    const std::string text = explain(sql);
    EXPECT_EQ(count(text, "keys=[]"), 1U) << sql << "\n" << text;
    EXPECT_EQ(count(text, "Join ONE-ROW"), 0U) << sql << "\n" << text;
    EXPECT_EQ(count(text, "residual="), 0U) << sql << "\n" << text;
  }
  // A condition between the two sides is the nested loop's residual: an inequality, a comparison
  // with a one-row relation's value, a filter of that relation on its own branch.
  for (const std::string_view sql : {
           "SELECT COUNT(*) FROM t JOIN u ON t.i16 > u.i16",
           "SELECT t.i16 FROM t, (SELECT MAX(i32) AS m FROM u) AS b WHERE t.i32 < b.m",
       }) {
    const std::string text = explain(sql);
    EXPECT_EQ(count(text, "Join INNER build="), 1U) << sql << "\n" << text;
    EXPECT_EQ(count(text, "keys=[] residual=["), 1U) << sql << "\n" << text;
  }
  const std::string filtered =
      explain("SELECT t.i16 FROM t, (SELECT MAX(i32) AS m FROM u) AS b WHERE b.m > 5");
  EXPECT_EQ(count(filtered, "keys=[]"), 1U) << filtered;
  EXPECT_EQ(count(filtered, "Join ONE-ROW"), 0U) << "a condition reads it\n" << filtered;
  // A one-row relation that nothing reads still joins as one row, above the cross of the others.
  const std::string first = explain("SELECT t.i16 FROM (SELECT MAX(i32) AS m FROM u) AS b, t, ok");
  EXPECT_EQ(count(first, "Join ONE-ROW"), 1U) << first;
  EXPECT_EQ(count(first, "Join INNER build="), 1U) << first;
  EXPECT_LT(first.find("Join ONE-ROW"), first.find("Join INNER")) << first;
  // Three relations, one keyed pair: one cross step and one keyed join.
  const std::string three = explain("SELECT COUNT(*) FROM t, ok, dec WHERE t.i16 = ok.i16");
  EXPECT_EQ(count(three, "Join INNER"), 2U) << three;
  EXPECT_EQ(count(three, "keys=[]"), 1U) << three;
  // A CROSS JOIN inside a FULL JOIN's left input, whose WHERE equality stays above the FULL JOIN:
  // the left input is a cross product, answered.
  const std::string full = explain(
      "SELECT t.i16 FROM t CROSS JOIN u FULL JOIN ok ON t.i16 = ok.i16 WHERE t.i32 = u.i32");
  EXPECT_LT(full.find("Join FULL"), full.find("keys=[]")) << "the cross below\n" << full;
  EXPECT_LT(full.find("Filter"), full.find("Join FULL")) << "the equality above\n" << full;
}

// A relation no join key connects joins as one row when it is a derived table or a CTE reference
// of exactly one row (an aggregate without GROUP BY, HAVING, LIMIT or OFFSET) that nothing else
// reads: a ONE-ROW join appends its row to every row of the others, above them, in FROM order --
// also when it comes first in FROM. A key makes it an ordinary inner join, and a semi join over its
// column stays above the one-row join, where it filters the rows rather than the relation's row.
TEST(BinderTest, OneRowRelationsJoinAsOneRow) {
  const Catalog catalog = MakeCatalog();
  const auto explain = [&catalog](std::string_view sql) {
    auto plan = BindSql(sql, catalog);
    EXPECT_TRUE(plan.ok()) << sql << ": " << plan.status().ToString();
    return plan.ok() ? Explain(*plan) : std::string();
  };
  const auto count = [](const std::string& text, std::string_view what) {
    std::size_t n = 0;
    for (std::size_t at = text.find(what); at != std::string::npos; at = text.find(what, at + 1)) {
      ++n;
    }
    return n;
  };
  const std::string two = explain(
      "SELECT a.n, b.m FROM (SELECT COUNT(*) AS n FROM t) AS a, (SELECT MAX(i32) AS m FROM "
      "u) AS b");
  EXPECT_EQ(count(two, "Join ONE-ROW build=right keys=[]"), 1U) << two;
  EXPECT_LT(two.find("Scan table=t"), two.find("Scan table=u")) << "a, then b's row: " << two;
  const std::string three = explain(
      "SELECT a.n FROM (SELECT COUNT(*) AS n FROM t) AS a CROSS JOIN (SELECT MIN(i16) AS m FROM "
      "u) AS b CROSS JOIN (SELECT SUM(i32) AS s FROM t) AS c");
  EXPECT_EQ(count(three, "Join ONE-ROW"), 2U) << three;
  const std::string first =
      explain("SELECT ok.i16, b.m FROM (SELECT MAX(i32) AS m FROM u) AS b, ok WHERE ok.i16 = 1");
  EXPECT_EQ(count(first, "Join ONE-ROW"), 1U) << first;
  EXPECT_LT(first.find("Scan table=ok"), first.find("Scan table=u")) << "the core first: " << first;
  const std::string cte =
      explain("WITH c AS (SELECT COUNT(*) AS n FROM u) SELECT ok.i16, c.n FROM ok, c");
  EXPECT_EQ(count(cte, "Join ONE-ROW"), 1U) << cte;
  const std::string keyed =
      explain("SELECT t.i16 FROM t, (SELECT MAX(i16) AS m FROM u) AS b WHERE t.i16 = b.m");
  EXPECT_EQ(count(keyed, "Join ONE-ROW"), 0U) << keyed;
  EXPECT_EQ(count(keyed, "Join INNER"), 1U) << keyed;
  const std::string semi = explain(
      "SELECT ok.i16 FROM ok, (SELECT MAX(i32) AS m FROM u) AS b WHERE b.m IN (SELECT i32 FROM t)");
  EXPECT_LT(semi.find("Join SEMI"), semi.find("Join ONE-ROW")) << "the semi join above: " << semi;
  // The core alone is ordered, renumbered, and its joins come back in FROM's numbering: with the
  // one-row relation first, a core of three keeps its residual on the join that has both its sides,
  // and with it in the middle, the core still joins the relations around it.
  const std::string core = explain(
      "SELECT COUNT(*) FROM (SELECT MAX(i32) AS m FROM u) AS b, t, ok, dec WHERE t.i16 = ok.i16 "
      "AND ok.i16 = dec.s16 AND t.i32 < dec.i");
  const std::size_t upper = core.find("Join INNER");
  ASSERT_EQ(count(core, "Join INNER"), 2U) << core;
  EXPECT_LT(core.find("Join ONE-ROW"), upper) << "above the core: " << core;
  EXPECT_EQ(count(core, "residual="), 1U) << core;
  EXPECT_GT(core.find("residual="), upper) << "on the upper join: " << core;
  EXPECT_LT(core.find("residual="), core.find('\n', upper)) << "on the upper join: " << core;
  for (const std::string_view scan : {"Scan table=t", "Scan table=ok", "Scan table=dec"}) {
    EXPECT_EQ(count(core, scan), 1U) << scan << ": " << core;
    EXPECT_LT(core.find(scan), core.find("Scan table=u")) << scan << ": " << core;
  }
  const std::string middle = explain(
      "SELECT COUNT(*) FROM t, (SELECT MAX(i32) AS m FROM u) AS b, ok WHERE t.i16 = ok.i16");
  EXPECT_EQ(count(middle, "Join INNER"), 1U) << middle;
  EXPECT_LT(middle.find("Join ONE-ROW"), middle.find("Join INNER")) << middle;
  EXPECT_LT(middle.find("Scan table=t"), middle.find("Scan table=u")) << middle;
  EXPECT_LT(middle.find("Scan table=ok"), middle.find("Scan table=u")) << middle;
}

// A UNION ALL in a CTE that nothing references is never checked or bound, as no such CTE is (DuckDB
// binds none either): neither its tail (X5) nor its column counts stop the query.
TEST(BinderTest, AnUnreferencedUnionAllCteIsNeverBound) {
  const Catalog catalog = MakeCatalog();
  for (const char* sql : {
           "WITH c AS (SELECT i16 FROM t UNION ALL SELECT i16 FROM u ORDER BY 1) SELECT COUNT(*) "
           "FROM t",
           "WITH c AS (SELECT i16 FROM t UNION ALL SELECT i16, i32 FROM u) SELECT COUNT(*) FROM t",
       }) {
    auto plan = BindSql(sql, catalog);
    ASSERT_TRUE(plan.ok()) << sql << ": " << plan.status().ToString();
    ASSERT_EQ(plan->output.size(), 1U);
    EXPECT_EQ(plan->output[0].name, "count_star()");
  }
}

// The types DuckDB gives a UNION ALL's columns (ADR 0036): a left fold in written order over each
// block's own type -- a literal typed by itself, unlike in CASE -- by CASE's rules for integers and
// DECIMALs, so the order of the blocks can change the type; equal types of any kind stay.
TEST(BinderTest, UnionAllTypes) {
  const Catalog catalog = MakeCatalog();
  for (const auto& [sql, type] : std::to_array<std::pair<std::string_view, LogicalType>>({
           {"SELECT i16 FROM t UNION ALL SELECT i32 FROM t", LogicalType::kInteger},
           {"SELECT i16 FROM t UNION ALL SELECT u16 FROM t", LogicalType::kInteger},
           {"SELECT u16 FROM t UNION ALL SELECT i16 FROM t", LogicalType::kInteger},
           {"SELECT u16 FROM t UNION ALL SELECT i64 FROM t", LogicalType::kBigInt},
           {"SELECT 1 FROM t UNION ALL SELECT i16 FROM t", LogicalType::kInteger},
           {"SELECT 3000000000 FROM t UNION ALL SELECT i32 FROM t", LogicalType::kBigInt},
           {"SELECT i32 FROM t UNION ALL SELECT SUM(i16) FROM t", LogicalType::kHugeInt},
           {"SELECT p FROM dec UNION ALL SELECT r FROM dec", LogicalType::Decimal(17, 4)},
           {"SELECT p FROM dec UNION ALL SELECT b FROM dec", LogicalType::Decimal(21, 2)},
           {"SELECT b FROM dec UNION ALL SELECT p FROM dec", LogicalType::Decimal(21, 2)},
           {"SELECT u16 FROM dec UNION ALL SELECT s16 FROM dec UNION ALL SELECT r FROM dec",
            LogicalType::Decimal(14, 4)},
           {"SELECT s16 FROM dec UNION ALL SELECT r FROM dec UNION ALL SELECT u16 FROM dec",
            LogicalType::Decimal(9, 4)},
           {"SELECT e FROM dec UNION ALL SELECT g FROM dec", LogicalType::Decimal(20, 10)},
           {"SELECT z FROM dec UNION ALL SELECT b FROM dec", LogicalType::Decimal(38, 10)},
           {"SELECT h FROM t UNION ALL SELECT SUM(i32) FROM t", LogicalType::Decimal(38, 0)},
           {"SELECT d FROM t UNION ALL SELECT d FROM u", LogicalType::kDouble},
           {"SELECT s FROM t UNION ALL SELECT 'x' FROM t", LogicalType::kVarchar},
           {"SELECT dt FROM t UNION ALL SELECT DATE '2020-01-01' FROM t", LogicalType::kDate},
           {"SELECT dt FROM t UNION ALL SELECT dt FROM u UNION ALL SELECT CAST('2020-01-02' AS "
            "DATE) "
            "FROM t",
            LogicalType::kDate},
       })) {
    auto plan = BindSql(sql, catalog);
    ASSERT_TRUE(plan.ok()) << sql << ": " << plan.status().ToString();
    EXPECT_EQ(plan->output[0].type, type) << sql;
  }
}

// A NULL item of a block (ADR 0037's NL3) is the identity of the union's fold, in any block; its
// column's name is the first block's (NULL, or its alias); each block that writes it computes a
// NULL of the union's type below the union. A column that is NULL in every block has DuckDB's NULL
// type, which antb1 does not support (exit 4).
TEST(BinderTest, UnionAllNullItems) {
  const Catalog catalog = MakeCatalog();
  for (const auto& [sql, type] : std::to_array<std::pair<std::string_view, LogicalType>>({
           {"SELECT NULL FROM t UNION ALL SELECT s FROM t", LogicalType::kVarchar},
           {"SELECT dt FROM t UNION ALL SELECT NULL FROM t", LogicalType::kDate},
           {"SELECT i16 FROM t UNION ALL SELECT NULL FROM t UNION ALL SELECT i32 FROM t",
            LogicalType::kInteger},
           {"SELECT NULL AS x FROM dec UNION ALL SELECT p FROM dec UNION ALL SELECT NULL FROM dec "
            "UNION ALL SELECT b FROM dec",
            LogicalType::Decimal(21, 2)},
       })) {
    auto plan = BindSql(sql, catalog);
    ASSERT_TRUE(plan.ok()) << sql << ": " << plan.status().ToString();
    EXPECT_EQ(plan->output[0].type, type) << sql;
  }
  auto plan = BindSql("SELECT NULL, i16 FROM t UNION ALL SELECT s, NULL AS y FROM t", catalog);
  ASSERT_TRUE(plan.ok()) << plan.status().ToString();
  EXPECT_EQ(plan->output[0].name, "NULL");
  EXPECT_EQ(plan->output[0].type, LogicalType::kVarchar);
  EXPECT_EQ(plan->output[1].name, "i16");
  EXPECT_EQ(plan->output[1].type, LogicalType::kSmallInt);
  const auto* node = std::get_if<UnionAllNode>(plan->root.get());
  ASSERT_NE(node, nullptr) << Explain(*plan);
  for (const LogicalNodePtr& input : node->inputs) {
    const auto* compute = std::get_if<ComputeNode>(input.get());
    ASSERT_NE(compute, nullptr) << Explain(*plan);
    ASSERT_EQ(compute->exprs.size(), 1U) << Explain(*plan);
    const auto* constant = std::get_if<ConstantExpr>(&compute->exprs[0]->node);
    ASSERT_NE(constant, nullptr) << Explain(*plan);
    EXPECT_TRUE(std::holds_alternative<std::monostate>(constant->value.value));
  }
  EXPECT_EQ(std::get<ComputeNode>(*node->inputs[0]).exprs[0]->type, LogicalType::kVarchar);
  EXPECT_EQ(std::get<ComputeNode>(*node->inputs[1]).exprs[0]->type, LogicalType::kSmallInt);

  // Its alias inside a HAVING expression is NULL in an expression: unsupported, never INTEGER.
  const auto in_having = BindSql(
      "SELECT NULL AS x, COUNT(*) AS c FROM t HAVING x + 1 > 0 UNION ALL SELECT dt, 2 FROM t",
      catalog);
  ASSERT_FALSE(in_having.ok());
  const auto having_detail = GetSqlError(in_having.status());
  ASSERT_NE(having_detail, nullptr);
  EXPECT_EQ(having_detail->kind(), SqlErrorDetail::Kind::kUnsupported);
  EXPECT_TRUE(in_having.status().message().contains("NULL literals are not supported"))
      << in_having.status().ToString();

  const auto all_null =
      BindSql("SELECT i16, NULL FROM t UNION ALL SELECT i32, NULL FROM t", catalog);
  ASSERT_FALSE(all_null.ok());
  const auto detail = GetSqlError(all_null.status());
  ASSERT_NE(detail, nullptr);
  EXPECT_EQ(detail->kind(), SqlErrorDetail::Kind::kUnsupported);
  EXPECT_TRUE(all_null.status().message().contains("whose values are all NULL"))
      << all_null.status().ToString();
}

// IS NULL (ADR 0037's NL4) binds as kIsNull wherever it is; IS NOT NULL as a WHERE conjunct as
// the folded kIsNotNull, which a filter evaluates alike, and inside an expression as NOT (IS
// NULL), which is false for NULL. A condition as the operand is refused as a value, and so is IS
// NULL outside a condition.
TEST(BinderTest, IsNullAndIsNotNull) {
  const Catalog catalog = MakeCatalog();
  const auto explain = [&catalog](std::string_view sql) {
    auto plan = BindSql(sql, catalog);
    EXPECT_TRUE(plan.ok()) << sql << ": " << plan.status().ToString();
    return plan.ok() ? Explain(*plan) : std::string();
  };
  EXPECT_TRUE(explain("SELECT i16 FROM t WHERE s IS NULL").contains("Filter s IS NULL"));
  EXPECT_TRUE(explain("SELECT i16 FROM t WHERE s IS NOT NULL").contains("Filter s IS NOT NULL"));
  EXPECT_TRUE(
      explain("SELECT i16 FROM t WHERE i32 + 1 IS NULL").contains("Filter \"(i32 + 1)\" IS NULL"));
  EXPECT_TRUE(explain("SELECT i16 FROM t WHERE i16 = 1 OR s IS NOT NULL")
                  .contains("((i16 = 1) OR (s IS NOT NULL))"));

  auto named = BindSql(
      "SELECT CASE WHEN s IS NOT NULL THEN 1 ELSE 0 END AS a, SUM(CASE WHEN dt IS NULL THEN 1 END) "
      "FROM t GROUP BY 1",
      catalog);
  ASSERT_TRUE(named.ok()) << named.status().ToString();
  EXPECT_EQ(named->output[1].name, "sum(CASE  WHEN ((dt IS NULL)) THEN (1) ELSE NULL END)");

  for (const std::string_view sql : {
           "SELECT s IS NULL FROM t",
           "SELECT i16 FROM t WHERE (i16 = 1) IS NULL",
       }) {
    const auto plan = BindSql(sql, catalog);
    ASSERT_FALSE(plan.ok()) << sql;
    const auto detail = GetSqlError(plan.status());
    ASSERT_NE(detail, nullptr) << sql;
    EXPECT_EQ(detail->kind(), SqlErrorDetail::Kind::kUnsupported)
        << sql << ": " << plan.status().ToString();
  }
}

// IS [NOT] DISTINCT FROM (ADR 0037's OJ5b). Two values bind as kIsDistinct or kIsNotDistinct; a
// constant side lowers: against NULL to IS [NOT] NULL, IS NOT DISTINCT FROM a value as a conjunct
// to
// `=`, IS DISTINCT FROM a value to a computed condition. Between two relations IS NOT DISTINCT FROM
// is a NULL-equal key where `=` would be a key, and `=` wins over it for one pair; IS DISTINCT FROM
// and a DOUBLE pair are residuals. DuckDB's names, NOT folded in. A condition as a value, two
// constants and the pairs `=` refuses are refused alike.
TEST(BinderTest, IsDistinctFrom) {
  const Catalog catalog = MakeCatalog();
  const auto explain = [&catalog](std::string_view sql) {
    auto plan = BindSql(sql, catalog);
    EXPECT_TRUE(plan.ok()) << sql << ": " << plan.status().ToString();
    return plan.ok() ? Explain(*plan) : std::string();
  };
  for (const auto& [sql, line] :
       std::initializer_list<std::pair<std::string_view, std::string_view>>{
           {"SELECT i16 FROM t WHERE i16 IS DISTINCT FROM i32", "Filter i16 IS DISTINCT FROM i32"},
           {"SELECT i16 FROM t WHERE i16 IS NOT DISTINCT FROM i32",
            "Filter i16 IS NOT DISTINCT FROM i32"},
           {"SELECT i16 FROM t WHERE i32 + 1 IS NOT DISTINCT FROM i64",
            "Filter \"(i32 + 1)\" IS NOT DISTINCT FROM i64"},
           {"SELECT i16 FROM t WHERE s IS NOT DISTINCT FROM NULL", "Filter s IS NULL"},
           {"SELECT i16 FROM t WHERE NULL IS DISTINCT FROM s", "Filter s IS NOT NULL"},
           {"SELECT i16 FROM t WHERE i16 IS NOT DISTINCT FROM 3", "Filter i16 = 3"},
           {"SELECT i16 FROM t WHERE 1 + 2 IS NOT DISTINCT FROM i16", "Filter i16 = 3"},
           {"SELECT i16 FROM t WHERE i16 IS DISTINCT FROM 3", "(i16 IS DISTINCT FROM 3)"},
           {"SELECT i16 FROM t WHERE NOT (i16 IS DISTINCT FROM i32)",
            "(i16 IS NOT DISTINCT FROM i32)"},
           {"SELECT COUNT(*) FROM t JOIN u ON t.i16 IS NOT DISTINCT FROM u.i16",
            "keys=[u.i16 IS NOT DISTINCT FROM t.i16]"},
           {"SELECT COUNT(*) FROM t, u WHERE t.s IS NOT DISTINCT FROM u.s",
            "keys=[u.s IS NOT DISTINCT FROM t.s]"},
           {"SELECT COUNT(*) FROM t JOIN u ON t.i16 IS NOT DISTINCT FROM u.i16 AND t.i16 = u.i16",
            "keys=[u.i16 = t.i16]"},
           {"SELECT COUNT(*) FROM t, u WHERE t.i16 = u.i16 AND u.i16 IS NOT DISTINCT FROM t.i16",
            "keys=[u.i16 = t.i16]"},
           {"SELECT COUNT(*) FROM t LEFT JOIN u ON t.i32 IS NOT DISTINCT FROM u.i16",
            "keys=[t.i32 IS NOT DISTINCT FROM \"CAST(u.i16 AS INTEGER)\"]"},
           {"SELECT COUNT(*) FROM t FULL JOIN u ON t.dt IS NOT DISTINCT FROM u.dt",
            "Join FULL build=right keys=[t.dt IS NOT DISTINCT FROM u.dt]"},
           {"SELECT COUNT(*) FROM t JOIN u ON t.i16 IS DISTINCT FROM u.i16",
            "keys=[] residual=[(t.i16 IS DISTINCT FROM u.i16)]"},
           {"SELECT COUNT(*) FROM t JOIN u ON t.d IS NOT DISTINCT FROM u.d",
            "keys=[] residual=[(t.d IS NOT DISTINCT FROM u.d)]"},
       }) {
    const std::string text = explain(sql);
    EXPECT_TRUE(text.contains(line)) << sql << "\n" << text;
  }

  auto named = BindSql(
      "SELECT CASE WHEN NOT (i16 IS DISTINCT FROM i32) THEN 1 ELSE 0 END AS a, "
      "SUM(CASE WHEN NOT NOT (s IS NOT DISTINCT FROM NULL) THEN 1 END), "
      "SUM(CASE WHEN NULL IS DISTINCT FROM i64 + 1 THEN 1 END) FROM t GROUP BY 1",
      catalog);
  ASSERT_TRUE(named.ok()) << named.status().ToString();
  EXPECT_EQ(named->output[1].name,
            "sum(CASE  WHEN ((s IS NOT DISTINCT FROM NULL)) THEN (1) ELSE NULL END)");
  EXPECT_EQ(named->output[2].name,
            "sum(CASE  WHEN ((NULL IS DISTINCT FROM (i64 + 1))) THEN (1) ELSE NULL END)");

  for (const auto& [sql, kind] :
       std::initializer_list<std::pair<std::string_view, SqlErrorDetail::Kind>>{
           {"SELECT i16 IS DISTINCT FROM i32 FROM t", SqlErrorDetail::Kind::kUnsupported},
           {"SELECT i16 FROM t WHERE (i16 = 1) IS DISTINCT FROM (i32 = 1)",
            SqlErrorDetail::Kind::kUnsupported},
           {"SELECT i16 FROM t WHERE 1 IS DISTINCT FROM 2", SqlErrorDetail::Kind::kUnsupported},
           {"SELECT i16 FROM t WHERE NULL IS NOT DISTINCT FROM NULL",
            SqlErrorDetail::Kind::kUnsupported},
           {"SELECT i16 FROM t WHERE dt IS DISTINCT FROM CAST(dt AS TIMESTAMP)",
            SqlErrorDetail::Kind::kUnsupported},
           {"SELECT i16 FROM t WHERE s IS DISTINCT FROM i16", SqlErrorDetail::Kind::kBind},
       }) {
    const auto plan = BindSql(sql, catalog);
    ASSERT_FALSE(plan.ok()) << sql;
    const auto detail = GetSqlError(plan.status());
    ASSERT_NE(detail, nullptr) << sql << ": " << plan.status().ToString();
    EXPECT_EQ(detail->kind(), kind) << sql << ": " << plan.status().ToString();
  }
}

// COALESCE (ADR 0037's NL5) types its arguments as CASE's values, folded from the first through
// the rest in written order, as DuckDB does: an integer literal takes the others' integer type when
// it fits, and with a DECIMAL the order of the arguments can change the type. DuckDB names it in
// upper case; a mix CASE refuses is refused here too, naming COALESCE.
TEST(BinderTest, CoalesceTypesItsArgumentsAsCaseDoes) {
  const Catalog catalog = MakeCatalog();
  const auto dec = [](int width, int scale) {
    return LogicalType::Decimal(static_cast<std::uint8_t>(width), static_cast<std::uint8_t>(scale));
  };
  for (const auto& [sql, type] : std::to_array<std::pair<std::string_view, LogicalType>>({
           {"SELECT COALESCE(s16, 7) FROM dec", LogicalType::kSmallInt},
           {"SELECT COALESCE(7, s16) FROM dec", LogicalType::kSmallInt},
           {"SELECT COALESCE(s16, 70000) FROM dec", LogicalType::kInteger},
           {"SELECT COALESCE(r, 7) FROM dec", dec(14, 4)},
           {"SELECT COALESCE(u16, s16, r) FROM dec", dec(14, 4)},
           {"SELECT COALESCE(r, u16, s16) FROM dec", dec(9, 4)},
           {"SELECT COALESCE(r, s16, 7) FROM dec", dec(14, 4)},
           {"SELECT COALESCE(7, s16, r) FROM dec", dec(9, 4)},
           {"SELECT COALESCE(i32) FROM t", LogicalType::kInteger},
           {"SELECT coalesce(s, 'x') FROM t", LogicalType::kVarchar},
           {"SELECT COALESCE(dt, '2020-01-01') FROM t", LogicalType::kDate},
       })) {
    auto plan = BindSql(sql, catalog);
    ASSERT_TRUE(plan.ok()) << sql << ": " << plan.status().ToString();
    EXPECT_EQ(ToString(plan->output[0].type), ToString(type)) << sql;
  }
  auto named =
      BindSql("SELECT coalesce(i32 + 1, i16), SUM(CoAlEsCe(i64, 0)) FROM t GROUP BY 1", catalog);
  ASSERT_TRUE(named.ok()) << named.status().ToString();
  EXPECT_EQ(named->output[0].name, "COALESCE((i32 + 1), i16)");
  EXPECT_EQ(named->output[1].name, "sum(COALESCE(i64, 0))");
  // Each argument is in the plan once: a COALESCE nested 60 deep binds at once, to one branch a
  // level (each argument read twice would double the tree at every level).
  std::string nested = "i32";
  for (int i = 0; i < 60; ++i) {
    nested = std::format("COALESCE({}, i16)", nested);
  }
  auto deep = BindSql("SELECT " + nested + " FROM t", catalog);
  ASSERT_TRUE(deep.ok()) << deep.status().ToString();
  EXPECT_EQ(deep->output[0].type, LogicalType::kInteger);
  for (const auto& [sql, kind, message] :
       std::to_array<std::tuple<std::string_view, SqlErrorDetail::Kind, std::string_view>>({
           {"SELECT COALESCE(s, i16) FROM t", SqlErrorDetail::Kind::kBind,
            "cannot mix values of type VARCHAR and SMALLINT in COALESCE"},
           {"SELECT COALESCE(i16, 'x') FROM t", SqlErrorDetail::Kind::kUnsupported,
            "a string literal as a COALESCE argument next to SMALLINT values"},
           {"SELECT COALESCE(dt, TIMESTAMP '2020-01-01 10:00:00') FROM t",
            SqlErrorDetail::Kind::kUnsupported,
            "COALESCE values of types DATE and TIMESTAMP are not supported"},
           {"SELECT COALESCE() FROM t", SqlErrorDetail::Kind::kBind,
            "COALESCE() takes at least 1 argument"},
           {"SELECT \"coalesce\"(i16, 0) FROM t", SqlErrorDetail::Kind::kUnsupported,
            "function coalesce() is not supported"},
       })) {
    const auto plan = BindSql(sql, catalog);
    ASSERT_FALSE(plan.ok()) << sql;
    const auto detail = GetSqlError(plan.status());
    ASSERT_NE(detail, nullptr) << sql;
    EXPECT_EQ(detail->kind(), kind) << sql << ": " << plan.status().ToString();
    EXPECT_TRUE(plan.status().message().contains(message)) << plan.status().ToString();
  }
}

// CAST (ADR 0037's CT1): an identity keeps the value, an exact widening is a CastExpr, a constant
// the type holds exactly folds into a constant of it; named as DuckDB names a cast, whatever alias
// of the type was written.
TEST(BinderTest, CastsThatChangeNoValue) {
  const Catalog catalog = MakeCatalog();
  struct Case {
    std::string_view expr;
    LogicalType type;
    std::string_view name;
    std::size_t node;  // the variant index of the bound expression
  };
  const std::size_t constant_node = Expr{.node = ConstantExpr{}}.node.index();
  const std::size_t cast_node = Expr{.node = CastExpr{}}.node.index();
  for (const Case& c : {
           Case{.expr = "CAST(dt AS DATE)",
                .type = LogicalType::kDate,
                .name = "CAST(dt AS \"DATE\")",
                .node = cast_node},
           Case{.expr = "i16::INT",
                .type = LogicalType::kInteger,
                .name = "CAST(i16 AS INTEGER)",
                .node = cast_node},
           Case{.expr = "CAST(i32 AS NUMERIC(12,2))",
                .type = LogicalType::Decimal(12, 2),
                .name = "CAST(i32 AS DECIMAL(12, 2))",
                .node = cast_node},
           Case{.expr = "CAST(u16 AS INT8)",
                .type = LogicalType::kBigInt,
                .name = "CAST(u16 AS BIGINT)",
                .node = cast_node},
           Case{.expr = "CAST(d AS FLOAT8)",
                .type = LogicalType::kDouble,
                .name = "CAST(d AS \"DOUBLE\")",
                .node = cast_node},
           Case{.expr = "CAST(0 AS DECIMAL(7,2))",
                .type = LogicalType::Decimal(7, 2),
                .name = "CAST(0 AS DECIMAL(7, 2))",
                .node = constant_node},
           Case{.expr = "CAST(-1.5 AS DECIMAL)",
                .type = LogicalType::Decimal(18, 3),
                .name = "CAST(-1.5 AS DECIMAL)",
                .node = constant_node},
           Case{.expr = "CAST(2.0 AS SMALLINT)",
                .type = LogicalType::kSmallInt,
                .name = "CAST(2.0 AS SMALLINT)",
                .node = constant_node},
           Case{.expr = "CAST(99999999999999999999999999999999999999 AS DECIMAL(38,0))",
                .type = LogicalType::Decimal(38, 0),
                .name = "CAST(99999999999999999999999999999999999999 AS DECIMAL(38, 0))",
                .node = constant_node},
           // Checked casts to a DECIMAL (ADR 0037's CT2): narrowed or rescaled, rounded as DuckDB
           // casts; a constant folds, rounded.
           Case{.expr = "CAST(h AS decimal(15,2))",
                .type = LogicalType::Decimal(15, 2),
                .name = "CAST(h AS DECIMAL(15, 2))",
                .node = cast_node},
           Case{.expr = "CAST(i64 AS DECIMAL(5,1))",
                .type = LogicalType::Decimal(5, 1),
                .name = "CAST(i64 AS DECIMAL(5, 1))",
                .node = cast_node},
           Case{.expr = "CAST(-1.25 AS DECIMAL(5,1))",
                .type = LogicalType::Decimal(5, 1),
                .name = "CAST(-1.25 AS DECIMAL(5, 1))",
                .node = constant_node},
           // Checked casts to an integer type (CT3): narrowed, rounded as DuckDB casts; a decimal
           // literal folds, rounded half away from zero.
           Case{.expr = "CAST(i64 AS SMALLINT)",
                .type = LogicalType::kSmallInt,
                .name = "CAST(i64 AS SMALLINT)",
                .node = cast_node},
           Case{.expr = "CAST(d AS INT)",
                .type = LogicalType::kInteger,
                .name = "CAST(d AS INTEGER)",
                .node = cast_node},
           Case{.expr = "CAST(h AS USMALLINT)",
                .type = LogicalType::kUSmallInt,
                .name = "CAST(h AS \"USMALLINT\")",
                .node = cast_node},
           Case{.expr = "CAST(-2.5 AS SMALLINT)",
                .type = LogicalType::kSmallInt,
                .name = "CAST(-2.5 AS SMALLINT)",
                .node = constant_node},
           Case{.expr = "CAST(3 AS DOUBLE)",
                .type = LogicalType::kDouble,
                .name = "CAST(3 AS \"DOUBLE\")",
                .node = constant_node},
           Case{.expr = "CAST('x' AS TEXT)",
                .type = LogicalType::kVarchar,
                .name = "CAST('x' AS VARCHAR)",
                .node = constant_node},
           Case{.expr = "CAST('2020-01-02 03:04:05' AS DATETIME)",
                .type = LogicalType::kTimestamp,
                .name = "CAST('2020-01-02 03:04:05' AS TIMESTAMP)",
                .node = constant_node},
       }) {
    const std::string sql = "SELECT " + std::string(c.expr) + " FROM t";
    auto plan = BindSql(sql, catalog);
    ASSERT_TRUE(plan.ok()) << sql << ": " << plan.status().ToString();
    EXPECT_EQ(ToString(plan->output[0].type), ToString(c.type)) << sql;
    EXPECT_EQ(plan->output[0].name, c.name) << sql;
    EXPECT_FALSE(plan->output[0].stored_as_float) << sql;
    const auto* project = std::get_if<ProjectNode>(plan->root.get());
    ASSERT_NE(project, nullptr) << Explain(*plan);
    const auto* compute = std::get_if<ComputeNode>(project->input.get());
    ASSERT_NE(compute, nullptr) << sql << "\n" << Explain(*plan);
    ASSERT_EQ(compute->exprs.size(), 1U) << sql << "\n" << Explain(*plan);
    EXPECT_EQ(compute->exprs[0]->node.index(), c.node) << sql << "\n" << Explain(*plan);
  }
}

// A quotient of two numbers (ADR 0037's CT4) in a comparison, a BETWEEN bound or an IN list folds
// into the DOUBLE DuckDB computes for it: both converted to double and divided. One by zero, or of
// anything but numbers, stays refused.
TEST(BinderTest, QuotientsOfNumbersFoldIntoDoubles) {
  const Catalog catalog = MakeCatalog();
  for (const auto& [sql, filter] : std::to_array<std::pair<std::string_view, std::string_view>>({
           {"SELECT i32 FROM t WHERE d > 2.0 / 3.0", "Filter d > 0.6666666666666666"},
           {"SELECT i32 FROM t WHERE d < -(1.5) / 3", "Filter d < -0.5"},
           {"SELECT i32 FROM t WHERE d BETWEEN 1 / 3 AND 7 / 2", "d >= 0.3333333333333333"},
           {"SELECT i32 FROM t WHERE d IN (1 / 2, 2 / 4.0)", "IN (0.5, 0.5)"},
           {"SELECT i32 FROM t WHERE i32 = 6 / 3", "Filter i32 = 2"},
           // An integer converts by DuckDB's type for it: a negated HUGEINT literal is negated
           // before DuckDB's two's-complement conversion, and a HUGEINT-typed value that fits a
           // BIGINT converts as a HUGEINT.
           {"SELECT i32 FROM t WHERE d = -(9223372036854776833) / 1",
            "Filter d = -9223372036854775808"},
           // A minus over a decimal is part of the literal, converted with its sign: DuckDB's
           // formula for a DECIMAL of more than 18 digits is not symmetric in it.
           {"SELECT i32 FROM t WHERE d > -(000009007199254740994.) / 1",
            "Filter d > -9007199254740992"},
           {"SELECT i32 FROM t WHERE d > 000009007199254740994. / 1",
            "Filter d > 9007199254740994"},
           {"SELECT i32 FROM t WHERE d = (-18014398509481987 + 0 * 100000000000000000000) / 1",
            "Filter d = -18014398509481984"},
       })) {
    auto plan = BindSql(sql, catalog);
    ASSERT_TRUE(plan.ok()) << sql << ": " << plan.status().ToString();
    EXPECT_TRUE(Explain(*plan).contains(filter)) << sql << "\n" << Explain(*plan);
  }
  for (const std::string_view sql :
       {"SELECT i32 FROM t WHERE d > 1 / 0", "SELECT i32 FROM t WHERE d > s / 2.0 / 3.0 + 1"}) {
    const auto plan = BindSql(sql, catalog);
    EXPECT_FALSE(plan.ok()) << sql;
  }
}

// A DATE plus or minus a number of days, or days plus a DATE (ADR 0037's CT5), is a DATE, named as
// DuckDB names arithmetic; the days are a SMALLINT, USMALLINT or INTEGER.
TEST(BinderTest, DatePlusOrMinusDays) {
  const Catalog catalog = MakeCatalog();
  for (const auto& [sql, name] : std::to_array<std::pair<std::string_view, std::string_view>>({
           {"SELECT dt + 7 FROM t", "(dt + 7)"},
           {"SELECT dt - i32 FROM t", "(dt - i32)"},
           {"SELECT i16 + dt FROM t", "(i16 + dt)"},
           {"SELECT dt + u16 FROM t", "(dt + u16)"},
           {"SELECT DATE '2020-01-01' + 30 FROM t", "(CAST('2020-01-01' AS \"DATE\") + 30)"},
       })) {
    auto plan = BindSql(sql, catalog);
    ASSERT_TRUE(plan.ok()) << sql << ": " << plan.status().ToString();
    EXPECT_EQ(plan->output[0].type, LogicalType::kDate) << sql;
    EXPECT_EQ(plan->output[0].name, name) << sql;
  }
  auto filtered = BindSql("SELECT i32 FROM t WHERE dt >= dt + 7", catalog);
  ASSERT_TRUE(filtered.ok()) << filtered.status().ToString();
}

// round of a DOUBLE (ADR 0037's FN2): with or without a precision, a DOUBLE named as DuckDB names
// it. DuckDB's DECIMAL and integer overloads, a FLOAT column and a negative or computed precision
// are unsupported; a VARCHAR is a bind error, and so is a call of three arguments.
TEST(BinderTest, RoundOfADouble) {
  const Catalog catalog = MakeCatalog();
  for (const auto& [sql, name] : std::to_array<std::pair<std::string_view, std::string_view>>({
           {"SELECT round(d) FROM t", "round(d)"},
           {"SELECT ROUND(d, 2) FROM t", "round(d, 2)"},
           {"SELECT round(i32 / 7, 1 + 2) FROM t", "round((i32 / 7), (1 + 2))"},
       })) {
    auto plan = BindSql(sql, catalog);
    ASSERT_TRUE(plan.ok()) << sql << ": " << plan.status().ToString();
    EXPECT_EQ(plan->output[0].type, LogicalType::kDouble) << sql;
    EXPECT_EQ(plan->output[0].name, name) << sql;
  }
  for (const auto& [sql, kind, message] :
       std::to_array<std::tuple<std::string_view, SqlErrorDetail::Kind, std::string_view>>({
           {"SELECT round(h) FROM t", SqlErrorDetail::Kind::kUnsupported,
            "round() of DECIMAL(38,0) values is not supported (only of a DOUBLE)"},
           {"SELECT round(i32, 1) FROM t", SqlErrorDetail::Kind::kUnsupported,
            "round() of INTEGER values is not supported"},
           {"SELECT round(d, -1) FROM t", SqlErrorDetail::Kind::kUnsupported,
            "round() with a precision other than an integer constant of at least 0"},
           {"SELECT round(d, i32) FROM t", SqlErrorDetail::Kind::kUnsupported,
            "round() with a precision other than an integer constant of at least 0"},
           {"SELECT round(s) FROM t", SqlErrorDetail::Kind::kBind,
            "round() needs a DOUBLE, but 's' is VARCHAR"},
           {"SELECT round(d, 1, 2) FROM t", SqlErrorDetail::Kind::kBind,
            "round() takes 1 or 2 arguments, not 3"},
           {"SELECT round(d, 2147483648) FROM t", SqlErrorDetail::Kind::kUnsupported,
            "round() with a precision other than an integer constant of at least 0"},
       })) {
    const auto plan = BindSql(sql, catalog);
    ASSERT_FALSE(plan.ok()) << sql;
    const auto detail = GetSqlError(plan.status());
    ASSERT_NE(detail, nullptr) << sql;
    EXPECT_EQ(detail->kind(), kind) << sql << ": " << plan.status().ToString();
    EXPECT_TRUE(plan.status().message().contains(message)) << plan.status().ToString();
  }
  // A FLOAT column: DuckDB rounds it in FLOAT, and antb1 reads it as DOUBLE (D11).
  Catalog with_float;
  ASSERT_TRUE(with_float
                  .Register("tf", std::make_shared<FakeTable>(
                                      arrow::schema({arrow::field("f", arrow::float32())}), 10,
                                      std::vector<int>{0}))
                  .ok());
  const auto on_float = BindSql("SELECT round(f, 1) FROM tf", with_float);
  ASSERT_FALSE(on_float.ok());
  const auto float_detail = GetSqlError(on_float.status());
  ASSERT_NE(float_detail, nullptr);
  EXPECT_EQ(float_detail->kind(), SqlErrorDetail::Kind::kUnsupported);
  EXPECT_TRUE(on_float.status().message().contains("round() of a FLOAT column is not supported"))
      << on_float.status().ToString();
}

// abs (ADR 0037's FN3) keeps its argument's type, as DuckDB's does: an integer type, a DECIMAL, a
// DOUBLE, a literal's own type; named abs(x). A FLOAT column is unsupported (D11), and a VARCHAR is
// a bind error.
TEST(BinderTest, AbsKeepsItsArgumentsType) {
  const Catalog catalog = MakeCatalog();
  for (const auto& [sql, type] : std::to_array<std::pair<std::string_view, LogicalType>>({
           {"SELECT abs(i16) FROM t", LogicalType::kSmallInt},
           {"SELECT abs(u16) FROM t", LogicalType::kUSmallInt},
           {"SELECT abs(i64 - 1) FROM t", LogicalType::kBigInt},
           {"SELECT abs(h) FROM t", LogicalType::Decimal(38, 0)},
           {"SELECT abs(d) FROM t", LogicalType::kDouble},
           {"SELECT abs(-7) FROM t", LogicalType::kInteger},
           {"SELECT abs(-1.25) FROM t", LogicalType::Decimal(3, 2)},
       })) {
    auto plan = BindSql(sql, catalog);
    ASSERT_TRUE(plan.ok()) << sql << ": " << plan.status().ToString();
    EXPECT_EQ(ToString(plan->output[0].type), ToString(type)) << sql;
  }
  auto named = BindSql("SELECT ABS(i32 - 3) FROM t", catalog);
  ASSERT_TRUE(named.ok()) << named.status().ToString();
  EXPECT_EQ(named->output[0].name, "abs((i32 - 3))");
  const auto text = BindSql("SELECT abs(s) FROM t", catalog);
  ASSERT_FALSE(text.ok());
  ASSERT_NE(GetSqlError(text.status()), nullptr);
  EXPECT_EQ(GetSqlError(text.status())->kind(), SqlErrorDetail::Kind::kBind);
  EXPECT_TRUE(text.status().message().contains("abs() needs a number, but 's' is VARCHAR"))
      << text.status().ToString();
  Catalog with_float;
  ASSERT_TRUE(with_float
                  .Register("tf", std::make_shared<FakeTable>(
                                      arrow::schema({arrow::field("f", arrow::float32())}), 10,
                                      std::vector<int>{0}))
                  .ok());
  const auto on_float = BindSql("SELECT abs(f) FROM tf", with_float);
  ASSERT_FALSE(on_float.ok());
  ASSERT_NE(GetSqlError(on_float.status()), nullptr);
  EXPECT_EQ(GetSqlError(on_float.status())->kind(), SqlErrorDetail::Kind::kUnsupported);
}

// upper and lower (ADR 0037's FN4) and DuckDB's aliases ucase and lcase: a VARCHAR of a VARCHAR,
// named as written; another type or a second argument is a bind error.
TEST(BinderTest, UpperAndLowerOfAVarchar) {
  const Catalog catalog = MakeCatalog();
  for (const auto& [sql, name] : std::to_array<std::pair<std::string_view, std::string_view>>({
           {"SELECT upper(s) FROM t", "upper(s)"},
           {"SELECT UCASE(s) FROM t", "ucase(s)"},
           {"SELECT lower(s) FROM t", "lower(s)"},
           {"SELECT lcase(s) FROM t", "lcase(s)"},
       })) {
    auto plan = BindSql(sql, catalog);
    ASSERT_TRUE(plan.ok()) << sql << ": " << plan.status().ToString();
    EXPECT_EQ(plan->output[0].type, LogicalType::kVarchar) << sql;
    EXPECT_EQ(plan->output[0].name, name) << sql;
  }
  for (const auto& [sql, message] : std::to_array<std::pair<std::string_view, std::string_view>>({
           {"SELECT upper(i32) FROM t", "upper() needs a VARCHAR, but 'i32' is INTEGER"},
           {"SELECT lcase(s, s) FROM t", "lcase"},
       })) {
    const auto bad = BindSql(sql, catalog);
    ASSERT_FALSE(bad.ok()) << sql;
    ASSERT_NE(GetSqlError(bad.status()), nullptr) << sql;
    EXPECT_EQ(GetSqlError(bad.status())->kind(), SqlErrorDetail::Kind::kBind) << sql;
    EXPECT_TRUE(bad.status().message().contains(message)) << bad.status().ToString();
  }
}

// concat (ADR 0037's FN5): a VARCHAR of one or more VARCHAR values, named as written. A value of
// another type is unsupported (DuckDB casts it to VARCHAR), at that value; concat() is a bind
// error.
TEST(BinderTest, ConcatOfVarchars) {
  const Catalog catalog = MakeCatalog();
  for (const auto& [sql, name] : std::to_array<std::pair<std::string_view, std::string_view>>({
           {"SELECT concat(s) FROM t", "concat(s)"},
           {"SELECT CONCAT(s, 'x', s, upper(s)) FROM t", "concat(s, 'x', s, upper(s))"},
           {"SELECT concat('a', concat(s, s)) FROM t", "concat('a', concat(s, s))"},
       })) {
    auto plan = BindSql(sql, catalog);
    ASSERT_TRUE(plan.ok()) << sql << ": " << plan.status().ToString();
    EXPECT_EQ(plan->output[0].type, LogicalType::kVarchar) << sql;
    EXPECT_EQ(plan->output[0].name, name) << sql;
  }
  for (const auto& [sql, span] : std::to_array<std::pair<std::string_view, std::string_view>>({
           {"SELECT concat(i32, s) FROM t", "i32"},
           {"SELECT concat(s, 'x', d) FROM t", "d"},
           {"SELECT concat(i32, d) FROM t", "i32"},  // the first refused value
       })) {
    const auto bad = BindSql(sql, catalog);
    ASSERT_FALSE(bad.ok()) << sql;
    ASSERT_NE(GetSqlError(bad.status()), nullptr) << sql;
    EXPECT_EQ(GetSqlError(bad.status())->kind(), SqlErrorDetail::Kind::kUnsupported) << sql;
    EXPECT_TRUE(bad.status().message().contains("concat() of ")) << bad.status().ToString();
    const SourceSpan at = GetSqlError(bad.status())->span();
    EXPECT_EQ(sql.substr(at.offset, at.length), span) << sql;
  }
  const auto none = BindSql("SELECT concat() FROM t", catalog);
  ASSERT_FALSE(none.ok());
  ASSERT_NE(GetSqlError(none.status()), nullptr);
  EXPECT_EQ(GetSqlError(none.status())->kind(), SqlErrorDetail::Kind::kBind);
  EXPECT_TRUE(none.status().message().contains("concat() takes at least 1 argument, not 0"))
      << none.status().ToString();
}

// A UNION ALL's plan (ADR 0036): one node over its blocks in written order, its names the first
// block's and its ids new; a block whose column is of another type is cast below it, once, to the
// union's type -- and only such a block.
TEST(BinderTest, UnionAllPlan) {
  const Catalog catalog = MakeCatalog();
  auto plan = BindSql(
      "SELECT i16 AS x, s FROM t UNION ALL SELECT i32, s FROM u UNION ALL SELECT i16, s FROM t",
      catalog);
  ASSERT_TRUE(plan.ok()) << plan.status().ToString();
  ASSERT_EQ(plan->output.size(), 2U);
  EXPECT_EQ(plan->output[0].name, "x");
  EXPECT_EQ(plan->output[0].type, LogicalType::kInteger);
  EXPECT_EQ(plan->output[1].name, "s");
  EXPECT_EQ(plan->output[1].type, LogicalType::kVarchar);
  const auto* node = std::get_if<UnionAllNode>(plan->root.get());
  ASSERT_NE(node, nullptr) << Explain(*plan);
  ASSERT_EQ(node->inputs.size(), 3U);
  EXPECT_TRUE(std::holds_alternative<ComputeNode>(*node->inputs[0])) << Explain(*plan);
  EXPECT_FALSE(std::holds_alternative<ComputeNode>(*node->inputs[1])) << Explain(*plan);
  EXPECT_TRUE(std::holds_alternative<ComputeNode>(*node->inputs[2])) << Explain(*plan);
  EXPECT_EQ(std::get<ComputeNode>(*node->inputs[0]).exprs.size(), 1U) << "only the SMALLINT column";
  for (const LogicalNodePtr& input : node->inputs) {
    for (const ColumnId id : OutputIds(*input)) {
      EXPECT_FALSE(std::ranges::contains(node->ids, id)) << "the union's ids are new";
    }
  }
  EXPECT_EQ(node->columns[1][0].name, "i32") << "each block reads under its own names";
  EXPECT_TRUE(Explain(*plan).contains("UnionAll inputs=3 columns=[x, s]")) << Explain(*plan);
}

// The names of a UNION ALL are its first block's: kept as they are at the top level, as any select
// list's, and made unique for a derived table's or a CTE's binding (ADR 0022's rule 7).
TEST(BinderTest, UnionAllNames) {
  const Catalog catalog = MakeCatalog();
  struct Case {
    std::string_view sql;
    std::vector<std::string> names;
  };
  for (const Case& c : {
           Case{.sql = "SELECT i16, i16 FROM t UNION ALL SELECT i32, i64 FROM u",
                .names = {"i16", "i16"}},
           Case{.sql =
                    "SELECT * FROM (SELECT i16, i16 FROM t UNION ALL SELECT i32, i64 FROM u) AS d",
                .names = {"i16", "i16_1"}},
           Case{.sql = "WITH c(a) AS (SELECT i16, i32 FROM t UNION ALL SELECT i32, i16 FROM u) "
                       "SELECT * FROM c",
                .names = {"a", "i32"}},
       }) {
    auto plan = BindSql(c.sql, catalog);
    ASSERT_TRUE(plan.ok()) << c.sql << ": " << plan.status().ToString();
    std::vector<std::string> names;
    for (const OutputColumn& out : plan->output) {
      names.push_back(out.name);
    }
    EXPECT_EQ(names, c.names) << c.sql;
  }
}

// A count that differs is a bind error at the UNION ALL before the first block that differs, not
// at the first UNION ALL (ADR 0036).
TEST(BinderTest, UnionAllCountErrorIsAtTheBlocksUnionAll) {
  const Catalog catalog = MakeCatalog();
  const std::string sql =
      "SELECT i16 FROM t UNION ALL SELECT i16 FROM u UNION ALL SELECT i16, i32 FROM t";
  auto plan = BindSql(sql, catalog);
  ASSERT_FALSE(plan.ok());
  const auto detail = GetSqlError(plan.status());
  ASSERT_NE(detail, nullptr) << plan.status().ToString();
  EXPECT_EQ(detail->kind(), kBind);
  EXPECT_EQ(detail->span().offset, sql.rfind("UNION ALL"));
}

// Parenthesized branches of a UNION ALL (ADR 0039's X4a): a run of UNION ALL in parentheses is
// spliced into its parent and typed with it, as one list in written order; one with a WITH list of
// its own is typed on its own first, as DuckDB does. A parenthesized block keeps its own tail.
TEST(BinderTest, ParenthesizedUnionAllBranches) {
  const Catalog catalog = MakeCatalog();
  for (const auto& [sql, type] : std::to_array<std::pair<std::string_view, LogicalType>>({
           {"SELECT r FROM dec UNION ALL SELECT s16 FROM dec UNION ALL SELECT u16 FROM dec",
            LogicalType::Decimal(9, 4)},
           {"SELECT r FROM dec UNION ALL (SELECT s16 FROM dec UNION ALL SELECT u16 FROM dec)",
            LogicalType::Decimal(9, 4)},
           {"(SELECT r FROM dec UNION ALL SELECT s16 FROM dec) UNION ALL SELECT u16 FROM dec",
            LogicalType::Decimal(9, 4)},
           {"SELECT r FROM dec UNION ALL (WITH c AS (SELECT s16, u16 FROM dec) SELECT s16 FROM c "
            "UNION ALL SELECT u16 FROM c)",
            LogicalType::Decimal(14, 4)},
           {"(SELECT r FROM dec ORDER BY r LIMIT 1) UNION ALL (SELECT s16 FROM dec LIMIT 2)",
            LogicalType::Decimal(9, 4)},
           {"((SELECT i16 FROM t)) UNION ALL ((SELECT i32 FROM u))", LogicalType::kInteger},
       })) {
    auto plan = BindSql(sql, catalog);
    ASSERT_TRUE(plan.ok()) << sql << ": " << plan.status().ToString();
    EXPECT_EQ(plan->output[0].type, type) << sql;
  }
  // The names are leaf 0's, through the parentheses.
  auto named = BindSql("(SELECT i16 AS x FROM t LIMIT 2) UNION ALL SELECT i32 FROM u", catalog);
  ASSERT_TRUE(named.ok()) << named.status().ToString();
  EXPECT_EQ(named->output[0].name, "x");
  // A count that differs is reported at the operator before the leaf, inside the parentheses too.
  const std::string sql =
      "SELECT i16 FROM t UNION ALL (SELECT i16 FROM u UNION ALL SELECT i16, i32 FROM t)";
  auto plan = BindSql(sql, catalog);
  ASSERT_FALSE(plan.ok());
  const auto detail = GetSqlError(plan.status());
  ASSERT_NE(detail, nullptr) << plan.status().ToString();
  EXPECT_EQ(detail->kind(), kBind);
  EXPECT_EQ(detail->span().offset, sql.rfind("UNION ALL"));
}

// UNION [DISTINCT] (ADR 0039's X4b): a maximal region of UNION is one SetOperation node over its
// leaves and UNION ALL subtrees in written order, a run of UNION ALL a UnionAll; a parenthesized
// run of UNION is spliced. Every node's ids are new, and the names are the first leaf's.
TEST(BinderTest, UnionPlans) {
  const Catalog catalog = MakeCatalog();
  struct Case {
    std::string_view sql;
    std::string_view explain;  // consecutive lines of its EXPLAIN
  };
  for (const Case& c : {
           Case{.sql = "SELECT i16 AS x FROM t UNION SELECT i16 FROM u",
                .explain = "SetOperation inputs=2 formula=(0 UNION 1) columns=[x]"},
           Case{.sql = "SELECT i16 FROM t UNION SELECT i16 FROM u UNION DISTINCT SELECT i16 FROM t",
                .explain = "SetOperation inputs=3 formula=(0 UNION 1 UNION 2)"},
           Case{.sql = "SELECT i16 FROM t UNION (SELECT i16 FROM u UNION SELECT i16 FROM t)",
                .explain = "SetOperation inputs=3 formula=(0 UNION 1 UNION 2)"},
           Case{.sql = "SELECT i16 FROM t UNION SELECT i16 FROM u UNION ALL SELECT i16 FROM t",
                .explain = "UnionAll inputs=2 columns=[i16]\n  SetOperation inputs=2"},
           Case{.sql = "SELECT i16 FROM t UNION ALL SELECT i16 FROM u UNION SELECT i16 FROM t",
                .explain = "SetOperation inputs=2 formula=(0 UNION 1) columns=[i16]\n  UnionAll "
                           "inputs=2"},
           Case{
               .sql = "SELECT i16 FROM t UNION (SELECT i16 FROM u UNION ALL SELECT i16 FROM t)",
               .explain = "SetOperation inputs=2 formula=(0 UNION 1) columns=[i16]\n  Project i16"},
       }) {
    auto plan = BindSql(c.sql, catalog);
    ASSERT_TRUE(plan.ok()) << c.sql << ": " << plan.status().ToString();
    const std::string explain = Explain(*plan);
    EXPECT_TRUE(explain.contains(c.explain)) << c.sql << "\n" << explain;
  }
  auto plan = BindSql("SELECT i16 AS x FROM t UNION ALL SELECT i16 FROM u UNION SELECT i32 FROM t",
                      catalog);
  ASSERT_TRUE(plan.ok()) << plan.status().ToString();
  EXPECT_EQ(plan->output[0].name, "x");
  EXPECT_EQ(plan->output[0].type, LogicalType::kInteger);
  const auto* node = std::get_if<SetOperationNode>(plan->root.get());
  ASSERT_NE(node, nullptr) << Explain(*plan);
  const auto* inner = std::get_if<UnionAllNode>(node->inputs[0].get());
  ASSERT_NE(inner, nullptr) << Explain(*plan);
  for (const LogicalNodePtr& input : node->inputs) {
    for (const ColumnId id : OutputIds(*input)) {
      EXPECT_FALSE(std::ranges::contains(node->ids, id)) << "the node's ids are new";
    }
  }
  EXPECT_EQ(node->columns[0][0].id, inner->ids[0]) << "the node reads the UnionAll's ids";
  EXPECT_EQ(node->columns[0][0].name, "x") << "a UnionAll is named by its first leaf";
  EXPECT_EQ(node->columns[1][0].name, "i32") << "each leaf reads under its own names";
}

// Types fold along DuckDB's tree (ADR 0039's Types rule): a run of UNION, or of UNION ALL, as one
// list in written order, spliced through parentheses; a run next to the other as a binary node.
// Each leaf is cast once, straight to the root's type, below the nodes.
TEST(BinderTest, UnionTypesFoldAlongTheTree) {
  const Catalog catalog = MakeCatalog();
  for (const auto& [sql, type] : std::to_array<std::pair<std::string_view, LogicalType>>({
           {"SELECT u16 FROM dec UNION SELECT s16 FROM dec UNION SELECT r FROM dec",
            LogicalType::Decimal(14, 4)},
           {"SELECT u16 FROM dec UNION (SELECT s16 FROM dec UNION SELECT r FROM dec)",
            LogicalType::Decimal(14, 4)},
           {"SELECT u16 FROM dec UNION ALL SELECT s16 FROM dec UNION SELECT r FROM dec",
            LogicalType::Decimal(14, 4)},
           {"SELECT u16 FROM dec UNION ALL (SELECT s16 FROM dec UNION SELECT r FROM dec)",
            LogicalType::Decimal(9, 4)},
           {"SELECT u16 FROM dec UNION (SELECT s16 FROM dec UNION ALL SELECT r FROM dec)",
            LogicalType::Decimal(9, 4)},
           {"SELECT NULL FROM dec UNION ALL (SELECT NULL FROM dec UNION SELECT r FROM dec)",
            LogicalType::Decimal(9, 4)},
       })) {
    auto plan = BindSql(sql, catalog);
    ASSERT_TRUE(plan.ok()) << sql << ": " << plan.status().ToString();
    EXPECT_EQ(plan->output[0].type, type) << sql;
  }
  auto plan = BindSql("SELECT u16 FROM dec UNION ALL (SELECT s16 FROM dec UNION SELECT r FROM dec)",
                      catalog);
  ASSERT_TRUE(plan.ok()) << plan.status().ToString();
  const auto* root = std::get_if<UnionAllNode>(plan->root.get());
  ASSERT_NE(root, nullptr) << Explain(*plan);
  EXPECT_TRUE(std::holds_alternative<ComputeNode>(*root->inputs[0])) << Explain(*plan);
  const auto* node = std::get_if<SetOperationNode>(root->inputs[1].get());
  ASSERT_NE(node, nullptr) << "no cast above the inner node\n" << Explain(*plan);
  const auto* cast = std::get_if<ComputeNode>(node->inputs[0].get());
  ASSERT_NE(cast, nullptr) << Explain(*plan);
  EXPECT_EQ(cast->exprs[0]->type, LogicalType::Decimal(9, 4)) << "straight to the root's type";
  EXPECT_FALSE(std::holds_alternative<ComputeNode>(*node->inputs[1])) << Explain(*plan);
}

// A UNION's errors name the operator: a count that differs at the operator before the operand, a
// run of UNION after all its leaves bound but a binary node right after its operands (DuckDB's
// order, ADR 0039's rule 6); a type refusal at the first item in the query that fails.
TEST(BinderTest, UnionErrors) {
  const Catalog catalog = MakeCatalog();
  struct Case {
    std::string_view sql;
    SqlErrorDetail::Kind kind;
    std::string_view at;
    std::string_view message;
  };
  for (const Case& c : {
           Case{.sql = "SELECT i16 FROM t UNION SELECT i16, i32 FROM u UNION SELECT nope FROM t",
                .kind = kBind,
                .at = "nope",
                .message = "nope"},
           Case{
               .sql = "SELECT i16 FROM t UNION SELECT i16, i32 FROM u UNION ALL SELECT nope FROM t",
               .kind = kBind,
               .at = "UNION SELECT",
               .message = "the same number of result columns"},
           Case{.sql = "SELECT s FROM t UNION SELECT i16 FROM u",
                .kind = kUnsupported,
                .at = "i16 FROM u",
                .message = "UNION of VARCHAR and SMALLINT values is not supported"},
           Case{.sql = "SELECT i16 FROM t UNION ALL (SELECT s FROM u UNION SELECT s FROM t)",
                .kind = kUnsupported,
                .at = "s FROM u",
                .message = "UNION ALL of SMALLINT and VARCHAR values is not supported"},
           Case{.sql = "SELECT NULL FROM t UNION SELECT NULL FROM u",
                .kind = kUnsupported,
                .at = "NULL FROM t",
                .message = "a UNION column whose values are all NULL is not supported"},
           // A nested run of NULL items is the identity of its parent's fold; the root names its
           // own operator.
           Case{.sql = "SELECT NULL FROM t UNION ALL (SELECT NULL FROM t UNION SELECT NULL FROM u)",
                .kind = kUnsupported,
                .at = "NULL FROM t",
                .message = "a UNION ALL column whose values are all NULL is not supported"},
           // An operand that fails the fold stops it: the first item in the query that fails is
           // refused, here the second leaf's, not the nested run's later one.
           Case{.sql = "SELECT i16 FROM t UNION SELECT s FROM t UNION (SELECT i16 FROM u UNION ALL "
                       "SELECT s FROM u)",
                .kind = kUnsupported,
                .at = "s FROM t",
                .message = "UNION of SMALLINT and VARCHAR values is not supported"},
           // An edge into a nested node must widen exactly too: the inner run's DECIMAL with a
           // scale does not widen to the root's DECIMAL(38,0), refused at its first leaf.
           Case{.sql = "SELECT h FROM t UNION ALL (SELECT p FROM dec UNION SELECT p FROM dec)",
                .kind = kUnsupported,
                .at = "p FROM dec",
                .message = "does not widen exactly to DECIMAL(38,0)"},
       }) {
    const std::string sql(c.sql);
    auto plan = BindSql(sql, catalog);
    ASSERT_FALSE(plan.ok()) << sql;
    const auto detail = GetSqlError(plan.status());
    ASSERT_NE(detail, nullptr) << plan.status().ToString();
    EXPECT_EQ(detail->kind(), c.kind) << sql;
    EXPECT_EQ(detail->span().offset, sql.find(c.at)) << sql << ": " << plan.status().ToString();
    EXPECT_TRUE(plan.status().ToString().contains(c.message)) << plan.status().ToString();
  }
}

// INTERSECT [ALL] (ADR 0039's X4c) binds tighter than UNION: a region of UNION and INTERSECT is
// one SetOperation node whose formula keeps the precedence, UNION ALL subtrees its inputs.
TEST(BinderTest, IntersectPlans) {
  const Catalog catalog = MakeCatalog();
  struct Case {
    std::string_view sql;
    std::string_view explain;  // consecutive lines of its EXPLAIN
  };
  for (const Case& c : {
           Case{.sql = "SELECT i16 AS x FROM t INTERSECT SELECT i16 FROM u",
                .explain = "SetOperation inputs=2 formula=(0 INTERSECT 1) columns=[x]"},
           Case{.sql = "SELECT i16 FROM t INTERSECT ALL SELECT i16 FROM u",
                .explain = "SetOperation inputs=2 formula=(0 INTERSECT ALL 1) columns=[i16]"},
           Case{.sql = "SELECT i16 FROM t UNION SELECT i16 FROM u INTERSECT SELECT i16 FROM t",
                .explain = "SetOperation inputs=3 formula=(0 UNION (1 INTERSECT 2))"},
           Case{.sql = "SELECT i16 FROM t INTERSECT SELECT i16 FROM u UNION SELECT i16 FROM t",
                .explain = "SetOperation inputs=3 formula=((0 INTERSECT 1) UNION 2)"},
           Case{.sql =
                    "SELECT i16 FROM t INTERSECT SELECT i16 FROM u INTERSECT ALL SELECT i16 FROM t",
                .explain = "SetOperation inputs=3 formula=((0 INTERSECT 1) INTERSECT ALL 2)"},
           Case{.sql = "(SELECT i16 FROM t INTERSECT ALL SELECT i16 FROM u) UNION ALL SELECT i16 "
                       "FROM t",
                .explain = "UnionAll inputs=2 columns=[i16]\n  SetOperation inputs=2 "
                           "formula=(0 INTERSECT ALL 1)"},
           Case{
               .sql = "SELECT i16 FROM t INTERSECT (SELECT i16 FROM u UNION ALL SELECT i16 FROM t)",
               .explain = "SetOperation inputs=2 formula=(0 INTERSECT 1) columns=[i16]\n  Project "
                          "i16"},
       }) {
    auto plan = BindSql(c.sql, catalog);
    ASSERT_TRUE(plan.ok()) << c.sql << ": " << plan.status().ToString();
    const std::string explain = Explain(*plan);
    EXPECT_TRUE(explain.contains(c.explain)) << c.sql << "\n" << explain;
  }
}

// A run of INTERSECT is DuckDB's binary nodes, ((A ∩ B) ∩ C), and its operand a tree's node, so
// its types fold pairwise, below any UNION around it (ADR 0039's Types rule); each step's two sides
// must widen exactly to that step's type.
TEST(BinderTest, IntersectTypesFoldAsBinaryNodes) {
  const Catalog catalog = MakeCatalog();
  for (const auto& [sql, type] : std::to_array<std::pair<std::string_view, LogicalType>>({
           // SMALLINT UNION ALL (USMALLINT INTERSECT DECIMAL(9,4)): the INTERSECT folds first.
           {"SELECT s16 FROM dec UNION ALL SELECT u16 FROM dec INTERSECT SELECT r FROM dec",
            LogicalType::Decimal(9, 4)},
           {"SELECT u16 FROM dec INTERSECT (SELECT s16 FROM dec INTERSECT SELECT r FROM dec)",
            LogicalType::Decimal(9, 4)},
           {"SELECT u16 FROM dec INTERSECT SELECT s16 FROM dec INTERSECT SELECT r FROM dec",
            LogicalType::Decimal(14, 4)},
           {"SELECT u16 FROM dec UNION ALL SELECT s16 FROM dec UNION ALL SELECT r FROM dec",
            LogicalType::Decimal(14, 4)},
       })) {
    auto plan = BindSql(sql, catalog);
    ASSERT_TRUE(plan.ok()) << sql << ": " << plan.status().ToString();
    EXPECT_EQ(plan->output[0].type, type) << sql;
  }
  // Each step's left side too, before the run's later operands: p and h fold to DECIMAL(38,0), to
  // which p does not widen exactly, so the refusal is at p -- past a leading NULL item -- where a
  // one-list check would fold on and fail at the VARCHAR operand.
  for (const std::string step : {
           "SELECT p FROM dec INTERSECT SELECT h FROM t INTERSECT SELECT s FROM t",
           "SELECT NULL FROM dec INTERSECT SELECT p FROM dec INTERSECT SELECT h FROM t",
       }) {
    auto refused = BindSql(step, catalog);
    ASSERT_FALSE(refused.ok()) << step;
    const auto step_detail = GetSqlError(refused.status());
    ASSERT_NE(step_detail, nullptr) << refused.status().ToString();
    EXPECT_EQ(step_detail->span().offset, step.find("p FROM dec")) << step;
    EXPECT_TRUE(refused.status().ToString().contains(
        "INTERSECT of DECIMAL(38,0) and DECIMAL(15,2) values is not supported (DECIMAL(15,2) "
        "does not widen exactly to DECIMAL(38,0))"))
        << refused.status().ToString();
  }
  const std::string sql = "SELECT h FROM t INTERSECT SELECT p FROM dec";
  auto plan = BindSql(sql, catalog);
  ASSERT_FALSE(plan.ok());
  const auto detail = GetSqlError(plan.status());
  ASSERT_NE(detail, nullptr) << plan.status().ToString();
  EXPECT_EQ(detail->kind(), kUnsupported);
  EXPECT_EQ(detail->span().offset, sql.find("p FROM dec"));
  EXPECT_TRUE(plan.status().ToString().contains(
      "INTERSECT of DECIMAL(38,0) and DECIMAL(15,2) values is not supported (DECIMAL(15,2) does "
      "not widen exactly to DECIMAL(38,0))"))
      << plan.status().ToString();
  // A count that differs: a binary node checks right after its two operands, before the next leaf
  // binds.
  const std::string counts =
      "SELECT i16 FROM t INTERSECT SELECT i16, i32 FROM u INTERSECT SELECT nope FROM t";
  auto counted = BindSql(counts, catalog);
  ASSERT_FALSE(counted.ok());
  const auto count_detail = GetSqlError(counted.status());
  ASSERT_NE(count_detail, nullptr) << counted.status().ToString();
  EXPECT_EQ(count_detail->kind(), kBind);
  EXPECT_EQ(count_detail->span().offset, counts.find("INTERSECT"));
}

// EXCEPT [ALL] (ADR 0039's X4d) shares UNION's level and associates to the left; INTERSECT binds
// tighter. A region of the three is one SetOperation node; a run of EXCEPT is binary, as
// INTERSECT's is, and its types fold pair by pair.
TEST(BinderTest, ExceptPlansAndTypes) {
  const Catalog catalog = MakeCatalog();
  struct Case {
    std::string_view sql;
    std::string_view explain;  // consecutive lines of its EXPLAIN
  };
  for (const Case& c : {
           Case{.sql = "SELECT i16 AS x FROM t EXCEPT SELECT i16 FROM u",
                .explain = "SetOperation inputs=2 formula=(0 EXCEPT 1) columns=[x]"},
           Case{
               .sql = "SELECT i16 FROM t EXCEPT ALL SELECT i16 FROM u EXCEPT ALL SELECT i16 FROM t",
               .explain = "SetOperation inputs=3 formula=(0 EXCEPT ALL 1 EXCEPT ALL 2)"},
           Case{.sql = "SELECT i16 FROM t EXCEPT SELECT i16 FROM u UNION SELECT i16 FROM t",
                .explain = "SetOperation inputs=3 formula=((0 EXCEPT 1) UNION 2)"},
           Case{.sql = "SELECT i16 FROM t UNION SELECT i16 FROM u EXCEPT SELECT i16 FROM t",
                .explain = "SetOperation inputs=3 formula=((0 UNION 1) EXCEPT 2)"},
           Case{.sql = "SELECT i16 FROM t EXCEPT (SELECT i16 FROM u UNION SELECT i16 FROM t)",
                .explain = "SetOperation inputs=3 formula=(0 EXCEPT (1 UNION 2))"},
           Case{.sql = "SELECT i16 FROM t EXCEPT SELECT i16 FROM u INTERSECT SELECT i16 FROM t",
                .explain = "SetOperation inputs=3 formula=(0 EXCEPT (1 INTERSECT 2))"},
           Case{.sql = "(SELECT i16 FROM t INTERSECT ALL SELECT i16 FROM u) EXCEPT ALL SELECT i16 "
                       "FROM t",
                .explain = "SetOperation inputs=3 formula=((0 INTERSECT ALL 1) EXCEPT ALL 2)"},
           Case{.sql = "SELECT i16 FROM t UNION ALL SELECT i16 FROM u EXCEPT SELECT i16 FROM t",
                .explain = "SetOperation inputs=2 formula=(0 EXCEPT 1) columns=[i16]\n  UnionAll "
                           "inputs=2"},
       }) {
    auto plan = BindSql(c.sql, catalog);
    ASSERT_TRUE(plan.ok()) << c.sql << ": " << plan.status().ToString();
    const std::string explain = Explain(*plan);
    EXPECT_TRUE(explain.contains(c.explain)) << c.sql << "\n" << explain;
  }
  for (const auto& [sql, type] : std::to_array<std::pair<std::string_view, LogicalType>>({
           {"SELECT s16 FROM dec EXCEPT SELECT u16 FROM dec EXCEPT SELECT r FROM dec",
            LogicalType::Decimal(14, 4)},
           {"SELECT s16 FROM dec EXCEPT (SELECT u16 FROM dec EXCEPT SELECT r FROM dec)",
            LogicalType::Decimal(9, 4)},
       })) {
    auto plan = BindSql(sql, catalog);
    ASSERT_TRUE(plan.ok()) << sql << ": " << plan.status().ToString();
    EXPECT_EQ(plan->output[0].type, type) << sql;
  }
  const std::string sql = "SELECT p FROM dec EXCEPT ALL SELECT h FROM t";
  auto plan = BindSql(sql, catalog);
  ASSERT_FALSE(plan.ok());
  EXPECT_TRUE(plan.status().ToString().contains("EXCEPT ALL of DECIMAL(38,0) and DECIMAL(15,2)"))
      << plan.status().ToString();
}

// A CTE that only a UNION ALL's last block reads is found in the UNION ALL's frame, and a WITH list
// before the first block is every block's (ADR 0036).
TEST(BinderTest, UnionAllBlocksSeeItsWithList) {
  const Catalog catalog = MakeCatalog();
  for (const char* sql : {
           "WITH c AS (SELECT i32 AS x FROM t) SELECT i32 FROM t UNION ALL SELECT i32 FROM u "
           "UNION ALL SELECT x FROM c",
           "WITH c AS (SELECT i32 AS x FROM t) SELECT x FROM c UNION ALL SELECT x FROM c",
           "SELECT y FROM (WITH c AS (SELECT i32 AS x FROM t) SELECT x AS y FROM c UNION ALL "
           "SELECT i32 FROM u) AS d",
       }) {
    auto plan = BindSql(sql, catalog);
    ASSERT_TRUE(plan.ok()) << sql << ": " << plan.status().ToString();
  }
}

// Rules 1, 2 and 6 of ADR 0022 over one FROM item: an alias names the binding, a qualified name
// resolves in the binding it names (ASCII case-insensitively, quoted or not), and a result name
// keeps the qualifier as written, each part quoted as DuckDB quotes an aggregate's argument. A
// plain column select item keeps its declared name, not the text as written.
TEST(BinderTest, QualifiedNamesAndAliasesBind) {
  const Catalog catalog = MakeCatalog();
  struct Case {
    std::string_view sql;
    std::string_view name;  // the first output column's name
  };
  for (const Case& c : {
           Case{.sql = "SELECT t.i16 FROM t", .name = "i16"},
           Case{.sql = R"(SELECT "t".i16 FROM t)", .name = "i16"},
           Case{.sql = "SELECT T.I16 FROM t", .name = "i16"},
           Case{.sql = "SELECT a.i16 FROM t AS a", .name = "i16"},
           Case{.sql = "SELECT a.i16 FROM t a", .name = "i16"},
           // `over` is a legal implicit alias, but the parser takes a reserved word as a qualifier
           // only quoted, so a reference to it needs the quotes.
           Case{.sql = R"(SELECT "over".i16 FROM t over)", .name = "i16"},
           Case{.sql = "SELECT i16 FROM t AS a", .name = "i16"},
           Case{.sql = "SELECT COUNT(*) FROM t over", .name = "count_star()"},
           // Rule 6: the qualifier stays as written inside an expression's name.
           Case{.sql = "SELECT SUM(t.i16) FROM t", .name = "sum(t.i16)"},
           // A plain identifier, so unquoted.
           Case{.sql = R"(SELECT SUM("t".i16) FROM t)", .name = "sum(t.i16)"},
           Case{.sql = R"(SELECT SUM(t."Mixed Case") FROM t)", .name = R"(sum(t."Mixed Case"))"},
           Case{.sql = R"(SELECT SUM("from".i16) FROM t AS "from")", .name = R"(sum("from".i16))"},
           Case{.sql = "SELECT i16 + t.i32 FROM t", .name = "(i16 + t.i32)"},
           Case{.sql = "SELECT -t.i64 FROM t", .name = "-(t.i64)"},
       }) {
    auto plan = BindSql(c.sql, catalog);
    ASSERT_TRUE(plan.ok()) << c.sql << ": " << plan.status().ToString();
    ASSERT_EQ(plan->output.size(), 1U) << c.sql;
    EXPECT_EQ(plan->output[0].name, c.name) << c.sql;
  }
  // Wherever else a column may stand, a qualified name binds as the unqualified one does.
  for (const std::string_view sql : {
           "SELECT strlen(t.s) FROM t",
           R"(SELECT COUNT(DISTINCT "t".i16) FROM t)",
           "SELECT EXTRACT(year FROM t.dt) FROM t",
           "SELECT CASE WHEN t.i16 = 1 THEN 2 END FROM t",
           "SELECT CASE i16 WHEN 1 THEN t.s END FROM t",
           "SELECT COUNT(*) FROM t WHERE t.i16 = 1",
           "SELECT COUNT(*) FROM t WHERE i16 BETWEEN t.i32 AND 2",
           "SELECT COUNT(*) FROM t WHERE i16 = 1 OR NOT t.i32 > 2",
           "SELECT i16 FROM t GROUP BY t.i16",
           "SELECT i16 FROM t GROUP BY i16 HAVING MAX(t.i32) > 1",
           "SELECT i16 FROM t ORDER BY t.i16",
           "SELECT a.i16 FROM t AS a GROUP BY a.i16 ORDER BY a.i16",
       }) {
    auto plan = BindSql(sql, catalog);
    EXPECT_TRUE(plan.ok()) << sql << ": " << plan.status().ToString();
  }
}

TEST(BinderTest, TablesMatchCaseInsensitively) {
  const Catalog catalog = MakeCatalog();
  for (const char* sql :
       {"SELECT COUNT(*) FROM T", "SELECT COUNT(*) FROM \"T\"", "select count(*) from \"t\""}) {
    auto plan = BindSql(sql, catalog);
    ASSERT_TRUE(plan.ok()) << sql << ": " << plan.status().ToString();
    const auto& scan = std::get<ScanNode>(Nth(*plan, 1));
    EXPECT_EQ(scan.table, catalog.Find("t")) << sql;
  }
}

// plan::Bind is public: a statement without a FROM item, which the parser never makes, is a
// programming error, never an out-of-range read.
TEST(BinderDeathTest, AStatementWithoutAFromItemAborts) {
  const Catalog catalog = MakeCatalog();
  sql::SelectStatement stmt;
  stmt.items.push_back(sql::SelectItem{.value = sql::SelectStar{}, .alias = {}, .span = {}});
  EXPECT_DEATH((void)Bind(stmt, catalog), "from.empty");
}

// Grouping sets in the grammar (ADR 0037's RU1, ADR 0038): every element is answered since RU6, and
// the expressions inside one are checked as any GROUP BY item's, at their own span, after the
// plain items before it, in a derived table, a CTE and a subquery's block too.
TEST(BinderTest, GroupingElementsAreCheckedLikeAnyGroupByItem) {
  const Catalog catalog = MakeCatalog();
  for (const std::string_view sql : {
           "SELECT 1 FROM t GROUP BY CUBE (i16, md5(s))",
           "SELECT 1 FROM t GROUP BY GROUPING SETS ((i16), ROLLUP (md5(s)))",
           "SELECT 1 FROM (SELECT 1 AS x FROM t GROUP BY CUBE (md5(s))) AS d",
           "WITH c AS (SELECT 1 AS x FROM t GROUP BY CUBE (md5(s))) SELECT x FROM c",
           "SELECT i16 FROM t WHERE i16 IN (SELECT i16 FROM t GROUP BY CUBE (i16, md5(s)))",
       }) {
    const auto bound = BindSql(sql, catalog);
    ASSERT_FALSE(bound.ok()) << sql;
    ASSERT_NE(GetSqlError(bound.status()), nullptr) << sql;
    EXPECT_EQ(GetSqlError(bound.status())->kind(), SqlErrorDetail::Kind::kUnsupported) << sql;
    EXPECT_EQ(SpanText(sql, bound.status()), "md5") << sql;
  }
  // A plain item before the element is checked first: its own refusal wins.
  const auto first = BindSql("SELECT 1 FROM t GROUP BY md5(s), CUBE (lower(s))", catalog);
  ASSERT_FALSE(first.ok());
  EXPECT_TRUE(first.status().message().contains("md5")) << first.status().ToString();
}

// GROUP BY CUBE (ADR 0037's RU6): every subset of its elements, depth first as DuckDB lists them,
// a composite one element, duplicates kept, nested in GROUPING SETS and in a cross product; 16
// elements are past DuckDB's 65535 sets, and a constant in it is refused.
TEST(BinderTest, CubeIsEverySubsetDepthFirst) {
  const Catalog catalog = MakeCatalog();
  using Sets = std::vector<std::vector<std::size_t>>;
  const auto sets_of = [&](std::string_view sql) -> Sets {
    const auto plan = BindSql(sql, catalog);
    EXPECT_TRUE(plan.ok()) << sql << ": " << plan.status().ToString();
    if (!plan.ok()) {
      return {};
    }
    const auto* node = std::get_if<GroupingSetsNode>(&Nth(*plan, 1));
    EXPECT_NE(node, nullptr) << sql;
    return node == nullptr ? Sets{} : node->sets;
  };
  EXPECT_EQ(sets_of("SELECT COUNT(*) FROM t GROUP BY CUBE (i16, s, i64)"),
            (Sets{{}, {0}, {0, 1}, {0, 1, 2}, {0, 2}, {1}, {1, 2}, {2}}));
  EXPECT_EQ(sets_of("SELECT COUNT(*) FROM t GROUP BY CUBE (i16, (s, i64))"),
            (Sets{{}, {0}, {0, 1, 2}, {1, 2}}));
  EXPECT_EQ(sets_of("SELECT COUNT(*) FROM t GROUP BY i32, CUBE (i16, s)"),
            (Sets{{0}, {0, 1}, {0, 1, 2}, {0, 2}}));
  EXPECT_EQ(sets_of("SELECT COUNT(*) FROM t GROUP BY CUBE (i16, i16)"), (Sets{{}, {0}, {0}, {0}}));
  EXPECT_EQ(sets_of("SELECT COUNT(*) FROM t GROUP BY GROUPING SETS (CUBE (i16, s), ())"),
            (Sets{{}, {0}, {0, 1}, {1}, {}}));
  const auto cube_of = [](int n) {
    std::string args = "i16";
    for (int i = 1; i < n; ++i) {
      args += std::format(", i16 + {}", i);
    }
    return std::format("SELECT COUNT(*) FROM t GROUP BY CUBE ({})", args);
  };
  EXPECT_TRUE(BindSql(cube_of(15), catalog).ok());
  const auto too_many = BindSql(cube_of(16), catalog);
  ASSERT_FALSE(too_many.ok());
  EXPECT_TRUE(too_many.status().message().contains("Maximum grouping set count of 65535 exceeded"))
      << too_many.status().ToString();
  const auto constant = BindSql("SELECT 'x' AS k, COUNT(*) FROM t GROUP BY CUBE (k, i16)", catalog);
  ASSERT_FALSE(constant.ok());
  ASSERT_NE(GetSqlError(constant.status()), nullptr);
  EXPECT_EQ(GetSqlError(constant.status())->kind(), SqlErrorDetail::Kind::kUnsupported);
  EXPECT_TRUE(constant.status().message().contains("a constant in CUBE is not supported"))
      << constant.status().ToString();
}

// GROUP BY ROLLUP (ADR 0037's RU3): a GroupingSets node over the distinct keys, its sets expanded
// as DuckDB expands them -- a ROLLUP's prefixes from () up, a composite one element, the items'
// cross product with the earlier ones varying slowest, duplicate sets kept -- and the select list
// reading the keys' outputs, NULL in the sets without them.
TEST(BinderTest, RollupBindsToAGroupingSetsNode) {
  const Catalog catalog = MakeCatalog();
  const auto sets_of = [&](std::string_view sql) -> std::vector<std::vector<std::size_t>> {
    const auto plan = BindSql(sql, catalog);
    EXPECT_TRUE(plan.ok()) << sql << ": " << plan.status().ToString();
    if (!plan.ok()) {
      return {};
    }
    const auto* node = std::get_if<GroupingSetsNode>(&Nth(*plan, 1));
    EXPECT_NE(node, nullptr) << sql;
    return node == nullptr ? std::vector<std::vector<std::size_t>>{} : node->sets;
  };
  using Sets = std::vector<std::vector<std::size_t>>;
  EXPECT_EQ(sets_of("SELECT i16, s, COUNT(*) FROM t GROUP BY ROLLUP (i16, s)"),
            (Sets{{}, {0}, {0, 1}}));
  EXPECT_EQ(sets_of("SELECT i16, COUNT(*) FROM t GROUP BY ROLLUP (i16, i16)"),
            (Sets{{}, {0}, {0}}));
  EXPECT_EQ(sets_of("SELECT COUNT(*) FROM t GROUP BY i32, ROLLUP (i16, (s, i64)), ROLLUP (dt)"),
            (Sets{{0}, {0, 4}, {0, 1}, {0, 1, 4}, {0, 1, 2, 3}, {0, 1, 2, 3, 4}}));
  EXPECT_EQ(sets_of("SELECT COUNT(*) FROM t GROUP BY ROLLUP (i16, s), i16"),
            (Sets{{0}, {0}, {0, 1}}));
  // An alias and a position name a select item's key.
  EXPECT_EQ(sets_of("SELECT i16 + 1 AS k, s, COUNT(*) FROM t GROUP BY ROLLUP (k, 2)"),
            (Sets{{}, {0}, {0, 1}}));

  const auto plan =
      BindSql("SELECT s, SUM(i32) FROM t GROUP BY ROLLUP (s) HAVING COUNT(*) > 1", catalog);
  ASSERT_TRUE(plan.ok()) << plan.status().ToString();
  EXPECT_EQ(Explain(*plan),
            R"x(Output: s:VARCHAR sum(i32):HUGEINT
Project s, "sum(i32)"
  Filter "count_star()" > 1
    GroupingSets keys=[s] sets=[(), (s)] SUM(i32), COUNT(*)
      Scan table=t source=fake columns=[i16, i32, i64, u16, h, d, s, dt, bad, "Mixed Case", from]
)x");
}

// GROUPING SETS, composites and () (ADR 0037's RU5): a GROUPING SETS list is its elements' sets
// one after another (a nested ROLLUP's in place), clauses still combine as a cross product, and a
// single set of every key is a plain GroupAggregate, as DuckDB plans it; () alone keeps the node,
// whose empty set has a row even over no input.
TEST(BinderTest, GroupingSetsComposeAndASingleSetIsAPlainGroupBy) {
  const Catalog catalog = MakeCatalog();
  using Sets = std::vector<std::vector<std::size_t>>;
  const auto sets_of = [&](std::string_view sql) -> Sets {
    const auto plan = BindSql(sql, catalog);
    EXPECT_TRUE(plan.ok()) << sql << ": " << plan.status().ToString();
    if (!plan.ok()) {
      return {};
    }
    const auto* node = std::get_if<GroupingSetsNode>(&Nth(*plan, 1));
    EXPECT_NE(node, nullptr) << sql;
    return node == nullptr ? Sets{} : node->sets;
  };
  EXPECT_EQ(sets_of("SELECT COUNT(*) FROM t GROUP BY GROUPING SETS ((i16, s), (i16), ())"),
            (Sets{{0, 1}, {0}, {}}));
  EXPECT_EQ(
      sets_of("SELECT COUNT(*) FROM t GROUP BY i32, GROUPING SETS (i16, (s, i64), ROLLUP (dt))"),
      (Sets{{0, 1}, {0, 2, 3}, {0}, {0, 4}}));
  EXPECT_EQ(
      sets_of("SELECT COUNT(*) FROM t GROUP BY GROUPING SETS (i16, s), GROUPING SETS (i32, i64)"),
      (Sets{{0, 2}, {0, 3}, {1, 2}, {1, 3}}));
  EXPECT_EQ(
      sets_of(
          "SELECT COUNT(*) FROM t GROUP BY GROUPING SETS ((i16), (i16), GROUPING SETS (i16, ()))"),
      (Sets{{0}, {0}, {0}, {}}));
  EXPECT_EQ(sets_of("SELECT COUNT(*) FROM t GROUP BY ()"), (Sets{{}}));
  for (const std::string_view sql :
       {"SELECT i16, s, COUNT(*) FROM t GROUP BY (i16, s)",
        "SELECT i16, COUNT(*) FROM t GROUP BY i16, ()",
        "SELECT i16, COUNT(*) FROM t GROUP BY GROUPING SETS ((i16))"}) {
    const auto plan = BindSql(sql, catalog);
    ASSERT_TRUE(plan.ok()) << sql << ": " << plan.status().ToString();
    EXPECT_TRUE(std::holds_alternative<GroupAggregateNode>(Nth(*plan, 1))) << Explain(*plan);
  }
  for (const auto& [sql, message] : std::to_array<std::pair<std::string_view, std::string_view>>({
           {"SELECT 'x' AS k, COUNT(*) FROM t GROUP BY GROUPING SETS ((k), ())",
            "a constant in GROUPING SETS is not supported"},
           {"SELECT 'x' AS k, COUNT(*) FROM t GROUP BY k, ()",
            "a constant GROUP BY item next to grouping sets is not supported"},
       })) {
    const auto bound = BindSql(sql, catalog);
    ASSERT_FALSE(bound.ok()) << sql;
    ASSERT_NE(GetSqlError(bound.status()), nullptr) << sql;
    EXPECT_EQ(GetSqlError(bound.status())->kind(), SqlErrorDetail::Kind::kUnsupported) << sql;
    EXPECT_TRUE(bound.status().message().contains(message)) << bound.status().ToString();
  }
  const auto plain_column = BindSql("SELECT i16, COUNT(*) FROM t GROUP BY ()", catalog);
  ASSERT_FALSE(plain_column.ok());
  EXPECT_TRUE(plain_column.status().message().contains("must appear in the GROUP BY clause"))
      << plain_column.status().ToString();
}

// What RU3 refuses or rejects: a constant in a ROLLUP (DuckDB groups by it, NULL where rolled up,
// which the select list's constant would not show), an aggregate named by position, and more than
// DuckDB's 65535 sets.
TEST(BinderTest, RollupErrors) {
  const Catalog catalog = MakeCatalog();
  const auto constant =
      BindSql("SELECT 'x' AS k, COUNT(*) FROM t GROUP BY ROLLUP (k, i16)", catalog);
  ASSERT_FALSE(constant.ok());
  ASSERT_NE(GetSqlError(constant.status()), nullptr);
  EXPECT_EQ(GetSqlError(constant.status())->kind(), SqlErrorDetail::Kind::kUnsupported);
  EXPECT_TRUE(constant.status().message().contains("a constant in ROLLUP is not supported"))
      << constant.status().ToString();

  // A constant next to a ROLLUP: DuckDB's every set groups by it, so no input gives no row.
  for (const std::string_view sql :
       {"SELECT 'x' AS k, i16, COUNT(*) FROM t GROUP BY 1, ROLLUP (i16)",
        "SELECT 'x' AS k, i16, COUNT(*) FROM t GROUP BY k, ROLLUP (i16)",
        "SELECT i16, COUNT(*) FROM t GROUP BY 'y', ROLLUP (i16)"}) {
    const auto beside = BindSql(sql, catalog);
    ASSERT_FALSE(beside.ok()) << sql;
    ASSERT_NE(GetSqlError(beside.status()), nullptr) << sql;
    EXPECT_EQ(GetSqlError(beside.status())->kind(), SqlErrorDetail::Kind::kUnsupported) << sql;
    EXPECT_TRUE(beside.status().message().contains(
        "a constant GROUP BY item next to grouping sets is not supported"))
        << beside.status().ToString();
  }

  const auto aggregate = BindSql("SELECT COUNT(*) FROM t GROUP BY ROLLUP (1)", catalog);
  ASSERT_FALSE(aggregate.ok());
  EXPECT_TRUE(aggregate.status().message().contains("GROUP BY cannot refer to the aggregate"))
      << aggregate.status().ToString();

  // 4^8 = 65536 sets: one too many. 4^7 is fine.
  std::string many = "SELECT COUNT(*) FROM t GROUP BY ROLLUP (i16, i32, i64)";
  for (int i = 0; i < 6; ++i) {
    many += ", ROLLUP (i16, i32, i64)";
  }
  EXPECT_TRUE(BindSql(many, catalog).ok());
  many += ", ROLLUP (i16, i32, i64)";
  const auto too_many = BindSql(many, catalog);
  ASSERT_FALSE(too_many.ok());
  EXPECT_TRUE(too_many.status().message().contains("Maximum grouping set count of 65535 exceeded"))
      << too_many.status().ToString();
  ASSERT_NE(GetSqlError(too_many.status()), nullptr);
  EXPECT_EQ(GetSqlError(too_many.status())->kind(), SqlErrorDetail::Kind::kBind);
}

// GROUPING(...) and GROUPING_ID(...) (ADR 0037's RU4): a BIGINT column of the grouping-sets node
// per distinct argument list, its arguments the keys they name as expressions; over a plain GROUP
// BY the node has one set of every key. DuckDB's errors otherwise.
TEST(BinderTest, GroupingCallsAreColumnsOfTheNode) {
  const Catalog catalog = MakeCatalog();
  const auto plan = BindSql(
      "SELECT i16, s, GROUPING(i16, s), grouping_id(s), COUNT(*) FROM t GROUP BY ROLLUP (i16, s) "
      "HAVING GROUPING(i16, s) < 3 ORDER BY GROUPING(s)",
      catalog);
  ASSERT_TRUE(plan.ok()) << plan.status().ToString();
  const GroupingSetsNode* node = nullptr;
  for (int depth = 0; depth < 6 && node == nullptr; ++depth) {
    node = std::get_if<GroupingSetsNode>(&Nth(*plan, depth));
  }
  ASSERT_NE(node, nullptr);
  ASSERT_EQ(node->grouping.size(), 2U);  // GROUPING(i16, s) once, GROUPING(s) once
  EXPECT_EQ(node->grouping[0].args, (std::vector<std::size_t>{0, 1}));
  EXPECT_EQ(node->grouping[1].args, (std::vector<std::size_t>{1}));
  EXPECT_EQ(plan->output[2].name, "GROUPING(i16, s)");
  EXPECT_EQ(plan->output[3].name, "GROUPING(s)");
  EXPECT_EQ(plan->output[3].type, LogicalType::kBigInt);

  const auto plain = BindSql("SELECT i16, GROUPING(i16) FROM t GROUP BY i16", catalog);
  ASSERT_TRUE(plain.ok()) << plain.status().ToString();
  const auto* one = std::get_if<GroupingSetsNode>(&Nth(*plain, 1));
  ASSERT_NE(one, nullptr);
  EXPECT_EQ(one->sets, (std::vector<std::vector<std::size_t>>{{0}}));

  for (const auto& [sql, message] : std::to_array<std::pair<std::string_view, std::string_view>>({
           {"SELECT GROUPING(i16) FROM t", "GROUPING statement cannot be used without groups"},
           {"SELECT GROUPING(i16), COUNT(*) FROM t",
            "GROUPING statement cannot be used without groups"},
           {"SELECT i16, GROUPING(s) FROM t GROUP BY ROLLUP (i16)",
            "GROUPING child \"s\" must be a grouping column"},
           {"SELECT i16 AS k, GROUPING(k) FROM t GROUP BY ROLLUP (i16)",
            "GROUPING child \"k\" must be a grouping column"},
           {"SELECT i16, GROUPING(1) FROM t GROUP BY ROLLUP (i16)",
            "GROUPING child \"1\" must be a grouping column"},
           {"SELECT i16 FROM t WHERE GROUPING(i16) = 0 GROUP BY i16",
            "GROUPING function is not supported here"},
           {"SELECT i16, SUM(GROUPING(i16)) FROM t GROUP BY ROLLUP (i16)",
            "GROUPING function is not supported here"},
           {"SELECT i16, GROUPING() FROM t GROUP BY i16", "GROUPING needs at least one argument"},
       })) {
    const auto bound = BindSql(sql, catalog);
    ASSERT_FALSE(bound.ok()) << sql;
    ASSERT_NE(GetSqlError(bound.status()), nullptr) << sql;
    EXPECT_EQ(GetSqlError(bound.status())->kind(), SqlErrorDetail::Kind::kBind) << sql;
    EXPECT_TRUE(bound.status().message().contains(message)) << bound.status().ToString();
  }
  // Refused rather than wrong: GROUPING of a constant GROUP BY item, which DuckDB answers (0), and
  // of an enclosing query's column, refused as any outer reference outside WHERE is.
  for (const std::string_view sql : {"SELECT i16, GROUPING('x') FROM t GROUP BY i16, 'x'",
                                     "SELECT 1, GROUPING(1) FROM t GROUP BY 1",
                                     "SELECT i16 FROM t WHERE i16 IN (SELECT i32 FROM u GROUP BY "
                                     "i32 HAVING GROUPING(t.i16) = 0)"}) {
    const auto refused = BindSql(sql, catalog);
    ASSERT_FALSE(refused.ok()) << sql;
    ASSERT_NE(GetSqlError(refused.status()), nullptr) << sql;
    EXPECT_EQ(GetSqlError(refused.status())->kind(), SqlErrorDetail::Kind::kUnsupported)
        << refused.status().ToString();
  }
  // 64 arguments are DuckDB's error; 63 are fine.
  const auto list = [](int n) {
    std::string args = "i16";
    for (int i = 1; i < n; ++i) {
      args += ", i16";
    }
    return std::format("SELECT GROUPING({}) FROM t GROUP BY i16", args);
  };
  EXPECT_TRUE(BindSql(list(63), catalog).ok());
  const auto many = BindSql(list(64), catalog);
  ASSERT_FALSE(many.ok());
  EXPECT_TRUE(many.status().message().contains("cannot have more than 64 groups"))
      << many.status().ToString();
}

// A block with a GROUP BY in grouping-set syntax is a grouped block wherever the binder asks
// (HasGroupBy): a correlated EXISTS or scalar subquery over one is refused as over a plain GROUP
// BY, with the same message at the same span, and an uncorrelated one is answered alike.
TEST(BinderTest, GroupingSetsMakeAGroupedBlock) {
  const Catalog catalog = MakeCatalog();
  // Correlated, a grouped block stays refused, with the same message at the same place.
  for (const std::string_view outer : {
           "SELECT i16 FROM t WHERE EXISTS (SELECT 1 FROM u WHERE u.i16 = t.i16 GROUP BY {})",
           "SELECT i16 FROM t WHERE i16 = (SELECT COUNT(*) FROM u WHERE u.i16 = t.i16 GROUP BY {})",
       }) {
    const std::string plain = std::vformat(outer, std::make_format_args("u.i32"));
    const std::string rollup = std::vformat(outer, std::make_format_args("ROLLUP (u.i32)"));
    const auto expected = BindSql(plain, catalog);
    const auto bound = BindSql(rollup, catalog);
    ASSERT_FALSE(expected.ok()) << plain;
    ASSERT_FALSE(bound.ok()) << rollup;
    EXPECT_EQ(bound.status().message(), expected.status().message()) << rollup;
    EXPECT_EQ(SpanText(rollup, bound.status()), SpanText(plain, expected.status())) << rollup;
  }
  // Correlated through the grouping keys alone, every spelling of them is correlated alike: the
  // gate reads a grouping set's keys as it reads a GROUP BY list's.
  for (const std::string_view keys :
       {"t.i32", "ROLLUP (t.i32)", "CUBE (t.i32)", "GROUPING SETS ((t.i32), ())"}) {
    const std::string sql =
        std::vformat("SELECT i16 FROM t WHERE EXISTS (SELECT 1 FROM u WHERE u.i16 > 0 GROUP BY {})",
                     std::make_format_args(keys));
    const auto bound = BindSql(sql, catalog);
    ASSERT_FALSE(bound.ok()) << sql;
    EXPECT_TRUE(bound.status().message().starts_with(
        "EXISTS over a subquery with GROUP BY is not supported"))
        << sql << ": " << bound.status().message();
    EXPECT_EQ(SpanText(sql, bound.status()), "EXISTS") << sql;
  }
  // Uncorrelated, any block is answered (ADR 0041's SQ4a), a grouping set's as a GROUP BY's.
  for (const std::string_view outer : {
           "SELECT i16 FROM t WHERE EXISTS (SELECT 1 FROM t GROUP BY {})",
           "SELECT i16 FROM t WHERE i16 = (SELECT COUNT(*) FROM t GROUP BY {})",
       }) {
    for (const std::string_view keys : {"i16", "ROLLUP (i16)"}) {
      const std::string sql = std::vformat(outer, std::make_format_args(keys));
      const auto bound = BindSql(sql, catalog);
      EXPECT_TRUE(bound.ok()) << sql << ": " << bound.status().ToString();
    }
  }
}

// SELECT DISTINCT (ADR 0037's AG3): a GroupAggregate without calls over the Project of the select
// list, below the Sort and the Limit; every item a key of its own (`a, a` keeps two columns); over
// a grouped block, above its aggregation; none over a global aggregate's one row. ORDER BY takes a
// position, an alias or a select item written again, and refuses anything else.
TEST(BinderTest, SelectDistinct) {
  const Catalog catalog = MakeCatalog();
  auto plan = BindSql("SELECT DISTINCT i16, i16, s FROM t ORDER BY 1 DESC LIMIT 3", catalog);
  ASSERT_TRUE(plan.ok()) << plan.status().ToString();
  ASSERT_EQ(plan->output.size(), 3U);
  EXPECT_EQ(plan->output[1].name, "i16");
  const auto& limit = std::get<LimitNode>(Nth(*plan, 0));
  EXPECT_EQ(limit.limit, 3);
  const auto& sort = std::get<SortNode>(Nth(*plan, 1));
  const auto& dedup = std::get<GroupAggregateNode>(Nth(*plan, 2));
  EXPECT_TRUE(dedup.aggregates.empty());
  ASSERT_EQ(dedup.keys.size(), 3U);
  ASSERT_EQ(sort.keys.size(), 1U);
  EXPECT_EQ(sort.keys[0].column.id, dedup.key_ids[0]);
  EXPECT_TRUE(sort.keys[0].descending);
  EXPECT_TRUE(std::holds_alternative<ProjectNode>(Nth(*plan, 3)));
  for (const auto& sql : std::to_array<std::string_view>(
           {"SELECT DISTINCT i16 AS x, s FROM t ORDER BY x",
            "SELECT DISTINCT i16, s FROM t ORDER BY s",
            "SELECT DISTINCT i16 + 1, s FROM t ORDER BY i16 + 1",
            "SELECT DISTINCT i16, COUNT(*) AS n FROM t GROUP BY i16, s ORDER BY n"})) {
    auto ordered = BindSql(sql, catalog);
    ASSERT_TRUE(ordered.ok()) << sql << ": " << ordered.status().ToString();
    const auto& above = std::get<SortNode>(Nth(*ordered, 0));
    const auto& below = std::get<GroupAggregateNode>(Nth(*ordered, 1));
    EXPECT_TRUE(below.aggregates.empty()) << sql;
    EXPECT_TRUE(std::ranges::contains(below.key_ids, above.keys[0].column.id)) << sql;
  }
  auto grouped = BindSql("SELECT DISTINCT i16, COUNT(*) FROM t GROUP BY i16, s", catalog);
  ASSERT_TRUE(grouped.ok()) << grouped.status().ToString();
  EXPECT_TRUE(std::get<GroupAggregateNode>(Nth(*grouped, 0)).aggregates.empty());
  EXPECT_FALSE(std::get<GroupAggregateNode>(Nth(*grouped, 2)).aggregates.empty())
      << Explain(*grouped);
  auto global = BindSql("SELECT DISTINCT COUNT(*) FROM t", catalog);
  ASSERT_TRUE(global.ok()) << global.status().ToString();
  EXPECT_TRUE(std::holds_alternative<AggregateNode>(Nth(*global, 0))) << Explain(*global);
  // ORDER BY a select-list column named another way: qualified, or under an alias (through a star:
  // tests/slt/cases/distinct/select_distinct.slt, since this catalog's t holds an unsupported
  // type).
  for (const auto& sql :
       std::to_array<std::string_view>({"SELECT DISTINCT i16 FROM t ORDER BY t.i16",
                                        "SELECT DISTINCT i16 AS x, s FROM t ORDER BY i16"})) {
    auto ordered = BindSql(sql, catalog);
    ASSERT_TRUE(ordered.ok()) << sql << ": " << ordered.status().ToString();
    const auto& above = std::get<SortNode>(Nth(*ordered, 0));
    const auto& below = std::get<GroupAggregateNode>(Nth(*ordered, 1));
    EXPECT_TRUE(std::ranges::contains(below.key_ids, above.keys[0].column.id)) << sql;
  }
  // A correlated EXISTS takes a plain block (ADR 0023's Path 1), so DISTINCT there is refused.
  const auto exists =
      BindSql("SELECT i16 FROM t WHERE EXISTS (SELECT DISTINCT s FROM t AS u WHERE u.i32 = t.i32)",
              catalog);
  ASSERT_FALSE(exists.ok());
  ASSERT_NE(GetSqlError(exists.status()), nullptr);
  EXPECT_EQ(GetSqlError(exists.status())->kind(), SqlErrorDetail::Kind::kUnsupported);
  EXPECT_TRUE(exists.status().message().contains("EXISTS over a subquery with DISTINCT"))
      << exists.status().ToString();
  const auto refused = BindSql("SELECT DISTINCT i16 FROM t ORDER BY s", catalog);
  ASSERT_FALSE(refused.ok());
  ASSERT_NE(GetSqlError(refused.status()), nullptr);
  EXPECT_EQ(GetSqlError(refused.status())->kind(), SqlErrorDetail::Kind::kUnsupported);
  EXPECT_TRUE(refused.status().message().contains(
      "ORDER BY of an expression that is not in the SELECT DISTINCT list is not supported"))
      << refused.status().ToString();
}

// The variance family (ADR 0037's AG2): DOUBLE over any number column, an alias bound to its
// target's kind but named as written; a VARCHAR argument is a bind error.
TEST(BinderTest, VarianceFamily) {
  const Catalog catalog = MakeCatalog();
  constexpr std::string_view kSql =
      "SELECT stddev_samp(i16), STDDEV(i64), stddev_pop(d), var_samp(h), VARIANCE(u16), "
      "var_pop(i32 + 1) FROM t";
  auto plan = BindSql(kSql, catalog);
  ASSERT_TRUE(plan.ok()) << plan.status().ToString();
  const auto names =
      std::to_array<std::string_view>({"stddev_samp(i16)", "stddev(i64)", "stddev_pop(d)",
                                       "var_samp(h)", "variance(u16)", "var_pop((i32 + 1))"});
  ASSERT_EQ(plan->output.size(), names.size());
  for (std::size_t i = 0; i < names.size(); ++i) {
    EXPECT_EQ(plan->output[i].name, names.at(i));
    EXPECT_EQ(plan->output[i].type, LogicalType::kDouble) << names.at(i);
  }
  const AggregateNode* found = nullptr;
  for (int depth = 0; depth < 4 && found == nullptr; ++depth) {
    found = std::get_if<AggregateNode>(&Nth(*plan, depth));
  }
  ASSERT_NE(found, nullptr) << Explain(*plan);
  const AggregateNode& agg = *found;
  const auto kinds =
      std::to_array<AggKind>({AggKind::kStddevSamp, AggKind::kStddevSamp, AggKind::kStddevPop,
                              AggKind::kVarSamp, AggKind::kVarSamp, AggKind::kVarPop});
  ASSERT_EQ(agg.aggregates.size(), kinds.size()) << Explain(*plan);
  for (std::size_t i = 0; i < kinds.size(); ++i) {
    EXPECT_EQ(agg.aggregates[i].kind, kinds.at(i)) << i;
  }
  const auto text = BindSql("SELECT var_pop(s) FROM t", catalog);
  ASSERT_FALSE(text.ok());
  ASSERT_NE(GetSqlError(text.status()), nullptr);
  EXPECT_EQ(GetSqlError(text.status())->kind(), SqlErrorDetail::Kind::kBind);
  EXPECT_TRUE(
      text.status().message().contains("VAR_POP needs a numeric column, but 's' is VARCHAR"))
      << text.status().ToString();
}

TEST(BinderTest, CountStarIsAnAggregateOverAScanOfEveryField) {
  const Catalog catalog = MakeCatalog();
  constexpr std::string_view kSql = "SELECT COUNT(*) FROM t";
  auto plan = BindSql(kSql, catalog);
  ASSERT_TRUE(plan.ok()) << plan.status().ToString();
  ASSERT_EQ(plan->output.size(), 1U);
  EXPECT_EQ(plan->output[0].name, "count_star()");
  EXPECT_EQ(plan->output[0].type, LogicalType::kBigInt);
  const auto& agg = std::get<AggregateNode>(Nth(*plan, 0));
  ASSERT_EQ(agg.aggregates.size(), 1U);
  EXPECT_EQ(agg.aggregates[0].kind, AggKind::kCountStar);
  EXPECT_FALSE(agg.aggregates[0].arg.has_value());
  EXPECT_EQ(agg.aggregates[0].type, LogicalType::kBigInt);
  EXPECT_EQ(kSql.substr(agg.span.offset, agg.span.length), "COUNT(*)");
  const auto& scan = std::get<ScanNode>(Nth(*plan, 1));
  EXPECT_EQ(scan.table_name, "t");
  EXPECT_EQ(scan.fields, (std::vector<int>{0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10}));
  EXPECT_EQ(kSql.substr(scan.span.offset, scan.span.length), "t");
}

// GroupAggregate outputs the keys, then the aggregates; the Project on top restores the select
// order, so plain columns point at keys and aggregates follow the keys.
TEST(BinderTest, GroupByBuildsAGroupAggregateUnderAProject) {
  const Catalog catalog = MakeCatalog();
  constexpr std::string_view kSql =
      "SELECT COUNT(*) AS n, s, SUM(i32) FROM t WHERE i16 > 0 GROUP BY s, i16 LIMIT 3";
  auto plan = BindSql(kSql, catalog);
  ASSERT_TRUE(plan.ok()) << plan.status().ToString();
  ASSERT_EQ(plan->output.size(), 3U);
  EXPECT_EQ(plan->output[0].name, "n");
  EXPECT_EQ(plan->output[1].name, "s");
  EXPECT_EQ(plan->output[1].type, LogicalType::kVarchar);
  EXPECT_EQ(plan->output[2].name, "sum(i32)");
  EXPECT_EQ(plan->output[2].type, LogicalType::kHugeInt);
  EXPECT_EQ(std::get<LimitNode>(Nth(*plan, 0)).limit, 3);
  const auto& project = std::get<ProjectNode>(Nth(*plan, 1));
  ASSERT_EQ(project.columns.size(), 3U);
  EXPECT_EQ(project.columns[0].index, 2);  // COUNT(*): after the two keys
  EXPECT_EQ(project.columns[1].index, 0);  // s: the first key
  EXPECT_EQ(project.columns[2].index, 3);  // SUM(i32)
  const auto& group = std::get<GroupAggregateNode>(Nth(*plan, 2));
  ASSERT_EQ(group.keys.size(), 2U);
  EXPECT_EQ(group.keys[0].name, "s");
  EXPECT_EQ(group.keys[0].index, 6);
  EXPECT_EQ(group.keys[1].name, "i16");
  EXPECT_EQ(group.keys[1].index, 0);
  ASSERT_EQ(group.aggregates.size(), 2U);
  EXPECT_EQ(group.aggregates[0].kind, AggKind::kCountStar);
  EXPECT_EQ(group.aggregates[1].kind, AggKind::kSum);
  EXPECT_EQ(kSql.substr(group.span.offset, group.span.length), "GROUP BY s, i16");
  EXPECT_TRUE(std::holds_alternative<FilterNode>(Nth(*plan, 3)));
  EXPECT_TRUE(std::holds_alternative<ScanNode>(Nth(*plan, 4)));
}

// A GROUP BY name is a table column first, else the alias of a plain select column (DuckDB);
// repeated keys, also spelled differently, are one key; a key need not be selected.
TEST(BinderTest, GroupByAliasesDuplicatesAndUnselectedKeys) {
  const Catalog catalog = MakeCatalog();
  auto alias = BindSql("SELECT s AS k, COUNT(*) FROM t GROUP BY k, s, \"S\"", catalog);
  ASSERT_TRUE(alias.ok()) << alias.status().ToString();
  const auto& group = std::get<GroupAggregateNode>(Nth(*alias, 1));
  ASSERT_EQ(group.keys.size(), 1U);
  EXPECT_EQ(group.keys[0].index, 6);
  // An alias that is also a column name means the column.
  auto shadow = BindSql("SELECT i16 AS i32, COUNT(*) FROM t GROUP BY i32, i16", catalog);
  ASSERT_TRUE(shadow.ok()) << shadow.status().ToString();
  EXPECT_EQ(std::get<GroupAggregateNode>(Nth(*shadow, 1)).keys.size(), 2U);
  auto unselected = BindSql("SELECT MAX(d) FROM t GROUP BY dt, u16", catalog);
  ASSERT_TRUE(unselected.ok()) << unselected.status().ToString();
  const auto& project = std::get<ProjectNode>(Nth(*unselected, 0));
  ASSERT_EQ(project.columns.size(), 1U);
  EXPECT_EQ(project.columns[0].index, 2);
  auto last_alias = BindSql("SELECT COUNT(*) AS k, s AS k, i16 AS k FROM t GROUP BY k, s", catalog);
  // k is i16 (the last item named k), so both plain columns are keys.
  ASSERT_TRUE(last_alias.ok()) << last_alias.status().ToString();
  EXPECT_EQ(std::get<GroupAggregateNode>(Nth(*last_alias, 1)).keys.size(), 2U);
  auto last = BindSql("SELECT COUNT(*) AS k, s AS k FROM t GROUP BY k", catalog);
  ASSERT_TRUE(last.ok()) << last.status().ToString();
  EXPECT_EQ(std::get<GroupAggregateNode>(Nth(*last, 1)).keys[0].index, 6);
  auto keys_only = BindSql("SELECT s, i16 FROM t GROUP BY i16, s", catalog);
  ASSERT_TRUE(keys_only.ok()) << keys_only.status().ToString();
  EXPECT_TRUE(std::get<GroupAggregateNode>(Nth(*keys_only, 1)).aggregates.empty());
  EXPECT_EQ(std::get<ProjectNode>(Nth(*keys_only, 0)).columns[0].index, 1);
}

// A projection sorts the table's columns below the Project, so it can order by a column it does
// not select; a select alias comes before a column of the same name (the last alias wins), and a
// column already ordered by is not a key again.
TEST(BinderTest, OrderByInAProjection) {
  const Catalog catalog = MakeCatalog();
  constexpr std::string_view kSql =
      "SELECT i16 AS i32, s FROM t ORDER BY i32 DESC, d NULLS FIRST, I16, s LIMIT 5 OFFSET 2";
  auto plan = BindSql(kSql, catalog);
  ASSERT_TRUE(plan.ok()) << plan.status().ToString();
  const auto& limit = std::get<LimitNode>(Nth(*plan, 0));
  EXPECT_EQ(limit.limit, 5);
  EXPECT_EQ(limit.offset, 2);
  EXPECT_EQ(kSql.substr(limit.span.offset, limit.span.length), "LIMIT 5 OFFSET 2");
  EXPECT_TRUE(std::holds_alternative<ProjectNode>(Nth(*plan, 1)));
  const auto& sort = std::get<SortNode>(Nth(*plan, 2));
  EXPECT_EQ(kSql.substr(sort.span.offset, sort.span.length),
            "ORDER BY i32 DESC, d NULLS FIRST, I16, s");
  ASSERT_EQ(sort.keys.size(), 3U);  // I16 repeats i32 (the alias of i16)
  EXPECT_EQ(sort.keys[0].column.index, 0);
  EXPECT_TRUE(sort.keys[0].descending);
  EXPECT_FALSE(sort.keys[0].nulls_first);
  EXPECT_EQ(sort.keys[1].column.index, 5);
  EXPECT_EQ(sort.keys[1].column.type, LogicalType::kDouble);
  EXPECT_FALSE(sort.keys[1].descending);
  EXPECT_TRUE(sort.keys[1].nulls_first);
  EXPECT_EQ(sort.keys[2].column.index, 6);
  EXPECT_TRUE(std::holds_alternative<ScanNode>(Nth(*plan, 3)));

  auto last_alias = BindSql("SELECT i16 AS x, i32 AS X FROM t ORDER BY x", catalog);
  ASSERT_TRUE(last_alias.ok()) << last_alias.status().ToString();
  EXPECT_EQ(std::get<SortNode>(Nth(*last_alias, 1)).keys[0].column.index, 1);

  auto offset = BindSql("SELECT * FROM ok OFFSET 3", catalog);
  ASSERT_TRUE(offset.ok()) << offset.status().ToString();
  EXPECT_FALSE(std::get<LimitNode>(Nth(*offset, 0)).limit.has_value());
  EXPECT_EQ(std::get<LimitNode>(Nth(*offset, 0)).offset, 3);
  auto no_offset = BindSql("SELECT * FROM ok OFFSET 0", catalog);
  ASSERT_TRUE(no_offset.ok()) << no_offset.status().ToString();
  EXPECT_TRUE(std::holds_alternative<ProjectNode>(Nth(*no_offset, 0)));
}

// A grouped query sorts the GroupAggregate's output (keys, then aggregates): an ORDER BY aggregate
// equal to a select one reuses it, another one is computed as a hidden aggregate that the Project
// drops.
TEST(BinderTest, OrderByInAGroupedQuery) {
  const Catalog catalog = MakeCatalog();
  auto plan = BindSql(
      "SELECT s AS k, COUNT(*) AS n FROM t GROUP BY s, i16 "
      "ORDER BY count(*), MAX(d) DESC, n, i16, k LIMIT 10",
      catalog);
  ASSERT_TRUE(plan.ok()) << plan.status().ToString();
  ASSERT_EQ(plan->output.size(), 2U);
  EXPECT_EQ(std::get<LimitNode>(Nth(*plan, 0)).limit, 10);
  const auto& project = std::get<ProjectNode>(Nth(*plan, 1));
  ASSERT_EQ(project.columns.size(), 2U);
  EXPECT_EQ(project.columns[0].index, 0);
  EXPECT_EQ(project.columns[1].index, 2);
  const auto& sort = std::get<SortNode>(Nth(*plan, 2));
  ASSERT_EQ(sort.keys.size(), 4U);  // n repeats count(*)
  EXPECT_EQ(sort.keys[0].column.index, 2);
  EXPECT_EQ(sort.keys[1].column.index, 3);
  EXPECT_EQ(sort.keys[1].column.name, "max(d)");
  EXPECT_EQ(sort.keys[1].column.type, LogicalType::kDouble);
  EXPECT_TRUE(sort.keys[1].descending);
  EXPECT_EQ(sort.keys[2].column.index, 1);  // i16: the second key
  EXPECT_EQ(sort.keys[3].column.index, 0);  // k: s, the first key
  const auto& group = std::get<GroupAggregateNode>(Nth(*plan, 3));
  ASSERT_EQ(group.aggregates.size(), 2U);
  EXPECT_EQ(group.aggregates[1].kind, AggKind::kMax);
}

// HAVING filters the GroupAggregate's output (keys, then aggregates) below the Sort: an aggregate
// equal to a select one reuses it, another one is a hidden aggregate; a column is a key (a table
// column that is one comes before an alias) or the alias of a select item.
TEST(BinderTest, HavingInAGroupedQuery) {
  const Catalog catalog = MakeCatalog();
  constexpr std::string_view kSql =
      "SELECT s, i16 AS i32, COUNT(*) AS c FROM t GROUP BY s, i16 HAVING count(*) > 1 AND "
      "5 >= SUM(i64) AND i32 IN (1, 2) AND c <> 3 AND MIN(s) LIKE 'a%' AND s = 'x' "
      "ORDER BY MAX(d) LIMIT 4";
  auto plan = BindSql(kSql, catalog);
  ASSERT_TRUE(plan.ok()) << plan.status().ToString();
  EXPECT_EQ(std::get<LimitNode>(Nth(*plan, 0)).limit, 4);
  const auto& project = std::get<ProjectNode>(Nth(*plan, 1));
  ASSERT_EQ(project.columns.size(), 3U);
  EXPECT_EQ(project.columns[2].index, 2);
  const auto& sort = std::get<SortNode>(Nth(*plan, 2));
  ASSERT_EQ(sort.keys.size(), 1U);
  EXPECT_EQ(sort.keys[0].column.index, 5);  // MAX(d): hidden, after the HAVING ones
  const auto& filter = std::get<FilterNode>(Nth(*plan, 3));
  EXPECT_EQ(kSql.substr(filter.span.offset, filter.span.length),
            "count(*) > 1 AND 5 >= SUM(i64) AND i32 IN (1, 2) AND c <> 3 AND MIN(s) LIKE 'a%' "
            "AND s = 'x'");
  const auto& p = filter.predicates;
  ASSERT_EQ(p.size(), 6U);
  const auto column = [](const Predicate& predicate) {
    return predicate.column.value_or(BoundColumn{});
  };
  EXPECT_EQ(column(p[0]).index, 2);  // the select's COUNT(*)
  EXPECT_EQ(column(p[0]).name, "count_star()");
  EXPECT_EQ(p[0].op, CompareOp::kGt);
  EXPECT_EQ(column(p[1]).index, 3);  // SUM(i64): hidden
  EXPECT_EQ(column(p[1]).type, LogicalType::kHugeInt);
  EXPECT_EQ(p[1].op, CompareOp::kLe);
  EXPECT_EQ(p[1].constant.type, LogicalType::kHugeInt);
  EXPECT_EQ(p[2].kind, Predicate::Kind::kIn);
  EXPECT_EQ(column(p[2]).index, 1);  // i32: an alias of the key i16 (the column i32 is no key)
  EXPECT_EQ(p[2].values[0].type, LogicalType::kSmallInt);
  EXPECT_EQ(column(p[3]).index, 2);  // c: the alias of COUNT(*)
  EXPECT_EQ(p[4].kind, Predicate::Kind::kLike);
  EXPECT_EQ(column(p[4]).index, 4);  // MIN(s): hidden
  EXPECT_EQ(column(p[5]).index, 0);  // s: the first key
  const auto& group = std::get<GroupAggregateNode>(Nth(*plan, 4));
  ASSERT_EQ(group.aggregates.size(), 4U);
  EXPECT_EQ(group.aggregates[1].kind, AggKind::kSum);
  EXPECT_EQ(group.aggregates[2].kind, AggKind::kMin);
  EXPECT_EQ(group.aggregates[3].kind, AggKind::kMax);

  // A table column that is a key comes before a select alias of the same name (DuckDB).
  auto key = BindSql("SELECT COUNT(*) AS i16 FROM t GROUP BY i16 HAVING i16 > 1", catalog);
  ASSERT_TRUE(key.ok()) << key.status().ToString();
  const auto& key_filter = std::get<FilterNode>(Nth(*key, 1));
  EXPECT_EQ(key_filter.predicates[0].column.value_or(BoundColumn{}).index, 0);
  EXPECT_EQ(key_filter.predicates[0].constant.type, LogicalType::kSmallInt);
}

// HAVING without GROUP BY makes one row (an aggregate query, even of constants only) and may filter
// it out; a Project drops the hidden aggregates.
TEST(BinderTest, HavingInAGlobalAggregate) {
  const Catalog catalog = MakeCatalog();
  auto plan = BindSql("SELECT 1 AS one FROM t HAVING COUNT(*) > 0 AND MAX(d) < 1.5", catalog);
  ASSERT_TRUE(plan.ok()) << plan.status().ToString();
  const auto& project = std::get<ProjectNode>(Nth(*plan, 0));
  ASSERT_EQ(project.columns.size(), 1U);
  ASSERT_EQ(project.constants.size(), 1U);
  EXPECT_TRUE(project.constants[0].has_value());
  const auto& filter = std::get<FilterNode>(Nth(*plan, 1));
  ASSERT_EQ(filter.predicates.size(), 2U);
  EXPECT_EQ(filter.predicates[1].column.value_or(BoundColumn{}).index, 1);
  EXPECT_EQ(std::get<double>(filter.predicates[1].constant.value), 1.5);
  EXPECT_EQ(std::get<AggregateNode>(Nth(*plan, 2)).aggregates.size(), 2U);

  auto hidden = BindSql("SELECT COUNT(*) AS n FROM t HAVING SUM(i16) >= 0 AND n > 1", catalog);
  ASSERT_TRUE(hidden.ok()) << hidden.status().ToString();
  const auto& drop = std::get<ProjectNode>(Nth(*hidden, 0));
  ASSERT_EQ(drop.columns.size(), 1U);
  EXPECT_EQ(drop.columns[0].index, 0);
  const auto& predicates = std::get<FilterNode>(Nth(*hidden, 1)).predicates;
  EXPECT_EQ(predicates[0].column.value_or(BoundColumn{}).index, 1);
  EXPECT_EQ(predicates[1].column.value_or(BoundColumn{}).index, 0);

  // No Project when nothing is hidden; a literal outside BIGINT folds as in WHERE.
  auto plain = BindSql("SELECT COUNT(*) FROM t HAVING COUNT(*) < -9223372036854775809", catalog);
  ASSERT_TRUE(plain.ok()) << plain.status().ToString();
  const auto& never = std::get<FilterNode>(Nth(*plain, 0));
  EXPECT_EQ(never.predicates[0].kind, Predicate::Kind::kFalse);
  EXPECT_TRUE(std::holds_alternative<AggregateNode>(Nth(*plain, 1)));
}

// Without GROUP BY an aggregate query has one row: ORDER BY is checked, but needs no Sort.
TEST(BinderTest, OrderByInAGlobalAggregateNeedsNoSort) {
  const Catalog catalog = MakeCatalog();
  auto plan = BindSql("SELECT COUNT(*) AS c FROM t ORDER BY c, MAX(s) OFFSET 1", catalog);
  ASSERT_TRUE(plan.ok()) << plan.status().ToString();
  EXPECT_EQ(std::get<LimitNode>(Nth(*plan, 0)).offset, 1);
  const auto& aggregate = std::get<AggregateNode>(Nth(*plan, 1));
  EXPECT_EQ(aggregate.aggregates.size(), 1U);
}

// COUNT(DISTINCT col) is its own aggregate kind (BIGINT), named like DuckDB; in ORDER BY it reuses
// an equal select aggregate, and COUNT(col) is not equal to it.
TEST(BinderTest, CountDistinct) {
  const Catalog catalog = MakeCatalog();
  auto plan = BindSql(
      "SELECT s, COUNT(DISTINCT \"Mixed Case\"), count(distinct I32) AS n FROM t GROUP BY s "
      "ORDER BY COUNT(DISTINCT i32) DESC, COUNT(i32)",
      catalog);
  ASSERT_TRUE(plan.ok()) << plan.status().ToString();
  ASSERT_EQ(plan->output.size(), 3U);
  EXPECT_EQ(plan->output[1].name, "count(DISTINCT \"Mixed Case\")");
  EXPECT_EQ(plan->output[1].type, LogicalType::kBigInt);
  EXPECT_EQ(plan->output[2].name, "n");
  const auto& sort = std::get<SortNode>(Nth(*plan, 1));
  ASSERT_EQ(sort.keys.size(), 2U);
  EXPECT_EQ(sort.keys[0].column.index, 2);  // the select's COUNT(DISTINCT i32)
  EXPECT_EQ(sort.keys[1].column.index, 3);  // COUNT(i32): hidden
  const auto& group = std::get<GroupAggregateNode>(Nth(*plan, 2));
  ASSERT_EQ(group.aggregates.size(), 3U);
  EXPECT_EQ(group.aggregates[0].kind, AggKind::kCountDistinct);
  EXPECT_EQ(group.aggregates[1].kind, AggKind::kCountDistinct);
  EXPECT_EQ(group.aggregates[2].kind, AggKind::kCount);
  auto name = BindSql("SELECT COUNT(DISTINCT \"i32\") FROM t", catalog);
  ASSERT_TRUE(name.ok()) << name.status().ToString();
  EXPECT_EQ(name->output[0].name, "count(DISTINCT i32)");
}

// Parentheses group without changing anything: a parenthesized column, aggregate argument or
// condition binds as without them, and a parenthesized AND is flattened into the conjunction.
// Arithmetic takes DuckDB's result types: an integer literal that fits the other operand's type
// takes that type, two integer types the wider one (USMALLINT with SMALLINT: BIGINT), DOUBLE wins,
// and / is always DOUBLE. Names are DuckDB's, fully parenthesized.
TEST(BinderTest, ArithmeticTypesAndNamesLikeDuckDb) {
  const Catalog catalog = MakeCatalog();
  struct Case {
    std::string_view expr;
    LogicalType type;
    std::string_view name;
  };
  for (const Case& c : {
           Case{.expr = "i16 + 1", .type = LogicalType::kSmallInt, .name = "(i16 + 1)"},
           Case{.expr = "i16 + 40000", .type = LogicalType::kInteger, .name = "(i16 + 40000)"},
           Case{.expr = "i16 + -32768", .type = LogicalType::kSmallInt, .name = "(i16 + -32768)"},
           Case{.expr = "i16 - i32", .type = LogicalType::kInteger, .name = "(i16 - i32)"},
           Case{.expr = "i32 * i64", .type = LogicalType::kBigInt, .name = "(i32 * i64)"},
           Case{.expr = "u16 + i16", .type = LogicalType::kBigInt, .name = "(u16 + i16)"},
           Case{.expr = "u16 + i32", .type = LogicalType::kInteger, .name = "(u16 + i32)"},
           Case{.expr = "u16 * 2", .type = LogicalType::kUSmallInt, .name = "(u16 * 2)"},
           Case{.expr = "u16 + -1", .type = LogicalType::kInteger, .name = "(u16 + -1)"},
           Case{.expr = "i16 / 2", .type = LogicalType::kDouble, .name = "(i16 / 2)"},
           Case{.expr = "i16 / 1.5", .type = LogicalType::kDouble, .name = "(i16 / 1.5)"},
           Case{.expr = "i16 // 1.5", .type = LogicalType::kDouble, .name = "(i16 // 1.5)"},
           Case{.expr = "i64 // 2", .type = LogicalType::kBigInt, .name = "(i64 // 2)"},
           Case{.expr = "i16 % 2", .type = LogicalType::kSmallInt, .name = "(i16 % 2)"},
           Case{.expr = "d + 1.5", .type = LogicalType::kDouble, .name = "(d + 1.5)"},
           Case{.expr = "i64 + d", .type = LogicalType::kDouble, .name = "(i64 + d)"},
           Case{.expr = "i32 + 1e3", .type = LogicalType::kDouble, .name = "(i32 + 1e3)"},
           Case{.expr = "-i16", .type = LogicalType::kSmallInt, .name = "-(i16)"},
           Case{.expr = "-(i16 + 1) * 2",
                .type = LogicalType::kSmallInt,
                .name = "(-((i16 + 1)) * 2)"},
           // A minus over a number is part of the literal, which takes the other operand's type; a
           // cast binds tighter, so a minus over one is an operation of the cast's type.
           Case{.expr = "i16 + -(1)", .type = LogicalType::kSmallInt, .name = "(i16 + -1)"},
           Case{.expr = "i16 * - -2", .type = LogicalType::kSmallInt, .name = "(i16 * 2)"},
           Case{.expr = "i16 - -(-(3))", .type = LogicalType::kSmallInt, .name = "(i16 - 3)"},
           Case{.expr = "i16 + -(1)::INTEGER",
                .type = LogicalType::kInteger,
                .name = "(i16 + -(CAST(1 AS INTEGER)))"},
           Case{.expr = "-(-(1))::SMALLINT",
                .type = LogicalType::kSmallInt,
                .name = "-(CAST(-1 AS SMALLINT))"},
           Case{.expr = "1 + 2", .type = LogicalType::kInteger, .name = "(1 + 2)"},
           Case{.expr = "SUM(i16) + 1", .type = LogicalType::kHugeInt, .name = "(sum(i16) + 1)"},
           Case{.expr = "\"Mixed Case\" + 007",
                .type = LogicalType::kInteger,
                .name = "(\"Mixed Case\" + 7)"},
       }) {
    const std::string sql = "SELECT " + std::string(c.expr) + " FROM t";
    auto plan = BindSql(sql, catalog);
    ASSERT_TRUE(plan.ok()) << sql << ": " << plan.status().ToString();
    ASSERT_EQ(plan->output.size(), 1U);
    EXPECT_EQ(plan->output[0].type, c.type) << sql;
    EXPECT_EQ(plan->output[0].name, c.name) << sql;
  }
}

// DuckDB's DECIMAL arithmetic types (ADR 0021 rules 4 to 6, each probed with DuckDB 1.5.5): an
// integer counts as DECIMAL(5,0), (10,0), (19,0) or (38,0) by its type, a literal too; + and -
// keep the larger scale and one more digit, * adds both; beyond 18 digits from two operands of at
// most 18 the width is 18 (for *, unless the scale reaches 18), beyond 38 it is 38.
TEST(BinderTest, DecimalArithmeticTypesLikeDuckDb) {
  const Catalog catalog = MakeCatalog();
  struct Case {
    std::string_view expr;
    LogicalType type;
  };
  const auto dec = [](int width, int scale) {
    return LogicalType::Decimal(static_cast<std::uint8_t>(width), static_cast<std::uint8_t>(scale));
  };
  for (const Case& c : {
           Case{.expr = "p + q", .type = dec(16, 2)},
           Case{.expr = "p - r", .type = dec(18, 4)},
           Case{.expr = "p * q", .type = dec(18, 4)},
           Case{.expr = "p * r", .type = dec(18, 6)},
           Case{.expr = "r * r", .type = dec(18, 8)},
           Case{.expr = "p + i", .type = dec(16, 2)},
           Case{.expr = "i * r", .type = dec(18, 4)},
           Case{.expr = "r - i", .type = dec(15, 4)},
           Case{.expr = "s16 + r", .type = dec(10, 4)},
           Case{.expr = "u16 * r", .type = dec(14, 4)},
           Case{.expr = "b - r", .type = dec(24, 4)},
           Case{.expr = "p * b", .type = dec(34, 2)},
           Case{.expr = "p + 7", .type = dec(16, 2)},
           Case{.expr = "7 * r", .type = dec(18, 4)},
           Case{.expr = "p * 3000000000", .type = dec(34, 2)},
           Case{.expr = "p * 100000000000000000000", .type = dec(38, 2)},
           Case{.expr = "z + i", .type = dec(38, 10)},
           Case{.expr = "z * p", .type = dec(38, 12)},
           Case{.expr = "z * z", .type = dec(38, 20)},
           Case{.expr = "e + g", .type = dec(18, 10)},
           Case{.expr = "e * g", .type = dec(36, 18)},
           Case{.expr = "g * g", .type = dec(36, 20)},
           Case{.expr = "p * q * r", .type = dec(18, 8)},
           Case{.expr = "-p", .type = dec(15, 2)},
           Case{.expr = "-(p * q)", .type = dec(18, 4)},
           Case{.expr = "SUM(p)", .type = dec(38, 2)},
           Case{.expr = "SUM(p * q)", .type = dec(38, 4)},
           Case{.expr = "SUM(z)", .type = dec(38, 10)},
           Case{.expr = "AVG(r)", .type = LogicalType::kDouble},
           Case{.expr = "MIN(p) + 1", .type = dec(16, 2)},
           Case{.expr = "SUM(p) + 1", .type = dec(38, 2)},
           Case{.expr = "SUM(p) * 2", .type = dec(38, 2)},
           Case{.expr = "SUM(i) + MIN(p)", .type = dec(38, 2)},
       }) {
    const std::string sql = "SELECT " + std::string(c.expr) + " FROM dec";
    auto plan = BindSql(sql, catalog);
    ASSERT_TRUE(plan.ok()) << sql << ": " << plan.status().ToString();
    ASSERT_EQ(plan->output.size(), 1U);
    EXPECT_EQ(plan->output[0].type, c.type) << sql << ": " << ToString(plan->output[0].type);
  }
}

// Decimal literals, / // % and DOUBLE operands as DuckDB types them (ADR 0021 rules 3, 8 and 9,
// each probed with DuckDB 1.5.5): a decimal literal is DECIMAL(digits, fraction digits), leading
// zeros included, and named by its value; / and // with a DECIMAL are DOUBLE, as is anything with a
// DOUBLE; % takes the common type without a cap to 18 digits.
TEST(BinderTest, DecimalLiteralsAndDivisionTypesLikeDuckDb) {
  const Catalog catalog = MakeCatalog();
  struct Case {
    std::string_view expr;
    LogicalType type;
    std::string_view name;
  };
  const auto dec = [](int width, int scale) {
    return LogicalType::Decimal(static_cast<std::uint8_t>(width), static_cast<std::uint8_t>(scale));
  };
  for (const Case& c : {
           Case{.expr = "2.5", .type = dec(2, 1), .name = "2.5"},
           Case{.expr = ".125", .type = dec(3, 3), .name = ".125"},
           Case{.expr = "007.50", .type = dec(5, 2), .name = "7.50"},
           Case{.expr = "5.", .type = dec(1, 0), .name = "5"},
           Case{.expr = "-0.5", .type = dec(2, 1), .name = "-0.5"},
           Case{.expr = "12345678901234567890123456789012345678.5 + 0",
                .type = LogicalType::kDouble,
                .name = "(12345678901234567890123456789012345678.5 + 0)"},
           Case{.expr = "i + 1.5", .type = dec(12, 1), .name = "(i + 1.5)"},
           Case{.expr = "s16 + 1.5", .type = dec(7, 1), .name = "(s16 + 1.5)"},
           Case{.expr = "i - 2.25", .type = dec(13, 2), .name = "(i - 2.25)"},
           Case{.expr = "b * 1.5", .type = dec(21, 1), .name = "(b * 1.5)"},
           Case{.expr = "1.5 + 2", .type = dec(12, 1), .name = "(1.5 + 2)"},
           Case{.expr = "1.5 * 2.25", .type = dec(5, 3), .name = "(1.5 * 2.25)"},
           Case{.expr = "-(1.5)", .type = dec(2, 1), .name = "-1.5"},
           Case{.expr = "p + 0.5", .type = dec(16, 2), .name = "(p + 0.5)"},
           Case{.expr = "p * 1.5", .type = dec(17, 3), .name = "(p * 1.5)"},
           Case{.expr = "e + 0.00005", .type = dec(18, 8), .name = "(e + 0.00005)"},
           Case{.expr = "p - 007.50", .type = dec(16, 2), .name = "(p - 7.50)"},
           Case{.expr = "SUM(i) + 1.5", .type = dec(38, 1), .name = "(sum(i) + 1.5)"},
           Case{.expr = "p / 2", .type = LogicalType::kDouble, .name = "(p / 2)"},
           Case{.expr = "p // 2", .type = LogicalType::kDouble, .name = "(p // 2)"},
           Case{.expr = "i // 1.5", .type = LogicalType::kDouble, .name = "(i // 1.5)"},
           Case{.expr = "i / 1.5", .type = LogicalType::kDouble, .name = "(i / 1.5)"},
           Case{.expr = "p // r", .type = LogicalType::kDouble, .name = "(p // r)"},
           Case{.expr = "p + f", .type = LogicalType::kDouble, .name = "(p + f)"},
           Case{.expr = "p * f", .type = LogicalType::kDouble, .name = "(p * f)"},
           Case{.expr = "f % p", .type = LogicalType::kDouble, .name = "(f % p)"},
           Case{.expr = "f + 1.5", .type = LogicalType::kDouble, .name = "(f + 1.5)"},
           Case{.expr = "p % 7", .type = dec(15, 2), .name = "(p % 7)"},
           Case{.expr = "p % q", .type = dec(15, 2), .name = "(p % q)"},
           Case{.expr = "p % r", .type = dec(17, 4), .name = "(p % r)"},
           Case{.expr = "e % g", .type = dec(20, 10), .name = "(e % g)"},
           Case{.expr = "i % 2.5", .type = dec(11, 1), .name = "(i % 2.5)"},
           Case{.expr = "b % 2.5", .type = dec(20, 1), .name = "(b % 2.5)"},
           Case{.expr = "z % 7", .type = dec(38, 10), .name = "(z % 7)"},
           Case{.expr = "z % e", .type = dec(38, 10), .name = "(z % e)"},
           // Beyond 38 digits % is DOUBLE.
           Case{.expr = "z % 0.00000000001",
                .type = LogicalType::kDouble,
                .name = "(z % 0.00000000001)"},
           Case{.expr = "SUM(b) % 2.5", .type = LogicalType::kDouble, .name = "(sum(b) % 2.5)"},
           Case{.expr = "7 % 2.5", .type = dec(11, 1), .name = "(7 % 2.5)"},
       }) {
    const std::string sql = "SELECT " + std::string(c.expr) + " FROM dec";
    auto plan = BindSql(sql, catalog);
    ASSERT_TRUE(plan.ok()) << sql << ": " << plan.status().ToString();
    ASSERT_EQ(plan->output.size(), 1U);
    EXPECT_EQ(plan->output[0].type, c.type) << sql << ": " << ToString(plan->output[0].type);
    EXPECT_EQ(plan->output[0].name, c.name) << sql;
  }
}

// CASE values with a DECIMAL fold as in DuckDB 1.5.5 (typeof; ADR 0021 rule 10): from the ELSE
// through the THENs in written order, so the order of the values matters; an integer literal (also
// negated) takes an integer type it fits but counts by its own type next to a DECIMAL, `1 + 1` and
// `7 + 0` count by their type; beyond 38 digits two DECIMALs lose scale, an integer keeps it; a
// DOUBLE value makes the CASE DOUBLE.
TEST(BinderTest, DecimalCaseValuesFoldLikeDuckDb) {
  const Catalog catalog = MakeCatalog();
  struct Case {
    std::string_view table;
    std::string_view expr;
    LogicalType type;
  };
  const auto dec = [](int width, int scale) {
    return LogicalType::Decimal(static_cast<std::uint8_t>(width), static_cast<std::uint8_t>(scale));
  };
  const LogicalType real = LogicalType::kDouble;
  for (const Case& c : {
           Case{.table = "dec", .expr = "CASE WHEN i > 0 THEN p ELSE 7 END", .type = dec(15, 2)},
           Case{.table = "dec", .expr = "CASE WHEN i > 0 THEN r ELSE 7 END", .type = dec(14, 4)},
           Case{.table = "dec", .expr = "CASE WHEN i > 0 THEN p ELSE b END", .type = dec(21, 2)},
           Case{.table = "dec", .expr = "CASE WHEN i > 0 THEN p ELSE 2.5 END", .type = dec(15, 2)},
           Case{.table = "dec", .expr = "CASE WHEN i > 0 THEN p END", .type = dec(15, 2)},
           // The order of the values: 7 next to s16 is SMALLINT, next to r INTEGER.
           Case{.table = "dec",
                .expr = "CASE WHEN i > 0 THEN 7 WHEN i > 1 THEN r ELSE s16 END",
                .type = dec(9, 4)},
           Case{.table = "dec",
                .expr = "CASE WHEN i > 0 THEN r WHEN i > 1 THEN 7 ELSE s16 END",
                .type = dec(14, 4)},
           Case{.table = "dec",
                .expr = "CASE WHEN i > 0 THEN u16 WHEN i > 1 THEN r ELSE s16 END",
                .type = dec(14, 4)},
           Case{.table = "dec",
                .expr = "CASE WHEN i > 0 THEN r WHEN i > 1 THEN u16 ELSE s16 END",
                .type = dec(9, 4)},
           Case{.table = "dec",
                .expr = "CASE WHEN i > 0 THEN 1 + 1 WHEN i > 1 THEN r ELSE s16 END",
                .type = dec(14, 4)},
           Case{.table = "dec",
                .expr = "CASE WHEN i > 0 THEN 7 + 0 WHEN i > 1 THEN r ELSE s16 END",
                .type = dec(14, 4)},
           Case{.table = "dec",
                .expr = "CASE WHEN i > 0 THEN -(7) WHEN i > 1 THEN r ELSE s16 END",
                .type = dec(9, 4)},
           Case{.table = "dec",
                .expr = "CASE WHEN i > 0 THEN - - 7 WHEN i > 1 THEN r ELSE s16 END",
                .type = dec(9, 4)},
           Case{.table = "dec",
                .expr = "CASE WHEN i > 0 THEN 40000 WHEN i > 1 THEN r ELSE s16 END",
                .type = dec(14, 4)},
           Case{.table = "dec",
                .expr = "CASE WHEN i > 0 THEN -7 WHEN i > 1 THEN r ELSE u16 END",
                .type = dec(14, 4)},
           Case{.table = "dec", .expr = "CASE WHEN i > 0 THEN 7 ELSE p END", .type = dec(15, 2)},
           Case{.table = "dec",
                .expr = "CASE WHEN i > 0 THEN 7 WHEN i > 1 THEN p END",
                .type = dec(15, 2)},
           Case{.table = "dec",
                .expr = "CASE WHEN i > 0 THEN 7 WHEN i > 1 THEN 8 ELSE p END",
                .type = dec(15, 2)},
           Case{.table = "dec",
                .expr = "CASE WHEN i > 0 THEN p WHEN i > 1 THEN 7 ELSE 8 END",
                .type = dec(15, 2)},
           Case{.table = "dec",
                .expr = "CASE WHEN i > 0 THEN 3000000000 ELSE p END",
                .type = dec(21, 2)},
           Case{.table = "dec",
                .expr = "CASE WHEN i > 0 THEN 100000000000000000000 ELSE p END",
                .type = dec(38, 2)},
           Case{.table = "dec", .expr = "CASE WHEN i > 0 THEN 2.5 ELSE 7 END", .type = dec(11, 1)},
           Case{
               .table = "dec", .expr = "CASE WHEN i > 0 THEN 2.5 ELSE 1.25 END", .type = dec(3, 2)},
           Case{.table = "dec",
                .expr = "CASE p WHEN 1.5 THEN r WHEN q THEN 7 ELSE e END",
                .type = dec(18, 8)},
           // Beyond 38 digits.
           Case{.table = "dec",
                .expr = "CASE WHEN i > 0 THEN z WHEN i > 1 THEN p ELSE r END",
                .type = dec(38, 10)},
           Case{.table = "dec", .expr = "CASE WHEN i > 0 THEN z ELSE b END", .type = dec(38, 10)},
           Case{.table = "dec", .expr = "CASE WHEN i > 0 THEN g ELSE b END", .type = dec(29, 10)},
           Case{.table = "dec",
                .expr = "CASE WHEN i > 0 THEN e WHEN i > 1 THEN g ELSE z END",
                .type = dec(38, 10)},
           Case{.table = "dec",
                .expr = "CASE WHEN i > 0 THEN z WHEN i > 1 THEN e ELSE "
                        "0.000000000000000000000000000001 END",
                .type = dec(38, 10)},
           Case{.table = "dec",
                .expr = "CASE WHEN i > 0 THEN b ELSE 0.000000000000000000000000000001 END",
                .type = dec(38, 30)},
           Case{.table = "t", .expr = "CASE WHEN i32 > 0 THEN h ELSE 1.5 END", .type = dec(38, 0)},
           Case{.table = "t", .expr = "CASE WHEN i32 > 0 THEN h ELSE i64 END", .type = dec(38, 0)},
           Case{.table = "t",
                .expr = "CASE WHEN i32 > 0 THEN -(1.5) ELSE h END",
                .type = dec(38, 0)},
           Case{.table = "t", .expr = "CASE WHEN i32 > 0 THEN 1.5 ELSE i16 END", .type = dec(6, 1)},
           Case{.table = "t",
                .expr = "CASE WHEN i32 > 0 THEN i16 WHEN i32 > 1 THEN 2.5 ELSE 7 END",
                .type = dec(6, 1)},
           Case{.table = "t",
                .expr = "CASE WHEN i32 > 0 THEN 2.5 WHEN i32 > 1 THEN i16 ELSE 7 END",
                .type = dec(11, 1)},
           // THEN NULL (ADR 0037's NL2) leaves a type as it is and makes a literal its own type,
           // as a missing ELSE does: 7 is INTEGER next to NULL, SMALLINT next to s16.
           Case{.table = "dec", .expr = "CASE WHEN i > 0 THEN NULL ELSE r END", .type = dec(9, 4)},
           Case{
               .table = "dec",
               .expr = "CASE WHEN i > 0 THEN NULL WHEN i > 1 THEN s16 WHEN i > 2 THEN r ELSE 7 END",
               .type = dec(14, 4)},
           Case{
               .table = "dec",
               .expr = "CASE WHEN i > 0 THEN s16 WHEN i > 1 THEN NULL WHEN i > 2 THEN r ELSE 7 END",
               .type = dec(9, 4)},
           Case{.table = "dec",
                .expr = "CASE WHEN i > 0 THEN r WHEN i > 1 THEN NULL ELSE 7 END",
                .type = dec(14, 4)},
           // A DOUBLE value, in any position.
           Case{.table = "dec", .expr = "CASE WHEN i = 1 THEN 1.5 ELSE f END", .type = real},
           Case{.table = "dec", .expr = "CASE WHEN i = 1 THEN f ELSE 1.5 END", .type = real},
           Case{.table = "dec", .expr = "CASE WHEN i = 1 THEN 1.5 ELSE 1e3 END", .type = real},
           Case{.table = "dec", .expr = "CASE WHEN i = 1 THEN -(1.5) ELSE f END", .type = real},
           Case{.table = "dec",
                .expr = "CASE WHEN i = 1 THEN p * q WHEN i = 2 THEN 7 ELSE f END",
                .type = real},
           Case{.table = "t", .expr = "CASE WHEN i32 > 0 THEN h ELSE d END", .type = real},
       }) {
    const std::string sql = "SELECT " + std::string(c.expr) + " FROM " + std::string(c.table);
    auto plan = BindSql(sql, catalog);
    ASSERT_TRUE(plan.ok()) << sql << ": " << plan.status().ToString();
    EXPECT_EQ(ToString(plan->output[0].type), ToString(c.type)) << sql;
  }
}

// Without a DECIMAL value antb1 types the CASE as before (divergence D20): the values that are no
// literals first, then each integer literal takes their type when it fits; DuckDB folds from the
// ELSE through the THENs, where a literal next to NULL (the first THEN without an ELSE) or another
// literal becomes its own type, and such a string literal is VARCHAR.
TEST(BinderTest, CaseTypesWithoutADecimalAreAntb1s) {
  const Catalog catalog = MakeCatalog();
  struct Case {
    std::string_view expr;
    LogicalType type;
  };
  for (const Case& c : {
           // DuckDB: INTEGER, INTEGER and a bind error (VARCHAR and DATE).
           Case{.expr = "CASE WHEN i32 = 1 THEN 7 WHEN i32 = 2 THEN i16 END",
                .type = LogicalType::kSmallInt},
           Case{.expr = "CASE WHEN i32 = 1 THEN 8 WHEN i32 = 2 THEN i16 ELSE 7 END",
                .type = LogicalType::kSmallInt},
           Case{.expr = "CASE WHEN i32 = 1 THEN '2020-01-01' WHEN i32 = 2 THEN dt END",
                .type = LogicalType::kDate},
           // DuckDB: INTEGER, its 7 next to the THEN NULL.
           Case{.expr = "CASE WHEN i32 = 1 THEN NULL WHEN i32 = 2 THEN i16 ELSE 7 END",
                .type = LogicalType::kSmallInt},
           // As DuckDB: -(7) is the literal -7 (the parser folds the minus into it), which takes
           // the type of i16.
           Case{.expr = "CASE WHEN i32 = 1 THEN -(7) ELSE i16 END", .type = LogicalType::kSmallInt},
       }) {
    const std::string sql = "SELECT " + std::string(c.expr) + " FROM t";
    auto plan = BindSql(sql, catalog);
    ASSERT_TRUE(plan.ok()) << sql << ": " << plan.status().ToString();
    EXPECT_EQ(plan->output[0].type, c.type) << sql;
  }
  // DuckDB: a bind error (VARCHAR and SMALLINT).
  const auto string_first = BindSql(
      "SELECT CASE WHEN i32 = 1 THEN 'a' WHEN i32 = 2 THEN i16 END "
      "FROM t",
      catalog);
  ASSERT_FALSE(string_first.ok());
  const auto detail = GetSqlError(string_first.status());
  ASSERT_NE(detail, nullptr);
  EXPECT_EQ(detail->kind(), SqlErrorDetail::Kind::kUnsupported);
}

// THEN NULL (ADR 0037's NL2) is a null value of the CASE: the identity of its type, named NULL as
// DuckDB names it. A CASE of NULLs only has DuckDB's NULL type, which antb1 has not (exit 4); a
// string literal made VARCHAR by a NULL does not mix with a DECIMAL (exit 1, as in DuckDB).
TEST(BinderTest, ThenNullIsANullValueOfTheCase) {
  const Catalog catalog = MakeCatalog();
  for (const auto& [sql, type] : {
           std::pair{"SELECT CASE WHEN i32 = 1 THEN NULL ELSE i16 END FROM t",
                     LogicalType::kSmallInt},
           std::pair{"SELECT CASE i32 WHEN 1 THEN i64 WHEN 2 THEN NULL END FROM t",
                     LogicalType::kBigInt},
           std::pair{"SELECT CASE WHEN i32 = 1 THEN NULL ELSE 'x' END FROM t",
                     LogicalType::kVarchar},
           std::pair{"SELECT CASE WHEN i32 = 1 THEN NULL WHEN i32 = 2 THEN 7 END FROM t",
                     LogicalType::kInteger},
       }) {
    auto plan = BindSql(sql, catalog);
    ASSERT_TRUE(plan.ok()) << sql << ": " << plan.status().ToString();
    EXPECT_EQ(plan->output[0].type, type) << sql;
  }
  auto named = BindSql("SELECT CASE i32 WHEN 1 THEN NULL ELSE i16 END FROM t", catalog);
  ASSERT_TRUE(named.ok()) << named.status().ToString();
  EXPECT_EQ(named->output[0].name, "CASE  WHEN ((i32 = 1)) THEN (NULL) ELSE i16 END");
  for (const auto& [sql, kind] : {
           std::pair{"SELECT CASE WHEN i32 = 1 THEN NULL END FROM t",
                     SqlErrorDetail::Kind::kUnsupported},
           std::pair{
               "SELECT CASE WHEN i32 = 1 THEN NULL WHEN i32 = 2 THEN NULL ELSE NULL END FROM t",
               SqlErrorDetail::Kind::kUnsupported},
           std::pair{"SELECT CASE WHEN i > 0 THEN NULL WHEN i > 1 THEN r ELSE 'x' END FROM dec",
                     SqlErrorDetail::Kind::kBind},
       }) {
    const auto plan = BindSql(sql, catalog);
    ASSERT_FALSE(plan.ok()) << sql;
    const auto detail = GetSqlError(plan.status());
    ASSERT_NE(detail, nullptr) << sql;
    EXPECT_EQ(detail->kind(), kind) << sql << ": " << plan.status().ToString();
  }
}

// No integer-only rewrite applies to DECIMAL (ADR 0021 rule 18): SUM(p + 1) sums p + 1, and
// p + 1 > 2 compares p + 1 (DuckDB moves neither).
TEST(BinderTest, DecimalIsNotRewrittenLikeIntegers) {
  const Catalog catalog = MakeCatalog();
  auto sum = BindSql("SELECT SUM(p + 1) FROM dec", catalog);
  ASSERT_TRUE(sum.ok()) << sum.status().ToString();
  EXPECT_EQ(sum->output[0].type, LogicalType::Decimal(38, 2));
  EXPECT_TRUE(Explain(*sum).contains("Aggregate SUM(\"(p + 1)\")")) << Explain(*sum);
  auto moved = BindSql("SELECT COUNT(*) FROM dec WHERE p + 1 > 2", catalog);
  ASSERT_TRUE(moved.ok()) << moved.status().ToString();
  EXPECT_TRUE(Explain(*moved).contains("Filter \"(p + 1)\" > 2.00")) << Explain(*moved);
}

// DuckDB's sum rewriter: SUM(x + c) is SUM(x) + c * COUNT(x) in HUGEINT for a signed integer x, so
// x + c is never computed (and never overflows); SUM(x - c), SUM(x * c) and a DOUBLE x are not.
TEST(BinderTest, SumOfAnIntegerPlusAConstantIsRewritten) {
  const Catalog catalog = MakeCatalog();
  auto plan =
      BindSql("SELECT SUM(i16 + 1), SUM(2 + (i16 + 3)), SUM(i16 - 1), SUM(d + 1) FROM t", catalog);
  ASSERT_TRUE(plan.ok()) << plan.status().ToString();
  EXPECT_EQ(plan->output[0].name, "sum((i16 + 1))");
  EXPECT_EQ(plan->output[0].type, LogicalType::kHugeInt);
  EXPECT_EQ(plan->output[1].type, LogicalType::kHugeInt);
  const auto& project = std::get<ProjectNode>(Nth(*plan, 0));
  ASSERT_EQ(project.columns.size(), 4U);
  const auto& post = std::get<ComputeNode>(Nth(*plan, 1));
  ASSERT_EQ(post.exprs.size(), 2U) << "one expression per rewritten SUM";
  EXPECT_EQ(post.exprs[0]->name, "sum((i16 + 1))");
  EXPECT_EQ(post.exprs[1]->name, "sum((2 + (i16 + 3)))");
  const auto& aggregate = std::get<AggregateNode>(Nth(*plan, 2));
  // SUM(i16 - 1) and SUM(d + 1) (the select's), then SUM(i16) and COUNT(i16) (hidden, shared by
  // both rewrites).
  ASSERT_EQ(aggregate.aggregates.size(), 4U);
  EXPECT_EQ(aggregate.aggregates[0].arg.value_or(BoundColumn{}).name, "(i16 - 1)");
  EXPECT_EQ(aggregate.aggregates[2].kind, AggKind::kSum);
  EXPECT_EQ(aggregate.aggregates[2].arg.value_or(BoundColumn{}).name, "i16");
  EXPECT_EQ(aggregate.aggregates[3].kind, AggKind::kCount);
  const auto& input = std::get<ComputeNode>(Nth(*plan, 3));
  ASSERT_EQ(input.exprs.size(), 2U);  // i16 - 1 and d + 1 are computed
  EXPECT_EQ(input.exprs[0]->name, "(i16 - 1)");
  EXPECT_EQ(input.exprs[1]->name, "(d + 1)");
}

// The constant of the sum rewrite is folded in DuckDB's types: 2147483647 + 1 and 65536 * 32768
// overflow INTEGER, which DuckDB leaves unfolded, so they are no constants, the SUM is not
// rewritten and its addition is computed, failing on a row as in DuckDB. 2147483646 + 1 fits, and
// is.
TEST(BinderTest, AConstantThatOverflowsItsTypeIsNoneToTheSumRewrite) {
  const Catalog catalog = MakeCatalog();
  for (const std::string_view sql :
       {"SELECT SUM(i32 + (2147483647 + 1)) FROM t", "SELECT SUM((2147483647 + 1) + i32) FROM t",
        "SELECT SUM(i32 + 65536 * 32768) FROM t", "SELECT SUM(i32 + -(-2147483647 - 1)) FROM t"}) {
    auto plan = BindSql(sql, catalog);
    ASSERT_TRUE(plan.ok()) << sql << ": " << plan.status().ToString();
    EXPECT_FALSE(Explain(*plan).contains("COUNT(")) << sql << "\n" << Explain(*plan);
  }
  auto fits = BindSql("SELECT SUM(i32 + (2147483646 + 1)) FROM t", catalog);
  ASSERT_TRUE(fits.ok()) << fits.status().ToString();
  EXPECT_TRUE(Explain(*fits).contains("COUNT(")) << Explain(*fits);
}

// GROUP BY an expression computes it below the aggregation; a select or ORDER BY expression equal
// to a key is that key, others over keys and aggregates are computed above the aggregation, and
// HAVING's below them, under its filter: DuckDB filters HAVING before it computes the select list.
TEST(BinderTest, ExpressionsBelowAndAboveTheAggregation) {
  const Catalog catalog = MakeCatalog();
  auto plan = BindSql(
      "SELECT i32 - 1 AS k, (i32 - 1) * 2, SUM(i16) * 2 AS s, COUNT(*) FROM t WHERE i16 // 2 > 0 "
      "GROUP BY i32 - 1 HAVING SUM(i16) + 1 > COUNT(*) ORDER BY -(i32 - 1), s LIMIT 5",
      catalog);
  ASSERT_TRUE(plan.ok()) << plan.status().ToString();
  EXPECT_EQ(plan->output[0].type, LogicalType::kInteger);
  EXPECT_EQ(plan->output[1].name, "((i32 - 1) * 2)");
  EXPECT_EQ(plan->output[2].type, LogicalType::kHugeInt);
  // Limit <- Project <- Sort <- Compute (select and ORDER BY) <- Filter (HAVING) <- Compute (its
  // operand) <- GroupAggregate <- Compute (the key) <- Filter (WHERE) <- Compute (its operand) <-
  // Scan.
  EXPECT_TRUE(std::holds_alternative<LimitNode>(Nth(*plan, 0)));
  EXPECT_TRUE(std::holds_alternative<ProjectNode>(Nth(*plan, 1)));
  const auto& sort = std::get<SortNode>(Nth(*plan, 2));
  ASSERT_EQ(sort.keys.size(), 2U);
  const auto names = [](const ComputeNode& compute) {
    std::vector<std::string> out;
    out.reserve(compute.exprs.size());
    for (const auto& e : compute.exprs) {
      out.push_back(e->name);
    }
    return out;
  };
  EXPECT_EQ(names(std::get<ComputeNode>(Nth(*plan, 3))),
            (std::vector<std::string>{"((i32 - 1) * 2)", "(sum(i16) * 2)", "-((i32 - 1))"}));
  const auto& having = std::get<FilterNode>(Nth(*plan, 4));
  ASSERT_EQ(having.predicates.size(), 1U);
  EXPECT_EQ(having.predicates[0].kind, Predicate::Kind::kCompareColumns);
  EXPECT_EQ(names(std::get<ComputeNode>(Nth(*plan, 5))),
            (std::vector<std::string>{"(sum(i16) + 1)"}));
  const auto& group = std::get<GroupAggregateNode>(Nth(*plan, 6));
  ASSERT_EQ(group.keys.size(), 1U);
  EXPECT_EQ(group.keys[0].name, "(i32 - 1)");
  const auto& keys = std::get<ComputeNode>(Nth(*plan, 7));
  ASSERT_EQ(keys.exprs.size(), 1U);
  EXPECT_EQ(keys.exprs[0]->name, "(i32 - 1)");
  const auto& where = std::get<FilterNode>(Nth(*plan, 8));
  EXPECT_EQ(where.predicates[0].column.value_or(BoundColumn{}).name, "(i16 // 2)");
  const auto& operand = std::get<ComputeNode>(Nth(*plan, 9));
  ASSERT_EQ(operand.exprs.size(), 1U);
  EXPECT_EQ(operand.exprs[0]->name, "(i16 // 2)");
  EXPECT_TRUE(std::holds_alternative<ScanNode>(Nth(*plan, 10)));

  // A select item over a non-key column is not grouped.
  auto ungrouped = BindSql("SELECT i16 + 1, COUNT(*) FROM t GROUP BY i32", catalog);
  ASSERT_FALSE(ungrouped.ok());
  EXPECT_EQ(SpanText("SELECT i16 + 1, COUNT(*) FROM t GROUP BY i32", ungrouped.status()), "i16");
}

// WHERE conditions over table columns only (two columns compared too) filter before any
// computation; conditions over expressions filter after it.
TEST(BinderTest, WhereSplitsAroundTheComputation) {
  const Catalog catalog = MakeCatalog();
  auto plan = BindSql("SELECT i16 FROM t WHERE i16 < i32 AND 5 < i16 * 2 AND i16 = 1", catalog);
  ASSERT_TRUE(plan.ok()) << plan.status().ToString();
  const auto& computed = std::get<FilterNode>(Nth(*plan, 1));
  ASSERT_EQ(computed.predicates.size(), 1U);
  EXPECT_EQ(computed.predicates[0].op, CompareOp::kGt) << "5 < e is e > 5";
  EXPECT_EQ(computed.predicates[0].constant.type, LogicalType::kSmallInt);
  EXPECT_TRUE(std::holds_alternative<ComputeNode>(Nth(*plan, 2)));
  const auto& scan = std::get<FilterNode>(Nth(*plan, 3));
  ASSERT_EQ(scan.predicates.size(), 2U);
  EXPECT_EQ(scan.predicates[0].kind, Predicate::Kind::kCompareColumns);
  EXPECT_EQ(scan.predicates[0].other.value_or(BoundColumn{}).name, "i32");
}

// The shape of an inner join (ADR 0022): the probe is the relation with the most rows it is
// estimated at (an unknown count is the most), the join builds on the relation it adds unless that
// relation is decisively larger (ADR 0026, covered below and in join_order_test), its key pair is
// oriented left input then right, and its span is the connector that added that relation. `t` has
// 100 rows, `ok` 7 and `u` an unknown count; none of them has footer statistics.
TEST(BinderTest, JoinOrderBuildSideAndKeys) {
  const Catalog catalog = MakeCatalog();
  struct Case {
    std::string_view sql;
    std::string_view built;  // the table the join builds its table on
    std::string_view span;   // the query text the Join is bound from
  };
  for (const Case& c : {
           // 100 rows against 7: the smaller is built and the larger probes.
           Case{.sql = "SELECT ok.s FROM t JOIN ok ON t.i16 = ok.i16",
                .built = "ok",
                .span = "JOIN"},
           // The same table is built whichever side is written first: which one that is comes from
           // the statistics, not from FROM. Which branch is the *left* does follow FROM, because
           // the two inputs of a join are interchangeable and the search orients them by relation
           // index; the span then names the right input, so it moves with the labels.
           Case{.sql = "SELECT ok.s FROM ok JOIN t ON t.i16 = ok.i16",
                .built = "ok",
                .span = "JOIN"},
           // A row count nothing knows must never be read as small, so u is probed and t built.
           Case{.sql = "SELECT t.s FROM t JOIN u ON t.i16 = u.i16", .built = "t", .span = "t"},
           // A comma join is the same join; its span is the comma.
           Case{.sql = "SELECT ok.s FROM t, ok WHERE t.i16 = ok.i16", .built = "ok", .span = ","},
           Case{.sql = "SELECT ok.s FROM t CROSS JOIN ok WHERE t.i16 = ok.i16",
                .built = "ok",
                .span = "CROSS JOIN"},
       }) {
    auto plan = BindSql(c.sql, catalog);
    ASSERT_TRUE(plan.ok()) << c.sql << ": " << plan.status().ToString();
    const JoinNode* join = testing::FirstJoin(*plan);
    ASSERT_NE(join, nullptr) << c.sql;
    EXPECT_EQ(join->kind, JoinKind::kInner) << c.sql;
    const LogicalNode& build = join->build == BuildSide::kLeft ? *join->left : *join->right;
    const LogicalNode& probe = join->build == BuildSide::kLeft ? *join->right : *join->left;
    EXPECT_EQ(testing::ScannedTable(build), c.built) << c.sql;
    EXPECT_NE(testing::ScannedTable(probe), c.built) << c.sql << ": the other side probes";
    ASSERT_EQ(join->keys.size(), 1U) << c.sql;
    EXPECT_TRUE(join->residual.empty()) << c.sql;
    EXPECT_EQ(c.sql.substr(join->span.offset, join->span.length), c.span) << c.sql;
  }
}

// The estimates account for the conjuncts routed to each relation (ADR 0026), so a filter can
// decide which relation is built and which probes. `t` has 100 rows and `ok` 7, so `ok` is built on
// its own; with two equalities on `t`, neither of which the footers can measure, `t` is estimated
// at 100 through two default factors of a tenth each, which is one row, and `t` is built instead.
TEST(BinderTest, FiltersDecideWhichRelationProbes) {
  const Catalog catalog = MakeCatalog();
  const auto built = [&catalog](std::string_view sql) {
    auto plan = BindSql(sql, catalog);
    EXPECT_TRUE(plan.ok()) << sql << ": " << plan.status().ToString();
    const JoinNode* join = testing::FirstJoin(*plan);
    EXPECT_NE(join, nullptr) << sql;
    if (join == nullptr) {
      return std::string{};
    }
    return testing::ScannedTable(join->build == BuildSide::kLeft ? *join->left : *join->right);
  };
  EXPECT_EQ(built("SELECT ok.s FROM t JOIN ok ON t.i16 = ok.i16"), "ok");
  EXPECT_EQ(built("SELECT ok.s FROM t JOIN ok ON t.i16 = ok.i16 "
                  "WHERE t.i32 = 5 AND t.i64 = 7"),
            "t");
  // A conjunct on the other relation moves it the other way again: `ok` is estimated at 7 through
  // one default factor, which is no row at all, so it is the side to build whatever `t` carries.
  EXPECT_EQ(built("SELECT ok.s FROM t JOIN ok ON t.i16 = ok.i16 WHERE ok.i16 = 3"), "ok");
}

// ---- a pair of facts closed by a cycle (ADR 0037's PF1) ----

// A pair of facts at a scale where joining them on the product alone fails a query: a sales fact, a
// stock snapshot 8 times its size with 800 rows a product, and their dimensions, around a calendar
// of 100 years whose roles close a cycle between the facts (whose days span 5 of them). Every key
// column states its range, so its domain is known; the VARCHAR filters keep the default factor.
Catalog FactPairCatalog() {
  struct Column {
    std::string name;
    std::shared_ptr<arrow::DataType> type;
    std::optional<std::pair<int64_t, int64_t>> range;  // in every part
  };
  Catalog catalog;
  const auto add = [&catalog](const std::string& name, int64_t rows,
                              const std::vector<Column>& columns) {
    arrow::FieldVector fields;
    fields.reserve(columns.size());
    for (const Column& column : columns) {
      fields.push_back(arrow::field(column.name, column.type));
    }
    auto table = std::make_shared<FakeTable>(arrow::schema(fields), rows);
    for (std::size_t f = 0; f < columns.size(); ++f) {
      if (const auto& range = columns[f].range; range.has_value()) {
        table->WithStats(
            0, Narrow<int>(f),
            PartStats{.min = range->first, .max = range->second, .null_count = 0, .rows = rows});
      }
    }
    EXPECT_TRUE(catalog.Register(name, table).ok()) << name;
  };
  const std::shared_ptr<arrow::DataType>& i32 = arrow::int32();
  constexpr int64_t kFirst = 18'251;  // the facts' first day, in year 51 of the calendar
  add("sales", 1'000'000,
      {{.name = "sa_product", .type = i32, .range = std::pair<int64_t, int64_t>{1, 10'000}},
       {.name = "sa_buyer", .type = i32, .range = std::pair<int64_t, int64_t>{1, 1'000'000}},
       {.name = "sa_segment", .type = i32, .range = std::pair<int64_t, int64_t>{1, 5'000}},
       {.name = "sa_sold",
        .type = i32,
        .range = std::pair<int64_t, int64_t>{kFirst, kFirst + 1'824}},
       {.name = "sa_ship",
        .type = i32,
        .range = std::pair<int64_t, int64_t>{kFirst + 1, kFirst + 1'944}},
       {.name = "sa_qty", .type = i32, .range = std::nullopt}});
  add("stock", 8'000'000,
      {{.name = "st_product", .type = i32, .range = std::pair<int64_t, int64_t>{1, 10'000}},
       {.name = "st_place", .type = i32, .range = std::pair<int64_t, int64_t>{1, 10}},
       {.name = "st_day",
        .type = i32,
        .range = std::pair<int64_t, int64_t>{kFirst, kFirst + 1'820}},
       {.name = "st_qty", .type = i32, .range = std::nullopt}});
  add("product", 10'000,
      {{.name = "pr_id", .type = i32, .range = std::pair<int64_t, int64_t>{1, 10'000}}});
  add("cal", 36'500,
      {{.name = "c_day", .type = i32, .range = std::pair<int64_t, int64_t>{1, 36'500}},
       {.name = "c_week", .type = i32, .range = std::pair<int64_t, int64_t>{1, 5'215}},
       {.name = "c_year", .type = i32, .range = std::pair<int64_t, int64_t>{1'950, 2'049}}});
  add("buyer", 1'000'000,
      {{.name = "b_id", .type = i32, .range = std::pair<int64_t, int64_t>{1, 1'000'000}},
       {.name = "b_kind", .type = arrow::utf8(), .range = std::nullopt}});
  add("segment", 5'000,
      {{.name = "sg_id", .type = i32, .range = std::pair<int64_t, int64_t>{1, 5'000}},
       {.name = "sg_kind", .type = arrow::utf8(), .range = std::nullopt}});
  add("place", 10, {{.name = "pl_id", .type = i32, .range = std::pair<int64_t, int64_t>{1, 10}}});
  return catalog;
}

// Nine relations, the facts joined on the product and closed by the cycle sales -> sold -> (week)
// -> stocked -> stock; the shape of tools/fixturegen's bookings and fleet (the CLI goldens
// explain_join_fact_pair*), at the scale the danger is real.
constexpr std::string_view kFactPair =
    "SELECT COUNT(*) FROM sales JOIN buyer ON sa_buyer = b_id JOIN segment ON sa_segment = sg_id "
    "JOIN cal AS sold ON sa_sold = sold.c_day JOIN cal AS stocked ON sold.c_week = stocked.c_week "
    "JOIN stock ON st_product = sa_product AND st_day = stocked.c_day JOIN place ON st_place = "
    "pl_id "
    "JOIN product ON pr_id = sa_product JOIN cal AS shipped ON sa_ship = shipped.c_day "
    "WHERE st_qty < sa_qty AND shipped.c_day > sold.c_day AND b_kind = 'x' AND sg_kind = 'y' "
    "AND sold.c_year = 2001";

// The tables a subtree scans, and its joins.
std::set<std::string> ScannedTables(const LogicalNode& node) {
  std::set<std::string> out;
  std::vector<const LogicalNode*> pending = {&node};
  while (!pending.empty()) {
    const LogicalNode* at = pending.back();
    pending.pop_back();
    if (const auto* scan = std::get_if<ScanNode>(at)) {
      out.insert(scan->table_name);
    }
    for (const LogicalNodePtr& input : InputsOf(*at)) {
      pending.push_back(input.get());
    }
  }
  return out;
}

std::vector<const JoinNode*> JoinsOf(const LogicalNode& node) {
  std::vector<const JoinNode*> out;
  std::vector<const LogicalNode*> pending = {&node};
  while (!pending.empty()) {
    const LogicalNode* at = pending.back();
    pending.pop_back();
    if (const auto* join = std::get_if<JoinNode>(at)) {
      out.push_back(join);
    }
    for (const LogicalNodePtr& input : InputsOf(*at)) {
      pending.push_back(input.get());
    }
  }
  return out;
}

// The join where `a`'s and `b`'s relations first meet: one input scans `a` and not `b`, the other
// `b` and not `a`.
const JoinNode* JoinBetween(const LogicalNode& root, const std::string& a, const std::string& b) {
  for (const JoinNode* join : JoinsOf(root)) {
    const std::set<std::string> left = ScannedTables(*join->left);
    const std::set<std::string> right = ScannedTables(*join->right);
    if ((left.contains(a) && !left.contains(b) && right.contains(b) && !right.contains(a)) ||
        (left.contains(b) && !left.contains(a) && right.contains(a) && !right.contains(b))) {
      return join;
    }
  }
  return nullptr;
}

using KeySet = std::set<std::pair<std::string, std::string>>;

// Each key of `join` as the sorted pair of its sides' qualifier.name.
KeySet KeyNames(const JoinNode& join) {
  KeySet out;
  for (const JoinKey& key : join.keys) {
    std::string left = key.left.qualifier + "." + key.left.name;
    std::string right = key.right.qualifier + "." + key.right.name;
    if (right < left) {
      std::swap(left, right);
    }
    out.emplace(std::move(left), std::move(right));
  }
  return out;
}

// The columns the Filters of a subtree compare.
std::set<std::string> FilteredColumns(const LogicalNode& node) {
  std::set<std::string> out;
  std::vector<const LogicalNode*> pending = {&node};
  while (!pending.empty()) {
    const LogicalNode* at = pending.back();
    pending.pop_back();
    if (const auto* filter = std::get_if<FilterNode>(at)) {
      for (const Predicate& predicate : filter->predicates) {
        if (predicate.column.has_value()) {
          out.insert(predicate.column->name);
        }
      }
    }
    for (const LogicalNodePtr& input : InputsOf(*at)) {
      pending.push_back(input.get());
    }
  }
  return out;
}

const LogicalNode& BuildOf(const JoinNode& join) {
  return join.build == BuildSide::kLeft ? *join.left : *join.right;
}

const LogicalNode& ProbeOf(const JoinNode& join) {
  return join.build == BuildSide::kLeft ? *join.right : *join.left;
}

// The search reduces the sales side through its filtered dimensions and the year of `sold`
// first, joins `stocked` through the week, builds that side, and streams the snapshot as the
// probe of the join of the two facts, on two keys: the product and the snapshot's day against
// `stocked`'s. Joining the facts
// on the product alone would be 1M x 800 rows.
TEST(BinderTest, AFactPairClosedByACycleMeetsOnTwoKeysAfterTheSalesSideShrinks) {
  const Catalog catalog = FactPairCatalog();
  auto plan = BindSql(kFactPair, catalog);
  ASSERT_TRUE(plan.ok()) << plan.status().ToString();
  const JoinNode* meet = JoinBetween(*plan->root, "sales", "stock");
  ASSERT_NE(meet, nullptr) << Explain(*plan);
  const KeySet product = {{"sales.sa_product", "stock.st_product"}};
  EXPECT_EQ(KeyNames(*meet),
            (KeySet{{"sales.sa_product", "stock.st_product"}, {"stock.st_day", "stocked.c_day"}}))
      << Explain(*plan);
  EXPECT_EQ(meet->residual.size(), 1U) << "st_qty < sa_qty";
  EXPECT_EQ(ScannedTables(ProbeOf(*meet)), std::set<std::string>{"stock"})
      << "the snapshot streams as the probe\n"
      << Explain(*plan);
  const std::set<std::string> filtered = FilteredColumns(BuildOf(*meet));
  for (const std::string column : {"c_year", "b_kind", "sg_kind"}) {
    EXPECT_TRUE(filtered.contains(column)) << column << " reduces the sales side first";
  }
  const std::vector<const JoinNode*> built = JoinsOf(BuildOf(*meet));
  EXPECT_TRUE(std::ranges::any_of(built,
                                  [](const JoinNode* join) {
                                    return KeyNames(*join) ==
                                           KeySet{{"sold.c_week", "stocked.c_week"}};
                                  }))
      << "the week equality is a key of its own join\n"
      << Explain(*plan);
  for (const JoinNode* join : JoinsOf(*plan->root)) {
    EXPECT_NE(KeyNames(*join), product) << "the facts never join on the product alone";
  }
}

// The control: the greedy rule (above 12 relations, or --join-order greedy) probes from the
// snapshot and builds the whole sales fact, unreduced, keyed on the product and `sold`'s day.
TEST(BinderTest, TheGreedyRuleMeetsTheFactPairWithTheSalesFactWhole) {
  const Catalog catalog = FactPairCatalog();
  auto stmt = sql::Parse(kFactPair);
  ASSERT_TRUE(stmt.has_value()) << stmt.error().message;
  auto plan = Bind(*stmt, catalog, BindOptions{.join_order = JoinOrderSearch::kGreedy});
  ASSERT_TRUE(plan.ok()) << plan.status().ToString();
  const JoinNode* meet = JoinBetween(*plan->root, "sales", "stock");
  ASSERT_NE(meet, nullptr) << Explain(*plan);
  const bool sales_left = ScannedTables(*meet->left).contains("sales");
  const LogicalNode& sales = sales_left ? *meet->left : *meet->right;
  const LogicalNode& stock = sales_left ? *meet->right : *meet->left;
  EXPECT_EQ(ScannedTables(sales), std::set<std::string>{"sales"}) << Explain(*plan);
  EXPECT_EQ(&BuildOf(*meet), &sales) << "the sales fact is built\n" << Explain(*plan);
  EXPECT_TRUE(FilteredColumns(sales).empty()) << "the sales fact meets the snapshot whole";
  EXPECT_TRUE(FilteredColumns(stock).contains("c_year")) << Explain(*plan);
  EXPECT_EQ(KeyNames(*meet),
            (KeySet{{"sales.sa_product", "stock.st_product"}, {"sales.sa_sold", "sold.c_day"}}))
      << Explain(*plan);
}

// The join ADR 0023's Path 2 creates for a correlated scalar aggregate is an inner join, so either
// side may build it, and the aggregate it puts on the right is often far larger than the outer rows
// that probe it (ADR 0026). It therefore chooses by the same rule the join order uses, and
// `--build-side-ratio` reaches it. `t` has 100 rows and `ok` 7.
TEST(BinderTest, Path2ChoosesItsBuildSideFromTheEstimates) {
  const Catalog catalog = MakeCatalog();
  const auto side = [&catalog](std::string_view sql, int64_t ratio) {
    auto bound = BindSql(sql, catalog);
    EXPECT_TRUE(bound.ok()) << sql << ": " << bound.status().ToString();
    auto optimized = Optimize(*bound, BindOptions{.build_side_ratio = ratio});
    EXPECT_TRUE(optimized.ok()) << sql << ": " << optimized.status().ToString();
    const JoinNode* join = testing::FirstJoin(*optimized);
    EXPECT_NE(join, nullptr) << sql;
    return join == nullptr ? BuildSide::kRight : join->build;
  };
  // 7 outer rows against an aggregate over 100: more than four times larger, so it builds on the
  // outer side instead of on the aggregate.
  constexpr std::string_view kSmallOuter =
      "SELECT i16 FROM ok WHERE i16 = (SELECT MAX(t.i16) FROM t WHERE t.i16 = ok.i16)";
  // The other way round: 100 outer rows against an aggregate over 7 keeps the aggregate as the
  // build side, which is where every such join built before this rule.
  constexpr std::string_view kLargeOuter =
      "SELECT i16 FROM t WHERE i16 = (SELECT MAX(ok.i16) FROM ok WHERE ok.i16 = t.i16)";
  EXPECT_EQ(side(kSmallOuter, kBuildSideRatio), BuildSide::kLeft);
  EXPECT_EQ(side(kLargeOuter, kBuildSideRatio), BuildSide::kRight);
  // The ratio decides, and it reaches the unnesting pass: at 100 hundredths the smaller side always
  // builds, and a ratio no difference can exceed never moves a build.
  EXPECT_EQ(side(kSmallOuter, 100), BuildSide::kLeft);
  EXPECT_EQ(side(kSmallOuter, 1'000'000), BuildSide::kRight);
}

// Only an inner join may build on its left input: every other kind outputs or preserves the rows of
// one specific input, and src/exec/hash_join.cc has no right-semi, right-anti or right-outer
// implementation (ADR 0026's C2b and C2c). The binder must therefore never produce
// BuildSide::kLeft for a non-inner kind, however the estimates fall.
//
// Nothing but this test checks that at test time. ResolvePositions fails on the combination and
// hash_join.cc's Make rejects it, but the first is an internal check that aborts, so a later change
// to join-order rule 5 that reached a LEFT JOIN unit or a subquery join would crash rather than
// fail a test. C1 made BuildSide::kLeft reachable from real SQL, which is what makes that worth
// pinning.
// A LEFT JOIN unit that is one relation of an inner join is estimated like any relation (ADR 0022's
// join-order rule 3: an inner join of its inputs, never below its left input), so the unit
// `ok LEFT JOIN st` (35 rows: 7 x 5, no key domain known) is built and `t` (100) probes. Its blocks
// of one relation each used to have no estimate, so the unit read as the largest relation: it
// probed, and `t` was built.
TEST(BinderTest, ALeftJoinUnitIsEstimatedLikeAnyRelation) {
  const Catalog catalog = MakeCatalog();
  for (const std::string_view sql : {
           "SELECT t.s FROM t, ok LEFT JOIN st ON ok.i16 = st.i16 WHERE t.i16 = ok.i16",
       }) {
    auto plan = BindSql(sql, catalog);
    ASSERT_TRUE(plan.ok()) << sql << ": " << plan.status().ToString();
    const JoinNode* join = testing::FirstJoin(*plan);
    ASSERT_NE(join, nullptr) << sql;
    ASSERT_EQ(join->kind, JoinKind::kInner) << sql << "\n" << Explain(*plan);
    const LogicalNode& build = join->build == BuildSide::kLeft ? *join->left : *join->right;
    const LogicalNode& probe = join->build == BuildSide::kLeft ? *join->right : *join->left;
    EXPECT_EQ(testing::ScannedTable(probe), "t") << sql << "\n" << Explain(*plan);
    const auto* unit = std::get_if<JoinNode>(&build);
    ASSERT_NE(unit, nullptr) << sql << ": the unit is built\n" << Explain(*plan);
    EXPECT_EQ(unit->kind, JoinKind::kLeft) << sql;
    ASSERT_TRUE(unit->estimate.has_value()) << sql << ": the unit's estimate";
    EXPECT_GE(unit->estimate.value_or(0), 7) << sql << ": never below its left input";
  }
}

// A LEFT or FULL JOIN without an equality key between its inputs is a nested loop of its unit
// (ADR 0040's OJ4c): `keys=[]`, every ON conjunct over both inputs a residual -- an inequality, a
// DOUBLE equality, a key side that reads two relations of one input -- built on the right input.
// Rule 11 still filters a LEFT JOIN's right input by a conjunct over it alone; a FULL JOIN keeps
// every one-sided conjunct a residual. Inside a derived table and a CTE too.
TEST(BinderTest, KeylessOuterJoinsAreNestedLoops) {
  const Catalog catalog = MakeCatalog();
  const auto explain = [&catalog](std::string_view sql) {
    auto plan = BindSql(sql, catalog);
    EXPECT_TRUE(plan.ok()) << sql << ": " << plan.status().ToString();
    return plan.ok() ? Explain(*plan) : std::string();
  };
  for (const auto& [sql, join] :
       std::initializer_list<std::pair<std::string_view, std::string_view>>{
           {"SELECT t.i16 FROM t LEFT OUTER JOIN u ON t.i16 > u.i16",
            "Join LEFT build=right keys=[] residual=["},
           {"SELECT t.i16 FROM t LEFT JOIN u ON t.d = u.d",
            "Join LEFT build=right keys=[] residual=["},
           {"SELECT t.i16 FROM t FULL OUTER JOIN u ON t.d = u.d",
            "Join FULL build=right keys=[] residual=["},
           {"SELECT t.i16 FROM t CROSS JOIN u LEFT JOIN ok ON t.i16 + u.i16 = ok.i16 WHERE t.i32 = "
            "u.i32",
            "Join LEFT build=right keys=[] residual=["},
           {"SELECT COUNT(*) FROM (SELECT t.i16 FROM t LEFT JOIN u ON t.d = u.d) AS s",
            "Join LEFT build=right keys=[] residual=["},
           {"WITH c AS (SELECT t.i16 AS k FROM t LEFT JOIN u ON t.d = u.d) SELECT COUNT(*) FROM c",
            "Join LEFT build=right keys=[] residual=["},
       }) {
    const std::string text = explain(sql);
    EXPECT_TRUE(text.contains(join)) << sql << "\n" << text;
  }
  // Rule 11: the right-only conjunct filters the right input, below the join; the inequality is
  // the residual alone.
  const std::string left =
      explain("SELECT t.i16 FROM t LEFT JOIN ok ON t.i16 < ok.i16 AND ok.i16 > 1");
  const std::size_t loop = left.find("Join LEFT build=right keys=[] residual=[");
  ASSERT_NE(loop, std::string::npos) << left;
  const std::size_t filter = left.find("Filter");
  ASSERT_NE(filter, std::string::npos) << "the right-only conjunct is a filter\n" << left;
  EXPECT_GT(filter, loop) << "below the join\n" << left;
  EXPECT_FALSE(left.substr(loop, left.find('\n', loop) - loop).contains("> 1")) << "one residual\n"
                                                                                << left;
  // A FULL JOIN keeps it a residual, as with a key.
  const std::string full =
      explain("SELECT t.i16 FROM t FULL JOIN ok ON t.i16 < ok.i16 AND ok.i16 > 1");
  EXPECT_TRUE(full.contains("Join FULL build=right keys=[] residual=[")) << full;
  EXPECT_EQ(full.find("Filter"), std::string::npos) << full;
}

// A FULL JOIN (ADR 0037's OJ3) is an outer join unit of kind FULL, built on its right input. It
// pads both inputs, so where a LEFT JOIN lets a WHERE conjunct over its left input into that input,
// and an ON conjunct over its right input filter that input, a FULL JOIN keeps the first above
// itself and the second as a residual: either input's rows come out whether or not they pass.
TEST(BinderTest, AFullJoinKeepsOneSidedConditionsAtTheJoin) {
  const Catalog catalog = MakeCatalog();
  const auto explain = [&](std::string_view sql) {
    auto plan = BindSql(sql, catalog);
    EXPECT_TRUE(plan.ok()) << sql << ": " << plan.status().ToString();
    return plan.ok() ? Explain(*plan) : std::string();
  };
  const auto filter_above_join = [](const std::string& text, std::string_view join) {
    const std::size_t filter = text.find("Filter");
    return filter != std::string::npos && filter < text.find(join);
  };
  const std::string full =
      explain("SELECT t.i16, ok.i16 FROM t FULL OUTER JOIN ok ON t.i16 = ok.i16 WHERE t.i32 > 1");
  EXPECT_TRUE(full.contains("Join FULL build=right keys=[")) << full;
  EXPECT_TRUE(filter_above_join(full, "Join FULL")) << full;
  const std::string left =
      explain("SELECT t.i16, ok.i16 FROM t LEFT JOIN ok ON t.i16 = ok.i16 WHERE t.i32 > 1");
  EXPECT_FALSE(filter_above_join(left, "Join LEFT")) << left;
  const std::string right_where =
      explain("SELECT t.i16, ok.i16 FROM t FULL JOIN ok ON t.i16 = ok.i16 WHERE ok.i16 > 1");
  EXPECT_TRUE(filter_above_join(right_where, "Join FULL")) << right_where;
  const std::string on = explain(
      "SELECT t.i16, ok.i16 FROM t FULL JOIN ok ON t.i16 = ok.i16 AND ok.i16 > 1 AND t.i32 < 9");
  EXPECT_TRUE(on.contains("residual=[")) << on;
  EXPECT_FALSE(filter_above_join(on, "Join FULL")) << on;
  const std::string left_on =
      explain("SELECT t.i16, ok.i16 FROM t LEFT JOIN ok ON t.i16 = ok.i16 AND ok.i16 > 1");
  EXPECT_FALSE(left_on.contains("residual=[")) << "rule 11 filters a LEFT JOIN's right input\n"
                                               << left_on;
  // The ON of an inner join before the unit is a condition of a join inside its left input, though
  // the unit pads that input; one after it, over the unit's left input alone, filters above it.
  const std::string inside = explain(
      "SELECT t.i16 FROM t JOIN u ON t.i16 = u.i16 AND t.i32 > 1 FULL JOIN ok ON u.i16 = ok.i16");
  EXPECT_GT(inside.find("Join INNER"), inside.find("Join FULL")) << inside;
  EXPECT_FALSE(filter_above_join(inside, "Join FULL")) << inside;
  const std::string after = explain(
      "SELECT t.i16 FROM t FULL JOIN ok ON t.i16 = ok.i16 JOIN u ON u.i16 = ok.i16 AND t.i32 > 1");
  EXPECT_TRUE(filter_above_join(after, "Join FULL")) << after;
  // In a chain, a derived table and a CTE.
  for (const std::string_view sql : {
           "SELECT t.i16 FROM t JOIN u ON t.i16 = u.i16 FULL JOIN ok ON u.i16 = ok.i16",
           "SELECT t.i16 FROM t FULL JOIN ok ON t.i16 = ok.i16 LEFT JOIN u ON u.i16 = ok.i16",
           "SELECT x FROM (SELECT t.i16 AS x FROM t FULL JOIN ok ON t.i16 = ok.i16) AS d",
           "WITH c AS (SELECT ok.i16 AS x FROM t FULL JOIN ok ON t.i16 = ok.i16) SELECT x FROM c",
       }) {
    EXPECT_TRUE(explain(sql).contains("Join FULL build=right")) << sql;
  }
}

// A key over an outer join unit's padded side, for a later join or a WHERE equality, is computed
// above the unit, not on its relation's branch below the padding: a padded row's side is then
// what the expression makes of its NULLs (COALESCE(u.i16, 1) is 1), as in DuckDB.
TEST(BinderTest, AKeyOverAPaddedSideIsComputedAboveTheUnit) {
  const Catalog catalog = MakeCatalog();
  for (
      const std::string_view sql : {
          "SELECT t.i16 FROM t LEFT JOIN u ON t.i16 = u.i16 JOIN ok ON ok.i16 = COALESCE(u.i16, 1)",
          "SELECT t.i16 FROM t LEFT JOIN u ON t.i16 = u.i16, ok WHERE COALESCE(u.i16, 1) = ok.i16",
          "SELECT t.i16 FROM t FULL JOIN u ON t.i16 = u.i16 JOIN ok ON ok.i16 = COALESCE(t.i16, 1)",
      }) {
    auto plan = BindSql(sql, catalog);
    ASSERT_TRUE(plan.ok()) << sql << ": " << plan.status().ToString();
    EXPECT_EQ(PositionMismatch(*plan), std::nullopt) << sql << "\n" << Explain(*plan);
    // EXPLAIN writes a node before its inputs: the key's Compute comes before the unit's join.
    const std::string text = Explain(*plan);
    const std::size_t key = text.find("Compute COALESCE(");
    const std::size_t unit = text.find(sql.contains("FULL") ? "Join FULL" : "Join LEFT");
    ASSERT_NE(key, std::string::npos) << sql << "\n" << text;
    ASSERT_NE(unit, std::string::npos) << sql << "\n" << text;
    EXPECT_LT(key, unit) << sql << "\n" << text;
  }
}

TEST(BinderTest, OnlyAnInnerJoinEverBuildsOnItsLeftInput) {
  const Catalog catalog = MakeCatalog();
  // Every join of a plan, both inputs of each, however deeply nested.
  const auto joins = [](const LogicalNode& root) {
    std::vector<const JoinNode*> found;
    std::vector<const LogicalNode*> pending = {&root};
    while (!pending.empty()) {
      const LogicalNode* at = pending.back();
      pending.pop_back();
      if (const auto* join = std::get_if<JoinNode>(at)) {
        found.push_back(join);
      }
      for (const LogicalNodePtr& input : InputsOf(*at)) {
        pending.push_back(input.get());
      }
    }
    return found;
  };
  for (const std::string_view sql : {
           // A LEFT JOIN unit, which is one relation of its block and never reordered.
           "SELECT t.s FROM t LEFT JOIN ok ON t.i16 = ok.i16",
           "SELECT t.s FROM ok LEFT JOIN t ON t.i16 = ok.i16",
           // A unit whose preserved side is the smaller, which is the case rule 5 would want to
           // flip if it ever applied to a unit.
           "SELECT ok.s FROM ok LEFT JOIN t ON t.i16 = ok.i16 WHERE ok.i16 = 3",
           // Subquery joins (ADR 0022's J5): a semi join for IN, a null-aware anti for NOT IN, and
           // a one-row join for a scalar subquery.
           "SELECT i16 FROM t WHERE i16 IN (SELECT i16 FROM ok)",
           "SELECT i16 FROM t WHERE i16 NOT IN (SELECT i16 FROM ok)",
           "SELECT i16 FROM t WHERE i16 = (SELECT MAX(i16) FROM ok)",
           // The same where the outer side is far smaller than the subquery, so an estimate-driven
           // rule would have every reason to want the other side.
           "SELECT i16 FROM ok WHERE i16 IN (SELECT i16 FROM t)",
           "SELECT i16 FROM ok WHERE i16 NOT IN (SELECT i16 FROM t)",
           // The ADR 0023 unnesting rewrites, which Optimize produces: a correlated EXISTS and a
           // correlated scalar aggregate.
           "SELECT i16 FROM t WHERE EXISTS (SELECT i16 FROM ok WHERE ok.i16 = t.i16)",
           "SELECT i16 FROM t WHERE NOT EXISTS (SELECT i16 FROM ok WHERE ok.i16 = t.i16)",
           "SELECT i16 FROM t WHERE i16 = (SELECT MAX(ok.i16) FROM ok WHERE ok.i16 = t.i16)",
       }) {
    auto bound = BindSql(sql, catalog);
    ASSERT_TRUE(bound.ok()) << sql << ": " << bound.status().ToString();
    const LogicalPlan plan = Optimized(*bound);
    const std::vector<const JoinNode*> all = joins(*plan.root);
    EXPECT_FALSE(all.empty()) << sql << ": no join to check";
    for (const JoinNode* join : all) {
      if (join->kind != JoinKind::kInner) {
        EXPECT_EQ(join->build, BuildSide::kRight)
            << sql << ": a " << ToString(join->kind) << " join builds on its left input";
      }
    }
  }
}

// Every subquery join of ADR 0022's J5 carries an upper bound on its rows: a semi or anti join
// keeps a subset of its left input and a one-row join pairs each of its rows with one, so the left
// input's estimate bounds all three. Without it the estimate was unknown, and an unknown estimate
// travels: every pass above read the join's rows as unknown too, which left ADR 0023's semi-join
// reduction unable to see the one query it gains most on, and left an anti join building the larger
// side because nothing could show the smaller one was smaller.
TEST(BinderTest, EverySubqueryJoinBoundsItsRows) {
  const Catalog catalog = MakeCatalog();
  for (const char* sql : {
           "SELECT i16 FROM t WHERE i16 IN (SELECT i16 FROM ok)",
           "SELECT i16 FROM t WHERE i16 NOT IN (SELECT i16 FROM ok)",
           "SELECT i16 FROM t WHERE i16 = (SELECT MAX(i16) FROM ok)",
       }) {
    auto bound = BindSql(sql, catalog);
    ASSERT_TRUE(bound.ok()) << sql << ": " << bound.status().ToString();
    const JoinNode* join = testing::FirstJoin(*bound);
    ASSERT_NE(join, nullptr) << sql;
    EXPECT_EQ(join->estimate, std::optional<int64_t>(100))
        << sql << ": t holds 100 rows, which bounds what the join keeps";
  }
}

// A cross-relation equality whose sides differ in type gets one key type both widen to exactly,
// and the side that is not of that type is cast on its own branch (ADR 0022's key casts). An
// equality with no such type, and any other cross-relation conjunct, is a residual of the join.
TEST(BinderTest, JoinKeyCastsAndResiduals) {
  const Catalog catalog = MakeCatalog();
  // INTEGER against SMALLINT is INTEGER: ok.i16 is cast on ok's branch, t.i32 is already one.
  auto cast = BindSql("SELECT ok.s FROM t JOIN ok ON t.i32 = ok.i16", catalog);
  ASSERT_TRUE(cast.ok()) << cast.status().ToString();
  const JoinNode* join = testing::FirstJoin(*cast);
  ASSERT_NE(join, nullptr);
  ASSERT_EQ(join->keys.size(), 1U);
  EXPECT_EQ(join->keys[0].left.type, LogicalType::kInteger);
  EXPECT_EQ(join->keys[0].right.type, LogicalType::kInteger);
  const auto* computed = std::get_if<ComputeNode>(join->right.get());
  ASSERT_NE(computed, nullptr) << "ok's branch computes the cast";
  ASSERT_EQ(computed->exprs.size(), 1U);
  EXPECT_EQ(computed->exprs[0]->name, "CAST(ok.i16 AS INTEGER)");  // rule 6 keeps the qualifier
  EXPECT_EQ(computed->exprs[0]->type, LogicalType::kInteger);
  EXPECT_TRUE(std::holds_alternative<CastExpr>(computed->exprs[0]->node));
  EXPECT_EQ(testing::ScannedTable(*join->right), "ok");
  // One key and one residual: the ON's second conjunct reads both relations and is no equality.
  auto residual =
      BindSql("SELECT ok.s FROM t JOIN ok ON t.i16 = ok.i16 AND t.i32 > ok.i16", catalog);
  ASSERT_TRUE(residual.ok()) << residual.status().ToString();
  const JoinNode* with_residual = testing::FirstJoin(*residual);
  ASSERT_NE(with_residual, nullptr);
  EXPECT_EQ(with_residual->keys.size(), 1U);
  EXPECT_EQ(with_residual->residual.size(), 1U);
  // The same equality in an ON and in WHERE is one key, not two of the same join.
  auto twice =
      BindSql("SELECT ok.s FROM t JOIN ok ON t.i16 = ok.i16 WHERE ok.i16 = t.i16", catalog);
  ASSERT_TRUE(twice.ok()) << twice.status().ToString();
  const JoinNode* deduped = testing::FirstJoin(*twice);
  ASSERT_NE(deduped, nullptr);
  EXPECT_EQ(deduped->keys.size(), 1U);
  EXPECT_TRUE(deduped->residual.empty());
  // A DOUBLE equality has no key type, so it is a residual, and with no key the join is a nested
  // loop (ADR 0040) that the residual decides.
  auto doubles = BindSql("SELECT COUNT(*) FROM t JOIN u ON t.d = u.d", catalog);
  ASSERT_TRUE(doubles.ok()) << doubles.status().ToString();
  const JoinNode* loop = testing::FirstJoin(*doubles);
  ASSERT_NE(loop, nullptr);
  EXPECT_TRUE(loop->keys.empty());
  EXPECT_EQ(loop->residual.size(), 1U);
}

// Each relation keeps the conjuncts that read it alone, on its own branch below the join, and a
// column of a scope with two bindings carries its binding's qualifier.
TEST(BinderTest, PerRelationFiltersAndQualifiers) {
  const Catalog catalog = MakeCatalog();
  auto plan = BindSql(
      "SELECT t.s, ok.s FROM t JOIN ok ON t.i16 = ok.i16 WHERE t.i32 = 1 AND ok.s = 'x'", catalog);
  ASSERT_TRUE(plan.ok()) << plan.status().ToString();
  const JoinNode* join = testing::FirstJoin(*plan);
  ASSERT_NE(join, nullptr);
  for (const auto& [branch, table] :
       {std::pair{join->left.get(), "t"}, std::pair{join->right.get(), "ok"}}) {
    const auto* filter = std::get_if<FilterNode>(branch);
    ASSERT_NE(filter, nullptr) << table << "'s own conjunct filters its own branch";
    ASSERT_EQ(filter->predicates.size(), 1U) << table;
    EXPECT_EQ(testing::ScannedTable(*branch), table);
  }
  const auto& project = std::get<ProjectNode>(Nth(*plan, 0));
  ASSERT_EQ(project.columns.size(), 2U);
  EXPECT_EQ(project.columns[0].qualifier, "t");
  EXPECT_EQ(project.columns[1].qualifier, "ok");
}

// ADR 0022's OR factoring (J3): the conjuncts every branch of an OR shares are hoisted out of it
// before the conjuncts are classified, so a shared equality becomes a join key and a shared
// single-relation conjunct reaches its scan. What is left of the branches stays one conjunct, on
// one relation's branch when it reads one relation and a residual of the join when it reads two.
TEST(BinderTest, FactorsConjunctsSharedByEveryOrBranch) {
  const Catalog catalog = MakeCatalog();
  // The shared equality is the join key, and the remainder reads only `ok`, so it filters `ok`'s
  // branch. AND binds tighter than OR, so the whole WHERE is one element of two branches.
  const std::string sql =
      "SELECT COUNT(*) FROM t, ok WHERE t.i16 = ok.i16 AND ok.s = 'x' OR "
      "t.i16 = ok.i16 AND ok.s = 'y'";
  auto plan = BindSql(sql, catalog);
  ASSERT_TRUE(plan.ok()) << plan.status().ToString();
  const JoinNode* join = testing::FirstJoin(*plan);
  ASSERT_NE(join, nullptr);
  ASSERT_EQ(join->keys.size(), 1U) << "the shared equality is a key, not part of the OR";
  EXPECT_TRUE(join->residual.empty()) << "what is left reads one relation, so it is no residual";
  // `ok`'s branch: the remainder is compound, so a Compute holds it and a Filter keeps its rows.
  const auto* filter = std::get_if<FilterNode>(join->right.get());
  ASSERT_NE(filter, nullptr) << Explain(*plan);
  ASSERT_EQ(filter->predicates.size(), 1U);
  EXPECT_NE(Explain(*plan).find("Compute ((ok.s = 'x') OR (ok.s = 'y'))"), std::string::npos)
      << Explain(*plan);
  EXPECT_EQ(testing::ScannedTable(*join->right), "ok");
  // The span of a node a relation stacks covers the WHERE elements its conjuncts came from, so
  // this whole element -- not just the part of it the factoring left behind.
  const std::size_t where = sql.find("t.i16");
  EXPECT_EQ(filter->span, (SourceSpan{.offset = where, .length = sql.size() - where}));
  // A remainder that reads both relations is a residual of the join instead.
  auto both = BindSql(
      "SELECT COUNT(*) FROM t, ok WHERE t.i16 = ok.i16 AND t.i32 = 1 OR "
      "t.i16 = ok.i16 AND ok.s = 'x'",
      catalog);
  ASSERT_TRUE(both.ok()) << both.status().ToString();
  const JoinNode* with_residual = testing::FirstJoin(*both);
  ASSERT_NE(with_residual, nullptr);
  EXPECT_EQ(with_residual->keys.size(), 1U);
  EXPECT_EQ(with_residual->residual.size(), 1U);
  // A branch of only shared conjuncts makes the OR true wherever they hold, so the key is all
  // that is left: `ok`'s branch is its bare Scan, with nothing of the dropped OR above it. (The
  // Scan still lists every field here -- the binder prunes none; the column list the optimizer
  // then reads is pinned by the EXPLAIN golden in tests/cli.)
  auto absorbed = BindSql(
      "SELECT COUNT(*) FROM t, ok WHERE t.i16 = ok.i16 OR t.i16 = ok.i16 AND ok.s = 'x'", catalog);
  ASSERT_TRUE(absorbed.ok()) << absorbed.status().ToString();
  const JoinNode* no_filter = testing::FirstJoin(*absorbed);
  ASSERT_NE(no_filter, nullptr);
  EXPECT_EQ(no_filter->keys.size(), 1U);
  EXPECT_TRUE(no_filter->residual.empty());
  EXPECT_TRUE(std::holds_alternative<ScanNode>(*no_filter->right)) << Explain(*absorbed);
  EXPECT_EQ(Explain(*absorbed).find("Compute"), std::string::npos)
      << "the dropped OR is bound for its errors only, and reaches no node\n"
      << Explain(*absorbed);
  // Matching is structural, so the same equality written the other way round shares nothing: with
  // no key left the join is a nested loop (ADR 0040), the whole OR its residual.
  auto reversed = BindSql(
      "SELECT COUNT(*) FROM t, ok WHERE t.i16 = ok.i16 AND ok.s = 'x' OR "
      "ok.i16 = t.i16 AND ok.s = 'y'",
      catalog);
  ASSERT_TRUE(reversed.ok()) << reversed.status().ToString();
  const JoinNode* unkeyed = testing::FirstJoin(*reversed);
  ASSERT_NE(unkeyed, nullptr);
  EXPECT_TRUE(unkeyed->keys.empty());
  EXPECT_EQ(unkeyed->residual.size(), 1U);
  // One relation, no join: the shared conjunct reaches the Scan, where the OR could not -- and so
  // does the IN list the remainder implies (ADR 0026's "filters implied by ORs"), which is the
  // whole point of deriving it.
  auto pushed =
      BindSql("SELECT COUNT(*) FROM t WHERE i16 = 1 AND i32 = 2 OR i16 = 1 AND i32 = 3", catalog);
  ASSERT_TRUE(pushed.ok()) << pushed.status().ToString();
  EXPECT_NE(Explain(*pushed).find("Filter i16 = 1 AND i32 IN (2, 3)\n"), std::string::npos)
      << Explain(*pushed);
  EXPECT_NE(Explain(*pushed).find("Compute ((i32 = 2) OR (i32 = 3))"), std::string::npos)
      << Explain(*pushed);
  // HAVING is not factored: its OR stays one computed conjunct, aggregate and all. Factoring it
  // would classify against the aggregation's output, not the relations, so it is left out.
  auto having = BindSql(
      "SELECT i16, COUNT(*) FROM t GROUP BY i16 "
      "HAVING COUNT(*) > 1 AND i16 = 1 OR COUNT(*) > 1 AND i16 = 2",
      catalog);
  ASSERT_TRUE(having.ok()) << having.status().ToString();
  EXPECT_NE(Explain(*having).find("Compute (((count_star() > 1) AND (i16 = 1)) OR "
                                  "((count_star() > 1) AND (i16 = 2)))"),
            std::string::npos)
      << "the whole HAVING is one condition, with count_star() > 1 not hoisted\n"
      << Explain(*having);
}

// A relation's own layers, in the order RelationNode stacks them over its Scan: the conjuncts on
// the Scan's columns, the operands the others need, then those others. Each lower node's span
// covers the WHERE elements its conjuncts came from, so the whole of this WHERE, and a Project
// above a projection reads the binding's column with no qualifier while there is one binding.
TEST(BinderTest, RelationLayersAndTheirSpans) {
  const Catalog catalog = MakeCatalog();
  const std::string sql =
      "SELECT i16 FROM t WHERE i16 = 1 AND (i16 = 2 OR i32 = 3) AND 5 < i16 * 2";
  auto plan = BindSql(sql, catalog);
  ASSERT_TRUE(plan.ok()) << plan.status().ToString();
  // Project, Filter (computed), Compute (the operands), Filter (on the Scan), Scan.
  ASSERT_TRUE(std::holds_alternative<ProjectNode>(Nth(*plan, 0)));
  const auto& computed = std::get<FilterNode>(Nth(*plan, 1));
  const auto& operands = std::get<ComputeNode>(Nth(*plan, 2));
  const auto& on_scan = std::get<FilterNode>(Nth(*plan, 3));
  EXPECT_TRUE(std::holds_alternative<ScanNode>(Nth(*plan, 4)));
  EXPECT_EQ(computed.predicates.size(), 2U);  // 5 < i16 * 2, and the OR
  EXPECT_EQ(operands.exprs.size(), 2U);       // i16 * 2, and the OR's own column
  EXPECT_EQ(on_scan.predicates.size(), 1U);   // i16 = 1
  // One span for all three, covering the first WHERE element through the last.
  const std::size_t where = sql.find("i16 = 1");
  const SourceSpan whole{.offset = where, .length = sql.size() - where};
  EXPECT_EQ(on_scan.span, whole);
  EXPECT_EQ(operands.span, whole);
  EXPECT_EQ(computed.span, whole);
  // One binding, so no column carries a qualifier.
  const auto& project = std::get<ProjectNode>(Nth(*plan, 0));
  ASSERT_EQ(project.columns.size(), 1U);
  EXPECT_EQ(project.columns[0].qualifier, "");
}

// DuckDB's constant moving in WHERE: x + c <op> k is x <op> k - c for an integer x (and likewise
// x - c, c + x, c - x, x * c when c divides k), so the arithmetic is never computed; it stops where
// k or the new constant leaves the type, where an = is FALSE instead.
TEST(BinderTest, WhereMovesConstantsLikeDuckDb) {
  const Catalog catalog = MakeCatalog();
  struct Case {
    std::string_view where;
    std::string_view filter;  // the EXPLAIN line of the only Filter
  };
  for (const Case& c : {
           Case{.where = "i16 + 1 > 0", .filter = "Filter i16 > -1"},
           Case{.where = "1 + i16 > 0", .filter = "Filter i16 > -1"},
           Case{.where = "i16 - 1 < 0", .filter = "Filter i16 < 1"},
           Case{.where = "1 - i16 < 0", .filter = "Filter i16 > 1"},
           Case{.where = "0 < i16 + (1 + 1)", .filter = "Filter i16 > -2"},
           Case{.where = "(i16 + 1) * 2 > 0", .filter = "Filter i16 > -1"},
           Case{.where = "i16 * -2 > 0", .filter = "Filter i16 < 0"},
           Case{.where = "i16 * 2 = 3", .filter = "Filter FALSE"},
           Case{.where = "i16 * 2 <> 3", .filter = "Filter i16 IS NOT NULL"},
           Case{.where = "i16 + 1 >= 32767", .filter = "Filter i16 >= 32766"},
           Case{.where = "i64 + 1 > 9223372036854775807",
                .filter = "Filter i64 > 9223372036854775806"},
           Case{.where = "i16 + 1 > -32768", .filter = "Filter \"(i16 + 1)\" > -32768"},
           Case{.where = "i16 + 1 > 40000", .filter = "Filter FALSE"},
           Case{.where = "i16 * 2 > 1", .filter = "Filter \"(i16 * 2)\" > 1"},
           Case{.where = "i16 * 0 = 0", .filter = "Filter \"(i16 * 0)\" = 0"},
           Case{.where = "i16 + 1 > 0.5", .filter = "Filter \"(i16 + 1)\" >= 1"},
           Case{.where = "u16 + 1 > 0", .filter = "Filter \"(u16 + 1)\" > 0"},
           Case{.where = "d + 1 > 2", .filter = "Filter \"(d + 1)\" > 2"},
           Case{.where = "i16 // 2 > 0", .filter = "Filter \"(i16 // 2)\" > 0"},
           Case{.where = "i16 + -(1) > 0", .filter = "Filter i16 > 1"},
           Case{.where = "i16 + 4 / 2 > 0", .filter = "Filter \"(i16 + (4 / 2))\" > 0"},
           Case{.where = "i16 + i32 > 0", .filter = "Filter \"(i16 + i32)\" > 0"},
           Case{.where = "i16 + 1 > 1e0", .filter = "Filter \"(i16 + 1)\" > 1"},
           Case{.where = "i16 * 2 <= 4", .filter = "Filter i16 <= 2"},
           // A constant folded in DuckDB's types moves; one that overflows its type is none.
           Case{.where = "i32 + (2147483646 + 1) > 5", .filter = "Filter i32 > -2147483642"},
           Case{.where = "i32 + (2147483647 + 1) > 5",
                .filter = "Filter \"(i32 + (2147483647 + 1))\" > 5"},
           Case{.where = "i32 + 65536 * 32768 > 5",
                .filter = "Filter \"(i32 + (65536 * 32768))\" > 5"},
           // The operand as DuckDB's expression rewriter leaves it (RewriteArithmetic): a chain's
           // constants first and folded, then moved; the regrouped constant that overflows is no
           // constant; x * 1 and x + 0 are x; with two columns the chain moves nothing.
           Case{.where = "i32 + 1 + 2 > 5", .filter = "Filter i32 > 2"},
           Case{.where = "1 + i32 + 2 = 7", .filter = "Filter i32 = 4"},
           Case{.where = "1 + (2 + i32) = 10", .filter = "Filter i32 = 7"},
           Case{.where = "i32 * 2 * 3 = 12", .filter = "Filter i32 = 2"},
           Case{.where = "i32 * 2 * 3 = 13", .filter = "Filter FALSE"},
           Case{.where = "i32 * 2 * 3 > 13", .filter = "Filter \"((i32 * 2) * 3)\" > 13"},
           Case{.where = "i32 + 1 + -1 >= 2147483647", .filter = "Filter i32 >= 2147483647"},
           Case{.where = "i32 * 1 > 5", .filter = "Filter i32 > 5"},
           Case{.where = "i32 + 2147483647 + 1 > 5",
                .filter = "Filter \"((i32 + 2147483647) + 1)\" > 5"},
           Case{.where = "i32 + 1 + i32 + 2 > 5",
                .filter = "Filter \"(((i32 + 1) + i32) + 2)\" > 5"},
           // Against a constant expression of a wider type, DuckDB moves the constants of the
           // operation's own type off the chain as written first; a SMALLINT chain's INTEGER
           // literals are none.
           Case{.where = "i32 + 2147483647 + 1 > 2147483648 - 1", .filter = "Filter i32 > -1"},
           Case{.where = "i16 + 32767 + 1 > 2147483648 - 2147483643",
                .filter = "Filter \"((i16 + 32767) + 1)\" > 5"},
           Case{.where = "i32 + 2147483647 + 1 BETWEEN 4 AND 2147483648 - 1",
                .filter = "Filter i32 >= -2147483644 AND i32 <= -1"},
           // In DuckDB's order: one pass of the rewrite, then the moves it allows, then another
           // pass: the * 1 the first pass removes lets 2147483647 move before a pass regroups it.
           Case{.where = "(i32 + 2147483647) * 1 + 1 > 5", .filter = "Filter i32 > -2147483643"},
           // An = whose moved constant leaves the type is FALSE.
           Case{.where = "i32 + 1 + -1 - 0 = 2147483648 - 1", .filter = "Filter FALSE"},
           // A widening cast is moved through where k fits its operand's type, and kept where
           // it does not, so that the operand is computed.
           Case{.where = "CAST(i16 AS INTEGER) + 1 > 5", .filter = "Filter i16 > 4"},
           Case{.where = "i16 + 30000 + 70000 < 5",
                .filter = "Filter \"CAST((i16 + 30000) AS INTEGER)\" < -69995"},
           // A cast at the top of the operand is dropped before its first pass, so its constants
           // move as written; a USMALLINT operand is compared behind its cast, which DuckDB keeps.
           Case{.where = "CAST(i32 + 2147483647 + 1 AS BIGINT) > 2147483647",
                .filter = "Filter i32 > -1"},
           Case{.where = "u16 + 0 + 1 = 1 + 1",
                .filter = "Filter \"CAST(((u16 + 0) + 1) AS INTEGER)\" = 2"},
           // Every integer type, USMALLINT too, and constants of // and %.
           Case{.where = "u16 + 1 > 5", .filter = "Filter u16 > 4"},
           Case{.where = "i32 + (4 // 2) + 1 > 5", .filter = "Filter i32 > 2"},
           // A cast to a value's own type is none, as DuckDB's binder writes none: the chain
           // regroups in the first pass and overflows.
           Case{.where = "CAST(i32 + 2147483647 + 1 AS INTEGER) > 5",
                .filter = "Filter \"CAST(((i32 + 2147483647) + 1) AS INTEGER)\" > 5"},
           // DuckDB drops a narrowing cast and one to a DECIMAL of scale 0 too, but none to a
           // DECIMAL of another scale and none between USMALLINT and a signed type.
           Case{.where = "CAST(i64 + 1 AS INTEGER) > 5", .filter = "Filter i64 > 4"},
           Case{.where = "CAST(i32 + 1 AS DECIMAL(12,0)) > 5", .filter = "Filter i32 > 4"},
           Case{.where = "CAST(i32 + 1 AS DECIMAL(12,2)) > 5",
                .filter = "Filter \"CAST((i32 + 1) AS DECIMAL(12, 2))\" > 5.00"},
           Case{.where = "CAST(u16 + 1 AS INTEGER) > 5",
                .filter = "Filter \"CAST((u16 + 1) AS INTEGER)\" > 5"},
           // x * 0 moves nothing, as written or once a pass has regrouped it.
           Case{.where = "i32 * 0 > 2147483648 - 1", .filter = "Filter \"(i32 * 0)\" > 2147483647"},
           Case{.where = "i32 * 2 * 0 > 5", .filter = "Filter \"((i32 * 2) * 0)\" > 5"},
           // A BETWEEN compares in the type of all three values: a literal or a column that does
           // not fit the operand's widens it for both comparisons; a decimal literal such as 3.5
           // moves nothing.
           Case{.where = "i32 + 2147483647 + 1 BETWEEN -3000000000 AND 5",
                .filter = "Filter i32 <= -2147483643"},
           Case{.where = "i32 + 2147483647 + 1 BETWEEN i64 AND 5",
                .filter = "Filter i32 <= -2147483643"},
           // A k the cast's own type does not hold is compared behind it, so it is computed (or,
           // as a whole conjunct, folds as D14 says).
           Case{.where = "CAST(i32 AS SMALLINT) > 40000", .filter = "Filter FALSE"},
           Case{.where = "CAST(i32 + 1 AS SMALLINT) + 0 < 40000",
                .filter = "Filter \"(CAST((i32 + 1) AS SMALLINT) + 0)\" IS NOT NULL"},
           Case{.where = "CAST(h AS SMALLINT) <> 40000",
                .filter = "Filter \"CAST(h AS SMALLINT)\" IS NOT NULL"},
           // A DECIMAL of scale 0 casts as an integer type does, and under a cast to a value's own
           // type the cast below is still the one at the top.
           Case{.where = "CAST(h AS BIGINT) > 5", .filter = "Filter h > 5"},
           Case{.where = "CAST(CAST(i32 + 2147483647 + 1 AS BIGINT) AS BIGINT) > 5",
                .filter = "Filter i32 > -2147483643"},
           // A decimal k of scale 0 is an integer compared in a DECIMAL, as written first; one of
           // another scale moves nothing.
           Case{.where = "i32 + 2147483647 + 1 <= 5.", .filter = "Filter i32 <= -2147483643"},
           Case{.where = "i32 + 2147483647 + 1 <= 5.0",
                .filter = "Filter \"((i32 + 2147483647) + 1)\" <= 5"},
           // A literal operand of a BETWEEN takes the type of a bound it fits.
           Case{.where = "5 BETWEEN u16 + 1 AND u16 + 10", .filter = "Filter u16 <= 4"},
           Case{.where = "i32 - 1 BETWEEN 0 AND 3.5",
                .filter = "Filter \"(i32 - 1)\" >= 0 AND \"(i32 - 1)\" <= 3"},
       }) {
    const std::string sql = "SELECT COUNT(*) FROM t WHERE " + std::string(c.where);
    auto plan = BindSql(sql, catalog);
    ASSERT_TRUE(plan.ok()) << sql << ": " << plan.status().ToString();
    const std::string explain = Explain(*plan);
    EXPECT_NE(explain.find(std::string(c.filter) + "\n"), std::string::npos) << sql << "\n"
                                                                             << explain;
  }
}

// A BETWEEN whose values read two relations, and one of whose comparisons reads only one, still
// compares both in the type of all three: a comparison bound again on its own relation keeps what
// the BETWEEN wrote for it.
TEST(BinderTest, ABetweenOverTwoRelationsKeepsItsType) {
  const Catalog catalog = MakeCatalog();
  auto plan = BindSql(
      "SELECT COUNT(*) FROM t AS a JOIN u AS b ON a.i16 = b.i16 "
      "WHERE a.i32 + 2147483647 + 1 BETWEEN b.i64 AND 5",
      catalog);
  ASSERT_TRUE(plan.ok()) << plan.status().ToString();
  const std::string explain = Explain(*plan);
  EXPECT_TRUE(explain.contains("Filter a.i32 <= -2147483643\n")) << explain;
}

// NOT over an = or a <> binds as the comparison it flips (through any NOTs), as DuckDB's parser
// writes it before its optimizer moves constants: NOT (x = k) is x <> k, which no moved constant
// that leaves the type decides, so its operand is computed as far as the moves reach. An ordered
// comparison keeps its NOT: its moves are the same either way, and NaN tells NOT (d < 1) from
// d >= 1 in antb1's IEEE comparison of DOUBLE values (divergence D10).
TEST(BinderTest, NotOverAnEqualityIsTheComparisonItFlips) {
  const Catalog catalog = MakeCatalog();
  struct Case {
    std::string_view where;
    bool flipped;  // else a NOT over the comparison
    CompareOp op;
    std::string_view constant;
    bool column;            // the moves reach the column; else an addition is left to compute
    std::string_view name;  // DuckDB's
  };
  for (const Case& c : {
           Case{.where = "NOT (i32 + 1 + -1 - 0 = 2147483648 - 1)",
                .flipped = true,
                .op = CompareOp::kNe,
                .constant = "2147483647",
                .column = true,
                .name = "((((i32 + 1) + -1) - 0) != (2147483648 - 1))"},
           Case{.where = "NOT (2 + (30000 + i16) = 2999997233 - 3000000000)",
                .flipped = true,
                .op = CompareOp::kNe,
                .constant = "-2769",
                .column = false,
                .name = "((2 + (30000 + i16)) != (2999997233 - 3000000000))"},
           Case{.where = "NOT (i16 + 1 <> 5)",
                .flipped = true,
                .op = CompareOp::kEq,
                .constant = "4",
                .column = true,
                .name = "((i16 + 1) = 5)"},
           Case{.where = "NOT NOT NOT (i16 + 1 = 5)",
                .flipped = true,
                .op = CompareOp::kNe,
                .constant = "4",
                .column = true,
                .name = "((i16 + 1) != 5)"},
           Case{.where = "NOT (i16 + 1 > 5)",
                .flipped = false,
                .op = CompareOp::kGt,
                .constant = "4",
                .column = true,
                .name = "((i16 + 1) <= 5)"},
       }) {
    const std::string sql = "SELECT COUNT(*) FROM t WHERE " + std::string(c.where);
    auto plan = BindSql(sql, catalog);
    ASSERT_TRUE(plan.ok()) << sql << ": " << plan.status().ToString();
    const auto& compute = std::get<ComputeNode>(Nth(*plan, 2));
    ASSERT_EQ(compute.exprs.size(), 1U) << sql;
    EXPECT_EQ(compute.exprs[0]->name, c.name) << sql;
    const Expr* compared = compute.exprs[0].get();
    if (!c.flipped) {
      const auto* negated = std::get_if<BoolExpr>(&compared->node);
      ASSERT_NE(negated, nullptr) << sql << ": a NOT is left";
      EXPECT_EQ(negated->op, BoolOp::kNot) << sql;
      ASSERT_EQ(negated->args.size(), 1U) << sql;
      compared = negated->args[0].get();
    }
    const auto* predicate = std::get_if<PredicateExpr>(&compared->node);
    ASSERT_NE(predicate, nullptr) << sql;
    EXPECT_EQ(predicate->predicate.kind, Predicate::Kind::kCompare) << sql;
    EXPECT_EQ(predicate->predicate.op, c.op) << sql;
    EXPECT_EQ(ToString(predicate->predicate.constant), c.constant) << sql;
    ASSERT_EQ(predicate->operands.size(), 1U) << sql;
    EXPECT_EQ(std::holds_alternative<ColumnExpr>(predicate->operands[0]->node), c.column) << sql;
  }
}

// A plain BETWEEN in WHERE or HAVING splits into its two comparisons, each folded exactly (so a
// scan filter); NOT BETWEEN, NOT (x BETWEEN ...) and BETWEEN under OR or in CASE are compound
// conditions, named as DuckDB names them. A constant integer expression on a side folds to its
// value.
TEST(BinderTest, BetweenAndConstantOperands) {
  const Catalog catalog = MakeCatalog();
  struct Case {
    std::string_view sql;
    std::string_view text;  // a part of EXPLAIN
  };
  for (const Case& c : {
           Case{.sql = "SELECT COUNT(*) FROM t WHERE i16 BETWEEN 1 AND 5",
                .text = "Filter i16 >= 1 AND i16 <= 5\n"},
           Case{.sql = "SELECT COUNT(*) FROM t WHERE i16 BETWEEN 1 AND 5 AND i32 = 2",
                .text = "Filter i16 >= 1 AND i16 <= 5 AND i32 = 2\n"},
           Case{.sql = "SELECT COUNT(*) FROM t WHERE (i32 = 2 AND i16 BETWEEN -1 AND 40000)",
                .text = "Filter i32 = 2 AND i16 >= -1 AND i16 IS NOT NULL\n"},
           Case{.sql = "SELECT COUNT(*) FROM t WHERE i16 BETWEEN 40000 AND 50000",
                .text = "Filter FALSE"},
           Case{.sql = "SELECT COUNT(*) FROM t WHERE dt BETWEEN DATE '2013-07-01' AND "
                       "CAST('2013-07-31' AS DATE)",
                .text = "Filter dt >= DATE '2013-07-01' AND dt <= DATE '2013-07-31'\n"},
           Case{.sql = "SELECT COUNT(*) FROM t WHERE i16 + 1 BETWEEN 2 * 3 AND 10 - 1",
                .text = "Filter i16 >= 5 AND i16 <= 8\n"},
           // 2147483648 is a BIGINT, so the addition is in BIGINT (2147483647 + 1 overflows
           // INTEGER).
           Case{.sql = "SELECT COUNT(*) FROM t WHERE i64 < 2147483648 + 1",
                .text = "Filter i64 < 2147483649\n"},
           // DuckDB types a literal INTEGER by its magnitude: -2147483648 is a BIGINT.
           Case{.sql = "SELECT COUNT(*) FROM t WHERE i64 > -2147483648 - 1",
                .text = "Filter i64 > -2147483649\n"},
           Case{.sql = "SELECT COUNT(*) FROM t WHERE i64 = -2147483648 * -1",
                .text = "Filter i64 = 2147483648\n"},
           // DuckDB folds the minus of a literal into it: -(-9223372036854775808) is a HUGEINT.
           Case{.sql = "SELECT COUNT(*) FROM t WHERE i64 < -(-9223372036854775808)",
                .text = "Filter i64 IS NOT NULL\n"},
           Case{.sql = "SELECT COUNT(*) FROM t WHERE h = -(-(9223372036854775808))",
                .text = "Filter h = 9223372036854775808\n"},
           Case{.sql = "SELECT COUNT(*) FROM t WHERE h > -(-9223372036854775808) - 1",
                .text = "Filter h > 9223372036854775807\n"},
           // A DOUBLE operand makes both comparisons DOUBLE, as DuckDB's common type.
           Case{.sql = "SELECT COUNT(*) FROM t WHERE d BETWEEN -9007199254740993 AND i64",
                .text = "Filter d >= -9007199254740992 AND d <= i64\n"},
           Case{.sql = "SELECT COUNT(*) FROM t WHERE d BETWEEN 1 AND 2147483647",
                .text = "Filter d >= 1 AND d <= 2147483647\n"},
           Case{.sql = "SELECT COUNT(*) FROM t WHERE -(3) * -(2) = i32",
                .text = "Filter i32 = 6\n"},
           Case{.sql = "SELECT COUNT(*) FROM t WHERE i32 BETWEEN i16 AND i64",
                .text = "Filter i32 >= i16 AND i32 <= i64\n"},
           Case{.sql = "SELECT COUNT(*) FROM t WHERE i16 NOT BETWEEN 1 AND 5",
                .text = "Compute (NOT (i16 BETWEEN 1 AND 5))\n"},
           Case{.sql = "SELECT COUNT(*) FROM t WHERE NOT i16 BETWEEN 1 AND 5",
                .text = "Compute (NOT (i16 BETWEEN 1 AND 5))\n"},
           Case{.sql = "SELECT COUNT(*) FROM t WHERE i16 BETWEEN 1 AND 5 OR i32 = 1",
                .text = "Compute ((i16 BETWEEN 1 AND 5) OR (i32 = 1))\n"},
           Case{.sql = "SELECT COUNT(*) FROM t WHERE i16 BETWEEN 1 AND 2 + 3 OR i32 = 1",
                .text = "Compute ((i16 BETWEEN 1 AND (2 + 3)) OR (i32 = 1))\n"},
           Case{.sql = "SELECT CASE WHEN i16 BETWEEN 1 AND 5 THEN 1 END FROM t",
                .text = "CASE  WHEN ((i16 BETWEEN 1 AND 5)) THEN (1) ELSE NULL END"},
           Case{.sql = "SELECT i16 FROM t GROUP BY i16 HAVING COUNT(*) BETWEEN 2 AND 3",
                .text = "Filter \"count_star()\" >= 2 AND \"count_star()\" <= 3\n"},
           Case{.sql = "SELECT i16 FROM t GROUP BY i16 HAVING COUNT(*) NOT BETWEEN 2 AND 3",
                .text = "(NOT (count_star() BETWEEN 2 AND 3))"},
       }) {
    auto plan = BindSql(c.sql, catalog);
    ASSERT_TRUE(plan.ok()) << c.sql << ": " << plan.status().ToString();
    const std::string explain = Explain(*plan);
    EXPECT_NE(explain.find(c.text), std::string::npos) << c.sql << "\n" << explain;
  }
}

// DuckDB moves constants in every comparison, HAVING and conditions over the aggregation included:
// over a key, an aggregate or an alias the arithmetic is never computed, so it never overflows.
TEST(BinderTest, HavingMovesConstantsLikeDuckDb) {
  const Catalog catalog = MakeCatalog();
  struct Case {
    std::string_view sql;
    std::string_view text;  // a part of EXPLAIN
  };
  for (const Case& c : {
           Case{.sql = "SELECT i16 FROM t GROUP BY i16 HAVING i16 + 30000 > 5",
                .text = "Filter i16 > -29995\n"},
           Case{.sql = "SELECT i16, MIN(i16) FROM t GROUP BY i16 HAVING MIN(i16) + 30000 > 5",
                .text = "Filter \"min(i16)\" > -29995\n"},
           Case{.sql = "SELECT i16, COUNT(*) AS n FROM t GROUP BY i16 HAVING n * 2 = 3",
                .text = "Filter FALSE\n"},
           Case{.sql = "SELECT i16 FROM t GROUP BY i16 HAVING 30000 - i16 < 20000",
                .text = "Filter i16 > 10000\n"},
           Case{.sql = "SELECT i16, CASE WHEN i16 + 30000 > 5 THEN 1 END FROM t GROUP BY i16",
                .text = "Compute CASE  WHEN (((i16 + 30000) > 5)) THEN (1) ELSE NULL END"},
           // The constants of a chain first and folded, then moved, as DuckDB moves them.
           Case{.sql = "SELECT i16 FROM t GROUP BY i16 HAVING i16 + 1 + 2 > 5",
                .text = "Filter i16 > 2\n"},
           // A cast GROUP BY key is one value, under a cast to its own type too: its operand,
           // no value of the output scope, is never bound alone.
           Case{.sql = "SELECT CAST(i16 AS INTEGER) AS k FROM t GROUP BY CAST(i16 AS INTEGER) "
                       "HAVING CAST(CAST(i16 AS INTEGER) AS INTEGER) > 1",
                .text = "Filter \"CAST(CAST(i16 AS INTEGER) AS INTEGER)\" > 1\n"},
           Case{.sql = "SELECT CAST(i16 AS INTEGER) AS k, CASE WHEN CAST(i16 AS INTEGER) = 7 THEN "
                       "1 END FROM t GROUP BY CAST(i16 AS INTEGER)",
                .text = "Compute CASE  WHEN ((CAST(i16 AS INTEGER) = 7)) THEN (1) ELSE NULL END"},
       }) {
    auto plan = BindSql(c.sql, catalog);
    ASSERT_TRUE(plan.ok()) << c.sql << ": " << plan.status().ToString();
    const std::string explain = Explain(*plan);
    EXPECT_NE(explain.find(c.text), std::string::npos) << c.sql << "\n" << explain;
    EXPECT_EQ(explain.find("(i16 + 30000)\n"), std::string::npos)
        << "no addition computed: " << c.sql << "\n"
        << explain;
  }
  // A constant that overflows its type is none (RewriteArithmetic leaves it unfolded), so the
  // operand is computed.
  auto overflowing =
      BindSql("SELECT i16 FROM t GROUP BY i16 HAVING i16 + (2147483647 + 1) > 5", catalog);
  ASSERT_TRUE(overflowing.ok()) << overflowing.status().ToString();
  EXPECT_TRUE(Explain(*overflowing).contains("Filter \"(i16 + (2147483647 + 1))\" > 5\n"))
      << Explain(*overflowing);
}

// The sum rewrite follows DuckDB: only without GROUP BY, for SUM(other + c) whatever `other` is
// (SUM(other) is rewritten again when it can be), counting `other` without its + c layers.
TEST(BinderTest, SumRewriteScopeLikeDuckDb) {
  const Catalog catalog = MakeCatalog();
  auto grouped = BindSql("SELECT SUM(i16 + 1) FROM t GROUP BY i32", catalog);
  ASSERT_TRUE(grouped.ok()) << grouped.status().ToString();
  EXPECT_NE(Explain(*grouped).find("SUM(\"(i16 + 1)\")"), std::string::npos) << Explain(*grouped);
  auto mixed = BindSql("SELECT SUM(i16 + (i16 - i16) + 1), SUM((i16 + 1) + 1) FROM t", catalog);
  ASSERT_TRUE(mixed.ok()) << mixed.status().ToString();
  const std::string explain = Explain(*mixed);
  EXPECT_NE(explain.find("SUM(\"(i16 + (i16 - i16))\")"), std::string::npos) << explain;
  EXPECT_NE(explain.find("COUNT(i16)"), std::string::npos) << explain;
  EXPECT_EQ(explain.find("(i16 + 1)\")"), std::string::npos) << "i16 + 1 is never computed\n"
                                                             << explain;
  auto having = BindSql("SELECT COUNT(*) FROM t HAVING SUM(i16 + 1) > 0", catalog);
  ASSERT_TRUE(having.ok()) << having.status().ToString();
  EXPECT_TRUE(std::holds_alternative<ProjectNode>(Nth(*having, 0)))
      << "the HAVING column is projected away\n"
      << Explain(*having);
}

// The sum rewrite reads the argument as DuckDB's expression rewriter leaves it, as DuckDB's sum
// rewriter does: a chain's constants first and folded, so one rewrite with their sum; a regrouped
// constant that overflows is none, so no rewrite; and constants that lead a chain stay apart, each
// a rewrite of its own whose COUNT counts what it adds to, as DuckDB counts it.
TEST(BinderTest, TheSumRewriteReadsTheArgumentAsDuckDbRewritesIt) {
  const Catalog catalog = MakeCatalog();
  const auto aggregates = [&catalog](std::string_view sql) {
    std::vector<std::string> out;
    auto plan = BindSql(sql, catalog);
    EXPECT_TRUE(plan.ok()) << sql << ": " << plan.status().ToString();
    if (!plan.ok()) {
      return out;
    }
    for (const LogicalNode* node = plan->root.get(); node != nullptr;) {
      if (const auto* aggregate = std::get_if<AggregateNode>(node)) {
        for (const AggregateCall& call : aggregate->aggregates) {
          out.push_back(std::format("{}({})", call.kind == AggKind::kSum ? "SUM" : "COUNT",
                                    call.arg.value_or(BoundColumn{}).name));
        }
        return out;
      }
      const std::vector<LogicalNodePtr> inputs = InputsOf(*node);
      node = inputs.size() == 1 ? inputs.front().get() : nullptr;
    }
    ADD_FAILURE() << sql << ": no aggregation";
    return out;
  };
  EXPECT_EQ(aggregates("SELECT SUM(i32 + 1 + 2) FROM t"),
            (std::vector<std::string>{"SUM(i32)", "COUNT(i32)"}));
  EXPECT_EQ(aggregates("SELECT SUM(i32 + 2147483647 + 1) FROM t"),
            (std::vector<std::string>{"SUM(((i32 + 2147483647) + 1))"}));
  EXPECT_EQ(aggregates("SELECT SUM(i32 + 1 + -1) FROM t"),
            (std::vector<std::string>{"SUM(((i32 + 1) + -1))"}))
      << "x + 0 is x: nothing to rewrite, and the argument computes nothing";
  EXPECT_EQ(aggregates("SELECT SUM(1 + (2 + i32)) FROM t"),
            (std::vector<std::string>{"SUM(i32)", "COUNT(i32)", "COUNT((2 + i32))"}));
  // A USMALLINT operand of an INTEGER sum, which DuckDB reads through a cast.
  EXPECT_EQ(aggregates("SELECT SUM(u16 + 2147483647) FROM t"),
            (std::vector<std::string>{"SUM(u16)", "COUNT(u16)"}));
  // SUM(i32) + 3 * COUNT(i32): the constants folded, one rewrite.
  auto plan = BindSql("SELECT SUM(i32 + 1 + 2) FROM t", catalog);
  ASSERT_TRUE(plan.ok()) << plan.status().ToString();
  const auto& post = std::get<ComputeNode>(Nth(*plan, 1));
  ASSERT_EQ(post.exprs.size(), 1U);
  const auto& sum = std::get<ArithExpr>(post.exprs[0]->node);
  const auto& product = std::get<ArithExpr>(sum.right->node);
  EXPECT_EQ(std::get<Int128>(std::get<ConstantExpr>(product.left->node).value.value), Int128{3});
}

// Inside ORDER BY and HAVING expressions a select alias is the fallback for a name that is no table
// column (DuckDB); an alias never refers to itself.
TEST(BinderTest, AliasesInsideOrderByAndHavingExpressions) {
  const Catalog catalog = MakeCatalog();
  for (const std::string_view sql : {
           "SELECT i16 + 1 AS k FROM t ORDER BY -k",
           "SELECT i32 AS k, SUM(i16) * 2 AS total FROM t GROUP BY i32 HAVING total + 1 > 0 "
           "ORDER BY total * k",
           "SELECT 5 AS five, i16 FROM t ORDER BY i16 * five",
           "SELECT COUNT(*) AS n FROM t HAVING n * 2 > 1 ORDER BY n + 1",
       }) {
    auto plan = BindSql(sql, catalog);
    EXPECT_TRUE(plan.ok()) << sql << ": " << plan.status().ToString();
  }
  auto self = BindSql("SELECT k + 1 AS k FROM t ORDER BY -k", catalog);
  ASSERT_FALSE(self.ok());
  EXPECT_NE(self.status().message().find("column 'k' does not exist"), std::string::npos);
  auto table_first = BindSql("SELECT i32 AS i16 FROM t ORDER BY -i16", catalog);
  ASSERT_TRUE(table_first.ok());
  EXPECT_NE(Explain(*table_first).find("-(i16)"), std::string::npos)
      << "inside an expression a table column comes before an alias\n"
      << Explain(*table_first);
}

// strlen (BIGINT, bytes) and regexp_replace (VARCHAR) of a VARCHAR, named like DuckDB (the function
// in lower case); regexp_replace takes a literal pattern and replacement. A GROUP BY alias of a
// function names its expression, which the select item then reads as the key.
TEST(BinderTest, StringFunctions) {
  const Catalog catalog = MakeCatalog();
  auto plan = BindSql(
      "SELECT REGEXP_REPLACE(s, '^(.)', '\\1') AS k, AVG(STRLEN(s)), MIN(s) FROM t GROUP BY k "
      "HAVING COUNT(*) > 1 ORDER BY strlen(k) DESC",
      catalog);
  ASSERT_TRUE(plan.ok()) << plan.status().ToString();
  EXPECT_EQ(plan->output[0].type, LogicalType::kVarchar);
  EXPECT_EQ(plan->output[1].name, "avg(strlen(s))");
  EXPECT_EQ(plan->output[1].type, LogicalType::kDouble);
  const std::string explain = Explain(*plan);
  EXPECT_NE(explain.find("GroupAggregate keys=[\"regexp_replace(s, '^(.)', '\\x5C1')\"]"),
            std::string::npos)
      << explain;
  EXPECT_NE(explain.find("Compute strlen(regexp_replace(s, '^(.)', '\\x5C1'))"), std::string::npos)
      << "strlen over the key, above the aggregation\n"
      << explain;
  auto where = BindSql("SELECT COUNT(*) FROM t WHERE strlen(s) > 3", catalog);
  ASSERT_TRUE(where.ok());
  EXPECT_EQ(std::get<FilterNode>(Nth(*where, 1)).predicates[0].constant.type, LogicalType::kBigInt);
}

// OR and NOT conjuncts are BOOLEAN expressions computed right before their filter (IS TRUE on the
// computed column); their leaves fold like WHERE's comparisons, a never-true one keeping its
// column so that NOT of it is NULL for NULL. HAVING's are computed over the aggregation.
TEST(BinderTest, BooleanConditions) {
  const Catalog catalog = MakeCatalog();
  auto plan =
      BindSql("SELECT COUNT(*) FROM t WHERE i32 > 0 AND (i16 = 1 OR s LIKE 'a%' AND NOT (i64 > 3))",
              catalog);
  ASSERT_TRUE(plan.ok()) << plan.status().ToString();
  std::string explain = Explain(*plan);
  EXPECT_NE(explain.find("Filter \"((i16 = 1) OR ((s ~~ 'a%') AND (i64 <= 3)))\"\n"
                         "    Compute ((i16 = 1) OR ((s ~~ 'a%') AND (i64 <= 3)))\n"
                         "      Filter i32 > 0"),
            std::string::npos)
      << explain;
  auto folded = BindSql("SELECT COUNT(*) FROM t WHERE NOT (i16 = 1.5) OR i16 + 1 > 10", catalog);
  ASSERT_TRUE(folded.ok()) << folded.status().ToString();
  const auto& compute = std::get<ComputeNode>(Nth(*folded, 2));
  ASSERT_EQ(compute.exprs.size(), 1U);
  const auto& any = std::get<BoolExpr>(compute.exprs[0]->node);
  ASSERT_EQ(any.args.size(), 2U);
  // NOT (i16 = 1.5) is i16 <> 1.5, as DuckDB's parser flips it: true for every i16 but NULL.
  const auto& always = std::get<PredicateExpr>(any.args[0]->node);
  EXPECT_EQ(always.predicate.kind, Predicate::Kind::kIsNotNull);
  ASSERT_TRUE(always.predicate.column.has_value()) << "NULL for a NULL i16, not TRUE";
  EXPECT_EQ(always.operands.size(), 1U);
  const auto& moved = std::get<PredicateExpr>(any.args[1]->node);
  EXPECT_EQ(moved.predicate.kind, Predicate::Kind::kCompare);
  EXPECT_EQ(ToString(moved.predicate.constant), "9") << "i16 > 9";
  ASSERT_EQ(moved.operands.size(), 1U);
  EXPECT_TRUE(std::holds_alternative<ColumnExpr>(moved.operands[0]->node))
      << "the constant moved off i16, so the addition is never computed";

  auto having = BindSql(
      "SELECT i16, COUNT(*) AS n FROM t GROUP BY i16 HAVING n > 1 OR NOT (MIN(s) LIKE 'x%')",
      catalog);
  ASSERT_TRUE(having.ok()) << having.status().ToString();
  explain = Explain(*having);
  EXPECT_NE(explain.find("Filter \"((n > 1) OR (NOT (min(s) ~~ 'x%')))\"\n"
                         "    Compute ((n > 1) OR (NOT (min(s) ~~ 'x%')))\n"
                         "      GroupAggregate"),
            std::string::npos)
      << explain;
}

// CASE is typed as DuckDB types it: the values' common type, where an integer literal takes the
// others' integer type when it fits, USMALLINT with SMALLINT is INTEGER and a string literal takes
// any type; the simple form compares with =.
TEST(BinderTest, CaseTypes) {
  const Catalog catalog = MakeCatalog();
  for (const auto& [expr, type] : std::to_array<std::pair<std::string_view, LogicalType>>({
           {"CASE WHEN i16 = 1 THEN i16 ELSE 0 END", LogicalType::kSmallInt},
           {"CASE WHEN i16 = 1 THEN i16 ELSE 40000 END", LogicalType::kInteger},
           {"CASE WHEN i16 = 1 THEN i16 ELSE i32 END", LogicalType::kInteger},
           {"CASE WHEN i16 = 1 THEN u16 ELSE i16 END", LogicalType::kInteger},
           {"CASE WHEN i16 = 1 THEN u16 ELSE i64 END", LogicalType::kBigInt},
           {"CASE WHEN i16 = 1 THEN 1 ELSE 100000000000000000000 END", LogicalType::kHugeInt},
           {"CASE WHEN i16 = 1 THEN i64 ELSE d END", LogicalType::kDouble},
           {"CASE WHEN i16 = 1 THEN i16 ELSE 1e3 END", LogicalType::kDouble},
           {"CASE WHEN i16 = 1 THEN 0.5 ELSE d END", LogicalType::kDouble},
           {"CASE WHEN i16 = 1 THEN 1 ELSE 3000000000 END", LogicalType::kBigInt},
           {"CASE WHEN i16 = 1 THEN 1 END", LogicalType::kInteger},
           {"CASE WHEN i16 = 1 THEN s ELSE '' END", LogicalType::kVarchar},
           {"CASE WHEN i16 = 1 THEN 'a' END", LogicalType::kVarchar},
           {"CASE WHEN i16 = 1 THEN dt ELSE '2020-01-02' END", LogicalType::kDate},
           {"CASE i16 WHEN 1 THEN dt WHEN 2 THEN DATE '2020-01-01' END", LogicalType::kDate},
       })) {
    auto plan = BindSql(std::format("SELECT {} FROM t", expr), catalog);
    ASSERT_TRUE(plan.ok()) << expr << ": " << plan.status().ToString();
    EXPECT_EQ(plan->output[0].type, type) << expr;
  }
  auto named = BindSql(
      "SELECT CASE i16 WHEN 1 THEN s END, CASE WHEN i16 > 0 OR i32 < 0 THEN 1 ELSE 2 END FROM t",
      catalog);
  ASSERT_TRUE(named.ok()) << named.status().ToString();
  EXPECT_EQ(named->output[0].name, "CASE  WHEN ((i16 = 1)) THEN (s) ELSE NULL END");
  // Conditions are named as DuckDB names them: NOT folded into a comparison or IN, <> as !=.
  for (const auto& [condition, name] :
       std::to_array<std::pair<std::string_view, std::string_view>>({
           {"NOT (i16 > 1)", "(i16 <= 1)"},
           {"NOT (NOT (i16 = 1))", "(i16 = 1)"},
           {"NOT NOT NOT i16 = 1", "(i16 != 1)"},
           {"NOT NOT i16 IN (1)", "(NOT (i16 NOT IN (1)))"},
           {"NOT NOT s LIKE 'a%'", "(NOT (NOT (s ~~ 'a%')))"},
           {"NOT NOT (i16 = 1 AND i32 = 5)", "(NOT (NOT ((i16 = 1) AND (i32 = 5))))"},
           {"NOT i16 IN (1, 2)", "(i16 NOT IN (1, 2))"},
           {"i16 IN (1, 2)", "(i16 IN (1, 2))"},
           {"NOT (i16 NOT IN (1, 2))", "(NOT (i16 NOT IN (1, 2)))"},
           {"i16 <> 1", "(i16 != 1)"},
           {"NOT (s LIKE 'a%')", "(NOT (s ~~ 'a%'))"},
           {"s NOT LIKE 'a%'", "(s !~~ 'a%')"},
           {"i16 = 1 OR (i16 = 2 OR i16 = 3)", "((i16 = 1) OR (i16 = 2) OR (i16 = 3))"},
           {"NOT (i16 = 1 AND i32 = 2)", "(NOT ((i16 = 1) AND (i32 = 2)))"},
           {"dt > DATE '2020-01-01'", "(dt > CAST('2020-01-01' AS \"DATE\"))"},
       })) {
    auto plan = BindSql(std::format("SELECT CASE WHEN {} THEN 1 END FROM t", condition), catalog);
    ASSERT_TRUE(plan.ok()) << condition << ": " << plan.status().ToString();
    EXPECT_EQ(plan->output[0].name, std::format("CASE  WHEN ({}) THEN (1) ELSE NULL END", name))
        << condition;
  }
  EXPECT_EQ(named->output[1].name, "CASE  WHEN (((i16 > 0) OR (i32 < 0))) THEN (1) ELSE 2 END");
  // A CASE key by its alias; aggregates inside a CASE of the output scope.
  auto grouped = BindSql(
      "SELECT CASE WHEN i16 = 0 AND i32 = 0 THEN s ELSE '' END AS k, COUNT(*), "
      "CASE WHEN SUM(i16) > 10 THEN 'many' END FROM t GROUP BY k",
      catalog);
  ASSERT_TRUE(grouped.ok()) << grouped.status().ToString();
  const std::string explain = Explain(*grouped);
  EXPECT_NE(explain.find("GroupAggregate keys=[\"CASE  WHEN (((i16 = 0) AND (i32 = 0))) THEN (s) "
                         "ELSE '' END\"]"),
            std::string::npos)
      << explain;
}

// toDateTime(t) is ClickBench's macro epoch_ms(t * 1000), the product typed as arithmetic;
// EXTRACT is BIGINT and date_trunc TIMESTAMP, of a TIMESTAMP or DATE; names are DuckDB's.
TEST(BinderTest, TimestampFunctions) {
  const Catalog catalog = MakeCatalog();
  auto plan = BindSql(
      "SELECT toDateTime(i64), EXTRACT(MINUTE FROM toDateTime(i64)) AS m, "
      "DATE_TRUNC('Hour', toDateTime(i64)), date_trunc('month', dt), extract(year FROM dt) FROM t",
      catalog);
  ASSERT_TRUE(plan.ok()) << plan.status().ToString();
  const std::vector<std::pair<std::string, LogicalType>> expected = {
      {"todatetime(i64)", LogicalType::kTimestamp},
      {"m", LogicalType::kBigInt},
      {"date_trunc('Hour', todatetime(i64))", LogicalType::kTimestamp},
      {"date_trunc('month', dt)", LogicalType::kTimestamp},
      {"main.date_part('year', dt)", LogicalType::kBigInt},
  };
  ASSERT_EQ(plan->output.size(), expected.size());
  for (std::size_t i = 0; i < expected.size(); ++i) {
    EXPECT_EQ(plan->output[i].name, expected[i].first);
    EXPECT_EQ(plan->output[i].type, expected[i].second) << expected[i].first;
  }
  // TIMESTAMP literals: a constant named as DuckDB names it; a TIMESTAMP compares with a TIMESTAMP,
  // string or DATE literal (its midnight) exactly.
  auto literals = BindSql(
      "SELECT TIMESTAMP '2013-07-15 14:00:00' FROM t WHERE toDateTime(i64) >= '2013-07-15' AND "
      "toDateTime(i64) < DATE '2013-07-16' AND toDateTime(i64) IN (TIMESTAMP '2013-07-15 "
      "14:00:00.5')",
      catalog);
  ASSERT_TRUE(literals.ok()) << literals.status().ToString();
  EXPECT_EQ(literals->output[0].name, "CAST('2013-07-15 14:00:00' AS TIMESTAMP)");
  EXPECT_EQ(literals->output[0].type, LogicalType::kTimestamp);
  const std::string literal_plan = Explain(*literals);
  EXPECT_NE(literal_plan.find("\"todatetime(i64)\" >= TIMESTAMP '2013-07-15 00:00:00'"),
            std::string::npos)
      << literal_plan;
  EXPECT_NE(literal_plan.find("\"todatetime(i64)\" < TIMESTAMP '2013-07-16 00:00:00'"),
            std::string::npos)
      << literal_plan;
  EXPECT_NE(literal_plan.find("IN (TIMESTAMP '2013-07-15 14:00:00.5')"), std::string::npos)
      << literal_plan;
  // DuckDB's date part spellings: synonyms of any case; epoch is DOUBLE; EXTRACT's six keywords
  // are named in lower case, other spellings as written.
  auto parts = BindSql(
      "SELECT EXTRACT(MS FROM toDateTime(i64)), EXTRACT(Epoch FROM dt), EXTRACT(YEARS FROM dt), "
      "EXTRACT(MONTH FROM dt), date_trunc('Weekday', dt), date_trunc('dec', dt), "
      "EXTRACT(Quarters FROM dt), EXTRACT(Msec FROM dt) FROM t",
      catalog);
  ASSERT_TRUE(parts.ok()) << parts.status().ToString();
  EXPECT_EQ(parts->output[0].name, "main.date_part('MS', todatetime(i64))");
  EXPECT_EQ(parts->output[0].type, LogicalType::kBigInt);
  EXPECT_EQ(parts->output[1].type, LogicalType::kDouble);
  EXPECT_EQ(parts->output[2].name, "main.date_part('year', dt)")
      << "a keyword spelling is named by its field";
  EXPECT_EQ(parts->output[3].name, "main.date_part('month', dt)");
  EXPECT_EQ(parts->output[4].name, "date_trunc('Weekday', dt)");
  EXPECT_EQ(parts->output[6].name, "main.date_part('quarter', dt)");
  EXPECT_EQ(parts->output[7].name, "main.date_part('Msec', dt)");
  const std::string trunc_plan = Explain(*parts);
  EXPECT_NE(trunc_plan.find("date_trunc('Weekday', dt)"), std::string::npos) << trunc_plan;
  // AVG of a DATE or TIMESTAMP is a TIMESTAMP, as in DuckDB.
  auto averages = BindSql("SELECT AVG(dt), AVG(toDateTime(i64)) FROM t GROUP BY i16", catalog);
  ASSERT_TRUE(averages.ok()) << averages.status().ToString();
  EXPECT_EQ(averages->output[0].type, LogicalType::kTimestamp);
  EXPECT_EQ(averages->output[1].type, LogicalType::kTimestamp);
  // The product keeps the argument's type: SMALLINT * 1000 is SMALLINT (it overflows above 32).
  auto small = BindSql("SELECT toDateTime(i16) FROM t", catalog);
  ASSERT_TRUE(small.ok()) << small.status().ToString();
  const auto& compute = std::get<ComputeNode>(Nth(*small, 1));
  const auto& epoch = std::get<FunctionExpr>(compute.exprs[0]->node);
  EXPECT_EQ(epoch.function, Function::kEpochMs);
  EXPECT_EQ(epoch.args[0]->type, LogicalType::kSmallInt);
  EXPECT_TRUE(std::holds_alternative<ArithExpr>(epoch.args[0]->node));
  // A TIMESTAMP key, selected and ordered by the same expression.
  auto grouped = BindSql(
      "SELECT date_trunc('minute', toDateTime(i64)) AS m, COUNT(*) FROM t GROUP BY "
      "date_trunc('minute', toDateTime(i64)) ORDER BY date_trunc('minute', toDateTime(i64))",
      catalog);
  ASSERT_TRUE(grouped.ok()) << grouped.status().ToString();
  const std::string explain = Explain(*grouped);
  EXPECT_NE(explain.find("GroupAggregate keys=[\"date_trunc('minute', todatetime(i64))\"]"),
            std::string::npos)
      << explain;
}

// An aggregate in a CASE operand, THEN or ELSE value, or in the source of EXTRACT makes the
// expression one over the aggregation, computed above it, where EXTRACT of a key reads the key. A
// CASE operand is read like any other operand: an aggregate of a CASE over a column is no constant.
TEST(BinderTest, AggregatesInsideCaseAndExtract) {
  const Catalog catalog = MakeCatalog();
  auto plan = BindSql(
      "SELECT CASE MAX(i16) WHEN 1 THEN 'one' END, CASE WHEN i16 = 1 THEN MAX(i32) END, "
      "CASE WHEN i16 = 1 THEN 0 ELSE MAX(i32) END, EXTRACT(year FROM dt), "
      "EXTRACT(month FROM MAX(dt)) FROM t GROUP BY i16, dt",
      catalog);
  ASSERT_TRUE(plan.ok()) << plan.status().ToString();
  const std::vector<std::pair<std::string, LogicalType>> expected = {
      {"CASE  WHEN ((max(i16) = 1)) THEN ('one') ELSE NULL END", LogicalType::kVarchar},
      {"CASE  WHEN ((i16 = 1)) THEN (max(i32)) ELSE NULL END", LogicalType::kInteger},
      {"CASE  WHEN ((i16 = 1)) THEN (0) ELSE max(i32) END", LogicalType::kInteger},
      {"main.date_part('year', dt)", LogicalType::kBigInt},
      {"main.date_part('month', max(dt))", LogicalType::kBigInt},
  };
  ASSERT_EQ(plan->output.size(), expected.size());
  for (std::size_t i = 0; i < expected.size(); ++i) {
    EXPECT_EQ(plan->output[i].name, expected[i].first) << i;
    EXPECT_EQ(plan->output[i].type, expected[i].second) << expected[i].first;
  }
  // Project <- Compute (the five expressions) <- GroupAggregate <- Scan.
  EXPECT_TRUE(std::holds_alternative<ProjectNode>(Nth(*plan, 0)));
  EXPECT_EQ(std::get<ComputeNode>(Nth(*plan, 1)).exprs.size(), expected.size());
  const auto& group = std::get<GroupAggregateNode>(Nth(*plan, 2));
  EXPECT_EQ(group.keys.size(), 2U);
  EXPECT_EQ(group.aggregates.size(), 3U) << "max(i16), max(i32) once, max(dt)";
  EXPECT_TRUE(std::holds_alternative<ScanNode>(Nth(*plan, 3)));

  auto sums = BindSql(
      "SELECT SUM(CASE i16 WHEN 1 THEN 1 END), SUM(CASE 1 WHEN i16 THEN 1 END) FROM t", catalog);
  ASSERT_TRUE(sums.ok()) << sums.status().ToString();
  ASSERT_EQ(sums->output.size(), 2U);
  EXPECT_EQ(sums->output[0].name, "sum(CASE  WHEN ((i16 = 1)) THEN (1) ELSE NULL END)");
  EXPECT_EQ(sums->output[1].name, "sum(CASE  WHEN ((1 = i16)) THEN (1) ELSE NULL END)");
  EXPECT_EQ(sums->output[0].type, LogicalType::kHugeInt);
  EXPECT_EQ(sums->output[1].type, LogicalType::kHugeInt);
}

TEST(BinderTest, ParenthesesGroupOnly) {
  const Catalog catalog = MakeCatalog();
  auto plain = BindSql(
      "SELECT i16, SUM(i32) FROM t WHERE i16 = 1 AND s = 'x' AND d > 0 GROUP BY i16", catalog);
  auto grouped = BindSql(
      "SELECT (i16), SUM((i32)) FROM t WHERE ((i16 = 1) AND (s = 'x')) AND (d > 0) GROUP BY (i16)",
      catalog);
  ASSERT_TRUE(plain.ok()) << plain.status().ToString();
  ASSERT_TRUE(grouped.ok()) << grouped.status().ToString();
  EXPECT_EQ(Explain(*plain), Explain(*grouped));
  EXPECT_EQ(std::get<FilterNode>(Nth(*grouped, 2)).predicates.size(), 3U);
}

// [NOT] IN binds each value like `column = value`: values no column value can equal are dropped,
// and without values IN is FALSE and NOT IN is IS NOT NULL.
TEST(BinderTest, In) {
  const Catalog catalog = MakeCatalog();
  auto plan = BindSql(
      "SELECT COUNT(*) FROM t WHERE i16 IN (1, 2.5, 40000, -3.0) AND s NOT IN ('a', 'b') AND "
      "u16 NOT IN (-1, 1.5) AND i32 IN (1.5) AND d IN (0.1)",
      catalog);
  ASSERT_TRUE(plan.ok()) << plan.status().ToString();
  const auto& predicates = std::get<FilterNode>(Nth(*plan, 1)).predicates;
  ASSERT_EQ(predicates.size(), 5U);
  EXPECT_EQ(predicates[0].kind, Predicate::Kind::kIn);
  ASSERT_EQ(predicates[0].values.size(), 2U);
  EXPECT_EQ(std::get<Int128>(predicates[0].values[0].value), Int128{1});
  EXPECT_EQ(std::get<Int128>(predicates[0].values[1].value), Int128{-3});
  EXPECT_EQ(predicates[0].values[1].type, LogicalType::kSmallInt);
  EXPECT_EQ(predicates[1].kind, Predicate::Kind::kNotIn);
  EXPECT_EQ(std::get<std::string>(predicates[1].values[1].value), "b");
  EXPECT_EQ(predicates[2].kind, Predicate::Kind::kIsNotNull);
  EXPECT_EQ(predicates[3].kind, Predicate::Kind::kFalse);
  EXPECT_FALSE(predicates[3].column.has_value());
  EXPECT_EQ(predicates[4].kind, Predicate::Kind::kIn);
  EXPECT_EQ(std::get<double>(predicates[4].values[0].value), 0.1);

  // DuckDB types the whole list as DOUBLE when one number is DOUBLE-typed (an exponent): every
  // integer is then the nearest double (2^53 + 1 becomes 2^53), folded exactly as in D7.
  auto exact = BindSql("SELECT COUNT(*) FROM t WHERE i64 IN (9007199254740993, 1)", catalog);
  ASSERT_TRUE(exact.ok()) << exact.status().ToString();
  EXPECT_EQ(std::get<Int128>(std::get<FilterNode>(Nth(*exact, 1)).predicates[0].values[0].value),
            Int128{9007199254740993});
  auto doubled = BindSql("SELECT COUNT(*) FROM t WHERE i64 IN (9007199254740993, 1e0)", catalog);
  ASSERT_TRUE(doubled.ok()) << doubled.status().ToString();
  const auto& values = std::get<FilterNode>(Nth(*doubled, 1)).predicates[0].values;
  ASSERT_EQ(values.size(), 2U);
  EXPECT_EQ(std::get<Int128>(values[0].value), Int128{9007199254740992});
  EXPECT_EQ(std::get<Int128>(values[1].value), Int128{1});
}

// The predicates of every Filter of a plan, from the root down.
std::vector<Predicate> FilterPredicates(const LogicalPlan& plan) {
  std::vector<Predicate> out;
  for (const LogicalNode* node = plan.root.get(); node != nullptr;) {
    if (const auto* filter = std::get_if<FilterNode>(node)) {
      out.insert(out.end(), filter->predicates.begin(), filter->predicates.end());
    }
    const std::vector<LogicalNodePtr> inputs = InputsOf(*node);
    node = inputs.empty() ? nullptr : inputs[0].get();
  }
  return out;
}

// A constant integer expression in an IN list is the literal of its value, as on a side of a
// comparison: the list binds exactly as the one written with those literals, in WHERE and HAVING,
// also where a DOUBLE-typed number makes the list DOUBLE, and an overflow is the comparison's
// error.
TEST(BinderTest, InListFoldsConstantIntegerExpressions) {
  const Catalog catalog = MakeCatalog();
  struct Case {
    std::string_view folded;
    std::string_view literal;
  };
  for (const Case& c : {
           Case{.folded = "SELECT COUNT(*) FROM t WHERE i16 IN (10, 10 + 1, 2 * 3, -(4))",
                .literal = "SELECT COUNT(*) FROM t WHERE i16 IN (10, 11, 6, -4)"},
           Case{.folded = "SELECT COUNT(*) FROM t WHERE i32 NOT IN (300 + 5, 7 - 8)",
                .literal = "SELECT COUNT(*) FROM t WHERE i32 NOT IN (305, -1)"},
           Case{.folded = "SELECT COUNT(*) FROM t WHERE i64 IN (9007199254740992 + 1, 1e0)",
                .literal = "SELECT COUNT(*) FROM t WHERE i64 IN (9007199254740993, 1e0)"},
           Case{.folded = "SELECT COUNT(*) FROM t WHERE i16 IN (40000 - 1)",
                .literal = "SELECT COUNT(*) FROM t WHERE i16 IN (39999)"},
           Case{.folded = "SELECT i16 FROM t GROUP BY i16 HAVING MAX(i32) IN (1 + 1, 3)",
                .literal = "SELECT i16 FROM t GROUP BY i16 HAVING MAX(i32) IN (2, 3)"},
       }) {
    auto folded = BindSql(c.folded, catalog);
    auto literal = BindSql(c.literal, catalog);
    ASSERT_TRUE(folded.ok()) << c.folded << ": " << folded.status().ToString();
    ASSERT_TRUE(literal.ok()) << c.literal << ": " << literal.status().ToString();
    const std::vector<Predicate> a = FilterPredicates(*folded);
    const std::vector<Predicate> b = FilterPredicates(*literal);
    ASSERT_EQ(a.size(), b.size()) << c.folded;
    for (std::size_t i = 0; i < a.size(); ++i) {
      EXPECT_EQ(a[i].kind, b[i].kind) << c.folded;
      ASSERT_EQ(a[i].values.size(), b[i].values.size()) << c.folded;
      for (std::size_t j = 0; j < a[i].values.size(); ++j) {
        EXPECT_EQ(a[i].values[j].type, b[i].values[j].type) << c.folded;
        EXPECT_EQ(a[i].values[j].value, b[i].values[j].value) << c.folded;
      }
    }
  }
  auto in = BindSql("SELECT COUNT(*) FROM t WHERE i32 IN (1, 2147483647 + 1)", catalog);
  auto eq = BindSql("SELECT COUNT(*) FROM t WHERE i32 = 2147483647 + 1", catalog);
  ASSERT_FALSE(in.ok());
  ASSERT_FALSE(eq.ok());
  EXPECT_EQ(in.status().message(), eq.status().message());
}

// A DECIMAL compares with a DECIMAL of another precision or scale, with an integer and with a
// DOUBLE (ADR 0021 rule 11): two columns, which the executor compares exactly by value or in
// DOUBLE; in WHERE, HAVING and a CASE condition alike.
TEST(BinderTest, DecimalComparesWithOtherNumbers) {
  const Catalog catalog = MakeCatalog();
  for (const char* sql : {
           "SELECT COUNT(*) FROM dec WHERE p = r",
           "SELECT COUNT(*) FROM dec WHERE i < p",
           "SELECT COUNT(*) FROM dec WHERE p >= b",
           "SELECT COUNT(*) FROM dec WHERE z <> g",
           "SELECT COUNT(*) FROM dec WHERE p < f",
           "SELECT COUNT(*) FROM dec WHERE f = z",
           "SELECT COUNT(*) FROM dec WHERE p * i > r",
           "SELECT i FROM dec GROUP BY i HAVING SUM(p) = MAX(r)",
           "SELECT i FROM dec GROUP BY i HAVING SUM(p) > AVG(f)",
           "SELECT i FROM dec GROUP BY i HAVING SUM(i) <= MIN(z)",
       }) {
    auto plan = BindSql(sql, catalog);
    ASSERT_TRUE(plan.ok()) << sql << ": " << plan.status().ToString();
    const std::vector<Predicate> predicates = FilterPredicates(*plan);
    ASSERT_EQ(predicates.size(), 1U) << sql;
    EXPECT_EQ(predicates[0].kind, Predicate::Kind::kCompareColumns) << sql;
  }
  for (const char* sql : {
           "SELECT CASE WHEN p > r THEN 1 ELSE 0 END FROM dec",
           "SELECT CASE p WHEN i THEN 1 END FROM dec",
           "SELECT COUNT(*) FROM dec WHERE p > q OR p = b OR NOT (z < f)",
           "SELECT SUM(CASE WHEN p > r THEN 1 END) FROM dec",
       }) {
    auto plan = BindSql(sql, catalog);
    EXPECT_TRUE(plan.ok()) << sql << ": " << plan.status().ToString();
  }
}

// A DECIMAL against a number DuckDB types as DOUBLE (an exponent, a decimal of more than 38 digits,
// an integer outside -2^127 to 2^128 - 1), or in an IN list with one, compares in DOUBLE: nothing
// is folded, and the constants are the doubles DuckDB converts the literals to (ADR 0021 rules 8
// and 11).
TEST(BinderTest, DecimalAgainstDoubleNumbersComparesInDouble) {
  const Catalog catalog = MakeCatalog();
  const auto predicate = [&](std::string_view where) -> Predicate {
    const std::string sql = "SELECT COUNT(*) FROM dec WHERE " + std::string(where);
    auto plan = BindSql(sql, catalog);
    EXPECT_TRUE(plan.ok()) << sql << ": " << plan.status().ToString();
    if (!plan.ok()) {
      return {};
    }
    return FilterPredicates(*plan).at(0);
  };
  const auto real = [](const Constant& c) {
    EXPECT_EQ(c.type, LogicalType::kDouble);
    const auto* value = std::get_if<double>(&c.value);
    return value != nullptr ? *value : -1.0;
  };
  const Predicate gt = predicate("p > 1e1");
  EXPECT_EQ(gt.kind, Predicate::Kind::kCompare);
  EXPECT_EQ(gt.op, CompareOp::kGt);
  EXPECT_EQ(real(gt.constant), 10.0);
  EXPECT_EQ(gt.column.value_or(BoundColumn{}).type, LogicalType::Decimal(15, 2));
  const Predicate eq = predicate("p = 1e-1");
  EXPECT_EQ(eq.kind, Predicate::Kind::kCompare);
  EXPECT_EQ(real(eq.constant), 0.1);
  EXPECT_EQ(real(predicate("p <= 1.00000000000000000000000000000000000001").constant), 1.0);
  EXPECT_EQ(real(predicate("r < 340282366920938463463374607431768211456").constant), 0x1p128);
  EXPECT_EQ(real(predicate("z > -1e400").constant), -std::numeric_limits<double>::infinity());
  EXPECT_EQ(real(predicate("1e2 < p * i").constant), 100.0);
  // An IN list with such a number: every value a double, a decimal literal converted as DuckDB
  // converts its DECIMAL (9007199254740993.5 is 2^53), a HUGEINT through DuckDB's 128-bit formula.
  const Predicate in = predicate("z IN (1.5, 9007199254740993.5, 27670116110564329473, 2e0)");
  EXPECT_EQ(in.kind, Predicate::Kind::kIn);
  ASSERT_EQ(in.values.size(), 4U);
  EXPECT_EQ(real(in.values[0]), 1.5);
  EXPECT_EQ(real(in.values[1]), 0x1p53);
  EXPECT_EQ(real(in.values[2]), 27670116110564327424.0);
  EXPECT_EQ(real(in.values[3]), 2.0);
  const Predicate not_in = predicate("p NOT IN (1, 2, 340282366920938463463374607431768211456)");
  EXPECT_EQ(not_in.kind, Predicate::Kind::kNotIn);
  ASSERT_EQ(not_in.values.size(), 3U);
  EXPECT_EQ(real(not_in.values[0]), 1.0);
  EXPECT_EQ(real(not_in.values[2]), 0x1p128);
  // Without one the list folds exactly into the column's scale: 1.001 cannot match DECIMAL(15,2).
  const Predicate exact = predicate("p IN (1.5, 1.001)");
  ASSERT_EQ(exact.values.size(), 1U);
  EXPECT_EQ(exact.values[0].type, LogicalType::Decimal(15, 2));
  EXPECT_EQ(std::get<Int128>(exact.values[0].value), Int128{150});
  // HAVING over a DECIMAL aggregate compares in DOUBLE too.
  auto having = BindSql("SELECT i FROM dec GROUP BY i HAVING SUM(p) > 1e3", catalog);
  ASSERT_TRUE(having.ok()) << having.status().ToString();
  EXPECT_EQ(real(FilterPredicates(*having).at(0).constant), 1000.0);
  // An integer column keeps today's rule: only an exponent or a decimal of more than 38 digits
  // make the list DOUBLE, so a 40-digit integer folds exactly (and drops out).
  auto integer = BindSql(
      "SELECT COUNT(*) FROM t WHERE i64 IN (9007199254740993, "
      "400000000000000000000000000000000000000)",
      catalog);
  ASSERT_TRUE(integer.ok()) << integer.status().ToString();
  const std::vector<Constant> values = FilterPredicates(*integer).at(0).values;
  ASSERT_EQ(values.size(), 1U);
  EXPECT_EQ(std::get<Int128>(values[0].value), Int128{9007199254740993});
}

// A BETWEEN with a DECIMAL binds as its two comparisons where they compare as DuckDB's one type
// does: without a DOUBLE value (exactly), or with both pairs in DOUBLE (a DOUBLE operand, or two
// DOUBLE bounds). The other mixes are unsupported (Expressions/BindErrorTest).
TEST(BinderTest, DecimalBetweenWhereThePairsAgree) {
  const Catalog catalog = MakeCatalog();
  for (const char* where :
       {"p BETWEEN 1e0 AND 2e0", "f BETWEEN p AND 1.5", "1e0 BETWEEN p AND q", "p BETWEEN r AND i",
        "p BETWEEN 1 AND 2.5", "z NOT BETWEEN b AND p", "f > 0 OR p BETWEEN 1e0 AND 1e1"}) {
    const std::string sql = "SELECT COUNT(*) FROM dec WHERE " + std::string(where);
    auto plan = BindSql(sql, catalog);
    EXPECT_TRUE(plan.ok()) << sql << ": " << plan.status().ToString();
  }
  auto both_double = BindSql("SELECT COUNT(*) FROM dec WHERE p BETWEEN 1e0 AND 2e0", catalog);
  ASSERT_TRUE(both_double.ok()) << both_double.status().ToString();
  const std::vector<Predicate> predicates = FilterPredicates(*both_double);
  ASSERT_EQ(predicates.size(), 2U);
  for (const Predicate& p : predicates) {
    EXPECT_EQ(p.constant.type, LogicalType::kDouble);
  }
}

// A DECIMAL that is no literal next to a FLOAT column stays unsupported (divergence D11): antb1
// reads FLOAT as DOUBLE, and DuckDB would compare in FLOAT.
TEST(BinderTest, DecimalAgainstAFloatColumnIsUnsupported) {
  Catalog catalog;
  ASSERT_TRUE(catalog
                  .Register("tf", std::make_shared<FakeTable>(
                                      arrow::schema({arrow::field("f", arrow::float32()),
                                                     arrow::field("p", arrow::decimal128(15, 2)),
                                                     arrow::field("d", arrow::float64()),
                                                     arrow::field("k", arrow::int32())}),
                                      10, std::vector<int>{0}))
                  .ok());
  for (const char* sql :
       {"SELECT COUNT(*) FROM tf WHERE p < f", "SELECT COUNT(*) FROM tf WHERE f = p",
        "SELECT k FROM tf GROUP BY k HAVING MIN(p) = MIN(f)",
        "SELECT COUNT(*) FROM tf WHERE p BETWEEN f AND 1",
        "SELECT COUNT(*) FROM tf WHERE p BETWEEN 1 AND f",
        "SELECT CASE WHEN p > f THEN 1 END FROM tf",
        "SELECT CASE WHEN k > 0 THEN p ELSE f END FROM tf",
        "SELECT CASE WHEN k > 0 THEN f ELSE p END FROM tf"}) {
    const auto plan = BindSql(sql, catalog);
    ASSERT_FALSE(plan.ok()) << sql;
    const auto detail = GetSqlError(plan.status());
    ASSERT_NE(detail, nullptr) << sql;
    EXPECT_EQ(detail->kind(), SqlErrorDetail::Kind::kUnsupported) << sql;
  }
  auto with_double = BindSql("SELECT COUNT(*) FROM tf WHERE p < d", catalog);
  EXPECT_TRUE(with_double.ok()) << with_double.status().ToString();
}

// [NOT] LIKE binds to its own predicate kinds with the pattern as a VARCHAR constant; a pattern of
// only % folds (LIKE: IS NOT NULL, NOT LIKE: FALSE).
TEST(BinderTest, Like) {
  const Catalog catalog = MakeCatalog();
  auto plan = BindSql(
      "SELECT COUNT(*) FROM t WHERE s LIKE '%a_%' AND s NOT LIKE '' AND s LIKE '%%' AND "
      "\"Mixed Case\" > 1 AND s NOT LIKE '%'",
      catalog);
  ASSERT_TRUE(plan.ok()) << plan.status().ToString();
  const auto& predicates = std::get<FilterNode>(Nth(*plan, 1)).predicates;
  ASSERT_EQ(predicates.size(), 5U);
  EXPECT_EQ(predicates[0].kind, Predicate::Kind::kLike);
  EXPECT_EQ(std::get<std::string>(predicates[0].constant.value), "%a_%");
  EXPECT_EQ(predicates[0].constant.type, LogicalType::kVarchar);
  EXPECT_EQ(predicates[0].column.value_or(BoundColumn{}).index, 6);
  EXPECT_EQ(predicates[1].kind, Predicate::Kind::kNotLike);
  EXPECT_EQ(std::get<std::string>(predicates[1].constant.value), "");
  EXPECT_EQ(predicates[2].kind, Predicate::Kind::kIsNotNull);
  EXPECT_EQ(predicates[3].kind, Predicate::Kind::kCompare);
  EXPECT_EQ(predicates[4].kind, Predicate::Kind::kFalse);
  EXPECT_FALSE(predicates[4].column.has_value());
}

// Constant items get DuckDB's types and names and become constants of the Project; positions name
// select items in GROUP BY and ORDER BY; GROUP BY a constant is a grouping without keys.
TEST(BinderTest, ConstantsAndPositions) {
  const Catalog catalog = MakeCatalog();
  auto plan = BindSql(
      "SELECT 1, -5 AS m, 3000000000, 'it''s', DATE '2020-01-02', i16 FROM t ORDER BY 6 DESC, 1",
      catalog);
  ASSERT_TRUE(plan.ok()) << plan.status().ToString();
  ASSERT_EQ(plan->output.size(), 6U);
  EXPECT_EQ(plan->output[0].name, "1");
  EXPECT_EQ(plan->output[0].type, LogicalType::kInteger);
  EXPECT_EQ(plan->output[1].name, "m");
  EXPECT_EQ(plan->output[2].type, LogicalType::kBigInt);
  EXPECT_EQ(plan->output[3].name, "'it''s'");
  EXPECT_EQ(plan->output[3].type, LogicalType::kVarchar);
  EXPECT_EQ(plan->output[4].name, "CAST('2020-01-02' AS \"DATE\")");
  EXPECT_EQ(plan->output[4].type, LogicalType::kDate);
  const auto& project = std::get<ProjectNode>(Nth(*plan, 0));
  ASSERT_EQ(project.constants.size(), 6U);
  EXPECT_EQ(std::get<Int128>(project.constants[1].value_or(Constant{}).value), Int128{-5});
  EXPECT_FALSE(project.constants[5].has_value());
  EXPECT_EQ(project.columns[5].index, 0);
  const auto& sort = std::get<SortNode>(Nth(*plan, 1));
  ASSERT_EQ(sort.keys.size(), 1U);  // ORDER BY 1 is a constant: it orders nothing
  EXPECT_EQ(sort.keys[0].column.index, 0);
  EXPECT_TRUE(sort.keys[0].descending);

  // DuckDB types the magnitude as INTEGER when it fits, and names integers by their value.
  auto integers = BindSql(
      "SELECT -2147483648, -2147483647, -9223372036854775808, 9223372036854775808, 007, -0 FROM t",
      catalog);
  ASSERT_TRUE(integers.ok()) << integers.status().ToString();
  const auto& out = integers->output;
  EXPECT_EQ(out[0].type, LogicalType::kBigInt);
  EXPECT_EQ(out[0].name, "-2147483648");
  EXPECT_EQ(out[1].type, LogicalType::kInteger);
  EXPECT_EQ(out[2].type, LogicalType::kBigInt);
  EXPECT_EQ(out[3].type, LogicalType::kHugeInt);
  EXPECT_EQ(out[4].name, "7");
  EXPECT_EQ(out[5].name, "0");
  EXPECT_EQ(out[5].type, LogicalType::kInteger);

  // A minus over a number is part of the literal, through parentheses and other minuses, as
  // DuckDB's parser folds it: named and typed by the literal's value.
  auto folded = BindSql(
      "SELECT -(1), - -1, -(-(1)), -(-9223372036854775808), - -2147483648, -(1.5), -(0) "
      "FROM t",
      catalog);
  ASSERT_TRUE(folded.ok()) << folded.status().ToString();
  const auto& negated = folded->output;
  ASSERT_EQ(negated.size(), 7U);
  for (std::size_t i = 0;
       const auto& [name, type] : std::to_array<std::pair<std::string_view, LogicalType>>({
           {"-1", LogicalType::kInteger},
           {"1", LogicalType::kInteger},
           {"1", LogicalType::kInteger},
           {"9223372036854775808", LogicalType::kHugeInt},
           {"2147483648", LogicalType::kBigInt},
           {"-1.5", LogicalType::Decimal(2, 1)},
           {"0", LogicalType::kInteger},
       })) {
    EXPECT_EQ(negated[i].name, name) << i;
    EXPECT_EQ(negated[i].type, type) << i;
    ++i;
  }

  auto grouped = BindSql("SELECT 7, s, COUNT(*) FROM t GROUP BY 1, 2 ORDER BY 3 DESC", catalog);
  ASSERT_TRUE(grouped.ok()) << grouped.status().ToString();
  const auto& group = std::get<GroupAggregateNode>(Nth(*grouped, 2));
  ASSERT_EQ(group.keys.size(), 1U);
  EXPECT_EQ(group.keys[0].index, 6);
  EXPECT_EQ(std::get<SortNode>(Nth(*grouped, 1)).keys[0].column.index, 1);

  auto constant_only = BindSql("SELECT 1, COUNT(*) FROM t GROUP BY 1", catalog);
  ASSERT_TRUE(constant_only.ok()) << constant_only.status().ToString();
  EXPECT_TRUE(std::get<GroupAggregateNode>(Nth(*constant_only, 1)).keys.empty());

  auto global = BindSql("SELECT 1, COUNT(*) FROM t", catalog);
  ASSERT_TRUE(global.ok()) << global.status().ToString();
  EXPECT_TRUE(std::holds_alternative<ProjectNode>(Nth(*global, 0)));
  EXPECT_TRUE(std::holds_alternative<AggregateNode>(Nth(*global, 1)));
  auto ordered_global = BindSql("SELECT 1 FROM t ORDER BY COUNT(*)", catalog);
  ASSERT_TRUE(ordered_global.ok()) << ordered_global.status().ToString();
  EXPECT_TRUE(std::holds_alternative<AggregateNode>(Nth(*ordered_global, 1)))
      << "an ORDER BY aggregate makes one row";
}

// A minus over a number is part of the literal (the parser folds it, as DuckDB's parser does,
// through parentheses and other minuses): a query binds to the plan, names and types of the same
// query with the literal written, in every clause.
TEST(BinderTest, AMinusOverANumberBindsAsTheLiteral) {
  const Catalog catalog = MakeCatalog();
  for (const auto& [folded, literal] :
       std::to_array<std::pair<std::string_view, std::string_view>>({
           {"SELECT -(1), - -1, -(-(1)), -((2)), -(1.5), -(-9223372036854775808) FROM t",
            "SELECT -1, 1, 1, -2, -1.5, 9223372036854775808 FROM t"},
           {"SELECT i16 + -(1), i16 * - -2, i32 - -(-(3)), i64 // -(2), -(1.5) / 3 FROM t",
            "SELECT i16 + -1, i16 * 2, i32 - 3, i64 // -2, -1.5 / 3 FROM t"},
           {"SELECT CASE WHEN i32 = 1 THEN -(7) WHEN i32 = 2 THEN - -8 ELSE i16 END FROM t",
            "SELECT CASE WHEN i32 = 1 THEN -7 WHEN i32 = 2 THEN 8 ELSE i16 END FROM t"},
           {"SELECT i32 FROM t WHERE i16 + -(1) > -(-(2)) AND i64 BETWEEN -(5) AND - -5 "
            "AND i32 IN (-(1), - -2) AND d < -(1.5) / 3",
            "SELECT i32 FROM t WHERE i16 + -1 > 2 AND i64 BETWEEN -5 AND 5 AND i32 IN (-1, 2) "
            "AND d < -1.5 / 3"},
           {"SELECT COUNT(*) FROM dec WHERE p > -(1.5) AND p IN (-(2.5), 3.0)",
            "SELECT COUNT(*) FROM dec WHERE p > -1.5 AND p IN (-2.5, 3.0)"},
           {"SELECT i16, s, COUNT(*) FROM t GROUP BY -(-1), - -2 HAVING COUNT(*) > -(-(1)) "
            "ORDER BY -(-3) DESC, - -1",
            "SELECT i16, s, COUNT(*) FROM t GROUP BY 1, 2 HAVING COUNT(*) > 1 ORDER BY 3 DESC, 1"},
           {"SELECT SUM(i32 + -(1)), SUM(i16 * - -2) FROM t",
            "SELECT SUM(i32 + -1), SUM(i16 * 2) FROM t"},
           {"SELECT COUNT(*) FROM t JOIN ok ON t.i16 = ok.i16 + -(1) WHERE t.i32 > - -3",
            "SELECT COUNT(*) FROM t JOIN ok ON t.i16 = ok.i16 + -1 WHERE t.i32 > 3"},
       })) {
    auto a = BindSql(folded, catalog);
    auto b = BindSql(literal, catalog);
    ASSERT_TRUE(a.ok()) << folded << ": " << a.status().ToString();
    ASSERT_TRUE(b.ok()) << literal << ": " << b.status().ToString();
    EXPECT_EQ(Explain(*a), Explain(*b)) << folded;
    ASSERT_EQ(a->output.size(), b->output.size()) << folded;
    for (std::size_t i = 0; i < a->output.size(); ++i) {
      EXPECT_EQ(a->output[i].name, b->output[i].name) << folded;
      EXPECT_EQ(a->output[i].type, b->output[i].type) << folded;
    }
  }
}

// CAST('YYYY-MM-DD' AS DATE) and 'YYYY-MM-DD'::DATE are the DATE literal, as in DuckDB, which names
// all three CAST('YYYY-MM-DD' AS "DATE"): the same plan in every clause.
TEST(BinderTest, DateCastsAreDateLiterals) {
  const Catalog catalog = MakeCatalog();
  struct Case {
    std::string_view literal;
    std::string_view cast;
  };
  for (const Case& c : {
           Case{.literal = "SELECT DATE '2020-01-02', dt FROM t WHERE dt >= DATE '2013-07-01' AND "
                           "DATE '2013-07-15' > dt",
                .cast = "SELECT CAST('2020-01-02' AS DATE), dt FROM t WHERE dt >= "
                        "'2013-07-01'::date AND cast('2013-07-15' as Date) > dt"},
           Case{.literal = "SELECT COUNT(*) FROM t WHERE dt IN (DATE '2013-07-01', DATE "
                           "'2013-07-02') AND (dt NOT IN (DATE '2013-07-03') OR dt <> DATE "
                           "'2013-07-04')",
                .cast = "SELECT COUNT(*) FROM t WHERE dt IN ('2013-07-01'::DATE, CAST('2013-07-02' "
                        "AS DATE)) AND (dt NOT IN (('2013-07-03')::DATE) OR dt <> "
                        "(CAST('2013-07-04' AS DATE)))"},
           Case{.literal = "SELECT i16, MIN(dt) FROM t GROUP BY i16 HAVING MAX(dt) < DATE "
                           "'2013-07-15' OR MIN(dt) IN (DATE '2013-07-01')",
                .cast = "SELECT i16, MIN(dt) FROM t GROUP BY i16 HAVING MAX(dt) < "
                        "'2013-07-15'::DATE OR MIN(dt) IN (CAST('2013-07-01' AS DATE))"},
           Case{.literal = "SELECT CASE WHEN i16 = 1 THEN dt ELSE DATE '2013-07-01' END, CASE dt "
                           "WHEN DATE '2013-07-02' THEN 1 END FROM t",
                .cast = "SELECT CASE WHEN i16 = 1 THEN dt ELSE '2013-07-01'::DATE END, CASE dt "
                        "WHEN CAST('2013-07-02' AS DATE) THEN 1 END FROM t"},
           Case{.literal = "SELECT CASE WHEN i16 = 1 THEN DATE '2013-07-01' ELSE dt END, CASE DATE "
                           "'2013-07-02' WHEN dt THEN 1 END FROM t",
                .cast = "SELECT CASE WHEN i16 = 1 THEN '2013-07-01'::DATE ELSE dt END, CASE "
                        "CAST('2013-07-02' AS DATE) WHEN dt THEN 1 END FROM t"},
           Case{.literal = "SELECT MAX(CASE WHEN i16 = 1 THEN dt ELSE DATE '2013-07-01' END), "
                           "EXTRACT(year FROM DATE '2013-07-15'), date_trunc('month', DATE "
                           "'2013-07-15') FROM t",
                .cast = "SELECT MAX(CASE WHEN i16 = 1 THEN dt ELSE '2013-07-01'::DATE END), "
                        "EXTRACT(year FROM CAST('2013-07-15' AS DATE)), date_trunc('month', "
                        "'2013-07-15'::DATE) FROM t"},
           Case{.literal = "SELECT DATE '2020-01-02' AS k, s, COUNT(*) FROM t GROUP BY DATE "
                           "'2020-01-02', s ORDER BY DATE '2024-01-31', 3 DESC",
                .cast = "SELECT '2020-01-02'::DATE AS k, s, COUNT(*) FROM t GROUP BY "
                        "CAST('2020-01-02' AS DATE), s ORDER BY '2024-01-31'::DATE, 3 DESC"},
           Case{.literal = "SELECT toDateTime(i64) FROM t WHERE toDateTime(i64) >= DATE "
                           "'2013-07-15' ORDER BY 1 LIMIT 3",
                .cast = "SELECT toDateTime(i64) FROM t WHERE toDateTime(i64) >= "
                        "CAST('2013-07-15' AS DATE) ORDER BY 1 LIMIT 3"},
       }) {
    auto literal = BindSql(c.literal, catalog);
    auto cast = BindSql(c.cast, catalog);
    ASSERT_TRUE(literal.ok()) << c.literal << ": " << literal.status().ToString();
    ASSERT_TRUE(cast.ok()) << c.cast << ": " << cast.status().ToString();
    EXPECT_EQ(Explain(*cast), Explain(*literal)) << c.cast;
  }

  constexpr std::string_view kSql = "SELECT '2020-01-02'::DATE, CAST('2024-01-31' AS DATE) FROM t";
  auto plan = BindSql(kSql, catalog);
  ASSERT_TRUE(plan.ok()) << plan.status().ToString();
  ASSERT_EQ(plan->output.size(), 2U);
  EXPECT_EQ(plan->output[0].name, "CAST('2020-01-02' AS \"DATE\")");
  EXPECT_EQ(plan->output[0].type, LogicalType::kDate);
  EXPECT_EQ(plan->output[1].name, "CAST('2024-01-31' AS \"DATE\")");
  const auto& project = std::get<ProjectNode>(Nth(*plan, 0));
  ASSERT_EQ(project.constants.size(), 2U);
  EXPECT_EQ(std::get<Int128>(project.constants[0].value_or(Constant{}).value), Int128{18263});
  EXPECT_EQ(std::get<Int128>(project.constants[1].value_or(Constant{}).value), Int128{19753});
}

// Stars among the select items (DuckDB's rules): `*` expands every FROM item, `t.*` the one FROM
// item of its own block named t -- which neither an enclosing block's item nor a second item of the
// name stands in for -- and every star's name binds before any item does.
INSTANTIATE_TEST_SUITE_P(
    StarItems, BindErrorTest,
    ::testing::Values(
        ErrorCase{"SELECT x.* FROM t", kBind, "x.*", "no FROM item is named 'x'"},
        ErrorCase{"SELECT t.* FROM t AS a", kBind, "t.*",
                  "no FROM item is named 't' (the alias 'a' hides it)"},
        // Never an enclosing block's FROM item, where a column would correlate (DuckDB:
        // "Referenced table not found").
        ErrorCase{"SELECT i16 FROM ok WHERE EXISTS (SELECT ok.* FROM u WHERE u.i16 = ok.i16)",
                  kBind, "ok.*", "no FROM item is named 'ok'"},
        ErrorCase{"SELECT ok.i16 FROM ok JOIN (SELECT ok.* FROM u) AS d ON ok.i16 = d.i16", kBind,
                  "ok.*", "no FROM item is named 'ok'"},
        ErrorCase{"SELECT i16 FROM ok WHERE i16 = (SELECT MAX(i16), ok.* FROM u)", kBind, "ok.*",
                  "no FROM item is named 'ok'"},
        // Two FROM items of the name, which share no column here (rule 5 would let `*` pass).
        ErrorCase{"SELECT x.* FROM ok AS x JOIN dec AS x ON x.i16 = x.s16", kBind, "x.*",
                  "'x.*' is ambiguous: two FROM items are named 'x'"},
        // No FROM item, but a column of the name: DuckDB expands a STRUCT's fields and rejects any
        // other type.
        ErrorCase{"SELECT rec.* FROM st", kUnsupported, "rec.*",
                  "'rec.*' over a STRUCT column is not supported"},
        ErrorCase{"SELECT bad.* FROM t", kBind, "bad.*", "no FROM item is named 'bad'"},
        ErrorCase{"SELECT i16.* FROM t", kBind, "i16.*", "no FROM item is named 'i16'"},
        ErrorCase{"SELECT rec.* FROM st AS a JOIN st AS b ON a.i16 = b.i16", kBind, "rec.*",
                  "no FROM item is named 'rec'"},
        // A star's name binds first, before an earlier item's name or unsupported type.
        ErrorCase{"SELECT nope, x.* FROM t", kBind, "x.*", "no FROM item is named 'x'"},
        ErrorCase{"SELECT bad, x.* FROM t", kBind, "x.*", "no FROM item is named 'x'"},
        ErrorCase{"SELECT i32, * FROM ok AS t, t", kBind, "*",
                  "'*' is ambiguous: two FROM items named 't' have a column 'i16'"},
        // A star reads its own FROM items' columns only: t's unsupported one, not ok's.
        ErrorCase{"SELECT ok.i16, t.* FROM ok JOIN t ON ok.i16 = t.i16", kUnsupported, "t.*",
                  "unsupported type"},
        // Aggregates without GROUP BY: the first plain column in select order, a star's
        // included, named as DuckDB names it (unqualified).
        ErrorCase{"SELECT i16, *, COUNT(*) FROM ok", kBind, "i16",
                  "column 'i16' must be inside an aggregate function"},
        ErrorCase{"SELECT COUNT(*), *, s FROM ok", kBind, "*",
                  "column 'i16' must be inside an aggregate function"},
        ErrorCase{"SELECT COUNT(*), ok.* FROM ok", kBind, "ok.*",
                  "column 'i16' must be inside an aggregate function"},
        ErrorCase{"SELECT t.i16, COUNT(*) FROM t", kBind, "t.i16",
                  "column 'i16' must be inside an aggregate function"},
        // GROUP BY: every column of a star must be a key.
        ErrorCase{"SELECT *, COUNT(*) FROM ok GROUP BY i16", kBind, "*",
                  "column 's' must appear in the GROUP BY clause"},
        // Positions count every column a star expands to.
        ErrorCase{"SELECT *, i16 FROM ok ORDER BY 4", kBind, "4",
                  "ORDER BY position 4 is not between 1 and 3"},
        // An EXISTS block checks its stars without expanding them: positions still count their
        // columns (u has 11), one inside a star is unsupported, and the other items bind.
        ErrorCase{"SELECT i16 FROM ok WHERE EXISTS (SELECT *, i32 FROM u WHERE u.i16 = ok.i16 "
                  "ORDER BY 1)",
                  kUnsupported, "1",
                  "ORDER BY position 1 over a '*' of an EXISTS subquery is not supported"},
        ErrorCase{"SELECT i16 FROM ok WHERE EXISTS (SELECT *, i32 FROM u WHERE u.i16 = ok.i16 "
                  "ORDER BY 99)",
                  kBind, "99", "ORDER BY position 99 is not between 1 and 12"},
        ErrorCase{"SELECT i16 FROM ok WHERE EXISTS (SELECT x.* FROM u WHERE u.i16 = ok.i16)", kBind,
                  "x.*", "no FROM item is named 'x'"},
        ErrorCase{"SELECT i16 FROM ok WHERE EXISTS (SELECT *, nope FROM u WHERE u.i16 = ok.i16)",
                  kBind, "nope", "column 'nope' does not exist"},
        // ORDER BY a name FROM has twice but one un-aliased column item carries as declared: DuckDB
        // orders by that item (its select-list step), which a later PR answers. A star's column and
        // a reference written as declared match in that case only.
        ErrorCase{"SELECT ok.*, t.i32 FROM ok JOIN t ON ok.i16 = t.i16 ORDER BY i16", kUnsupported,
                  "i16", "which only the select list makes unambiguous"},
        ErrorCase{"SELECT ok.*, t.i32 FROM ok JOIN t ON ok.i16 = t.i16 ORDER BY I16", kBind, "I16",
                  "is ambiguous"},
        ErrorCase{"SELECT a.*, b.* FROM ok AS a JOIN ok AS b ON a.i16 = b.i16 ORDER BY i16", kBind,
                  "i16", "is ambiguous"},
        // An aliased item carries only its alias, and a reference written as declared matches in
        // that case only: FROM's ambiguity stands, as in DuckDB.
        ErrorCase{"SELECT ok.i16 AS x, t.i32 FROM ok JOIN t ON ok.i16 = t.i16 ORDER BY i16", kBind,
                  "i16", "is ambiguous"},
        ErrorCase{"SELECT ok.i16, t.i32 FROM ok JOIN t ON ok.i16 = t.i16 ORDER BY I16", kBind,
                  "I16", "is ambiguous"},
        // An EXISTS block's star carries its columns' names too, unexpanded.
        ErrorCase{"SELECT i16 FROM ok WHERE EXISTS (SELECT u.*, t.d FROM u JOIN t ON u.i16 = t.i16 "
                  "WHERE u.i16 = ok.i16 ORDER BY i32)",
                  kUnsupported, "i32", "which only the select list makes unambiguous"},
        ErrorCase{
            "SELECT i16 FROM ok WHERE EXISTS (SELECT t.i32, u.* FROM u JOIN t ON u.i16 = t.i16 "
            "WHERE u.i16 = ok.i16 ORDER BY i32)",
            kBind, "i32", "is ambiguous"},
        // Positions in an EXISTS block over a star of two FROM items: u's 11 columns, then ok's 2.
        ErrorCase{
            "SELECT i16 FROM t WHERE EXISTS (SELECT *, u.i32 FROM u JOIN ok ON u.i16 = ok.i16 "
            "WHERE u.i16 = t.i16 ORDER BY 13)",
            kUnsupported, "13", "over a '*' of an EXISTS subquery"},
        ErrorCase{
            "SELECT i16 FROM t WHERE EXISTS (SELECT *, u.i32 FROM u JOIN ok ON u.i16 = ok.i16 "
            "WHERE u.i16 = t.i16 ORDER BY 15)",
            kBind, "15", "ORDER BY position 15 is not between 1 and 14"},
        // The aggregate-mixing error names a reference as written.
        ErrorCase{"SELECT I16, COUNT(*) FROM ok", kBind, "I16",
                  "column 'I16' must be inside an aggregate function"}));

// DuckDB's implicit ORDER BY alias: a plain column written otherwise than its column is declared is
// an alias of the declared name, found in any case, the last alias -- implicit or explicit --
// winning, where a FROM column would be ambiguous too.
TEST(BinderTest, OrderByFindsAColumnWrittenOtherwiseAsAnAlias) {
  const Catalog catalog = MakeCatalog();
  const auto key = [&catalog](std::string_view sql, int sort_at) {
    auto plan = BindSql(sql, catalog);
    EXPECT_TRUE(plan.ok()) << sql << ": " << plan.status().ToString();
    if (!plan.ok()) {
      return std::string();
    }
    const auto& sort = std::get<SortNode>(Nth(*plan, sort_at));
    EXPECT_EQ(sort.keys.size(), 1U) << sql;
    return sort.keys.empty() ? std::string() : sort.keys[0].column.name;
  };
  EXPECT_EQ(key("SELECT i32 AS i16, I16 FROM t ORDER BY i16", 1), "i16");
  EXPECT_EQ(key("SELECT I16, i32 AS i16 FROM t ORDER BY i16", 1), "i32");
  EXPECT_EQ(key("SELECT i32 AS i16, t.I16 FROM t ORDER BY I16", 1), "i16");
  // The alias decides before FROM's ambiguity, also next to a star's column of the name.
  EXPECT_EQ(key("SELECT ok.*, t.I16 FROM ok JOIN t ON ok.i16 = t.i16 ORDER BY i16", 1), "i16");
  auto plan = BindSql("SELECT ok.*, t.I16 FROM ok JOIN t ON ok.i16 = t.i16 ORDER BY i16", catalog);
  ASSERT_TRUE(plan.ok()) << plan.status().ToString();
  const auto& project = std::get<ProjectNode>(Nth(*plan, 0));
  const auto& sort = std::get<SortNode>(Nth(*plan, 1));
  ASSERT_EQ(project.columns.size(), 3U);
  ASSERT_EQ(sort.keys.size(), 1U);
  EXPECT_EQ(sort.keys[0].column.id, project.columns[2].id) << "t.i16, not ok.i16";
  EXPECT_NE(sort.keys[0].column.id, project.columns[0].id);
}

// A star expands where it stands among the other items -- `*` every FROM item's columns in FROM
// order, `t.*` its one FROM item's -- each under its declared name and read by its own id, so a
// column two stars read is the one column twice. A derived table's names are made unique after
// its alias list, as in DuckDB.
TEST(BinderTest, StarsExpandWhereTheyStand) {
  const Catalog catalog = MakeCatalog();
  using Names = std::vector<std::string>;
  const auto names = [&catalog](std::string_view sql) {
    auto plan = BindSql(sql, catalog);
    EXPECT_TRUE(plan.ok()) << sql << ": " << plan.status().ToString();
    Names out;
    if (plan.ok()) {
      for (const OutputColumn& column : plan->output) {
        out.push_back(column.name);
      }
    }
    return out;
  };
  EXPECT_EQ(names("SELECT i16 AS x, *, s FROM ok"), (Names{"x", "i16", "s", "s"}));
  EXPECT_EQ(names("SELECT *, * FROM ok"), (Names{"i16", "s", "i16", "s"}));
  EXPECT_EQ(names("SELECT a.*, 1, b.i16 FROM ok AS a JOIN ok AS b ON a.i16 = b.i16"),
            (Names{"i16", "s", "1", "i16"}));
  EXPECT_EQ(names("SELECT dec.p, ok.* FROM ok JOIN dec ON ok.i16 = dec.s16"),
            (Names{"p", "i16", "s"}));
  EXPECT_EQ(names(R"(SELECT "OK".*, T.i32 FROM ok JOIN t ON ok.i16 = t.i16)"),
            (Names{"i16", "s", "i32"}));
  EXPECT_EQ(names(R"(SELECT "over".* FROM ok AS "over")"), (Names{"i16", "s"}));
  EXPECT_EQ(names("SELECT bad.* FROM ok AS bad"), (Names{"i16", "s"}));  // an item, not a column
  EXPECT_EQ(names("SELECT * FROM (SELECT *, i16, * FROM ok) AS d"),
            (Names{"i16", "s", "i16_1", "i16_2", "s_1"}));
  EXPECT_EQ(names("SELECT * FROM (SELECT *, i16 FROM ok) AS d(x)"), (Names{"x", "s", "i16"}));
  EXPECT_EQ(names("SELECT i16_1 FROM (SELECT *, i16 FROM ok) AS d"), (Names{"i16_1"}));
  // In a grouped query a star's columns are keys like any other column's.
  EXPECT_EQ(names("SELECT *, COUNT(*) FROM ok GROUP BY i16, s"),
            (Names{"i16", "s", "count_star()"}));
  EXPECT_EQ(names("SELECT i16 AS x, *, COUNT(*) FROM ok GROUP BY 2, 3 ORDER BY 3 DESC"),
            (Names{"x", "i16", "s", "count_star()"}));

  constexpr std::string_view kSql = "SELECT *, i16, ok.* FROM ok";
  auto plan = BindSql(kSql, catalog);
  ASSERT_TRUE(plan.ok()) << plan.status().ToString();
  const auto& project = std::get<ProjectNode>(Nth(*plan, 0));
  ASSERT_EQ(project.columns.size(), 5U);
  EXPECT_EQ(project.columns[0].id, project.columns[2].id);
  EXPECT_EQ(project.columns[0].id, project.columns[3].id);
  EXPECT_EQ(project.columns[1].id, project.columns[4].id);
  EXPECT_NE(project.columns[0].id, project.columns[1].id);
  EXPECT_EQ(kSql.substr(project.span.offset, project.span.length), "*, i16, ok.*");
}

// A UNION ALL refusal points at the select item a column comes from, found by its place in the
// query: the star it expands from, or an item after a star.
TEST(BinderTest, UnionAllRefusalsPointAtTheItemAColumnComesFrom) {
  const Catalog catalog = MakeCatalog();
  const auto refused_at = [&catalog](std::string_view sql) {
    auto plan = BindSql(sql, catalog);
    EXPECT_FALSE(plan.ok()) << sql;
    const auto detail = GetSqlError(plan.status());
    EXPECT_NE(detail, nullptr) << sql;
    if (detail == nullptr) {
      return std::string_view::npos;
    }
    EXPECT_EQ(detail->kind(), kUnsupported) << plan.status().ToString();
    return static_cast<std::size_t>(detail->span().offset);
  };
  constexpr std::string_view kStar =
      "SELECT i16, i16, i16 FROM ok UNION ALL SELECT i16, ok.* FROM ok";
  EXPECT_EQ(refused_at(kStar), kStar.rfind("ok.*"));
  constexpr std::string_view kAfter = "SELECT i16, s, s FROM ok UNION ALL SELECT *, i16 FROM ok";
  EXPECT_EQ(refused_at(kAfter), kAfter.rfind("i16"));
}

// In an EXISTS block a star is checked, not expanded (ADR 0023): its FROM item's unsupported
// columns are not read, the other items bind, and a position after it skips its columns.
TEST(BinderTest, StarsOfAnExistsBlockAreCheckedNotExpanded) {
  const Catalog catalog = MakeCatalog();
  for (const std::string_view sql : {
           "SELECT i16 FROM ok WHERE EXISTS (SELECT u.*, u.i32 FROM u WHERE u.i16 = ok.i16)",
           "SELECT i16 FROM ok WHERE EXISTS (SELECT i32, * FROM u WHERE u.i16 = ok.i16 ORDER BY "
           "1)",
           "SELECT i16 FROM ok WHERE EXISTS (SELECT *, i32 FROM u WHERE u.i16 = ok.i16 ORDER BY "
           "12)",
       }) {
    auto plan = BindSql(sql, catalog);
    EXPECT_TRUE(plan.ok()) << sql << ": " << plan.status().ToString();
  }
}

TEST(BinderTest, SelectStarProjectsEveryColumn) {
  const Catalog catalog = MakeCatalog();
  constexpr std::string_view kSql = "SELECT * FROM OK LIMIT 5";
  auto plan = BindSql(kSql, catalog);
  ASSERT_TRUE(plan.ok()) << plan.status().ToString();
  ASSERT_EQ(plan->output.size(), 2U);
  EXPECT_EQ(plan->output[0].name, "i16");
  EXPECT_EQ(plan->output[0].type, LogicalType::kSmallInt);
  EXPECT_EQ(plan->output[1].name, "s");
  EXPECT_EQ(plan->output[1].type, LogicalType::kVarchar);
  const auto& limit = std::get<LimitNode>(Nth(*plan, 0));
  EXPECT_EQ(limit.limit, 5);
  EXPECT_EQ(kSql.substr(limit.span.offset, limit.span.length), "LIMIT 5");
  const auto& project = std::get<ProjectNode>(Nth(*plan, 1));
  EXPECT_EQ(kSql.substr(project.span.offset, project.span.length), "*");
  ASSERT_EQ(project.columns.size(), 2U);
  EXPECT_EQ(project.columns[0].index, 0);
  EXPECT_EQ(project.columns[1].index, 1);
  EXPECT_EQ(project.columns[1].name, "s");
  EXPECT_EQ(std::get<ScanNode>(Nth(*plan, 2)).table_name, "OK");
}

TEST(BinderTest, ColumnsResolveCaseInsensitivelyAndKeepTheirDeclaredNames) {
  const Catalog catalog = MakeCatalog();
  constexpr std::string_view kSql =
      R"(SELECT I16, "S", dT, "mixed case", "FROM", u16 AS "Alias", H x FROM t)";
  auto plan = BindSql(kSql, catalog);
  ASSERT_TRUE(plan.ok()) << plan.status().ToString();
  const std::vector<std::pair<std::string, LogicalType>> expected = {
      {"i16", LogicalType::kSmallInt},   {"s", LogicalType::kVarchar},
      {"dt", LogicalType::kDate},        {"Mixed Case", LogicalType::kInteger},
      {"from", LogicalType::kInteger},   {"Alias", LogicalType::kUSmallInt},
      {"x", LogicalType::Decimal(38, 0)}};
  ASSERT_EQ(plan->output.size(), expected.size());
  const auto& project = std::get<ProjectNode>(Nth(*plan, 0));
  EXPECT_EQ(kSql.substr(project.span.offset, project.span.length),
            R"(I16, "S", dT, "mixed case", "FROM", u16 AS "Alias", H x)");
  const std::vector<int> fields = {0, 6, 7, 9, 10, 3, 4};
  ASSERT_EQ(project.columns.size(), expected.size());
  for (std::size_t i = 0; i < expected.size(); ++i) {
    EXPECT_EQ(plan->output[i].name, expected[i].first) << i;
    EXPECT_EQ(plan->output[i].type, expected[i].second) << i;
    EXPECT_EQ(project.columns[i].index, fields[i]) << i;
    EXPECT_EQ(project.columns[i].type, expected[i].second) << i;
  }
  EXPECT_EQ(project.columns[3].name, "Mixed Case");
  EXPECT_TRUE(std::holds_alternative<ScanNode>(Nth(*plan, 1)));
}

TEST(BinderTest, AggregateResultTypes) {
  const Catalog catalog = MakeCatalog();
  const std::vector<std::pair<std::string_view, LogicalType>> cases = {
      {"COUNT(*)", LogicalType::kBigInt},
      {"COUNT(i16)", LogicalType::kBigInt},
      {"COUNT(s)", LogicalType::kBigInt},
      {"COUNT(dt)", LogicalType::kBigInt},
      {"COUNT(h)", LogicalType::kBigInt},
      {"SUM(i16)", LogicalType::kHugeInt},
      {"SUM(i32)", LogicalType::kHugeInt},
      {"SUM(i64)", LogicalType::kHugeInt},
      {"SUM(u16)", LogicalType::kHugeInt},
      {"SUM(d)", LogicalType::kDouble},
      {"AVG(i16)", LogicalType::kDouble},
      {"AVG(i64)", LogicalType::kDouble},
      {"AVG(u16)", LogicalType::kDouble},
      {"AVG(d)", LogicalType::kDouble},
      {"MIN(i16)", LogicalType::kSmallInt},
      {"MAX(i32)", LogicalType::kInteger},
      {"MIN(i64)", LogicalType::kBigInt},
      {"MAX(u16)", LogicalType::kUSmallInt},
      {"MAX(d)", LogicalType::kDouble},
      {"MIN(s)", LogicalType::kVarchar},
      {"MAX(dt)", LogicalType::kDate},
      // MIN and MAX keep a DECIMAL's precision and scale; COUNT DISTINCT is BIGINT.
      {"MIN(h)", LogicalType::Decimal(38, 0)},
      {"COUNT(DISTINCT h)", LogicalType::kBigInt},
  };
  for (const auto& [call, type] : cases) {
    const std::string sql = "SELECT " + std::string(call) + " FROM t";
    auto plan = BindSql(sql, catalog);
    ASSERT_TRUE(plan.ok()) << sql << ": " << plan.status().ToString();
    EXPECT_EQ(plan->output.at(0).type, type) << sql;
    const auto& agg = std::get<AggregateNode>(Nth(*plan, 0));
    EXPECT_EQ(agg.aggregates.at(0).type, type) << sql;
  }
}

TEST(BinderTest, AggregateCallsAndDuckDbResultNames) {
  const Catalog catalog = MakeCatalog();
  constexpr std::string_view kSql =
      R"(SELECT count(*), COUNT(I16), Sum(i32), avg("i64"), MIN("Mixed Case"), MAX("FROM"),
                COUNT(*) AS n, SUM(d) total, max(dt) AS "Last Day" FROM t)";
  auto plan = BindSql(kSql, catalog);
  ASSERT_TRUE(plan.ok()) << plan.status().ToString();
  const std::vector<std::string> names = {
      "count_star()",   "count(I16)", "sum(i32)", "avg(i64)", R"(min("Mixed Case"))",
      R"(max("FROM"))", "n",          "total",    "Last Day"};
  ASSERT_EQ(plan->output.size(), names.size());
  for (std::size_t i = 0; i < names.size(); ++i) {
    EXPECT_EQ(plan->output[i].name, names[i]) << i;
  }
  const auto& agg = std::get<AggregateNode>(Nth(*plan, 0));
  const std::vector<AggKind> kinds = {AggKind::kCountStar, AggKind::kCount, AggKind::kSum,
                                      AggKind::kAvg,       AggKind::kMin,   AggKind::kMax,
                                      AggKind::kCountStar, AggKind::kSum,   AggKind::kMax};
  const std::vector<std::optional<int>> args = {std::nullopt, 0, 1, 2, 9, 10, std::nullopt, 5, 7};
  ASSERT_EQ(agg.aggregates.size(), kinds.size());
  for (std::size_t i = 0; i < kinds.size(); ++i) {
    EXPECT_EQ(agg.aggregates[i].kind, kinds[i]) << i;
    EXPECT_EQ(agg.aggregates[i].arg.has_value(), args[i].has_value()) << i;
    EXPECT_EQ(agg.aggregates[i].arg.value_or(BoundColumn{.index = -1}).index, args[i].value_or(-1))
        << i;
  }
  const SourceSpan second = agg.aggregates[1].span;
  EXPECT_EQ(kSql.substr(second.offset, second.length), "COUNT(I16)");
  EXPECT_EQ(agg.aggregates[4].arg.value_or(BoundColumn{}).name, "Mixed Case");
}

// A function call's result name, as DuckDB re-serializes it: the name is quoted only where the
// identifier needs quoting, so `strlen` is plain however it was written, while `substring` -- which
// DuckDB reserves -- is schema-qualified and quoted when the call was unquoted and quoted alone
// when it was quoted, in either case lower-cased. The positions of substring are arguments like any
// other, so they appear as written.
TEST(BinderTest, FunctionCallDuckDbResultNames) {
  const Catalog catalog = MakeCatalog();
  constexpr std::string_view kSql =
      R"(SELECT strlen(s), "strlen"(s), "STRLEN"(s), STRLEN(s), substring(s, 1, 2),
                "substring"(s, 1, 2), "SUBSTRING"(s, 1, 2), SUBSTRING(s FROM 1 FOR 2),
                substr(s, 1, 2), "substr"(s, i32), substring(s, i32 + 1) FROM t)";
  auto plan = BindSql(kSql, catalog);
  ASSERT_TRUE(plan.ok()) << plan.status().ToString();
  const std::vector<std::string> names = {
      "strlen(s)",
      "strlen(s)",
      "strlen(s)",
      "strlen(s)",
      R"(main."substring"(s, 1, 2))",
      R"("substring"(s, 1, 2))",
      R"("substring"(s, 1, 2))",
      R"(main."substring"(s, 1, 2))",
      "substr(s, 1, 2)",
      "substr(s, i32)",
      R"(main."substring"(s, (i32 + 1)))",
  };
  ASSERT_EQ(plan->output.size(), names.size());
  for (std::size_t i = 0; i < names.size(); ++i) {
    EXPECT_EQ(plan->output[i].name, names[i]) << i;
    EXPECT_EQ(plan->output[i].type, i < 4 ? LogicalType::kBigInt : LogicalType::kVarchar) << i;
  }
}

TEST(BinderTest, WhereBuildsAFilterUnderTheSelectList) {
  const Catalog catalog = MakeCatalog();
  constexpr std::string_view kSql =
      "SELECT i16, s FROM t WHERE i32 >= -7 AND 'abc' < s AND dt = DATE '2022-01-08' "
      "AND d <> 0.5 AND h < 12 LIMIT 0";
  auto plan = BindSql(kSql, catalog);
  ASSERT_TRUE(plan.ok()) << plan.status().ToString();
  EXPECT_EQ(std::get<LimitNode>(Nth(*plan, 0)).limit, 0);
  EXPECT_TRUE(std::holds_alternative<ProjectNode>(Nth(*plan, 1)));
  const auto& filter = std::get<FilterNode>(Nth(*plan, 2));
  EXPECT_EQ(kSql.substr(filter.span.offset, filter.span.length),
            "i32 >= -7 AND 'abc' < s AND dt = DATE '2022-01-08' AND d <> 0.5 AND h < 12");
  ASSERT_EQ(filter.predicates.size(), 5U);
  const auto check = [&](std::size_t i, int index, CompareOp op, LogicalType type,
                         std::string_view span) {
    const Predicate& p = filter.predicates[i];
    EXPECT_EQ(p.kind, Predicate::Kind::kCompare) << i;
    EXPECT_EQ(p.column.value_or(BoundColumn{.index = -1}).index, index) << i;
    EXPECT_EQ(p.column.value_or(BoundColumn{}).type, type) << i;
    EXPECT_EQ(p.op, op) << i;
    EXPECT_EQ(p.constant.type, type) << i;
    EXPECT_EQ(kSql.substr(p.span.offset, p.span.length), span) << i;
  };
  check(0, 1, CompareOp::kGe, LogicalType::kInteger, "i32 >= -7");
  check(1, 6, CompareOp::kGt, LogicalType::kVarchar, "'abc' < s");  // literal first, mirrored
  check(2, 7, CompareOp::kEq, LogicalType::kDate, "dt = DATE '2022-01-08'");
  check(3, 5, CompareOp::kNe, LogicalType::kDouble, "d <> 0.5");
  check(4, 4, CompareOp::kLt, LogicalType::Decimal(38, 0), "h < 12");
  EXPECT_EQ(std::get<Int128>(filter.predicates[0].constant.value), -7);
  EXPECT_EQ(std::get<std::string>(filter.predicates[1].constant.value), "abc");
  EXPECT_EQ(std::get<Int128>(filter.predicates[2].constant.value), 19000);
  EXPECT_EQ(std::get<double>(filter.predicates[3].constant.value), 0.5);
  EXPECT_EQ(std::get<Int128>(filter.predicates[4].constant.value), 12);
  EXPECT_TRUE(std::holds_alternative<ScanNode>(Nth(*plan, 3)));
}

TEST(BinderTest, EveryComparisonOperator) {
  const Catalog catalog = MakeCatalog();
  const std::vector<std::pair<std::string_view, CompareOp>> ops = {
      {"=", CompareOp::kEq},  {"<>", CompareOp::kNe}, {"!=", CompareOp::kNe}, {"<", CompareOp::kLt},
      {"<=", CompareOp::kLe}, {">", CompareOp::kGt},  {">=", CompareOp::kGe}};
  for (const auto& [text, op] : ops) {
    const std::string sql = "SELECT COUNT(*) FROM t WHERE i64 " + std::string(text) + " 5";
    auto plan = BindSql(sql, catalog);
    ASSERT_TRUE(plan.ok()) << sql << ": " << plan.status().ToString();
    const auto& filter = std::get<FilterNode>(Nth(*plan, 1));
    EXPECT_EQ(filter.predicates.at(0).op, op) << sql;
  }
}

// String and DATE literals against VARCHAR and DATE columns; numbers against DOUBLE (rounded to
// the nearest double, as in DuckDB).
TEST(BinderTest, NonIntegerConstants) {
  const Catalog catalog = MakeCatalog();
  const auto constant = [&](std::string_view where) -> Constant {
    const std::string sql = "SELECT COUNT(*) FROM t WHERE " + std::string(where);
    auto plan = BindSql(sql, catalog);
    EXPECT_TRUE(plan.ok()) << sql << ": " << plan.status().ToString();
    if (!plan.ok()) {
      return {};
    }
    const auto& p = std::get<FilterNode>(Nth(*plan, 1)).predicates.at(0);
    EXPECT_EQ(p.kind, Predicate::Kind::kCompare) << sql;
    return p.constant;
  };
  const auto days = [&](std::string_view where) { return std::get<Int128>(constant(where).value); };
  EXPECT_EQ(days("dt = '1970-01-01'"), 0);
  EXPECT_EQ(days("dt = DATE '1969-12-31'"), -1);
  EXPECT_EQ(days("dt >= '2000-02-29'"), 11016);
  EXPECT_EQ(days("dt < DATE '0000-01-01'"), -719528);
  EXPECT_EQ(days("dt <= '9999-12-31'"), 2932896);
  const auto bytes = [&](std::string_view where) {
    return std::get<std::string>(constant(where).value);
  };
  EXPECT_EQ(bytes("s = ''"), "");
  EXPECT_EQ(bytes("s = 'O''Brien'"), "O'Brien");
  EXPECT_EQ(bytes("s < '2013-07-01'"), "2013-07-01");
  const auto real = [&](std::string_view where) { return std::get<double>(constant(where).value); };
  EXPECT_EQ(real("d = 1.5"), 1.5);
  EXPECT_EQ(real("d = -0.25"), -0.25);
  EXPECT_EQ(real("d > 7"), 7.0);
  EXPECT_EQ(real("d > 1e3"), 1000.0);
  EXPECT_EQ(real("d < 9007199254740993"), 9007199254740992.0);
  EXPECT_EQ(real("d < 0.1"), 0.1);
  // A decimal literal is DuckDB's DECIMAL, converted as DuckDB converts one (ADR 0021 rule 8),
  // which is not always the nearest double (that is 9007199254740994 here): div + mod / 10^scale
  // beyond 2^53, at most 18 digits and beyond (values from DuckDB 1.5.5).
  EXPECT_EQ(real("d = 9007199254740993.5"), 9007199254740992.0);
  EXPECT_EQ(real("d < 19948972279101698.2"), 1.9948972279101696e+16);
  EXPECT_EQ(real("d >= 60719098953061412.540"), 6.071909895306141e+16);
  EXPECT_EQ(real("d < 1e400"), std::numeric_limits<double>::infinity());
  EXPECT_EQ(real("d > -1e400"), -std::numeric_limits<double>::infinity());
  EXPECT_EQ(real("d > 1e-400"), 0.0);
  EXPECT_TRUE(std::signbit(real("d > -1e-400")));
}

TEST(BinderTest, FoldedComparisonsKeepNullRejection) {
  const Catalog catalog = MakeCatalog();
  constexpr std::string_view kSql =
      "SELECT COUNT(*) FROM t WHERE i16 < 40000 AND u16 = -1 AND i64 > 1.5 AND i32 <> 2.5";
  auto plan = BindSql(kSql, catalog);
  ASSERT_TRUE(plan.ok()) << plan.status().ToString();
  const auto& predicates = std::get<FilterNode>(Nth(*plan, 1)).predicates;
  ASSERT_EQ(predicates.size(), 4U);
  // True for every value, but a NULL still rejects the row.
  EXPECT_EQ(predicates[0].kind, Predicate::Kind::kIsNotNull);
  EXPECT_EQ(predicates[0].column.value_or(BoundColumn{}).name, "i16");
  // Never true: the column is not even needed.
  EXPECT_EQ(predicates[1].kind, Predicate::Kind::kFalse);
  EXPECT_FALSE(predicates[1].column.has_value());
  EXPECT_EQ(kSql.substr(predicates[1].span.offset, predicates[1].span.length), "u16 = -1");
  // c > 1.5 is c >= 2.
  EXPECT_EQ(predicates[2].kind, Predicate::Kind::kCompare);
  EXPECT_EQ(predicates[2].op, CompareOp::kGe);
  EXPECT_EQ(std::get<Int128>(predicates[2].constant.value), 2);
  EXPECT_EQ(predicates[2].constant.type, LogicalType::kBigInt);
  // c <> 2.5 holds for every value.
  EXPECT_EQ(predicates[3].kind, Predicate::Kind::kIsNotNull);
}

TEST(BinderTest, NegativeLimitIsABindError) {
  // The parser never produces one; the binder still checks the AST it is given.
  const Catalog catalog = MakeCatalog();
  constexpr std::string_view kSql = "SELECT i16 FROM t LIMIT 3";
  auto stmt = sql::Parse(kSql);
  ASSERT_TRUE(stmt.has_value());
  stmt->limit = -1;
  auto plan = Bind(*stmt, catalog);
  ASSERT_FALSE(plan.ok());
  const auto detail = GetSqlError(plan.status());
  ASSERT_NE(detail, nullptr);
  EXPECT_EQ(detail->kind(), SqlErrorDetail::Kind::kBind);
  EXPECT_EQ(SpanText(kSql, plan.status()), "LIMIT 3");
}

TEST(BinderTest, LargestLimit) {
  const Catalog catalog = MakeCatalog();
  auto plan = BindSql("SELECT COUNT(*) FROM t LIMIT 9223372036854775807", catalog);
  ASSERT_TRUE(plan.ok()) << plan.status().ToString();
  EXPECT_EQ(std::get<LimitNode>(Nth(*plan, 0)).limit, std::numeric_limits<int64_t>::max());
}

TEST(BinderTest, EmptySelectListIsABindError) {
  const Catalog catalog = MakeCatalog();
  auto stmt = sql::Parse("SELECT i16 FROM t");
  ASSERT_TRUE(stmt.has_value());
  stmt->items.clear();
  auto plan = Bind(*stmt, catalog);
  ASSERT_FALSE(plan.ok());
  const auto detail = GetSqlError(plan.status());
  ASSERT_NE(detail, nullptr);
  EXPECT_EQ(detail->kind(), SqlErrorDetail::Kind::kBind);
}

TEST(BinderTest, UnknownRowCountStillBinds) {
  const Catalog catalog = MakeCatalog();
  auto plan = BindSql("SELECT COUNT(*) FROM u", catalog);
  ASSERT_TRUE(plan.ok()) << plan.status().ToString();
  EXPECT_TRUE(std::holds_alternative<AggregateNode>(Nth(*plan, 0)));
}

TEST(BinderTest, PathsOpenThroughTheCatalog) {
  std::vector<std::string> opened;
  const Catalog catalog(
      [&opened](const std::string& path) -> arrow::Result<std::shared_ptr<Table>> {
        opened.push_back(path);
        if (path == "missing.parquet") {
          return arrow::Status::IOError("no such file");
        }
        return std::make_shared<FakeTable>(testing::AllTypesSchema(), 1);
      });
  auto plan = BindSql("SELECT MAX(i64) FROM 'data/it''s.parquet'", catalog);
  ASSERT_TRUE(plan.ok()) << plan.status().ToString();
  EXPECT_EQ(std::get<ScanNode>(Nth(*plan, 1)).table_name, "data/it's.parquet");
  // I/O errors keep their code: they are not SQL errors.
  auto missing = BindSql("SELECT COUNT(*) FROM 'missing.parquet'", catalog);
  EXPECT_TRUE(missing.status().IsIOError());
  EXPECT_EQ(GetSqlError(missing.status()), nullptr);
  EXPECT_EQ(opened, (std::vector<std::string>{"data/it's.parquet", "missing.parquet"}));
}

TEST(BinderTest, DuplicateRegistrationFails) {
  Catalog catalog;
  const auto table = std::make_shared<FakeTable>(testing::AllTypesSchema(), 1);
  ASSERT_TRUE(catalog.Register("t", table).ok());
  EXPECT_TRUE(catalog.Register("T", table).IsAlreadyExists());
  EXPECT_TRUE(catalog.Register("x", nullptr).IsInvalid());
}

// The double constants of the comparisons of every Filter, from the root down.
std::vector<double> FilterConstants(const LogicalPlan& plan) {
  std::vector<double> out;
  for (const LogicalNode* node = plan.root.get(); node != nullptr;) {
    if (const auto* filter = std::get_if<FilterNode>(node)) {
      for (const Predicate& p : filter->predicates) {
        if (const auto* value = std::get_if<double>(&p.constant.value)) {
          out.push_back(*value);
        }
      }
    }
    const std::vector<LogicalNodePtr> inputs = InputsOf(*node);
    node = inputs.empty() ? nullptr : inputs[0].get();
  }
  return out;
}

// A FLOAT column (read as DOUBLE) compares with a number as DuckDB compares a FLOAT, and DuckDB's
// MIN and MAX of it are FLOAT (divergence D11). The binder finds a column's field by the column's
// id, also for a GROUP BY key (whose output column is no field) and an alias. With f at field 0, a
// position read as a field number would make the other columns FLOAT too.
TEST(BinderTest, FloatColumnsByColumnId) {
  Catalog catalog;
  ASSERT_TRUE(catalog
                  .Register("tf", std::make_shared<FakeTable>(
                                      arrow::schema({arrow::field("f", arrow::float32()),
                                                     arrow::field("d", arrow::float64()),
                                                     arrow::field("k", arrow::int32())}),
                                      10, std::vector<int>{0}))
                  .ok());
  const auto as_float = static_cast<double>(0.1F);
  ASSERT_NE(as_float, 0.1);
  for (const auto& [sql, constants] : {
           std::pair{"SELECT COUNT(*) FROM tf WHERE f = 0.1 AND d = 0.1",
                     std::vector<double>{as_float, 0.1}},
           std::pair{"SELECT MIN(f), MAX(d) FROM tf HAVING MIN(f) = 0.1 AND MAX(d) = 0.1",
                     std::vector<double>{as_float, 0.1}},
           std::pair{"SELECT MAX(f) AS m, MIN(d) AS n FROM tf HAVING m = 0.1 AND n = 0.1",
                     std::vector<double>{as_float, 0.1}},
           std::pair{"SELECT f, d, COUNT(*) FROM tf GROUP BY f, d HAVING f = 0.1 AND d = 0.1",
                     std::vector<double>{as_float, 0.1}},
           std::pair{"SELECT f AS g, k, COUNT(*) FROM tf GROUP BY f, k HAVING g = 0.1",
                     std::vector<double>{as_float}},
           std::pair{"SELECT MIN(d + 1), SUM(f) FROM tf HAVING MIN(d + 1) = 0.1 AND SUM(f) = 0.1",
                     std::vector<double>{0.1, 0.1}},
       }) {
    auto plan = BindSql(sql, catalog);
    ASSERT_TRUE(plan.ok()) << sql << ": " << plan.status().ToString();
    EXPECT_EQ(FilterConstants(*plan), constants) << sql;
  }
  // Without a DOUBLE value, DuckDB compares a FLOAT BETWEEN in FLOAT as antb1 does.
  auto between = BindSql("SELECT COUNT(*) FROM tf WHERE f BETWEEN 0.1 AND 0.1", catalog);
  ASSERT_TRUE(between.ok()) << between.status().ToString();
  EXPECT_EQ(FilterConstants(*between), (std::vector<double>{as_float, as_float}));
  // With one, DuckDB compares all three in DOUBLE: unsupported.
  for (const char* sql : {"SELECT f + 1 FROM tf", "SELECT -f FROM tf",
                          "SELECT COUNT(*) FROM tf WHERE f BETWEEN 0.7 AND 1e0",
                          "SELECT COUNT(*) FROM tf WHERE f BETWEEN 16777217 AND d",
                          "SELECT COUNT(*) FROM tf WHERE f NOT BETWEEN 1 AND 1e10"}) {
    const auto plan = BindSql(sql, catalog);
    ASSERT_FALSE(plan.ok()) << sql;
    const auto detail = GetSqlError(plan.status());
    ASSERT_NE(detail, nullptr) << sql;
    EXPECT_EQ(detail->kind(), SqlErrorDetail::Kind::kUnsupported) << sql;
  }
}

// A UNION ALL's FLOAT flag is the AND over its blocks (ADR 0036): FLOAT and FLOAT values stay
// FLOAT, whose literals fold at FLOAT precision (divergence D11), while FLOAT and DOUBLE values
// make a DOUBLE in DuckDB, whose literals do not; a FLOAT next to an integer or a DECIMAL is
// refused, as DuckDB would make a FLOAT of them.
TEST(BinderTest, UnionAllFloatFlagIsTheAndOverItsBlocks) {
  Catalog catalog;
  ASSERT_TRUE(catalog
                  .Register("tf", std::make_shared<FakeTable>(
                                      arrow::schema({arrow::field("f", arrow::float32()),
                                                     arrow::field("d", arrow::float64()),
                                                     arrow::field("k", arrow::int32())}),
                                      10, std::vector<int>{0}))
                  .ok());
  const auto as_float = static_cast<double>(0.1F);
  for (const auto& [sql, constant] : {
           std::pair{"SELECT COUNT(*) FROM (SELECT f AS x FROM tf UNION ALL SELECT f FROM tf) AS u "
                     "WHERE x = 0.1",
                     as_float},
           std::pair{"SELECT COUNT(*) FROM (SELECT f AS x FROM tf UNION ALL SELECT d FROM tf) AS u "
                     "WHERE x = 0.1",
                     0.1},
           std::pair{"SELECT COUNT(*) FROM (SELECT d AS x FROM tf UNION ALL SELECT f FROM tf) AS u "
                     "WHERE x = 0.1",
                     0.1},
           // Through a nested UNION (ADR 0039's X4b): the AND over every leaf, a NULL item none.
           std::pair{
               "SELECT COUNT(*) FROM (SELECT f AS x FROM tf UNION ALL (SELECT f FROM tf UNION "
               "SELECT NULL FROM tf)) AS u WHERE x = 0.1",
               as_float},
           std::pair{
               "SELECT COUNT(*) FROM (SELECT f AS x FROM tf UNION ALL (SELECT f FROM tf UNION "
               "SELECT d FROM tf)) AS u WHERE x = 0.1",
               0.1},
       }) {
    auto plan = BindSql(sql, catalog);
    ASSERT_TRUE(plan.ok()) << sql << ": " << plan.status().ToString();
    EXPECT_EQ(FilterConstants(*plan), std::vector<double>{constant}) << sql;
  }
  const std::string sql = "SELECT f FROM tf UNION ALL SELECT k FROM tf";
  auto refused = BindSql(sql, catalog);
  ASSERT_FALSE(refused.ok());
  EXPECT_TRUE(refused.status().IsNotImplemented()) << refused.status().ToString();
  EXPECT_TRUE(refused.status().message().contains(
      "UNION ALL of FLOAT and INTEGER values is not supported (antb1 reads FLOAT as DOUBLE, "
      "divergence D11)"))
      << refused.status().message();
  EXPECT_EQ(SpanText(sql, refused.status()), "k");
}

// ORDER BY a select item by name, alias or position: a column already ordered by is no key again,
// whichever way it is named; a computed item is its computed column.
TEST(BinderTest, OrderByComputedItemsByColumnId) {
  const Catalog catalog = MakeCatalog();
  auto plan = BindSql("SELECT i16 + 1 AS x, i16 FROM t ORDER BY i16, x, 1, 2, i16 + 1", catalog);
  ASSERT_TRUE(plan.ok()) << plan.status().ToString();
  const auto& sort = std::get<SortNode>(Nth(*plan, 1));
  ASSERT_EQ(sort.keys.size(), 2U);
  EXPECT_EQ(sort.keys[0].column.name, "i16");
  EXPECT_EQ(sort.keys[0].column.index, 0);  // the Scan's field i16
  const auto& compute = std::get<ComputeNode>(Nth(*plan, 2));
  ASSERT_EQ(compute.ids.size(), 1U);
  EXPECT_EQ(sort.keys[1].column.id, compute.ids[0]);
  EXPECT_EQ(sort.keys[1].column.name, "x");  // a computed column, not field 0
  EXPECT_EQ(sort.keys[1].column.index, 11);  // after the 11 fields
}

// Aggregates with the same kind are the same call when their arguments are the same column: MAX of
// a field and MAX of an expression of it are two calls, each reused by ORDER BY.
TEST(BinderTest, AggregatesOfAFieldAndOfAnExpressionDiffer) {
  const Catalog catalog = MakeCatalog();
  auto plan =
      BindSql("SELECT s, MAX(i16), MAX(i16 + 1) FROM t GROUP BY s ORDER BY MAX(i16 + 1), MAX(i16)",
              catalog);
  ASSERT_TRUE(plan.ok()) << plan.status().ToString();
  const auto& sort = std::get<SortNode>(Nth(*plan, 1));
  const auto& group = std::get<GroupAggregateNode>(Nth(*plan, 2));
  ASSERT_EQ(group.aggregates.size(), 2U);
  ASSERT_EQ(sort.keys.size(), 2U);
  EXPECT_EQ(sort.keys[0].column.id, group.aggregates[1].id);
  EXPECT_EQ(sort.keys[0].column.index, 2);
  EXPECT_EQ(sort.keys[1].column.id, group.aggregates[0].id);
  EXPECT_EQ(sort.keys[1].column.index, 1);
}

// In a HAVING condition with OR, the alias of an item computed over the aggregation is the item's
// expression, computed again inside the condition: HAVING's Compute stands below the select list's.
TEST(BinderTest, HavingAliasOfAComputedItemInsideOr) {
  const Catalog catalog = MakeCatalog();
  auto plan = BindSql(
      "SELECT i16, CASE WHEN COUNT(*) > 1 THEN 'many' END AS size FROM t GROUP BY i16 "
      "HAVING size = 'many' OR size = 'few'",
      catalog);
  ASSERT_TRUE(plan.ok()) << plan.status().ToString();
  const auto& select = std::get<ComputeNode>(Nth(*plan, 1));
  ASSERT_EQ(select.exprs.size(), 1U);  // the CASE item
  EXPECT_TRUE(std::holds_alternative<CaseExpr>(select.exprs[0]->node));
  EXPECT_TRUE(std::holds_alternative<FilterNode>(Nth(*plan, 2)));
  const auto& compute = std::get<ComputeNode>(Nth(*plan, 3));
  ASSERT_EQ(compute.exprs.size(), 1U);  // the condition
  const auto& condition = std::get<BoolExpr>(compute.exprs[0]->node);
  ASSERT_EQ(condition.args.size(), 2U);
  for (const ExprPtr& arg : condition.args) {
    const auto& predicate = std::get<PredicateExpr>(arg->node);
    ASSERT_EQ(predicate.operands.size(), 1U);
    EXPECT_TRUE(std::holds_alternative<CaseExpr>(predicate.operands[0]->node));
  }
}

// HAVING computes what its conditions read and filters the groups before the select list and ORDER
// BY are computed, as DuckDB does: their expressions run only over the groups that pass. The alias
// of a computed item is its expression there, in a comparison, a compound condition and a global
// aggregate's HAVING alike.
TEST(BinderTest, HavingFiltersBeforeTheSelectListIsComputed) {
  const Catalog catalog = MakeCatalog();
  const auto optimized = [&catalog](std::string_view sql) -> std::string {
    auto plan = BindSql(sql, catalog);
    EXPECT_TRUE(plan.ok()) << sql << ": " << plan.status().ToString();
    if (!plan.ok()) {
      return "";
    }
    EXPECT_EQ(PositionMismatch(*plan), std::nullopt) << sql << "\n" << Explain(*plan);
    return Explain(Optimized(*plan));
  };
  EXPECT_EQ(optimized("SELECT i32, SUM(i16) + 1 AS s FROM t GROUP BY i32 HAVING s > 5 ORDER BY s"),
            "Output: i32:INTEGER s:HUGEINT\n"
            "Project i32, s\n"
            "  Sort s ASC NULLS LAST\n"
            "    Compute (sum(i16) + 1)\n"
            "      Filter \"sum(i16)\" > 4\n"
            "        GroupAggregate keys=[i32] SUM(i16)\n"
            "          Scan table=t source=fake columns=[i16, i32]\n");
  EXPECT_EQ(
      optimized("SELECT i32, SUM(i16) + 1 AS s FROM t GROUP BY i32 HAVING s > 5 OR i32 * 2 < 0"),
      "Output: i32:INTEGER s:HUGEINT\n"
      "Project i32, s\n"
      "  Compute (sum(i16) + 1)\n"
      "    Filter \"((s > 5) OR ((i32 * 2) < 0))\"\n"
      "      Compute ((s > 5) OR ((i32 * 2) < 0))\n"
      "        GroupAggregate keys=[i32] SUM(i16)\n"
      "          Scan table=t source=fake columns=[i16, i32]\n");
  EXPECT_EQ(optimized("SELECT MAX(i32) * 2 FROM t HAVING MAX(i32) < 0"),
            "Output: (max(i32) * 2):INTEGER\n"
            "Project \"(max(i32) * 2)\"\n"
            "  Compute (max(i32) * 2)\n"
            "    Filter \"max(i32)\" < 0\n"
            "      Aggregate MAX(i32)\n"
            "        Scan table=t source=fake columns=[i32]\n");
}

// Every column a bound plan creates has its own id, and Bind resolves every position from the ids
// (ADR 0022).
TEST(BinderTest, ColumnIdsAreUniqueAndResolve) {
  const auto catalog = MakeCatalog();
  constexpr const char* kGrouped =
      "SELECT i32, i32 + 1, SUM(i32 + 1) FROM t GROUP BY i32, i32 + 1 HAVING SUM(i32 + 1) > 2 "
      "ORDER BY 2";
  for (const char* sql :
       {"SELECT * FROM ok",
        "SELECT i16 + 1, s FROM t WHERE i32 // 2 > 0 AND (i16 > 1 OR NOT s = 'x')", kGrouped,
        "SELECT SUM(i32 + 1), COUNT(*) FROM t HAVING SUM(i32 + 1) > 0",
        "SELECT 1, 'a', i16 FROM t ORDER BY i16 LIMIT 2"}) {
    auto plan = BindSql(sql, catalog);
    ASSERT_TRUE(plan.ok()) << sql << ": " << plan.status().ToString();
    EXPECT_EQ(PositionMismatch(*plan), std::nullopt) << sql;
    std::vector<ColumnId> output;
    for (const OutputColumn& column : plan->output) {
      output.push_back(column.id);
    }
    EXPECT_EQ(output, OutputIds(*plan->root)) << sql;
  }

  // A column the select list reads twice gives two output columns; a constant reads no column.
  auto twice = BindSql("SELECT s, i16, s, 1 FROM t", catalog);
  ASSERT_TRUE(twice.ok());
  const auto& project = std::get<ProjectNode>(*twice->root);
  EXPECT_EQ(project.columns.at(0).id, project.columns.at(2).id);
  EXPECT_EQ(project.columns.at(3).id, kNoColumnId);
  EXPECT_EQ(std::set<ColumnId>(project.ids.begin(), project.ids.end()).size(), 4U);

  // A GROUP BY key's output is a new column (one value per group), which the select list reads.
  auto grouped = BindSql("SELECT i32, COUNT(*) FROM t GROUP BY i32", catalog);
  ASSERT_TRUE(grouped.ok());
  const auto& group = std::get<GroupAggregateNode>(Nth(*grouped, 1));
  ASSERT_EQ(group.key_ids.size(), 1U);
  EXPECT_NE(group.key_ids[0], group.keys.at(0).id);
  EXPECT_EQ(std::get<ProjectNode>(*grouped->root).columns.at(0).id, group.key_ids[0]);
}

// ADR 0023's Path 1: inside EXISTS a `*` is checked but not expanded, so a table with a column of
// a type antb1 cannot read still works there, as it does in DuckDB. The same `*` outside EXISTS is
// exit 4 at the star, which the bind-error cases pin.
TEST(BinderTest, AStarInsideExistsIsCheckedButNotExpanded) {
  const auto catalog = testing::MakeCatalog();
  // `t` has `bad`, a list<int32> column with no engine type.
  auto plan =
      BindSql("SELECT i16 FROM u WHERE EXISTS (SELECT * FROM t WHERE t.i64 = u.i64)", catalog);
  ASSERT_TRUE(plan.ok()) << plan.status().ToString();
  EXPECT_EQ(PositionMismatch(*plan), std::nullopt) << Explain(*plan);
  // Rule 5 of ADR 0022 still applies to it, before any expansion: two bindings of one name that
  // share a column name make the star a bind error inside EXISTS too.
  auto conflict = BindSql(
      "SELECT i16 FROM u WHERE EXISTS (SELECT * FROM t, t AS t WHERE t.i64 = u.i64)", catalog);
  EXPECT_FALSE(conflict.ok());
}

// An uncorrelated scalar subquery of any shape is its single value (ADR 0041's decision 4): the
// one-row join's right input is a SINGLE_VALUE aggregate over the subquery's plan, of the
// subquery's column's type, which exec makes NULL over no row and an error over two. A plan that is
// exactly one row, an aggregate over every row under Projects and Computes (a CTE's too), gets
// none, so every scalar subquery answered before keeps its plan.
TEST(BinderTest, AnUncorrelatedScalarSubqueryOfAnyShapeIsItsSingleValue) {
  const Catalog catalog = MakeCatalog();
  // The right input of the plan's outermost join, which must be a one-row join.
  const auto one_row_right = [&catalog](std::string_view sql) -> LogicalNodePtr {
    auto plan = BindSql(sql, catalog);
    EXPECT_TRUE(plan.ok()) << sql << ": " << plan.status().ToString();
    if (!plan.ok()) {
      return nullptr;
    }
    // The comparison above the join reads the single value's column, which only the join defines.
    EXPECT_EQ(PositionMismatch(*plan), std::nullopt) << sql << "\n" << Explain(*plan);
    EXPECT_TRUE(Optimize(*plan).ok()) << sql;
    const JoinNode* join = testing::FirstJoin(*plan);
    if (join == nullptr || join->kind != JoinKind::kOneRow) {
      ADD_FAILURE() << sql << ": no one-row join\n" << Explain(*plan);
      return nullptr;
    }
    return join->right;
  };
  const auto single_value = [](const LogicalNode& node) -> const AggregateCall* {
    const auto* aggregate = std::get_if<AggregateNode>(&node);
    if (aggregate == nullptr || aggregate->aggregates.size() != 1 ||
        aggregate->aggregates.front().kind != AggKind::kSingleValue) {
      return nullptr;
    }
    return &aggregate->aggregates.front();
  };
  for (const auto& [sql, type] : {
           std::pair{"SELECT i16 FROM t WHERE i32 = (SELECT i32 FROM u)", "INTEGER"},
           std::pair{"SELECT i16 FROM t WHERE i32 = (SELECT MAX(i32) FROM u GROUP BY i16)",
                     "INTEGER"},
           std::pair{"SELECT i16 FROM t WHERE i32 = (SELECT i32 FROM u ORDER BY i32 LIMIT 1 "
                     "OFFSET 2)",
                     "INTEGER"},
           std::pair{"SELECT i16 FROM t WHERE i32 = (SELECT MAX(i32) FROM u HAVING COUNT(*) > 1)",
                     "INTEGER"},
           std::pair{"SELECT i16 FROM t WHERE i32 = (SELECT DISTINCT i32 FROM u)", "INTEGER"},
           std::pair{"SELECT i16 FROM t WHERE i16 = (SELECT i16 FROM u INTERSECT SELECT i16 FROM "
                     "ok)",
                     "SMALLINT"},
           std::pair{"SELECT i16 FROM t WHERE i32 = (SELECT i16 FROM u EXCEPT ALL SELECT i32 FROM "
                     "u)",
                     "INTEGER"},
           std::pair{
               "WITH c AS (SELECT MAX(i32) AS m FROM u) SELECT i16 FROM t WHERE i32 = (SELECT "
               "m FROM c WHERE m > 0)",
               "INTEGER"},
           std::pair{"SELECT i16 FROM t WHERE s = (SELECT s FROM ok LIMIT 1)", "VARCHAR"},
           std::pair{"SELECT i16 FROM t WHERE dt > (SELECT MAX(dt) FROM u GROUP BY i16)", "DATE"},
           std::pair{"SELECT i16 FROM t WHERE h < (SELECT h FROM u ORDER BY h LIMIT 1)",
                     "DECIMAL(38,0)"},
           std::pair{"SELECT t.i16 FROM t JOIN ok ON t.i16 = ok.i16 AND ok.i16 = (SELECT i16 FROM "
                     "u LIMIT 1)",
                     "SMALLINT"},
           std::pair{"SELECT i16, COUNT(*) FROM t GROUP BY i16 HAVING COUNT(*) > (SELECT i64 FROM "
                     "u LIMIT 1)",
                     "BIGINT"},
       }) {
    const LogicalNodePtr right = one_row_right(sql);
    ASSERT_NE(right, nullptr) << sql;
    const AggregateCall* call = single_value(*right);
    ASSERT_NE(call, nullptr) << sql << ": no single value at the join's right input";
    EXPECT_EQ(ToString(call->type), type) << sql;
    if (const std::optional<BoundColumn>& arg = call->arg; arg.has_value()) {
      EXPECT_EQ(arg->type, call->type) << sql << ": the value keeps its column's type";
    } else {
      ADD_FAILURE() << sql << ": the single value has no argument";
    }
  }
  for (const char* sql : {
           "SELECT i16 FROM t WHERE i32 = (SELECT MAX(i32) FROM u)",
           "SELECT i16 FROM t WHERE i32 = (SELECT MAX(i32) + 1 FROM u)",
           "WITH c AS (SELECT MAX(i32) AS m FROM u) SELECT i16 FROM t WHERE i32 = (SELECT m FROM "
           "c)",
       }) {
    const LogicalNodePtr right = one_row_right(sql);
    ASSERT_NE(right, nullptr) << sql;
    EXPECT_EQ(single_value(*right), nullptr) << sql << ": a plan of exactly one row is its value";
  }
  auto plan = BindSql("SELECT i16 FROM t WHERE i32 = (SELECT i32 FROM u)", catalog);
  ASSERT_TRUE(plan.ok()) << plan.status().ToString();
  EXPECT_EQ(Explain(Optimized(*plan)),
            "Output: i16:SMALLINT\n"
            "Project i16\n"
            "  Filter i32 = i32\n"
            "    Join ONE-ROW build=right keys=[]\n"
            "      Scan table=t source=fake columns=[i16, i32]\n"
            "      Aggregate SINGLE_VALUE(i32)\n"
            "        Project i32\n"
            "          Scan table=u source=fake columns=[i32]\n");
}

// The correlated-EXISTS gate reads a name that a select alias of the block and a column of the
// enclosing block share as DuckDB reads it: the alias in ORDER BY, HAVING, a bare GROUP BY key,
// WHERE and the select list, outside the argument of an aggregate or of GROUPING(); the enclosing
// column in an ON, a GROUP BY expression and such an argument (DuckDB reads a GROUPING() argument
// as the alias where the block groups by it, a GROUPING() of an alias, which antb1 refuses anyway).
TEST(BinderTest, AnExistsBlocksAliasIsNoEnclosingColumn) {
  const Catalog catalog = MakeCatalog();
  // The alias, where the block's binder takes it too: an uncorrelated EXISTS, answered.
  for (const char* sql : {
           "SELECT i16 FROM t WHERE EXISTS (SELECT s AS i32 FROM ok ORDER BY i32 LIMIT 1)",
           "SELECT i16 FROM t WHERE EXISTS (SELECT s AS i32 FROM ok GROUP BY i32)",
           "SELECT i16 FROM t WHERE EXISTS (SELECT s AS i32 FROM ok GROUP BY ROLLUP (i32))",
           "SELECT i16 FROM t WHERE NOT EXISTS (SELECT s AS i32 FROM ok GROUP BY s HAVING i32 > "
           "'a')",
           "SELECT i16, COUNT(*) FROM t GROUP BY i16 HAVING EXISTS (SELECT s AS i32 FROM ok ORDER "
           "BY i32 LIMIT 1)",
       }) {
    const auto bound = BindSql(sql, catalog);
    ASSERT_TRUE(bound.ok()) << sql << ": " << bound.status().ToString();
    EXPECT_TRUE(FreeVariables(*bound->root).empty()) << sql;
  }
  // The alias where the block's binder takes none: its bind error in WHERE and in a select
  // expression, as for a name that is neither a column nor the alias of an item before it (DuckDB
  // answers in WHERE, where it takes the alias, and fails in the select list), never the gate's
  // refusal of a correlated block.
  for (const char* sql : {
           "SELECT i16 FROM t WHERE EXISTS (SELECT s AS i32 FROM ok WHERE ok.i16 = i32 LIMIT 1)",
           "SELECT i16 FROM t WHERE EXISTS (SELECT i32 + 1, s AS i32 FROM ok LIMIT 1)",
       }) {
    const auto bound = BindSql(sql, catalog);
    ASSERT_FALSE(bound.ok()) << sql;
    EXPECT_TRUE(bound.status().IsInvalid()) << sql << ": " << bound.status();
  }
  // A name only the enclosing block has stays an enclosing column, and so does an alias's name in
  // an ON, a GROUP BY expression and the argument of an aggregate or of GROUPING(): correlated, and
  // refused at the EXISTS.
  for (const char* sql : {
           "SELECT i16 FROM t WHERE EXISTS (SELECT s FROM ok ORDER BY i32 LIMIT 1)",
           "SELECT i16 FROM t WHERE EXISTS (SELECT ok.s AS i64 FROM ok JOIN dec ON dec.b = i64 "
           "LIMIT 1)",
           "SELECT i16 FROM t WHERE EXISTS (SELECT s AS i32 FROM ok GROUP BY s HAVING MAX(i32) > 0 "
           "LIMIT 1)",
           "SELECT i16 FROM t WHERE EXISTS (SELECT s AS i32 FROM ok GROUP BY i32 + 1, s LIMIT 1)",
           "SELECT i16 FROM t WHERE EXISTS (SELECT s AS i32 FROM ok GROUP BY ROLLUP (s) HAVING "
           "GROUPING(i32) = 0)",
       }) {
    const auto correlated = BindSql(sql, catalog);
    ASSERT_FALSE(correlated.ok()) << sql;
    EXPECT_TRUE(correlated.status().IsNotImplemented()) << sql << ": " << correlated.status();
    EXPECT_TRUE(correlated.status().message().starts_with("EXISTS over a subquery with"))
        << sql << ": " << correlated.status().message();
  }
}

// The single value keeps its column's FLOAT flag (divergence D11): a literal it is compared with
// folds at FLOAT precision, and another column is refused, as next to the column itself.
TEST(BinderTest, TheSingleValueOfAFloatColumnIsAFloat) {
  Catalog catalog;
  ASSERT_TRUE(catalog
                  .Register("tf", std::make_shared<FakeTable>(
                                      arrow::schema({arrow::field("f", arrow::float32()),
                                                     arrow::field("k", arrow::int32())}),
                                      10, std::vector<int>{0}))
                  .ok());
  auto folded = BindSql("SELECT COUNT(*) FROM tf WHERE 0.1 = (SELECT f FROM tf LIMIT 1)", catalog);
  ASSERT_TRUE(folded.ok()) << folded.status().ToString();
  EXPECT_EQ(FilterConstants(*folded), std::vector<double>{static_cast<double>(0.1F)});
  for (const char* sql : {"SELECT COUNT(*) FROM tf WHERE k = (SELECT f FROM tf LIMIT 1)",
                          "SELECT COUNT(*) FROM tf WHERE f BETWEEN (SELECT f FROM tf LIMIT 1) AND "
                          "0.1"}) {
    const auto refused = BindSql(sql, catalog);
    ASSERT_FALSE(refused.ok()) << sql;
    EXPECT_TRUE(refused.status().IsNotImplemented()) << sql << ": " << refused.status().ToString();
    EXPECT_TRUE(refused.status().message().contains("divergence D11"))
        << refused.status().message();
  }
}

// Every shape of ADR 0041's SQ1 places its values (decision 9) without an abort: bound, resolved
// and optimized, a correlated block's residual included, which ADR 0023's unnesting then takes.
TEST(BinderTest, ValuePlacements) {
  const Catalog catalog = MakeCatalog();
  for (const char* sql : {
           // Decision 8: a conjunct of a correlated EXISTS that reads the enclosing block and holds
           // a value is a residual of Path 1, a BETWEEN with a subquery bound whole among them...
           "SELECT i16 FROM t WHERE EXISTS (SELECT 1 FROM u WHERE u.i16 = t.i16 AND t.i64 > "
           "(SELECT MIN(ok.i16) FROM ok))",
           "SELECT i16 FROM t WHERE EXISTS (SELECT 1 FROM u WHERE u.i16 = t.i16 AND t.i32 BETWEEN "
           "(SELECT MIN(ok.i16) FROM ok) AND 100)",
           // ... and a value conjunct of a correlated aggregate's block stands in its slot, which
           // Path 2 takes with the block.
           "SELECT i16 FROM t WHERE i32 = (SELECT MIN(u.i16) FROM u WHERE u.i64 = t.i64 AND "
           "u.i32 > (SELECT MIN(ok.i16) FROM ok))",
           // An IN in an inner ON inside a FULL JOIN's left input: that input's own slot.
           "SELECT t.i16 FROM t JOIN u ON t.i16 = u.i16 AND t.i32 IN (SELECT dec.i FROM dec) FULL "
           "JOIN ok ON u.i16 = ok.i16",
           "SELECT t.i16 FROM t JOIN u ON t.i16 = u.i16 AND t.i32 BETWEEN 1 AND (SELECT "
           "MAX(dec.i) FROM dec) FULL JOIN ok ON u.i16 = ok.i16",
           "SELECT t.i16 FROM t JOIN u ON t.i16 = u.i16 AND (t.i32 > (SELECT MIN(dec.i) FROM dec) "
           "OR NOT EXISTS (SELECT 1 FROM dec)) FULL JOIN ok ON u.i16 = ok.i16",
           // Values inside expressions of WHERE and of an inner ON.
           "SELECT i16 FROM t WHERE (SELECT MIN(i16) FROM u) < (SELECT MAX(i16) FROM u)",
           "SELECT i16 FROM t WHERE i32 > (SELECT COUNT(*) FROM u) + 1",
           "SELECT i16 FROM t WHERE i32 + (SELECT COUNT(*) FROM u) > 0",
           "SELECT i16 FROM t WHERE CAST((SELECT COUNT(*) FROM u) AS BIGINT) > 0",
           "SELECT i16 FROM t WHERE i32 = 1 OR i32 > (SELECT MIN(i16) FROM u)",
           "SELECT i16 FROM t WHERE NOT (i32 > (SELECT MIN(i16) FROM u))",
           "SELECT i16 FROM t WHERE CASE WHEN i32 > (SELECT MIN(i16) FROM u) THEN 1 ELSE (SELECT "
           "MAX(i16) FROM u) END = 1",
           "SELECT i16 FROM t WHERE COALESCE((SELECT MIN(i16) FROM u), i16) > 0",
           "SELECT i16 FROM t WHERE (SELECT MIN(i16) FROM u) IS NULL",
           "SELECT i16 FROM t WHERE i16 IS DISTINCT FROM (SELECT MIN(i16) FROM u)",
           "SELECT i16 FROM t WHERE (SELECT s FROM ok LIMIT 1) LIKE 'a%'",
           "SELECT i16 FROM t WHERE i32 IN (1, (SELECT COUNT(*) FROM u))",
           "SELECT i16 FROM t WHERE i32 NOT IN (1, (SELECT COUNT(*) FROM u))",
           "SELECT i16 FROM t WHERE (SELECT MIN(i16) FROM u) IN (1, 2)",
           "SELECT i16 FROM t WHERE i32 NOT BETWEEN (SELECT MIN(i16) FROM u) AND 10",
           "SELECT i16 FROM t WHERE i32 BETWEEN 1 AND (SELECT MIN(i16) FROM u) + 1",
           "SELECT i16 FROM t WHERE (SELECT MIN(i16) FROM u) BETWEEN i32 AND 10",
           "SELECT i16 FROM t WHERE i32 = 1 OR EXISTS (SELECT 1 FROM u)",
           "SELECT i16 FROM t WHERE CASE WHEN NOT EXISTS (SELECT 1 FROM u) THEN i32 ELSE i16 END > "
           "0",
           "SELECT t.i16 FROM t JOIN u ON t.i16 = u.i16 AND (t.i32 > (SELECT MIN(i16) FROM ok) OR "
           "u.i32 < 0)",
           // One subquery written twice, in one slot: one join.
           "SELECT i16 FROM t WHERE i32 > (SELECT MIN(i16) FROM u) OR i16 < (SELECT MIN(i16) FROM "
           "u)",
           // A correlated comparison whose other side holds a value, or is one, in either order:
           // the value's join stands below Path 2's.
           "SELECT i16 FROM t WHERE i32 + (SELECT COUNT(*) FROM ok) > (SELECT MIN(u.i16) FROM u "
           "WHERE u.i64 = t.i64)",
           "SELECT i16 FROM t WHERE (SELECT MIN(u.i16) FROM u WHERE u.i64 = t.i64) < (SELECT "
           "COUNT(*) FROM ok)",
           "SELECT i16 FROM t WHERE (SELECT COUNT(*) FROM ok) > (SELECT MIN(u.i16) FROM u WHERE "
           "u.i64 = t.i64)",
           // A value that keys a correlated block: Path 1's semi key, Path 2's inner equivalent.
           "SELECT i16 FROM t WHERE EXISTS (SELECT 1 FROM u WHERE (SELECT MIN(ok.i16) FROM ok) = "
           "t.i16)",
           "SELECT i16 FROM t WHERE i32 > (SELECT SUM(u.i32) FROM u WHERE (SELECT MIN(ok.i16) FROM "
           "ok) = t.i16)",
       }) {
    auto plan = BindSql(sql, catalog);
    ASSERT_TRUE(plan.ok()) << sql << ": " << plan.status().ToString();
    EXPECT_EQ(PositionMismatch(*plan), std::nullopt) << sql << "\n" << Explain(*plan);
    const LogicalPlan optimized = Optimized(*plan);
    EXPECT_NE(optimized.root, nullptr) << sql;
  }
}

// ADR 0041's SQ1b: a HAVING conjunct that holds a value is a filter of HAVING's slot, above the
// aggregation, so the value's join is a one-row join over the groups, never a per-row one below
// them (EXPLAIN writes a node before its inputs), and an aggregate's computed argument stays below
// the aggregation, in every form a condition takes there.
TEST(BinderTest, AHavingValueJoinsAboveTheAggregation) {
  const Catalog catalog = MakeCatalog();
  for (const char* sql : {
           "SELECT i16, SUM(i32 * 2) FROM t GROUP BY i16 HAVING SUM(i32 * 2) > (SELECT MIN(u.i32) "
           "FROM u) + 1",
           "SELECT i16 FROM t GROUP BY i16 HAVING COUNT(*) > (SELECT COUNT(*) FROM u) OR MAX(i32) "
           "< 0",
           "SELECT i16 FROM t GROUP BY i16 HAVING i16 IN (1, (SELECT MIN(u.i16) FROM u))",
           "SELECT i16 FROM t GROUP BY i16 HAVING EXISTS (SELECT 1 FROM u) OR COUNT(*) > 1",
           "SELECT i16 FROM t GROUP BY i16 HAVING NOT EXISTS (SELECT 1 FROM u)",
           "SELECT i16 FROM t GROUP BY i16 HAVING MAX(i32) BETWEEN (SELECT MIN(u.i32) FROM u) AND "
           "(SELECT MAX(u.i32) FROM u) + 1",
           "SELECT i16, SUM(i32) + 1 AS s FROM t GROUP BY i16 HAVING s > (SELECT MIN(u.i32) FROM "
           "u) + 1",
           "SELECT COUNT(*) FROM t HAVING COUNT(*) > (SELECT COUNT(*) FROM u) + 0",
           "SELECT i16 FROM t GROUP BY ROLLUP (i16) HAVING GROUPING(i16) = 0 AND COUNT(*) > "
           "(SELECT COUNT(*) FROM u) - 1",
       }) {
    auto plan = BindSql(sql, catalog);
    ASSERT_TRUE(plan.ok()) << sql << ": " << plan.status().ToString();
    EXPECT_EQ(PositionMismatch(*plan), std::nullopt) << sql << "\n" << Explain(*plan);
    const std::string text = Explain(*plan);
    EXPECT_LT(text.find("Join ONE-ROW"), text.find("Aggregate")) << sql << "\n" << text;
    EXPECT_NE(Optimized(*plan).root, nullptr) << sql;
  }
}

// The same subquery in WHERE and in HAVING is two values, one per slot -- a per-row join below the
// aggregation and one over the groups above it -- while written twice in HAVING it is one.
TEST(BinderTest, TheMemoKeepsWhereAndHavingApart) {
  const Catalog catalog = MakeCatalog();
  const auto joins = [&catalog](std::string_view sql) {
    auto plan = BindSql(sql, catalog);
    EXPECT_TRUE(plan.ok()) << sql << ": " << plan.status().ToString();
    if (!plan.ok()) {
      return std::size_t{0};
    }
    const std::string text = Explain(*plan);
    std::size_t n = 0;
    for (std::size_t at = text.find("Join ONE-ROW"); at != std::string::npos;
         at = text.find("Join ONE-ROW", at + 1)) {
      ++n;
    }
    return n;
  };
  EXPECT_EQ(joins("SELECT i16 FROM t WHERE i32 > (SELECT MIN(u.i32) FROM u) GROUP BY i16 HAVING "
                  "COUNT(*) > (SELECT MIN(u.i32) FROM u)"),
            2U);
  EXPECT_EQ(joins("SELECT i16 FROM t GROUP BY i16 HAVING COUNT(*) > (SELECT MIN(u.i32) FROM u) "
                  "AND MAX(i32) < (SELECT MIN(u.i32) FROM u) + 10"),
            1U);
}

// ADR 0041's SQ2a: a value in a select item or an ORDER BY item of a block that does not aggregate
// binds and resolves in every form and block it may stand in.
TEST(BinderTest, SelectValuePlacements) {
  const Catalog catalog = MakeCatalog();
  for (const char* sql : {
           "SELECT i16, (SELECT MIN(i16) FROM u) FROM t",
           "SELECT i16 * (SELECT MIN(i16) FROM u) + 1 AS v FROM t",
           "SELECT *, (SELECT COUNT(*) FROM u) FROM ok",
           "SELECT COALESCE((SELECT MIN(i32) FROM u), i32), CAST((SELECT MIN(i16) FROM u) AS "
           "BIGINT) FROM t",
           "SELECT CASE WHEN EXISTS (SELECT 1 FROM u) THEN (SELECT MIN(i16) FROM u) ELSE "
           "(SELECT MAX(i16) FROM u) END FROM t",
           "SELECT CASE (SELECT MIN(i16) FROM u) WHEN 1 THEN i32 ELSE 0 END FROM t",
           "SELECT CASE WHEN i16 IN (1, (SELECT MIN(i16) FROM u)) OR i32 BETWEEN (SELECT "
           "MIN(i32) FROM u) AND 10 THEN 1 ELSE 0 END FROM t",
           "SELECT DISTINCT i16, (SELECT MIN(i16) FROM u) AS m FROM t ORDER BY m, i16",
           "SELECT i16 FROM t ORDER BY i32 * (SELECT MIN(i16) FROM u) DESC NULLS FIRST, i16 LIMIT "
           "3 OFFSET 1",
           "SELECT i16, (SELECT MIN(i16) FROM u) AS m FROM t ORDER BY m, 1",
           // Divergence D37: an ORDER BY expression over a value item's alias.
           "SELECT i16, (SELECT COUNT(*) FROM u) AS n FROM t ORDER BY n - i16",
           // Inside the blocks a value may be in: IN's, a value's, a derived table's, a CTE's and
           // a compound's arms.
           "SELECT i16 FROM t WHERE i32 IN (SELECT i16 + (SELECT MIN(i16) FROM ok) FROM u)",
           "SELECT i16 FROM t WHERE i32 > (SELECT i16 + 0 FROM u ORDER BY (SELECT MIN(i16) FROM "
           "ok), i16 LIMIT 1)",
           "SELECT (SELECT (SELECT MIN(i16) FROM ok) FROM u LIMIT 1) FROM t",
           "SELECT d.v FROM (SELECT i16 + (SELECT MIN(i16) FROM u) AS v FROM t) AS d",
           "WITH c AS (SELECT i16, (SELECT MIN(i16) FROM u) AS m FROM t) SELECT i16, m FROM c",
           "SELECT x FROM (SELECT i16 + (SELECT MIN(i16) FROM ok) AS x FROM t UNION ALL SELECT "
           "(SELECT MAX(i16) FROM ok) FROM u) AS d ORDER BY x",
           "SELECT t.i16, (SELECT MIN(i16) FROM ok) FROM t LEFT JOIN u ON t.i16 = u.i16",
           "SELECT t.i16, u.i32 + (SELECT MIN(i16) FROM ok) FROM t FULL JOIN u ON t.i16 = u.i16",
       }) {
    auto plan = BindSql(sql, catalog);
    ASSERT_TRUE(plan.ok()) << sql << ": " << plan.status().ToString();
    EXPECT_EQ(PositionMismatch(*plan), std::nullopt) << sql << "\n" << Explain(*plan);
    EXPECT_NE(Optimized(*plan).root, nullptr) << sql;
  }
}

// Decision 9's per-row slot: a select item's or an ORDER BY item's value joins in block 0's slot,
// below the computed items, the sort, DISTINCT and the projection (EXPLAIN writes a node before its
// inputs); one subquery written in the list and in ORDER BY is one join (decision 10).
TEST(BinderTest, AProjectionValueJoinsInTheRowSlot) {
  const Catalog catalog = MakeCatalog();
  const auto count = [](const std::string& text, std::string_view what) {
    std::size_t n = 0;
    for (std::size_t at = text.find(what); at != std::string::npos; at = text.find(what, at + 1)) {
      ++n;
    }
    return n;
  };
  for (const char* sql : {
           "SELECT i16, i32 * (SELECT MIN(i16) FROM u) AS v FROM t ORDER BY i32 * (SELECT "
           "MIN(i16) FROM u), i16",
           "SELECT DISTINCT i16 + (SELECT MIN(i16) FROM u) AS v FROM t ORDER BY v",
           "SELECT CASE WHEN EXISTS (SELECT 1 FROM u) THEN i16 ELSE 0 END FROM t WHERE i32 > 0 "
           "ORDER BY 1",
       }) {
    auto plan = BindSql(sql, catalog);
    ASSERT_TRUE(plan.ok()) << sql << ": " << plan.status().ToString();
    const std::string text = Explain(*plan);
    EXPECT_EQ(count(text, "Join ONE-ROW"), 1U) << sql << "\n" << text;
    for (const std::string_view above : {"Compute", "Sort", "Project"}) {
      if (text.contains(above)) {
        EXPECT_LT(text.find(above), text.find("Join ONE-ROW")) << sql << "\n" << text;
      }
    }
    EXPECT_LT(text.find("Join ONE-ROW"), text.find("Scan table=t")) << sql << "\n" << text;
  }
}

// An EXISTS block's select list is dropped (DuckDB too): a value in it that no other clause of the
// block reads binds as a dry run and plants no join (an uncorrelated EXISTS is one join itself, of
// its count), correlated EXISTS or not, while one that its ORDER BY reads -- by position, by alias,
// or inside an expression -- is planted. The placeholder of a dropped item keeps the items after it
// at their positions (critique P1-4's c26).
TEST(BinderTest, AnExistsSelectListPlantsNothing) {
  const Catalog catalog = MakeCatalog();
  for (
      const char* sql : {
          "SELECT i16 FROM t WHERE EXISTS (SELECT (SELECT i16 FROM ok) FROM u)",
          "SELECT i16 FROM t WHERE EXISTS (SELECT (SELECT i16 FROM ok), u.i16 FROM u WHERE u.i64 "
          "= t.i64)",
          // A correlated value of a shape Path 2 takes, dropped with the list (DuckDB answers).
          "SELECT i16 FROM t WHERE EXISTS (SELECT (SELECT MIN(ok.i16) FROM ok WHERE ok.i16 = "
          "u.i32) FROM u)",
          "SELECT i16 FROM t WHERE i32 = 1 OR NOT EXISTS (SELECT i16 + (SELECT MIN(i16) FROM ok) "
          "FROM u)",
          "SELECT i16 FROM t WHERE EXISTS (SELECT i16 + (SELECT MIN(i16) FROM ok) AS a, i16 * 2 AS "
          "b, i16 * 3 AS c FROM u ORDER BY 2 LIMIT 1)",
          "SELECT i16 FROM t WHERE EXISTS (SELECT i16 + (SELECT MIN(i16) FROM ok) AS a, i16 * 2 AS "
          "b, i16 * 3 AS c FROM u ORDER BY 3 LIMIT 1)",
          // What ORDER BY reads as it binds: a position after a star's eleven columns, a name that
          // the block's column takes before the alias inside an expression, and a bare name that a
          // later item's alias takes.
          "SELECT i16 FROM t WHERE EXISTS (SELECT *, i16, (SELECT MIN(i16) FROM ok) FROM u ORDER "
          "BY "
          "12 LIMIT 1)",
          "SELECT i16 FROM t WHERE EXISTS (SELECT (SELECT MIN(i16) FROM ok) AS i32 FROM u ORDER BY "
          "i32 + 1 LIMIT 1)",
          "SELECT i16 FROM t WHERE EXISTS (SELECT (SELECT MIN(i16) FROM ok) AS v, i16 AS v FROM u "
          "ORDER BY v LIMIT 1)",
      }) {
    auto plan = BindSql(sql, catalog);
    ASSERT_TRUE(plan.ok()) << sql << ": " << plan.status().ToString();
    const std::string text = Explain(*plan);
    EXPECT_FALSE(text.contains("table=ok")) << sql << "\n" << text;
    EXPECT_EQ(PositionMismatch(*plan), std::nullopt) << sql << "\n" << text;
    EXPECT_NE(Optimized(*plan).root, nullptr) << sql;
  }
  // The items after a dropped one keep their columns: ORDER BY 2 sorts by b, ORDER BY 3 by c, the
  // one expression the optimized plan computes.
  for (const auto& [position, computed] :
       {std::pair{2, "Compute (i16 * 2)\n"}, std::pair{3, "Compute (i16 * 3)\n"}}) {
    const std::string sql = std::format(
        "SELECT i16 FROM t WHERE EXISTS (SELECT i16 + (SELECT MIN(i16) FROM ok) AS a, i16 * 2 AS "
        "b, "
        "i16 * 3 AS c FROM u ORDER BY {} LIMIT 1)",
        position);
    auto plan = BindSql(sql, catalog);
    ASSERT_TRUE(plan.ok()) << sql << ": " << plan.status().ToString();
    const std::string text = Explain(Optimized(*plan));
    EXPECT_TRUE(text.contains(computed)) << sql << "\n" << text;
  }
}

// ADR 0041's SQ2b: a value in a block that aggregates binds and resolves in every place: select
// items and ORDER BY over keys and aggregates, GROUP BY keys and their alias and position,
// aggregate arguments, HAVING over a value item's alias, global blocks and grouping sets with plain
// elements.
TEST(BinderTest, GroupValuePlacements) {
  const Catalog catalog = MakeCatalog();
  for (
      const char* sql : {
          "SELECT i16, COUNT(*) + (SELECT COUNT(*) FROM u) AS c FROM t GROUP BY i16",
          "SELECT COUNT(*) * (SELECT MIN(i16) FROM u) FROM t",
          "SELECT i16 + (SELECT MIN(i16) FROM u), SUM(i32) FROM t GROUP BY i16",
          "SELECT i16, CASE WHEN COUNT(*) > (SELECT COUNT(*) FROM u) THEN i16 ELSE 0 END FROM t "
          "GROUP BY i16",
          "SELECT i16, CASE WHEN EXISTS (SELECT 1 FROM u) THEN COUNT(*) ELSE 0 END FROM t GROUP BY "
          "i16",
          "SELECT i16 % 2 + (SELECT MIN(i16) FROM u) AS k, COUNT(*) FROM t GROUP BY i16 % 2 + "
          "(SELECT MIN(i16) FROM u)",
          "SELECT i16 % 2 + (SELECT MIN(i16) FROM u) AS k, COUNT(*) FROM t GROUP BY k ORDER BY k",
          "SELECT i16 % 2 + (SELECT MIN(i16) FROM u) AS k, COUNT(*) FROM t GROUP BY 1 HAVING k > 1",
          "SELECT i16 % 2 + (SELECT MIN(i16) FROM u) + 1 AS m, COUNT(*) AS c FROM t GROUP BY i16 % "
          "2 + (SELECT MIN(i16) FROM u) ORDER BY m * 2, c",
          "SELECT (SELECT MIN(i16) FROM u), COUNT(*) FROM t GROUP BY (SELECT MIN(i16) FROM u)",
          "SELECT i16, SUM(i32 * (SELECT MIN(i16) FROM u)) FROM t GROUP BY i16",
          "SELECT COUNT(DISTINCT i32 % (SELECT MIN(i16) FROM u)), MAX(i16 + (SELECT MIN(i16) FROM "
          "u)) FROM t",
          "SELECT i16 FROM t GROUP BY i16 HAVING SUM(i32 * (SELECT MIN(i16) FROM u)) > 0 ORDER BY "
          "SUM(i32 + (SELECT MAX(i16) FROM u))",
          "SELECT i16, COUNT(*) + (SELECT COUNT(*) FROM u) AS c FROM t GROUP BY i16 HAVING c > 2 "
          "AND c BETWEEN 1 AND 9",
          "SELECT i16, MAX(i32) + (SELECT MIN(i32) FROM u) AS c FROM t GROUP BY i16 HAVING c IN "
          "(SELECT i16 FROM ok)",
          "SELECT i16 % 2 + 1 AS k, COUNT(*) FROM t GROUP BY i16 % 2 + 1 HAVING k + (SELECT "
          "MIN(i16) FROM u) > 2",
          "SELECT COUNT(*) AS c, (SELECT MAX(i16) FROM u) AS m FROM t ORDER BY m",
          "SELECT SUM(i32) FROM t ORDER BY SUM(i32 * (SELECT MIN(i16) FROM u))",
          "SELECT DISTINCT i16 % 3, COUNT(*) + (SELECT COUNT(*) FROM u) FROM t GROUP BY i16",
          "SELECT i16, i32, COUNT(*) + (SELECT COUNT(*) FROM u) FROM t GROUP BY ROLLUP (i16, i32)",
          "SELECT d.c FROM (SELECT i16, COUNT(*) * (SELECT MIN(i16) FROM u) AS c FROM t GROUP BY "
          "i16) AS d",
          "SELECT i16 FROM t WHERE i32 > (SELECT SUM(i32) + (SELECT MIN(i16) FROM ok) FROM u)",
          // Divergence D37 over a grouped item that still holds its value.
          "SELECT i16, COUNT(*) + (SELECT COUNT(*) FROM u) AS c FROM t GROUP BY i16 ORDER BY c * 2",
      }) {
    auto plan = BindSql(sql, catalog);
    ASSERT_TRUE(plan.ok()) << sql << ": " << plan.status().ToString();
    EXPECT_EQ(PositionMismatch(*plan), std::nullopt) << sql << "\n" << Explain(*plan);
    EXPECT_NE(Optimized(*plan).root, nullptr) << sql;
  }
}

// Decision 9's slots in a block that aggregates: a value over the aggregation joins above it, one
// in a GROUP BY key or an aggregate's argument below it, per row (EXPLAIN writes a node before its
// inputs, and the per-row join's own input is the scan of t).
TEST(BinderTest, GroupValuesJoinInTheirSlots) {
  const Catalog catalog = MakeCatalog();
  const auto text = [&catalog](std::string_view sql) {
    auto plan = BindSql(sql, catalog);
    EXPECT_TRUE(plan.ok()) << sql << ": " << plan.status().ToString();
    return plan.ok() ? Explain(*plan) : std::string();
  };
  for (const char* sql : {
           "SELECT i16, COUNT(*) + (SELECT COUNT(*) FROM u) FROM t GROUP BY i16",
           "SELECT i16 FROM t GROUP BY i16 ORDER BY COUNT(*) * (SELECT MIN(i16) FROM u)",
           "SELECT i16, COUNT(*) + (SELECT COUNT(*) FROM u) AS c FROM t GROUP BY i16 HAVING c > 2",
       }) {
    const std::string plan = text(sql);
    EXPECT_LT(plan.find("Join ONE-ROW"), plan.find("GroupAggregate")) << sql << "\n" << plan;
  }
  for (const char* sql : {
           "SELECT i16 % 2 + (SELECT MIN(i16) FROM u) AS k, COUNT(*) FROM t GROUP BY 1",
           "SELECT i16, SUM(i32 * (SELECT MIN(i16) FROM u)) FROM t GROUP BY i16",
           "SELECT i16 FROM t GROUP BY i16 HAVING SUM(i32 * (SELECT MIN(i16) FROM u)) > 0",
       }) {
    const std::string plan = text(sql);
    EXPECT_GT(plan.find("Join ONE-ROW"), plan.find("GroupAggregate")) << sql << "\n" << plan;
  }
}

// A GROUP BY key that holds a value binds it per row, and the select item, HAVING and ORDER BY
// written alike find the key through the value already bound (BindOutput's lookup-only
// speculation): one join, below the aggregation. A HAVING operand that no key holds still plants no
// per-row join (SQ1b's rule), also next to the alias of a key item (the SQ2 critique's c27).
TEST(BinderTest, AValueKeyIsMatchedWithoutASecondJoin) {
  const Catalog catalog = MakeCatalog();
  const auto joins = [&catalog](std::string_view sql) {
    auto plan = BindSql(sql, catalog);
    EXPECT_TRUE(plan.ok()) << sql << ": " << plan.status().ToString();
    if (!plan.ok()) {
      return std::pair<std::size_t, std::string>{0, ""};
    }
    const std::string text = Explain(*plan);
    std::size_t n = 0;
    for (std::size_t at = text.find("Join ONE-ROW"); at != std::string::npos;
         at = text.find("Join ONE-ROW", at + 1)) {
      ++n;
    }
    return std::pair{n, text};
  };
  for (const char* sql : {
           "SELECT i16 % 2 + (SELECT MIN(i16) FROM u), COUNT(*) FROM t GROUP BY i16 % 2 + (SELECT "
           "MIN(i16) FROM u) HAVING i16 % 2 + (SELECT MIN(i16) FROM u) > 1 ORDER BY i16 % 2 + "
           "(SELECT MIN(i16) FROM u)",
           "SELECT i16 % 2 + (SELECT MIN(i16) FROM u) AS k, COUNT(*) FROM t GROUP BY k",
       }) {
    const auto [n, text] = joins(sql);
    EXPECT_EQ(n, 1U) << sql << "\n" << text;
    EXPECT_GT(text.find("Join ONE-ROW"), text.find("GroupAggregate")) << sql << "\n" << text;
  }
  const auto [n, text] = joins(
      "SELECT i16 % 2 + 1 AS k, COUNT(*) FROM t GROUP BY i16 % 2 + 1 HAVING k + (SELECT "
      "MIN(i16) FROM u) > 2");
  EXPECT_EQ(n, 1U) << text;
  EXPECT_LT(text.find("Join ONE-ROW"), text.find("GroupAggregate")) << text;
}

// An aggregate is computed even in an EXISTS block's dropped item (divergence D32), so a value in
// its argument is planted, dry run or not, its join below the aggregation: under WHERE and under
// HAVING, grouped or not.
TEST(BinderTest, AnExistsAggregateArgumentValueIsPlanted) {
  const Catalog catalog = MakeCatalog();
  for (const char* sql : {
           "SELECT i16 FROM t WHERE EXISTS (SELECT SUM(i32 * (SELECT MIN(i16) FROM ok)) + 1 FROM "
           "u)",
           "SELECT i16 FROM t WHERE EXISTS (SELECT i16, MAX(i32 + (SELECT MIN(i16) FROM ok)) * 2 "
           "FROM u "
           "GROUP BY i16 ORDER BY 1 LIMIT 1)",
           "SELECT i16, COUNT(*) FROM t GROUP BY i16 HAVING EXISTS (SELECT SUM(i32 * (SELECT "
           "MIN(i16) "
           "FROM ok)) + 1 FROM u)",
       }) {
    auto plan = BindSql(sql, catalog);
    ASSERT_TRUE(plan.ok()) << sql << ": " << plan.status().ToString();
    const std::string text = Explain(*plan);
    EXPECT_TRUE(text.contains("table=ok")) << sql << "\n" << text;
    EXPECT_EQ(PositionMismatch(*plan), std::nullopt) << sql << "\n" << text;
    EXPECT_NE(Optimized(*plan).root, nullptr) << sql;
  }
}

TEST(BinderTest, AnExistsOrderByThatReadsAnItemPlantsIt) {
  const Catalog catalog = MakeCatalog();
  for (const char* sql : {
           "SELECT i16 FROM t WHERE EXISTS (SELECT i16 + (SELECT MIN(i16) FROM ok) AS a FROM u "
           "ORDER BY 1 LIMIT 1)",
           "SELECT i16 FROM t WHERE EXISTS (SELECT i16 + (SELECT MIN(i16) FROM ok) AS a FROM u "
           "ORDER BY a LIMIT 1)",
           "SELECT i16 FROM t WHERE EXISTS (SELECT i16 + (SELECT MIN(i16) FROM ok) AS a FROM u "
           "ORDER BY a * 2 LIMIT 1)",
           // A position counts a star's columns, and a leading zero is the same position.
           "SELECT i16 FROM t WHERE EXISTS (SELECT *, i16 + (SELECT MIN(i16) FROM ok) FROM u ORDER "
           "BY "
           "12 LIMIT 1)",
           "SELECT i16 FROM t WHERE EXISTS (SELECT i16 + (SELECT MIN(i16) FROM ok) FROM u ORDER BY "
           "01 "
           "LIMIT 1)",
       }) {
    auto plan = BindSql(sql, catalog);
    ASSERT_TRUE(plan.ok()) << sql << ": " << plan.status().ToString();
    const std::string text = Explain(*plan);
    EXPECT_TRUE(text.contains("Join ONE-ROW")) << sql << "\n" << text;
    EXPECT_TRUE(text.contains("table=ok")) << sql << "\n" << text;
    EXPECT_EQ(PositionMismatch(*plan), std::nullopt) << sql << "\n" << text;
    EXPECT_NE(Optimized(*plan).root, nullptr) << sql;
  }
}

// ADR 0041's decision 10: a subquery binds once per slot, keyed by what is written, so the copies
// the binder makes of a conjunct -- a BETWEEN's two comparisons, IS DISTINCT FROM's lowering, CASE
// x WHEN's equalities, a comparison whose constant moves -- and a subquery written twice, in an ON
// and in WHERE alike, read one join.
TEST(BinderTest, ASubqueryIsBoundOncePerSlot) {
  const Catalog catalog = MakeCatalog();
  const auto optimized = [&catalog](std::string_view sql) -> std::string {
    auto plan = BindSql(sql, catalog);
    EXPECT_TRUE(plan.ok()) << sql << ": " << plan.status().ToString();
    if (!plan.ok()) {
      return "";
    }
    EXPECT_EQ(PositionMismatch(*plan), std::nullopt) << sql << "\n" << Explain(*plan);
    return Explain(Optimized(*plan));
  };
  EXPECT_EQ(optimized("SELECT i16 FROM t WHERE i32 BETWEEN (SELECT MIN(i16) FROM u) AND (SELECT "
                      "MIN(i16) FROM u) + 10"),
            "Output: i16:SMALLINT\n"
            "Project i16\n"
            "  Filter i32 <= \"(min(i16) + 10)\"\n"
            "    Compute (min(i16) + 10)\n"
            "      Filter i32 >= \"min(i16)\"\n"
            "        Join ONE-ROW build=right keys=[]\n"
            "          Scan table=t source=fake columns=[i16, i32]\n"
            "          Aggregate MIN(i16)\n"
            "            Scan table=u source=fake columns=[i16]\n");
  EXPECT_EQ(optimized("SELECT i16 FROM t WHERE i16 IS DISTINCT FROM (SELECT MIN(i16) FROM u) AND "
                      "CASE (SELECT MIN(i16) FROM u) WHEN 1 THEN i32 ELSE 0 END > 0"),
            "Output: i16:SMALLINT\n"
            "Project i16\n"
            "  Filter \"CASE  WHEN (((SELECT MIN(i16) FROM u) = 1)) THEN (i32) ELSE 0 END\" > 0\n"
            "    Compute CASE  WHEN (((SELECT MIN(i16) FROM u) = 1)) THEN (i32) ELSE 0 END\n"
            "      Filter i16 IS DISTINCT FROM \"min(i16)\"\n"
            "        Join ONE-ROW build=right keys=[]\n"
            "          Scan table=t source=fake columns=[i16, i32]\n"
            "          Aggregate MIN(i16)\n"
            "            Scan table=u source=fake columns=[i16]\n");
  EXPECT_EQ(
      optimized("SELECT t.i16 FROM t JOIN u ON t.i16 = u.i16 AND t.i32 > (SELECT MIN(i16) FROM "
                "ok) WHERE u.i32 + 1 > (SELECT MIN(i16) FROM ok) OR t.i64 < 0"),
      "Output: i16:SMALLINT\n"
      "Project t.i16\n"
      "  Filter \"(((u.i32 + 1) > (SELECT MIN(i16) FROM ok)) OR (t.i64 < 0))\"\n"
      "    Compute (((u.i32 + 1) > (SELECT MIN(i16) FROM ok)) OR (t.i64 < 0))\n"
      "      Filter t.i32 > \"min(i16)\"\n"
      "        Join ONE-ROW build=right keys=[] carries 1 of 6 unread above\n"
      "          Join INNER build=right keys=[u.i16 = t.i16] carries 1 of 5 unread above\n"
      "            Scan table=u source=fake columns=[i16, i32]\n"
      "            Scan table=t source=fake columns=[i16, i32, i64]\n"
      "          Aggregate MIN(i16)\n"
      "            Scan table=ok source=fake columns=[i16]\n");
  // IS DISTINCT FROM's lowering, which a constant side makes (x <> 5 OR x IS NULL), and a
  // comparison whose constant moves across the value both read the one join.
  EXPECT_EQ(optimized("SELECT i16 FROM t WHERE (SELECT MIN(i16) FROM u) IS DISTINCT FROM 5 AND "
                      "(SELECT MIN(i16) FROM u) + 1 > 5"),
            "Output: i16:SMALLINT\n"
            "Project i16\n"
            "  Filter \"min(i16)\" > 4\n"
            "    Filter \"((SELECT MIN(i16) FROM u) IS DISTINCT FROM 5)\"\n"
            "      Compute ((SELECT MIN(i16) FROM u) IS DISTINCT FROM 5)\n"
            "        Join ONE-ROW build=right keys=[]\n"
            "          Scan table=t source=fake columns=[i16]\n"
            "          Aggregate MIN(i16)\n"
            "            Scan table=u source=fake columns=[i16]\n");
  // Top-level comparisons and EXISTS conjuncts written alike read one join each, too.
  EXPECT_EQ(
      optimized("SELECT i16 FROM t WHERE i32 > (SELECT MIN(i16) FROM u) AND i16 < (SELECT "
                "MIN(i16) FROM u) AND EXISTS (SELECT 1 FROM ok) AND EXISTS (SELECT 1 FROM ok)"),
      "Output: i16:SMALLINT\n"
      "Project i16\n"
      "  Filter \"count_star()\" = 1\n"
      "    Filter \"count_star()\" = 1\n"
      "      Join ONE-ROW build=right keys=[] carries 2 of 4 unread above\n"
      "        Filter i16 < \"min(i16)\"\n"
      "          Filter i32 > \"min(i16)\"\n"
      "            Join ONE-ROW build=right keys=[]\n"
      "              Scan table=t source=fake columns=[i16, i32]\n"
      "              Aggregate MIN(i16)\n"
      "                Scan table=u source=fake columns=[i16]\n"
      "        Aggregate COUNT(*)\n"
      "          Limit 1\n"
      "            Scan table=ok source=fake columns=[]\n");
}

// OR factoring's dropped branch binds as a dry run (ADR 0041's decision 10): its subquery plants no
// join, so a value of two rows there never fails, but a bind error in it is still that error.
TEST(BinderTest, ADroppedOrBranchPlansNoJoinButKeepsItsBindErrors) {
  const Catalog catalog = MakeCatalog();
  const auto optimized = [&catalog](std::string_view sql) -> std::string {
    auto plan = BindSql(sql, catalog);
    EXPECT_TRUE(plan.ok()) << sql << ": " << plan.status().ToString();
    if (!plan.ok()) {
      return "";
    }
    EXPECT_EQ(PositionMismatch(*plan), std::nullopt) << sql << "\n" << Explain(*plan);
    return Explain(Optimized(*plan));
  };
  EXPECT_EQ(optimized("SELECT i16 FROM t WHERE (i32 = 2 AND i32 > (SELECT i16 FROM u)) OR i32 = 2"),
            "Output: i16:SMALLINT\n"
            "Project i16\n"
            "  Filter i32 = 2\n"
            "    Scan table=t source=fake columns=[i16, i32]\n");
  auto error = BindSql(
      "SELECT i16 FROM t WHERE (i32 = 2 AND i32 > (SELECT nope FROM u)) OR i32 = 2", catalog);
  ASSERT_FALSE(error.ok());
  EXPECT_EQ(SpanText("SELECT i16 FROM t WHERE (i32 = 2 AND i32 > (SELECT nope FROM u)) OR i32 = 2",
                     error.status()),
            "nope");
}

// A block's per-row slot stands above its joins and below the filter of its correlated conjuncts
// (ADR 0041's decision 9), which a value that such a conjunct holds is then read by: ADR 0023's
// Path 1 takes the slot with the block, and the conjunct as a residual of its semi join (decision
// 8).
TEST(BinderTest, TheSlotsStandWhereDecisionNinePutsThem) {
  const Catalog catalog = MakeCatalog();
  constexpr std::string_view kSql =
      "SELECT i16 FROM t WHERE EXISTS (SELECT 1 FROM u WHERE u.i16 = t.i16 AND t.i64 > (SELECT "
      "MIN(ok.i16) FROM ok) AND u.i32 > (SELECT MAX(ok.i16) FROM ok) + 1)";
  auto plan = BindSql(kSql, catalog);
  ASSERT_TRUE(plan.ok()) << plan.status().ToString();
  EXPECT_EQ(
      Explain(*plan),
      "Output: i16:SMALLINT\n"
      "Project i16\n"
      "  DependentJoin SEMI\n"
      "    Scan table=t source=fake columns=[i16, i32, i64, u16, h, d, s, dt, bad, \"Mixed Case\", "
      "from]\n"
      "    Project 1\n"
      "      Filter \"(u.i16 = t.i16)\" AND \"(t.i64 > (SELECT MIN(ok.i16) FROM ok))\"\n"
      "        Compute (u.i16 = t.i16), (t.i64 > (SELECT MIN(ok.i16) FROM ok))\n"
      "          Filter i32 > \"(max(ok.i16) + 1)\"\n"
      "            Compute (max(ok.i16) + 1)\n"
      "              Join ONE-ROW build=right keys=[]\n"
      "                Join ONE-ROW build=right keys=[]\n"
      "                  Scan table=u source=fake columns=[i16, i32, i64, u16, h, d, s, dt, bad, "
      "\"Mixed Case\", from]\n"
      "                  Aggregate MIN(i16)\n"
      "                    Scan table=ok source=fake columns=[i16, s]  [shared #1 of 2, 2 fields]\n"
      "                Aggregate MAX(i16)\n"
      "                  Scan table=ok source=fake columns=[i16, s]  [shared #1 of 2, 2 fields]\n");
  EXPECT_EQ(Explain(Optimized(*plan)),
            "Output: i16:SMALLINT\n"
            "Project i16\n"
            "  Join SEMI build=right keys=[\"t.i16\" = \"u.i16\"] residual=[(t.i64 > (SELECT "
            "MIN(ok.i16) FROM ok))] carries 1 of 2 unread above\n"
            "    Scan table=t source=fake columns=[i16, i64]\n"
            "    Filter i32 > \"(max(ok.i16) + 1)\"\n"
            "      Compute (max(ok.i16) + 1)\n"
            "        Join ONE-ROW build=right keys=[]\n"
            "          Join ONE-ROW build=right keys=[]\n"
            "            Scan table=u source=fake columns=[i16, i32]\n"
            "            Aggregate MIN(i16)\n"
            "              Scan table=ok source=fake columns=[i16]  [shared #1 of 2, 1 field]\n"
            "          Aggregate MAX(i16)\n"
            "            Scan table=ok source=fake columns=[i16]  [shared #1 of 2, 1 field]\n");
}

// An inner ON inside a FULL JOIN's left input has that input's own slot, below the FULL JOIN, which
// then pads what its filters keep (ADR 0041's SQ1; ADR 0037's OJ3 left it to this phase).
TEST(BinderTest, AFullJoinsLeftInputHasASlotOfItsOwn) {
  const Catalog catalog = MakeCatalog();
  const auto optimized = [&catalog](std::string_view sql) -> std::string {
    auto plan = BindSql(sql, catalog);
    EXPECT_TRUE(plan.ok()) << sql << ": " << plan.status().ToString();
    if (!plan.ok()) {
      return "";
    }
    EXPECT_EQ(PositionMismatch(*plan), std::nullopt) << sql << "\n" << Explain(*plan);
    return Explain(Optimized(*plan));
  };
  EXPECT_EQ(optimized("SELECT COUNT(*) FROM t JOIN u ON t.i16 = u.i16 AND t.i32 < (SELECT MIN(i16) "
                      "FROM ok) + 2 FULL JOIN dec ON u.i32 = dec.i"),
            "Output: count_star():BIGINT\n"
            "Aggregate COUNT(*)\n"
            "  Join FULL build=right keys=[u.i32 = dec.i]\n"
            "    Filter t.i32 < \"(min(i16) + 2)\"\n"
            "      Compute (min(i16) + 2)\n"
            "        Join ONE-ROW build=right keys=[] carries 2 of 5 unread above\n"
            "          Join INNER build=right keys=[u.i16 = t.i16] carries 2 of 4 unread above\n"
            "            Scan table=u source=fake columns=[i16, i32]\n"
            "            Scan table=t source=fake columns=[i16, i32]\n"
            "          Aggregate MIN(i16)\n"
            "            Scan table=ok source=fake columns=[i16]\n"
            "    Scan table=dec source=fake columns=[i]\n");
}

// An uncorrelated EXISTS is COUNT(*) over LIMIT 1 of its block under a one-row join, compared with
// 1, or with 0 for NOT EXISTS (ADR 0041's decision 5), DuckDB's own plan. Its select list -- the
// Projects among the Limits and Sorts at the top of its plan -- is dropped, as DuckDB drops it, so
// nothing computes a select item that nothing else reads, also under a LIMIT; its ORDER BY stays,
// whose keys DuckDB computes too (a top-N under the LIMIT 1). Its join stands where a scalar
// subquery's would: above the block's joins, and above the aggregation for a HAVING one.
TEST(BinderTest, AnUncorrelatedExistsIsACountOverLimitOne) {
  const Catalog catalog = MakeCatalog();
  const auto optimized = [&catalog](std::string_view sql) -> std::string {
    auto plan = BindSql(sql, catalog);
    EXPECT_TRUE(plan.ok()) << sql << ": " << plan.status().ToString();
    if (!plan.ok()) {
      return "";
    }
    EXPECT_EQ(PositionMismatch(*plan), std::nullopt) << sql << "\n" << Explain(*plan);
    return Explain(Optimized(*plan));
  };
  EXPECT_EQ(optimized("SELECT i16 FROM t WHERE EXISTS (SELECT i32, s FROM u WHERE i64 > 0 ORDER "
                      "BY s)"),
            "Output: i16:SMALLINT\n"
            "Project i16\n"
            "  Filter \"count_star()\" = 1\n"
            "    Join ONE-ROW build=right keys=[]\n"
            "      Scan table=t source=fake columns=[i16]\n"
            "      Aggregate COUNT(*)\n"
            "        Limit 1\n"
            "          Sort s ASC NULLS LAST\n"
            "            Filter i64 > 0\n"
            "              Scan table=u source=fake columns=[i64, s]\n");
  EXPECT_EQ(optimized("SELECT i16 FROM t WHERE NOT EXISTS (SELECT * FROM u)"),
            "Output: i16:SMALLINT\n"
            "Project i16\n"
            "  Filter \"count_star()\" = 0\n"
            "    Join ONE-ROW build=right keys=[]\n"
            "      Scan table=t source=fake columns=[i16]\n"
            "      Aggregate COUNT(*)\n"
            "        Limit 1\n"
            "          Scan table=u source=fake columns=[]\n");
  EXPECT_EQ(optimized("SELECT i16 FROM t WHERE EXISTS (SELECT i32 + 1, s FROM u ORDER BY i32 "
                      "LIMIT 2 OFFSET 3)"),
            "Output: i16:SMALLINT\n"
            "Project i16\n"
            "  Filter \"count_star()\" = 1\n"
            "    Join ONE-ROW build=right keys=[]\n"
            "      Scan table=t source=fake columns=[i16]\n"
            "      Aggregate COUNT(*)\n"
            "        Limit 1\n"
            "          Limit 2 OFFSET 3\n"
            "            Sort i32 ASC NULLS LAST\n"
            "              Scan table=u source=fake columns=[i32]\n");
  // Under a DISTINCT the select list is the rows: it stays, and so does the Project its Sort reads.
  // A DISTINCT star expands, its columns being the rows: over every column of `ok`, and a column of
  // no engine type is then refused, as in any select list.
  EXPECT_EQ(optimized("SELECT i16 FROM t WHERE EXISTS (SELECT DISTINCT * FROM ok OFFSET 1)"),
            "Output: i16:SMALLINT\n"
            "Project i16\n"
            "  Filter \"count_star()\" = 1\n"
            "    Join ONE-ROW build=right keys=[]\n"
            "      Scan table=t source=fake columns=[i16]\n"
            "      Aggregate COUNT(*)\n"
            "        Limit 1\n"
            "          Limit ALL OFFSET 1\n"
            "            GroupAggregate keys=[i16, s]\n"
            "              Project i16, s\n"
            "                Scan table=ok source=fake columns=[i16, s]\n");
  {
    const auto refused =
        BindSql("SELECT i16 FROM t WHERE EXISTS (SELECT DISTINCT * FROM u)", catalog);
    ASSERT_FALSE(refused.ok());
    EXPECT_TRUE(refused.status().IsNotImplemented()) << refused.status().ToString();
    EXPECT_TRUE(BindSql("SELECT i16 FROM t WHERE EXISTS (SELECT * FROM u)", catalog).ok());
  }
  EXPECT_EQ(optimized("SELECT i16 FROM t WHERE EXISTS (SELECT DISTINCT i32 + 1 AS x FROM u ORDER "
                      "BY x)"),
            "Output: i16:SMALLINT\n"
            "Project i16\n"
            "  Filter \"count_star()\" = 1\n"
            "    Join ONE-ROW build=right keys=[]\n"
            "      Scan table=t source=fake columns=[i16]\n"
            "      Aggregate COUNT(*)\n"
            "        Limit 1\n"
            "          Sort x ASC NULLS LAST\n"
            "            GroupAggregate keys=[x]\n"
            "              Project x\n"
            "                Compute (i32 + 1)\n"
            "                  Scan table=u source=fake columns=[i32]\n");
  EXPECT_EQ(optimized("SELECT i16, COUNT(*) FROM t GROUP BY i16 HAVING EXISTS (SELECT i16 FROM u "
                      "GROUP BY i16 HAVING COUNT(*) > 1)"),
            "Output: i16:SMALLINT count_star():BIGINT\n"
            "Project i16, \"count_star()\"\n"
            "  Filter \"count_star()\" = 1\n"
            "    Join ONE-ROW build=right keys=[]\n"
            "      GroupAggregate keys=[i16] COUNT(*)\n"
            "        Scan table=t source=fake columns=[i16]\n"
            "      Aggregate COUNT(*)\n"
            "        Limit 1\n"
            "          Filter \"count_star()\" > 1\n"
            "            GroupAggregate keys=[i16] COUNT(*)\n"
            "              Scan table=u source=fake columns=[i16]\n");
  EXPECT_EQ(optimized("SELECT t.i16 FROM t JOIN ok ON t.i16 = ok.i16 AND NOT EXISTS (SELECT 1 "
                      "FROM u)"),
            "Output: i16:SMALLINT\n"
            "Project t.i16\n"
            "  Filter \"count_star()\" = 0\n"
            "    Join ONE-ROW build=right keys=[] carries 1 of 3 unread above\n"
            "      Join INNER build=right keys=[t.i16 = ok.i16] carries 1 of 2 unread above\n"
            "        Scan table=t source=fake columns=[i16]\n"
            "        Scan table=ok source=fake columns=[i16]\n"
            "      Aggregate COUNT(*)\n"
            "        Limit 1\n"
            "          Scan table=u source=fake columns=[]\n");
  // A set operation as its query (ADR 0036's X6): its arms bound and typed as any set operation's.
  EXPECT_EQ(optimized("SELECT i16 FROM t WHERE EXISTS (SELECT i16 FROM u UNION SELECT i32 FROM u)"),
            "Output: i16:SMALLINT\n"
            "Project i16\n"
            "  Filter \"count_star()\" = 1\n"
            "    Join ONE-ROW build=right keys=[]\n"
            "      Scan table=t source=fake columns=[i16]\n"
            "      Aggregate COUNT(*)\n"
            "        Limit 1\n"
            "          SetOperation inputs=2 formula=(0 UNION 1) columns=[i16]\n"
            "            Compute CAST(i16 AS INTEGER)\n"
            "              Project i16\n"
            "                Scan table=u source=fake columns=[i16]\n"
            "            Project i32\n"
            "              Scan table=u source=fake columns=[i32]\n");
}

// A BETWEEN with a subquery bound is its two comparisons (ADR 0041's decision 6), each a conjunct
// of its clause: the bounds' one-row joins come first, planted where the BETWEEN is typed, then a
// subquery's comparison filters above them (in WHERE a filter of the slot, computing its operand
// there, ADR 0041's SQ1), and the other is an ordinary conjunct -- a filter below the joins, in
// WHERE and HAVING alike. Both compare in the type DuckDB
// gives all three values, a subquery bound's being its column's: with a DOUBLE bound, the plain
// half keeps `i32 + 1` as written (DuckDB keeps the cast to DOUBLE above it), where an INTEGER
// bound lets the constant move.
TEST(BinderTest, ABetweenWithASubqueryBoundIsItsTwoComparisons) {
  const Catalog catalog = MakeCatalog();
  const auto optimized = [&catalog](std::string_view sql) -> std::string {
    auto plan = BindSql(sql, catalog);
    EXPECT_TRUE(plan.ok()) << sql << ": " << plan.status().ToString();
    if (!plan.ok()) {
      return "";
    }
    EXPECT_EQ(PositionMismatch(*plan), std::nullopt) << sql << "\n" << Explain(*plan);
    return Explain(Optimized(*plan));
  };
  EXPECT_EQ(optimized("SELECT i16 FROM t WHERE i32 BETWEEN (SELECT MIN(i32) FROM u) AND (SELECT "
                      "i32 FROM u LIMIT 1)"),
            "Output: i16:SMALLINT\n"
            "Project i16\n"
            "  Filter i32 <= i32\n"
            "    Filter i32 >= \"min(i32)\"\n"
            "      Join ONE-ROW build=right keys=[]\n"
            "        Join ONE-ROW build=right keys=[]\n"
            "          Scan table=t source=fake columns=[i16, i32]\n"
            "          Aggregate MIN(i32)\n"
            "            Scan table=u source=fake columns=[i32]  [shared #1 of 2, 1 field]\n"
            "        Aggregate SINGLE_VALUE(i32)\n"
            "          Project i32\n"
            "            Limit 1\n"
            "              Scan table=u source=fake columns=[i32]  [shared #1 of 2, 1 field]\n");
  EXPECT_EQ(optimized("SELECT i16 FROM t WHERE dt BETWEEN (SELECT MIN(dt) FROM u) AND "
                      "'2020-01-01'"),
            "Output: i16:SMALLINT\n"
            "Project i16\n"
            "  Filter dt >= \"min(dt)\"\n"
            "    Join ONE-ROW build=right keys=[]\n"
            "      Filter dt <= DATE '2020-01-01'\n"
            "        Scan table=t source=fake columns=[i16, dt]\n"
            "      Aggregate MIN(dt)\n"
            "        Scan table=u source=fake columns=[dt]\n");
  EXPECT_EQ(optimized("SELECT i16 FROM t WHERE i32 + 1 BETWEEN (SELECT MIN(d) FROM u) AND 100"),
            "Output: i16:SMALLINT\n"
            "Project i16\n"
            "  Filter \"(i32 + 1)\" >= \"min(d)\"\n"
            "    Compute (i32 + 1)\n"
            "      Join ONE-ROW build=right keys=[] carries 1 of 4 unread above\n"
            "        Filter \"(i32 + 1)\" <= 100\n"
            "          Compute (i32 + 1)\n"
            "            Scan table=t source=fake columns=[i16, i32]\n"
            "        Aggregate MIN(d)\n"
            "          Scan table=u source=fake columns=[d]\n");
  EXPECT_EQ(optimized("SELECT i16 FROM t WHERE i32 + 1 BETWEEN (SELECT MIN(i32) FROM u) AND 100"),
            "Output: i16:SMALLINT\n"
            "Project i16\n"
            "  Filter \"(i32 + 1)\" >= \"min(i32)\"\n"
            "    Compute (i32 + 1)\n"
            "      Join ONE-ROW build=right keys=[]\n"
            "        Filter i32 <= 99\n"
            "          Scan table=t source=fake columns=[i16, i32]\n"
            "        Aggregate MIN(i32)\n"
            "          Scan table=u source=fake columns=[i32]\n");
  EXPECT_EQ(optimized("SELECT i16, COUNT(*) FROM t GROUP BY i16 HAVING COUNT(*) BETWEEN (SELECT "
                      "MIN(i16) FROM u) AND 10"),
            "Output: i16:SMALLINT count_star():BIGINT\n"
            "Project i16, \"count_star()\"\n"
            "  Filter \"count_star()\" >= \"min(i16)\"\n"
            "    Join ONE-ROW build=right keys=[]\n"
            "      Filter \"count_star()\" <= 10\n"
            "        GroupAggregate keys=[i16] COUNT(*)\n"
            "          Scan table=t source=fake columns=[i16]\n"
            "      Aggregate MIN(i16)\n"
            "        Scan table=u source=fake columns=[i16]\n");
}

// A correlated scalar subquery binds to a DependentJoin (ADR 0023's U1), which the unnesting pass
// then rewrites (Path 2, U3).
TEST(BinderTest, ACorrelatedScalarSubqueryBindsToADependentJoin) {
  const auto catalog = testing::MakeCatalog();
  auto plan = BindSql(
      "SELECT i16 FROM t WHERE i32 = (SELECT MIN(u.i32) FROM u WHERE u.i64 = t.i64)", catalog);
  ASSERT_TRUE(plan.ok()) << plan.status().ToString();

  // The whole plan resolves: the sub-plan reads t.i64, which only the dependent join's left input
  // defines, and the Resolver's binding frame is what makes that legal (no abort, no mismatch).
  EXPECT_EQ(PositionMismatch(*plan), std::nullopt) << Explain(*plan);

  const DependentJoinNode* dependent = nullptr;
  const auto find = [&](auto&& self, const LogicalNodePtr& node) -> void {
    if (node == nullptr || dependent != nullptr) {
      return;
    }
    if (const auto* d = std::get_if<DependentJoinNode>(node.get())) {
      dependent = d;
      return;
    }
    for (const LogicalNodePtr& input : InputsOf(*node)) {
      self(self, input);
    }
  };
  find(find, plan->root);
  ASSERT_NE(dependent, nullptr) << Explain(*plan);
  // The whole shape ADR 0023's Binding (U1) asks for: the comparison above the dependent join, the
  // correlated conjunct in ONE filter at the top of the subquery's block -- above its joins and
  // below its aggregation -- where the unnesting pass finds it, and no part of it pushed into the
  // scan, which would read a column the scan does not have.
  EXPECT_EQ(Explain(*plan),
            "Output: i16:SMALLINT\n"
            "Project i16\n"
            "  Filter i32 = \"min(u.i32)\"\n"
            "    DependentJoin ONE-ROW\n"
            "      Scan table=t source=fake columns=[i16, i32, i64, u16, h, d, s, dt, bad, \"Mixed "
            "Case\", from]\n"
            "      Aggregate MIN(i32)\n"
            "        Filter \"(u.i64 = t.i64)\"\n"
            "          Compute (u.i64 = t.i64)\n"
            "            Scan table=u source=fake columns=[i16, i32, i64, u16, h, d, s, dt, bad, "
            "\"Mixed Case\", from]\n");
  EXPECT_EQ(dependent->kind, JoinKind::kOneRow) << "a scalar subquery appends its single value";
  EXPECT_EQ(dependent->mark, kNoColumnId) << "no kind defines a marker yet";
  // Its right input is the correlated one, and its left input defines what that reads.
  EXPECT_FALSE(FreeVariables(*dependent->right).empty());
  EXPECT_TRUE(FreeVariables(*plan->root).empty()) << "the left input binds every free variable";

  // Path 2 (roadmap id U3) unnests it: the aggregation groups by the inner equivalent and the
  // dependent join becomes an inner join keyed on it.
  const auto optimized = Optimize(*plan);
  ASSERT_TRUE(optimized.ok()) << optimized.status().ToString();
  EXPECT_EQ(Explain(*optimized),
            "Output: i16:SMALLINT\n"
            "Project i16\n"
            "  Filter i32 = \"min(u.i32)\"\n"
            // The join carries t.i64 and u.i64 -- its own keys -- past itself, which nothing
            // above reads: the Filter reads i32 and the min, the Project i16.
            "    Join INNER build=right keys=[\"t.i64\" = \"u.i64\"] carries 2 of 5 unread above\n"
            "      Scan table=t source=fake columns=[i16, i32, i64]\n"
            "      GroupAggregate keys=[\"u.i64\"] MIN(i32)\n"
            "        Scan table=u source=fake columns=[i32, i64]\n");

  // An uncorrelated scalar subquery keeps ADR 0022's direct one-row join.
  auto plain = BindSql("SELECT i16 FROM t WHERE i32 = (SELECT MIN(u.i32) FROM u)", catalog);
  ASSERT_TRUE(plain.ok()) << plain.status().ToString();
  const DependentJoinNode* none = nullptr;
  std::swap(dependent, none);
  find(find, plain->root);
  EXPECT_EQ(dependent, nullptr) << Explain(*plain);
  EXPECT_TRUE(Optimize(*plain).ok());
}

// Path 2 joins the outer side to the grouped subquery with an inner join, which gives an outer row
// with no group no row: right only where the subquery's value is NULL over no rows, so that the
// comparison above rejects that row too. A select item that reads no aggregate call is not: a
// constant over an aggregation that an ORDER BY aggregate makes one row (its call is then dropped)
// is that constant over no rows. Refused, with the strict combinations kept answered.
TEST(BinderTest, ACorrelatedScalarValueMustBeNullOverNoRows) {
  const auto catalog = testing::MakeCatalog();
  const auto optimize = [&catalog](std::string_view sql) {
    auto plan = BindSql(sql, catalog);
    EXPECT_TRUE(plan.ok()) << sql << ": " << plan.status().ToString();
    return plan.ok() ? Optimize(*plan) : arrow::Result<LogicalPlan>(plan.status());
  };
  for (const std::string_view sql : {
           "SELECT i16 FROM t WHERE i32 = (SELECT 1 FROM u WHERE u.i64 = t.i64 ORDER BY COUNT(*))",
           "SELECT i16 FROM t WHERE i32 = (SELECT 1 FROM u WHERE u.i64 = t.i64 ORDER BY "
           "MAX(u.i32))",
           "SELECT i16 FROM t WHERE i32 = (SELECT 1 + 0 FROM u WHERE u.i64 = t.i64 ORDER BY "
           "MIN(u.i32))",
       }) {
    const auto optimized = optimize(sql);
    ASSERT_FALSE(optimized.ok()) << sql;
    const auto detail = GetSqlError(optimized.status());
    ASSERT_NE(detail, nullptr) << sql << ": " << optimized.status().ToString();
    EXPECT_EQ(detail->kind(), SqlErrorDetail::Kind::kUnsupported) << sql;
    EXPECT_EQ(optimized.status().message(),
              "a correlated scalar subquery whose select item does not combine its aggregate "
              "strictly is not supported")
        << sql;
  }
  for (const std::string_view sql : {
           "SELECT i16 FROM t WHERE i32 = (SELECT MIN(u.i32) FROM u WHERE u.i64 = t.i64)",
           "SELECT i16 FROM t WHERE i32 < (SELECT MAX(u.i32) + 1 FROM u WHERE u.i64 = t.i64)",
           "SELECT i16 FROM t WHERE i32 < (SELECT -MIN(u.i32) * 2 FROM u WHERE u.i64 = t.i64 ORDER "
           "BY MAX(u.i32))",
           "SELECT i16 FROM t WHERE i64 < (SELECT CAST(AVG(u.i32) AS BIGINT) FROM u WHERE u.i64 = "
           "t.i64)",
       }) {
    const auto optimized = optimize(sql);
    EXPECT_TRUE(optimized.ok()) << sql << ": " << optimized.status().ToString();
  }
}

// DuckDB's sum rewriter (SUM(x + c) as SUM(x) + c * COUNT(x)) runs after DuckDB's unnesting, over
// aggregations without groups, so it never rewrites a correlated subquery's aggregation, which has
// gained the correlation's groups by then. The binder leaves it alone too: a correlated SUM(x + c)
// stays a SUM, which Path 2 answers, where its COUNT made the query exit 4. Uncorrelated, it is
// still rewritten.
TEST(BinderTest, ACorrelatedSumOfAnAdditionIsNotRewritten) {
  const auto catalog = testing::MakeCatalog();
  auto correlated = BindSql(
      "SELECT i16 FROM t WHERE i64 < (SELECT SUM(u.i32 + 1) FROM u WHERE u.i64 = t.i64)", catalog);
  ASSERT_TRUE(correlated.ok()) << correlated.status().ToString();
  const auto optimized = Optimize(*correlated);
  ASSERT_TRUE(optimized.ok()) << optimized.status().ToString();
  const std::string text = Explain(*optimized);
  EXPECT_TRUE(text.contains("GroupAggregate keys=[\"u.i64\"] SUM(")) << text;
  EXPECT_FALSE(text.contains("COUNT(")) << text;

  auto plain = BindSql("SELECT i16 FROM t WHERE i64 < (SELECT SUM(u.i32 + 1) FROM u)", catalog);
  ASSERT_TRUE(plain.ok()) << plain.status().ToString();
  EXPECT_TRUE(Explain(*plain).contains("COUNT(")) << Explain(*plain);

  // Correlated through an inner ON, and through a name of that ON that the later d2 has too: the
  // ON does not see d2 (rule 10), so `i` is the outer dec.i there, and the block is correlated.
  for (const std::string_view sql :
       {"SELECT p FROM dec WHERE b < (SELECT SUM(u.i32 + 1) FROM u JOIN ok ON ok.i16 = u.i16 AND "
        "u.i32 = dec.i)",
        "SELECT p FROM dec WHERE b < (SELECT SUM(u.i32 + 1) FROM u JOIN ok ON ok.i16 = u.i16 AND "
        "u.i32 = i JOIN dec AS d2 ON d2.s16 = u.i16)"}) {
    auto on = BindSql(sql, catalog);
    ASSERT_TRUE(on.ok()) << sql << ": " << on.status().ToString();
    const auto unnested = Optimize(*on);
    ASSERT_TRUE(unnested.ok()) << sql << ": " << unnested.status().ToString();
    EXPECT_FALSE(Explain(*unnested).contains("COUNT(")) << Explain(*unnested);
  }
}

// A subquery in an inner join's ON sees its enclosing block as the ON does (rule 10): the FROM
// items up to that JOIN. A name only a later item has is no outer reference (DuckDB: "Referenced
// table not found"), an unqualified name that a later item also has is not ambiguous, and a name
// that a later item hides from an enclosing block further out is that block's -- which exits 4,
// being two blocks out, where it once read the later item's column.
TEST(BinderTest, ASubqueryInAnOnSeesTheItemsTheOnSees) {
  const auto catalog = testing::MakeCatalog();
  const auto error_of = [&catalog](std::string_view sql) {
    auto plan = BindSql(sql, catalog);
    EXPECT_FALSE(plan.ok()) << sql;
    const auto detail = GetSqlError(plan.status());
    EXPECT_NE(detail, nullptr) << sql << ": " << plan.status().ToString();
    return detail == nullptr ? SqlErrorDetail::Kind::kParse : detail->kind();
  };
  EXPECT_EQ(error_of("SELECT t.i16 FROM t JOIN ok ON t.i16 = ok.i16 AND EXISTS (SELECT * FROM dec "
                     "WHERE dec.i = x.i32) JOIN u AS x ON x.i16 = t.i16"),
            SqlErrorDetail::Kind::kBind);
  EXPECT_EQ(error_of("SELECT x.i16 FROM u AS x WHERE EXISTS (SELECT * FROM t JOIN ok ON t.i16 = "
                     "ok.i16 AND EXISTS (SELECT * FROM dec WHERE dec.i = x.i32) JOIN u AS x ON "
                     "x.i16 = t.i16)"),
            SqlErrorDetail::Kind::kUnsupported);

  // i32 is t's and u's; the ON of t JOIN ok sees only t's.
  auto plan = BindSql(
      "SELECT t.i16 FROM t JOIN ok ON t.i16 = ok.i16 AND EXISTS (SELECT * FROM dec WHERE dec.i = "
      "i32) JOIN u ON u.i16 = t.i16",
      catalog);
  ASSERT_TRUE(plan.ok()) << plan.status().ToString();
  const auto optimized = Optimize(*plan);
  ASSERT_TRUE(optimized.ok()) << optimized.status().ToString();
  // The semi join's key reads t's i32: t's scan reads it, u's does not.
  const std::string text = Explain(*optimized);
  EXPECT_TRUE(text.contains("Join SEMI build=right keys=[i32 = \"dec.i\"]")) << text;
  EXPECT_TRUE(text.contains("Scan table=t source=fake columns=[i16, i32]\n")) << text;
  EXPECT_TRUE(text.contains("Scan table=u source=fake columns=[i16]\n")) << text;
}

// A semi join over one relation runs on that relation's own branch rather than above every join of
// its block, so the joins above carry the rows it kept instead of all of them (ADR 0029's D0). It
// answers the same either way: a joined row matches exactly when its row from that relation does.
// ADR 0022 makes "an outer, semi, anti or one-row join a fixed unit for ordering", so each
// condition that holds one back is pinned here beside the one case that moves.
TEST(BinderTest, ASemiJoinOverOneRelationRunsOnItsBranch) {
  const Catalog catalog = MakeCatalog();
  const auto plan_of = [&catalog](std::string_view sql) {
    auto plan = BindSql(sql, catalog);
    EXPECT_TRUE(plan.ok()) << sql << ": " << plan.status().ToString();
    return plan.ok() ? Explain(Optimized(*plan)) : std::string();
  };
  // `ok` holds 7 rows, fewer than the join above it is estimated to output, so the semi join moves
  // onto its scan and the INNER join probes what the semi join kept.
  EXPECT_EQ(
      plan_of("SELECT COUNT(*) FROM t, ok WHERE t.i16 = ok.i16 AND ok.i16 IN (SELECT i16 FROM t)"),
      // Both scans of `t` carry E1's note: the query reads it twice, once in the FROM and once in
      // the subquery, and they read the same field (docs/adr/0030-shared-sub-plans.md).
      "Output: count_star():BIGINT\n"
      "Aggregate COUNT(*)\n"
      "  Join INNER build=right keys=[t.i16 = ok.i16]\n"
      "    Scan table=t source=fake columns=[i16]  [shared #1 of 2, 1 field]\n"
      R"(    Join SEMI build=right keys=["ok.i16" = i16])"
      "\n"
      "      Scan table=ok source=fake columns=[i16]\n"
      "      Project i16\n"
      "        Scan table=t source=fake columns=[i16]  [shared #1 of 2, 1 field]\n");
  // A subquery join above every join of its block renders before the INNER join; one on a branch
  // renders after it.
  const auto on_top = [&plan_of](std::string_view sql) {
    const std::string plan = plan_of(sql);
    const std::size_t join = std::min(plan.find("Join SEMI"), plan.find("Join NULL-AWARE ANTI"));
    const std::size_t inner = plan.find("Join INNER");
    EXPECT_NE(join, std::string::npos) << plan;
    EXPECT_NE(inner, std::string::npos) << plan;
    return join < inner;
  };
  struct Case {
    std::string_view sql;
    bool on_top;  // whether it stays above every join of its block, as before D0
    std::string_view why;
  };
  for (const Case& c : {
           Case{.sql = "SELECT COUNT(*) FROM t, ok WHERE t.i16 = ok.i16 AND ok.i16 IN (SELECT "
                       "i16 FROM t)",
                .on_top = false,
                .why = "7 rows under a join estimated to output more"},
           // The guard against moving it onto more rows than it would see above: `t` holds 100,
           // and this join of 100 rows and 7 outputs fewer than either, because `t.i16` declares a
           // domain of 100 (plan_test_util.h). That the domain is DECLARED is the point of the
           // case: an estimator that stands a relation's rows in for an unknown domain reaches the
           // same number here by accident, and this guard then rests on nothing.
           Case{.sql = "SELECT COUNT(*) FROM t, ok WHERE t.i16 = ok.i16 AND t.i32 IN (SELECT "
                       "i16 FROM ok)",
                .on_top = true,
                .why = "100 rows under a join estimated to output fewer"},
           // The same relation that moves above, with NOT IN instead, so the kind is all that
           // differs: a null-aware anti join reads its whole set to answer one row.
           Case{.sql = "SELECT COUNT(*) FROM t, ok WHERE t.i16 = ok.i16 AND ok.i16 NOT IN "
                       "(SELECT i16 FROM t)",
                .on_top = true,
                .why = "a null-aware anti join is not a filter on one row"},
           // `ok`'s 7 rows are known but `u`'s are not, which saturates the root's estimate --
           // and against a saturated root every relation reads as small. This is the shape the
           // comparison would otherwise admit however selective the joins below it are.
           Case{.sql = "SELECT COUNT(*) FROM ok, u WHERE ok.i16 = u.i16 AND ok.i16 IN (SELECT "
                       "i16 FROM t)",
                .on_top = true,
                .why = "another relation of the block has no row count"},
           // The moved relation's own count unknown, which must never read as small either.
           Case{.sql = "SELECT COUNT(*) FROM t, u WHERE t.i16 = u.i16 AND u.i32 IN (SELECT i16 "
                       "FROM ok)",
                .on_top = true,
                .why = "an unknown row count never reads as small"},
           // The probe needs widening to the key type, so the join's key is a column computed in
           // its own layer, which belongs to no binding and names no relation to move to.
           Case{.sql = "SELECT COUNT(*) FROM t, ok WHERE t.i16 = ok.i16 AND ok.i16 IN (SELECT "
                       "i64 FROM t)",
                .on_top = true,
                .why = "its probe is widened in the join's own layer, below it"},
           // The key belongs to a binding inside a LEFT JOIN unit, whose branch is a join of its
           // own and whose rows pad.
           Case{.sql = "SELECT COUNT(*) FROM t LEFT JOIN ok ON t.i16 = ok.i16, dec WHERE "
                       "t.i32 = dec.i AND ok.i16 IN (SELECT i16 FROM ok)",
                .on_top = true,
                .why = "a LEFT JOIN unit's branch is a join of its own"},
       }) {
    EXPECT_EQ(on_top(c.sql), c.on_top) << c.why << ": " << c.sql;
  }
  // Two of them on the one relation: both move, stacked on its branch in the order written.
  EXPECT_EQ(plan_of("SELECT COUNT(*) FROM t, ok WHERE t.i16 = ok.i16 AND ok.i16 IN (SELECT i16 "
                    "FROM t) AND ok.i16 IN (SELECT s16 FROM dec)"),
            "Output: count_star():BIGINT\n"
            "Aggregate COUNT(*)\n"
            "  Join INNER build=right keys=[t.i16 = ok.i16]\n"
            "    Scan table=t source=fake columns=[i16]  [shared #1 of 2, 1 field]\n"
            R"(    Join SEMI build=right keys=["ok.i16" = s16])"
            "\n"
            R"(      Join SEMI build=right keys=["ok.i16" = i16])"
            "\n"
            "        Scan table=ok source=fake columns=[i16]\n"
            "        Project i16\n"
            "          Scan table=t source=fake columns=[i16]  [shared #1 of 2, 1 field]\n"
            "      Project s16\n"
            "        Scan table=dec source=fake columns=[s16]\n");
}

// ---- ADR 0041's SQ7a: IN and NOT IN inside a condition, through a null-aware mark join ----

std::size_t CountOf(std::string_view text, std::string_view what) {
  std::size_t n = 0;
  for (std::size_t at = text.find(what); at != std::string_view::npos;
       at = text.find(what, at + 1)) {
    ++n;
  }
  return n;
}

// Every clause and block an IN under OR, NOT or CASE may stand in binds to a plan whose ids
// resolve, which the optimizer then takes; each plants exactly one mark. A route the gate opens
// without an arm in the binder aborts here (RejectedSubquery), not in a user's query.
TEST(BinderTest, InMarkPlacements) {
  const Catalog catalog = MakeCatalog();
  for (const char* sql : {
           // WHERE, under OR, NOT and CASE (NOTs over the whole conjunct fold into a semi or anti
           // join instead: NotsOverAnInConjunctFoldIntoItsKind).
           "SELECT i16 FROM t WHERE i16 < 10 OR i32 IN (SELECT i16 FROM u)",
           "SELECT i16 FROM t WHERE i16 < 10 OR i32 NOT IN (SELECT i16 FROM u)",
           "SELECT i16 FROM t WHERE NOT (i32 NOT IN (SELECT i16 FROM u) AND i16 < 0)",
           "SELECT i16 FROM t WHERE CASE WHEN i32 + 1 IN (SELECT i64 FROM u) THEN 1 ELSE 0 END = 0",
           // An inner ON, inside a FULL JOIN's left input too, and a keyless one.
           "SELECT t.i16 FROM t JOIN u ON t.i16 = u.i16 AND (t.i32 IN (SELECT i16 FROM ok) OR "
           "u.i32 < 0)",
           "SELECT t.i16 FROM t JOIN u ON t.i16 = u.i16 AND (t.i32 IN (SELECT i16 FROM ok) OR "
           "u.i32 < 0) FULL JOIN dec ON u.i32 = dec.i",
           "SELECT t.i16 FROM t JOIN u ON t.i32 IN (SELECT i16 FROM ok) OR u.i32 < 0",
           // HAVING, its probe an aggregate or a key.
           "SELECT i16 FROM t GROUP BY i16 HAVING COUNT(*) IN (SELECT i64 FROM u) OR i16 < 0",
           "SELECT i16 FROM t GROUP BY i16 HAVING NOT (i16 IN (SELECT i16 FROM u)) OR MAX(i32) > 1",
           // A select item, ORDER BY, GROUP BY key and aggregate argument, in each shape.
           "SELECT CASE WHEN i16 IN (SELECT i16 FROM u) THEN 1 ELSE 0 END FROM t",
           "SELECT i16 FROM t ORDER BY CASE WHEN i32 NOT IN (SELECT i16 FROM u) THEN 0 ELSE 1 END",
           "SELECT CASE WHEN i16 IN (SELECT i16 FROM u) THEN 1 ELSE 0 END AS k, COUNT(*) FROM t "
           "GROUP BY k",
           "SELECT SUM(CASE WHEN i32 IN (SELECT i16 FROM u) THEN 1 ELSE 0 END) FROM t",
           "SELECT i16, SUM(CASE WHEN i32 IN (SELECT i16 FROM u) THEN i64 END) FROM t GROUP BY i16",
           "SELECT i16, CASE WHEN MAX(i32) IN (SELECT i16 FROM u) THEN 1 ELSE 0 END FROM t GROUP "
           "BY i16",
           "SELECT i16 FROM t GROUP BY i16 HAVING SUM(CASE WHEN i32 IN (SELECT i16 FROM u) THEN 1 "
           "ELSE 0 END) > 1",
           "SELECT i16 FROM t GROUP BY i16 ORDER BY CASE WHEN i16 IN (SELECT i16 FROM u) THEN 0 "
           "ELSE 1 END, i16",
           // HAVING over the alias of an item whose mark waits in the slot's tail: moved first.
           "SELECT i16, CASE WHEN MAX(i32) IN (SELECT i16 FROM u) THEN 1 ELSE 0 END AS k FROM t "
           "GROUP BY i16 HAVING k = 0",
           // Inside a derived table, a CTE, and the blocks of an IN, a scalar and an EXISTS.
           "SELECT x FROM (SELECT i16 AS x FROM t WHERE i32 IN (SELECT i16 FROM u) OR i16 < 0) AS "
           "d",
           "WITH c AS (SELECT i16 FROM t WHERE i32 IN (SELECT i16 FROM u) OR i16 < 0) SELECT i16 "
           "FROM c",
           "SELECT i16 FROM t WHERE i16 IN (SELECT i16 FROM u WHERE i32 IN (SELECT i16 FROM ok) OR "
           "i64 < 0)",
           "SELECT i16 FROM t WHERE i32 > (SELECT MIN(i16) FROM u WHERE i32 IN (SELECT i16 FROM "
           "ok) OR i64 < 0)",
           "SELECT i16 FROM t WHERE EXISTS (SELECT 1 FROM u WHERE i32 IN (SELECT i16 FROM ok) OR "
           "i64 < 0)",
           // A mark on the outer side of a correlated subquery, which ADR 0023's Path 2 clones for
           // its semi-join reduction (fresh ids, the mark's among them), and Path 1 keeps below.
           "SELECT i16 FROM t WHERE (i16 < 0 OR i32 IN (SELECT i16 FROM ok)) AND i32 > (SELECT "
           "MIN(u.i16) FROM u WHERE u.i64 = t.i64)",
           "SELECT i16 FROM t WHERE (i16 < 0 OR i32 IN (SELECT i16 FROM ok)) AND EXISTS (SELECT 1 "
           "FROM u WHERE u.i64 = t.i64)",
           // A correlated block's conjunct whose mark's probe is inner: a Path 1 or 2 residual.
           "SELECT i16 FROM t WHERE EXISTS (SELECT 1 FROM u WHERE u.i16 = t.i16 AND (u.i32 IN "
           "(SELECT i16 FROM ok) OR t.i64 < 0))",
           "SELECT i16 FROM t WHERE i32 > (SELECT MIN(u.i16) FROM u WHERE u.i64 = t.i64 AND (u.i32 "
           "IN (SELECT i16 FROM ok) OR u.i16 < 0))",
       }) {
    auto plan = BindSql(sql, catalog);
    ASSERT_TRUE(plan.ok()) << sql << ": " << plan.status().ToString();
    EXPECT_EQ(PositionMismatch(*plan), std::nullopt) << sql << "\n" << Explain(*plan);
    EXPECT_EQ(CountOf(Explain(*plan), "Join NULL-AWARE MARK"), 1U) << sql << "\n" << Explain(*plan);
    const LogicalPlan optimized = Optimized(*plan);
    ASSERT_NE(optimized.root, nullptr) << sql;
    EXPECT_EQ(PositionMismatch(optimized), std::nullopt) << sql << "\n" << Explain(optimized);
  }
}

// The mark join keeps each row of its left input and appends the mark, which the OR's filter
// reads; its probe is widened to the key type in its own layer, below it, as a semi join's is.
TEST(BinderTest, AnInUnderOrIsAMarkJoin) {
  const Catalog catalog = MakeCatalog();
  auto plan = BindSql("SELECT i16 FROM t WHERE i16 < 10 OR i16 IN (SELECT i64 FROM u)", catalog);
  ASSERT_TRUE(plan.ok()) << plan.status().ToString();
  EXPECT_EQ(Explain(Optimized(*plan)),
            "Output: i16:SMALLINT\n"
            "Project i16\n"
            "  Filter \"((i16 < 10) OR i16 IN (SELECT i64 FROM u))\"\n"
            "    Compute ((i16 < 10) OR i16 IN (SELECT i64 FROM u))\n"
            "      Join NULL-AWARE MARK build=right keys=[\"CAST(i16 AS BIGINT)\" = i64] carries 1 "
            "of 3 unread above\n"
            "        Compute CAST(i16 AS BIGINT)\n"
            "          Scan table=t source=fake columns=[i16]\n"
            "        Project i64\n"
            "          Scan table=u source=fake columns=[i64]\n");
}

// One slot binds one mark per set and probe: IN and NOT IN share it, a select item reads a GROUP
// BY key's, HAVING and a select item over aggregates share one; WHERE and a grouped select item
// are two slots, and one set with two probes two marks. A probe spelled otherwise is one column,
// and so one mark.
TEST(BinderTest, InAndNotInShareOneMark) {
  const Catalog catalog = MakeCatalog();
  const auto marks = [&catalog](std::string_view sql) {
    auto plan = BindSql(sql, catalog);
    EXPECT_TRUE(plan.ok()) << sql << ": " << plan.status().ToString();
    if (!plan.ok()) {
      return std::size_t{0};
    }
    EXPECT_EQ(PositionMismatch(*plan), std::nullopt) << sql << "\n" << Explain(*plan);
    return CountOf(Explain(*plan), "Join NULL-AWARE MARK");
  };
  EXPECT_EQ(marks("SELECT i16 FROM t WHERE (i32 IN (SELECT i16 FROM u) AND i16 > 0) OR (i32 NOT "
                  "IN (SELECT i16 FROM u) AND i16 < 0)"),
            1U);
  EXPECT_EQ(marks("SELECT i16 FROM t WHERE i32 IN (SELECT i16 FROM u) OR i16 IN (SELECT i16 FROM "
                  "u)"),
            2U);
  EXPECT_EQ(marks("SELECT CASE WHEN i16 IN (SELECT i16 FROM u) THEN 1 ELSE 0 END, COUNT(*) FROM t "
                  "GROUP BY CASE WHEN I16 IN (SELECT i16 FROM u) THEN 1 ELSE 0 END"),
            1U);
  EXPECT_EQ(marks("SELECT i16, CASE WHEN COUNT(*) IN (SELECT i64 FROM u) THEN 1 ELSE 0 END FROM t "
                  "GROUP BY i16 HAVING COUNT(*) NOT IN (SELECT i64 FROM u) OR i16 < 0"),
            1U);
  EXPECT_EQ(marks("SELECT i16, SUM(CASE WHEN i32 IN (SELECT i16 FROM u) THEN 1 ELSE 0 END) FROM t "
                  "WHERE i32 IN (SELECT i16 FROM u) OR i16 < 0 GROUP BY i16"),
            1U);
  EXPECT_EQ(marks("SELECT i16, CASE WHEN i16 IN (SELECT i16 FROM u) THEN 1 ELSE 0 END FROM t "
                  "WHERE i16 IN (SELECT i16 FROM u) OR i32 < 0 GROUP BY i16"),
            2U);
}

// An IN that every branch of an OR shares is factored out first (ADR 0022's OR factoring) and is
// then a top-level conjunct: a semi join, with no mark. A branch that the shared conjuncts alone
// make true is dropped, bound as a dry run that plants no mark but keeps its bind errors.
TEST(BinderTest, ASharedInUnderOrIsASemiJoin) {
  const Catalog catalog = MakeCatalog();
  const auto optimized = [&catalog](std::string_view sql) -> std::string {
    auto plan = BindSql(sql, catalog);
    EXPECT_TRUE(plan.ok()) << sql << ": " << plan.status().ToString();
    if (!plan.ok()) {
      return "";
    }
    EXPECT_EQ(PositionMismatch(*plan), std::nullopt) << sql << "\n" << Explain(*plan);
    return Explain(Optimized(*plan));
  };
  EXPECT_EQ(optimized("SELECT i16 FROM t WHERE (i16 > 0 AND i32 IN (SELECT i16 FROM u)) OR (i16 < "
                      "-5 AND i32 IN (SELECT i16 FROM u))"),
            "Output: i16:SMALLINT\n"
            "Project i16\n"
            "  Join SEMI build=right keys=[i32 = \"CAST(i16 AS INTEGER)\"] carries 2 of 3 unread "
            "above\n"
            "    Filter \"((i16 > 0) OR (i16 < -5))\"\n"
            "      Compute ((i16 > 0) OR (i16 < -5))\n"
            "        Scan table=t source=fake columns=[i16, i32]\n"
            "    Compute CAST(i16 AS INTEGER)\n"
            "      Project i16\n"
            "        Scan table=u source=fake columns=[i16]\n");
  EXPECT_EQ(
      optimized("SELECT i16 FROM t WHERE (i16 = 2 AND i32 IN (SELECT i16 FROM u)) OR i16 = 2"),
      "Output: i16:SMALLINT\n"
      "Project i16\n"
      "  Filter i16 = 2\n"
      "    Scan table=t source=fake columns=[i16]\n");
  constexpr std::string_view kDropped =
      "SELECT i16 FROM t WHERE (i16 = 2 AND i32 IN (SELECT nope FROM u)) OR i16 = 2";
  auto error = BindSql(kDropped, catalog);
  ASSERT_FALSE(error.ok());
  EXPECT_EQ(SpanText(kDropped, error.status()), "nope");
}

// NOTs over a top-level IN conjunct fold into its parity (ADR 0041's SQ7b): an even number keeps
// the IN a semi join, an odd one makes NOT IN a semi join and IN a null-aware anti join, in WHERE,
// an inner ON and HAVING -- no mark, which a NOT NOT over EXISTS still is.
TEST(BinderTest, NotsOverAnInConjunctFoldIntoItsKind) {
  const Catalog catalog = MakeCatalog();
  const auto plan_of = [&catalog](std::string_view sql) -> std::string {
    auto plan = BindSql(sql, catalog);
    EXPECT_TRUE(plan.ok()) << sql << ": " << plan.status().ToString();
    if (!plan.ok()) {
      return "";
    }
    EXPECT_EQ(PositionMismatch(*plan), std::nullopt) << sql << "\n" << Explain(*plan);
    return Explain(*plan);
  };
  struct Case {
    std::string_view sql;
    std::string_view kind;
  };
  for (const Case& c : {
           Case{.sql = "SELECT i16 FROM t WHERE NOT (i32 NOT IN (SELECT i16 FROM u))",
                .kind = "Join SEMI"},
           Case{.sql = "SELECT i16 FROM t WHERE NOT NOT (i32 IN (SELECT i16 FROM u))",
                .kind = "Join SEMI"},
           Case{.sql = "SELECT i16 FROM t WHERE NOT NOT NOT (i32 IN (SELECT i16 FROM u))",
                .kind = "Join NULL-AWARE ANTI"},
           Case{.sql = "SELECT i16 FROM t WHERE NOT NOT (i32 NOT IN (SELECT i16 FROM u))",
                .kind = "Join NULL-AWARE ANTI"},
           Case{.sql = "SELECT t.i16 FROM t JOIN ok ON t.i16 = ok.i16 AND NOT (t.i32 NOT IN "
                       "(SELECT i16 "
                       "FROM u))",
                .kind = "Join SEMI"},
           Case{.sql = "SELECT i16 FROM t GROUP BY i16 HAVING NOT (i16 NOT IN (SELECT i16 FROM u))",
                .kind = "Join SEMI"},
       }) {
    const std::string text = plan_of(c.sql);
    EXPECT_NE(text.find(c.kind), std::string::npos) << c.sql << "\n" << text;
    EXPECT_EQ(text.find("MARK"), std::string::npos) << c.sql << "\n" << text;
  }
  EXPECT_NE(plan_of("SELECT i16 FROM t WHERE NOT NOT EXISTS (SELECT 1 FROM u)").find("ONE-ROW"),
            std::string::npos);
}

// ADR 0041's SQ7b: an IN's probe is any operand, and its set any query whose arms read no enclosing
// block. A constant probe is a key computed in the join's own layer, so the semi join stays above
// the block's joins (no relation's branch holds its key); a string literal against a DATE set is
// that DATE; a set operation's set is its unified type, the probe widened to it.
TEST(BinderTest, InProbesAndSetsOfSq7b) {
  const Catalog catalog = MakeCatalog();
  const auto plan_of = [&catalog](std::string_view sql) -> std::string {
    auto plan = BindSql(sql, catalog);
    EXPECT_TRUE(plan.ok()) << sql << ": " << plan.status().ToString();
    if (!plan.ok()) {
      return "";
    }
    EXPECT_EQ(PositionMismatch(*plan), std::nullopt) << sql << "\n" << Explain(*plan);
    const LogicalPlan optimized = Optimized(*plan);
    EXPECT_EQ(PositionMismatch(optimized), std::nullopt) << sql << "\n" << Explain(optimized);
    return Explain(optimized);
  };
  EXPECT_EQ(plan_of("SELECT i16 FROM t WHERE 2 IN (SELECT i16 FROM u)"),
            "Output: i16:SMALLINT\n"
            "Project i16\n"
            "  Join SEMI build=right keys=[\"2\" = \"CAST(i16 AS INTEGER)\"] carries 1 of 2 unread "
            "above\n"
            "    Compute 2\n"
            "      Scan table=t source=fake columns=[i16]\n"
            "    Compute CAST(i16 AS INTEGER)\n"
            "      Project i16\n"
            "        Scan table=u source=fake columns=[i16]\n");
  EXPECT_EQ(plan_of("SELECT i16 FROM t WHERE '2027-01-05' IN (SELECT dt FROM u)"),
            "Output: i16:SMALLINT\n"
            "Project i16\n"
            "  Join SEMI build=right keys=[\"'2027-01-05'\" = dt] carries 1 of 2 unread above\n"
            "    Compute '2027-01-05'\n"
            "      Scan table=t source=fake columns=[i16]\n"
            "    Project dt\n"
            "      Scan table=u source=fake columns=[dt]\n");
  EXPECT_EQ(plan_of("SELECT i16 FROM t WHERE i16 IN (SELECT i16 FROM u UNION SELECT i64 FROM u)"),
            "Output: i16:SMALLINT\n"
            "Project i16\n"
            "  Join SEMI build=right keys=[\"CAST(i16 AS BIGINT)\" = i16] carries 1 of 2 unread "
            "above\n"
            "    Compute CAST(i16 AS BIGINT)\n"
            "      Scan table=t source=fake columns=[i16]\n"
            "    SetOperation inputs=2 formula=(0 UNION 1) columns=[i16]\n"
            "      Compute CAST(i16 AS BIGINT)\n"
            "        Project i16\n"
            "          Scan table=u source=fake columns=[i16]\n"
            "      Project i64\n"
            "        Scan table=u source=fake columns=[i64]\n");
  for (const char* sql : {
           "SELECT i16 FROM t WHERE i16 < 0 OR 2 NOT IN (SELECT i16 FROM u)",
           "SELECT i16 FROM t WHERE (SELECT MIN(i16) FROM ok) + i16 IN (SELECT i32 FROM u)",
           "SELECT i16 FROM t WHERE i16 < 0 OR (SELECT MIN(i16) FROM ok) IN (SELECT i32 FROM u)",
           "SELECT i16 FROM t WHERE i32 = ANY (SELECT i16 FROM u EXCEPT SELECT i16 FROM ok)",
           "SELECT i16 FROM t WHERE i16 != ALL (SELECT i16 FROM u INTERSECT SELECT i16 FROM ok)",
           "SELECT i16, CASE WHEN 's' IN (SELECT s FROM u) THEN 1 ELSE 0 END FROM t",
           "SELECT i16 FROM t GROUP BY i16 HAVING 3 IN (SELECT i16 FROM u) OR COUNT(*) > 1",
       }) {
    EXPECT_NE(plan_of(sql), "") << sql;
  }
}

// The select list binds before WHERE, and its expressions over aggregates before HAVING, but a mark
// it plants stands above their filters, in its slot's tail, so that it computes its probe only for
// the rows and groups they keep, as DuckDB does. A later clause that reads the same mark moves it
// to where that clause binds, which computes the probe there anyway.
TEST(BinderTest, ASelectListMarkStandsAboveTheFilters) {
  const Catalog catalog = MakeCatalog();
  const auto plan_of = [&catalog](std::string_view sql) -> std::string {
    auto plan = BindSql(sql, catalog);
    EXPECT_TRUE(plan.ok()) << sql << ": " << plan.status().ToString();
    if (!plan.ok()) {
      return "";
    }
    EXPECT_EQ(PositionMismatch(*plan), std::nullopt) << sql << "\n" << Explain(*plan);
    return Explain(*plan);
  };
  // The plans before the optimizer, whose pruning would only shorten the scans' lists.
  EXPECT_EQ(
      plan_of("SELECT SUM(CASE WHEN i32 + 1 IN (SELECT i16 FROM u) THEN 1 ELSE 0 END) FROM t "
              "WHERE i16 > (SELECT MIN(i16) FROM ok)"),
      "Output: sum(CASE  WHEN ((i32 + 1) IN (SELECT i16 FROM u)) THEN (1) ELSE 0 END):HUGEINT\n"
      "Aggregate SUM(\"CASE  WHEN ((i32 + 1) IN (SELECT i16 FROM u)) THEN (1) ELSE 0 END\")\n"
      "  Compute CASE  WHEN ((i32 + 1) IN (SELECT i16 FROM u)) THEN (1) ELSE 0 END\n"
      "    Join NULL-AWARE MARK build=right keys=[\"(i32 + 1)\" = \"CAST(i16 AS INTEGER)\"]\n"
      "      Compute (i32 + 1)\n"
      "        Filter i16 > \"min(i16)\"\n"
      "          Join ONE-ROW build=right keys=[]\n"
      "            Scan table=t source=fake columns=[i16, i32, i64, u16, h, d, s, dt, bad, \"Mixed "
      "Case\", from]\n"
      "            Aggregate MIN(i16)\n"
      "              Scan table=ok source=fake columns=[i16, s]\n"
      "      Compute CAST(i16 AS INTEGER)\n"
      "        Project i16\n"
      "          Scan table=u source=fake columns=[i16, i32, i64, u16, h, d, s, dt, bad, \"Mixed "
      "Case\", from]\n");
  EXPECT_EQ(
      plan_of("SELECT i16, CASE WHEN MAX(i32) + 1 IN (SELECT i16 FROM u) THEN 1 ELSE 0 END "
              "FROM t GROUP BY i16 HAVING i16 > (SELECT MIN(i16) FROM ok)"),
      "Output: i16:SMALLINT CASE  WHEN ((max(i32) + 1) IN (SELECT i16 FROM u)) THEN (1) ELSE 0 "
      "END:INTEGER\n"
      "Project i16, \"CASE  WHEN ((max(i32) + 1) IN (SELECT i16 FROM u)) THEN (1) ELSE 0 "
      "END\"\n"
      "  Compute CASE  WHEN ((max(i32) + 1) IN (SELECT i16 FROM u)) THEN (1) ELSE 0 END\n"
      "    Join NULL-AWARE MARK build=right keys=[\"(max(i32) + 1)\" = \"CAST(i16 AS "
      "INTEGER)\"]\n"
      "      Compute (max(i32) + 1)\n"
      "        Filter i16 > \"min(i16)\"\n"
      "          Join ONE-ROW build=right keys=[]\n"
      "            GroupAggregate keys=[i16] MAX(i32)\n"
      "              Scan table=t source=fake columns=[i16, i32, i64, u16, h, d, s, dt, bad, "
      "\"Mixed Case\", from]\n"
      "            Aggregate MIN(i16)\n"
      "              Scan table=ok source=fake columns=[i16, s]\n"
      "      Compute CAST(i16 AS INTEGER)\n"
      "        Project i16\n"
      "          Scan table=u source=fake columns=[i16, i32, i64, u16, h, d, s, dt, bad, \"Mixed "
      "Case\", from]\n");
  // WHERE reads the select list's mark: it stands where WHERE's conjunct binds, after the filter
  // of the conjunct written before it, and is one join.
  EXPECT_EQ(plan_of("SELECT SUM(CASE WHEN i32 IN (SELECT i16 FROM u) THEN 1 ELSE 0 END) FROM t "
                    "WHERE i16 > (SELECT MIN(i16) FROM ok) AND (i32 IN (SELECT i16 FROM u) OR "
                    "i64 < 0)"),
            "Output: sum(CASE  WHEN (i32 IN (SELECT i16 FROM u)) THEN (1) ELSE 0 END):HUGEINT\n"
            "Aggregate SUM(\"CASE  WHEN (i32 IN (SELECT i16 FROM u)) THEN (1) ELSE 0 END\")\n"
            "  Compute CASE  WHEN (i32 IN (SELECT i16 FROM u)) THEN (1) ELSE 0 END\n"
            "    Filter \"(i32 IN (SELECT i16 FROM u) OR (i64 < 0))\"\n"
            "      Compute (i32 IN (SELECT i16 FROM u) OR (i64 < 0))\n"
            "        Join NULL-AWARE MARK build=right keys=[i32 = \"CAST(i16 AS INTEGER)\"]\n"
            "          Filter i16 > \"min(i16)\"\n"
            "            Join ONE-ROW build=right keys=[]\n"
            "              Scan table=t source=fake columns=[i16, i32, i64, u16, h, d, s, dt, bad, "
            "\"Mixed Case\", from]\n"
            "              Aggregate MIN(i16)\n"
            "                Scan table=ok source=fake columns=[i16, s]\n"
            "          Compute CAST(i16 AS INTEGER)\n"
            "            Project i16\n"
            "              Scan table=u source=fake columns=[i16, i32, i64, u16, h, d, s, dt, bad, "
            "\"Mixed Case\", from]\n");
}

// A global block's one row is never sorted, so its ORDER BY items are dropped, but a mark one of
// them plants reads its probe's aggregate, which stays: the generator found
// it dropped with the item, and the plan's ids did not resolve (an abort).
TEST(BinderTest, AGlobalBlocksOrderByMarkKeepsItsProbesAggregate) {
  const Catalog catalog = MakeCatalog();
  for (const char* sql : {
           "SELECT COUNT(*) FROM t ORDER BY CASE WHEN MIN(i16) IN (SELECT i16 FROM u) THEN 1 ELSE "
           "0 "
           "END",
           "SELECT COUNT(*) FROM t ORDER BY CASE WHEN SUM(i32) NOT IN (SELECT i64 FROM u) OR "
           "MIN(i16) = 1 THEN 1 ELSE 0 END",
       }) {
    auto plan = BindSql(sql, catalog);
    ASSERT_TRUE(plan.ok()) << sql << ": " << plan.status().ToString();
    EXPECT_EQ(PositionMismatch(*plan), std::nullopt) << sql << "\n" << Explain(*plan);
    const LogicalPlan optimized = Optimized(*plan);
    EXPECT_EQ(PositionMismatch(optimized), std::nullopt) << sql << "\n" << Explain(optimized);
  }
}

}  // namespace
}  // namespace antb1::plan
