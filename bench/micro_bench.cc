// Micro benchmarks of antb1's hot paths (Google Benchmark; docs/benchmarks.md). `pixi run bench`
// runs them in build/static (preset `static`: the static Arrow, -O3, ThinLTO) and writes
// micro.json into build/static/bench-results; bench.yml publishes the numbers from main. Numbers
// never gate a PR. Inputs are synthetic (splitmix64), never ClickBench data.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <arrow/api.h>
#include <arrow/compute/api.h>
#include <arrow/io/file.h>
#include <arrow/util/bit_util.h>
#include <benchmark/benchmark.h>
#include <parquet/arrow/writer.h>

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

#include "group_loops.h"
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

uint64_t GroupIdOf(int64_t row, int64_t groups) {
  return (static_cast<uint64_t>(row) * kGroupStride) % static_cast<uint64_t>(groups);
}

// Which key columns a shape groups by. The gate's four: one and two fixed-width keys, a string, and
// a fixed-width key beside a string.
enum class GroupShape : std::uint8_t { kInt64, kInt64Int32, kString, kInt64String };

// `rows` rows of `shape`'s key columns, in 64Ki-row batches, with keys over `groups` values; an
// empty vector if they cannot be built. Each row's keys come from one group id, so a shape's
// columns agree on which group a row belongs to and the group count is the id count. `rows` is at
// least `groups` (GroupRowsFor), so every group is reached and the count the grouper reports is the
// argument exactly -- which the `groups` counter shows.
arrow::RecordBatchVector GroupInput(GroupShape shape, int64_t groups, int64_t rows) {
  arrow::FieldVector fields;
  switch (shape) {
    case GroupShape::kInt64:
      fields = {arrow::field("k", arrow::int64())};
      break;
    case GroupShape::kInt64Int32:
      fields = {arrow::field("k", arrow::int64()), arrow::field("j", arrow::int32())};
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
  const bool has_int32 = shape == GroupShape::kInt64Int32;
  const bool has_string = shape == GroupShape::kString || shape == GroupShape::kInt64String;
  arrow::RecordBatchVector out;
  for (int64_t start = 0; start < rows; start += kBatchSize) {
    arrow::Int64Builder keys;
    arrow::Int32Builder seconds;
    arrow::BinaryBuilder strings;
    for (int64_t row = start; row < start + kBatchSize; ++row) {
      const uint64_t id = GroupIdOf(row, groups);
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
    if ((has_int64 && !take(keys)) || (has_int32 && !take(seconds)) ||
        (has_string && !take(strings))) {
      return {};
    }
    out.push_back(arrow::RecordBatch::Make(schema, kBatchSize, std::move(columns)));
  }
  return out;
}

// One grouper over every batch, as GroupTable::Consume does per part; with `budgeted`, its buffers
// come from a MemoryBudget without a limit, as the engine's do (engine::Session's pool).
void GroupKeys(benchmark::State& state, GroupShape shape, bool budgeted = false) {
  const int64_t groups = state.range(0);
  const int64_t rows = GroupRowsFor(groups);
  const arrow::RecordBatchVector batches = GroupInput(shape, groups, rows);
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

// ---- Step 0: candidate loops of our own for fixed-width keys (bench/group_loops.h) ----
//
// The same inputs and counters as BM_Group*, under a MemoryBudget as BM_Group*Budget, so their
// ns_per_row compare directly; the go for a shape is 1.3x the budgeted grouper at every count from
// 1000 to 1 Mi groups. Each variant is first checked once per benchmark name: one group per key,
// and every row of a group given that group's id.

step0::Int64Key::Columns ColumnsOf(const arrow::RecordBatch& batch, step0::Int64Key* /*tag*/) {
  return {.k = batch.column_data(0)->GetValues<std::int64_t>(1)};
}
step0::Int64Int32Key::Columns ColumnsOf(const arrow::RecordBatch& batch,
                                        step0::Int64Int32Key* /*tag*/) {
  return {.k = batch.column_data(0)->GetValues<std::int64_t>(1),
          .j = batch.column_data(1)->GetValues<std::int32_t>(1)};
}

template <class Key, step0::Slots kSlots>
arrow::Status Step0Consume(step0::FixedTable<Key, kSlots>& table, const arrow::RecordBatch& batch,
                           std::uint32_t* ids) {
  return table.ConsumeChunked(ColumnsOf(batch, static_cast<Key*>(nullptr)),
                              static_cast<std::size_t>(batch.num_rows()), ids);
}

// One table over every batch: an empty string, or what is wrong with its ids.
template <class Key, step0::Slots kSlots>
std::string Step0Check(const arrow::RecordBatchVector& batches, int64_t groups,
                       step0::TableOptions options, bool presized) {
  exec::MemoryBudget budget(std::nullopt);
  step0::FixedTable<Key, kSlots> table(&budget, options);
  if (presized) {
    if (auto status = table.Reserve(static_cast<std::size_t>(groups)); !status.ok()) {
      return status.ToString();
    }
  }
  std::vector<std::uint32_t> ids(static_cast<std::size_t>(kBatchSize));
  // The table's id of each true group, and back: both must be one to one.
  std::vector<int64_t> ours(static_cast<std::size_t>(groups), -1);
  std::vector<int64_t> theirs(static_cast<std::size_t>(groups), -1);
  int64_t row = 0;
  for (const auto& batch : batches) {
    if (auto status = Step0Consume(table, *batch, ids.data()); !status.ok()) {
      return status.ToString();
    }
    for (int64_t i = 0; i < batch->num_rows(); ++i, ++row) {
      const auto truth = static_cast<std::size_t>(GroupIdOf(row, groups));
      const std::uint32_t id = ids[static_cast<std::size_t>(i)];
      if (id >= static_cast<std::uint64_t>(groups)) {
        return "id " + std::to_string(id) + " past the group count";
      }
      int64_t& mine = ours[truth];
      int64_t& back = theirs[id];
      if (mine == -1 && back == -1) {
        mine = id;
        back = static_cast<int64_t>(truth);
      } else if (mine != id || back != static_cast<int64_t>(truth)) {
        return "row " + std::to_string(row) + " has id " + std::to_string(id) +
               " but its group has " + std::to_string(mine);
      }
    }
  }
  if (table.num_groups() != groups) {
    return std::to_string(table.num_groups()) + " groups for " + std::to_string(groups);
  }
  if (budget.bytes_allocated() <= 0) {
    return "nothing was charged to the budget";
  }
  return {};
}

template <class Key, step0::Slots kSlots>
void Step0Keys(benchmark::State& state, GroupShape shape, step0::TableOptions options,
               bool presized = false) {
  const int64_t groups = state.range(0);
  const int64_t rows = GroupRowsFor(groups);
  const arrow::RecordBatchVector batches = GroupInput(shape, groups, rows);
  if (batches.empty()) {
    state.SkipWithError("cannot build the grouping input");
    return;
  }
  static std::vector<std::string>
      checked;  // Google Benchmark calls this once per run, not per name
  if (std::ranges::find(checked, state.name()) == checked.end()) {
    if (const std::string wrong = Step0Check<Key, kSlots>(batches, groups, options, presized);
        !wrong.empty()) {
      state.SkipWithError("wrong ids: " + wrong);
      return;
    }
    checked.push_back(state.name());
  }
  exec::MemoryBudget budget(std::nullopt);
  std::vector<std::uint32_t> ids(static_cast<std::size_t>(kBatchSize));
  uint32_t made = 0;
  for (auto _ : state) {
    step0::FixedTable<Key, kSlots> table(&budget, options);
    if (presized) {
      if (auto status = table.Reserve(static_cast<std::size_t>(groups)); !status.ok()) {
        state.SkipWithError(status.ToString());
        return;
      }
    }
    for (const auto& batch : batches) {
      if (auto status = Step0Consume(table, *batch, ids.data()); !status.ok()) {
        state.SkipWithError(status.ToString());
        return;
      }
      benchmark::DoNotOptimize(ids.data());
    }
    made = table.num_groups();
    benchmark::DoNotOptimize(made);
  }
  state.SetItemsProcessed(state.iterations() * rows);
  state.counters["groups"] = static_cast<double>(made);
  state.counters["rows"] = static_cast<double>(rows);
  state.counters["ns_per_row"] =
      benchmark::Counter(static_cast<double>(rows), benchmark::Counter::kIsIterationInvariantRate |
                                                        benchmark::Counter::kInvert);
}

// BM_S0<shape>_<slots>[_Presized]: the chunked loop (a room check and a hash pass per 256 rows,
// then the probes, prefetching the buckets of the row 8 places on) at load 7/8. Indirect: the id
// alone in a 4-byte slot, re-placed in id order on growth; Hash32: 32 hash bits beside the id in an
// 8-byte slot, re-placed in slot order. Presized: every group reserved before the first row, the
// floor growth can reach (a diagnostic: the engine may size a table only from its own data).
// Step 0's runs also measured a per-row loop, the key or a 64-bit hash in the slot, prefetch
// distances of 16 and 32, load 1/2 and 4x growth; each lost to these on this host.
#define ANTB1_STEP0(name, Key, shape, slots, presized)                                          \
  void BM_S0##name(benchmark::State& state) {                                                   \
    Step0Keys<step0::Key, step0::Slots::slots>(state, GroupShape::shape, step0::TableOptions{}, \
                                               presized);                                       \
  }
ANTB1_STEP0(Int64_Indirect, Int64Key, kInt64, kIndirect, false)
ANTB1_STEP0(Int64_Hash32, Int64Key, kInt64, kHash32, false)
ANTB1_STEP0(Int64_Hash32_Presized, Int64Key, kInt64, kHash32, true)
ANTB1_STEP0(Int64Int32_Indirect, Int64Int32Key, kInt64Int32, kIndirect, false)
ANTB1_STEP0(Int64Int32_Hash32, Int64Int32Key, kInt64Int32, kHash32, false)
ANTB1_STEP0(Int64Int32_Hash32_Presized, Int64Int32Key, kInt64Int32, kHash32, true)
#undef ANTB1_STEP0

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

// Step 0's counts: the gate's 1000 to 1 Mi and 4 Mi reported (docs/adr/0019's band update).
#define ANTB1_STEP0_COUNTS \
  Arg(1000)->Arg(16 * 1024)->Arg(256 * 1024)->Arg(1024 * 1024)->Arg(4 * 1024 * 1024)
BENCHMARK(BM_GroupInt64Budget)->ANTB1_STEP0_COUNTS->Unit(benchmark::kMillisecond);
BENCHMARK(BM_GroupInt64Int32Budget)->ANTB1_STEP0_COUNTS->Unit(benchmark::kMillisecond);
BENCHMARK(BM_S0Int64_Indirect)->ANTB1_STEP0_COUNTS->Unit(benchmark::kMillisecond);
BENCHMARK(BM_S0Int64_Hash32)->ANTB1_STEP0_COUNTS->Unit(benchmark::kMillisecond);
BENCHMARK(BM_S0Int64_Hash32_Presized)->ANTB1_STEP0_COUNTS->Unit(benchmark::kMillisecond);
BENCHMARK(BM_S0Int64Int32_Indirect)->ANTB1_STEP0_COUNTS->Unit(benchmark::kMillisecond);
BENCHMARK(BM_S0Int64Int32_Hash32)->ANTB1_STEP0_COUNTS->Unit(benchmark::kMillisecond);
BENCHMARK(BM_S0Int64Int32_Hash32_Presized)->ANTB1_STEP0_COUNTS->Unit(benchmark::kMillisecond);
#undef ANTB1_STEP0_COUNTS

}  // namespace
}  // namespace antb1::bench

int main(int argc, char** argv) {
  if (!arrow::compute::Initialize().ok()) {
    return 1;
  }
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
