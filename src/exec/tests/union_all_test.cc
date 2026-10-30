#include "../union_all.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <arrow/api.h>
#include <gtest/gtest.h>

#include "antb1/exec/limit.h"
#include "antb1/exec/operator.h"
#include "antb1/plan/types.h"

#include "exec_test_util.h"

namespace antb1::exec {
namespace {

using plan::LogicalType;
using testing::Bools;
using testing::Int64Column;
using testing::Int64s;
using testing::ScriptedSource;
using testing::Strings;

// Input 0's shape: (k BIGINT NOT NULL, s VARCHAR).
std::shared_ptr<arrow::Schema> KeyThenText() {
  return arrow::schema(
      {arrow::field("k", arrow::int64(), /*nullable=*/false), arrow::field("s", arrow::binary())});
}

// Input 1's: the same two columns the other way round, under other names, and a third it does not
// contribute.
std::shared_ptr<arrow::Schema> TextThenKey() {
  return arrow::schema({arrow::field("t", arrow::binary()), arrow::field("n", arrow::int64()),
                        arrow::field("unused", arrow::int64())});
}

Batch KeyText(const std::vector<std::optional<int64_t>>& keys,
              const std::vector<std::optional<std::string>>& texts) {
  const auto rows = static_cast<int64_t>(keys.size());
  return Batch{
      .data = arrow::RecordBatch::Make(KeyThenText(), rows, {Int64s(keys), Strings(texts)}),
      .selection = nullptr};
}

Batch TextKey(const std::vector<std::optional<std::string>>& texts,
              const std::vector<std::optional<int64_t>>& keys) {
  const auto rows = static_cast<int64_t>(keys.size());
  return Batch{.data = arrow::RecordBatch::Make(TextThenKey(), rows,
                                                {Strings(texts), Int64s(keys), Int64s(keys)}),
               .selection = nullptr};
}

// The union of `first` and `second` with each one's key, then its text.
arrow::Result<std::unique_ptr<UnionAllOperator>> UnionOf(std::unique_ptr<Operator> first,
                                                         std::unique_ptr<Operator> second) {
  std::vector<std::unique_ptr<Operator>> inputs;
  inputs.push_back(std::move(first));
  inputs.push_back(std::move(second));
  return UnionAllOperator::Make(std::move(inputs), {{0, 1}, {1, 0}},
                                {LogicalType::kBigInt, LogicalType::kVarchar});
}

class UnionAllTest : public testing::ExecTest {};

// Every row of input 0, then every row of input 1, each input's columns at the positions the union
// maps; under fields named as input 0 names them and all nullable, whatever the inputs declare.
TEST_F(UnionAllTest, EmitsEveryInputInOrderUnderItsOwnSchema) {
  auto first = std::make_unique<ScriptedSource>(
      KeyThenText(), std::vector<Batch>{KeyText({1, 2}, {"a", "b"}), KeyText({3}, {"c"})});
  auto second = std::make_unique<ScriptedSource>(
      TextThenKey(), std::vector<Batch>{TextKey({"d", std::nullopt}, {std::nullopt, 5})});
  auto op = UnionOf(std::move(first), std::move(second));
  ASSERT_TRUE(op.ok()) << op.status().ToString();
  const std::shared_ptr<arrow::Schema>& schema = (*op)->output_schema();
  ASSERT_EQ(schema->num_fields(), 2);
  EXPECT_EQ(schema->field(0)->name(), "k");
  EXPECT_EQ(schema->field(1)->name(), "s");
  EXPECT_TRUE(schema->field(0)->nullable()) << "input 1's key is nullable";
  EXPECT_TRUE(schema->field(1)->type()->Equals(*arrow::binary()));

  ExecContext ctx;
  auto table = Drain(**op, ctx);
  ASSERT_TRUE(table.ok()) << table.status().ToString();
  EXPECT_EQ(Int64Column(**table, 0),
            (std::vector<std::optional<int64_t>>{1, 2, 3, std::nullopt, 5}));
  std::vector<std::optional<std::string>> texts;
  for (const auto& chunk : (*table)->column(1)->chunks()) {
    const auto& values = static_cast<const arrow::BinaryArray&>(*chunk);
    for (int64_t i = 0; i < values.length(); ++i) {
      texts.push_back(values.IsNull(i) ? std::nullopt : std::optional(values.GetString(i)));
    }
  }
  EXPECT_EQ(texts, (std::vector<std::optional<std::string>>{"a", "b", "c", "d", std::nullopt}));
}

// A batch is rebuilt, not copied: the union's columns are the input's own arrays, and its selection
// is the input's own.
TEST_F(UnionAllTest, KeepsTheInputsArraysAndSelection) {
  Batch selected = KeyText({1, 2, 3}, {"a", "b", "c"});
  selected.selection = Bools({true, false, true});
  const Batch kept = selected;
  auto op = UnionOf(std::make_unique<ScriptedSource>(KeyThenText(), std::vector<Batch>{selected}),
                    std::make_unique<ScriptedSource>(TextThenKey(), std::vector<Batch>{}));
  ASSERT_TRUE(op.ok()) << op.status().ToString();
  ExecContext ctx;
  ASSERT_TRUE((*op)->Open(ctx).ok());
  auto batch = (*op)->Next();
  ASSERT_TRUE(batch.ok()) << batch.status().ToString();
  ASSERT_FALSE(batch->end());
  EXPECT_EQ(batch->data->column(0).get(), kept.data->column(0).get());
  EXPECT_EQ(batch->data->column(1).get(), kept.data->column(1).get());
  EXPECT_EQ(batch->selection.get(), kept.selection.get());
  EXPECT_EQ(batch->selected_rows(), 2);
  EXPECT_TRUE(batch->data->schema()->Equals(*(*op)->output_schema()));
  auto end = (*op)->Next();
  ASSERT_TRUE(end.ok());
  EXPECT_TRUE(end->end());
  EXPECT_TRUE((*op)->Close().ok());
}

// An input is opened when the union reaches it and closed at its end, before the next one opens:
// one input's resources at a time.
TEST_F(UnionAllTest, OpensEachInputWhenItIsReachedAndClosesItBeforeTheNext) {
  auto owned_first = std::make_unique<ScriptedSource>(
      KeyThenText(), std::vector<Batch>{KeyText({1}, {"a"}), KeyText({2}, {"b"})});
  auto owned_second =
      std::make_unique<ScriptedSource>(TextThenKey(), std::vector<Batch>{TextKey({"c"}, {3})});
  const ScriptedSource& first = *owned_first;
  const ScriptedSource& second = *owned_second;
  auto op = UnionOf(std::move(owned_first), std::move(owned_second));
  ASSERT_TRUE(op.ok()) << op.status().ToString();
  ExecContext ctx;
  ASSERT_TRUE((*op)->Open(ctx).ok());
  EXPECT_EQ(first.opens(), 0) << "opened by the first Next, not by Open";

  ASSERT_TRUE((*op)->Next().ok());
  ASSERT_TRUE((*op)->Next().ok());
  EXPECT_EQ(first.opens(), 1);
  EXPECT_EQ(second.opens(), 0) << "input 1 waits for input 0's end";

  auto third = (*op)->Next();  // input 0 ends, closes, and input 1 gives its batch
  ASSERT_TRUE(third.ok());
  ASSERT_FALSE(third->end());
  EXPECT_EQ(first.closes(), 1);
  EXPECT_EQ(second.opens(), 1);

  auto end = (*op)->Next();
  ASSERT_TRUE(end.ok());
  EXPECT_TRUE(end->end());
  EXPECT_EQ(second.closes(), 1);
  EXPECT_TRUE((*op)->Close().ok());
  EXPECT_EQ(first.closes(), 1) << "closed once";
  EXPECT_EQ(second.closes(), 1) << "closed once";

  // A rerun starts from input 0 again.
  ASSERT_TRUE((*op)->Open(ctx).ok());
  auto again = Drain(**op, ctx);
  ASSERT_TRUE(again.ok()) << again.status().ToString();
  EXPECT_EQ(Int64Column(**again, 0), (std::vector<std::optional<int64_t>>{1, 2, 3}));
  EXPECT_EQ(first.opens(), 2);
  EXPECT_EQ(second.opens(), 2);
}

// A consumer that stops before an input's end never opens the inputs after it, so their errors are
// never raised: the rows of a met LIMIT are all from input 0 here.
TEST_F(UnionAllTest, ALimitAboveNeverOpensALaterInput) {
  auto owned_second =
      std::make_unique<ScriptedSource>(TextThenKey(), std::vector<Batch>{TextKey({"c"}, {3})});
  owned_second->FailAt(0);
  const ScriptedSource& second = *owned_second;
  auto op = UnionOf(std::make_unique<ScriptedSource>(
                        KeyThenText(), std::vector<Batch>{KeyText({1, 2, 3}, {"a", "b", "c"})}),
                    std::move(owned_second));
  ASSERT_TRUE(op.ok()) << op.status().ToString();
  LimitOperator limit(*std::move(op), 2);
  ExecContext ctx;
  auto table = Drain(limit, ctx);
  ASSERT_TRUE(table.ok()) << table.status().ToString();
  EXPECT_EQ(Int64Column(**table, 0), (std::vector<std::optional<int64_t>>{1, 2}));
  EXPECT_EQ(second.opens(), 0);
}

// The first failing input in input order decides the error, after the rows of the inputs before it;
// the inputs after it are never opened, and the failing one is closed.
TEST_F(UnionAllTest, TheFirstFailingInputDecides) {
  auto owned_second =
      std::make_unique<ScriptedSource>(TextThenKey(), std::vector<Batch>{TextKey({"c"}, {3})});
  owned_second->FailAt(0);
  auto owned_third =
      std::make_unique<ScriptedSource>(KeyThenText(), std::vector<Batch>{KeyText({4}, {"d"})});
  const ScriptedSource& second = *owned_second;
  const ScriptedSource& third = *owned_third;
  std::vector<std::unique_ptr<Operator>> inputs;
  inputs.push_back(std::make_unique<ScriptedSource>(
      KeyThenText(), std::vector<Batch>{KeyText({1, 2}, {"a", "b"})}));
  inputs.push_back(std::move(owned_second));
  inputs.push_back(std::move(owned_third));
  auto op = UnionAllOperator::Make(std::move(inputs), {{0, 1}, {1, 0}, {0, 1}},
                                   {LogicalType::kBigInt, LogicalType::kVarchar});
  ASSERT_TRUE(op.ok()) << op.status().ToString();
  ExecContext ctx;
  ASSERT_TRUE((*op)->Open(ctx).ok());
  auto rows = (*op)->Next();
  ASSERT_TRUE(rows.ok()) << "input 0's rows come first";
  EXPECT_EQ(rows->selected_rows(), 2);
  auto failed = (*op)->Next();
  ASSERT_FALSE(failed.ok());
  EXPECT_TRUE(failed.status().IsIOError()) << failed.status().ToString();
  EXPECT_TRUE((*op)->Close().ok());
  EXPECT_EQ(second.closes(), 1) << "the failing input is closed";
  EXPECT_EQ(third.opens(), 0) << "an input after the failure is never opened";
}

// An input whose Open fails is still closed, as BuildsFirstOperator closes its sink.
TEST_F(UnionAllTest, AnInputWhoseOpenFailsIsClosed) {
  class FailingOpen final : public Operator {
   public:
    [[nodiscard]] const std::shared_ptr<arrow::Schema>& output_schema() const override {
      return schema_;
    }
    arrow::Status Open(ExecContext& /*ctx*/) override {
      return arrow::Status::IOError("cannot open");
    }
    arrow::Result<Batch> Next() override { return Batch{}; }
    arrow::Status Close() override {
      ++*closes_;
      return arrow::Status::OK();
    }
    explicit FailingOpen(int* closes) : closes_(closes) {}

   private:
    std::shared_ptr<arrow::Schema> schema_ = TextThenKey();
    int* closes_;
  };
  int closes = 0;
  auto op = UnionOf(
      std::make_unique<ScriptedSource>(KeyThenText(), std::vector<Batch>{KeyText({1}, {"a"})}),
      std::make_unique<FailingOpen>(&closes));
  ASSERT_TRUE(op.ok()) << op.status().ToString();
  ExecContext ctx;
  auto table = Drain(**op, ctx);
  ASSERT_FALSE(table.ok());
  EXPECT_TRUE(table.status().IsIOError()) << table.status().ToString();
  EXPECT_EQ(closes, 1);
}

// A source of one batch whose first Close fails; it counts its opens and closes.
class FailingClose final : public Operator {
 public:
  [[nodiscard]] const std::shared_ptr<arrow::Schema>& output_schema() const override {
    return schema_;
  }
  arrow::Status Open(ExecContext& /*ctx*/) override {
    ++opens_;
    given_ = false;
    return arrow::Status::OK();
  }
  arrow::Result<Batch> Next() override {
    if (given_) {
      return Batch{};
    }
    given_ = true;
    return KeyText({7}, {"x"});
  }
  arrow::Status Close() override {
    ++closes_;
    return closes_ == 1 ? arrow::Status::IOError("cannot close") : arrow::Status::OK();
  }
  [[nodiscard]] int opens() const { return opens_; }
  [[nodiscard]] int closes() const { return closes_; }

 private:
  std::shared_ptr<arrow::Schema> schema_ = KeyThenText();
  bool given_ = false;
  int opens_ = 0;
  int closes_ = 0;
};

// An input whose Close fails at its end fails the Next that reached it, after its rows: the input
// after it is never opened, and the union's own Close does not close the failed input again.
TEST_F(UnionAllTest, AnInputThatFailsToCloseAtItsEndFailsTheNext) {
  auto owned_first = std::make_unique<FailingClose>();
  auto owned_second =
      std::make_unique<ScriptedSource>(TextThenKey(), std::vector<Batch>{TextKey({"c"}, {3})});
  const FailingClose& first = *owned_first;
  const ScriptedSource& second = *owned_second;
  auto op = UnionOf(std::move(owned_first), std::move(owned_second));
  ASSERT_TRUE(op.ok()) << op.status().ToString();
  ExecContext ctx;
  ASSERT_TRUE((*op)->Open(ctx).ok());
  auto rows = (*op)->Next();
  ASSERT_TRUE(rows.ok()) << rows.status().ToString();
  EXPECT_EQ(rows->selected_rows(), 1);
  auto failed = (*op)->Next();
  ASSERT_FALSE(failed.ok());
  EXPECT_TRUE(failed.status().IsIOError()) << failed.status().ToString();
  EXPECT_EQ(second.opens(), 0);
  EXPECT_TRUE((*op)->Close().ok());
  EXPECT_EQ(first.closes(), 1) << "closed once, at its end";
}

// The union's Close passes on the open input's failure, and the union opens again afterwards.
TEST_F(UnionAllTest, CloseReportsTheOpenInputsFailureAndReopens) {
  auto owned_first = std::make_unique<FailingClose>();
  const FailingClose& first = *owned_first;
  auto op = UnionOf(std::move(owned_first),
                    std::make_unique<ScriptedSource>(TextThenKey(), std::vector<Batch>{}));
  ASSERT_TRUE(op.ok()) << op.status().ToString();
  ExecContext ctx;
  ASSERT_TRUE((*op)->Open(ctx).ok());
  ASSERT_TRUE((*op)->Next().ok());  // input 0 is open
  const arrow::Status closed = (*op)->Close();
  EXPECT_TRUE(closed.IsIOError()) << closed.ToString();
  EXPECT_EQ(first.closes(), 1);

  auto again = Drain(**op, ctx);  // Drain opens it again
  ASSERT_TRUE(again.ok()) << again.status().ToString();
  EXPECT_EQ(Int64Column(**again, 0), (std::vector<std::optional<int64_t>>{7}));
  EXPECT_EQ(first.opens(), 2);
}

// Inputs without rows add none, and a union of empty inputs is empty under its own schema.
TEST_F(UnionAllTest, EmptyInputsAddNoRows) {
  auto op = UnionOf(std::make_unique<ScriptedSource>(KeyThenText(), std::vector<Batch>{}),
                    std::make_unique<ScriptedSource>(TextThenKey(), std::vector<Batch>{}));
  ASSERT_TRUE(op.ok()) << op.status().ToString();
  ExecContext ctx;
  auto empty = Drain(**op, ctx);
  ASSERT_TRUE(empty.ok()) << empty.status().ToString();
  EXPECT_EQ((*empty)->num_rows(), 0);
  EXPECT_TRUE((*empty)->schema()->Equals(*(*op)->output_schema()));

  auto partly = UnionOf(
      std::make_unique<ScriptedSource>(KeyThenText(), std::vector<Batch>{}),
      std::make_unique<ScriptedSource>(TextThenKey(), std::vector<Batch>{TextKey({"c"}, {3})}));
  ASSERT_TRUE(partly.ok());
  auto rows = Drain(**partly, ctx);
  ASSERT_TRUE(rows.ok()) << rows.status().ToString();
  EXPECT_EQ(Int64Column(**rows, 0), (std::vector<std::optional<int64_t>>{3}));
}

// What a correct plan never holds is Invalid: fewer than two inputs, a column list per input that
// does not match the types, a column outside its input or of another type, a call out of order.
TEST_F(UnionAllTest, RejectsWhatItCannotUnion) {
  const auto source = [] {
    return std::make_unique<ScriptedSource>(KeyThenText(), std::vector<Batch>{});
  };
  const std::vector<LogicalType> types = {LogicalType::kBigInt, LogicalType::kVarchar};
  const auto make =
      [&](std::vector<std::vector<int>> columns,
          const std::vector<LogicalType>& as = {LogicalType::kBigInt, LogicalType::kVarchar}) {
        std::vector<std::unique_ptr<Operator>> inputs;
        inputs.push_back(source());
        inputs.push_back(source());
        return UnionAllOperator::Make(std::move(inputs), std::move(columns), as).status();
      };
  std::vector<std::unique_ptr<Operator>> one;
  one.push_back(source());
  EXPECT_TRUE(UnionAllOperator::Make(std::move(one), {{0, 1}}, types).status().IsInvalid());
  std::vector<std::unique_ptr<Operator>> missing;
  missing.push_back(source());
  missing.push_back(nullptr);
  EXPECT_TRUE(
      UnionAllOperator::Make(std::move(missing), {{0, 1}, {0, 1}}, types).status().IsInvalid())
      << "an input that is not there";
  EXPECT_TRUE(make({{0, 1}}).IsInvalid()) << "one column list for two inputs";
  EXPECT_TRUE(make({{0, 1}, {0}}).IsInvalid()) << "an input without a column per type";
  EXPECT_TRUE(make({{0, 1}, {0, 2}}).IsInvalid()) << "a column outside its input";
  EXPECT_TRUE(make({{0, 1}, {1, 0}}).IsInvalid()) << "input 1's column 0 is VARCHAR";
  EXPECT_TRUE(make({{0, 1}, {0, 1}}, {LogicalType::kInteger, LogicalType::kVarchar}).IsInvalid())
      << "a BIGINT column is not an INTEGER one";
  EXPECT_TRUE(make({{0, 1}, {0, 1}}).ok());

  auto op =
      UnionOf(source(), std::make_unique<ScriptedSource>(TextThenKey(), std::vector<Batch>{}));
  ASSERT_TRUE(op.ok());
  EXPECT_TRUE((*op)->Next().status().IsInvalid()) << "Next before Open";
}

// An input that hands on a batch of other types than its schema says is a bug of that input, and
// the union reports it rather than labelling the arrays with types they do not have.
TEST_F(UnionAllTest, ABatchOfAnotherTypeIsInvalid) {
  const Batch wrong{
      .data = arrow::RecordBatch::Make(
          arrow::schema({arrow::field("k", arrow::binary()), arrow::field("s", arrow::binary())}),
          1, {Strings({"x"}), Strings({"y"})}),
      .selection = nullptr};
  auto op = UnionOf(std::make_unique<ScriptedSource>(KeyThenText(), std::vector<Batch>{wrong}),
                    std::make_unique<ScriptedSource>(TextThenKey(), std::vector<Batch>{}));
  ASSERT_TRUE(op.ok()) << op.status().ToString();
  ExecContext ctx;
  auto table = Drain(**op, ctx);
  ASSERT_FALSE(table.ok());
  EXPECT_TRUE(table.status().IsInvalid()) << table.status().ToString();
}

}  // namespace
}  // namespace antb1::exec
