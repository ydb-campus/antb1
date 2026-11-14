#pragma once

// ADR 0027's fixed-plan payload experiment. Inputs are independent synthetic Parquet files.
// Timing includes physical planning, Parquet decoding, the join, post-join numeric selection,
// top-N, final Binary output, a byte-reading consumer, and destruction. Fixture generation,
// registration, the independent oracle and profile/ownership checks are outside timing. SQL
// parsing and optimization are deliberately excluded: both arms execute the same logical plan.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <tuple>
#include <utility>
#include <vector>

#include <arrow/api.h>
#include <arrow/io/file.h>
#include <benchmark/benchmark.h>
#include <parquet/arrow/writer.h>

#include "antb1/common/int128.h"
#include "antb1/common/narrow.h"
#include "antb1/exec/memory_budget.h"
#include "antb1/exec/operator.h"
#include "antb1/exec/physical_planner.h"
#include "antb1/exec/profile.h"
#include "antb1/io/parquet_table.h"
#include "antb1/plan/logical_plan.h"

#include <unistd.h>

namespace antb1::bench::view_pipeline {
namespace {

constexpr int64_t kProbeRows = int64_t{1} << 18;
constexpr int64_t kBuildKeys = int64_t{1} << 12;
constexpr int64_t kRowGroupRows = int64_t{1} << 14;

struct Shape {
  int length;
  int percent;
  int fanout;
  int64_t keep;
};

constexpr std::array<Shape, 12> kShapes{{
    {1, 100, 1, 100},
    {12, 100, 1, 8192},
    {13, 100, 1, 8192},
    {32, 100, 1, 8192},
    {128, 100, 1, 8192},
    {13, 1, 1, 8192},
    {128, 1, 1, 8192},
    {128, 10, 1, 8192},
    {13, 100, 4, 8192},
    {128, 100, 4, 8192},
    {128, 1, 4, 100},
    {128, 100, 4, 262144},
}};

uint64_t Mix(uint64_t value) {
  value += 0x9E3779B97F4A7C15ULL;
  value = (value ^ (value >> 30U)) * 0xBF58476D1CE4E5B9ULL;
  value = (value ^ (value >> 27U)) * 0x94D049BB133111EBULL;
  return value ^ (value >> 31U);
}

int64_t ProbeKey(int64_t id) { return (id * 4051) % kBuildKeys; }
int64_t Score(int64_t id) { return (id * 104729) % 65536; }
int64_t Gate(int64_t id) { return (id * 37) % 100; }

std::optional<std::string> Payload(int64_t key, int copy, int length) {
  if (key % 97 == 0) {
    return std::nullopt;
  }
  if (key % 103 == 0) {
    return std::string();
  }
  std::string value(Narrow<std::size_t>(length), '\0');
  uint64_t bits = Mix(static_cast<uint64_t>(key) + static_cast<uint64_t>(copy) * 65537U);
  for (char& byte : value) {
    bits = Mix(bits);
    byte = static_cast<char>('a' + bits % 26U);
  }
  return value;
}

const std::filesystem::path& Directory() {
  static const std::filesystem::path path = [] {
    // An explicit directory keeps fixtures for a separate fresh-process CLI campaign. The
    // default is unique to this process and is removed by Cleanup() after benchmark shutdown.
    const char* kept = std::getenv("ANTB1_VIEW_BENCH_DATA");
    return kept != nullptr && *kept != '\0'
               ? std::filesystem::path(kept)
               : std::filesystem::temp_directory_path() /
                     ("antb1-bench-view-" + std::to_string(::getpid()));
  }();
  return path;
}

arrow::Status Write(const arrow::Table& table, const std::filesystem::path& file) {
  ARROW_ASSIGN_OR_RAISE(auto stream, arrow::io::FileOutputStream::Open(file.string()));
  const auto properties = parquet::WriterProperties::Builder()
                              .compression(parquet::Compression::SNAPPY)
                              ->disable_dictionary()
                              ->build();
  ARROW_RETURN_NOT_OK(parquet::arrow::WriteTable(table, arrow::default_memory_pool(), stream,
                                                 kRowGroupRows, properties));
  return stream->Close();
}

arrow::Result<std::filesystem::path> ProbeFile() {
  static std::filesystem::path file;
  if (!file.empty()) {
    return file;
  }
  std::error_code error;
  std::filesystem::create_directories(Directory(), error);
  if (error) {
    return arrow::Status::IOError("creating view benchmark directory: ", error.message());
  }
  arrow::Int64Builder keys;
  arrow::Int64Builder ids;
  arrow::Int64Builder scores;
  arrow::Int64Builder gates;
  for (auto* builder : {&keys, &ids, &scores, &gates}) {
    ARROW_RETURN_NOT_OK(builder->Reserve(kProbeRows));
  }
  for (int64_t id = 0; id < kProbeRows; ++id) {
    keys.UnsafeAppend(ProbeKey(id));
    ids.UnsafeAppend(id);
    scores.UnsafeAppend(Score(id));
    gates.UnsafeAppend(Gate(id));
  }
  arrow::ArrayVector arrays;
  for (auto* builder : {&keys, &ids, &scores, &gates}) {
    ARROW_ASSIGN_OR_RAISE(auto array, builder->Finish());
    arrays.push_back(std::move(array));
  }
  const auto schema =
      arrow::schema({arrow::field("k", arrow::int64()), arrow::field("id", arrow::int64()),
                     arrow::field("score", arrow::int64()), arrow::field("gate", arrow::int64())});
  const auto candidate = Directory() / "probe.parquet";
  ARROW_RETURN_NOT_OK(Write(*arrow::Table::Make(schema, arrays), candidate));
  file = candidate;
  return file;
}

arrow::Result<std::filesystem::path> BuildFile(const Shape& shape) {
  static std::map<std::pair<int, int>, std::filesystem::path> files;
  const auto key = std::pair(shape.length, shape.fanout);
  if (const auto found = files.find(key); found != files.end()) {
    return found->second;
  }
  arrow::Int64Builder keys;
  arrow::Int64Builder copies;
  arrow::BinaryBuilder payloads;
  for (int copy = 0; copy < shape.fanout; ++copy) {
    for (int64_t value = 0; value < kBuildKeys; ++value) {
      ARROW_RETURN_NOT_OK(keys.Append(value));
      ARROW_RETURN_NOT_OK(copies.Append(copy));
      const auto payload = Payload(value, copy, shape.length);
      ARROW_RETURN_NOT_OK(payload.has_value() ? payloads.Append(*payload) : payloads.AppendNull());
    }
  }
  ARROW_ASSIGN_OR_RAISE(auto key_array, keys.Finish());
  ARROW_ASSIGN_OR_RAISE(auto copy_array, copies.Finish());
  ARROW_ASSIGN_OR_RAISE(auto payload_array, payloads.Finish());
  const auto schema =
      arrow::schema({arrow::field("k", arrow::int64()), arrow::field("copy", arrow::int64()),
                     arrow::field("payload", arrow::binary())});
  const auto file = Directory() / ("build-l" + std::to_string(shape.length) + "-f" +
                                   std::to_string(shape.fanout) + ".parquet");
  ARROW_RETURN_NOT_OK(
      Write(*arrow::Table::Make(schema, {key_array, copy_array, payload_array}), file));
  files.emplace(key, file);
  return file;
}

plan::LogicalNodePtr Node(plan::LogicalNode node) {
  return std::make_shared<const plan::LogicalNode>(std::move(node));
}

plan::BoundColumn Column(int index, std::string name,
                         plan::LogicalType type = plan::LogicalType::kBigInt) {
  return {.index = index, .name = std::move(name), .type = type};
}

arrow::Result<plan::LogicalPlan> MakePlan(const Shape& shape) {
  ARROW_ASSIGN_OR_RAISE(auto probe_file, ProbeFile());
  ARROW_ASSIGN_OR_RAISE(auto build_file, BuildFile(shape));
  ARROW_ASSIGN_OR_RAISE(auto probe, io::ParquetTable::Open({probe_file.string()}));
  ARROW_ASSIGN_OR_RAISE(auto build, io::ParquetTable::Open({build_file.string()}));
  const auto left = Node(plan::ScanNode{.table = probe, .table_name = "p", .fields = {0, 1, 2, 3}});
  const auto right = Node(plan::ScanNode{.table = build, .table_name = "b", .fields = {0, 1, 2}});
  const auto join = Node(plan::JoinNode{.kind = plan::JoinKind::kInner,
                                        .left = left,
                                        .right = right,
                                        .keys = {{.left = Column(0, "k"), .right = Column(0, "k")}},
                                        .build = plan::BuildSide::kRight});
  const auto gate = std::make_shared<const plan::Expr>(
      plan::Expr{.node = plan::ColumnExpr{.index = 3}, .type = plan::LogicalType::kBigInt});
  const auto copy = std::make_shared<const plan::Expr>(
      plan::Expr{.node = plan::ColumnExpr{.index = 5}, .type = plan::LogicalType::kBigInt});
  const auto sum = std::make_shared<const plan::Expr>(
      plan::Expr{.node = plan::ArithExpr{.op = plan::ArithOp::kAdd, .left = gate, .right = copy},
                 .type = plan::LogicalType::kBigInt});
  const auto hundred = std::make_shared<const plan::Expr>(plan::Expr{
      .node =
          plan::ConstantExpr{.value = {.type = plan::LogicalType::kBigInt, .value = Int128{100}}},
      .type = plan::LogicalType::kBigInt});
  const auto residue = std::make_shared<const plan::Expr>(plan::Expr{
      .node = plan::ArithExpr{.op = plan::ArithOp::kModulo, .left = sum, .right = hundred},
      .type = plan::LogicalType::kBigInt,
      .name = "(gate + copy) % 100"});
  const auto computed = Node(plan::ComputeNode{.input = join, .exprs = {residue}});
  const auto selected =
      Node(plan::FilterNode{.input = computed,
                            .predicates = {{.kind = plan::Predicate::Kind::kCompare,
                                            .column = Column(7, "(gate + copy) % 100"),
                                            .op = plan::CompareOp::kLt,
                                            .constant = {.type = plan::LogicalType::kBigInt,
                                                         .value = Int128{shape.percent}}}}});
  const auto sorted = Node(plan::SortNode{.input = selected,
                                          .keys = {{.column = Column(2, "score")},
                                                   {.column = Column(1, "id")},
                                                   {.column = Column(5, "copy")}}});
  const auto limited = Node(plan::LimitNode{.input = sorted, .limit = shape.keep});
  return plan::LogicalPlan{.root = Node(plan::ProjectNode{
                               .input = limited,
                               .columns = {Column(2, "score"), Column(1, "id"), Column(5, "copy"),
                                           Column(6, "payload", plan::LogicalType::kVarchar)}}),
                           .output = {{.name = "score", .type = plan::LogicalType::kBigInt},
                                      {.name = "id", .type = plan::LogicalType::kBigInt},
                                      {.name = "copy", .type = plan::LogicalType::kBigInt},
                                      {.name = "payload", .type = plan::LogicalType::kVarchar}}};
}

struct ExpectedRow {
  int64_t score;
  int64_t id;
  int copy;
};

std::vector<ExpectedRow> Expected(const Shape& shape) {
  std::vector<ExpectedRow> rows;
  for (int64_t id = 0; id < kProbeRows; ++id) {
    for (int copy = 0; copy < shape.fanout; ++copy) {
      if ((Gate(id) + copy) % 100 < shape.percent) {
        rows.push_back({.score = Score(id), .id = id, .copy = copy});
      }
    }
  }
  const auto kept = std::min(rows.size(), Narrow<std::size_t>(shape.keep));
  std::partial_sort(rows.begin(), rows.begin() + Narrow<std::ptrdiff_t>(kept), rows.end(),
                    [](const auto& a, const auto& b) {
                      return std::tie(a.score, a.id, a.copy) < std::tie(b.score, b.id, b.copy);
                    });
  rows.resize(kept);
  return rows;
}

bool ContainsBinaryOutput(const exec::ProfileNode& profile) {
  if (profile.name() == "BinaryOutput") {
    return true;
  }
  return std::ranges::any_of(profile.children(), [](const exec::ProfileNode* child) {
    return ContainsBinaryOutput(*child);
  });
}

struct MemorySample {
  int64_t peak = 0;
  int64_t with_result = 0;
  int64_t allocations = 0;
  int64_t total_bytes = 0;
};

arrow::Result<uint64_t> ReadOutput(const arrow::Table& table, const Shape& shape,
                                   const std::vector<ExpectedRow>* expected) {
  if (table.num_columns() != 4 || table.field(0)->type()->id() != arrow::Type::INT64 ||
      table.field(1)->type()->id() != arrow::Type::INT64 ||
      table.field(2)->type()->id() != arrow::Type::INT64 ||
      table.field(3)->type()->id() != arrow::Type::BINARY) {
    return arrow::Status::Invalid("view pipeline output must have three BIGINTs and Binary");
  }
  if (expected != nullptr && !std::cmp_equal(table.num_rows(), expected->size())) {
    return arrow::Status::Invalid("view pipeline returned a wrong row count");
  }
  arrow::TableBatchReader reader(table);
  uint64_t checksum = 0;
  std::size_t at = 0;
  while (true) {
    std::shared_ptr<arrow::RecordBatch> batch;
    ARROW_RETURN_NOT_OK(reader.ReadNext(&batch));
    if (batch == nullptr) {
      break;
    }
    const auto& scores = static_cast<const arrow::Int64Array&>(*batch->column(0));
    const auto& ids = static_cast<const arrow::Int64Array&>(*batch->column(1));
    const auto& copies = static_cast<const arrow::Int64Array&>(*batch->column(2));
    const auto& payloads = static_cast<const arrow::BinaryArray&>(*batch->column(3));
    for (int64_t row = 0; row < batch->num_rows(); ++row, ++at) {
      if (scores.IsNull(row) || ids.IsNull(row) || copies.IsNull(row)) {
        return arrow::Status::Invalid("view pipeline unexpectedly returned a NULL numeric key");
      }
      if (expected != nullptr) {
        const auto& want = (*expected)[at];
        const auto payload = Payload(ProbeKey(want.id), want.copy, shape.length);
        if (scores.Value(row) != want.score || ids.Value(row) != want.id ||
            copies.Value(row) != want.copy || payloads.IsNull(row) != !payload.has_value() ||
            (payload.has_value() && payloads.GetView(row) != *payload)) {
          return arrow::Status::Invalid("view pipeline differs from its independent row model");
        }
      }
      checksum += static_cast<uint64_t>(scores.Value(row)) + static_cast<uint64_t>(ids.Value(row)) +
                  static_cast<uint64_t>(copies.Value(row));
      if (payloads.IsNull(row)) {
        checksum += 17U;
      } else {
        const std::string_view value = payloads.GetView(row);
        checksum += value.size();
        for (const char byte : value) {
          checksum += static_cast<unsigned char>(byte);
        }
      }
    }
  }
  return checksum;
}

arrow::Result<uint64_t> Run(const plan::LogicalPlan& plan, const Shape& shape, bool views,
                            const std::vector<ExpectedRow>* expected = nullptr,
                            MemorySample* sample = nullptr) {
  exec::MemoryBudget budget(std::nullopt);
  const int64_t allocations_before = budget.num_allocations();
  const int64_t bytes_before = budget.total_bytes_allocated();
  uint64_t checksum = 0;
  {
    ARROW_ASSIGN_OR_RAISE(auto op,
                          exec::BuildPhysicalPlan(plan, nullptr, {.binary_view_payload = views}));
    exec::ExecContext ctx{.pool = &budget, .budget = &budget};
    ARROW_ASSIGN_OR_RAISE(auto table, exec::Drain(*op, ctx));
    // Drain already closed the operators. Drop the tree too, then read every result byte: output
    // correctness must not depend on retained join state or an open producer.
    op.reset();
    ARROW_ASSIGN_OR_RAISE(checksum, ReadOutput(*table, shape, expected));
    if (sample != nullptr) {
      sample->peak = budget.max_memory();
      sample->with_result = budget.bytes_allocated();
      sample->allocations = budget.num_allocations() - allocations_before;
      sample->total_bytes = budget.total_bytes_allocated() - bytes_before;
    }
  }
  if (budget.bytes_allocated() != 0) {
    return arrow::Status::Invalid("view pipeline retained query memory after destruction");
  }
  return checksum;
}

void Pipeline(benchmark::State& state, Shape shape, bool views) {
  const auto plan = MakePlan(shape);
  if (!plan.ok()) {
    state.SkipWithError(plan.status().ToString());
    return;
  }
  exec::ProfileNode profile;
  const auto profiled = exec::BuildPhysicalPlan(*plan, &profile, {.binary_view_payload = views});
  if (!profiled.ok() || ContainsBinaryOutput(profile) != views) {
    state.SkipWithError(profiled.ok() ? "view pipeline eligibility was not exercised"
                                      : profiled.status().ToString());
    return;
  }
  const auto expected = Expected(shape);
  MemorySample memory;
  const auto checked = Run(*plan, shape, views, &expected, &memory);
  if (!checked.ok()) {
    state.SkipWithError(checked.status().ToString());
    return;
  }
  for (auto _ : state) {
    auto result = Run(*plan, shape, views);
    if (!result.ok()) {
      state.SkipWithError(result.status().ToString());
      return;
    }
    benchmark::DoNotOptimize(*result);
  }
  state.SetItemsProcessed(state.iterations() * kProbeRows);
  state.counters["probe_rows"] = static_cast<double>(kProbeRows);
  state.counters["output_rows"] = static_cast<double>(expected.size());
  state.counters["pool_peak_bytes"] = static_cast<double>(memory.peak);
  state.counters["pool_live_with_result"] = static_cast<double>(memory.with_result);
  state.counters["pool_allocations"] = static_cast<double>(memory.allocations);
  state.counters["pool_total_bytes"] = static_cast<double>(memory.total_bytes);
  state.counters["view_path"] = views ? 1.0 : 0.0;
}

}  // namespace

inline void Register() {
  for (const auto& shape : kShapes) {
    const auto suffix = "/len" + std::to_string(shape.length) + "/select" +
                        std::to_string(shape.percent) + "/fanout" + std::to_string(shape.fanout) +
                        "/keep" + std::to_string(shape.keep);
    for (const bool views : {false, true}) {
      const auto name =
          std::string("BM_BinaryViewPayloadPipeline/") + (views ? "views" : "binary") + suffix;
      benchmark::RegisterBenchmark(
          name.c_str(), [shape, views](benchmark::State& state) { Pipeline(state, shape, views); });
    }
  }
}

inline void Cleanup() {
  const char* kept = std::getenv("ANTB1_VIEW_BENCH_DATA");
  if (kept == nullptr || *kept == '\0') {
    std::error_code ignored;
    std::filesystem::remove_all(Directory(), ignored);
  }
}

}  // namespace antb1::bench::view_pipeline
