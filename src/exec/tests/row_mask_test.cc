// RowMask: the rows of a batch that are selected and not NULL in the columns an aggregate or a join
// reads.

#include "../row_mask.h"

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

#include <arrow/api.h>
#include <gtest/gtest.h>

#include "antb1/exec/memory_budget.h"

#include "exec_test_util.h"

namespace antb1::exec {
namespace {

using testing::Bools;
using testing::Int64s;

// The rows of a mask, through its runs (and counted).
std::vector<int64_t> Rows(const RowMask& mask) {
  std::vector<int64_t> rows;
  mask.ForEachRun([&rows](int64_t position, int64_t count) {
    for (int64_t row = position; row < position + count; ++row) {
      rows.push_back(row);
    }
  });
  EXPECT_EQ(static_cast<int64_t>(rows.size()), mask.CountRows());
  return rows;
}

// Eight rows: a column with NULLs at rows 1 and 7 and another at row 0 (both at an offset of 1 or
// 0), one without NULLs, and a selection that is false at row 3 and NULL at row 5.
class RowMaskTest : public ::testing::Test {
 protected:
  std::shared_ptr<arrow::Array> a_ =
      Int64s({0, 1, std::nullopt, 3, 4, 5, 6, 7, std::nullopt})->Slice(1);
  std::shared_ptr<arrow::Array> b_ = Int64s({std::nullopt, 1, 2, 3, 4, 5, 6, 7});
  std::shared_ptr<arrow::Array> c_ = Int64s({1, 2, 3, 4, 5, 6, 7, 8});
  std::shared_ptr<arrow::BooleanArray> selection_ = std::static_pointer_cast<arrow::BooleanArray>(
      Bools({true, true, true, true, false, true, std::nullopt, true, true})->Slice(1));
};

// Every column's NULLs and the selection, wherever their bitmaps start; a nullptr column is
// skipped, and a column without NULLs reads nothing.
TEST_F(RowMaskTest, FoldsTheNullsOfEveryColumnAndTheSelection) {
  const std::array<const arrow::Array*, 4> values = {a_.get(), nullptr, b_.get(), c_.get()};
  auto mask = RowMask::Make(values, selection_.get(), 8, arrow::default_memory_pool());
  ASSERT_TRUE(mask.ok()) << mask.status().ToString();
  EXPECT_FALSE(mask->all());
  EXPECT_EQ(Rows(*mask), (std::vector<int64_t>{2, 4, 6}));

  // One column: its bitmap, borrowed. No column (COUNT(*), nullptr): the selection.
  auto one = RowMask::Make(a_.get(), nullptr, 8, arrow::default_memory_pool());
  ASSERT_TRUE(one.ok());
  EXPECT_EQ(Rows(*one), (std::vector<int64_t>{0, 2, 3, 4, 5, 6}));
  auto selected = RowMask::Make(nullptr, selection_.get(), 8, arrow::default_memory_pool());
  ASSERT_TRUE(selected.ok());
  EXPECT_EQ(Rows(*selected), (std::vector<int64_t>{0, 1, 2, 4, 6, 7}));

  // Nothing to fold: every row; no row at all: nothing to visit.
  auto every = RowMask::Make(std::span<const arrow::Array* const>(), nullptr, 8,
                             arrow::default_memory_pool());
  ASSERT_TRUE(every.ok());
  EXPECT_TRUE(every->all());
  EXPECT_EQ(Rows(*every).size(), 8U);
  auto none = RowMask::Make(values, selection_.get(), 0, arrow::default_memory_pool());
  ASSERT_TRUE(none.ok());
  EXPECT_EQ(none->length(), 0);
  EXPECT_TRUE(Rows(*none).empty());
}

// Folding a second bitmap allocates from the pool: without room, OutOfMemory.
TEST_F(RowMaskTest, RunsOutOfMemoryCleanly) {
  MemoryBudget empty(0);
  const std::array<const arrow::Array*, 2> two = {a_.get(), b_.get()};
  EXPECT_TRUE(RowMask::Make(two, nullptr, 8, &empty).status().IsOutOfMemory());
  EXPECT_TRUE(RowMask::Make(a_.get(), selection_.get(), 8, &empty).status().IsOutOfMemory());
  EXPECT_TRUE(RowMask::Make(nullptr, selection_.get(), 8, &empty).status().IsOutOfMemory());
  EXPECT_EQ(empty.bytes_allocated(), 0);
}

}  // namespace
}  // namespace antb1::exec
