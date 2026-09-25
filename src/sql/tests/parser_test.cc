#include "antb1/sql/parser.h"

#include <variant>

#include <gtest/gtest.h>

namespace antb1::sql {
namespace {

TEST(ParserTest, ParsesCountStarFromName) {
  auto stmt = Parse("SELECT COUNT(*) FROM events;");
  ASSERT_TRUE(stmt.has_value()) << stmt.error().message;
  ASSERT_EQ(stmt->items.size(), 1U);
  const auto* agg = std::get_if<AggregateCall>(&stmt->items[0].expr);
  ASSERT_NE(agg, nullptr);
  EXPECT_EQ(agg->kind, AggKind::kCountStar);
  EXPECT_EQ(stmt->from.kind, TableRef::Kind::kName);
  EXPECT_EQ(stmt->from.name, "events");
}

TEST(ParserTest, ParsesQuotedPath) {
  auto stmt = Parse("select count(*) from 'data/x.parquet'");
  ASSERT_TRUE(stmt.has_value());
  EXPECT_EQ(stmt->from.kind, TableRef::Kind::kPath);
  EXPECT_EQ(stmt->from.name, "data/x.parquet");
}

TEST(ParserTest, UnsupportedConstructsHaveSpans) {
  auto r = Parse("SELECT COUNT(*) FROM tt GROUP BY x");
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error().kind, ParseError::Kind::kUnsupported);
  EXPECT_EQ(r.error().span.offset, 24U);  // "GROUP"
}

TEST(ParserTest, SyntaxErrors) {
  auto r = Parse("SELECT COUNT(*) hits");
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error().kind, ParseError::Kind::kSyntax);
  EXPECT_FALSE(Parse("").has_value());
  EXPECT_FALSE(Parse("SELECT COUNT(*) FROM").has_value());
}

}  // namespace
}  // namespace antb1::sql
