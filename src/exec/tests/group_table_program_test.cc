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
#include "antb1/exec/operator.h"

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

  auto MakeCardinality(MemoryBudget& budget, std::uint32_t threshold) const {
    return CardinalityLocalGroups::Make(
        keys, calls, input->num_fields(), &budget, &budget, true, GroupTable::kPartitions,
        {.kind = KeyTableKind::kFixed, .estimate = false}, threshold);
  }

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
  if (query.input->num_fields() == 3) {
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

TEST_F(GroupTableProgramTest, DirectDispatchMatchesSelectedRowsAndCanonicalPartitionOrder) {
  for (const bool composite : {false, true}) {
    SCOPED_TRACE(composite);
    auto query = NumericQuery(composite);
    if (composite) {
      query.keys.push_back(query.keys[0]);
      auto fields = query.output->fields();
      fields.insert(fields.begin() + 2, query.input->field(0));
      query.output = arrow::schema(std::move(fields));
    }
    MemoryBudget budget(std::nullopt);
    using Lanes = std::array<std::unique_ptr<const GroupTable>, GroupTable::kPartitions>;
    std::vector<Lanes> released;
    std::vector<std::unique_ptr<GroupTable>> controls;
    for (int part = 0; part < 3; ++part) {
      auto dense_control = query.Make(Mode::kSeparate, budget, false);
      auto control = query.Make(Mode::kBatch, budget, false);
      auto made = PartitionedGroupTables::Make(query.keys, query.calls, query.input->num_fields(),
                                               &budget, &budget, false);
      ASSERT_TRUE(made.ok()) << made.status();
      auto dispatch = std::move(*made);
      {
        const auto small = MakeRows(query, part);
        const auto input = arrow::ConcatenateRecordBatches({small, small, small}).ValueOrDie();
        const auto batch = input->Slice(3, input->num_rows() - 7);
        std::vector<std::optional<bool>> bits(Narrow<std::size_t>(batch->num_rows()) + 5);
        for (std::size_t i = 5; i < bits.size(); ++i) {
          bits[i] = i % 11 == 0 ? std::nullopt : std::optional(i % 3 != 0);
        }
        const auto mask =
            std::static_pointer_cast<arrow::BooleanArray>(testing::Bools(bits)->Slice(5));
        const auto dense = Materialize({.data = batch, .selection = mask}, &budget).ValueOrDie();
        ASSERT_TRUE(dense_control->Consume(*dense).ok());
        ASSERT_TRUE(control->ConsumeSelected(*batch, mask.get()).ok());
        ASSERT_TRUE(dispatch->Consume(*batch, mask.get()).ok());
      }
      ASSERT_EQ(dispatch->num_groups(), dense_control->num_groups());
      ASSERT_EQ(dispatch->num_groups(), control->num_groups());
      ASSERT_TRUE(control->Partition().ok());
      ASSERT_TRUE(dense_control->Partition().ok());
      ASSERT_TRUE(dispatch->Finish().ok());
      EXPECT_TRUE(dispatch->Consume(*MakeRows(query, part)).IsInvalid());
      auto& saved = released.emplace_back();
      for (std::size_t lane = 0; lane < saved.size(); ++lane) {
        saved[lane] = dispatch->ReleasePartition(lane);
        const auto groups = saved[lane] ? saved[lane]->num_groups() : 0U;
        EXPECT_EQ(groups, dense_control->partition_groups(lane).size());
        EXPECT_EQ(groups, control->partition_groups(lane).size());
      }
      controls.push_back(std::move(dense_control));
    }
    for (std::size_t lane = 0; lane < GroupTable::kPartitions; ++lane) {
      auto expected = query.Make(Mode::kSeparate, budget);
      auto actual = query.Make(Mode::kBatch, budget);
      for (std::size_t p = 0; p < controls.size(); ++p) {
        ASSERT_TRUE(expected->MergePartition(*controls[p], lane).ok());
        if (auto& owned = released[p][lane]) {
          ASSERT_TRUE(actual->Merge(*owned).ok());
          owned.reset();
        }
      }
      Results before;
      Results after;
      ASSERT_NO_FATAL_FAILURE(Collect(*expected, query, before));
      ASSERT_NO_FATAL_FAILURE(Collect(*actual, query, after));
      EXPECT_EQ(before, after);
    }
    controls.clear();
    EXPECT_EQ(budget.bytes_allocated(), 0);
  }
}

TEST_F(GroupTableProgramTest, LocalPartitionsReleaseIndependentlyAndRefusedDispatchIsSticky) {
  const auto query = NumericQuery(false);
  const auto batch = MakeRows(query, 0);
  constexpr std::int64_t kLimit = std::int64_t{4} * 1024 * 1024;
  MemoryBudget budget(kLimit);
  auto make = [&] {
    return PartitionedGroupTables::Make(query.keys, query.calls, query.input->num_fields(), &budget,
                                        &budget, false)
        .ValueOrDie();
  };
  {
    auto bundle = make();
    std::vector<std::optional<bool>> keep(Narrow<std::size_t>(batch->num_rows()), false);
    const auto mask = testing::Bools(keep);
    ASSERT_TRUE(bundle->Consume(*batch, mask.get()).ok());
    EXPECT_EQ(bundle->num_groups(), 0U);
    ASSERT_TRUE(bundle->Finish().ok());
    for (std::size_t lane = 0; lane < GroupTable::kPartitions; ++lane) {
      EXPECT_EQ(bundle->ReleasePartition(lane), nullptr);
    }
  }
  {
    auto bundle = make();
    MemoryReservation pressure;
    pressure.Reset(&budget);
    ASSERT_TRUE(pressure.Resize(kLimit - budget.bytes_allocated()).ok());
    const auto refused = bundle->Consume(*batch);
    ASSERT_TRUE(refused.IsOutOfMemory()) << refused;
    pressure.Release();
    EXPECT_EQ(bundle->Consume(*batch), refused);
    EXPECT_EQ(bundle->Finish(), refused);
  }
  EXPECT_EQ(budget.bytes_allocated(), 0);
  auto fresh = make();
  ASSERT_TRUE(fresh->Consume(*batch).ok());
  ASSERT_TRUE(fresh->Finish().ok());
  std::vector<std::unique_ptr<const GroupTable>> lanes;
  for (std::size_t lane = 0; lane < GroupTable::kPartitions; ++lane) {
    if (auto table = fresh->ReleasePartition(lane)) {
      lanes.push_back(std::move(table));
    }
  }
  fresh.reset();
  ASSERT_GT(lanes.size(), 1U);
  while (!lanes.empty()) {
    const auto before = budget.bytes_allocated();
    lanes.pop_back();
    EXPECT_LT(budget.bytes_allocated(), before);
  }
  EXPECT_EQ(budget.bytes_allocated(), 0);
}

Query CountByKey() {
  return {.keys = {Column(0, "k", LogicalType::kBigInt)},
          .calls = {{.kind = plan::AggKind::kCountStar, .type = LogicalType::kBigInt}},
          .input = arrow::schema({arrow::field("k", arrow::int64())}),
          .output = arrow::schema(
              {arrow::field("k", arrow::int64()), arrow::field("n", arrow::int64())})};
}

TEST_F(GroupTableProgramTest, CardinalityCrossingWaitsForMoreSelectedInput) {
  const auto query = CountByKey();
  const auto batch =
      arrow::RecordBatch::Make(query.input, 6, {testing::Int64s({1, 2, 1, 3, 4, 2})});
  const auto drop = testing::Bools({false, std::nullopt});
  for (bool another_row : {false, true}) {
    MemoryBudget budget(std::nullopt);
    {
      auto source = query.MakeCardinality(budget, 2).ValueOrDie();
      ASSERT_TRUE(source->Consume(*batch->Slice(0, 2)).ok());
      ASSERT_TRUE(source->Consume(*batch->Slice(2, 1)).ok());
      EXPECT_EQ(source->num_groups(), 2U);
      EXPECT_FALSE(source->promoted());
      ASSERT_TRUE(source->Consume(*batch->Slice(3, 1)).ok());
      ASSERT_TRUE(source->Consume(*batch->Slice(4, 0)).ok());
      ASSERT_TRUE(source->Consume(*batch->Slice(4), drop.get()).ok());
      EXPECT_EQ(source->num_groups(), 3U);
      EXPECT_FALSE(source->promoted());
      if (another_row) {
        ASSERT_TRUE(source->Consume(*batch->Slice(5)).ok());
      }
      EXPECT_EQ(source->promoted(), another_row);
      EXPECT_EQ(source->promoted_groups(), another_row ? 3U : 0U);
      std::unique_ptr<GroupTable> single;
      std::unique_ptr<PartitionedGroupTables> partitioned;
      ASSERT_TRUE(source->Finish(single, partitioned).ok());
      EXPECT_EQ(single != nullptr, !another_row);
      EXPECT_EQ(partitioned != nullptr, another_row);
    }
    EXPECT_EQ(budget.bytes_allocated(), 0);
  }
}

TEST_F(GroupTableProgramTest, CardinalityDistinguishesOneKeySkewFromLateGrowthWithinOneBatch) {
  const auto query = CountByKey();
  // Cross the internal tile boundary within one Consume, with a long run of one hot key.
  constexpr auto kTile = GroupTable::kMaxMergeChunk;
  for (bool growing : {false, true}) {
    std::vector<std::optional<std::int64_t>> keys(kTile + 1U, 0);
    if (growing) {
      keys[kTile - 2U] = 1;
      keys[kTile - 1U] = 2;
    }
    const auto batch = arrow::RecordBatch::Make(query.input, Narrow<std::int64_t>(keys.size()),
                                                {testing::Int64s(keys)});
    MemoryBudget budget(std::nullopt);
    auto source = query.MakeCardinality(budget, 2).ValueOrDie();
    ASSERT_TRUE(source->Consume(*batch).ok());
    EXPECT_EQ(source->num_groups(), growing ? 3U : 1U);
    EXPECT_EQ(source->promoted(), growing);
    EXPECT_EQ(source->promoted_groups(), growing ? 3U : 0U);
    std::unique_ptr<GroupTable> single;
    std::unique_ptr<PartitionedGroupTables> partitioned;
    ASSERT_TRUE(source->Finish(single, partitioned).ok());
    if (!growing) {
      ASSERT_NE(single, nullptr);
      EXPECT_EQ(partitioned, nullptr);
      const auto result = single->NextChunk(query.output).ValueOrDie();
      ASSERT_EQ(result->num_rows(), 1);
      EXPECT_EQ(static_cast<const arrow::Int64Array&>(*result->column(1)).Value(0),
                batch->num_rows());
    }
  }
}

TEST_F(GroupTableProgramTest, CardinalityMigrationKeepsCompositeGroupsSpellingAndCounts) {
  const auto tag = Column(1, "tag", LogicalType::kBigInt);
  const auto schema =
      arrow::schema({arrow::field("k", arrow::float64()), arrow::field("tag", arrow::int64()),
                     arrow::field("v", arrow::int64())});
  const Query query{.keys = {Column(0, "k", LogicalType::kDouble), tag, tag},
                    .calls = {{.kind = plan::AggKind::kCountStar, .type = LogicalType::kBigInt},
                              {.kind = plan::AggKind::kCount,
                               .arg = Column(2, "v", LogicalType::kBigInt),
                               .type = LogicalType::kBigInt}},
                    .input = schema,
                    .output = arrow::schema({schema->field(0), schema->field(1), schema->field(1),
                                             arrow::field("n", arrow::int64()),
                                             arrow::field("valid", arrow::int64())})};
  constexpr auto kNegZero = std::bit_cast<std::int64_t>(-0.);
  constexpr auto kNan = std::bit_cast<std::int64_t>(0xfff800000000001aULL);
  constexpr auto kOtherNan = std::bit_cast<std::int64_t>(0x7ff800000000006bULL);
  constexpr auto kOne = std::bit_cast<std::int64_t>(1.);
  constexpr auto kNull = std::nullopt;
  MemoryBudget budget(std::nullopt);
  auto source = query.MakeCardinality(budget, 2).ValueOrDie();
  {
    // The first +0 is masked. Both spellings recur after the group IDs change on promotion.
    const auto bits = testing::Int64s(
        {0, kNegZero, kNan, kNull, 0, kOtherNan, kOne, kNull, 0, kOtherNan, kNegZero});
    const auto keys = bits->View(arrow::float64()).ValueOrDie();
    const auto tags = testing::Int64s({1, 1, 2, 3, 1, 2, kNull, 3, 1, 2, kNull});
    const auto values = testing::Int64s({9, kNull, 8, kNull, 5, kNull, 10, 11, 5, 6, 9});
    const auto batch = arrow::RecordBatch::Make(schema, keys->length(), {keys, tags, values});
    const auto prefix = testing::Bools({false, true, true, true, true, true});
    ASSERT_TRUE(source->Consume(*batch->Slice(0, 6), prefix.get()).ok());
    EXPECT_FALSE(source->promoted());
    ASSERT_TRUE(source->Consume(*batch->Slice(6)).ok());
    EXPECT_EQ(source->promoted_groups(), 3U);
    EXPECT_EQ(source->num_groups(), 5U);
  }
  std::unique_ptr<GroupTable> single;
  std::unique_ptr<PartitionedGroupTables> partitioned;
  ASSERT_TRUE(source->Finish(single, partitioned).ok());
  ASSERT_EQ(single, nullptr);
  ASSERT_NE(partitioned, nullptr);
  source.reset();
  using Nullable = std::optional<std::int64_t>;
  using Counts = std::map<std::pair<Nullable, Nullable>, std::pair<std::int64_t, std::int64_t>>;
  const Counts expected{{{kNegZero, 1}, {3, 2}},
                        {{kNan, 2}, {3, 2}},
                        {{kNull, 3}, {2, 1}},
                        {{kOne, kNull}, {1, 1}},
                        {{kNegZero, kNull}, {1, 1}}};
  Counts actual;
  for (std::size_t lane = 0; lane < GroupTable::kPartitions; ++lane) {
    auto part = partitioned->ReleasePartition(lane);
    if (!part) {
      continue;
    }
    auto dest = query.Make(Mode::kBatch, budget);
    ASSERT_TRUE(dest->Merge(*part).ok());
    part.reset();
    for (;;) {
      const auto batch = dest->NextChunk(query.output).ValueOrDie();
      if (!batch) {
        break;
      }
      const auto& keys = static_cast<const arrow::DoubleArray&>(*batch->column(0));
      const auto& tags = static_cast<const arrow::Int64Array&>(*batch->column(1));
      EXPECT_TRUE(tags.Equals(*batch->column(2)));
      const auto& counts = static_cast<const arrow::Int64Array&>(*batch->column(3));
      const auto& valid = static_cast<const arrow::Int64Array&>(*batch->column(4));
      for (std::int64_t i = 0; i < batch->num_rows(); ++i) {
        const Nullable bits =
            keys.IsValid(i) ? Nullable(std::bit_cast<std::int64_t>(keys.Value(i))) : kNull;
        const Nullable tag_value = tags.IsValid(i) ? Nullable(tags.Value(i)) : kNull;
        EXPECT_TRUE(
            actual.emplace(std::pair{bits, tag_value}, std::pair{counts.Value(i), valid.Value(i)})
                .second);
      }
    }
  }
  EXPECT_EQ(actual, expected);
  partitioned.reset();
  EXPECT_EQ(budget.bytes_allocated(), 0);
}

TEST_F(GroupTableProgramTest, CardinalityPromotionRefusalCannotPublishPartialResults) {
  const auto query = CountByKey();
  constexpr std::int64_t kKeys = 256;
  std::vector<std::optional<std::int64_t>> keys;
  keys.reserve(kKeys);
  for (std::int64_t i = 0; i < kKeys; ++i) {
    keys.emplace_back(i);
  }
  const auto batch = arrow::RecordBatch::Make(query.input, Narrow<std::int64_t>(keys.size()),
                                              {testing::Int64s(keys)});
  const auto tail = batch->Slice(0, 1);
  MemoryBudget measured(std::nullopt);
  auto control = query.MakeCardinality(measured, 2).ValueOrDie();
  ASSERT_TRUE(control->Consume(*batch).ok());
  const auto prefix_bytes = measured.bytes_allocated();
  ASSERT_TRUE(control->Consume(*tail).ok());
  ASSERT_TRUE(control->promoted());
  const auto extra = measured.max_memory() - prefix_bytes;
  ASSERT_GT(extra, 1);
  control.reset();
  EXPECT_EQ(measured.bytes_allocated(), 0);

  // A failed transfer may keep its original source and frozen routing IDs, but no temporary
  // destination. Measure the same source's normal handoff without any promotion.
  auto frozen = query.MakeCardinality(measured, 2).ValueOrDie();
  ASSERT_TRUE(frozen->Consume(*batch).ok());
  std::unique_ptr<GroupTable> original;
  std::unique_ptr<PartitionedGroupTables> unused;
  ASSERT_TRUE(frozen->Finish(original, unused).ok());
  const auto frozen_bytes = measured.bytes_allocated();
  original.reset();
  frozen.reset();
  ASSERT_EQ(measured.bytes_allocated(), 0);

  // Refuse both at the start and partway through constructing the destination lanes. Leave
  // headroom relative to this build's actual allocations, independent of object and slot sizes.
  for (const auto headroom : {std::int64_t{0}, extra / 2}) {
    MemoryBudget budget(measured.max_memory());
    {
      auto source = query.MakeCardinality(budget, 2).ValueOrDie();
      ASSERT_TRUE(source->Consume(*batch).ok());
      EXPECT_FALSE(source->promoted());
      MemoryReservation pressure;
      pressure.Reset(&budget);
      ASSERT_TRUE(
          pressure.Resize(measured.max_memory() - budget.bytes_allocated() - headroom).ok());
      const auto refused = source->Consume(*tail);
      ASSERT_TRUE(refused.IsOutOfMemory()) << refused;
      EXPECT_FALSE(source->promoted());
      pressure.Release();
      EXPECT_LE(budget.bytes_allocated(), std::max(prefix_bytes, frozen_bytes));
      EXPECT_EQ(source->Consume(*tail), refused);
      std::unique_ptr<GroupTable> single;
      std::unique_ptr<PartitionedGroupTables> partitioned;
      EXPECT_EQ(source->Finish(single, partitioned), refused);
      EXPECT_EQ(single, nullptr);
      EXPECT_EQ(partitioned, nullptr);
    }
    EXPECT_EQ(budget.bytes_allocated(), 0);
  }
}

TEST_F(GroupTableProgramTest, CardinalityKeepsDoubleAccumulationOnOneTable) {
  for (const auto kind : {plan::AggKind::kSum, plan::AggKind::kAvg}) {
    auto query = NumericQuery(false);
    query.calls.back().kind = kind;
    const auto batch = MakeRows(query, 0);
    MemoryBudget budget(std::nullopt);
    auto source = query.MakeCardinality(budget, 2).ValueOrDie();
    ASSERT_TRUE(source->Consume(*batch).ok());
    ASSERT_GT(source->num_groups(), 2U);
    ASSERT_TRUE(source->Consume(*batch).ok());
    EXPECT_FALSE(source->promoted());
    std::unique_ptr<GroupTable> single;
    std::unique_ptr<PartitionedGroupTables> partitioned;
    ASSERT_TRUE(source->Finish(single, partitioned).ok());
    EXPECT_NE(single, nullptr);
    EXPECT_EQ(partitioned, nullptr);
  }
}

TEST_F(GroupTableProgramTest, LocalDispatchKeepsFallibleUpdatesOnTheirInputOrderPath) {
  const std::vector keys{Column(0, "k", LogicalType::kBigInt)};
  const auto narrow = LogicalType::Decimal(18, 2);
  const auto wide = LogicalType::Decimal(38, 2);
  const auto check = [&](plan::AggKind kind, LogicalType input, LogicalType result, bool eligible) {
    const std::vector<plan::AggregateCall> calls{
        {.kind = kind, .arg = Column(1, "v", input), .type = result}};
    MemoryBudget budget(std::nullopt);
    auto original =
        GroupTable::Make(keys, calls, 2, &budget, &budget, true, GroupTable::kPartitions,
                         {.kind = KeyTableKind::kFixed}, Mode::kBatch)
            .ValueOrDie();
    ASSERT_TRUE(original->uses_batch_program());
    EXPECT_EQ(original->supports_local_dispatch(), eligible);
    EXPECT_EQ(GroupTable::MakeLocal(keys, calls, 2, &budget, &budget).ok(), eligible);
    EXPECT_EQ(PartitionedGroupTables::Make(keys, calls, 2, &budget, &budget).ok(), eligible);
  };
  check(plan::AggKind::kAvg, LogicalType::kDate, LogicalType::kTimestamp, false);
  check(plan::AggKind::kSum, wide, wide, false);
  check(plan::AggKind::kAvg, LogicalType::kHugeInt, LogicalType::kDouble, false);
  check(plan::AggKind::kSumCounts, LogicalType::kBigInt, LogicalType::kBigInt, false);
  check(plan::AggKind::kSum, narrow, wide, true);
  check(plan::AggKind::kAvg, narrow, LogicalType::kDouble, true);
}

TEST_F(GroupTableProgramTest, WideLocalTableMetadataCanBeRefusedWithNoGroups) {
  auto query = NumericQuery(false);
  query.calls.assign(128, query.calls.front());
  const auto make = [&](MemoryBudget& budget) {
    return GroupTable::MakeLocal(query.keys, query.calls, query.input->num_fields(), &budget,
                                 &budget);
  };
  MemoryBudget measured(std::nullopt);
  auto table = make(measured).ValueOrDie();
  EXPECT_EQ(table->num_groups(), 0U);
  const auto live = measured.bytes_allocated();
  EXPECT_GT(live, Narrow<std::int64_t>(query.calls.size() * sizeof(plan::AggregateCall)));
  table.reset();
  EXPECT_EQ(measured.bytes_allocated(), 0);
  MemoryBudget tight(live - 1);
  EXPECT_TRUE(make(tight).status().IsOutOfMemory());
  EXPECT_EQ(tight.bytes_allocated(), 0);
}

TEST_F(GroupTableProgramTest, PartitionedDoubleKeysKeepFirstSelectedSpelling) {
  constexpr std::uint64_t kNegZero = 1ULL << 63U;
  constexpr std::uint64_t kNan = 0xfff8000000000046ULL;
  constexpr std::uint64_t kOther = 0x7ff8000000000001ULL;
  const auto schema = arrow::schema({arrow::field("k", arrow::float64())});
  const auto keys = std::vector{Column(0, "k", LogicalType::kDouble)};
  const auto bits = testing::Int64s(
      {0, std::bit_cast<std::int64_t>(kOther), std::bit_cast<std::int64_t>(kNan),
       std::bit_cast<std::int64_t>(kNegZero), 0, std::bit_cast<std::int64_t>(kOther)});
  const auto column = bits->View(arrow::float64()).ValueOrDie();
  const auto batch = arrow::RecordBatch::Make(schema, 6, {column});
  const auto mask = testing::Bools({false, false, true, true, true, true});
  MemoryBudget budget(std::nullopt);
  auto bundle = PartitionedGroupTables::Make(keys, {}, 1, &budget, &budget).ValueOrDie();
  ASSERT_TRUE(bundle->Consume(*batch, mask.get()).ok());
  ASSERT_TRUE(bundle->Finish().ok());
  std::vector<std::uint64_t> seen;
  for (std::size_t i = 0; i < GroupTable::kPartitions; ++i) {
    auto source = bundle->ReleasePartition(i);
    if (!source) {
      continue;
    }
    auto dest = GroupTable::MakeLocal(keys, {}, 1, &budget, &budget).ValueOrDie();
    ASSERT_TRUE(dest->Merge(*source).ok());
    source.reset();
    const auto result = dest->NextChunk(schema).ValueOrDie();
    const auto& out = static_cast<const arrow::DoubleArray&>(*result->column(0));
    for (std::int64_t r = 0; r < out.length(); ++r) {
      seen.push_back(std::bit_cast<std::uint64_t>(out.Value(r)));
    }
  }
  std::ranges::sort(seen);
  EXPECT_EQ(seen, (std::vector<std::uint64_t>{kNegZero, kNan}));
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
