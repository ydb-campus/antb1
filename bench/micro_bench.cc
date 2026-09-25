// Micro benchmarks of antb1's hot paths (Google Benchmark; docs/benchmarks.md). `pixi run bench`
// runs them on the Release `bench` preset and writes build/bench/micro.json; bench.yml publishes
// the numbers from main. Numbers never gate a PR. Inputs are synthetic (splitmix64), never
// ClickBench data.

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <arrow/api.h>
#include <arrow/compute/api.h>
#include <arrow/io/file.h>
#include <benchmark/benchmark.h>
#include <parquet/arrow/writer.h>

#include "antb1/exec/aggregate_state.h"
#include "antb1/io/parquet_table.h"
#include "antb1/plan/logical_plan.h"
#include "antb1/plan/types.h"
#include "antb1/sql/parser.h"

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
  benchmark::RunSpecifiedBenchmarks();
  benchmark::Shutdown();
  std::error_code ec;
  std::filesystem::remove(antb1::bench::ScanFilePath(), ec);  // if BM_ScanColumn wrote it
  return 0;
}
