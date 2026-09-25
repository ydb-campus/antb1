#include "antb1/plan/sql_status.h"

#include <gtest/gtest.h>

namespace antb1::plan {
namespace {

TEST(SqlStatusTest, ParseErrorsKeepSpanAndKind) {
  const sql::ParseError syntax{.kind = sql::ParseError::Kind::kSyntax,
                               .message = "boom",
                               .span = SourceSpan{.offset = 3, .length = 2}};
  const auto st = ToArrowStatus(syntax);
  EXPECT_TRUE(st.IsInvalid());
  auto detail = GetSqlError(st);
  ASSERT_NE(detail, nullptr);
  EXPECT_EQ(detail->kind(), SqlErrorDetail::Kind::kParse);
  EXPECT_EQ(detail->span().offset, 3U);

  const sql::ParseError unsupported{.kind = sql::ParseError::Kind::kUnsupported, .message = "x"};
  const auto st2 = ToArrowStatus(unsupported);
  EXPECT_TRUE(st2.IsNotImplemented());
  EXPECT_EQ(GetSqlError(st2)->kind(), SqlErrorDetail::Kind::kUnsupported);
}

TEST(SqlStatusTest, PlainStatusHasNoDetail) {
  EXPECT_EQ(GetSqlError(arrow::Status::NotImplemented("internal")), nullptr);
  EXPECT_EQ(GetSqlError(arrow::Status::OK()), nullptr);
}

}  // namespace
}  // namespace antb1::plan
