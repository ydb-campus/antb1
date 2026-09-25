#include "antb1/plan/binder.h"

#include <memory>
#include <optional>
#include <variant>

#include <arrow/api.h>
#include <gtest/gtest.h>

#include "antb1/plan/sql_status.h"
#include "antb1/sql/parser.h"

namespace antb1::plan {
namespace {

class FakeTable final : public Table {
 public:
  explicit FakeTable(std::optional<int64_t> rows)
      : schema_(arrow::schema({arrow::field("x", arrow::int64())})), rows_(rows) {}
  const std::shared_ptr<arrow::Schema>& schema() const override { return schema_; }
  std::optional<int64_t> exact_row_count() const override { return rows_; }
  arrow::Result<std::unique_ptr<arrow::RecordBatchReader>> Scan(
      const std::vector<int>& /*fields*/, int64_t /*batch_size*/) const override {
    return arrow::Status::NotImplemented("fake");
  }
  std::string Describe() const override { return "fake"; }

 private:
  std::shared_ptr<arrow::Schema> schema_;
  std::optional<int64_t> rows_;
};

arrow::Result<LogicalPlan> BindSql(std::string_view sql, const Catalog& catalog) {
  auto stmt = sql::Parse(sql);
  if (!stmt) {
    return ToArrowStatus(stmt.error());
  }
  return Bind(*stmt, catalog);
}

TEST(BinderTest, CountStarBindsToRowCount) {
  Catalog catalog;
  ASSERT_TRUE(catalog.Register("Events", std::make_shared<FakeTable>(42)).ok());
  auto plan = BindSql("SELECT COUNT(*) FROM events", catalog);  // case-insensitive name
  ASSERT_TRUE(plan.ok()) << plan.status().ToString();
  ASSERT_TRUE(std::holds_alternative<RowCountNode>(*plan->root));
  ASSERT_EQ(plan->output.size(), 1U);
  EXPECT_EQ(plan->output[0].name, "count_star()");
  EXPECT_EQ(plan->output[0].type, LogicalType::kBigInt);
}

TEST(BinderTest, UnknownTableIsBindError) {
  Catalog catalog;
  auto plan = BindSql("SELECT COUNT(*) FROM nope", catalog);
  ASSERT_FALSE(plan.ok());
  auto detail = GetSqlError(plan.status());
  ASSERT_NE(detail, nullptr);
  EXPECT_EQ(detail->kind(), SqlErrorDetail::Kind::kBind);
  EXPECT_EQ(detail->span().offset, 21U);
}

TEST(BinderTest, DuplicateRegistrationFails) {
  Catalog catalog;
  ASSERT_TRUE(catalog.Register("t", std::make_shared<FakeTable>(1)).ok());
  EXPECT_TRUE(catalog.Register("T", std::make_shared<FakeTable>(1)).IsAlreadyExists());
}

TEST(BinderTest, PathWithoutOpenerIsUnsupported) {
  Catalog catalog;
  auto plan = BindSql("SELECT COUNT(*) FROM 'x.parquet'", catalog);
  ASSERT_FALSE(plan.ok());
  EXPECT_EQ(GetSqlError(plan.status())->kind(), SqlErrorDetail::Kind::kUnsupported);
}

TEST(BinderTest, UnknownRowCountIsUnsupported) {
  Catalog catalog;
  ASSERT_TRUE(catalog.Register("t", std::make_shared<FakeTable>(std::nullopt)).ok());
  auto plan = BindSql("SELECT COUNT(*) FROM t", catalog);
  ASSERT_FALSE(plan.ok());
  EXPECT_EQ(GetSqlError(plan.status())->kind(), SqlErrorDetail::Kind::kUnsupported);
}

}  // namespace
}  // namespace antb1::plan
