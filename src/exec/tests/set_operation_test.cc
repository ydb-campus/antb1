// UNION [DISTINCT], INTERSECT [ALL] and EXCEPT [ALL] as one operator
// (docs/adr/0039-set-operations.md): each operator and mixed formulas, NULLs, the spelling of -0.0
// and NaN, the starters that insert and the inputs that only look up, ALL repetition, errors, the
// budget, and a randomized comparison with the multiset definitions.

#include "../set_operation.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include <arrow/api.h>
#include <gtest/gtest.h>

#include "antb1/exec/memory_budget.h"
#include "antb1/exec/operator.h"
#include "antb1/exec/profile.h"
#include "antb1/plan/logical_plan.h"
#include "antb1/plan/types.h"

#include "exec_test_util.h"

namespace antb1::exec {
namespace {

using plan::LogicalType;
using plan::SetTerm;
using plan::SetTermKind;
using testing::Int64s;
using testing::ScriptedSource;
using testing::Strings;

// A row of the inputs: (k BIGINT, s VARCHAR), either NULL.
using Row = std::pair<std::optional<int64_t>, std::optional<std::string>>;
using Rows = std::vector<Row>;

std::shared_ptr<arrow::Schema> RowSchema() {
  return arrow::schema({arrow::field("k", arrow::int64()), arrow::field("s", arrow::binary())});
}

// `rows` in batches of `batch` rows; with `select_odd`, each batch also carries every row twice,
// the copies deselected, so a selection is applied before anything is counted.
std::unique_ptr<ScriptedSource> Source(const Rows& rows, std::size_t batch = 1024,
                                       bool select_odd = false) {
  std::vector<Batch> batches;
  for (std::size_t begin = 0; begin < rows.size(); begin += batch) {
    const std::size_t end = std::min(rows.size(), begin + batch);
    std::vector<std::optional<int64_t>> keys;
    std::vector<std::optional<std::string>> texts;
    std::vector<std::optional<bool>> selected;
    for (std::size_t i = begin; i < end; ++i) {
      keys.push_back(rows[i].first);
      texts.push_back(rows[i].second);
      selected.emplace_back(true);
      if (select_odd) {
        keys.emplace_back(-1);  // a row that must not count
        texts.emplace_back("deselected");
        selected.emplace_back(false);
      }
    }
    const auto n = static_cast<int64_t>(keys.size());
    batches.push_back(
        Batch{.data = arrow::RecordBatch::Make(RowSchema(), n, {Int64s(keys), Strings(texts)}),
              .selection = select_odd ? testing::Bools(selected) : nullptr});
  }
  return std::make_unique<ScriptedSource>(RowSchema(), std::move(batches));
}

SetTerm In(std::size_t input) {
  return SetTerm{.kind = SetTermKind::kInput, .input = input, .operands = 0};
}
SetTerm Op(SetTermKind kind, std::size_t operands = 2) {
  return SetTerm{.kind = kind, .input = 0, .operands = operands};
}

arrow::Result<std::unique_ptr<SetOperationOperator>> Make(
    std::vector<std::unique_ptr<Operator>> inputs, std::vector<SetTerm> formula) {
  std::vector<std::vector<int>> columns(inputs.size(), std::vector<int>{0, 1});
  return SetOperationOperator::Make(std::move(inputs), std::move(columns),
                                    {LogicalType::kBigInt, LogicalType::kVarchar},
                                    std::move(formula));
}

// The rows of a drained result, sorted (row order is unspecified).
Rows Sorted(const arrow::Table& table) {
  Rows out;
  if (table.num_rows() == 0) {
    return out;
  }
  const auto keys = testing::Int64Column(table, 0);
  const auto texts = std::static_pointer_cast<arrow::BinaryArray>(
      table.column(1)->num_chunks() == 1
          ? table.column(1)->chunk(0)
          : arrow::Concatenate(table.column(1)->chunks()).ValueOrDie());
  for (int64_t i = 0; i < table.num_rows(); ++i) {
    out.emplace_back(
        keys[static_cast<std::size_t>(i)],
        texts->IsNull(i) ? std::nullopt : std::optional<std::string>(texts->GetString(i)));
  }
  std::ranges::sort(out);
  return out;
}

// The sorted result of `formula` over `inputs`, each in batches of `batch` rows.
Rows RunSet(const std::vector<Rows>& inputs, std::vector<SetTerm> formula, std::size_t batch = 1024,
            bool select_odd = false) {
  std::vector<std::unique_ptr<Operator>> sources;
  for (const Rows& rows : inputs) {
    sources.push_back(Source(rows, batch, select_odd));
  }
  auto op = Make(std::move(sources), std::move(formula));
  EXPECT_TRUE(op.ok()) << op.status().ToString();
  if (!op.ok()) {
    return {};
  }
  ExecContext ctx;
  auto table = Drain(**op, ctx);
  EXPECT_TRUE(table.ok()) << table.status().ToString();
  return table.ok() ? Sorted(**table) : Rows{};
}

Row R(std::optional<int64_t> k, std::optional<std::string> s = "x") { return {k, std::move(s)}; }

class SetOperationTest : public testing::ExecTest {};

// Each operator on its own, NULL a value like any other, and the DISTINCT forms remove the left
// input's duplicates even where nothing matches.
TEST_F(SetOperationTest, EachOperator) {
  const Rows a = {R(1), R(1), R(2), R(std::nullopt), R(std::nullopt), R(3)};
  const Rows b = {R(2), R(3), R(3), R(std::nullopt), R(4)};
  const Rows empty;
  EXPECT_EQ(RunSet({a, b}, {In(0), In(1), Op(SetTermKind::kUnion)}),
            (Rows{R(std::nullopt), R(1), R(2), R(3), R(4)}));
  EXPECT_EQ(RunSet({a, b}, {In(0), In(1), Op(SetTermKind::kIntersect)}),
            (Rows{R(std::nullopt), R(2), R(3)}));
  EXPECT_EQ(RunSet({a, b}, {In(0), In(1), Op(SetTermKind::kExcept)}), (Rows{R(1)}));
  EXPECT_EQ(RunSet({a, empty}, {In(0), In(1), Op(SetTermKind::kExcept)}),
            (Rows{R(std::nullopt), R(1), R(2), R(3)}));
  // min(m, n) and max(m - n, 0) copies.
  EXPECT_EQ(RunSet({a, b}, {In(0), In(1), Op(SetTermKind::kIntersectAll)}),
            (Rows{R(std::nullopt), R(2), R(3)}));
  EXPECT_EQ(RunSet({b, a}, {In(0), In(1), Op(SetTermKind::kIntersectAll)}),
            (Rows{R(std::nullopt), R(2), R(3)}));
  EXPECT_EQ(RunSet({a, b}, {In(0), In(1), Op(SetTermKind::kExceptAll)}),
            (Rows{R(std::nullopt), R(1), R(1)}));
  EXPECT_EQ(RunSet({b, a}, {In(0), In(1), Op(SetTermKind::kExceptAll)}), (Rows{R(3), R(4)}));
  EXPECT_EQ(RunSet({a, empty}, {In(0), In(1), Op(SetTermKind::kExceptAll)}),
            (Rows{R(std::nullopt), R(std::nullopt), R(1), R(1), R(2), R(3)}));
  // n-ary runs: three inputs at once.
  const Rows c = {R(3), R(5)};
  EXPECT_EQ(RunSet({a, b, c}, {In(0), In(1), In(2), Op(SetTermKind::kIntersect, 3)}), (Rows{R(3)}));
  EXPECT_EQ(RunSet({a, b, c}, {In(0), In(1), In(2), Op(SetTermKind::kExcept, 3)}), (Rows{R(1)}));
  EXPECT_EQ(RunSet({a, b, c}, {In(0), In(1), In(2), Op(SetTermKind::kUnion, 3)}),
            (Rows{R(std::nullopt), R(1), R(2), R(3), R(4), R(5)}));
}

// A row is all its columns, NULL equal to NULL in each: (1, NULL) is no (1, 'x').
TEST_F(SetOperationTest, PartialNullsCompareColumnByColumn) {
  const Rows a = {
      R(1, std::nullopt), R(std::nullopt, "a"),         R(std::nullopt, std::nullopt), R(1, "a"),
      R(2, "b"),          R(std::nullopt, std::nullopt)};
  const Rows b = {R(1, std::nullopt), R(std::nullopt, std::nullopt), R(2, "b"),
                  R(std::nullopt, "b")};
  EXPECT_EQ(RunSet({a, b}, {In(0), In(1), Op(SetTermKind::kIntersect)}),
            (Rows{R(std::nullopt, std::nullopt), R(1, std::nullopt), R(2, "b")}));
  EXPECT_EQ(RunSet({a, b}, {In(0), In(1), Op(SetTermKind::kExcept)}),
            (Rows{R(std::nullopt, "a"), R(1, "a")}));
  EXPECT_EQ(RunSet({a, b}, {In(0), In(1), Op(SetTermKind::kExceptAll)}),
            (Rows{R(std::nullopt, std::nullopt), R(std::nullopt, "a"), R(1, "a")}));
}

// Mixed trees in one formula: A UNION (B INTERSECT C), (A INTERSECT ALL B) EXCEPT ALL C, and
// (A EXCEPT ALL B) UNION C -- whose EXCEPT ALL needs counts even though only presence is emitted.
TEST_F(SetOperationTest, MixedFormulas) {
  const Rows a = {R(1), R(1), R(1), R(2)};
  const Rows b = {R(1), R(1), R(3), R(4)};
  const Rows c = {R(1), R(3), R(5)};
  EXPECT_EQ(RunSet({a, b, c},
                   {In(0), In(1), In(2), Op(SetTermKind::kIntersect), Op(SetTermKind::kUnion)}),
            (Rows{R(1), R(2), R(3)}));
  EXPECT_EQ(RunSet({a, b, c}, {In(0), In(1), Op(SetTermKind::kIntersectAll), In(2),
                               Op(SetTermKind::kExceptAll)}),
            (Rows{R(1)}));
  EXPECT_EQ(RunSet({a, b, c},
                   {In(0), In(1), Op(SetTermKind::kExceptAll), In(2), Op(SetTermKind::kUnion)}),
            (Rows{R(1), R(2), R(3), R(5)}));
  // C EXCEPT (A INTERSECT B): only C inserts; A and B look up.
  EXPECT_EQ(RunSet({c, a, b},
                   {In(0), In(1), In(2), Op(SetTermKind::kIntersect), Op(SetTermKind::kExcept)}),
            (Rows{R(3), R(5)}));
}

// The memberships: starters insert their groups, the others add none, so the table holds the
// starters' distinct rows only -- the "groups" metric of the profile.
TEST_F(SetOperationTest, OnlyStartersInsertGroups) {
  std::vector<std::unique_ptr<Operator>> inputs;
  inputs.push_back(Source({R(1), R(2), R(2)}));
  inputs.push_back(Source({R(2), R(3), R(4), R(5), R(6)}));
  inputs.push_back(Source({R(1), R(7), R(8)}));
  auto op = Make(std::move(inputs), {In(0), In(1), In(2), Op(SetTermKind::kIntersect, 3)});
  ASSERT_TRUE(op.ok()) << op.status().ToString();
  ProfileNode profile;
  (*op)->set_profile(&profile);
  ExecContext ctx;
  auto table = Drain(**op, ctx);
  ASSERT_TRUE(table.ok()) << table.status().ToString();
  EXPECT_EQ((*table)->num_rows(), 0);
  std::map<std::string, int64_t> metrics;
  for (const ProfileMetric& metric : profile.metrics()) {
    metrics[metric.name] = metric.value;
  }
  EXPECT_EQ(metrics["groups"], 2) << "input 0's distinct rows only";
  EXPECT_EQ(metrics["starter_rows"], 3);
  EXPECT_EQ(metrics["lookup_rows"], 8);
  EXPECT_EQ(metrics["lookup_hits"], 2);
}

// -0.0 equals 0.0 and every NaN equals every other. A DISTINCT root shows the value first seen
// among the starters; an ALL root the normalized one (0.0, NaN), as DuckDB prints the ALL forms.
TEST_F(SetOperationTest, DoubleSpelling) {
  const auto schema = arrow::schema({arrow::field("d", arrow::float64())});
  const auto doubles = [&schema](std::vector<double> values) {
    std::vector<Batch> batches;
    arrow::DoubleBuilder builder;
    EXPECT_TRUE(builder.AppendValues(values).ok());
    const auto n = static_cast<int64_t>(values.size());
    batches.push_back(Batch{.data = arrow::RecordBatch::Make(schema, n, {*builder.Finish()}),
                            .selection = nullptr});
    return std::make_unique<ScriptedSource>(schema, std::move(batches));
  };
  const double nan = std::numeric_limits<double>::quiet_NaN();
  const auto run = [&](std::vector<double> first, std::vector<double> second, SetTermKind kind) {
    std::vector<std::unique_ptr<Operator>> inputs;
    inputs.push_back(doubles(std::move(first)));
    inputs.push_back(doubles(std::move(second)));
    auto op = SetOperationOperator::Make(std::move(inputs), {{0}, {0}}, {LogicalType::kDouble},
                                         {In(0), In(1), Op(kind)});
    EXPECT_TRUE(op.ok()) << op.status().ToString();
    ExecContext ctx;
    auto table = Drain(**op, ctx);
    EXPECT_TRUE(table.ok()) << table.status().ToString();
    std::vector<double> out;
    const auto column = (*table)->column(0);
    for (const auto& chunk : column->chunks()) {
      const auto& values = static_cast<const arrow::DoubleArray&>(*chunk);
      for (int64_t i = 0; i < values.length(); ++i) {
        out.push_back(values.Value(i));
      }
    }
    return out;
  };
  const auto one = [](const std::vector<double>& values) {
    EXPECT_EQ(values.size(), 1U);
    return values.empty() ? 1.0 : values.front();
  };
  EXPECT_TRUE(std::signbit(one(run({-0.0}, {0.0}, SetTermKind::kUnion))));
  EXPECT_FALSE(std::signbit(one(run({0.0}, {-0.0}, SetTermKind::kUnion))));
  EXPECT_TRUE(std::signbit(one(run({-0.0}, {0.0}, SetTermKind::kIntersect))));
  EXPECT_FALSE(std::signbit(one(run({-0.0}, {0.0}, SetTermKind::kIntersectAll))));
  EXPECT_FALSE(std::signbit(one(run({-0.0}, {}, SetTermKind::kExceptAll))));
  EXPECT_TRUE(std::isnan(one(run({nan, -nan}, {nan}, SetTermKind::kIntersect))));
  EXPECT_FALSE(std::signbit(one(run({-nan}, {nan}, SetTermKind::kIntersectAll))));
  // In (B EXCEPT C) UNION A, B is a starter: its -0.0 is the one first seen (ADR 0039's D29).
  std::vector<std::unique_ptr<Operator>> inputs;
  inputs.push_back(doubles({-0.0}));
  inputs.push_back(doubles({0.0}));
  inputs.push_back(doubles({0.0}));
  auto op = SetOperationOperator::Make(
      std::move(inputs), {{0}, {0}, {0}}, {LogicalType::kDouble},
      {In(0), In(1), Op(SetTermKind::kExcept), In(2), Op(SetTermKind::kUnion)});
  ASSERT_TRUE(op.ok()) << op.status().ToString();
  ExecContext ctx;
  auto table = Drain(**op, ctx);
  ASSERT_TRUE(table.ok());
  ASSERT_EQ((*table)->num_rows(), 1);
  EXPECT_TRUE(std::signbit(
      std::static_pointer_cast<arrow::DoubleArray>((*table)->column(0)->chunk(0))->Value(0)));
}

// Empty inputs, batches of one row, and selections: a deselected row counts nowhere.
TEST_F(SetOperationTest, EmptyInputsSmallBatchesAndSelections) {
  const Rows a = {R(1), R(2), R(2), R(3)};
  const Rows b = {R(2), R(4)};
  for (const std::size_t batch : {std::size_t{1}, std::size_t{3}}) {
    for (const bool select_odd : {false, true}) {
      EXPECT_EQ(RunSet({a, b}, {In(0), In(1), Op(SetTermKind::kExceptAll)}, batch, select_odd),
                (Rows{R(1), R(2), R(3)}));
      EXPECT_EQ(RunSet({{}, b}, {In(0), In(1), Op(SetTermKind::kUnion)}, batch, select_odd),
                (Rows{R(2), R(4)}));
      EXPECT_EQ(RunSet({{}, {}}, {In(0), In(1), Op(SetTermKind::kIntersect)}, batch, select_odd),
                Rows{});
    }
  }
}

// An ALL root repeats a group by its copies, across output batches when they are many.
TEST_F(SetOperationTest, AllRootRepeatsAcrossBatches) {
  const int64_t copies = SetOperationOperator::kMaxRepeatedRows + 100;
  Rows a(static_cast<std::size_t>(copies), R(7));
  a.push_back(R(8));
  Rows b(static_cast<std::size_t>(copies + 5), R(7));
  std::vector<std::unique_ptr<Operator>> inputs;
  inputs.push_back(Source(a, 4096));
  inputs.push_back(Source(b, 4096));
  auto op = Make(std::move(inputs), {In(0), In(1), Op(SetTermKind::kIntersectAll)});
  ASSERT_TRUE(op.ok());
  ExecContext ctx;
  ASSERT_TRUE((*op)->Open(ctx).ok());
  int64_t rows = 0;
  int batches = 0;
  while (true) {
    auto batch = (*op)->Next();
    ASSERT_TRUE(batch.ok()) << batch.status().ToString();
    if (batch->end()) {
      break;
    }
    ++batches;
    EXPECT_LE(batch->data->num_rows(), SetOperationOperator::kMaxRepeatedRows);
    rows += batch->data->num_rows();
  }
  ASSERT_TRUE((*op)->Close().ok());
  EXPECT_EQ(rows, copies);
  EXPECT_EQ(batches, 2);
}

// The inputs open one at a time, each closed before the next opens; every input is read in full,
// so the first that fails decides the error, and the inputs after it never open.
TEST_F(SetOperationTest, InputsOpenInTurnAndTheFirstFailureDecides) {
  auto first = Source({R(1)});
  auto second = Source({R(1), R(2)}, 1);
  auto third = Source({R(1)});
  second->FailAt(1);
  third->FailAt(0);
  ScriptedSource* second_ptr = second.get();
  ScriptedSource* third_ptr = third.get();
  std::vector<std::unique_ptr<Operator>> inputs;
  inputs.push_back(std::move(first));
  inputs.push_back(std::move(second));
  inputs.push_back(std::move(third));
  auto op = Make(std::move(inputs), {In(0), In(1), In(2), Op(SetTermKind::kUnion, 3)});
  ASSERT_TRUE(op.ok());
  ExecContext ctx;
  auto table = Drain(**op, ctx);
  ASSERT_FALSE(table.ok());
  EXPECT_TRUE(table.status().IsIOError()) << table.status().ToString();
  EXPECT_EQ(second_ptr->closes(), 1) << "Close closes the input that failed";
  EXPECT_EQ(third_ptr->opens(), 0) << "an input after the failure never opens";
}

// The table and the memberships count against the budget: a small one fails the query.
TEST_F(SetOperationTest, TheBudgetBoundsTheTable) {
  Rows a;
  for (int64_t i = 0; i < 20000; ++i) {
    a.push_back(R(i, std::string(32, 'x')));
  }
  std::vector<std::unique_ptr<Operator>> inputs;
  inputs.push_back(Source(a));
  inputs.push_back(Source(a));
  auto op = Make(std::move(inputs), {In(0), In(1), Op(SetTermKind::kIntersect)});
  ASSERT_TRUE(op.ok());
  MemoryBudget budget(64 * 1024, arrow::default_memory_pool());
  ExecContext ctx{.pool = &budget, .budget = &budget};
  auto table = Drain(**op, ctx);
  ASSERT_FALSE(table.ok());
  EXPECT_TRUE(table.status().IsOutOfMemory()) << table.status().ToString();
}

// The bits and counts are charged too: four more counted inputs, empty, charge a count per group
// each, and Close gives every byte back once the result is dropped. A EXCEPT ALL A keeps no row, so
// the peak is the memberships' and not the output's.
TEST_F(SetOperationTest, TheBudgetChargesTheMemberships) {
  constexpr int64_t kGroups = 4096;
  Rows a;
  for (int64_t i = 0; i < kGroups; ++i) {
    a.push_back(R(i));
  }
  const auto peak = [&a](std::size_t n) {
    std::vector<std::unique_ptr<Operator>> inputs;
    inputs.push_back(Source(a));
    inputs.push_back(Source(a));
    std::vector<SetTerm> formula{In(0), In(1)};
    for (std::size_t k = 2; k < n; ++k) {
      inputs.push_back(Source(Rows{}));
      formula.push_back(In(k));
    }
    formula.push_back(Op(SetTermKind::kExceptAll, n));
    auto op = Make(std::move(inputs), std::move(formula));
    EXPECT_TRUE(op.ok());
    MemoryBudget budget(std::nullopt, arrow::default_memory_pool());
    ExecContext ctx{.pool = &budget, .budget = &budget};
    {
      auto table = Drain(**op, ctx);
      EXPECT_TRUE(table.ok()) << table.status().ToString();
      EXPECT_EQ((*table)->num_rows(), 0);
    }
    EXPECT_EQ(budget.bytes_allocated(), 0) << n << " inputs, after Drain's Close";
    return budget.max_memory();
  };
  EXPECT_GE(peak(6) - peak(2), 4 * kGroups * int64_t{sizeof(int64_t)});
}

// A run reopens the inputs and gives the same rows.
TEST_F(SetOperationTest, ReopensForASecondRun) {
  std::vector<std::unique_ptr<Operator>> inputs;
  inputs.push_back(Source({R(1), R(2), R(2)}));
  inputs.push_back(Source({R(2)}));
  auto op = Make(std::move(inputs), {In(0), In(1), Op(SetTermKind::kExceptAll)});
  ASSERT_TRUE(op.ok());
  for (int run = 0; run < 2; ++run) {
    ExecContext ctx;
    auto table = Drain(**op, ctx);
    ASSERT_TRUE(table.ok()) << table.status().ToString();
    EXPECT_EQ(Sorted(**table), (Rows{R(1), R(2)})) << run;
  }
}

TEST_F(SetOperationTest, MakeChecksTheFormulaAndTheColumns) {
  const auto inputs = [] {
    std::vector<std::unique_ptr<Operator>> out;
    out.push_back(Source({R(1)}));
    out.push_back(Source({R(1)}));
    return out;
  };
  EXPECT_FALSE(Make(inputs(), {In(0), In(1)}).ok()) << "two roots";
  EXPECT_FALSE(Make(inputs(), {In(1), In(0), Op(SetTermKind::kUnion)}).ok()) << "out of order";
  EXPECT_FALSE(Make(inputs(), {In(0), Op(SetTermKind::kUnion, 1)}).ok()) << "input 1 unread";
  EXPECT_FALSE(Make(inputs(), {In(0), In(1), Op(SetTermKind::kUnion, 3)}).ok()) << "operands";
  EXPECT_FALSE(SetOperationOperator::Make(inputs(), {{0, 1}, {1, 0}},
                                          {LogicalType::kBigInt, LogicalType::kVarchar},
                                          {In(0), In(1), Op(SetTermKind::kUnion)})
                   .ok())
      << "input 1's columns are of the other types";
}

// ---- a randomized comparison with the multiset definitions ----

// Each row's copies under a formula, from its count in each input: UNION, INTERSECT and EXCEPT as
// presence, the ALL forms as min and max(first - rest, 0) -- the definitions, term by term.
int64_t Copies(const std::vector<SetTerm>& formula, const std::vector<int64_t>& counts) {
  std::vector<int64_t> stack;
  for (const SetTerm& term : formula) {
    if (term.kind == SetTermKind::kInput) {
      stack.push_back(counts[term.input]);
      continue;
    }
    const std::vector<int64_t> operands(stack.end() - static_cast<std::ptrdiff_t>(term.operands),
                                        stack.end());
    stack.resize(stack.size() - term.operands);
    int64_t value = 0;
    switch (term.kind) {
      case SetTermKind::kUnion:
        value = std::ranges::any_of(operands, [](int64_t c) { return c > 0; }) ? 1 : 0;
        break;
      case SetTermKind::kIntersect:
        value = std::ranges::all_of(operands, [](int64_t c) { return c > 0; }) ? 1 : 0;
        break;
      case SetTermKind::kExcept:
        value = operands[0] > 0 && std::all_of(operands.begin() + 1, operands.end(),
                                               [](int64_t c) { return c == 0; })
                    ? 1
                    : 0;
        break;
      case SetTermKind::kIntersectAll:
        value = std::ranges::min(operands);
        break;
      case SetTermKind::kExceptAll:
        value = operands[0];
        for (std::size_t i = 1; i < operands.size(); ++i) {
          value -= operands[i];
        }
        value = std::max<int64_t>(value, 0);
        break;
      case SetTermKind::kInput:
        break;
    }
    stack.push_back(value);
  }
  return stack.back();
}

// A random formula over `inputs` inputs: a random tree, inputs in order, operators of 2 or 3
// operands.
std::vector<SetTerm> RandomFormula(std::mt19937_64& rng, std::size_t inputs) {
  static constexpr std::array kKinds = {SetTermKind::kUnion, SetTermKind::kIntersect,
                                        SetTermKind::kExcept, SetTermKind::kIntersectAll,
                                        SetTermKind::kExceptAll};
  std::vector<SetTerm> formula;
  std::size_t next = 0;
  std::size_t depth = 0;
  while (next < inputs || depth > 1) {
    const bool can_combine = depth >= 2;
    const bool must_combine = next == inputs;
    if (must_combine || (can_combine && rng() % 3 == 0)) {
      const std::size_t operands = depth >= 3 && rng() % 3 == 0 ? 3 : 2;
      formula.push_back(Op(kKinds[rng() % kKinds.size()], operands));
      depth -= operands - 1;
    } else {
      formula.push_back(In(next++));
      ++depth;
    }
  }
  return formula;
}

TEST_F(SetOperationTest, RandomFormulasMatchTheDefinitions) {
  int compared = 0;
  for (const std::uint64_t seed : {1U, 2U, 3U, 4U, 5U}) {
    std::mt19937_64 rng(seed);
    for (int trial = 0; trial < 60; ++trial) {
      const std::size_t n = 2 + rng() % 4;
      const std::vector<SetTerm> formula = RandomFormula(rng, n);
      std::vector<Rows> inputs(n);
      std::map<Row, std::vector<int64_t>> counts;
      for (std::size_t k = 0; k < n; ++k) {
        const std::size_t rows = rng() % 12;
        for (std::size_t r = 0; r < rows; ++r) {
          const std::optional<int64_t> key =
              rng() % 5 == 0 ? std::nullopt : std::optional<int64_t>(rng() % 4);
          const std::optional<std::string> text =
              rng() % 4 == 0 ? std::nullopt
                             : std::optional<std::string>(rng() % 2 == 0 ? "a" : "b");
          inputs[k].emplace_back(key, text);
          std::vector<int64_t>& c = counts[{key, text}];
          c.resize(n, 0);
          ++c[k];
        }
      }
      Rows expected;
      for (const auto& [row, c] : counts) {
        const int64_t copies = Copies(formula, c);
        for (int64_t i = 0; i < copies; ++i) {
          expected.push_back(row);
        }
      }
      std::ranges::sort(expected);
      for (const std::size_t batch : {std::size_t{1}, std::size_t{7}}) {
        EXPECT_EQ(RunSet(inputs, formula, batch), expected)
            << "seed " << seed << " trial " << trial << " batch " << batch;
        ++compared;
      }
    }
  }
  EXPECT_EQ(compared, 600);
}

}  // namespace
}  // namespace antb1::exec
