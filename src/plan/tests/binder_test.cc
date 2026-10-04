#include "antb1/plan/binder.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
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
#include "antb1/plan/explain.h"
#include "antb1/plan/logical_plan.h"
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
        ErrorCase{R"(SELECT "lower"(url) FROM t)", kUnsupported, R"("lower")",
                  "function lower() is not supported"},
        ErrorCase{"SELECT a FROM t GROUP BY year(d)", kUnsupported, "year",
                  "function year() is not supported (only COUNT, SUM, AVG, MIN, MAX, STRLEN, "
                  "REGEXP_REPLACE, TODATETIME and DATE_TRUNC)"},
        ErrorCase{"SELECT a FROM t GROUP BY a HAVING 1 < 2", kUnsupported, "2",
                  "comparisons between two literals are not supported"},
        ErrorCase{"SELECT a FROM t GROUP BY a HAVING COUNT(*)", kUnsupported, "COUNT(*)",
                  "conditions other than comparisons, [NOT] LIKE, [NOT] IN, [NOT] BETWEEN, AND, OR "
                  "and NOT are "
                  "not supported"},
        ErrorCase{"SELECT a FROM t GROUP BY a HAVING MIN(s) LIKE MAX(s)", kUnsupported, "MAX(s)",
                  "LIKE with a column or an aggregate as the pattern is not supported"},
        ErrorCase{"SELECT a FROM t GROUP BY a HAVING a IN (1, COUNT(*))", kUnsupported, "COUNT(*)",
                  "only literals are supported in an IN list"},
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
                  "only literals are supported in an IN list"},
        ErrorCase{"SELECT a FROM t WHERE 1 IN (a)", kUnsupported, "1",
                  "IN needs a column on the left"},
        ErrorCase{"SELECT region IN ('a') FROM t", kUnsupported, "IN",
                  "IN is only supported in conditions (WHERE, HAVING and CASE WHEN)"},
        ErrorCase{"SELECT a FROM t WHERE region IN (1 + 2)", kUnsupported, "1 + 2",
                  "only literals are supported in an IN list"},
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
        ErrorCase{"SELECT CASE WHEN i16 = 1 THEN i16 ELSE 1.5 END FROM t", kUnsupported, "1.5",
                  "a decimal literal as a CASE value is supported only next to a DOUBLE value"},
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
                  "under +, - and * are folded)"},
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
        ErrorCase{"SELECT dt + 1 FROM t", kUnsupported, "+", "DATE arithmetic"},
        ErrorCase{"SELECT -u16 FROM t", kUnsupported, "-", "negating a USMALLINT is not supported"},
        ErrorCase{"SELECT SUM(i64) % 2 FROM t", kUnsupported, "%", "'%' in HUGEINT"},
        // DECIMAL (ADR 0021). %: DuckDB computes it in DOUBLE beyond 38 digits (D4c).
        ErrorCase{"SELECT h % 0.5 FROM t", kUnsupported, "%",
                  "'%' of DECIMAL(38,0) and DECIMAL(2,1) is not supported"},
        ErrorCase{"SELECT SUM(i64) % 2.5 FROM t", kUnsupported, "%",
                  "'%' of DECIMAL(38,0) and DECIMAL(2,1) is not supported"},
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
        ErrorCase{"SELECT i16 FROM t GROUP BY i16 HAVING MIN(h) = MIN(s)", kBind, "=",
                  "cannot compare"},
        ErrorCase{"SELECT CASE WHEN i = 1 THEN p END FROM dec", kUnsupported, "p",
                  "DECIMAL CASE values are supported only next to a DOUBLE value"},
        ErrorCase{"SELECT CASE WHEN i = 1 THEN p ELSE q END FROM dec", kUnsupported, "p",
                  "DECIMAL CASE values are supported only next to a DOUBLE value"},
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
        ErrorCase{"SELECT regexp_replace(s, 'a', 1) FROM t", kBind, "1",
                  "argument 3 of regexp_replace() must be a string literal"},
        ErrorCase{"SELECT regexp_replace(s, 'a', 'b', 'g') FROM t", kUnsupported, "'g'",
                  "regexp_replace() with options (a fourth argument) is not supported"},
        ErrorCase{"SELECT regexp_replace(s, 'a', 'b', 'g', 'x') FROM t", kBind,
                  "regexp_replace(s, 'a', 'b', 'g', 'x')", "takes 3 arguments, not 5"},
        ErrorCase{"SELECT strlen(lower(s)) FROM t", kUnsupported, "lower",
                  "function lower() is not supported"},
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
        ErrorCase{"SELECT lower(url) FROM t", kUnsupported, "lower",
                  "function lower() is not supported (only COUNT, SUM, AVG, MIN, MAX, STRLEN, "
                  "REGEXP_REPLACE, TODATETIME and DATE_TRUNC)"},
        ErrorCase{"SELECT a FROM t WHERE length(url) > 5", kUnsupported, "length",
                  "function length() is not supported (only COUNT, SUM, AVG, MIN, MAX, STRLEN, "
                  "REGEXP_REPLACE, TODATETIME and DATE_TRUNC)"},
        ErrorCase{"SELECT a FROM t WHERE d > now()", kUnsupported, "now",
                  "function now() is not supported (only COUNT, SUM, AVG, MIN, MAX, STRLEN, "
                  "REGEXP_REPLACE, TODATETIME and DATE_TRUNC)"},
        ErrorCase{"SELECT SUM(abs(a)) FROM t", kUnsupported, "abs",
                  "function abs() is not supported (only COUNT, SUM, AVG, MIN, MAX, STRLEN, "
                  "REGEXP_REPLACE, TODATETIME and DATE_TRUNC)"},
        ErrorCase{"SELECT left(url, 3) FROM t", kUnsupported, "left",
                  "function left() is not supported (only COUNT, SUM, AVG, MIN, MAX, STRLEN, "
                  "REGEXP_REPLACE, TODATETIME and DATE_TRUNC)"},
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
        ErrorCase{"SELECT a FROM t WHERE lower(url)", kUnsupported, "lower",
                  "function lower() is not supported"},
        ErrorCase{"SELECT a FROM t WHERE lower(url) LIKE 'x%'", kUnsupported, "lower",
                  "function lower() is not supported"},
        ErrorCase{"SELECT a FROM t WHERE url LIKE lower(title)", kUnsupported, "lower",
                  "function lower() is not supported"},
        ErrorCase{"SELECT a FROM t WHERE lower(url) IN ('x')", kUnsupported, "lower",
                  "function lower() is not supported"},
        ErrorCase{"SELECT a FROM t WHERE url IN ('x', lower(title))", kUnsupported, "lower",
                  "function lower() is not supported"}));

// Casts: CAST('YYYY-MM-DD' AS DATE) and 'YYYY-MM-DD'::DATE are DATE literals spanning the cast,
// with their bind errors (D4, D5); every other cast is kUnsupported, before any name is resolved.
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
        ErrorCase{"SELECT CAST('2013-07-01' AS DATE) + 1 FROM t", kUnsupported, "+",
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
        ErrorCase{"SELECT try_cast(lower(nope) AS INT) FROM t", kUnsupported, "try_cast",
                  "TRY_CAST is not supported"},
        ErrorCase{"SELECT CAST(i16 AS BIGINT) FROM t", kUnsupported, "BIGINT",
                  "CAST to BIGINT is not supported (only a string literal cast to DATE is)"},
        ErrorCase{"SELECT nope::varchar FROM t", kUnsupported, "varchar",
                  "CAST to VARCHAR is not supported"},
        ErrorCase{"SELECT CAST(h AS decimal(15,2)) FROM t", kUnsupported, "decimal(15,2)",
                  "CAST to DECIMAL(15, 2) is not supported"},
        ErrorCase{"SELECT CAST('2013-07-01' AS DATE(3)) FROM t", kUnsupported, "DATE(3)",
                  "CAST to DATE(3) is not supported"},
        ErrorCase{"SELECT CAST(dt AS DATE) FROM t", kUnsupported, "dt",
                  "CAST to DATE is only supported for a string literal ('YYYY-MM-DD')"},
        ErrorCase{"SELECT CAST(DATE '2013-07-01' AS DATE) FROM t", kUnsupported,
                  "DATE '2013-07-01'", "CAST to DATE is only supported for a string literal"},
        ErrorCase{"SELECT '2013-07-01'::DATE::DATE FROM t", kUnsupported, "'2013-07-01'::DATE",
                  "CAST to DATE is only supported for a string literal"},
        ErrorCase{"SELECT CAST(20130701 AS DATE) FROM t", kUnsupported, "20130701",
                  "CAST to DATE is only supported for a string literal"},
        ErrorCase{"SELECT CAST(lower(s) AS INT) FROM t", kUnsupported, "lower",
                  "function lower() is not supported"},
        ErrorCase{"SELECT (i16 = 1)::VARCHAR FROM t", kUnsupported, "=",
                  "comparisons are only supported in conditions"},
        ErrorCase{"SELECT COUNT(*) FROM t WHERE CAST(i16 AS VARCHAR) = '1'", kUnsupported,
                  "VARCHAR", "CAST to VARCHAR is not supported"},
        ErrorCase{"SELECT COUNT(*) FROM t WHERE CAST(i16 AS BOOLEAN)", kUnsupported, "BOOLEAN",
                  "CAST to BOOLEAN is not supported"},
        ErrorCase{"SELECT COUNT(*) FROM t WHERE i16 IN (1, 2::INT)", kUnsupported, "INT",
                  "CAST to INT is not supported"},
        ErrorCase{"SELECT COUNT(*) FROM t WHERE s LIKE 'x%'::VARCHAR", kUnsupported, "VARCHAR",
                  "CAST to VARCHAR is not supported"},
        ErrorCase{"SELECT i16 FROM t GROUP BY i16::INT", kUnsupported, "INT",
                  "CAST to INT is not supported"},
        ErrorCase{"SELECT i16 FROM t ORDER BY CAST(i16 AS BIGINT) DESC", kUnsupported, "BIGINT",
                  "CAST to BIGINT is not supported"},
        ErrorCase{"SELECT SUM(i16::BIGINT) FROM t", kUnsupported, "BIGINT",
                  "CAST to BIGINT is not supported"},
        ErrorCase{"SELECT COUNT(*) FROM t GROUP BY i16 HAVING SUM(i16)::BIGINT > 1", kUnsupported,
                  "BIGINT", "CAST to BIGINT is not supported"},
        ErrorCase{"SELECT CASE WHEN i16 = 1 THEN CAST(i16 AS INT) END FROM t", kUnsupported, "INT",
                  "CAST to INT is not supported"},
        ErrorCase{"SELECT EXTRACT(year FROM CAST(i64 AS TIMESTAMP)) FROM t", kUnsupported,
                  "TIMESTAMP", "CAST to TIMESTAMP is not supported"},
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
           Case{.expr = "-(1.5)", .type = dec(2, 1), .name = "-(1.5)"},
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

// A DECIMAL CASE value, a decimal literal or a negated one included, next to a DOUBLE value makes
// the CASE DOUBLE, in either order (DuckDB 1.5.5's typeof); other DECIMAL common types wait for
// D4b.
TEST(BinderTest, DecimalCaseValuesNextToADoubleAreDouble) {
  const Catalog catalog = MakeCatalog();
  for (const std::string_view expr : {
           "CASE WHEN i = 1 THEN 1.5 ELSE f END",
           "CASE WHEN i = 1 THEN f ELSE 1.5 END",
           "CASE WHEN i = 1 THEN 1.5 ELSE 1e3 END",
           "CASE WHEN i = 1 THEN -(1.5) ELSE f END",
           "CASE WHEN i = 1 THEN p ELSE f END",
           "CASE WHEN i = 1 THEN p * q WHEN i = 2 THEN 7 ELSE f END",
       }) {
    const std::string sql = "SELECT " + std::string(expr) + " FROM dec";
    auto plan = BindSql(sql, catalog);
    ASSERT_TRUE(plan.ok()) << sql << ": " << plan.status().ToString();
    EXPECT_EQ(plan->output[0].type, LogicalType::kDouble) << sql;
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

// GROUP BY an expression computes it below the aggregation; a select or ORDER BY expression equal
// to a key is that key, others over keys and aggregates are computed above the aggregation, as are
// HAVING and ORDER BY expressions.
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
  // Limit <- Project <- Sort <- Filter (HAVING) <- Compute <- GroupAggregate <- Compute (the key)
  // <- Filter (WHERE) <- Compute (its operand) <- Scan.
  EXPECT_TRUE(std::holds_alternative<LimitNode>(Nth(*plan, 0)));
  EXPECT_TRUE(std::holds_alternative<ProjectNode>(Nth(*plan, 1)));
  const auto& sort = std::get<SortNode>(Nth(*plan, 2));
  ASSERT_EQ(sort.keys.size(), 2U);
  const auto& having = std::get<FilterNode>(Nth(*plan, 3));
  ASSERT_EQ(having.predicates.size(), 1U);
  EXPECT_EQ(having.predicates[0].kind, Predicate::Kind::kCompareColumns);
  const auto& post = std::get<ComputeNode>(Nth(*plan, 4));
  std::vector<std::string> names;
  names.reserve(post.exprs.size());
  for (const auto& e : post.exprs) {
    names.push_back(e->name);
  }
  EXPECT_EQ(names, (std::vector<std::string>{"((i32 - 1) * 2)", "(sum(i16) * 2)", "(sum(i16) + 1)",
                                             "-((i32 - 1))"}));
  const auto& group = std::get<GroupAggregateNode>(Nth(*plan, 5));
  ASSERT_EQ(group.keys.size(), 1U);
  EXPECT_EQ(group.keys[0].name, "(i32 - 1)");
  const auto& keys = std::get<ComputeNode>(Nth(*plan, 6));
  ASSERT_EQ(keys.exprs.size(), 1U);
  EXPECT_EQ(keys.exprs[0]->name, "(i32 - 1)");
  const auto& where = std::get<FilterNode>(Nth(*plan, 7));
  EXPECT_EQ(where.predicates[0].column.value_or(BoundColumn{}).name, "(i16 // 2)");
  const auto& operand = std::get<ComputeNode>(Nth(*plan, 8));
  ASSERT_EQ(operand.exprs.size(), 1U);
  EXPECT_EQ(operand.exprs[0]->name, "(i16 // 2)");
  EXPECT_TRUE(std::holds_alternative<ScanNode>(Nth(*plan, 9)));

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

// DuckDB's constant moving in WHERE: x + c <op> k is x <op> k - c for a signed integer x (and
// likewise x - c, c + x, c - x, x * c when c divides k), so the arithmetic is never computed;
// it stops where k or the new constant leaves the type, and never applies to USMALLINT.
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
       }) {
    const std::string sql = "SELECT COUNT(*) FROM t WHERE " + std::string(c.where);
    auto plan = BindSql(sql, catalog);
    ASSERT_TRUE(plan.ok()) << sql << ": " << plan.status().ToString();
    const std::string explain = Explain(*plan);
    EXPECT_NE(explain.find(std::string(c.filter) + "\n"), std::string::npos) << sql << "\n"
                                                                             << explain;
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
       }) {
    auto plan = BindSql(c.sql, catalog);
    ASSERT_TRUE(plan.ok()) << c.sql << ": " << plan.status().ToString();
    const std::string explain = Explain(*plan);
    EXPECT_NE(explain.find(c.text), std::string::npos) << c.sql << "\n" << explain;
    EXPECT_EQ(explain.find("(i16 + 30000)\n"), std::string::npos)
        << "no addition computed: " << c.sql << "\n"
        << explain;
  }
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
  const auto& negated = std::get<BoolExpr>(any.args[0]->node);
  EXPECT_EQ(negated.op, BoolOp::kNot);
  const auto& never = std::get<PredicateExpr>(negated.args[0]->node);
  EXPECT_EQ(never.predicate.kind, Predicate::Kind::kFalse);
  ASSERT_TRUE(never.predicate.column.has_value()) << "NULL for a NULL i16, not FALSE";
  EXPECT_EQ(never.operands.size(), 1U);
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

// A DECIMAL against a number DuckDB types as DOUBLE (an exponent, more than 38 digits, an integer
// beyond UHUGEINT), or in an IN list with one, compares in DOUBLE: nothing is folded, and the
// constants are the doubles DuckDB converts the literals to (ADR 0021 rules 8 and 11).
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
  // An integer column keeps today's rule: only an exponent or more than 38 digits make the list
  // DOUBLE, so a 40-digit integer folds exactly (and drops out).
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
        "SELECT CASE WHEN p > f THEN 1 END FROM tf"}) {
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
// expression, computed again inside the condition: a Compute's expressions read only its input.
TEST(BinderTest, HavingAliasOfAComputedItemInsideOr) {
  const Catalog catalog = MakeCatalog();
  auto plan = BindSql(
      "SELECT i16, CASE WHEN COUNT(*) > 1 THEN 'many' END AS size FROM t GROUP BY i16 "
      "HAVING size = 'many' OR size = 'few'",
      catalog);
  ASSERT_TRUE(plan.ok()) << plan.status().ToString();
  const auto& compute = std::get<ComputeNode>(Nth(*plan, 2));
  ASSERT_EQ(compute.exprs.size(), 2U);  // the CASE item and the condition
  const auto& condition = std::get<BoolExpr>(compute.exprs[1]->node);
  ASSERT_EQ(condition.args.size(), 2U);
  for (const ExprPtr& arg : condition.args) {
    const auto& predicate = std::get<PredicateExpr>(arg->node);
    ASSERT_EQ(predicate.operands.size(), 1U);
    EXPECT_TRUE(std::holds_alternative<CaseExpr>(predicate.operands[0]->node));
  }
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

}  // namespace
}  // namespace antb1::plan
