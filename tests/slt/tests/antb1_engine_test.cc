#include "antb1_engine.h"

#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
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

// An engine that returns a fixed result or error for every query.
class FixedEngine final : public Engine {
 public:
  explicit FixedEngine(ExecResult result) : result_(std::move(result)) {}
  [[nodiscard]] std::string_view name() const override { return "antb1"; }
  ExecResult Execute(const std::string& /*sql*/) override { return result_; }

 private:
  ExecResult result_;
};

ResultSet Rows(std::vector<Row> rows) {
  return ResultSet{
      .classes = {ColumnClass::kInteger}, .type_names = {"BIGINT"}, .rows = std::move(rows)};
}

// --same-as-threads: any difference from the reference engine, in rows, row order, types or
// errors, is an internal error that names no value; identical results pass through.
TEST(SameResultEngineTest, AnyDifferenceIsAnInternalError) {
  const auto check = [](ExecResult a, ExecResult b) {
    auto engine = MakeSameResultEngine(std::make_unique<FixedEngine>(std::move(a)),
                                       std::make_unique<FixedEngine>(std::move(b)), "1 thread(s)");
    EXPECT_EQ(engine->name(), "antb1");
    return engine->Execute("SELECT 1");
  };
  const EngineError bind{
      .kind = "bind", .message = "bind: no such column", .unsupported = false, .internal = false};
  const ExecResult same = check(Rows({{"1"}, {"2"}}), Rows({{"1"}, {"2"}}));
  ASSERT_TRUE(same.has_value());
  EXPECT_EQ(same->rows, (std::vector<Row>{{"1"}, {"2"}}));
  const ExecResult same_error = check(std::unexpected(bind), std::unexpected(bind));
  ASSERT_FALSE(same_error.has_value());
  EXPECT_EQ(same_error.error().kind, "bind");

  EngineError other = bind;
  other.message = "bind: another message";
  for (const auto& [a, b] : std::vector<std::pair<ExecResult, ExecResult>>{
           {Rows({{"1"}, {"2"}}), Rows({{"2"}, {"1"}})},
           {Rows({{"1"}}), Rows({{"1"}, {"1"}})},
           {Rows({{"secret"}}), Rows({{std::nullopt}})},
           {Rows({{"1"}}), std::unexpected(bind)},
           {std::unexpected(bind), Rows({{"1"}})},
           {std::unexpected(bind), std::unexpected(other)},
       }) {
    const ExecResult result = check(a, b);
    ASSERT_FALSE(result.has_value());
    EXPECT_TRUE(result.error().internal);
    EXPECT_NE(result.error().message.find("differs from the result with 1 thread(s)"),
              std::string::npos)
        << result.error().message;
    EXPECT_EQ(result.error().message.find("secret"), std::string::npos);
  }
  ResultSet text = Rows({{"1"}});
  text.classes = {ColumnClass::kText};
  EXPECT_FALSE(check(Rows({{"1"}}), text).has_value());
}

}  // namespace
}  // namespace antb1::slt
