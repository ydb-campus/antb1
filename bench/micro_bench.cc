// Micro benchmarks of antb1's hot paths (Google Benchmark; docs/benchmarks.md). `pixi run bench`
// runs them in build/static (preset `static`: the static Arrow, -O3, ThinLTO) and writes
// micro.json into build/static/bench-results; bench.yml publishes the numbers from main. Numbers
// never gate a PR. Inputs are synthetic (splitmix64), never ClickBench data.

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <initializer_list>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <tuple>
#include <typeinfo>
#include <unordered_map>
#include <utility>
#include <vector>

#include <arrow/api.h>
#include <arrow/compute/api.h>
#include <arrow/io/file.h>
#include <arrow/util/bit_util.h>
#include <benchmark/benchmark.h>
#include <parquet/arrow/writer.h>

#include "antb1/common/narrow.h"
#include "antb1/common/version.h"
#include "antb1/exec/aggregate_state.h"
#include "antb1/exec/join_table.h"
#include "antb1/exec/memory_budget.h"
#include "antb1/exec/operator.h"
#include "antb1/exec/sort.h"
#include "antb1/io/parquet_table.h"
#include "antb1/plan/logical_plan.h"
#include "antb1/plan/table.h"
#include "antb1/plan/types.h"
#include "antb1/sql/parser.h"

#include "group_table.h"
#include "key_table.h"
#include <unistd.h>

namespace antb1::bench {
namespace {

constexpr int64_t kRows = int64_t{1} << 20;  // 16 batches of 64Ki rows
constexpr int64_t kBatchSize = int64_t{64} * 1024;

uint64_t SplitMix64(uint64_t& state) {
  state += 0x9E3779B97F4A7C15ULL;
  uint64_t z = state;
  z = (z ^ (z >> 30U)) * 0xBF58476D1CE4E5B9ULL;
  z = (z ^ (z >> 27U)) * 0x94D049BB133111EBULL;
  return z ^ (z >> 31U);
}

// kRows int16 values in [-1000, 1000], about one in eight 0 (a WHERE x <> 0 selects the rest);
// nullptr if the array cannot be built.
const std::shared_ptr<arrow::Array>& Int16Values() {
  static const std::shared_ptr<arrow::Array> values = []() -> std::shared_ptr<arrow::Array> {
    arrow::Int16Builder builder;
    if (!builder.Reserve(kRows).ok()) {
      return nullptr;
    }
    uint64_t state = 16;
    for (int64_t i = 0; i < kRows; ++i) {
      const uint64_t r = SplitMix64(state);
      const auto v = static_cast<int16_t>(static_cast<int64_t>(r % 2001U) - 1000);
      builder.UnsafeAppend(r % 8U == 0 ? int16_t{0} : v);
    }
    return builder.Finish().ValueOr(nullptr);
  }();
  return values;
}

// kRows int64 values of up to 62 bits: their sum leaves the int64 range; nullptr on failure.
const std::shared_ptr<arrow::Array>& Int64Values() {
  static const std::shared_ptr<arrow::Array> values = []() -> std::shared_ptr<arrow::Array> {
    arrow::Int64Builder builder;
    if (!builder.Reserve(kRows).ok()) {
      return nullptr;
    }
    uint64_t state = 64;
    for (int64_t i = 0; i < kRows; ++i) {
      builder.UnsafeAppend(static_cast<int64_t>(SplitMix64(state) >> 2U));
    }
    return builder.Finish().ValueOr(nullptr);
  }();
  return values;
}

// Where BM_ScanColumn's file lives (TMPDIR or /tmp, unique per process).
std::filesystem::path ScanFilePath() {
  return std::filesystem::temp_directory_path() /
         ("antb1-bench-scan-" + std::to_string(::getpid()) + ".parquet");
}

// A Parquet file with the Int16Values() column in 64Ki-row row groups, written once per run (empty
// path if that fails).
const std::filesystem::path& ScanFile() {
  static const std::filesystem::path path = [] {
    auto file = ScanFilePath();
    if (Int16Values() == nullptr) {
      return std::filesystem::path();
    }
    const auto table =
        arrow::Table::Make(arrow::schema({arrow::field("x", arrow::int16())}), {Int16Values()});
    auto out = arrow::io::FileOutputStream::Open(file.string());
    if (!out.ok() ||
        !parquet::arrow::WriteTable(*table, arrow::default_memory_pool(), *out, kBatchSize).ok() ||
        !(*out)->Close().ok()) {
      return std::filesystem::path();
    }
    return file;
  }();
  return path;
}

void BM_ParseSmallAggQuery(benchmark::State& state) {
  constexpr std::string_view kSql =
      "SELECT COUNT(*), SUM(Price) AS total, AVG(Quantity) FROM orders "
      "WHERE Region <> 0 AND Quantity >= 1.5";
  for (auto _ : state) {
    auto stmt = sql::Parse(kSql);
    benchmark::DoNotOptimize(stmt);
  }
}
BENCHMARK(BM_ParseSmallAggQuery);

// SUM over SMALLINT: antb1's exact 128-bit state...
void BM_SumInt16_Exact(benchmark::State& state) {
  const auto& values = Int16Values();
  if (values == nullptr) {
    state.SkipWithError("cannot build the input");
    return;
  }
  for (auto _ : state) {
    auto sum = exec::MakeAggregateState(plan::AggKind::kSum, plan::LogicalType::kSmallInt,
                                        plan::LogicalType::kHugeInt);
    if (!sum.ok() || !(*sum)->Consume(*values, nullptr).ok()) {
      state.SkipWithError("SUM failed");
      return;
    }
    auto result = (*sum)->Finalize(arrow::default_memory_pool());
    benchmark::DoNotOptimize(result);
  }
  state.SetItemsProcessed(state.iterations() * kRows);
}
BENCHMARK(BM_SumInt16_Exact);

// ...versus Arrow's "sum" kernel, which returns int64 and wraps (never used by the engine).
void BM_SumInt16_ArrowKernel(benchmark::State& state) {
  const auto& values = Int16Values();
  if (values == nullptr) {
    state.SkipWithError("cannot build the input");
    return;
  }
  for (auto _ : state) {
    auto sum = arrow::compute::Sum(values);
    benchmark::DoNotOptimize(sum);
  }
  state.SetItemsProcessed(state.iterations() * kRows);
}
BENCHMARK(BM_SumInt16_ArrowKernel);

// COUNT(*) WHERE x <> 0: the comparison kernel and the selection's true count.
void BM_NotEqualTrueCount(benchmark::State& state) {
  if (Int16Values() == nullptr) {
    state.SkipWithError("cannot build the input");
    return;
  }
  const arrow::Datum values(Int16Values());
  const arrow::Datum zero(std::make_shared<arrow::Int16Scalar>(0));
  int64_t selected = 0;
  for (auto _ : state) {
    auto mask = arrow::compute::CallFunction("not_equal", {values, zero});
    if (!mask.ok()) {
      state.SkipWithError("not_equal failed");
      return;
    }
    selected = std::static_pointer_cast<arrow::BooleanArray>(mask->make_array())->true_count();
    benchmark::DoNotOptimize(selected);
  }
  state.SetItemsProcessed(state.iterations() * kRows);
  state.counters["selected"] = static_cast<double>(selected);
}
BENCHMARK(BM_NotEqualTrueCount);

// AVG over BIGINT: exact 128-bit accumulation, one division at the end.
void BM_Int128AvgAccumulate(benchmark::State& state) {
  const auto& values = Int64Values();
  if (values == nullptr) {
    state.SkipWithError("cannot build the input");
    return;
  }
  for (auto _ : state) {
    auto avg = exec::MakeAggregateState(plan::AggKind::kAvg, plan::LogicalType::kBigInt,
                                        plan::LogicalType::kDouble);
    if (!avg.ok() || !(*avg)->Consume(*values, nullptr).ok()) {
      state.SkipWithError("AVG failed");
      return;
    }
    auto result = (*avg)->Finalize(arrow::default_memory_pool());
    benchmark::DoNotOptimize(result);
  }
  state.SetItemsProcessed(state.iterations() * kRows);
}
BENCHMARK(BM_Int128AvgAccumulate);

// Decoding one SMALLINT column of a Parquet file through io::ParquetTable::Scan (64Ki-row batches).
void BM_ScanColumn(benchmark::State& state) {
  const auto& file = ScanFile();
  auto table = file.empty() ? arrow::Result<std::shared_ptr<io::ParquetTable>>(
                                  arrow::Status::IOError("cannot write the scan file"))
                            : io::ParquetTable::Open({file.string()});
  if (!table.ok()) {
    state.SkipWithError(table.status().ToString());
    return;
  }
  int64_t rows = 0;
  for (auto _ : state) {
    rows = 0;
    auto reader = (*table)->Scan({0}, kBatchSize);
    if (!reader.ok()) {
      state.SkipWithError(reader.status().ToString());
      return;
    }
    std::shared_ptr<arrow::RecordBatch> batch;
    while (true) {
      if (const arrow::Status read = (*reader)->ReadNext(&batch); !read.ok()) {
        state.SkipWithError(read.ToString());
        return;
      }
      if (batch == nullptr) {
        break;
      }
      rows += batch->num_rows();
    }
    benchmark::DoNotOptimize(rows);
  }
  state.SetItemsProcessed(state.iterations() * kRows);
  state.counters["rows"] = static_cast<double>(rows);
}
BENCHMARK(BM_ScanColumn);

// ---- A string filter: decoded into arrays then matched, against filtered in the scan ----

// A Parquet file of URL-like strings (about 60 bytes, 1 in 50 contains "/7/") in 64Ki-row row
// groups, written once per run (empty path if that fails).
std::filesystem::path UrlFilePath() {
  return std::filesystem::temp_directory_path() /
         ("antb1-bench-urls-" + std::to_string(::getpid()) + ".parquet");
}

const std::filesystem::path& UrlFile() {
  static const std::filesystem::path path = [] {
    auto file = UrlFilePath();
    arrow::StringBuilder urls;
    for (int64_t r = 0; r < kRows; ++r) {
      const int64_t page = (r * 2654435761LL) % 100000;
      const std::string url = "https://www.example.org/section/" + std::to_string(r % 50) + "/" +
                              std::to_string(page) + "?utm_source=bench";
      if (!urls.Append(url).ok()) {
        return std::filesystem::path();
      }
    }
    const auto table = arrow::Table::Make(arrow::schema({arrow::field("u", arrow::utf8())}),
                                          {urls.Finish().ValueOrDie()});
    auto out = arrow::io::FileOutputStream::Open(file.string());
    if (!out.ok() ||
        !parquet::arrow::WriteTable(*table, arrow::default_memory_pool(), *out, kBatchSize).ok() ||
        !(*out)->Close().ok()) {
      return std::filesystem::path();
    }
    return file;
  }();
  return path;
}

bool HasNeedle(std::string_view text) { return text.find("/7/") != std::string_view::npos; }

// Keeps the rows whose string contains "/7/".
class NeedleFilter final : public plan::ScanFilter {
 public:
  [[nodiscard]] const std::vector<int>& columns() const override { return columns_; }
  arrow::Status Apply(int /*column*/, const plan::ScanValues& values, int64_t offset,
                      std::uint8_t* selected) const override {
    for (int64_t i = 0; i < values.rows; ++i) {
      if (!HasNeedle(values.strings[static_cast<std::size_t>(i)])) {
        arrow::bit_util::ClearBit(selected, offset + i);
      }
    }
    return arrow::Status::OK();
  }

 private:
  std::vector<int> columns_{0};
};

void ScanUrls(benchmark::State& state, bool pushed) {
  const auto& file = UrlFile();
  auto table = file.empty() ? arrow::Result<std::shared_ptr<io::ParquetTable>>(
                                  arrow::Status::IOError("cannot write the URL file"))
                            : io::ParquetTable::Open({file.string()});
  if (!table.ok()) {
    state.SkipWithError(table.status().ToString());
    return;
  }
  const auto filter = std::make_shared<NeedleFilter>();
  int64_t matches = 0;
  for (auto _ : state) {
    matches = 0;
    for (int64_t part = 0; part < (*table)->num_parts(); ++part) {
      auto reader =
          pushed ? (*table)->ScanPart(part, {0}, kBatchSize, arrow::default_memory_pool(), filter)
                 : (*table)->ScanPart(part, {0}, kBatchSize);
      if (!reader.ok()) {
        state.SkipWithError(reader.status().ToString());
        return;
      }
      std::shared_ptr<arrow::RecordBatch> batch;
      while ((*reader)->ReadNext(&batch).ok() && batch != nullptr) {
        if (pushed) {
          matches += batch->num_rows();
          continue;
        }
        const auto& urls = static_cast<const arrow::BinaryArray&>(*batch->column(0));
        for (int64_t i = 0; i < urls.length(); ++i) {
          matches += HasNeedle(urls.GetView(i)) ? 1 : 0;
        }
      }
    }
    benchmark::DoNotOptimize(matches);
  }
  state.SetItemsProcessed(state.iterations() * kRows);
  state.counters["matches"] = static_cast<double>(matches);
}

// io::ParquetTable::ScanPart, then the filter over the BinaryArray (today's path).
void BM_StringFilterAfterScan(benchmark::State& state) { ScanUrls(state, false); }
BENCHMARK(BM_StringFilterAfterScan)->Unit(benchmark::kMillisecond);

// io::ParquetTable::ScanPart with the filter pushed into the scan (ADR 0020).
void BM_StringFilterInScan(benchmark::State& state) { ScanUrls(state, true); }
BENCHMARK(BM_StringFilterInScan)->Unit(benchmark::kMillisecond);

// A random BIGINT key and an 8 to 23 byte VARCHAR payload, kRows rows in 64Ki-row batches.
const arrow::RecordBatchVector& SortInput() {
  static const arrow::RecordBatchVector batches = [] {
    arrow::RecordBatchVector out;
    const auto schema =
        arrow::schema({arrow::field("k", arrow::int64()), arrow::field("s", arrow::binary())});
    uint64_t state = 64;
    for (int64_t start = 0; start < kRows; start += kBatchSize) {
      arrow::Int64Builder keys;
      arrow::BinaryBuilder payload;
      for (int64_t i = 0; i < kBatchSize; ++i) {
        const uint64_t r = SplitMix64(state);
        if (!keys.Append(static_cast<int64_t>(r)).ok() ||
            !payload.Append(std::string(8 + (r % 16U), static_cast<char>('a' + (r % 26U)))).ok()) {
          return arrow::RecordBatchVector{};
        }
      }
      auto k = keys.Finish();
      auto s = payload.Finish();
      if (!k.ok() || !s.ok()) {
        return arrow::RecordBatchVector{};
      }
      out.push_back(arrow::RecordBatch::Make(schema, kBatchSize, {*k, *s}));
    }
    return out;
  }();
  return batches;
}

// Emits prepared batches.
class BatchSource final : public exec::Operator {
 public:
  explicit BatchSource(const arrow::RecordBatchVector& batches)
      : batches_(batches), schema_(batches.front()->schema()) {}
  [[nodiscard]] const std::shared_ptr<arrow::Schema>& output_schema() const override {
    return schema_;
  }
  arrow::Status Open(exec::ExecContext& /*ctx*/) override {
    next_ = 0;
    return arrow::Status::OK();
  }
  arrow::Result<exec::Batch> Next() override {
    if (next_ >= batches_.size()) {
      return exec::Batch{};
    }
    return exec::Batch{.data = batches_[next_++], .selection = {}};
  }
  arrow::Status Close() override { return arrow::Status::OK(); }

 private:
  const arrow::RecordBatchVector& batches_;
  std::shared_ptr<arrow::Schema> schema_;
  std::size_t next_ = 0;
};

// ORDER BY k over kRows rows (without LIMIT: a full sort), and with LIMIT 10 (a top-N).
void SortRows(benchmark::State& state, std::optional<int64_t> limit) {
  const auto& batches = SortInput();
  if (batches.empty()) {
    state.SkipWithError("cannot build the sort input");
    return;
  }
  const std::vector<plan::SortKey> keys = {
      {.column = {.index = 0, .name = "k", .type = plan::LogicalType::kBigInt}}};
  int64_t rows = 0;
  for (auto _ : state) {
    exec::SortOperator sort(std::make_unique<BatchSource>(batches), keys, limit);
    exec::ExecContext ctx;
    auto table = exec::Drain(sort, ctx);
    if (!table.ok()) {
      state.SkipWithError(table.status().ToString());
      return;
    }
    rows = (*table)->num_rows();
    benchmark::DoNotOptimize(rows);
  }
  state.SetItemsProcessed(state.iterations() * kRows);
  state.counters["rows"] = static_cast<double>(rows);
}
void BM_SortRows(benchmark::State& state) { SortRows(state, std::nullopt); }
BENCHMARK(BM_SortRows)->Unit(benchmark::kMillisecond);
void BM_TopNRows(benchmark::State& state) { SortRows(state, 10); }
BENCHMARK(BM_TopNRows)->Unit(benchmark::kMillisecond);

// ---- The join hash table: dense keys (the direct layout) and random keys (hashed) ----

// The key of build row i: dense keys, a bijection of [0, 2^20) (0x9E3779B1 is odd), or random
// 64-bit keys (splitmix64, also a bijection: no key repeats).
int64_t JoinKey(int64_t i, bool dense) {
  if (dense) {
    return static_cast<int64_t>((static_cast<uint64_t>(i) * 0x9E3779B1ULL) % (uint64_t{1} << 20U));
  }
  uint64_t state = static_cast<uint64_t>(i);
  return static_cast<int64_t>(SplitMix64(state));
}

// kRows rows of a BIGINT key and a BIGINT payload in 64Ki-row batches, one per build part (16).
arrow::RecordBatchVector MakeJoinBuildInput(bool dense) {
  arrow::RecordBatchVector out;
  const auto schema =
      arrow::schema({arrow::field("k", arrow::int64()), arrow::field("v", arrow::int64())});
  for (int64_t start = 0; start < kRows; start += kBatchSize) {
    arrow::Int64Builder keys;
    arrow::Int64Builder values;
    if (!keys.Reserve(kBatchSize).ok() || !values.Reserve(kBatchSize).ok()) {
      return {};
    }
    for (int64_t i = start; i < start + kBatchSize; ++i) {
      keys.UnsafeAppend(JoinKey(i, dense));
      values.UnsafeAppend(i);
    }
    auto k = keys.Finish();
    auto v = values.Finish();
    if (!k.ok() || !v.ok()) {
      return {};
    }
    out.push_back(arrow::RecordBatch::Make(schema, kBatchSize, {*k, *v}));
  }
  return out;
}

const arrow::RecordBatchVector& JoinBuildInput(bool dense) {
  static const arrow::RecordBatchVector dense_input = MakeJoinBuildInput(true);
  static const arrow::RecordBatchVector random_input = MakeJoinBuildInput(false);
  return dense ? dense_input : random_input;
}

// kRows probe keys in 64Ki-row arrays: every other one a build key (in another order), the others
// absent from the build.
arrow::ArrayVector MakeJoinProbeInput(bool dense) {
  arrow::ArrayVector out;
  for (int64_t start = 0; start < kRows; start += kBatchSize) {
    arrow::Int64Builder keys;
    if (!keys.Reserve(kBatchSize).ok()) {
      return {};
    }
    for (int64_t j = start; j < start + kBatchSize; ++j) {
      const int64_t absent = dense ? kRows + j : JoinKey(j + (kRows * kRows), false);
      keys.UnsafeAppend(j % 2 == 0 ? JoinKey((j * 7919) % kRows, dense) : absent);
    }
    auto k = keys.Finish();
    if (!k.ok()) {
      return {};
    }
    out.push_back(*k);
  }
  return out;
}

const arrow::ArrayVector& JoinProbeInput(bool dense) {
  static const arrow::ArrayVector dense_input = MakeJoinProbeInput(true);
  static const arrow::ArrayVector random_input = MakeJoinProbeInput(false);
  return dense ? dense_input : random_input;
}

// The table of `batches` on this thread (no executor): one part per batch, added in order.
arrow::Result<std::shared_ptr<const exec::JoinTable>> BuildJoinTable(
    const arrow::RecordBatchVector& batches) {
  ARROW_ASSIGN_OR_RAISE(
      auto spec,
      exec::JoinBuildSpec::Make(
          batches.front()->schema(),
          {plan::BoundColumn{.index = 0, .name = "k", .type = plan::LogicalType::kBigInt}}));
  ARROW_ASSIGN_OR_RAISE(auto builder,
                        exec::JoinTableBuilder::Make(spec, static_cast<int64_t>(batches.size()),
                                                     nullptr, 1, nullptr));
  for (std::size_t p = 0; p < batches.size(); ++p) {
    auto part = std::make_shared<exec::JoinBuildPart>(spec, nullptr);
    ARROW_RETURN_NOT_OK(part->Append(exec::Batch{.data = batches[p], .selection = {}},
                                     arrow::default_memory_pool()));
    ARROW_RETURN_NOT_OK(builder->Add(static_cast<int64_t>(p), std::move(part)));
  }
  ARROW_RETURN_NOT_OK(builder->Merged());
  return builder->Finish();
}

// Building the table of kRows rows: /0 dense keys (direct), /1 random keys (hashed).
void BM_JoinTableBuild(benchmark::State& state) {
  const bool dense = state.range(0) == 0;
  const auto& batches = JoinBuildInput(dense);
  if (batches.empty()) {
    state.SkipWithError("cannot build the join input");
    return;
  }
  bool direct = false;
  for (auto _ : state) {
    auto table = BuildJoinTable(batches);
    if (!table.ok()) {
      state.SkipWithError(table.status().ToString());
      return;
    }
    direct = (*table)->layout() == exec::JoinTable::Layout::kDirect;
    benchmark::DoNotOptimize(table);
  }
  state.SetItemsProcessed(state.iterations() * kRows);
  state.counters["direct"] = direct ? 1 : 0;
}
BENCHMARK(BM_JoinTableBuild)->Arg(0)->Arg(1)->Unit(benchmark::kMillisecond);

// Probing that table with kRows keys, half of them absent: /0 direct, /1 hashed.
void BM_JoinTableProbe(benchmark::State& state) {
  const bool dense = state.range(0) == 0;
  const auto& batches = JoinBuildInput(dense);
  const auto& probes = JoinProbeInput(dense);
  if (batches.empty() || probes.empty()) {
    state.SkipWithError("cannot build the join input");
    return;
  }
  auto table = BuildJoinTable(batches);
  if (!table.ok()) {
    state.SkipWithError(table.status().ToString());
    return;
  }
  std::vector<exec::JoinMatches> out(static_cast<std::size_t>(kBatchSize));
  int64_t matches = 0;
  for (auto _ : state) {
    matches = 0;
    for (const auto& keys : probes) {
      if (const arrow::Status found =
              (*table)->Find({&keys, 1}, nullptr, arrow::default_memory_pool(), out);
          !found.ok()) {
        state.SkipWithError(found.ToString());
        return;
      }
      for (const exec::JoinMatches& match : out) {
        matches += match.end - match.begin;
      }
    }
    benchmark::DoNotOptimize(matches);
  }
  state.SetItemsProcessed(state.iterations() * kRows);
  state.counters["matches"] = static_cast<double>(matches);
  state.counters["direct"] = (*table)->layout() == exec::JoinTable::Layout::kDirect ? 1 : 0;
}
BENCHMARK(BM_JoinTableProbe)->Arg(0)->Arg(1)->Unit(benchmark::kMillisecond);

// ---- Grouping: arrow::compute::Grouper, which GroupTable uses to turn key rows into group ids
// ----
//
// The baseline for the table of docs/adr/0019-own-aggregation-hash-table.md, whose gate (as its
// 2026-10-18 update restates it) is 1.3x this on every shape at 1000, 16 Ki, 256 Ki and 1 Mi
// groups, the band the engine's partitioned tables sit in, with 4 Mi reported. A prototype of that
// table was 1.17-1.30x at 1-4 M groups and 0.22-0.70x at 1000, so the small counts are the ones to
// watch, not only the large: a table that wins only where the groups do not fit in cache is the
// prototype again.
//
// Rows, not groups, are what a benchmark iteration costs, so every shape reads kGroupRows of them
// and the group count is the argument. At 4 M groups there is one row per group; at 1000 about four
// thousand. `groups` reports what the grouper made, which is the argument exactly: the ids come
// from a multiplication by an odd number modulo the count, which visits every group rather than
// colliding as a random id would (a random one reached only 2.65 M of 4 M, and the gate names 4 M).

constexpr int64_t kGroupRows =
    int64_t{4} * 1024 * 1024;  // 64 batches of 64Ki rows, enough for 4 M groups

// The rows a group count needs: kGroupRows, or one row per group once there are more groups than
// that. A table's cost per row is not flat in its size -- which is the point of measuring the curve
// rather than one point on it.
int64_t GroupRowsFor(int64_t groups) {
  return arrow::bit_util::RoundUpToPowerOf2(std::max(kGroupRows, groups), kBatchSize);
}

// A row's group id is row * kGroupStride modulo the group count: every group is visited, since the
// stride is coprime with every count registered here (it is odd and not a multiple of 5, and the
// counts are 1000 and powers of two), and consecutive rows land far apart. Unlike random ids, the
// ids are a permutation: no group repeats until every group has been seen, which a table that
// estimates its size from the rows it has seen must not rely on.
constexpr uint64_t kGroupStride = 0x9E3779B1ULL;

// How rows pick their group: the permutation above, or splitmix64 of the row number modulo the
// count, where groups repeat early as in real data (and some go unseen: about 37% of them when the
// rows equal the groups, so the `groups` counter is then below the argument).
enum class GroupIds : std::uint8_t { kPermutation, kRandom, kClustered };

uint64_t GroupIdOf(int64_t row, int64_t groups, GroupIds ids = GroupIds::kPermutation) {
  if (ids == GroupIds::kClustered) {
    return static_cast<uint64_t>(row) * static_cast<uint64_t>(groups) /
           static_cast<uint64_t>(GroupRowsFor(groups));
  }
  if (ids == GroupIds::kRandom) {
    uint64_t state = static_cast<uint64_t>(row);
    return SplitMix64(state) % static_cast<uint64_t>(groups);
  }
  return (static_cast<uint64_t>(row) * kGroupStride) % static_cast<uint64_t>(groups);
}

// The original four grouping shapes and a three-column key for the column-oriented table.
enum class GroupShape : std::uint8_t {
  kInt64,
  kInt64Int32,
  kString,
  kInt64String,
  kInt64Int32Bool
};

// `rows` rows of `shape`'s key columns, in 64Ki-row batches, with keys over `groups` values; an
// empty vector if they cannot be built. Each row's keys come from one group id, so a shape's
// columns agree on which group a row belongs to and the group count is the id count. `rows` is at
// least `groups` (GroupRowsFor), so with the permutation every group is reached and the count the
// grouper reports is the argument exactly -- which the `groups` counter shows.
arrow::RecordBatchVector GroupInput(GroupShape shape, int64_t groups, int64_t rows,
                                    GroupIds ids = GroupIds::kPermutation) {
  arrow::FieldVector fields;
  switch (shape) {
    case GroupShape::kInt64:
      fields = {arrow::field("k", arrow::int64())};
      break;
    case GroupShape::kInt64Int32:
      fields = {arrow::field("k", arrow::int64()), arrow::field("j", arrow::int32())};
      break;
    case GroupShape::kInt64Int32Bool:
      fields = {arrow::field("k", arrow::int64()), arrow::field("j", arrow::int32()),
                arrow::field("b", arrow::boolean())};
      break;
    case GroupShape::kString:
      fields = {arrow::field("s", arrow::binary())};
      break;
    case GroupShape::kInt64String:
      fields = {arrow::field("k", arrow::int64()), arrow::field("s", arrow::binary())};
      break;
  }
  const auto schema = arrow::schema(fields);
  const bool has_int64 = shape != GroupShape::kString;
  const bool has_bool = shape == GroupShape::kInt64Int32Bool;
  const bool has_int32 = shape == GroupShape::kInt64Int32 || has_bool;
  const bool has_string = shape == GroupShape::kString || shape == GroupShape::kInt64String;
  arrow::RecordBatchVector out;
  for (int64_t start = 0; start < rows; start += kBatchSize) {
    arrow::Int64Builder keys;
    arrow::Int32Builder seconds;
    arrow::BooleanBuilder flags;
    arrow::BinaryBuilder strings;
    for (int64_t row = start; row < start + kBatchSize; ++row) {
      const uint64_t id = GroupIdOf(row, groups, ids);
      // The three-column shape stores the quotient and the two remainders separately.
      // Even 1000 groups need all three columns; one in three rows has a NULL flag.
      if (has_bool) {
        const uint64_t flag = id % 3U;
        if (!keys.Append(static_cast<int64_t>(id / (32U * 3U))).ok() ||
            !seconds.Append(static_cast<int32_t>((id / 3U) % 32U)).ok() ||
            !(flag == 2 ? flags.AppendNull() : flags.Append(flag == 1)).ok()) {
          return {};
        }
        continue;
      }
      // A two-key shape splits the id between its columns (id / 1024, id % 1024), so that from 1024
      // groups on neither column alone separates the groups and the grouper has to read both.
      if (has_int64 && !keys.Append(static_cast<int64_t>(has_int32 ? id / 1024U : id)).ok()) {
        return {};
      }
      if (has_int32 && !seconds.Append(static_cast<int32_t>(id % 1024U)).ok()) {
        return {};
      }
      // About twenty bytes, as a URL or a search phrase is longer than the eight a word compares.
      if (has_string && !strings.Append("key-" + std::to_string(id) + "-pad______").ok()) {
        return {};
      }
    }
    arrow::ArrayVector columns;
    const auto take = [&columns](auto& builder) {
      auto array = builder.Finish();
      if (array.ok()) {
        columns.push_back(*array);
      }
      return array.ok();
    };
    if ((has_int64 && !take(keys)) || (has_int32 && !take(seconds)) || (has_bool && !take(flags)) ||
        (has_string && !take(strings))) {
      return {};
    }
    out.push_back(arrow::RecordBatch::Make(schema, kBatchSize, std::move(columns)));
  }
  return out;
}

// One grouper over every batch, as GroupTable::Consume does per part; with `budgeted`, its buffers
// come from a MemoryBudget without a limit, as the engine's do (engine::Session's pool).
void GroupKeys(benchmark::State& state, GroupShape shape, bool budgeted = false,
               GroupIds ids_mode = GroupIds::kPermutation) {
  const int64_t groups = state.range(0);
  const int64_t rows = GroupRowsFor(groups);
  const arrow::RecordBatchVector batches = GroupInput(shape, groups, rows, ids_mode);
  if (batches.empty()) {
    state.SkipWithError("cannot build the grouping input");
    return;
  }
  std::vector<arrow::TypeHolder> types;
  types.reserve(static_cast<std::size_t>(batches.front()->num_columns()));
  for (const auto& field : batches.front()->schema()->fields()) {
    types.emplace_back(field->type());
  }
  exec::MemoryBudget budget(std::nullopt);
  arrow::compute::ExecContext budgeted_ctx(&budget);
  uint32_t made = 0;
  for (auto _ : state) {
    auto grouper = budgeted ? arrow::compute::Grouper::Make(types, &budgeted_ctx)
                            : arrow::compute::Grouper::Make(types);
    if (!grouper.ok()) {
      state.SkipWithError(grouper.status().ToString());
      return;
    }
    for (const auto& batch : batches) {
      const arrow::compute::ExecBatch keys(*batch);
      auto ids = (*grouper)->Consume(arrow::compute::ExecSpan(keys));
      if (!ids.ok()) {
        state.SkipWithError(ids.status().ToString());
        return;
      }
      benchmark::DoNotOptimize(ids->length());
    }
    made = (*grouper)->num_groups();
    benchmark::DoNotOptimize(made);
  }
  state.SetItemsProcessed(state.iterations() * rows);
  state.counters["groups"] = static_cast<double>(made);
  state.counters["rows"] = static_cast<double>(rows);
  // What the curve is about: the cost of one row at this table size.
  state.counters["ns_per_row"] =
      benchmark::Counter(static_cast<double>(rows), benchmark::Counter::kIsIterationInvariantRate |
                                                        benchmark::Counter::kInvert);
}

void BM_GroupInt64(benchmark::State& state) { GroupKeys(state, GroupShape::kInt64); }
void BM_GroupInt64Int32(benchmark::State& state) { GroupKeys(state, GroupShape::kInt64Int32); }
void BM_GroupString(benchmark::State& state) { GroupKeys(state, GroupShape::kString); }
void BM_GroupInt64String(benchmark::State& state) { GroupKeys(state, GroupShape::kInt64String); }
void BM_GroupInt64Budget(benchmark::State& state) { GroupKeys(state, GroupShape::kInt64, true); }
void BM_GroupInt64Int32Budget(benchmark::State& state) {
  GroupKeys(state, GroupShape::kInt64Int32, true);
}
void BM_GroupInt64BudgetRandom(benchmark::State& state) {
  GroupKeys(state, GroupShape::kInt64, true, GroupIds::kRandom);
}
void BM_GroupInt64Int32BudgetRandom(benchmark::State& state) {
  GroupKeys(state, GroupShape::kInt64Int32, true, GroupIds::kRandom);
}

// ---- The engine's key table of fixed-width keys (exec::FixedKeyTable, step 1) ----
//
// Each table runs as GroupTable runs it: made by exec::MakeKeyTable on an ExecContext over a
// MemoryBudget, one Consume per batch, each call's ids in a buffer of its own. The inputs and
// counters are BM_Group*'s and the budget BM_Group*Budget's, so ns_per_row compares directly with
// the budgeted grouper of the same run and ids: ADR 0019's micro gate for a shape is 1.3x at every
// count from 1000 to 1 Mi groups. Each benchmark name is checked once first: dense ids in
// first-seen order, one group per key, each group's uniques row the key of its rows, and the size
// estimate failing small on memory.

std::vector<arrow::TypeHolder> KeyTypesOf(const arrow::RecordBatch& batch) {
  std::vector<arrow::TypeHolder> types;
  types.reserve(static_cast<std::size_t>(batch.num_columns()));
  for (const auto& field : batch.schema()->fields()) {
    types.emplace_back(field->type());
  }
  return types;
}

// The full key of the benchmark row, including the third column's validity bit.
std::tuple<int64_t, int32_t, std::optional<bool>> KeyAt(const arrow::compute::ExecBatch& keys,
                                                        int64_t row) {
  const int64_t k = keys.values[0].array()->GetValues<int64_t>(1)[row];
  const int32_t j = keys.values.size() > 1 ? keys.values[1].array()->GetValues<int32_t>(1)[row] : 0;
  std::optional<bool> flag;
  if (keys.values.size() == 3) {
    const arrow::ArrayData& data = *keys.values[2].array();
    if (data.IsValid(row)) {
      flag = arrow::bit_util::GetBit(data.buffers[1]->data(),
                                     static_cast<uint64_t>(data.offset + row));
    }
  }
  return {k, j, flag};
}

// The groups of the first `count` batches through a table of `kind` on `budget`, or an error.
arrow::Result<uint32_t> GroupsOfFirst(const arrow::RecordBatchVector& batches, std::size_t count,
                                      exec::MemoryBudget& budget,
                                      const exec::KeyTableOptions& options) {
  arrow::compute::ExecContext ctx(&budget);
  ARROW_ASSIGN_OR_RAISE(auto table,
                        exec::MakeKeyTable(KeyTypesOf(*batches.front()), &ctx, options));
  for (std::size_t b = 0; b < count; ++b) {
    ARROW_RETURN_NOT_OK(table->Consume(arrow::compute::ExecBatch(*batches[b])).status());
  }
  return table->num_groups();
}

// One table of `kind` over every batch: an empty string, or what is wrong with its ids.
std::string KeyTableCheck(const arrow::RecordBatchVector& batches, int64_t groups,
                          GroupIds ids_mode, exec::KeyTableKind kind) {
  exec::MemoryBudget budget(std::nullopt);
  arrow::compute::ExecContext ctx(&budget);
  const auto types = KeyTypesOf(*batches.front());
  auto made = exec::MakeKeyTable(types, &ctx, {.kind = kind});
  if (!made.ok()) {
    return made.status().ToString();
  }
  exec::KeyTable& table = **made;
  if (kind != exec::KeyTableKind::kGrouper) {
    const auto oracle = exec::MakeKeyTable(types, &ctx, {.kind = exec::KeyTableKind::kGrouper});
    if (!oracle.ok()) {
      return oracle.status().ToString();
    }
    const exec::KeyTable& grouper = **oracle;
    if (typeid(table) == typeid(grouper)) {
      return "fixed-key benchmark selected the Arrow fallback";
    }
  }
  // Check both directions of the mapping. The fixed tables also promise first-seen ID order.
  std::vector<int64_t> ours(static_cast<std::size_t>(groups), -1);
  std::vector<int64_t> theirs(static_cast<std::size_t>(groups), -1);
  int64_t row = 0;
  std::uint32_t next = 0;
  for (const auto& batch : batches) {
    const auto ids = table.Consume(arrow::compute::ExecBatch(*batch));
    if (!ids.ok()) {
      return ids.status().ToString();
    }
    for (int64_t i = 0; i < batch->num_rows(); ++i, ++row) {
      const auto truth = static_cast<std::size_t>(GroupIdOf(row, groups, ids_mode));
      const std::uint32_t id = ids->ids[static_cast<std::size_t>(i)];
      if (id >= ids->after || id >= static_cast<std::uint64_t>(groups)) {
        return "id " + std::to_string(id) + " past the group count";
      }
      int64_t& mine = ours[truth];
      int64_t& back = theirs[id];
      if (mine == -1 && back == -1) {
        if (kind != exec::KeyTableKind::kGrouper && id != next) {
          return "row " + std::to_string(row) + " starts a group with id " + std::to_string(id) +
                 " where the next id is " + std::to_string(next);
        }
        ++next;
        mine = id;
        back = static_cast<int64_t>(truth);
      } else if (mine != id || back != static_cast<int64_t>(truth)) {
        return "row " + std::to_string(row) + " has id " + std::to_string(id) +
               " but its group has " + std::to_string(mine);
      }
    }
  }
  // Every group's uniques row is the key of the rows given its id.
  const auto snapshot = table.Uniques();
  if (!snapshot.ok()) {
    return snapshot.status().ToString();
  }
  const auto& uniques = snapshot->batch;
  if (uniques.length != table.num_groups() ||
      uniques.values.size() != static_cast<std::size_t>(batches.front()->num_columns())) {
    return "unique keys have the wrong shape";
  }
  for (std::size_t c = 0; c < types.size(); ++c) {
    const auto& column = *uniques.values[c].array();
    if (!column.type->Equals(*types[c].type) || (c < 2 && column.GetNullCount() != 0)) {
      return "wrong type or unexpected NULLs in unique keys";
    }
  }
  row = 0;
  for (const auto& batch : batches) {
    const arrow::compute::ExecBatch keys(*batch);
    for (int64_t i = 0; i < batch->num_rows(); ++i, ++row) {
      const int64_t id = ours[static_cast<std::size_t>(GroupIdOf(row, groups, ids_mode))];
      if (KeyAt(uniques, id) != KeyAt(keys, i)) {
        return "group " + std::to_string(id) + " keeps a key other than its rows'";
      }
    }
  }
  const auto seen = std::ranges::count_if(ours, [](int64_t id) { return id != -1; });
  if (static_cast<int64_t>(table.num_groups()) != seen) {
    return std::to_string(table.num_groups()) + " groups for " + std::to_string(seen);
  }
  if (budget.bytes_allocated() <= 0) {
    return "nothing was charged to the budget";
  }
  // The estimate fails small on memory too: under a budget that holds only what the first two
  // batches need without an estimate, its reservation is refused and the table carries on with the
  // same groups.
  if (batches.size() >= 2) {
    exec::MemoryBudget open(std::nullopt);
    const auto grown = GroupsOfFirst(batches, 2, open, {.kind = kind, .estimate = false});
    exec::MemoryBudget tight(open.max_memory() + (open.max_memory() / 8));
    const auto sized = GroupsOfFirst(batches, 2, tight, {.kind = kind});
    if (!grown.ok() || !sized.ok()) {
      return "under a tight budget the estimate failed the table: " +
             (grown.ok() ? sized.status() : grown.status()).ToString();
    }
    if (*sized != *grown) {
      return "under a tight budget: " + std::to_string(*sized) + " groups for " +
             std::to_string(*grown);
    }
  }
  return {};
}

void KeyTableKeys(benchmark::State& state, GroupShape shape, GroupIds ids_mode,
                  exec::KeyTableKind kind) {
  const int64_t groups = state.range(0);
  const int64_t rows = GroupRowsFor(groups);
  const arrow::RecordBatchVector batches = GroupInput(shape, groups, rows, ids_mode);
  if (batches.empty()) {
    state.SkipWithError("cannot build the grouping input");
    return;
  }
  // Google Benchmark calls this once per run, not once per name.
  static std::vector<std::string> checked;
  if (std::ranges::find(checked, state.name()) == checked.end()) {
    if (const std::string wrong = KeyTableCheck(batches, groups, ids_mode, kind); !wrong.empty()) {
      state.SkipWithError("wrong ids: " + wrong);
      return;
    }
    checked.push_back(state.name());
  }
  const std::vector<arrow::TypeHolder> types = KeyTypesOf(*batches.front());
  exec::MemoryBudget budget(std::nullopt);
  arrow::compute::ExecContext ctx(&budget);
  uint32_t made = 0;
  for (auto _ : state) {
    auto table = exec::MakeKeyTable(types, &ctx, {.kind = kind});
    if (!table.ok()) {
      state.SkipWithError(table.status().ToString());
      return;
    }
    for (const auto& batch : batches) {
      const arrow::compute::ExecBatch keys(*batch);
      const auto ids = (*table)->Consume(keys);
      if (!ids.ok()) {
        state.SkipWithError(ids.status().ToString());
        return;
      }
      benchmark::DoNotOptimize(ids->ids.data());
    }
    made = (*table)->num_groups();
    benchmark::DoNotOptimize(made);
  }
  state.SetItemsProcessed(state.iterations() * rows);
  state.counters["pool_peak_bytes"] = static_cast<double>(budget.max_memory());
  state.counters["groups"] = static_cast<double>(made);
  state.counters["rows"] = static_cast<double>(rows);
  state.counters["ns_per_row"] =
      benchmark::Counter(static_cast<double>(rows), benchmark::Counter::kIsIterationInvariantRate |
                                                        benchmark::Counter::kInvert);
}

// BM_KeyTable_<shape>_<growth>[_Random]: exec::FixedKeyTable for one BIGINT key or a BIGINT and an
// INTEGER, growing in slot order (the engine's, KeyTableKind::kFixed) or in id order
// (kFixedIdOrder, which only tests run), sized by its estimate after its second batch. Random: ids
// that repeat at random (GroupIds::kRandom), compared with BM_Group<shape>BudgetRandom; the others
// use the permutation, where no id repeats early and the estimate must leave the table alone.
#define ANTB1_KEY_TABLE(name, shape, kind, ids)                                      \
  void BM_KeyTable_##name(benchmark::State& state) {                                 \
    KeyTableKeys(state, GroupShape::shape, GroupIds::ids, exec::KeyTableKind::kind); \
  }
ANTB1_KEY_TABLE(Int64_SlotOrder, kInt64, kFixed, kPermutation)
ANTB1_KEY_TABLE(Int64_IdOrder, kInt64, kFixedIdOrder, kPermutation)
ANTB1_KEY_TABLE(Int64_SlotOrder_Random, kInt64, kFixed, kRandom)
ANTB1_KEY_TABLE(Int64_IdOrder_Random, kInt64, kFixedIdOrder, kRandom)
ANTB1_KEY_TABLE(Int64Int32_SlotOrder, kInt64Int32, kFixed, kPermutation)
ANTB1_KEY_TABLE(Int64Int32_IdOrder, kInt64Int32, kFixedIdOrder, kPermutation)
ANTB1_KEY_TABLE(Int64Int32_SlotOrder_Random, kInt64Int32, kFixed, kRandom)
ANTB1_KEY_TABLE(Int64Int32_IdOrder_Random, kInt64Int32, kFixedIdOrder, kRandom)
// Both three-column cases include the same KeyTable adapter and budget in their timing.
ANTB1_KEY_TABLE(Int64Int32Bool_Grouper, kInt64Int32Bool, kGrouper, kPermutation)
ANTB1_KEY_TABLE(Int64Int32Bool_Column, kInt64Int32Bool, kFixed, kPermutation)
ANTB1_KEY_TABLE(Int64Int32Bool_Grouper_Random, kInt64Int32Bool, kGrouper, kRandom)
ANTB1_KEY_TABLE(Int64Int32Bool_Column_Random, kInt64Int32Bool, kFixed, kRandom)
#undef ANTB1_KEY_TABLE

// The gate's counts (1000 to 1 Mi: cached to past cache) with 4 Mi (about one row a group) and 16
// Mi above them, so the shape of the cost per row is visible rather than inferred.
#define ANTB1_GROUP_COUNTS   \
  Arg(1000)                  \
      ->Arg(16 * 1024)       \
      ->Arg(256 * 1024)      \
      ->Arg(1024 * 1024)     \
      ->Arg(4 * 1024 * 1024) \
      ->Arg(16 * 1024 * 1024)
BENCHMARK(BM_GroupInt64)->ANTB1_GROUP_COUNTS->Unit(benchmark::kMillisecond);
BENCHMARK(BM_GroupInt64Int32)->ANTB1_GROUP_COUNTS->Unit(benchmark::kMillisecond);
BENCHMARK(BM_GroupString)->ANTB1_GROUP_COUNTS->Unit(benchmark::kMillisecond);
BENCHMARK(BM_GroupInt64String)->ANTB1_GROUP_COUNTS->Unit(benchmark::kMillisecond);
#undef ANTB1_GROUP_COUNTS

// The key tables' counts: the gate's 1000 to 1 Mi, and 4 Mi reported (docs/adr/0019's band update).
#define ANTB1_KEY_TABLE_COUNTS \
  Arg(1000)->Arg(16 * 1024)->Arg(256 * 1024)->Arg(1024 * 1024)->Arg(4 * 1024 * 1024)
BENCHMARK(BM_GroupInt64Budget)->ANTB1_KEY_TABLE_COUNTS->Unit(benchmark::kMillisecond);
BENCHMARK(BM_GroupInt64Int32Budget)->ANTB1_KEY_TABLE_COUNTS->Unit(benchmark::kMillisecond);
BENCHMARK(BM_GroupInt64BudgetRandom)->ANTB1_KEY_TABLE_COUNTS->Unit(benchmark::kMillisecond);
BENCHMARK(BM_GroupInt64Int32BudgetRandom)->ANTB1_KEY_TABLE_COUNTS->Unit(benchmark::kMillisecond);
BENCHMARK(BM_KeyTable_Int64_SlotOrder)->ANTB1_KEY_TABLE_COUNTS->Unit(benchmark::kMillisecond);
BENCHMARK(BM_KeyTable_Int64_IdOrder)->ANTB1_KEY_TABLE_COUNTS->Unit(benchmark::kMillisecond);
BENCHMARK(BM_KeyTable_Int64_SlotOrder_Random)
    ->ANTB1_KEY_TABLE_COUNTS->Unit(benchmark::kMillisecond);
BENCHMARK(BM_KeyTable_Int64_IdOrder_Random)->ANTB1_KEY_TABLE_COUNTS->Unit(benchmark::kMillisecond);
BENCHMARK(BM_KeyTable_Int64Int32_SlotOrder)->ANTB1_KEY_TABLE_COUNTS->Unit(benchmark::kMillisecond);
BENCHMARK(BM_KeyTable_Int64Int32_IdOrder)->ANTB1_KEY_TABLE_COUNTS->Unit(benchmark::kMillisecond);
BENCHMARK(BM_KeyTable_Int64Int32_SlotOrder_Random)
    ->ANTB1_KEY_TABLE_COUNTS->Unit(benchmark::kMillisecond);
BENCHMARK(BM_KeyTable_Int64Int32_IdOrder_Random)
    ->ANTB1_KEY_TABLE_COUNTS->Unit(benchmark::kMillisecond);
BENCHMARK(BM_KeyTable_Int64Int32Bool_Grouper)
    ->ANTB1_KEY_TABLE_COUNTS->Unit(benchmark::kMillisecond);
BENCHMARK(BM_KeyTable_Int64Int32Bool_Column)->ANTB1_KEY_TABLE_COUNTS->Unit(benchmark::kMillisecond);
BENCHMARK(BM_KeyTable_Int64Int32Bool_Grouper_Random)
    ->ANTB1_KEY_TABLE_COUNTS->Unit(benchmark::kMillisecond);
BENCHMARK(BM_KeyTable_Int64Int32Bool_Column_Random)
    ->ANTB1_KEY_TABLE_COUNTS->Unit(benchmark::kMillisecond);
#undef ANTB1_KEY_TABLE_COUNTS

// ADR 0019 step 3. Feed the same four-batch parts into each GroupTable path and merge their
// partitions in part order. Measure building the local tables separately from the complete
// handoff (partitioning, all destination merges and releasing the tables).
struct ProgramInput {
  arrow::RecordBatchVector batches;
  std::vector<plan::BoundColumn> keys;
  std::vector<plan::AggregateCall> calls;
  std::shared_ptr<arrow::Schema> output;
};

std::optional<int64_t> ProgramValue(int64_t row) {
  if (row % 11 == 0) {
    return std::nullopt;
  }
  return (row % 127) - 63;
}

arrow::Result<ProgramInput> MakeProgramInput(GroupShape shape, int64_t groups, GroupIds ids) {
  ProgramInput in;
  in.batches = GroupInput(shape, groups, GroupRowsFor(groups), ids);
  if (in.batches.empty()) {
    return arrow::Status::OutOfMemory("cannot build aggregate benchmark keys");
  }
  auto fields = in.batches.front()->schema()->fields();
  const std::array<plan::LogicalType, 3> types = {
      plan::LogicalType::kBigInt, plan::LogicalType::kInteger, plan::LogicalType::kBoolean};
  for (std::size_t i = 0; i < fields.size(); ++i) {
    in.keys.push_back({.index = Narrow<int>(i), .name = fields[i]->name(), .type = types.at(i)});
  }
  const int value_index = Narrow<int>(fields.size());
  const plan::BoundColumn v{.index = value_index, .name = "v", .type = plan::LogicalType::kBigInt};
  in.calls = {{.kind = plan::AggKind::kCountStar, .type = plan::LogicalType::kBigInt},
              {.kind = plan::AggKind::kSum, .arg = v, .type = plan::LogicalType::kHugeInt},
              {.kind = plan::AggKind::kMax, .arg = v, .type = plan::LogicalType::kBigInt}};
  fields.push_back(arrow::field("count", arrow::int64()));
  fields.push_back(arrow::field("sum", plan::ToArrow(plan::LogicalType::kHugeInt)));
  fields.push_back(arrow::field("max", arrow::int64()));
  in.output = arrow::schema(std::move(fields));
  int64_t row = 0;
  for (auto& batch : in.batches) {
    arrow::Int64Builder values;
    ARROW_RETURN_NOT_OK(values.Reserve(batch->num_rows()));
    for (int64_t i = 0; i < batch->num_rows(); ++i, ++row) {
      const auto value = ProgramValue(row);
      if (value.has_value()) {
        values.UnsafeAppend(*value);
      } else {
        values.UnsafeAppendNull();
      }
    }
    ARROW_ASSIGN_OR_RAISE(auto data, values.Finish());
    ARROW_ASSIGN_OR_RAISE(batch,
                          batch->AddColumn(value_index, arrow::field("v", arrow::int64()), data));
  }
  return in;
}

struct ExpectedGroup {
  int64_t count = 0;
  int64_t sum = 0;
  int64_t maximum = std::numeric_limits<int64_t>::min();
};

// Check output including each component of the key and NULL-only aggregate groups. Clearing a
// matched count detects duplicates across lanes and leaves any missing group visible at the end.
arrow::Status DrainProgramLane(exec::GroupTable& lane, const ProgramInput& in,
                               std::vector<ExpectedGroup>* expected) {
  ARROW_RETURN_NOT_OK(lane.CheckFinalize());
  const auto n = Narrow<int>(in.keys.size());
  while (true) {
    ARROW_ASSIGN_OR_RAISE(auto chunk, lane.NextChunk(in.output));
    if (chunk == nullptr) {
      return arrow::Status::OK();
    }
    if (expected == nullptr) {
      benchmark::DoNotOptimize(chunk);
      continue;
    }
    ARROW_RETURN_NOT_OK(chunk->ValidateFull());
    arrow::compute::ExecBatch keys(*chunk);
    keys.values.resize(in.keys.size());
    const auto& counts = static_cast<const arrow::Int64Array&>(*chunk->column(n));
    const auto& sums = static_cast<const arrow::Decimal128Array&>(*chunk->column(n + 1));
    const auto& maxima = static_cast<const arrow::Int64Array&>(*chunk->column(n + 2));
    for (int64_t i = 0; i < chunk->num_rows(); ++i) {
      const auto [k, j, b] = KeyAt(keys, i);
      if (chunk->column(0)->IsNull(i) || k < 0 || static_cast<uint64_t>(k) >= expected->size() ||
          (n > 1 && (chunk->column(1)->IsNull(i) || j < 0 || j >= (n == 3 ? 32 : 1024)))) {
        return arrow::Status::Invalid("unexpected aggregate benchmark key");
      }
      int64_t id = k;
      if (n == 3) {
        id = (k * 32 + j) * 3 + (b.has_value() ? static_cast<int>(*b) : 2);
      } else if (n == 2) {
        id = k * 1024 + j;
      }
      if (static_cast<uint64_t>(id) >= expected->size()) {
        return arrow::Status::Invalid("aggregate benchmark key outside generated range");
      }
      auto& group = expected->at(static_cast<std::size_t>(id));
      const bool empty = group.maximum == std::numeric_limits<int64_t>::min();
      if (group.count == 0 || counts.IsNull(i) || counts.Value(i) != group.count ||
          sums.IsNull(i) != empty || maxima.IsNull(i) != empty ||
          (!empty && (arrow::Decimal128(sums.GetValue(i)) != arrow::Decimal128(group.sum) ||
                      maxima.Value(i) != group.maximum))) {
        return arrow::Status::Invalid("wrong aggregate result at generated key ", id);
      }
      group.count = 0;
    }
  }
}

struct ProgramTimes {
  double local_ms = 0;
  double merge_ms = 0;
  double lane_merge_ms = 0;
  double output_ms = 0;
  double local_cpu_ms = 0;
  double merge_cpu_ms = 0;
  double lane_merge_cpu_ms = 0;
  double output_cpu_ms = 0;
  uint64_t partial_groups = 0;
  uint64_t groups = 0;
};

struct ProgramArm {
  const char* name;
  exec::KeyTableKind kind;
  exec::GroupTableMode mode;
  bool prefetch_merge = false;
};

arrow::Result<ProgramTimes> RunGroupProgram(const ProgramInput& in, ProgramArm arm,
                                            std::size_t lanes, exec::MemoryBudget& budget,
                                            std::vector<ExpectedGroup>* expected = nullptr) {
  using Clock = std::chrono::steady_clock;
  using Millis = std::chrono::duration<double, std::milli>;
  const bool verify_merge =
      expected != nullptr && in.keys.size() == 3 && arm.mode == exec::GroupTableMode::kBatch;
  bool observed_prefetch = !arm.prefetch_merge;
  const auto make = [&](bool destination) -> arrow::Result<std::unique_ptr<exec::GroupTable>> {
    exec::KeyTableOptions options{
        .kind = arm.kind,
        .estimate = !destination,
        .prefetch_composite_merge = destination && arm.prefetch_merge,
        .prefetched_composite_merge = destination && verify_merge ? &observed_prefetch : nullptr};
    ARROW_ASSIGN_OR_RAISE(auto table, exec::GroupTable::Make(
                                          in.keys, in.calls, in.batches.front()->num_columns(),
                                          &budget, &budget, destination, lanes, options, arm.mode));
    if (table->uses_batch_program() != (arm.mode == exec::GroupTableMode::kBatch)) {
      return arrow::Status::Invalid("aggregate benchmark selected an unexpected program");
    }
    return table;
  };
  const auto start = Clock::now();
  const auto cpu_start = std::clock();
  std::vector<std::unique_ptr<exec::GroupTable>> parts;
  constexpr std::size_t kPartBatches = 4;
  for (std::size_t i = 0; i < in.batches.size(); ++i) {
    if (i % kPartBatches == 0) {
      ARROW_ASSIGN_OR_RAISE(auto table, make(false));
      parts.push_back(std::move(table));
    }
    ARROW_RETURN_NOT_OK(parts.back()->Consume(*in.batches[i]));
  }
  const auto local_done = Clock::now();
  const auto cpu_local_done = std::clock();
  uint64_t partial_groups = 0;
  for (auto& part : parts) {
    partial_groups += part->num_groups();
    ARROW_RETURN_NOT_OK(part->Partition());
  }
  uint64_t groups = 0;
  Clock::duration lane_merge_wall{};
  std::clock_t lane_merge_cpu = 0;
  Clock::duration output_wall{};
  std::clock_t output_cpu = 0;
  for (std::size_t lane = 0; lane < lanes; ++lane) {
    ARROW_ASSIGN_OR_RAISE(auto dest, make(true));
    // Time all incoming parts for this destination with one pair of clock reads per clock.
    const auto lane_start = Clock::now();
    const auto cpu_lane_start = std::clock();
    for (const auto& part : parts) {
      ARROW_RETURN_NOT_OK(dest->MergePartition(*part, lane));
    }
    const auto output_start = Clock::now();
    const auto cpu_output_start = std::clock();
    lane_merge_wall += output_start - lane_start;
    lane_merge_cpu += cpu_output_start - cpu_lane_start;
    groups += dest->num_groups();
    ARROW_RETURN_NOT_OK(DrainProgramLane(*dest, in, expected));
    output_cpu += std::clock() - cpu_output_start;
    output_wall += Clock::now() - output_start;
  }
  parts.clear();
  if (verify_merge && observed_prefetch != arm.prefetch_merge) {
    return arrow::Status::Invalid("aggregate benchmark did not observe its composite merge");
  }
  return ProgramTimes{
      .local_ms = Millis(local_done - start).count(),
      .merge_ms = Millis(Clock::now() - local_done - output_wall).count(),
      .lane_merge_ms = Millis(lane_merge_wall).count(),
      .output_ms = Millis(output_wall).count(),
      .local_cpu_ms = 1000.0 * static_cast<double>(cpu_local_done - cpu_start) / CLOCKS_PER_SEC,
      .merge_cpu_ms =
          1000.0 * static_cast<double>(std::clock() - cpu_local_done - output_cpu) / CLOCKS_PER_SEC,
      .lane_merge_cpu_ms = 1000.0 * static_cast<double>(lane_merge_cpu) / CLOCKS_PER_SEC,
      .output_cpu_ms = 1000.0 * static_cast<double>(output_cpu) / CLOCKS_PER_SEC,
      .partial_groups = partial_groups,
      .groups = groups};
}

void GroupProgram(benchmark::State& state, GroupShape shape, ProgramArm arm, GroupIds ids,
                  std::size_t lanes) {
  const int64_t groups = state.range(0);
  const int64_t rows = GroupRowsFor(groups);
  const auto in = MakeProgramInput(shape, groups, ids);
  if (!in.ok()) {
    state.SkipWithError(in.status().ToString());
    return;
  }
  // Validate each arm and shape once outside the timed iterations.
  static std::vector<std::string> checked;
  if (std::ranges::find(checked, state.name()) == checked.end()) {
    std::vector<ExpectedGroup> expected(Narrow<std::size_t>(groups));
    for (int64_t row = 0; row < rows; ++row) {
      auto& group = expected[GroupIdOf(row, groups, ids)];
      ++group.count;
      if (const auto value = ProgramValue(row); value.has_value()) {
        group.sum += *value;
        group.maximum = std::max(group.maximum, *value);
      }
    }
    exec::MemoryBudget validation(std::nullopt);
    const auto result = RunGroupProgram(*in, arm, lanes, validation, &expected);
    if (!result.ok() || validation.bytes_allocated() != 0 ||
        std::ranges::any_of(expected, [](const auto& group) { return group.count != 0; })) {
      state.SkipWithError(result.ok() ? "unmatched group or unreleased memory"
                                      : result.status().ToString());
      return;
    }
    checked.push_back(state.name());
  }
  exec::MemoryBudget budget(std::nullopt);
  ProgramTimes times;
  for (auto _ : state) {
    const auto result = RunGroupProgram(*in, arm, lanes, budget);
    if (!result.ok()) {
      state.SkipWithError(result.status().ToString());
      return;
    }
    times.local_ms += result->local_ms;
    times.merge_ms += result->merge_ms;
    times.lane_merge_ms += result->lane_merge_ms;
    times.output_ms += result->output_ms;
    times.local_cpu_ms += result->local_cpu_ms;
    times.merge_cpu_ms += result->merge_cpu_ms;
    times.lane_merge_cpu_ms += result->lane_merge_cpu_ms;
    times.output_cpu_ms += result->output_cpu_ms;
    times.partial_groups = result->partial_groups;
    times.groups = result->groups;
    benchmark::DoNotOptimize(times.groups);
  }
  state.counters["local_ms"] = {times.local_ms, benchmark::Counter::kAvgIterations};
  state.counters["merge_ms"] = {times.merge_ms, benchmark::Counter::kAvgIterations};
  state.counters["lane_merge_ms"] = {times.lane_merge_ms, benchmark::Counter::kAvgIterations};
  state.counters["output_ms"] = {times.output_ms, benchmark::Counter::kAvgIterations};
  state.counters["local_cpu_ms"] = {times.local_cpu_ms, benchmark::Counter::kAvgIterations};
  state.counters["merge_cpu_ms"] = {times.merge_cpu_ms, benchmark::Counter::kAvgIterations};
  state.counters["lane_merge_cpu_ms"] = {times.lane_merge_cpu_ms,
                                         benchmark::Counter::kAvgIterations};
  state.counters["output_cpu_ms"] = {times.output_cpu_ms, benchmark::Counter::kAvgIterations};
  state.counters["pool_peak_bytes"] = static_cast<double>(budget.max_memory());
  state.counters["partial_groups"] = static_cast<double>(times.partial_groups);
  state.counters["groups"] = static_cast<double>(times.groups);
  state.SetItemsProcessed(state.iterations() * rows);
}

void RegisterGroupPrograms() {
  using enum exec::KeyTableKind;
  using enum exec::GroupTableMode;
  const auto add = [](GroupShape shape, const std::string& name, GroupIds ids, std::size_t lanes,
                      std::initializer_list<int64_t> counts) {
    const auto add_arm = [&](ProgramArm arm) {
      auto* bench = benchmark::RegisterBenchmark(
          ("BM_GroupProgram/" + name + "/" + arm.name).c_str(),
          [=](benchmark::State& state) { GroupProgram(state, shape, arm, ids, lanes); });
      bench->Unit(benchmark::kMillisecond);
      for (int64_t groups : counts) {
        bench->Arg(groups);
      }
    };
    add_arm({"Grouper", kGrouper, kSeparate});
    add_arm({"Fixed", kFixed, kSeparate});
    add_arm({"Batch", kFixed, kBatch});
    if (shape == GroupShape::kInt64Int32Bool) {
      add_arm({"DensePrefetch", kFixed, kBatch, true});
    }
  };
  const std::initializer_list<int64_t> counts = {1000, 16 * 1024, 256 * 1024, 1024 * 1024,
                                                 4 * 1024 * 1024};
  constexpr auto kLanes = exec::GroupTable::kPartitions;
  add(GroupShape::kInt64, "Int64", GroupIds::kRandom, kLanes, counts);
  add(GroupShape::kInt64Int32, "Int64Int32", GroupIds::kRandom, kLanes, counts);
  add(GroupShape::kInt64Int32Bool, "Int64Int32Bool", GroupIds::kRandom, kLanes, counts);
  add(GroupShape::kInt64, "Int64Clustered", GroupIds::kClustered, kLanes, {256 * 1024});
  add(GroupShape::kInt64Int32Bool, "Int64Int32BoolClustered", GroupIds::kClustered, kLanes,
      {256 * 1024});
  // A single destination models one large global lane without building the 31 other lanes.
  if (std::getenv("ANTB1_BENCH_LARGE_GROUPS") != nullptr) {
    add(GroupShape::kInt64, "Int64LargeLane", GroupIds::kPermutation, 1, {47 * 1024 * 1024});
  }
}

// Compare the production key-table implementations, including packing, decoded uniques and
// destruction. Fixtures and the independent string-key oracle are deliberately outside timing.
// Shapes cover short keys, fallback, small batches, high cardinality and changing batch sizes.
struct StringKeyCase {
  int columns = 1;
  int length = 3;
  uint32_t groups = 8;
  int64_t batch_rows = kBatchSize;
  bool shuffled = true;
  bool nullable = false;
  bool sliced = false;
  bool binary = false;
  bool promote = false;
  bool shrinking = false;
  bool mixed = false;
};

struct StringKeyInput {
  std::vector<arrow::TypeHolder> types;
  std::vector<arrow::compute::ExecBatch> batches;
  int64_t rows = 0;
  int64_t value_bytes = 0;
  int64_t nulls = 0;
  int min_length = 24;
  int max_length = 0;
};

std::string StringKeyValue(uint32_t group, int column, const StringKeyCase& shape) {
  constexpr std::string_view kAlphabet =
      "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz_-";
  const uint64_t capacity = uint64_t{1} << Narrow<unsigned>(6 * std::min(shape.length, 7));
  uint64_t value = group;
  if (column != 0) {
    // The quotient makes two one-byte columns represent all 4096 distinct pairs; the other
    // term varies both columns even at low cardinality.
    value = (value / capacity) + value * 13U + 7U;
  }
  std::string out(Narrow<std::size_t>(shape.length), '0');
  for (char& byte : out) {
    byte = kAlphabet[Narrow<std::size_t>(value % kAlphabet.size())];
    value /= kAlphabet.size();
  }
  if (shape.binary) {
    out.back() = '\0';  // Embedded NUL is a value byte, never a terminator.
  }
  return out;
}

arrow::Result<StringKeyInput> MakeStringKeyInput(const StringKeyCase& shape) {
  StringKeyInput input;
  constexpr std::array<int64_t, 6> kMixedBatchRows{1, 256, kBatchSize, 16, 257, 256};
  const int64_t batch_count = shape.mixed ? Narrow<int64_t>(kMixedBatchRows.size())
                                          : (shape.batch_rows == kBatchSize ? 4 : 64);
  if (shape.mixed) {
    for (const int64_t rows : kMixedBatchRows) {
      input.rows += rows;
    }
  } else {
    input.rows = shape.shrinking ? shape.batch_rows + 33 : shape.batch_rows * batch_count;
  }
  const auto row_count = Narrow<std::size_t>(input.rows);
  std::vector<uint32_t> groups(row_count);
  for (std::size_t i = 0; i < row_count; ++i) {
    groups[i] =
        Narrow<uint32_t>(shape.shuffled ? i % shape.groups : (i * shape.groups) / row_count);
  }
  if (shape.shuffled) {
    uint64_t seed = 0x535452494E474B45ULL;
    for (std::size_t end = groups.size(); end > 1; --end) {
      const auto other = Narrow<std::size_t>(SplitMix64(seed) % end);
      std::swap(groups[end - 1], groups[other]);
    }
  }
  const auto type = shape.binary ? arrow::binary() : arrow::utf8();
  std::vector<std::vector<std::string>> values(Narrow<std::size_t>(shape.columns));
  for (int column = 0; column < shape.columns; ++column) {
    input.types.emplace_back(type);
    auto& keys = values[Narrow<std::size_t>(column)];
    keys.reserve(shape.groups);
    for (uint32_t group = 0; group < shape.groups; ++group) {
      keys.push_back(StringKeyValue(group, column, shape));
    }
  }
  int64_t row_start = 0;
  for (int64_t batch_index = 0; batch_index < batch_count; ++batch_index) {
    const int64_t batch_rows =
        shape.mixed
            ? kMixedBatchRows[Narrow<std::size_t>(batch_index)]
            : (shape.shrinking && batch_index > 0 ? (batch_index == 2 ? 1 : 16) : shape.batch_rows);
    std::vector<arrow::Datum> columns;
    for (int column = 0; column < shape.columns; ++column) {
      ARROW_ASSIGN_OR_RAISE(auto builder, arrow::MakeBuilder(type));
      auto* binary_builder = static_cast<arrow::BinaryBuilder*>(builder.get());
      const int64_t prefix = shape.sliced ? 7 : 0;
      const int64_t suffix = shape.sliced ? 3 : 0;
      ARROW_RETURN_NOT_OK(binary_builder->Reserve(batch_rows + prefix + suffix));
      for (int64_t i = 0; i < prefix; ++i) {
        ARROW_RETURN_NOT_OK(binary_builder->Append("prefix-outside-the-slice"));
      }
      for (int64_t row = 0; row < batch_rows; ++row) {
        const uint32_t group = groups[Narrow<std::size_t>(row_start + row)];
        const uint32_t pattern = group + Narrow<uint32_t>(column) * 3U;
        if (shape.nullable && pattern % 11U == 0) {
          ARROW_RETURN_NOT_OK(binary_builder->AppendNull());
          ++input.nulls;
          continue;
        }
        std::string_view value = values[Narrow<std::size_t>(column)][group];
        if (shape.nullable && pattern % 13U == 2) {
          value = {};
        }
        // Two complete short batches precede this new long key. The last batch contains the
        // original short keys again, exercising stable ids after permanent promotion.
        if (shape.promote && batch_index == 2 && row == 0 && column == shape.columns - 1) {
          value = "a-new-long-key-after-run";
        }
        ARROW_RETURN_NOT_OK(binary_builder->Append(value));
        input.value_bytes += Narrow<int64_t>(value.size());
        input.min_length = std::min(input.min_length, Narrow<int>(value.size()));
        input.max_length = std::max(input.max_length, Narrow<int>(value.size()));
      }
      for (int64_t i = 0; i < suffix; ++i) {
        ARROW_RETURN_NOT_OK(binary_builder->Append("suffix-outside-the-slice"));
      }
      ARROW_ASSIGN_OR_RAISE(auto array, builder->Finish());
      columns.emplace_back(array->Slice(prefix, batch_rows));
    }
    input.batches.emplace_back(std::move(columns), batch_rows);
    row_start += batch_rows;
  }
  return input;
}

// An unambiguous semantic representation, unrelated to the packed adapter's word encoding.
// Input and unique arrays are both read through Arrow, including offsets and NULL validity.
std::string ModelStringKey(const arrow::ArrayVector& columns, int64_t row) {
  std::string key;
  for (const auto& array : columns) {
    if (array->IsNull(row)) {
      key += "N;";
      continue;
    }
    const auto bytes = static_cast<const arrow::BinaryArray&>(*array).GetView(row);
    key += 'S';
    key += std::to_string(bytes.size());
    key += ':';
    key.append(bytes);
    key += ';';
  }
  return key;
}

arrow::Result<uint32_t> ValidateStringKeyTable(const StringKeyInput& input,
                                               exec::KeyTableKind kind) {
  exec::MemoryBudget budget(std::nullopt);
  arrow::compute::ExecContext ctx(&budget);
  ARROW_ASSIGN_OR_RAISE(auto table, exec::MakeKeyTable(input.types, &ctx, {.kind = kind}));
  std::unordered_map<std::string, uint32_t> model;
  std::vector<std::optional<std::string>> keys_by_id;
  for (const auto& batch : input.batches) {
    const auto before = Narrow<uint32_t>(model.size());
    arrow::ArrayVector columns;
    for (const auto& value : batch.values) {
      columns.push_back(value.make_array());
    }
    ARROW_ASSIGN_OR_RAISE(auto ids, table->Consume(batch));
    if (ids.before != before || ids.after < ids.before || ids.after != table->num_groups() ||
        ids.ids.size() != Narrow<std::size_t>(batch.length)) {
      return arrow::Status::Invalid("key-table benchmark: invalid group-count contract");
    }
    keys_by_id.resize(ids.after);
    for (int64_t row = 0; row < batch.length; ++row) {
      const uint32_t id = ids.ids[Narrow<std::size_t>(row)];
      if (id >= ids.after) {
        return arrow::Status::Invalid("key-table benchmark: out-of-range group id");
      }
      auto key = ModelStringKey(columns, row);
      const auto [entry, inserted] = model.try_emplace(key, id);
      if (entry->second != id || (keys_by_id[id] && *keys_by_id[id] != key)) {
        return arrow::Status::Invalid("key-table benchmark: inconsistent key-to-id mapping");
      }
      if (inserted) {
        keys_by_id[id] = std::move(key);
      }
    }
    if (model.size() != ids.after) {
      return arrow::Status::Invalid("key-table benchmark: distinct-key count differs from oracle");
    }
  }
  ARROW_ASSIGN_OR_RAISE(auto snapshot, table->Uniques());
  const auto& uniques = snapshot.batch;
  if (uniques.length != Narrow<int64_t>(model.size()) ||
      uniques.values.size() != input.types.size()) {
    return arrow::Status::Invalid("key-table benchmark: wrong unique dimensions");
  }
  arrow::ArrayVector unique_columns;
  for (std::size_t column = 0; column < input.types.size(); ++column) {
    if (!uniques.values[column].type()->Equals(*input.types[column].type)) {
      return arrow::Status::Invalid("key-table benchmark: wrong unique type");
    }
    unique_columns.push_back(uniques.values[column].make_array());
  }
  for (int64_t id = 0; id < uniques.length; ++id) {
    const auto& expected = keys_by_id[Narrow<std::size_t>(id)];
    if (!expected || *expected != ModelStringKey(unique_columns, id)) {
      return arrow::Status::Invalid("key-table benchmark: unique keys not in public id order");
    }
  }
  return table->num_groups();
}

struct StringKeyMemory {
  int64_t after_first = 0;
  int64_t after_consume = 0;
  int64_t with_uniques = 0;
};

template <bool kMeasureMemory = false>
arrow::Status StringKeyTableLifecycle(const StringKeyInput& input, exec::KeyTableKind kind,
                                      arrow::compute::ExecContext* ctx,
                                      StringKeyMemory* memory = nullptr) {
  ARROW_ASSIGN_OR_RAISE(auto table, exec::MakeKeyTable(input.types, ctx, {.kind = kind}));
  for (const auto& batch : input.batches) {
    {
      ARROW_ASSIGN_OR_RAISE(auto ids, table->Consume(batch));
      benchmark::DoNotOptimize(ids.ids.data());
    }
    if constexpr (kMeasureMemory) {
      if (&batch == &input.batches.front()) {
        memory->after_first = ctx->memory_pool()->bytes_allocated();
      }
    }
  }
  if constexpr (kMeasureMemory) {
    memory->after_consume = ctx->memory_pool()->bytes_allocated();
  }
  ARROW_ASSIGN_OR_RAISE(auto snapshot, table->Uniques());
  benchmark::DoNotOptimize(snapshot.batch);
  if constexpr (kMeasureMemory) {
    memory->with_uniques = ctx->memory_pool()->bytes_allocated();
  }
  return arrow::Status::OK();  // The decoded arrays and table are destroyed before returning.
}

void StringKeyTables(benchmark::State& state, StringKeyCase shape, exec::KeyTableKind kind) {
  auto input = MakeStringKeyInput(shape);
  if (!input.ok()) {
    state.SkipWithError(input.status().ToString());
    return;
  }
  auto groups = ValidateStringKeyTable(*input, kind);
  if (!groups.ok()) {
    state.SkipWithError(groups.status().ToString());
    return;
  }
  exec::MemoryBudget budget(std::nullopt);
  arrow::compute::ExecContext ctx(&budget);
  // Sample pool statistics during one separate lifecycle. No oracle/input allocations occur
  // between snapshots. These counters exclude allocations outside Arrow's query pool.
  const int64_t bytes_before = budget.total_bytes_allocated();
  const int64_t allocations_before = budget.num_allocations();
  StringKeyMemory memory;
  const auto measured = StringKeyTableLifecycle<true>(*input, kind, &ctx, &memory);
  if (!measured.ok()) {
    state.SkipWithError(measured.ToString());
    return;
  }
  const int64_t allocated_bytes = budget.total_bytes_allocated() - bytes_before;
  const int64_t allocations = budget.num_allocations() - allocations_before;
  const int64_t peak_bytes = budget.max_memory();
  if (budget.bytes_allocated() != 0) {
    state.SkipWithError("key-table benchmark: lifecycle left query-pool allocations alive");
    return;
  }
  for (auto _ : state) {
    const auto status = StringKeyTableLifecycle(*input, kind, &ctx);
    if (!status.ok()) {
      state.SkipWithError(status.ToString());
      return;
    }
  }
  state.SetItemsProcessed(state.iterations() * input->rows);
  state.SetBytesProcessed(state.iterations() * input->value_bytes);
  state.counters["groups"] = *groups;
  state.counters["rows"] = static_cast<double>(input->rows);
  state.counters["columns"] = shape.columns;
  state.counters["batch_rows"] = static_cast<double>(shape.batch_rows);
  state.counters["min_key_bytes"] = input->min_length;
  state.counters["max_key_bytes"] = input->max_length;
  const int64_t value_count = input->rows * shape.columns;
  state.counters["mean_key_bytes"] =
      static_cast<double>(input->value_bytes) / static_cast<double>(value_count - input->nulls);
  state.counters["null_fraction"] =
      static_cast<double>(input->nulls) / static_cast<double>(value_count);
  state.counters["pool_peak_bytes"] = static_cast<double>(peak_bytes);
  state.counters["pool_total_bytes"] = static_cast<double>(allocated_bytes);
  state.counters["pool_allocations"] = static_cast<double>(allocations);
  state.counters["pool_live_after_first"] = static_cast<double>(memory.after_first);
  state.counters["pool_live_after_consume"] = static_cast<double>(memory.after_consume);
  state.counters["pool_live_with_uniques"] = static_cast<double>(memory.with_uniques);
  state.counters["seconds_per_row"] = benchmark::Counter(
      static_cast<double>(input->rows),
      benchmark::Counter::kIsIterationInvariantRate | benchmark::Counter::kInvert);
}

void RegisterStringKeyTables() {
  const auto add = [](const StringKeyCase& shape) {
    std::string name = "/keys:" + std::to_string(shape.columns) +
                       "/len:" + std::to_string(shape.length) + (shape.promote ? "_to24" : "") +
                       "/input_groups:" + std::to_string(shape.groups) +
                       "/batch:" + std::to_string(shape.batch_rows) +
                       (shape.shuffled ? "/shuffled" : "/clustered");
    if (shape.nullable) {
      name += "/null_empty";
    }
    if (shape.sliced) {
      name += "/sliced";
    }
    name += shape.binary ? "/binary" : "/string";
    if (shape.shrinking) {
      name += "/shrinking";
    }
    if (shape.mixed) {
      name += "/mixed_1_256_65536_16_257_256";
    }
    benchmark::RegisterBenchmark(("BM_KeyTableGrouper" + name).c_str(),
                                 [shape](benchmark::State& state) {
                                   StringKeyTables(state, shape, exec::KeyTableKind::kGrouper);
                                 })
        ->Unit(benchmark::kMillisecond);
    benchmark::RegisterBenchmark(("BM_KeyTablePacked" + name).c_str(), [shape](benchmark::State&
                                                                                   state) {
      StringKeyTables(state, shape, exec::KeyTableKind::kPackedStrings);
    })->Unit(benchmark::kMillisecond);
  };
  for (const int columns : {1, 2}) {
    for (const int length : {1, 3, 7, 8, 24}) {
      add({.columns = columns, .length = length});
      const uint32_t high_groups = length == 1 ? (columns == 1 ? 64U : 4096U) : 32768U;
      add({.columns = columns, .length = length, .groups = high_groups});
    }
    for (const int length : {1, 3, 7}) {
      add({.columns = columns, .length = length, .shuffled = false});
    }
    add({.columns = columns, .length = 7, .groups = 32768, .shuffled = false});
    for (const int64_t batch_rows :
         {int64_t{1}, int64_t{16}, int64_t{255}, int64_t{256}, int64_t{257}}) {
      add({.columns = columns, .batch_rows = batch_rows});
    }
    for (const bool binary : {false, true}) {
      add({.columns = columns,
           .length = 7,
           .groups = 64,
           .nullable = true,
           .sliced = true,
           .binary = binary});
    }
    add({.columns = columns, .promote = true});
    add({.columns = columns, .shrinking = true});
    add({.columns = columns, .mixed = true});
  }
}

}  // namespace
}  // namespace antb1::bench

int main(int argc, char** argv) {
  if (!arrow::compute::Initialize().ok()) {
    return 1;
  }
  antb1::bench::RegisterStringKeyTables();
  antb1::bench::RegisterGroupPrograms();
  benchmark::Initialize(&argc, argv);
  if (benchmark::ReportUnrecognizedArguments(argc, argv)) {
    return 1;
  }
  // The build these numbers come from, in the report's context (antb1_ keys next to Google
  // Benchmark's own): the binary is not antb1, so it cannot print `version --verbose`.
  const std::string_view commit = antb1::GitSha();
  benchmark::AddCustomContext("antb1_commit", commit.empty() ? "unknown" : std::string(commit));
  benchmark::AddCustomContext("antb1_flavor", std::string(antb1::BuildFlavor()));
  benchmark::AddCustomContext("antb1_cpu_baseline", std::string(antb1::CpuBaseline()));
  benchmark::AddCustomContext("antb1_build_type", std::string(antb1::BuildType()));
  benchmark::RunSpecifiedBenchmarks();
  benchmark::Shutdown();
  std::error_code ec;
  std::filesystem::remove(antb1::bench::ScanFilePath(), ec);  // if BM_ScanColumn wrote it
  std::filesystem::remove(antb1::bench::UrlFilePath(), ec);   // if the string filters wrote it
  return 0;
}
