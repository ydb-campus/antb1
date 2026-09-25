#include "antb1/exec/row_count.h"

#include <arrow/api.h>
#include <gtest/gtest.h>

namespace antb1::exec {
namespace {

TEST(RowCountOperatorTest, EmitsOneRowThenEnds) {
  RowCountOperator op("count_star()", 123456);
  ExecContext ctx;
  auto table = Drain(op, ctx);
  ASSERT_TRUE(table.ok()) << table.status().ToString();
  ASSERT_EQ((*table)->num_rows(), 1);
  ASSERT_EQ((*table)->num_columns(), 1);
  EXPECT_EQ((*table)->schema()->field(0)->name(), "count_star()");
  auto chunk = std::static_pointer_cast<arrow::Int64Array>((*table)->column(0)->chunk(0));
  EXPECT_EQ(chunk->Value(0), 123456);
}

TEST(RowCountOperatorTest, ReopenRestarts) {
  RowCountOperator op("c", 0);
  ExecContext ctx;
  ASSERT_TRUE(Drain(op, ctx).ok());
  auto again = Drain(op, ctx);
  ASSERT_TRUE(again.ok());
  EXPECT_EQ((*again)->num_rows(), 1);
}

}  // namespace
}  // namespace antb1::exec
