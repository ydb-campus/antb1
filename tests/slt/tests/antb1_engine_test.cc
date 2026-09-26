#include "antb1_engine.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <arrow/api.h>
#include <gtest/gtest.h>

#include "antb1/engine/session.h"
#include "antb1/plan/types.h"

#include "engine.h"

namespace antb1::slt {
namespace {

using plan::LogicalType;
using S = std::optional<std::string>;
using I = std::optional<int64_t>;
using Row = std::vector<std::optional<std::string>>;

std::shared_ptr<arrow::Array> Strings(const std::vector<S>& values) {
  arrow::BinaryBuilder b;
  for (const auto& v : values) {
    EXPECT_TRUE((v ? b.Append(*v) : b.AppendNull()).ok());
  }
  return b.Finish().ValueOrDie();
}

std::shared_ptr<arrow::Array> Ints(const std::vector<I>& values) {
  arrow::Int64Builder b;
  for (const auto& v : values) {
    EXPECT_TRUE((v ? b.Append(*v) : b.AppendNull()).ok());
  }
  return b.Finish().ValueOrDie();
}

engine::QueryResult Result(const std::vector<arrow::ArrayVector>& columns,
                           std::vector<LogicalType> types) {
  arrow::FieldVector fields;
  arrow::ChunkedArrayVector chunked;
  for (std::size_t c = 0; c < columns.size(); ++c) {
    fields.push_back(arrow::field("c" + std::to_string(c), columns[c].front()->type()));
    chunked.push_back(std::make_shared<arrow::ChunkedArray>(columns[c]));
  }
  engine::QueryResult result;
  result.table = arrow::Table::Make(arrow::schema(fields), chunked);
  result.types = std::move(types);
  return result;
}

// Arrow keeps a binary column over 2 GiB in several chunks even after CombineChunks, so the
// harness must not assume one chunk. Small data cannot force that split; this checks that every
// chunk layout (empty and sliced chunks, NULLs and values in later chunks) gives the same rows.
TEST(ToResultSet, ChunkedColumnsGiveTheSameRowsAsOneChunk) {
  const arrow::ArrayVector s = {Strings({"a"}), Strings({}),
                                Strings({"skip", "b", std::nullopt, "NULL"})->Slice(1),
                                Strings({std::nullopt})};
  const arrow::ArrayVector n = {Ints({1, 2}), Ints({std::nullopt, 4, 5})};
  const std::vector<LogicalType> types = {LogicalType::kVarchar, LogicalType::kBigInt};

  auto chunked = ToResultSet(Result({s, n}, types));
  ASSERT_TRUE(chunked.ok()) << chunked.status();
  const std::vector<Row> expected = {
      {"a", "1"}, {"b", "2"}, {std::nullopt, std::nullopt}, {"NULL", "4"}, {std::nullopt, "5"}};
  EXPECT_EQ(chunked->rows, expected);

  auto one = [](const arrow::ArrayVector& chunks) {
    return arrow::ArrayVector{arrow::Concatenate(chunks).ValueOrDie()};
  };
  auto single = ToResultSet(Result({one(s), one(n)}, types));
  ASSERT_TRUE(single.ok()) << single.status();
  EXPECT_EQ(single->rows, chunked->rows);
  EXPECT_EQ(single->type_names, (std::vector<std::string>{"VARCHAR", "BIGINT"}));
}

TEST(ToResultSet, EmptyResultWithoutChunks) {
  engine::QueryResult result;
  result.table = arrow::Table::Make(
      arrow::schema({arrow::field("n", arrow::int64())}),
      {std::make_shared<arrow::ChunkedArray>(arrow::ArrayVector{}, arrow::int64())}, 0);
  result.types = {LogicalType::kBigInt};
  auto rows = ToResultSet(result);
  ASSERT_TRUE(rows.ok()) << rows.status();
  EXPECT_TRUE(rows->rows.empty());
}

// A malformed result (a column shorter than the table) is an error, never an out-of-bounds read.
TEST(ToResultSet, ColumnLengthDiffersFromTheTable) {
  auto result =
      Result({{Ints({1, 2, 3})}, {Ints({1})}}, {LogicalType::kBigInt, LogicalType::kBigInt});
  auto rows = ToResultSet(result);
  EXPECT_TRUE(rows.status().IsInvalid()) << rows.status();
}

TEST(ToResultSet, ColumnCountDiffersFromTheTypes) {
  auto rows = ToResultSet(Result({{Ints({1})}}, {LogicalType::kBigInt, LogicalType::kBigInt}));
  EXPECT_TRUE(rows.status().IsInvalid()) << rows.status();
}

}  // namespace
}  // namespace antb1::slt
