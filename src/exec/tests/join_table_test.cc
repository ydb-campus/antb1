// The join hash table (docs/adr/0022-joins-and-query-blocks.md): built from parts in any order and
// on any number of threads, both layouts, keys of every type and of several columns, NULL keys,
// sliced batches, probes sharing the table, what the planner must not send, and running out of
// memory.

#include "antb1/exec/join_table.h"

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <numeric>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <arrow/api.h>
#include <arrow/compute/exec.h>
#include <arrow/util/thread_pool.h>
#include <gtest/gtest.h>

#include "antb1/common/narrow.h"
#include "antb1/exec/memory_budget.h"
#include "antb1/exec/operator.h"
#include "antb1/plan/logical_plan.h"
#include "antb1/plan/types.h"

#include "../group_table.h"
#include "../partition_lanes.h"
#include "exec_test_util.h"

namespace antb1::exec {
namespace {

using plan::LogicalType;
using testing::Bools;
using testing::Column;
using testing::Dates;
using testing::Decimals;
using testing::Int16s;
using testing::Int32s;
using testing::Int64s;
using testing::Strings;
using testing::ThrowingExecutor;
using testing::Timestamps;
using testing::UInt16s;

using Ids = std::vector<std::vector<int64_t>>;  // per probe row, the ids of its matches

constexpr int kThreads = 4;
constexpr int64_t kGiB = int64_t{1024} * 1024 * 1024;

class JoinTableTest : public testing::ExecTest {};

std::shared_ptr<arrow::internal::ThreadPool> MakeThreadPool() {
  auto pool = arrow::internal::ThreadPool::Make(kThreads);
  EXPECT_TRUE(pool.ok()) << pool.status().ToString();
  return *pool;
}

// No executor (the calling thread), then the pool.
std::vector<arrow::internal::Executor*> Executors(arrow::internal::ThreadPool* pool) {
  return {nullptr, pool};
}

// A build input: per part, its batches of the key columns k0, k1, ... and an id (BIGINT) that
// numbers the rows in the order they are added (add the parts in order: ids then rise in part,
// batch and row order).
class BuildData {
 public:
  explicit BuildData(const std::vector<LogicalType>& key_types) {
    arrow::FieldVector fields;
    fields.reserve(key_types.size() + 1);
    keys_.reserve(key_types.size());
    for (std::size_t k = 0; k < key_types.size(); ++k) {
      const std::string name = "k" + std::to_string(k);
      fields.push_back(arrow::field(name, plan::ToArrow(key_types[k])));
      keys_.push_back(Column(static_cast<int>(k), name, key_types[k]));
    }
    fields.push_back(arrow::field("id", arrow::int64()));
    schema_ = arrow::schema(fields);
  }

  // A batch of part `part` with these key columns and selection; with `slice`, only its rows
  // [slice, length - slice) (keys, ids and selection at an offset).
  BuildData& Add(std::size_t part, std::vector<std::shared_ptr<arrow::Array>> keys,
                 std::shared_ptr<arrow::BooleanArray> selection = nullptr, int64_t slice = 0) {
    const int64_t length = keys.front()->length();
    std::vector<std::optional<int64_t>> ids;
    ids.reserve(static_cast<std::size_t>(length));
    for (int64_t i = 0; i < length; ++i) {
      ids.emplace_back(next_id_++);
    }
    keys.push_back(Int64s(ids));
    Batch batch{.data = arrow::RecordBatch::Make(schema_, length, std::move(keys)),
                .selection = std::move(selection)};
    if (slice > 0) {
      batch.data = batch.data->Slice(slice, length - (2 * slice));
      if (batch.selection != nullptr) {
        batch.selection = std::static_pointer_cast<arrow::BooleanArray>(
            batch.selection->Slice(slice, length - (2 * slice)));
      }
    }
    if (parts_.size() <= part) {
      parts_.resize(part + 1);
    }
    parts_[part].push_back(std::move(batch));
    return *this;
  }

  [[nodiscard]] const std::shared_ptr<arrow::Schema>& schema() const { return schema_; }
  [[nodiscard]] const std::vector<plan::BoundColumn>& keys() const { return keys_; }
  [[nodiscard]] const std::vector<std::vector<Batch>>& parts() const { return parts_; }

 private:
  std::shared_ptr<arrow::Schema> schema_;
  std::vector<plan::BoundColumn> keys_;
  std::vector<std::vector<Batch>> parts_;
  int64_t next_id_ = 0;
};

// The parts of `data`, each appended to its own JoinBuildPart (Arrow buffers from `pool`).
arrow::Result<std::vector<std::shared_ptr<const JoinBuildPart>>> MakeParts(
    const BuildData& data, const std::shared_ptr<const JoinBuildSpec>& spec,
    MemoryBudget* budget = nullptr, arrow::MemoryPool* pool = arrow::default_memory_pool()) {
  std::vector<std::shared_ptr<const JoinBuildPart>> parts;
  parts.reserve(data.parts().size());
  for (const std::vector<Batch>& batches : data.parts()) {
    auto part = std::make_shared<JoinBuildPart>(spec, budget);
    for (const Batch& batch : batches) {
      ARROW_RETURN_NOT_OK(part->Append(batch, pool));
    }
    parts.push_back(std::move(part));
  }
  return parts;
}

// Adds `parts` in `order` (empty: part order) and finishes the table.
arrow::Result<std::shared_ptr<const JoinTable>> Assemble(
    const std::shared_ptr<const JoinBuildSpec>& spec,
    const std::vector<std::shared_ptr<const JoinBuildPart>>& parts,
    arrow::internal::Executor* executor, std::vector<std::size_t> order = {},
    MemoryBudget* budget = nullptr) {
  if (order.empty()) {
    order.resize(parts.size());
    std::ranges::iota(order, std::size_t{0});
  }
  ARROW_ASSIGN_OR_RAISE(
      auto builder,
      JoinTableBuilder::Make(spec, static_cast<int64_t>(parts.size()), executor, kThreads, budget));
  for (const std::size_t part : order) {
    ARROW_RETURN_NOT_OK(builder->Add(static_cast<int64_t>(part), parts[part]));
  }
  ARROW_RETURN_NOT_OK(builder->Merged());
  return builder->Finish();
}

// The table of `data`: its parts appended on this thread, added in `order`.
arrow::Result<std::shared_ptr<const JoinTable>> BuildTable(
    const BuildData& data, arrow::internal::Executor* executor, std::vector<std::size_t> order = {},
    MemoryBudget* budget = nullptr, arrow::MemoryPool* pool = arrow::default_memory_pool()) {
  ARROW_ASSIGN_OR_RAISE(auto spec, JoinBuildSpec::Make(data.schema(), data.keys()));
  ARROW_ASSIGN_OR_RAISE(auto parts, MakeParts(data, spec, budget, pool));
  return Assemble(spec, parts, executor, std::move(order), budget);
}

// Probe rows: one array per key, and a selection (nullptr: every row).
struct Probe {
  std::vector<std::shared_ptr<arrow::Array>> keys;
  std::shared_ptr<arrow::BooleanArray> selection;
};

// The ids of the rows of each JoinMatches.
Ids MatchIds(const JoinTable& table, std::span<const JoinMatches> matches) {
  const int id_column = table.spec().schema()->num_fields() - 1;
  Ids out;
  out.reserve(matches.size());
  for (const JoinMatches& match : matches) {
    std::vector<int64_t> ids;
    ids.reserve(match.end - match.begin);
    for (std::uint32_t r = match.begin; r < match.end; ++r) {
      const JoinRowRef ref = table.rows()[r];
      ids.push_back(
          static_cast<const arrow::Int64Array&>(*table.chunks().at(ref.chunk)->column(id_column))
              .Value(ref.row));
    }
    out.push_back(std::move(ids));
  }
  return out;
}

// Find over `probe`: every probe row's matches, empty ones as {0, 0}.
std::vector<JoinMatches> FindAll(const JoinTable& table, const Probe& probe,
                                 arrow::MemoryPool* pool = arrow::default_memory_pool()) {
  std::vector<JoinMatches> out(static_cast<std::size_t>(probe.keys.front()->length()),
                               JoinMatches{.begin = 7, .end = 7});
  const arrow::Status found = table.Find(probe.keys, probe.selection.get(), pool, out);
  EXPECT_TRUE(found.ok()) << found.ToString();
  for (const JoinMatches& match : out) {
    EXPECT_TRUE(match.begin < match.end || (match.begin == 0 && match.end == 0));
  }
  return out;
}

Ids Matches(const JoinTable& table, const Probe& probe) {
  return MatchIds(table, FindAll(table, probe));
}

bool Selected(const std::shared_ptr<arrow::BooleanArray>& selection, int64_t row) {
  return selection == nullptr || (selection->IsValid(row) && selection->Value(row));
}

bool AnyNull(std::span<const std::shared_ptr<arrow::Array>> keys, int64_t row) {
  return std::ranges::any_of(keys, [row](const auto& key) { return key->IsNull(row); });
}

// The matches by nested loops with SQL's equality: a selected probe row whose keys are not NULL
// matches every selected build row whose keys are not NULL and equal its keys, in part, batch and
// row order.
Ids Reference(const BuildData& data, const Probe& probe) {
  const int64_t length = probe.keys.front()->length();
  const std::size_t num_keys = probe.keys.size();
  Ids out(static_cast<std::size_t>(length));
  for (int64_t i = 0; i < length; ++i) {
    if (!Selected(probe.selection, i) || AnyNull(probe.keys, i)) {
      continue;
    }
    for (const std::vector<Batch>& part : data.parts()) {
      for (const Batch& batch : part) {
        const std::vector<std::shared_ptr<arrow::Array>> keys(
            batch.data->columns().begin(),
            batch.data->columns().begin() + static_cast<std::ptrdiff_t>(num_keys));
        const auto& ids =
            static_cast<const arrow::Int64Array&>(*batch.data->column(static_cast<int>(num_keys)));
        for (int64_t j = 0; j < batch.data->num_rows(); ++j) {
          if (!Selected(batch.selection, j) || AnyNull(keys, j)) {
            continue;
          }
          bool equal = true;
          for (std::size_t k = 0; k < num_keys; ++k) {
            equal = equal && probe.keys[k]->RangeEquals(i, i + 1, j, *keys[k]);
          }
          if (equal) {
            out[static_cast<std::size_t>(i)].push_back(ids.Value(j));
          }
        }
      }
    }
  }
  return out;
}

// The rows of a table in rows() order, as (chunk, row) pairs.
std::vector<std::pair<std::uint32_t, std::uint32_t>> RowRefs(const JoinTable& table) {
  std::vector<std::pair<std::uint32_t, std::uint32_t>> out;
  out.reserve(table.rows().size());
  for (const JoinRowRef& ref : table.rows()) {
    out.emplace_back(ref.chunk, ref.row);
  }
  return out;
}

// An array of `type` (an integer, DATE or TIMESTAMP type) holding `values`.
std::shared_ptr<arrow::Array> IntegersOf(LogicalType type,
                                         const std::vector<std::optional<int64_t>>& values) {
  const auto narrowed = [&values]<class T>(T /*type*/) {
    std::vector<std::optional<T>> out;
    out.reserve(values.size());
    for (const std::optional<int64_t>& v : values) {
      out.push_back(v.has_value() ? std::optional<T>(Narrow<T>(*v)) : std::nullopt);
    }
    return out;
  };
  switch (type.id()) {
    case LogicalType::kSmallInt:
      return Int16s(narrowed(std::int16_t{}));
    case LogicalType::kInteger:
      return Int32s(narrowed(std::int32_t{}));
    case LogicalType::kUSmallInt:
      return UInt16s(narrowed(std::uint16_t{}));
    case LogicalType::kDate:
      return Dates(narrowed(std::int32_t{}));
    case LogicalType::kTimestamp:
      return Timestamps(values);
    default:
      return Int64s(values);
  }
}

// Every row's ids for a probe of `keys` (one BIGINT key, no selection).
Probe Int64Probe(const std::vector<std::optional<int64_t>>& keys,
                 std::shared_ptr<arrow::BooleanArray> selection = nullptr) {
  return Probe{.keys = {Int64s(keys)}, .selection = std::move(selection)};
}

// ---- Matches ----

// Every match of every probe row, in the build's part and row order; NULL keys and rows outside a
// selection match nothing, on both sides.
TEST_F(JoinTableTest, FindsEveryMatchInPartOrder) {
  const auto pool = MakeThreadPool();
  BuildData data({LogicalType::kBigInt});
  int64_t selected = 0;
  int64_t nulls = 0;
  for (std::size_t part = 0; part < 4; ++part) {
    for (int64_t b = 0; b < 2; ++b) {
      std::vector<std::optional<int64_t>> keys;
      std::vector<std::optional<bool>> keep;
      keys.reserve(40);
      keep.reserve(40);
      for (int64_t r = 0; r < 40; ++r) {
        const bool null = r % 7 == 3;
        keys.push_back(
            null ? std::nullopt
                 : std::optional<int64_t>(((r * 5) + (static_cast<int64_t>(part) * 3) + b) % 23));
        keep.emplace_back(r % 5 == 0);  // a selective selection: only its rows are hashed
        const bool counted = part != 2 || b != 1 || r % 5 == 0;
        selected += counted ? 1 : 0;
        nulls += counted && null ? 1 : 0;
      }
      data.Add(part, {Int64s(keys)}, part == 2 && b == 1 ? Bools(keep) : nullptr);
    }
  }
  std::vector<std::optional<int64_t>> probe_keys;
  std::vector<std::optional<bool>> probe_keep;
  probe_keys.reserve(30);
  probe_keep.reserve(30);
  for (int64_t k = -2; k < 26; ++k) {
    probe_keys.emplace_back(k);
    probe_keep.emplace_back(k != 4);
  }
  probe_keys.emplace_back(std::nullopt);
  probe_keep.emplace_back(true);
  probe_keys.emplace_back(5);
  probe_keep.emplace_back(std::nullopt);  // a NULL in a selection drops the row
  const Probe probe = Int64Probe(probe_keys, Bools(probe_keep));
  const Ids expected = Reference(data, probe);
  ASSERT_FALSE(expected[7].empty());  // key 5 has matches
  for (arrow::internal::Executor* executor : Executors(pool.get())) {
    auto table = BuildTable(data, executor);
    ASSERT_TRUE(table.ok()) << table.status().ToString();
    const Ids found = Matches(**table, probe);
    EXPECT_EQ(found, expected);
    for (const std::vector<int64_t>& ids : found) {
      EXPECT_TRUE(std::ranges::is_sorted(ids));  // part, batch and row order
    }
    EXPECT_EQ((*table)->input_rows(), selected);
    EXPECT_EQ((*table)->null_key_rows(), nulls);
    EXPECT_EQ((*table)->num_rows(), selected - nulls);
    EXPECT_FALSE((*table)->unique());
    EXPECT_TRUE((*table)->has_null());
    EXPECT_FALSE((*table)->empty());
  }
}

// Dense keys (the direct layout) and the same keys spread out (hashed) give the same matches, and
// either layout's rows() holds every inserted row once.
TEST_F(JoinTableTest, BothLayoutsAgree) {
  const auto spread = [](int64_t k) { return (k * 1000003) + 17; };
  std::vector<Ids> results;
  for (const bool hashed : {false, true}) {
    const auto key = [&](int64_t k) { return hashed ? spread(k) : k; };
    BuildData data({LogicalType::kBigInt});
    for (std::size_t part = 0; part < 3; ++part) {
      std::vector<std::optional<int64_t>> keys;
      keys.reserve(300);
      for (int64_t r = 0; r < 300; ++r) {
        const int64_t k = ((r * 7) + static_cast<int64_t>(part)) % 211;
        keys.push_back(r % 50 == 9 ? std::nullopt : std::optional<int64_t>(key(k)));
      }
      data.Add(part, {Int64s(keys)});
    }
    std::vector<std::optional<int64_t>> probe_keys;
    std::vector<std::optional<bool>> few;
    probe_keys.reserve(224);
    few.reserve(224);
    for (int64_t k = -3; k < 220; ++k) {
      probe_keys.emplace_back(key(k));
      few.emplace_back(k % 5 == 0);  // a selective probe: only its rows are hashed
    }
    probe_keys.emplace_back(std::nullopt);
    few.emplace_back(true);
    auto table = BuildTable(data, nullptr);
    ASSERT_TRUE(table.ok()) << table.status().ToString();
    EXPECT_EQ((*table)->layout(), hashed ? JoinTable::Layout::kHashed : JoinTable::Layout::kDirect);
    for (const Probe& probe : {Int64Probe(probe_keys), Int64Probe(probe_keys, Bools(few))}) {
      const Ids found = Matches(**table, probe);
      EXPECT_EQ(found, Reference(data, probe));
      results.push_back(found);
    }
    // rows() is every inserted row once.
    std::vector<JoinMatches> all(
        1, JoinMatches{.begin = 0, .end = Narrow<std::uint32_t>((*table)->num_rows())});
    std::vector<int64_t> ids = MatchIds(**table, all).front();
    std::ranges::sort(ids);
    std::vector<int64_t> inserted;
    inserted.reserve(900);
    for (int64_t id = 0; id < 900; ++id) {
      if ((id % 300) % 50 != 9) {
        inserted.push_back(id);
      }
    }
    EXPECT_EQ(ids, inserted);
  }
  ASSERT_EQ(results.size(), 4U);
  EXPECT_EQ(results[0], results[2]);
  EXPECT_EQ(results[1], results[3]);
}

// The direct layout serves a single integer key whose values span fewer than 8 times the rows,
// whatever the type, the sign or the range of the keys.
TEST_F(JoinTableTest, DirectLayoutBoundaries) {
  const auto check = [](const BuildData& data, JoinTable::Layout layout, const Probe& probe) {
    auto table = BuildTable(data, nullptr);
    ASSERT_TRUE(table.ok()) << table.status().ToString();
    EXPECT_EQ((*table)->layout(), layout);
    EXPECT_EQ(Matches(**table, probe), Reference(data, probe));
  };
  // Ten rows: a span of 79 (8N - 1) is direct, 80 (8N) is hashed.
  for (const int64_t last : {79, 80}) {
    std::vector<std::optional<int64_t>> keys;
    keys.reserve(10);
    for (int64_t k = 0; k < 9; ++k) {
      keys.emplace_back(k);
    }
    keys.emplace_back(last);
    BuildData data({LogicalType::kBigInt});
    data.Add(0, {Int64s(keys)});
    std::vector<std::optional<int64_t>> probe;
    probe.reserve(83);
    for (int64_t k = -1; k <= 81; ++k) {
      probe.emplace_back(k);
    }
    check(data, last == 79 ? JoinTable::Layout::kDirect : JoinTable::Layout::kHashed,
          Int64Probe(probe));
  }
  // The whole BIGINT range: hashed, and the span does not overflow.
  constexpr int64_t kMin = std::numeric_limits<int64_t>::min();
  constexpr int64_t kMax = std::numeric_limits<int64_t>::max();
  {
    BuildData data({LogicalType::kBigInt});
    data.Add(0, {Int64s({kMin, kMax, kMax})});
    check(data, JoinTable::Layout::kHashed, Int64Probe({kMin, kMax, 0, kMin + 1, kMax - 1}));
  }
  // Negative dense keys are direct; keys below the smallest, above the largest and in a gap find
  // nothing, even at the ends of the range.
  {
    std::vector<std::optional<int64_t>> keys;
    keys.reserve(100);
    for (int64_t k = -100; k < 0; ++k) {
      if (k != -50) {
        keys.emplace_back(k);
      }
    }
    BuildData data({LogicalType::kBigInt});
    data.Add(0, {Int64s(keys)});
    check(data, JoinTable::Layout::kDirect,
          Int64Probe({-100, -1, -50, -101, 0, kMin, kMax, std::nullopt, -7}));
  }
  // The largest keys: max - min fits, and no probe wraps around to a match.
  {
    BuildData data({LogicalType::kBigInt});
    data.Add(0, {Int64s({kMax - 2, kMax, kMax - 1})});
    check(data, JoinTable::Layout::kDirect, Int64Probe({kMax, kMax - 2, kMax - 3, kMin, kMin + 1}));
  }
  // A single row.
  {
    BuildData data({LogicalType::kBigInt});
    data.Add(0, {Int64s({42})});
    check(data, JoinTable::Layout::kDirect, Int64Probe({41, 42, 43}));
  }
  // Dense keys of every direct type, with duplicates and a NULL, in two parts.
  struct Case {
    LogicalType type;
    int64_t first;
  };
  for (const Case& c : {Case{.type = LogicalType::kSmallInt, .first = -40},
                        Case{.type = LogicalType::kInteger, .first = 100000},
                        Case{.type = LogicalType::kBigInt, .first = kGiB * 1024},
                        Case{.type = LogicalType::kUSmallInt, .first = 65500},
                        Case{.type = LogicalType::kDate, .first = 9000},
                        Case{.type = LogicalType::kTimestamp, .first = -1000}}) {
    SCOPED_TRACE(plan::ToString(c.type));
    BuildData data({c.type});
    std::vector<std::optional<int64_t>> probe;
    probe.reserve(40);
    for (std::size_t part = 0; part < 2; ++part) {
      std::vector<std::optional<int64_t>> keys;
      keys.reserve(36);
      for (int64_t r = 0; r < 36; ++r) {
        keys.push_back(r == 5 ? std::nullopt : std::optional<int64_t>(c.first + ((r * 3) % 36)));
      }
      data.Add(part, {IntegersOf(c.type, keys)});
    }
    for (int64_t k = -2; k < 37; ++k) {
      if (c.type != LogicalType::kUSmallInt || c.first + k <= 65535) {
        probe.emplace_back(c.first + k);
      }
    }
    probe.emplace_back(std::nullopt);
    check(data, JoinTable::Layout::kDirect,
          Probe{.keys = {IntegersOf(c.type, probe)}, .selection = nullptr});
  }
}

// Keys of the types that are always hashed and keys of two columns follow SQL's equality: equal
// bytes match ('' matches '', an embedded NUL counts), a NULL in any key column never matches.
TEST_F(JoinTableTest, KeysOfEveryTypeAndSeveralColumns) {
  const auto pool = MakeThreadPool();
  const auto check = [&pool](const BuildData& data, const Probe& probe) {
    const Ids expected = Reference(data, probe);
    ASSERT_TRUE(std::ranges::any_of(expected, [](const auto& ids) { return ids.size() > 1; }));
    for (arrow::internal::Executor* executor : Executors(pool.get())) {
      auto table = BuildTable(data, executor);
      ASSERT_TRUE(table.ok()) << table.status().ToString();
      EXPECT_EQ((*table)->layout(), JoinTable::Layout::kHashed);
      EXPECT_EQ(Matches(**table, probe), expected);
    }
  };
  {
    SCOPED_TRACE("DECIMAL(15,2)");
    BuildData data({LogicalType::Decimal(15, 2)});
    data.Add(0, {Decimals(15, 2, {"12345", "-1", "12345", std::nullopt})})
        .Add(1, {Decimals(15, 2, {"99999999999999", "0", "-1", "-99999999999999"})});
    check(data, Probe{.keys = {Decimals(15, 2,
                                        {"12345", "0", "-1", "1", std::nullopt, "99999999999999",
                                         "-99999999999999"})},
                      .selection = nullptr});
  }
  {
    SCOPED_TRACE("HUGEINT");
    const std::string big(38, '9');
    BuildData data({LogicalType::kHugeInt});
    data.Add(0, {Decimals(38, 0, {big, "-" + big, "18446744073709551616", big, std::nullopt})})
        .Add(1, {Decimals(38, 0, {"1", "18446744073709551616", "-18446744073709551616"})});
    check(data, Probe{.keys = {Decimals(38, 0,
                                        {big, "-" + big, "18446744073709551616", "1",
                                         "18446744073709551615", std::nullopt})},
                      .selection = nullptr});
  }
  {
    SCOPED_TRACE("VARCHAR");
    const std::string longer = "a string of more than sixteen bytes";
    const std::string nul_b("a\0b", 3);
    const std::string nul_c("a\0c", 3);
    BuildData data({LogicalType::kVarchar});
    data.Add(0, {Strings({"", "a", longer, nul_b, std::nullopt, "abc"})})
        .Add(1, {Strings({nul_c, "a", "", longer, std::string("a\0", 2), nul_b})});
    check(data, Probe{.keys = {Strings({"", "a", nul_b, nul_c, std::string("a\0", 2), longer,
                                        longer.substr(1), std::nullopt, "zzz"})},
                      .selection = nullptr});
  }
  {
    SCOPED_TRACE("(BIGINT, VARCHAR)");
    BuildData data({LogicalType::kBigInt, LogicalType::kVarchar});
    data.Add(0, {Int64s({1, 1, 2, std::nullopt, 1, 1}),
                 Strings({"x", "y", "x", "x", std::nullopt, "x"})})
        .Add(1, {Int64s({2, 1}), Strings({"x", ""})});
    check(data, Probe{.keys = {Int64s({1, 1, 2, std::nullopt, 1, 2, 1}),
                               Strings({"x", "y", "x", "x", std::nullopt, "y", ""})},
                      .selection = nullptr});
  }
  {
    SCOPED_TRACE("(INTEGER, DATE)");
    BuildData data({LogicalType::kInteger, LogicalType::kDate});
    data.Add(0, {Int32s({7, 7, 8, 7, std::nullopt}), Dates({100, 101, 100, 100, 100})})
        .Add(1, {Int32s({7, 8}), Dates({std::nullopt, 100})});
    check(data, Probe{.keys = {Int32s({7, 7, 8, 8, std::nullopt, 7}),
                               Dates({100, 101, 100, 101, 100, std::nullopt})},
                      .selection = nullptr});
  }
}

// Build and probe batches sliced from larger ones: keys, ids and selections at an offset.
TEST_F(JoinTableTest, SlicedBatches) {
  constexpr int64_t kSlice = 3;
  // Keeps one row in `every` (with 5, few enough that only those rows are hashed).
  const auto selection = [](int64_t length, int64_t every) {
    std::vector<std::optional<bool>> keep;
    keep.reserve(static_cast<std::size_t>(length));
    for (int64_t i = 0; i < length; ++i) {
      keep.emplace_back(i % every == 0);
    }
    return Bools(keep);
  };
  const auto sliced = [](const std::shared_ptr<arrow::Array>& array) {
    return array->Slice(kSlice, array->length() - (2 * kSlice));
  };
  const auto sliced_selection = [&](int64_t length, int64_t every) {
    return std::static_pointer_cast<arrow::BooleanArray>(sliced(selection(length, every)));
  };
  const auto check = [&](const BuildData& data,
                         const std::vector<std::shared_ptr<arrow::Array>>& keys,
                         JoinTable::Layout layout) {
    auto table = BuildTable(data, nullptr);
    ASSERT_TRUE(table.ok()) << table.status().ToString();
    EXPECT_EQ((*table)->layout(), layout);
    std::vector<std::shared_ptr<arrow::Array>> probe_keys;
    probe_keys.reserve(keys.size());
    for (const auto& key : keys) {
      probe_keys.push_back(sliced(key));
    }
    const int64_t length = keys.front()->length();
    for (const Probe& probe :
         {Probe{.keys = probe_keys, .selection = nullptr},
          Probe{.keys = probe_keys, .selection = sliced_selection(length, 2)},
          Probe{.keys = probe_keys, .selection = sliced_selection(length, 5)}}) {
      const Ids expected = Reference(data, probe);
      ASSERT_TRUE(std::ranges::any_of(expected, [](const auto& ids) { return !ids.empty(); }));
      EXPECT_EQ(Matches(**table, probe), expected);
    }
  };
  std::vector<std::optional<int64_t>> ints;
  std::vector<std::optional<int64_t>> sparse;
  std::vector<std::optional<std::string>> decimals;
  std::vector<std::optional<std::string>> strings;
  ints.reserve(26);
  sparse.reserve(26);
  decimals.reserve(26);
  strings.reserve(26);
  for (int64_t i = 0; i < 26; ++i) {
    const int64_t k = (i * 7) % 13;
    ints.push_back(i == 4 ? std::nullopt : std::optional<int64_t>(k));
    sparse.emplace_back((k * 1000003) - 5);
    decimals.emplace_back(std::to_string((k * 100) - 7));
    strings.emplace_back(k % 2 == 0 ? "short " + std::to_string(k)
                                    : "a key of more than sixteen bytes " + std::to_string(k));
  }
  const auto every = [&](std::size_t part, BuildData& data, auto make_keys) {
    data.Add(part, make_keys(), part == 0 ? selection(26, 2) : selection(26, 5), kSlice);
  };
  {
    SCOPED_TRACE("hashed BIGINT");
    BuildData data({LogicalType::kBigInt});
    for (std::size_t part = 0; part < 2; ++part) {
      every(part, data, [&] { return std::vector{Int64s(sparse)}; });
    }
    check(data, {Int64s(sparse)}, JoinTable::Layout::kHashed);
  }
  {
    SCOPED_TRACE("direct INTEGER");
    BuildData data({LogicalType::kInteger});
    for (std::size_t part = 0; part < 2; ++part) {
      every(part, data, [&] { return std::vector{IntegersOf(LogicalType::kInteger, ints)}; });
    }
    check(data, {IntegersOf(LogicalType::kInteger, ints)}, JoinTable::Layout::kDirect);
  }
  {
    SCOPED_TRACE("DECIMAL(15,2)");
    BuildData data({LogicalType::Decimal(15, 2)});
    for (std::size_t part = 0; part < 2; ++part) {
      every(part, data, [&] { return std::vector{Decimals(15, 2, decimals)}; });
    }
    check(data, {Decimals(15, 2, decimals)}, JoinTable::Layout::kHashed);
  }
  {
    SCOPED_TRACE("VARCHAR");
    BuildData data({LogicalType::kVarchar});
    for (std::size_t part = 0; part < 2; ++part) {
      every(part, data, [&] { return std::vector{Strings(strings)}; });
    }
    check(data, {Strings(strings)}, JoinTable::Layout::kHashed);
  }
  {
    SCOPED_TRACE("(BIGINT, VARCHAR)");
    BuildData data({LogicalType::kBigInt, LogicalType::kVarchar});
    for (std::size_t part = 0; part < 2; ++part) {
      every(part, data, [&] { return std::vector{Int64s(ints), Strings(strings)}; });
    }
    check(data, {Int64s(ints), Strings(strings)}, JoinTable::Layout::kHashed);
  }
}

// Keys whose hashes share a partition and a bucket: each key's rows end up together and in their
// order, whether they came together or interleaved with another key's.
TEST_F(JoinTableTest, KeysSharingABucketStayTogether) {
  // `count` VARCHAR keys whose hashes (KeyHashes) have the same partition (the low 6 bits) and the
  // same bucket in a partition of `rows` rows (the bits above them, modulo bit_ceil(rows)).
  const auto colliding = [](std::size_t count, std::uint64_t rows) {
    std::vector<std::optional<std::string>> candidates;
    candidates.reserve(4096);
    for (int i = 0; i < 4096; ++i) {
      candidates.emplace_back("key " + std::to_string(i));
    }
    const auto hashes = KeyHashes(
        arrow::compute::ExecBatch({Strings(candidates)}, static_cast<int64_t>(candidates.size())));
    EXPECT_TRUE(hashes.ok());
    std::map<std::pair<std::uint64_t, std::uint64_t>, std::vector<std::string>> cells;
    for (std::size_t i = 0; i < candidates.size(); ++i) {
      const std::uint64_t hash = (*hashes)[i];
      auto& cell = cells[{hash % kJoinPartitions, (hash >> 6U) & (std::bit_ceil(rows) - 1)}];
      cell.push_back(candidates[i].value_or(""));
      if (cell.size() == count) {
        return cell;
      }
    }
    return std::vector<std::string>{};
  };
  {
    // a, b, a, c, b: not together, sorted by key (first seen first) without reordering a key's
    // rows; on every memory limit up to the one the build fits, OutOfMemory first.
    const std::vector<std::string> keys = colliding(3, 5);
    ASSERT_EQ(keys.size(), 3U);
    BuildData data({LogicalType::kVarchar});
    data.Add(0, {Strings({keys[0], keys[1], keys[0], keys[2], keys[1]})});
    const Probe probe{.keys = {Strings({keys[0], keys[1], keys[2], "other"})},
                      .selection = nullptr};
    for (int64_t limit = 0;; limit += 16) {
      MemoryBudget budget(limit);
      bool built = false;
      {
        auto table = BuildTable(data, nullptr, {}, &budget, &budget);
        if (table.ok()) {
          built = true;
          EXPECT_FALSE((*table)->unique());
          EXPECT_EQ(Matches(**table, probe), (Ids{{0, 2}, {1, 4}, {3}, {}}));
        } else {
          EXPECT_TRUE(table.status().IsOutOfMemory()) << table.status().ToString();
        }
      }
      EXPECT_EQ(budget.bytes_allocated(), 0);
      if (built) {
        break;
      }
      ASSERT_LT(limit, 1024 * 1024);
    }
  }
  {
    // a, a, b: together already.
    const std::vector<std::string> keys = colliding(3, 3);
    ASSERT_EQ(keys.size(), 3U);
    BuildData data({LogicalType::kVarchar});
    data.Add(0, {Strings({keys[0], keys[0], keys[1]})});
    auto table = BuildTable(data, nullptr);
    ASSERT_TRUE(table.ok()) << table.status().ToString();
    EXPECT_FALSE((*table)->unique());
    EXPECT_EQ(Matches(**table,
                      Probe{.keys = {Strings({keys[1], keys[0], keys[2]})}, .selection = nullptr}),
              (Ids{{2}, {0, 1}, {}}));
  }
  {
    // a, b: distinct keys in one bucket.
    const std::vector<std::string> keys = colliding(2, 2);
    ASSERT_EQ(keys.size(), 2U);
    BuildData data({LogicalType::kVarchar});
    data.Add(0, {Strings({keys[0], keys[1]})});
    auto table = BuildTable(data, nullptr);
    ASSERT_TRUE(table.ok()) << table.status().ToString();
    EXPECT_TRUE((*table)->unique());
    EXPECT_EQ(Matches(**table, Probe{.keys = {Strings({keys[1], keys[0]})}, .selection = nullptr}),
              (Ids{{1}, {0}}));
  }
}

// ---- Flags ----

// unique() looks at the inserted keys only: NULL keys are not inserted.
TEST_F(JoinTableTest, UniquenessIgnoresNullKeys) {
  for (const int64_t step : {1, 1000003}) {  // direct, then hashed
    SCOPED_TRACE(step);
    BuildData unique({LogicalType::kBigInt});
    unique.Add(0, {Int64s({0, std::nullopt, step, std::nullopt})}).Add(1, {Int64s({2 * step})});
    auto table = BuildTable(unique, nullptr);
    ASSERT_TRUE(table.ok()) << table.status().ToString();
    EXPECT_TRUE((*table)->unique());
    EXPECT_TRUE((*table)->has_null());
    EXPECT_EQ((*table)->null_key_rows(), 2);
    EXPECT_EQ((*table)->num_rows(), 3);
    EXPECT_EQ((*table)->layout(),
              step == 1 ? JoinTable::Layout::kDirect : JoinTable::Layout::kHashed);

    BuildData repeated({LogicalType::kBigInt});
    repeated.Add(0, {Int64s({0, std::nullopt, step})}).Add(1, {Int64s({step})});
    table = BuildTable(repeated, nullptr);
    ASSERT_TRUE(table.ok()) << table.status().ToString();
    EXPECT_FALSE((*table)->unique());
    EXPECT_TRUE((*table)->has_null());
  }
}

// A build without selected rows is empty; one with only NULL keys is not empty but has no rows.
TEST_F(JoinTableTest, FlagsForEmptyAndNullOnlyBuilds) {
  const Probe probe = Int64Probe({1, std::nullopt, 2});
  const auto expect_no_match = [&probe](const JoinTable& table) {
    for (const JoinMatches& match : FindAll(table, probe)) {
      EXPECT_EQ(match.begin, 0U);
      EXPECT_EQ(match.end, 0U);
    }
  };
  {
    BuildData none({LogicalType::kBigInt});
    none.Add(0, {Int64s({1, std::nullopt, 2})}, Bools({false, false, false}));
    for (const bool parts : {true, false}) {
      auto spec = JoinBuildSpec::Make(none.schema(), none.keys());
      ASSERT_TRUE(spec.ok());
      auto table = parts ? BuildTable(none, nullptr) : Assemble(*spec, {}, nullptr);
      ASSERT_TRUE(table.ok()) << table.status().ToString();
      EXPECT_TRUE((*table)->empty());
      EXPECT_FALSE((*table)->has_null());
      EXPECT_TRUE((*table)->unique());
      EXPECT_EQ((*table)->num_rows(), 0);
      EXPECT_EQ((*table)->input_rows(), 0);
      expect_no_match(**table);
    }
  }
  {
    BuildData nulls({LogicalType::kBigInt});
    nulls.Add(0, {Int64s({std::nullopt, std::nullopt})}).Add(1, {Int64s({std::nullopt})});
    auto table = BuildTable(nulls, nullptr);
    ASSERT_TRUE(table.ok()) << table.status().ToString();
    EXPECT_FALSE((*table)->empty());
    EXPECT_TRUE((*table)->has_null());
    EXPECT_TRUE((*table)->unique());
    EXPECT_EQ((*table)->num_rows(), 0);
    EXPECT_EQ((*table)->input_rows(), 3);
    EXPECT_EQ((*table)->null_key_rows(), 3);
    EXPECT_TRUE((*table)->chunks().empty());
    expect_no_match(**table);
  }
}

// Probes with nothing to look up: no rows, only NULL keys, no selected row.
TEST_F(JoinTableTest, ProbesWithoutCandidates) {
  for (const int64_t step : {1, 1000003}) {  // direct, then hashed
    SCOPED_TRACE(step);
    BuildData data({LogicalType::kBigInt});
    data.Add(0, {Int64s({0, step, 2 * step})});
    auto table = BuildTable(data, nullptr);
    ASSERT_TRUE(table.ok()) << table.status().ToString();
    const std::vector<std::shared_ptr<arrow::Array>> no_rows = {Int64s({})};
    std::vector<JoinMatches> none;
    EXPECT_TRUE((*table)->Find(no_rows, nullptr, arrow::default_memory_pool(), none).ok());
    for (const Probe& probe :
         {Int64Probe({std::nullopt, std::nullopt}), Int64Probe({0, step}, Bools({false, false}))}) {
      EXPECT_EQ(Matches(**table, probe), (Ids{{}, {}}));
    }
  }
}

// ---- Parts and threads ----

constexpr int64_t kRowsPerBatch = 40;

// A build input of `parts` parts of two batches, with duplicates (about 30%) and NULL keys: dense
// keys, or spread out (hashed).
BuildData ManyParts(std::size_t parts, bool hashed) {
  BuildData data({LogicalType::kBigInt});
  for (std::size_t part = 0; part < parts; ++part) {
    for (int64_t b = 0; b < 2; ++b) {
      std::vector<std::optional<int64_t>> keys;
      keys.reserve(kRowsPerBatch);
      for (int64_t r = 0; r < kRowsPerBatch; ++r) {
        const int64_t row = (((static_cast<int64_t>(part) * 2) + b) * kRowsPerBatch) + r;
        const int64_t k = row % 10 < 3 ? row / 3 : row;  // 3 rows in 10 repeat a key
        keys.push_back(row % 17 == 0 ? std::nullopt
                                     : std::optional<int64_t>(hashed ? (k * 7919) + 3 : k));
      }
      data.Add(part, {Int64s(keys)});
    }
  }
  return data;
}

// Every third key of ManyParts' range (hits and misses), and a NULL.
Probe ManyPartsProbe(std::size_t parts, bool hashed) {
  std::vector<std::optional<int64_t>> keys;
  keys.reserve(parts * static_cast<std::size_t>(kRowsPerBatch));
  for (int64_t k = 0; k < static_cast<int64_t>(parts) * 2 * kRowsPerBatch; k += 3) {
    keys.emplace_back(hashed ? (k * 7919) + 3 : k);
  }
  keys.emplace_back(std::nullopt);
  return Int64Probe(keys);
}

// Whatever order the parts arrive in, and on any number of threads, the table is the same.
TEST_F(JoinTableTest, PartsMayArriveInAnyOrder) {
  const auto pool = MakeThreadPool();
  for (const bool hashed : {false, true}) {
    SCOPED_TRACE(hashed);
    const BuildData data = ManyParts(6, hashed);
    const Probe probe = ManyPartsProbe(6, hashed);
    std::optional<std::vector<std::pair<std::uint32_t, std::uint32_t>>> rows;
    std::optional<Ids> matches;
    for (arrow::internal::Executor* executor : Executors(pool.get())) {
      for (const std::vector<std::size_t>& order :
           {std::vector<std::size_t>{0, 1, 2, 3, 4, 5}, std::vector<std::size_t>{5, 4, 3, 2, 1, 0},
            std::vector<std::size_t>{3, 0, 5, 1, 4, 2}}) {
        auto table = BuildTable(data, executor, order);
        ASSERT_TRUE(table.ok()) << table.status().ToString();
        if (!rows.has_value() || !matches.has_value()) {
          rows = RowRefs(**table);
          matches = Matches(**table, probe);
          EXPECT_EQ(*matches, Reference(data, probe));
          continue;
        }
        EXPECT_EQ(RowRefs(**table), *rows);
        EXPECT_EQ(Matches(**table, probe), *matches);
      }
    }
  }
}

// Parts appended in pool tasks (as part tasks do) and added on this thread in a shuffled order give
// the table a serial build gives.
TEST_F(JoinTableTest, PartsBuiltOnWorkers) {
  const auto pool = MakeThreadPool();
  for (const bool hashed : {false, true}) {
    SCOPED_TRACE(hashed);
    const BuildData data = ManyParts(12, hashed);
    auto spec = JoinBuildSpec::Make(data.schema(), data.keys());
    ASSERT_TRUE(spec.ok());
    std::vector<std::shared_ptr<const JoinBuildPart>> parts(data.parts().size());
    ASSERT_TRUE(ForEach(pool.get(), parts.size(), [&](std::size_t p) {
                  auto part = std::make_shared<JoinBuildPart>(*spec, nullptr);
                  for (const Batch& batch : data.parts()[p]) {
                    ARROW_RETURN_NOT_OK(part->Append(batch, arrow::default_memory_pool()));
                  }
                  parts[p] = std::move(part);
                  return arrow::Status::OK();
                }).ok());
    auto table = Assemble(*spec, parts, pool.get(), {7, 2, 11, 0, 5, 9, 1, 3, 10, 4, 8, 6});
    ASSERT_TRUE(table.ok()) << table.status().ToString();
    auto serial = BuildTable(data, nullptr);
    ASSERT_TRUE(serial.ok()) << serial.status().ToString();
    EXPECT_EQ(RowRefs(**table), RowRefs(**serial));
    const Probe probe = ManyPartsProbe(12, hashed);
    EXPECT_EQ(Matches(**table, probe), Matches(**serial, probe));
  }
}

// One thread or four: the same rows in the same order, and the same matches.
TEST_F(JoinTableTest, SameTableForAnyThreadCount) {
  const auto pool = MakeThreadPool();
  for (const bool hashed : {false, true}) {
    SCOPED_TRACE(hashed);
    const BuildData data = ManyParts(24, hashed);
    const Probe probe = ManyPartsProbe(24, hashed);
    auto serial = BuildTable(data, nullptr);
    auto parallel = BuildTable(data, pool.get());
    ASSERT_TRUE(serial.ok()) << serial.status().ToString();
    ASSERT_TRUE(parallel.ok()) << parallel.status().ToString();
    EXPECT_EQ((*serial)->layout(),
              hashed ? JoinTable::Layout::kHashed : JoinTable::Layout::kDirect);
    EXPECT_EQ((*parallel)->layout(), (*serial)->layout());
    EXPECT_EQ(RowRefs(**parallel), RowRefs(**serial));
    const Ids matches = Matches(**serial, probe);
    EXPECT_EQ(Matches(**parallel, probe), matches);
    EXPECT_EQ(matches, Reference(data, probe));
  }
}

// Probes on many threads at once read the one table (no lock, nothing written): each gets the
// matches a probe alone gets.
TEST_F(JoinTableTest, ProbesShareTheTable) {
  const auto pool = MakeThreadPool();
  for (const bool hashed : {false, true}) {
    SCOPED_TRACE(hashed);
    const BuildData data = ManyParts(8, hashed);
    auto table = BuildTable(data, pool.get());
    ASSERT_TRUE(table.ok()) << table.status().ToString();
    const Probe probe = ManyPartsProbe(8, hashed);
    const std::vector<JoinMatches> alone = FindAll(**table, probe);
    constexpr std::size_t kProbes = 16;
    std::vector<std::vector<JoinMatches>> shared(kProbes, std::vector<JoinMatches>(alone.size()));
    ASSERT_TRUE(ForEach(pool.get(), kProbes, [&](std::size_t i) {
                  return (*table)->Find(probe.keys, probe.selection.get(),
                                        arrow::default_memory_pool(), shared[i]);
                }).ok());
    for (const std::vector<JoinMatches>& matches : shared) {
      EXPECT_EQ(MatchIds(**table, matches), MatchIds(**table, alone));
    }
  }
}

// ---- Errors ----

// What a correct physical plan never sends is Invalid (exit code 1), never NotImplemented.
TEST_F(JoinTableTest, RejectsWhatThePlannerMustNotSend) {
  BuildData data({LogicalType::kBigInt, LogicalType::kVarchar});
  data.Add(0, {Int64s({1, 2}), Strings({"a", "b"})});
  const auto schema = data.schema();
  const auto bigint = Column(0, "k0", LogicalType::kBigInt);
  const auto expect_invalid = [](const arrow::Status& status) {
    EXPECT_TRUE(status.IsInvalid()) << status.ToString();
  };
  // The spec.
  expect_invalid(JoinBuildSpec::Make(nullptr, {bigint}).status());
  expect_invalid(JoinBuildSpec::Make(schema, {}).status());
  expect_invalid(JoinBuildSpec::Make(schema, {Column(-1, "k", LogicalType::kBigInt)}).status());
  expect_invalid(JoinBuildSpec::Make(schema, {Column(3, "k", LogicalType::kBigInt)}).status());
  expect_invalid(JoinBuildSpec::Make(schema, {Column(0, "k0", LogicalType::kInteger)}).status());
  expect_invalid(JoinBuildSpec::Make(arrow::schema({arrow::field("d", arrow::float64())}),
                                     {Column(0, "d", LogicalType::kDouble)})
                     .status());
  expect_invalid(JoinBuildSpec::Make(arrow::schema({arrow::field("b", arrow::boolean())}),
                                     {Column(0, "b", LogicalType::kBoolean)})
                     .status());
  auto spec = JoinBuildSpec::Make(schema, data.keys());
  ASSERT_TRUE(spec.ok()) << spec.status().ToString();
  EXPECT_FALSE((*spec)->direct_candidate());
  // A part.
  JoinBuildPart part(*spec, nullptr);
  expect_invalid(part.Append(Batch{}, arrow::default_memory_pool()));
  expect_invalid(
      part.Append(Batch{.data = arrow::RecordBatch::Make(
                            arrow::schema({arrow::field("x", arrow::int64())}), 1, {Int64s({1})}),
                        .selection = nullptr},
                  arrow::default_memory_pool()));
  expect_invalid(
      part.Append(Batch{.data = data.parts()[0][0].data, .selection = Bools({true, true, true})},
                  arrow::default_memory_pool()));
  EXPECT_EQ(part.input_rows(), 0);  // a part that failed is unchanged
  // The builder.
  expect_invalid(JoinTableBuilder::Make(nullptr, 1, nullptr, 1, nullptr).status());
  expect_invalid(JoinTableBuilder::Make(*spec, -1, nullptr, 1, nullptr).status());
  auto parts = MakeParts(data, *spec);
  ASSERT_TRUE(parts.ok()) << parts.status().ToString();
  auto other_spec = JoinBuildSpec::Make(schema, data.keys());  // equal, but another build's
  ASSERT_TRUE(other_spec.ok());
  auto other_parts = MakeParts(data, *other_spec);
  ASSERT_TRUE(other_parts.ok());
  {
    auto builder = JoinTableBuilder::Make(*spec, 2, nullptr, 1, nullptr);
    ASSERT_TRUE(builder.ok());
    expect_invalid((*builder)->Add(-1, parts->front()));
    expect_invalid((*builder)->Add(2, parts->front()));
    expect_invalid((*builder)->Add(0, nullptr));
    expect_invalid((*builder)->Add(0, other_parts->front()));
    ASSERT_TRUE((*builder)->Add(1, parts->front()).ok());
    expect_invalid((*builder)->Add(1, parts->front()));  // twice
    const arrow::Status missing = (*builder)->Finish().status();
    expect_invalid(missing);
    EXPECT_NE(missing.message().find("part 0 was not added"), std::string::npos)
        << missing.ToString();
    expect_invalid((*builder)->Finish().status());       // twice
    expect_invalid((*builder)->Add(0, parts->front()));  // after Finish
  }
  // A probe.
  auto table = Assemble(*spec, *parts, nullptr);
  ASSERT_TRUE(table.ok()) << table.status().ToString();
  std::vector<JoinMatches> out(3);
  const auto find = [&](const std::vector<std::shared_ptr<arrow::Array>>& keys,
                        const std::shared_ptr<arrow::BooleanArray>& selection,
                        std::span<JoinMatches> matches) {
    return (*table)->Find(keys, selection.get(), arrow::default_memory_pool(), matches);
  };
  const auto ints = Int64s({1, 2, 3});
  const auto strings = Strings({"a", "b", "c"});
  ASSERT_TRUE(find({ints, strings}, nullptr, out).ok());
  expect_invalid(find({ints}, nullptr, out));
  expect_invalid(find({strings, ints}, nullptr, out));
  expect_invalid(find({ints, nullptr}, nullptr, out));
  expect_invalid(find({nullptr, strings}, nullptr, out));
  expect_invalid(find({ints, Strings({"a", "b"})}, nullptr, out));
  expect_invalid(find({ints, strings}, Bools({true, false}), out));
  expect_invalid(find({ints, strings}, nullptr, std::span(out).first(2)));
}

// ---- Memory ----

// Every part, merge, finish and probe that passes the memory limit fails with OutOfMemory and
// leaves nothing behind; a finished table holds its memory until it goes.
TEST_F(JoinTableTest, RunsOutOfMemoryCleanly) {
  const auto pool = MakeThreadPool();
  const auto expect_oom = [](const arrow::Status& status) {
    EXPECT_TRUE(status.IsOutOfMemory()) << status.ToString();
  };
  for (const bool hashed : {false, true}) {
    SCOPED_TRACE(hashed);
    const BuildData data = ManyParts(4, hashed);
    const Probe probe = ManyPartsProbe(4, hashed);
    auto unlimited = BuildTable(data, nullptr);
    ASSERT_TRUE(unlimited.ok()) << unlimited.status().ToString();
    const Ids expected = Matches(**unlimited, probe);
    auto spec = JoinBuildSpec::Make(data.schema(), data.keys());
    ASSERT_TRUE(spec.ok());

    // A part past a small limit: OutOfMemory, the part unchanged, nothing left once it goes. Its
    // pool too: NULL keys and a selection need a bitmap.
    {
      MemoryBudget budget(1024);
      {
        JoinBuildPart part(*spec, &budget);
        expect_oom(part.Append(data.parts()[0][0], &budget));
        EXPECT_EQ(part.input_rows(), 0);
        EXPECT_EQ(part.num_rows(), 0);
        MemoryBudget empty(0);
        const std::vector<std::optional<bool>> keep(static_cast<std::size_t>(kRowsPerBatch), true);
        expect_oom(
            part.Append(Batch{.data = data.parts()[0][0].data, .selection = Bools(keep)}, &empty));
        EXPECT_EQ(part.input_rows(), 0);
      }
      EXPECT_EQ(budget.bytes_allocated(), 0);
    }
    // Without a limit a budget still counts: a table holds its memory until it goes.
    {
      MemoryBudget budget(std::nullopt);
      {
        auto table = BuildTable(data, pool.get(), {}, &budget, &budget);
        ASSERT_TRUE(table.ok()) << table.status().ToString();
        EXPECT_GT(budget.bytes_allocated(), 0);
        EXPECT_EQ(Matches(**table, probe), expected);
      }
      EXPECT_EQ(budget.bytes_allocated(), 0);
    }
    // A builder for absurdly many parts.
    {
      MemoryBudget budget(kGiB);
      expect_oom(JoinTableBuilder::Make(*spec, kGiB * 1024, nullptr, 1, &budget).status());
      expect_oom(
          JoinTableBuilder::Make(*spec, std::numeric_limits<int64_t>::max(), nullptr, 1, nullptr)
              .status());
      EXPECT_EQ(budget.bytes_allocated(), 0);
    }
    for (arrow::internal::Executor* executor : Executors(pool.get())) {
      // Every limit up to the one the build fits (in steps of 512 bytes; without an executor also
      // in steps of 16 bytes over the last 8 KiB, where Finish runs out): each failure is
      // OutOfMemory and gives everything back; the build gives the matches of an unlimited one.
      const auto build_within = [&](int64_t limit) {
        MemoryBudget budget(limit);
        bool built = false;
        {
          auto table = BuildTable(data, executor, {}, &budget, &budget);
          if (table.ok()) {
            built = true;
            EXPECT_GT(budget.bytes_allocated(), 0);  // a finished table holds its memory
            EXPECT_EQ(Matches(**table, probe), expected);
          } else {
            expect_oom(table.status());
          }
        }
        EXPECT_EQ(budget.bytes_allocated(), 0) << limit;
        return built;
      };
      int64_t fits = 0;
      while (!build_within(fits)) {
        fits += 512;
        ASSERT_LT(fits, 64 * 1024 * 1024);
      }
      for (int64_t limit = std::max<int64_t>(0, fits - 8192); executor == nullptr && limit < fits;
           limit += 16) {
        build_within(limit);
      }
      // A full budget fails the first merge: Add returns the failure, or Merged() does once the
      // lanes run on the executor; every later call returns it too.
      {
        MemoryBudget budget(kGiB);
        auto parts = MakeParts(data, *spec, &budget, &budget);
        ASSERT_TRUE(parts.ok()) << parts.status().ToString();
        auto builder = JoinTableBuilder::Make(*spec, 4, executor, kThreads, &budget);
        ASSERT_TRUE(builder.ok());
        const int64_t pinned = kGiB - budget.bytes_allocated();
        ASSERT_TRUE(budget.Reserve(pinned).ok());
        const arrow::Status added = (*builder)->Add(0, (*parts)[0]);
        if (added.ok()) {
          EXPECT_NE(executor, nullptr);
          expect_oom((*builder)->Merged());
        } else {
          expect_oom(added);
        }
        expect_oom((*builder)->Add(1, (*parts)[1]));
        expect_oom((*builder)->Finish().status());
        builder->reset();
        parts->clear();
        budget.Release(pinned);
        EXPECT_EQ(budget.bytes_allocated(), 0);
      }
      // A budget that fills up once every part is merged fails Finish.
      {
        MemoryBudget budget(kGiB);
        auto parts = MakeParts(data, *spec, &budget, &budget);
        ASSERT_TRUE(parts.ok()) << parts.status().ToString();
        auto builder = JoinTableBuilder::Make(*spec, 4, executor, kThreads, &budget);
        ASSERT_TRUE(builder.ok());
        for (int64_t p = 0; p < 4; ++p) {
          ASSERT_TRUE((*builder)->Add(p, (*parts)[static_cast<std::size_t>(p)]).ok());
        }
        ASSERT_TRUE((*builder)->Merged().ok());
        const int64_t pinned = kGiB - budget.bytes_allocated();
        ASSERT_TRUE(budget.Reserve(pinned).ok());
        expect_oom((*builder)->Finish().status());
        budget.Release(pinned);
        builder->reset();
        parts->clear();
        EXPECT_EQ(budget.bytes_allocated(), 0);
      }
      // Under memory pressure the partitions are built one at a time: the same table.
      {
        MemoryBudget budget(kGiB);
        ASSERT_TRUE(budget.Reserve((kGiB / 2) + 1).ok());
        ASSERT_TRUE(budget.under_pressure());
        {
          auto table = BuildTable(data, executor, {}, &budget, &budget);
          ASSERT_TRUE(table.ok()) << table.status().ToString();
          EXPECT_EQ(Matches(**table, probe), expected);
          EXPECT_EQ(RowRefs(**table), RowRefs(**unlimited));
        }
        budget.Release((kGiB / 2) + 1);
        EXPECT_EQ(budget.bytes_allocated(), 0);
      }
    }
    // A probe whose pool runs out (NULL keys and a selection need a bitmap) fails, and the table
    // still answers the next probe.
    {
      std::vector<std::optional<int64_t>> keys;
      std::vector<std::optional<bool>> keep;
      keys.reserve(2000);
      keep.reserve(2000);
      for (int64_t i = 0; i < 2000; ++i) {
        keys.push_back(i % 9 == 0 ? std::nullopt
                                  : std::optional<int64_t>(hashed ? (i * 7919) + 3 : i));
        keep.emplace_back(i % 2 == 0);
      }
      const Probe big = Int64Probe(keys, Bools(keep));
      MemoryBudget tiny(64);
      std::vector<JoinMatches> out(keys.size());
      expect_oom((*unlimited)->Find(big.keys, big.selection.get(), &tiny, out));
      EXPECT_EQ(tiny.bytes_allocated(), 0);
      EXPECT_EQ(Matches(**unlimited, big), Reference(data, big));
    }
    // A selective probe (only its rows are hashed, through a copy of their keys) on every pool
    // limit up to the one it fits: OutOfMemory, then the matches.
    {
      std::vector<std::optional<int64_t>> keys;
      std::vector<std::optional<bool>> keep;
      keys.reserve(2000);
      keep.reserve(2000);
      for (int64_t i = 0; i < 2000; ++i) {
        keys.emplace_back(hashed ? (i * 7919) + 3 : i);
        keep.emplace_back(i % 5 == 0);
      }
      const Probe selective = Int64Probe(keys, Bools(keep));
      const Ids found = Reference(data, selective);
      std::vector<JoinMatches> out(keys.size());
      for (int64_t limit = 0;; limit += 64) {
        MemoryBudget budget(limit);
        const arrow::Status status =
            (*unlimited)->Find(selective.keys, selective.selection.get(), &budget, out);
        EXPECT_EQ(budget.bytes_allocated(), 0);
        if (status.ok()) {
          EXPECT_EQ(MatchIds(**unlimited, out), found);
          break;
        }
        expect_oom(status);
        ASSERT_LT(limit, 1024 * 1024);
      }
    }
  }
}

// A part that cannot be handed to the partitions (std::bad_alloc from the executor's Submit) fails
// the build with OutOfMemory, whichever parts came before it: every later Add returns that failure,
// in or out of order, and so do Merged() and Finish() (never a part "not added"); nothing is left
// in the budget once the builder and the parts go.
TEST_F(JoinTableTest, AFailedReleaseFailsTheBuild) {
  const auto pool = MakeThreadPool();
  for (const bool hashed : {false, true}) {
    SCOPED_TRACE(hashed);
    const BuildData data = ManyParts(4, hashed);
    auto spec = JoinBuildSpec::Make(data.schema(), data.keys());
    ASSERT_TRUE(spec.ok());
    for (const std::vector<std::size_t>& order :
         {std::vector<std::size_t>{0, 1, 2, 3}, std::vector<std::size_t>{2, 3, 0, 1}}) {
      MemoryBudget budget(std::nullopt);
      {
        auto parts = MakeParts(data, *spec, &budget, &budget);
        ASSERT_TRUE(parts.ok()) << parts.status().ToString();
        // Part 0 is the first released: every lane is idle and gets a task, and lane 5's throws.
        ThrowingExecutor executor(pool.get(), /*throw_at=*/5);
        auto builder = JoinTableBuilder::Make(*spec, 4, &executor, kThreads, &budget);
        ASSERT_TRUE(builder.ok()) << builder.status().ToString();
        std::optional<arrow::Status> failure;
        for (const std::size_t part : order) {
          const arrow::Status added = (*builder)->Add(static_cast<int64_t>(part), (*parts)[part]);
          if (failure.has_value()) {
            EXPECT_EQ(added.ToString(), failure->ToString()) << part;
          } else if (part == 0) {
            EXPECT_TRUE(added.IsOutOfMemory()) << added.ToString();
            failure = added;
          } else {
            EXPECT_TRUE(added.ok()) << added.ToString();  // nothing released yet
          }
        }
        ASSERT_TRUE(failure.has_value());
        EXPECT_EQ((*builder)->Merged().ToString(), failure->ToString());
        EXPECT_EQ((*builder)->Finish().status().ToString(), failure->ToString());
        // Part 0's tasks only: no later part was released, and no table built.
        EXPECT_EQ(executor.spawns(), static_cast<int>(kJoinPartitions));
        builder->reset();
        parts->clear();
      }
      EXPECT_EQ(budget.bytes_allocated(), 0);
    }
  }
}

// A merge that fails on the executor fails the build as soon as it is known: an Add that releases
// only a part without rows, or nothing (out of order), returns it, and so does every later call,
// Merged(), Finish() and an Add after Finish() included.
TEST_F(JoinTableTest, AMergeFailureOnTheExecutorFailsEveryLaterCall) {
  const auto pool = MakeThreadPool();
  BuildData data({LogicalType::kBigInt});
  data.Add(0, {Int64s({1, 2, 3, 4})})
      .Add(1, {Int64s({std::nullopt, std::nullopt})})  // no row kept
      .Add(2, {Int64s({5, 6})})
      .Add(3, {Int64s({7, 8})});
  auto spec = JoinBuildSpec::Make(data.schema(), data.keys());
  ASSERT_TRUE(spec.ok());
  MemoryBudget budget(kGiB);
  {
    auto parts = MakeParts(data, *spec, &budget, &budget);
    ASSERT_TRUE(parts.ok()) << parts.status().ToString();
    auto builder = JoinTableBuilder::Make(*spec, 4, pool.get(), kThreads, &budget);
    ASSERT_TRUE(builder.ok()) << builder.status().ToString();
    const int64_t pinned = kGiB - budget.bytes_allocated();
    ASSERT_TRUE(budget.Reserve(pinned).ok());  // no room for the partitions' runs
    // Add only queues part 0's merges; they fail on the pool, which then goes idle.
    ASSERT_TRUE((*builder)->Add(0, (*parts)[0]).ok());
    pool->WaitForIdle();
    const arrow::Status failure = (*builder)->Add(1, (*parts)[1]);
    EXPECT_TRUE(failure.IsOutOfMemory()) << failure.ToString();
    EXPECT_EQ((*builder)->Add(3, (*parts)[3]).ToString(), failure.ToString());
    EXPECT_EQ((*builder)->Merged().ToString(), failure.ToString());
    EXPECT_EQ((*builder)->Add(2, (*parts)[2]).ToString(), failure.ToString());
    EXPECT_EQ((*builder)->Finish().status().ToString(), failure.ToString());
    EXPECT_EQ((*builder)->Add(2, (*parts)[2]).ToString(), failure.ToString());
    budget.Release(pinned);
    builder->reset();
    parts->clear();
  }
  EXPECT_EQ(budget.bytes_allocated(), 0);
}

}  // namespace
}  // namespace antb1::exec
