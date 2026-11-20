#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <arrow/api.h>
#include <gtest/gtest.h>

#include "antb1/common/narrow.h"
#include "antb1/exec/memory_budget.h"

#include "../group_table.h"
#include "exec_test_util.h"

namespace antb1::exec {
namespace {

using plan::LogicalType;
using testing::Column;
using Mode = GroupTableMode;

class GroupTableProgramTest : public testing::ExecTest {};

struct Query {
  std::vector<plan::BoundColumn> keys;
  std::vector<plan::AggregateCall> calls;
  std::shared_ptr<arrow::Schema> input;
  std::shared_ptr<arrow::Schema> output;

  std::unique_ptr<GroupTable> Make(Mode mode, MemoryBudget& budget, bool first = true,
                                   const KeyTableOptions& options = {
                                       .kind = KeyTableKind::kFixed}) const {
    auto made = GroupTable::Make(keys, calls, input->num_fields(), &budget, &budget, first,
                                 GroupTable::kPartitions, options, mode);
    EXPECT_TRUE(made.ok()) << made.status();
    return made.ok() ? std::move(*made) : nullptr;
  }
};

Query NumericQuery(bool composite) {
  Query query;
  query.keys = {Column(0, "k", LogicalType::kBigInt)};
  auto fields = arrow::FieldVector{arrow::field("k", arrow::int64())};
  if (composite) {
    query.keys.push_back(Column(1, "b", LogicalType::kBoolean));
    fields.push_back(arrow::field("b", arrow::boolean()));
  }
  auto output = fields;
  query.calls = {{.kind = plan::AggKind::kCountStar, .type = LogicalType::kBigInt},
                 {.kind = plan::AggKind::kSum,
                  .arg = Column(Narrow<int>(fields.size()), "v", LogicalType::kDouble),
                  .type = LogicalType::kDouble}};
  fields.push_back(arrow::field("v", arrow::float64()));
  output.push_back(arrow::field("n", arrow::int64()));
  output.push_back(arrow::field("total", arrow::float64()));
  query.input = arrow::schema(std::move(fields));
  query.output = arrow::schema(std::move(output));
  return query;
}

std::shared_ptr<arrow::RecordBatch> MakeRows(const Query& query, int part) {
  std::vector<std::optional<std::int64_t>> keys;
  std::vector<std::optional<bool>> flags;
  arrow::DoubleBuilder payload;
  constexpr int kRows = 1603;
  keys.reserve(kRows);
  flags.reserve(kRows);
  for (int i = 0; i < kRows; ++i) {
    keys.emplace_back(i % 31 == 0 ? std::nullopt : std::optional((i + (53 * part)) % 1301));
    flags.emplace_back(i % 29 == 0 ? std::nullopt : std::optional(i % 2 == 0));
    // The three parts add large opposites followed by small values. A changed merge order
    // can give an observably different DOUBLE sum.
    constexpr auto kValues = std::to_array<double>({1.0e18, -1.0e18, 1.0});
    EXPECT_TRUE(payload.Append(kValues[static_cast<std::size_t>(part)]).ok());
  }
  arrow::ArrayVector columns{testing::Int64s(keys)};
  if (query.keys.size() == 2) {
    columns.push_back(testing::Bools(flags));
  }
  columns.push_back(payload.Finish().ValueOrDie());
  return arrow::RecordBatch::Make(query.input, Narrow<std::int64_t>(keys.size()),
                                  std::move(columns));
}

// No group may appear twice, including across output chunks and lanes. Compare DOUBLEs by bits.
using Results = std::map<std::vector<std::string>, std::pair<std::int64_t, std::uint64_t>>;
void Collect(GroupTable& table, const Query& query, Results& rows) {
  for (;;) {
    const auto next = table.NextChunk(query.output);
    ASSERT_TRUE(next.ok()) << next.status();
    if (!*next) {
      return;
    }
    const auto& batch = **next;
    const auto& counts =
        static_cast<const arrow::Int64Array&>(*batch.column(batch.num_columns() - 2));
    const auto& sums =
        static_cast<const arrow::DoubleArray&>(*batch.column(batch.num_columns() - 1));
    for (std::int64_t i = 0; i < batch.num_rows(); ++i) {
      std::vector<std::string> key;
      key.reserve(query.keys.size());
      for (std::size_t k = 0; k < query.keys.size(); ++k) {
        key.push_back(batch.column(Narrow<int>(k))->GetScalar(i).ValueOrDie()->ToString());
      }
      EXPECT_TRUE(
          rows.emplace(std::move(key),
                       std::pair{counts.Value(i), std::bit_cast<std::uint64_t>(sums.Value(i))})
              .second);
    }
  }
}

TEST_F(GroupTableProgramTest, BothLayoutsAndMergeModesAgreeInPartOrder) {
  for (const bool composite : {false, true}) {
    SCOPED_TRACE(composite);
    const auto query = NumericQuery(composite);
    Results oracle;
    MemoryBudget budget(std::nullopt);
    for (const auto source_mode : {Mode::kSeparate, Mode::kBatch}) {
      std::vector<std::unique_ptr<GroupTable>> parts;
      for (int p = 0; p < 3; ++p) {
        auto part = query.Make(source_mode, budget, /*first=*/false);
        ASSERT_NE(part, nullptr);
        ASSERT_EQ(part->uses_batch_program(), source_mode == Mode::kBatch);
        const auto rows = MakeRows(query, p);
        ASSERT_TRUE(part->Consume(*rows->Slice(0, 571)).ok());
        ASSERT_TRUE(part->Consume(*rows->Slice(571, 900)).ok());
        const auto tail = rows->Slice(1471);
        std::vector<std::uint32_t> ids;
        ASSERT_TRUE(part->Consume(*tail, &ids).ok());  // callers that require the stable ID array
        const auto probe = part->Lookup(*tail);
        ASSERT_TRUE(probe.ok()) << probe.status();
        ASSERT_EQ(ids.size(), static_cast<std::size_t>(tail->num_rows()));
        for (std::size_t i = 0; i < ids.size(); ++i) {
          ASSERT_EQ(ids[i], (*probe)->Value(Narrow<std::int64_t>(i)));
        }
        ASSERT_TRUE(part->Partition().ok());
        ASSERT_EQ(part->num_partitions(), 32U);
        if (source_mode == Mode::kBatch) {
          EXPECT_TRUE(part->Consume(*tail).IsInvalid());
          EXPECT_TRUE(part->Lookup(*tail).status().IsInvalid());
          ASSERT_TRUE(part->uniques().ok());
        }
        parts.push_back(std::move(part));
      }  // every source batch has gone away before the first merge
      for (const auto destination_mode : {Mode::kSeparate, Mode::kBatch}) {
        SCOPED_TRACE(static_cast<int>(source_mode));
        SCOPED_TRACE(static_cast<int>(destination_mode));
        Results whole;
        {
          auto table = query.Make(destination_mode, budget);
          for (const auto& part : parts) {
            const auto merged = table->Merge(*part);
            ASSERT_TRUE(merged.ok()) << merged;
          }
          ASSERT_NO_FATAL_FAILURE(Collect(*table, query, whole));
        }
        if (source_mode == Mode::kSeparate && destination_mode == Mode::kSeparate) {
          oracle = whole;
          ASSERT_GT(oracle.size(), 1024U);
        } else {
          EXPECT_EQ(whole, oracle);
        }
        Results partitioned;
        for (std::size_t lane = 0; lane < GroupTable::kPartitions; ++lane) {
          auto table = query.Make(destination_mode, budget);
          for (const auto& part : parts) {
            const auto merged = table->MergePartition(*part, lane);
            ASSERT_TRUE(merged.ok()) << merged;
          }
          ASSERT_NO_FATAL_FAILURE(Collect(*table, query, partitioned));
        }
        EXPECT_EQ(partitioned, oracle);
      }
    }
    EXPECT_EQ(budget.bytes_allocated(), 0);
  }
}

TEST_F(GroupTableProgramTest, FirstSeenDoubleBitsSurviveFullAndPartitionMerges) {
  const auto schema = arrow::schema({arrow::field("k", arrow::float64())});
  const auto output = arrow::schema({schema->field(0), arrow::field("n", arrow::int64())});
  const Query query{.keys = {Column(0, "k", LogicalType::kDouble)},
                    .calls = {{.kind = plan::AggKind::kCountStar, .type = LogicalType::kBigInt}},
                    .input = schema,
                    .output = output};
  constexpr std::uint64_t kNan = 0xfff8000000000046ULL;
  constexpr std::uint64_t kOtherNan = 0x7ff8000000000001ULL;
  constexpr std::uint64_t kNegativeZero = 0x8000000000000000ULL;
  const std::map<std::uint64_t, std::int64_t> expected{{kNegativeZero, 3}, {kNan, 3}};
  MemoryBudget budget(std::nullopt);
  for (const auto source_mode : {Mode::kBatch, Mode::kSeparate}) {
    std::vector<std::unique_ptr<GroupTable>> parts;
    for (const auto bits : {std::array{kNegativeZero, kNan, kNan},
                            std::array{std::uint64_t{0}, kOtherNan, kNegativeZero}}) {
      auto part = query.Make(source_mode, budget);
      arrow::DoubleBuilder keys;
      for (const auto value : bits) {
        ASSERT_TRUE(keys.Append(std::bit_cast<double>(value)).ok());
      }
      ASSERT_TRUE(
          part->Consume(*arrow::RecordBatch::Make(schema, 3, {keys.Finish().ValueOrDie()})).ok());
      ASSERT_TRUE(part->Partition().ok());
      parts.push_back(std::move(part));
    }
    for (const auto mode : {Mode::kBatch, Mode::kSeparate}) {
      for (const bool split : {false, true}) {
        std::map<std::uint64_t, std::int64_t> seen;
        for (std::size_t partition = 0; partition < (split ? GroupTable::kPartitions : 1U);
             ++partition) {
          auto table = query.Make(mode, budget);
          for (const auto& part : parts) {
            const auto merged =
                split ? table->MergePartition(*part, partition) : table->Merge(*part);
            ASSERT_TRUE(merged.ok()) << merged;
          }
          for (;;) {
            const auto next = table->NextChunk(output);
            ASSERT_TRUE(next.ok()) << next.status();
            if (!*next) {
              break;
            }
            const auto& batch = **next;
            const auto& keys = static_cast<const arrow::DoubleArray&>(*batch.column(0));
            const auto& counts = static_cast<const arrow::Int64Array&>(*batch.column(1));
            for (std::int64_t i = 0; i < batch.num_rows(); ++i) {
              EXPECT_TRUE(seen.emplace(std::bit_cast<std::uint64_t>(keys.Value(i)), counts.Value(i))
                              .second);
            }
          }
        }
        EXPECT_EQ(seen, expected);
      }
    }
  }
}

TEST_F(GroupTableProgramTest, RefusedFreezeCanRetryAndRetainedOutputOutlivesBothTables) {
  const auto query = NumericQuery(true);
  constexpr std::int64_t kBytes = 4LL * 1024 * 1024;
  MemoryBudget budget(kBytes);
  std::shared_ptr<arrow::RecordBatch> kept;
  {
    auto source = query.Make(Mode::kBatch, budget, false);
    ASSERT_TRUE(source->Consume(*MakeRows(query, 0)).ok());
    const auto used = budget.bytes_allocated();
    MemoryReservation pressure;
    pressure.Reset(&budget);
    ASSERT_TRUE(pressure.Resize(kBytes - used - 1).ok());
    EXPECT_TRUE(source->Partition().IsOutOfMemory());
    pressure.Release();
    EXPECT_EQ(budget.bytes_allocated(), used);
    ASSERT_TRUE(source->Partition().ok());
    ASSERT_TRUE(source->Partition().ok());
    auto target = query.Make(Mode::kBatch, budget);
    ASSERT_TRUE(target->Merge(*source).ok());
    source.reset();
    auto next = target->NextChunk(query.output);
    ASSERT_TRUE(next.ok()) << next.status();
    kept = std::move(*next);
  }
  ASSERT_NE(kept, nullptr);
  EXPECT_TRUE(kept->ValidateFull().ok());
  EXPECT_GT(kept->num_rows(), 1000);
  EXPECT_GT(budget.bytes_allocated(), 0);
  kept.reset();
  EXPECT_EQ(budget.bytes_allocated(), 0);
}

TEST_F(GroupTableProgramTest, PrefixPartitionsAndManyToOneProjectionMatchSeparateStates) {
  const auto query = NumericQuery(true);
  auto projected = query;
  projected.keys = {query.keys[1]};
  projected.output =
      arrow::schema({query.output->field(1), query.output->field(2), query.output->field(3)});
  const auto rows = MakeRows(query, 2);
  MemoryBudget budget(std::nullopt);
  Results oracle;
  std::array<std::vector<std::uint32_t>, GroupTable::kPartitions> expected_lanes;
  for (const auto mode : {Mode::kSeparate, Mode::kBatch}) {
    auto source = query.Make(mode, budget, false);
    ASSERT_TRUE(source->Consume(*rows).ok());
    const auto keys = source->uniques();
    ASSERT_TRUE(keys.ok()) << keys.status();
    const auto values =
        std::static_pointer_cast<arrow::BooleanArray>(keys->batch.values[1].make_array());
    const auto prefix = source->PrefixHashes(1);
    ASSERT_TRUE(prefix.ok()) << prefix.status();
    ASSERT_GT(prefix->size(), 2U);
    // The chosen BIGINT prefix uses the full (BIGINT, BOOLEAN) routing hash; all others
    // route on BIGINT alone, as the two-level grouping policy requires.
    const std::array heavy{prefix->at(1)};
    ASSERT_TRUE(source->Partition(1, heavy).ok());
    auto folded = projected.Make(mode, budget);
    for (std::size_t lane = 0; lane < GroupTable::kPartitions; ++lane) {
      const auto groups = source->partition_groups(lane);
      if (mode == Mode::kSeparate) {
        expected_lanes[lane].assign(groups.begin(), groups.end());
      } else {
        EXPECT_TRUE(std::ranges::equal(groups, expected_lanes[lane]));
      }
      arrow::BooleanBuilder bools;
      for (const auto group : groups) {
        ASSERT_TRUE(
            (values->IsNull(group) ? bools.AppendNull() : bools.Append(values->Value(group))).ok());
      }
      const arrow::compute::ExecBatch subset({bools.Finish().ValueOrDie()},
                                             Narrow<std::int64_t>(groups.size()));
      ASSERT_TRUE(folded->MergeProjected(*source, subset, groups).ok());
    }
    Results result;
    ASSERT_NO_FATAL_FAILURE(Collect(*folded, projected, result));
    ASSERT_EQ(result.size(), 3U);  // false, true and NULL each fold many source groups
    if (mode == Mode::kSeparate) {
      oracle = std::move(result);
    } else {
      EXPECT_EQ(result, oracle);
    }
  }
}

TEST_F(GroupTableProgramTest, StoredBatchChargesBothColumnVectorsUntilTheBatchIsReleased) {
  const auto flag = arrow::field("flag", arrow::boolean());
  const auto input = arrow::schema({flag});
  const Query query{
      .keys = std::vector<plan::BoundColumn>(66, Column(0, "flag", LogicalType::kBoolean)),
      .calls = {},
      .input = input,
      .output = arrow::schema(arrow::FieldVector(66, flag))};
  const auto row = arrow::RecordBatch::Make(input, 1, {testing::Bools({true})});
  std::int64_t extra_peak = 0;
  {
    arrow::ProxyMemoryPool backend(arrow::default_memory_pool());
    MemoryBudget budget(std::nullopt, &backend);
    auto table = query.Make(Mode::kBatch, budget);
    ASSERT_TRUE(table->uses_batch_program());
    ASSERT_TRUE(table->Consume(*row).ok());
    const auto before = budget.bytes_allocated();
    budget.ResetPeak();
    auto out = table->NextChunk(query.output);
    ASSERT_TRUE(out.ok()) << out.status();
    ASSERT_NE(*out, nullptr);
    extra_peak = budget.max_memory() - before;
    table.reset();
    // Compute the retained heap bytes from the actual Arrow objects, independently of the budget.
    const auto& batch = **out;
    auto expected = (batch.columns().capacity() * sizeof(std::shared_ptr<arrow::Array>)) +
                    (batch.column_data().capacity() * sizeof(std::shared_ptr<arrow::ArrayData>));
    for (const auto& data : batch.column_data()) {
      expected += data->buffers.capacity() * sizeof(std::shared_ptr<arrow::Buffer>);
    }
    EXPECT_EQ(budget.bytes_allocated() - backend.bytes_allocated(), Narrow<std::int64_t>(expected));
    auto column = batch.column(0);
    out->reset();
    const auto handles =
        column->data()->buffers.capacity() * sizeof(std::shared_ptr<arrow::Buffer>);
    EXPECT_EQ(budget.bytes_allocated() - backend.bytes_allocated(), Narrow<std::int64_t>(handles));
    column.reset();
    EXPECT_EQ(budget.bytes_allocated(), 0);
  }
  constexpr std::int64_t kLimit = 1024LL * 1024;
  MemoryBudget tight(kLimit);
  {
    auto table = query.Make(Mode::kBatch, tight);
    ASSERT_TRUE(table->Consume(*row).ok());
    const auto before = tight.bytes_allocated();
    MemoryReservation pressure;
    pressure.Reset(&tight);
    ASSERT_TRUE(pressure.Resize(kLimit - before - extra_peak + 1).ok());
    EXPECT_TRUE(table->NextChunk(query.output).status().IsOutOfMemory());
    pressure.Release();
    EXPECT_EQ(tight.bytes_allocated(), before);
    const auto retry = table->NextChunk(query.output);
    ASSERT_TRUE(retry.ok()) << retry.status();
    ASSERT_NE(*retry, nullptr);
    EXPECT_EQ((*retry)->num_columns(), 66);
    EXPECT_EQ((*retry)->num_rows(), 1);
  }
  EXPECT_EQ(tight.bytes_allocated(), 0);
}

TEST_F(GroupTableProgramTest, DoubleKeyConcatenationCanRetryAfterRefusal) {
  const auto schema = arrow::schema({arrow::field("k", arrow::float64())});
  const Query query{.keys = {Column(0, "k", LogicalType::kDouble)},
                    .calls = {},
                    .input = schema,
                    .output = schema};
  const auto zero = std::bit_cast<std::int64_t>(-0.0);
  const auto one = std::bit_cast<std::int64_t>(1.0);
  const auto raw = testing::Int64s({zero, one});
  const auto keys = raw->View(arrow::float64()).ValueOrDie();
  const auto rows = arrow::RecordBatch::Make(schema, 2, {keys});
  constexpr std::int64_t kLimit = 1024LL * 1024;
  std::int64_t peak = 0;
  for (const bool refuse : {false, true}) {
    MemoryBudget budget(kLimit);
    auto table = query.Make(Mode::kBatch, budget);
    ASSERT_TRUE(table->uses_batch_program());
    // Form two separately retained first-key chunks that must be joined for output.
    ASSERT_TRUE(table->Consume(*rows->Slice(0, 1)).ok());
    ASSERT_TRUE(table->Consume(*rows->Slice(1, 1)).ok());
    budget.ResetPeak();
    if (refuse) {
      const auto before = budget.bytes_allocated();
      MemoryReservation pressure;
      pressure.Reset(&budget);
      ASSERT_TRUE(pressure.Resize(kLimit - peak + 1).ok());
      EXPECT_TRUE(table->NextChunk(schema).status().IsOutOfMemory());
      pressure.Release();
      EXPECT_EQ(budget.bytes_allocated(), before);
    }
    const auto out = table->NextChunk(schema);
    ASSERT_TRUE(out.ok()) << out.status();
    ASSERT_NE(*out, nullptr);
    const auto bits = (*out)->column(0)->View(arrow::int64());
    ASSERT_TRUE(bits.ok()) << bits.status();
    EXPECT_TRUE((*bits)->Equals(*raw));
    if (!refuse) {
      peak = budget.max_memory();
    }
  }
}

TEST_F(GroupTableProgramTest, RefusedLateChunkCannotBeReplayedOrPublished) {
  const auto query = NumericQuery(false);
  MemoryBudget budget(std::nullopt);
  auto failed =
      query.Make(Mode::kBatch, budget, true, {.kind = KeyTableKind::kFixed, .max_groups = 350});
  const auto rows = MakeRows(query, 0);
  const auto status = failed->Consume(*rows);
  ASSERT_TRUE(status.IsOutOfMemory()) << status;
  // The first 256 rows have already updated the state before the next chunk exceeds the cap.
  ASSERT_GT(failed->state(0).num_groups(), 0U);
  EXPECT_EQ(failed->Consume(*rows->Slice(0, 1)), status);
  EXPECT_EQ(failed->CheckFinalize(), status);
  EXPECT_EQ(failed->NextChunk(query.output).status(), status);
  EXPECT_EQ(failed->Partition(), status);
  auto fresh = query.Make(Mode::kBatch, budget);
  EXPECT_EQ(fresh->Merge(*failed), status);
}

TEST_F(GroupTableProgramTest, CheckFinalizeInspectsEveryGroupAfterStoredMerge) {
  const auto decimal = LogicalType::Decimal(38, 0);
  const auto schema =
      arrow::schema({arrow::field("k", arrow::int64()), arrow::field("v", plan::ToArrow(decimal))});
  const Query query{
      .keys = {Column(0, "k", LogicalType::kBigInt)},
      .calls = {{.kind = plan::AggKind::kSum, .arg = Column(1, "v", decimal), .type = decimal}},
      .input = schema,
      .output = schema};
  const std::string largest(38, '9');
  const auto first = arrow::RecordBatch::Make(
      schema, 2, {testing::Int64s({1, 2}), testing::Decimals(decimal, {"1", largest})});
  const auto second = arrow::RecordBatch::Make(
      schema, 1, {testing::Int64s({2}), testing::Decimals(decimal, {"1"})});
  MemoryBudget budget(std::nullopt);
  for (const auto mode : {Mode::kSeparate, Mode::kBatch}) {
    auto all = query.Make(mode, budget);
    for (const auto& rows : {first, second}) {
      auto part = query.Make(mode, budget, false);
      ASSERT_TRUE(part->Consume(*rows).ok());
      ASSERT_TRUE(part->CheckFinalize().ok());
      ASSERT_TRUE(all->Merge(*part).ok());
    }
    // The first group would satisfy LIMIT 1. The second sum fits Int128 but exceeds DECIMAL(38).
    EXPECT_TRUE(all->state(0).Finalize(0, 1, &budget).ok());
    const auto checked = all->CheckFinalize();
    EXPECT_TRUE(checked.IsExecutionError()) << checked;
  }
}

TEST_F(GroupTableProgramTest, UnsupportedKeysAndAggregatesUseExistingPath) {
  MemoryBudget budget(std::nullopt);
  auto query = NumericQuery(false);
  EXPECT_FALSE(
      query.Make(Mode::kBatch, budget, true, {.kind = KeyTableKind::kAuto})->uses_batch_program());
  const auto rows = MakeRows(query, 1);
  for (const auto kind : {plan::AggKind::kStddevPop, plan::AggKind::kCountDistinct}) {
    query.calls = {{.kind = kind,
                    .arg = Column(1, "v", LogicalType::kDouble),
                    .type = kind == plan::AggKind::kCountDistinct ? LogicalType::kBigInt
                                                                  : LogicalType::kDouble}};
    auto table = query.Make(Mode::kBatch, budget);
    ASSERT_FALSE(table->uses_batch_program());
    ASSERT_TRUE(table->Consume(*rows).ok());
    ASSERT_TRUE(table->Partition().ok());
  }
  query.keys.clear();
  query.calls = {{.kind = plan::AggKind::kCountStar, .type = LogicalType::kBigInt}};
  auto global = query.Make(Mode::kBatch, budget);
  ASSERT_FALSE(global->uses_batch_program());
  ASSERT_TRUE(global->Consume(*rows).ok());
  EXPECT_EQ(global->num_groups(), 1U);
}

}  // namespace
}  // namespace antb1::exec
