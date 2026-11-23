// Prototype measurements for grouping sets (ADR 0038's Evidence); not engine code. The work of
// answering GROUP BY ROLLUP(k1, ..., kK) over in-memory batches with the engine's own
// GroupAggregateOperator, on one thread, shaped as the engine runs a GROUP BY: each part (128Ki
// rows) aggregated into partial groups, then the parts' partial groups merged. Seven ways (Way
// below): the finest set alone (the floor); every set fed from each part's rows (the work of
// DuckDB's one-pass operator); the rows copied once per set with a set id (the "hidden set id" of
// ADR 0037's survey); every set derived per part from the part's finest partial groups, as a star
// or a chain; and every set derived from the finest set's final groups, as a star or a chain.
// Counts merge through kSumCounts, HUGEINT sums through SUM, MIN and MAX through themselves, so
// every way gives the same groups. GSETS_ROWS sets the input's rows (default 20,000,000).
// BM_Operator times the engine's own parallel operators on 1 and 14 threads: the finest set alone,
// the two-level PartGroupingSetsOperator, and that operator with every set aggregated from the
// input.

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <numeric>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <arrow/api.h>
#include <arrow/util/thread_pool.h>
#include <benchmark/benchmark.h>

#include "antb1/common/narrow.h"
#include "antb1/exec/group_aggregate.h"
#include "antb1/exec/memory_budget.h"
#include "antb1/exec/operator.h"
#include "antb1/plan/logical_plan.h"
#include "antb1/plan/types.h"

#include "part_operators.h"  // src/exec: the parallel operators, timed directly

namespace antb1::bench {
namespace {

constexpr int64_t kBatch = int64_t{64} * 1024;

uint64_t Mix(uint64_t x) {
  x += 0x9E3779B97F4A7C15ULL;
  x = (x ^ (x >> 30U)) * 0xBF58476D1CE4E5B9ULL;
  x = (x ^ (x >> 27U)) * 0x94D049BB133111EBULL;
  return x ^ (x >> 31U);
}

int64_t Rows() {
  const char* rows = std::getenv("GSETS_ROWS");
  return rows == nullptr ? int64_t{20'000'000} : std::atoll(rows);
}

enum class Profile : std::uint8_t { kHierLow, kHierMid, kHierHigh, kIndepHigh, kWide8 };

int KeysOf(Profile p) { return p == Profile::kWide8 ? 8 : 4; }

// The keys of row `i`: four of one hierarchy (k4 determines k3, k2 and k1; 10, 100 and 1000 values
// at the top three levels and 1e4, 1e6 or 2e7 possible values of k4), four independent keys of 100
// values, or eight: a four-level hierarchy (k4 of 1e5 values), a year of 5 values, a quarter of 4,
// a month that determines its quarter, and an independent key of 50 values.
std::array<int32_t, 8> KeysOfRow(Profile p, uint64_t i) {
  const auto r = [i](uint64_t salt, uint64_t mod) {
    return static_cast<int32_t>(Mix((i * 7919U) + salt) % mod);
  };
  switch (p) {
    case Profile::kHierLow: {
      const int32_t k4 = r(1, 10'000);
      return {k4 / 1000, k4 / 100, k4 / 10, k4};
    }
    case Profile::kHierMid: {
      const int32_t k4 = r(2, 1'000'000);
      return {k4 / 100'000, k4 / 10'000, k4 / 1000, k4};
    }
    case Profile::kHierHigh: {
      const int32_t k4 = r(3, 20'000'000);
      return {k4 / 2'000'000, k4 / 200'000, k4 / 20'000, k4};
    }
    case Profile::kIndepHigh:
      return {r(11, 100), r(12, 100), r(13, 100), r(14, 100)};
    case Profile::kWide8: {
      const int32_t c4 = r(21, 100'000);
      const int32_t quarter = r(23, 4);
      return {c4 / 10'000, c4 / 1000, c4 / 100, c4, r(22, 5), quarter, (quarter * 3) + r(24, 3),
              r(25, 50)};
    }
  }
  return {};
}

// k1..kK and vi, all int32, in kBatch-row batches; built once per profile.
const arrow::RecordBatchVector& Input(Profile p) {
  static std::array<std::optional<arrow::RecordBatchVector>, 5> made;
  auto& slot = made.at(static_cast<std::size_t>(p));
  if (slot.has_value()) {
    return *slot;
  }
  const int keys = KeysOf(p);
  arrow::FieldVector fields;
  for (int k = 1; k <= keys; ++k) {
    fields.push_back(arrow::field("k" + std::to_string(k), arrow::int32()));
  }
  fields.push_back(arrow::field("vi", arrow::int32()));
  const auto schema = arrow::schema(fields);
  arrow::RecordBatchVector batches;
  const int64_t rows = Rows();
  for (int64_t start = 0; start < rows; start += kBatch) {
    const int64_t n = std::min(kBatch, rows - start);
    std::vector<arrow::Int32Builder> builders(Narrow<std::size_t>(keys + 1));
    for (auto& b : builders) {
      if (!b.Reserve(n).ok()) {
        std::abort();
      }
    }
    for (int64_t i = start; i < start + n; ++i) {
      const auto row = KeysOfRow(p, static_cast<uint64_t>(i));
      for (int k = 0; k < keys; ++k) {
        builders[Narrow<std::size_t>(k)].UnsafeAppend(row.at(Narrow<std::size_t>(k)));
      }
      builders.back().UnsafeAppend(
          static_cast<int32_t>(Mix((static_cast<uint64_t>(i) * 7919U) + 101) % 1000U));
    }
    arrow::ArrayVector columns;
    for (auto& b : builders) {
      columns.push_back(b.Finish().ValueOrDie());
    }
    batches.push_back(arrow::RecordBatch::Make(schema, n, std::move(columns)));
  }
  slot = std::move(batches);
  return *slot;
}

class Source final : public exec::Operator {
 public:
  explicit Source(const arrow::RecordBatchVector& batches)
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

plan::BoundColumn Column(int index, plan::LogicalType type) {
  return {.index = index, .name = "c" + std::to_string(index), .type = type};
}

std::vector<plan::BoundColumn> Prefix(int keys) {
  std::vector<plan::BoundColumn> out;
  for (int k = 0; k < keys; ++k) {
    out.push_back(Column(k, plan::LogicalType::kInteger));
  }
  return out;
}

// COUNT(*), SUM(vi), MIN(vi), MAX(vi) of the input, vi at `value`.
std::vector<plan::AggregateCall> InputCalls(int value) {
  const auto vi = Column(value, plan::LogicalType::kInteger);
  return {{.kind = plan::AggKind::kCountStar, .type = plan::LogicalType::kBigInt},
          {.kind = plan::AggKind::kSum, .arg = vi, .type = plan::LogicalType::kHugeInt},
          {.kind = plan::AggKind::kMin, .arg = vi, .type = plan::LogicalType::kInteger},
          {.kind = plan::AggKind::kMax, .arg = vi, .type = plan::LogicalType::kInteger}};
}

// The same four, merged from a grouped result whose states start at column `first`.
std::vector<plan::AggregateCall> MergeCalls(int first) {
  return {{.kind = plan::AggKind::kSumCounts,
           .arg = Column(first, plan::LogicalType::kBigInt),
           .type = plan::LogicalType::kBigInt},
          {.kind = plan::AggKind::kSum,
           .arg = Column(first + 1, plan::LogicalType::kHugeInt),
           .type = plan::LogicalType::kHugeInt},
          {.kind = plan::AggKind::kMin,
           .arg = Column(first + 2, plan::LogicalType::kInteger),
           .type = plan::LogicalType::kInteger},
          {.kind = plan::AggKind::kMax,
           .arg = Column(first + 3, plan::LogicalType::kInteger),
           .type = plan::LogicalType::kInteger}};
}

// Parts of two batches (128Ki rows), as the engine aggregates a Parquet row group per part.
constexpr std::size_t kBatchesPerPart = 2;

std::vector<arrow::RecordBatchVector> PartsOf(const arrow::RecordBatchVector& input) {
  std::vector<arrow::RecordBatchVector> parts;
  for (std::size_t i = 0; i < input.size(); i += kBatchesPerPart) {
    parts.emplace_back(
        input.begin() + static_cast<std::ptrdiff_t>(i),
        input.begin() + static_cast<std::ptrdiff_t>(std::min(input.size(), i + kBatchesPerPart)));
  }
  return parts;
}

// Where the time went: the per-part phase (early aggregation and anything derived from it, which
// runs in parallel over parts), the merges (in parallel over partitions and sets), and the work
// that must wait for the finest set's final merge.
struct Run {
  int64_t groups = 0;          // final groups, every set
  int64_t partial_groups = 0;  // groups of the per-part tables, every set
  double part_ms = 0;
  double merge_ms = 0;
  double after_ms = 0;
};

using Clock = std::chrono::steady_clock;

double MsSince(Clock::time_point start) {
  return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

// One GROUP BY over `in`; its groups as batches.
arrow::Result<arrow::RecordBatchVector> Aggregate(const arrow::RecordBatchVector& in,
                                                  std::vector<plan::BoundColumn> keys,
                                                  std::vector<plan::AggregateCall> calls,
                                                  exec::ExecContext& ctx) {
  exec::GroupAggregateOperator op(std::make_unique<Source>(in), std::move(keys), std::move(calls));
  ARROW_ASSIGN_OR_RAISE(const auto table, exec::Drain(op, ctx));
  arrow::TableBatchReader reader(*table);
  return reader.ToRecordBatches();
}

int64_t RowsOf(const arrow::RecordBatchVector& batches) {
  int64_t rows = 0;
  for (const auto& b : batches) {
    rows += b->num_rows();
  }
  return rows;
}

void Append(arrow::RecordBatchVector& to, const arrow::RecordBatchVector& from) {
  to.insert(to.end(), from.begin(), from.end());
}

// The final merge of every set's partial groups: set `level` groups by the first `level` keys.
arrow::Status MergeSets(std::vector<arrow::RecordBatchVector>& partials, exec::ExecContext& ctx,
                        Run& run) {
  const auto start = Clock::now();
  for (std::size_t level = 0; level < partials.size(); ++level) {
    if (partials[level].empty()) {
      continue;
    }
    const int keys = static_cast<int>(level);
    ARROW_ASSIGN_OR_RAISE(const auto merged,
                          Aggregate(partials[level], Prefix(keys), MergeCalls(keys), ctx));
    run.groups += RowsOf(merged);
  }
  run.merge_ms += MsSince(start);
  return arrow::Status::OK();
}

enum class Way : std::uint8_t {
  kFinest,        // the finest set alone
  kIndep,         // every set's partial table fed from the part's rows (DuckDB's one-pass shape)
  kExpand,        // one partial table over the part's rows copied once per set, with a set id
  kStarPartial,   // per part: the finest partial, then every other set's partial from it
  kChainPartial,  // per part: the finest partial, then each set's partial from the next finer one
  kStarFinal,     // the finest set merged first, then every other set from its final groups
  kChainFinal,    // the finest set merged first, then each set from the next finer set's groups
};

arrow::Status Answer(Profile p, Way way, exec::ExecContext& ctx, Run& run) {
  const auto parts = PartsOf(Input(p));
  const int keys = KeysOf(p);
  // partials[level]: the per-part groups of the set that groups by the first `level` keys.
  std::vector<arrow::RecordBatchVector> partials(static_cast<std::size_t>(keys) + 1);
  const auto at = [&partials](int level) -> arrow::RecordBatchVector& {
    return partials[static_cast<std::size_t>(level)];
  };
  const auto part_start = Clock::now();
  switch (way) {
    case Way::kFinest:
    case Way::kStarFinal:
    case Way::kChainFinal:
      for (const auto& part : parts) {
        ARROW_ASSIGN_OR_RAISE(const auto f, Aggregate(part, Prefix(keys), InputCalls(keys), ctx));
        Append(at(keys), f);
      }
      break;
    case Way::kIndep:
      for (const auto& part : parts) {
        for (int level = keys; level >= 0; --level) {
          ARROW_ASSIGN_OR_RAISE(const auto g,
                                Aggregate(part, Prefix(level), InputCalls(keys), ctx));
          Append(at(level), g);
        }
      }
      break;
    case Way::kExpand: {
      arrow::FieldVector fields;
      for (int k = 1; k <= keys; ++k) {
        fields.push_back(arrow::field("k" + std::to_string(k), arrow::int32()));
      }
      fields.push_back(arrow::field("set", arrow::int32()));
      fields.push_back(arrow::field("vi", arrow::int32()));
      const auto schema = arrow::schema(fields);
      arrow::RecordBatchVector all;
      for (const auto& part : parts) {
        arrow::RecordBatchVector expanded;
        for (const auto& batch : part) {
          ARROW_ASSIGN_OR_RAISE(const auto nulls,
                                arrow::MakeArrayOfNull(arrow::int32(), batch->num_rows()));
          for (int level = keys; level >= 0; --level) {
            arrow::ArrayVector columns;
            for (int k = 0; k < keys; ++k) {
              columns.push_back(k < level ? batch->column(k) : nulls);
            }
            ARROW_ASSIGN_OR_RAISE(
                auto id, arrow::MakeArrayFromScalar(arrow::Int32Scalar(level), batch->num_rows()));
            columns.push_back(std::move(id));
            columns.push_back(batch->column(keys));
            expanded.push_back(arrow::RecordBatch::Make(schema, batch->num_rows(), columns));
          }
        }
        ARROW_ASSIGN_OR_RAISE(const auto g,
                              Aggregate(expanded, Prefix(keys + 1), InputCalls(keys + 1), ctx));
        Append(all, g);
      }
      run.part_ms += MsSince(part_start);
      run.partial_groups += RowsOf(all);
      const auto merge_start = Clock::now();
      ARROW_ASSIGN_OR_RAISE(const auto merged,
                            Aggregate(all, Prefix(keys + 1), MergeCalls(keys + 1), ctx));
      run.groups += RowsOf(merged);
      run.merge_ms += MsSince(merge_start);
      return arrow::Status::OK();
    }
    case Way::kStarPartial:
    case Way::kChainPartial:
      for (const auto& part : parts) {
        ARROW_ASSIGN_OR_RAISE(auto finer, Aggregate(part, Prefix(keys), InputCalls(keys), ctx));
        Append(at(keys), finer);
        const arrow::RecordBatchVector finest = finer;
        for (int level = keys - 1; level >= 0; --level) {
          const bool star = way == Way::kStarPartial;
          ARROW_ASSIGN_OR_RAISE(auto g, Aggregate(star ? finest : finer, Prefix(level),
                                                  MergeCalls(star ? keys : level + 1), ctx));
          Append(at(level), g);
          finer = std::move(g);
        }
      }
      break;
  }
  run.part_ms += MsSince(part_start);
  for (const auto& level : partials) {
    run.partial_groups += RowsOf(level);
  }
  if (way != Way::kStarFinal && way != Way::kChainFinal) {
    return MergeSets(partials, ctx, run);
  }
  // From the final finest groups: merge them first, then derive every other set from them.
  const auto merge_start = Clock::now();
  ARROW_ASSIGN_OR_RAISE(auto finer, Aggregate(at(keys), Prefix(keys), MergeCalls(keys), ctx));
  run.groups += RowsOf(finer);
  run.merge_ms += MsSince(merge_start);
  const auto after_start = Clock::now();
  const arrow::RecordBatchVector finest = finer;
  for (int level = keys - 1; level >= 0; --level) {
    const bool star = way == Way::kStarFinal;
    ARROW_ASSIGN_OR_RAISE(auto g, Aggregate(star ? finest : finer, Prefix(level),
                                            MergeCalls(star ? keys : level + 1), ctx));
    run.groups += RowsOf(g);
    finer = std::move(g);
  }
  run.after_ms += MsSince(after_start);
  return arrow::Status::OK();
}

void BM_RollUp(benchmark::State& state, Profile p, Way way) {
  (void)Input(p);  // built outside the timed loop
  Run last;
  int64_t peak = 0;
  double part_ms = 0;
  double merge_ms = 0;
  double after_ms = 0;
  for (auto _ : state) {
    exec::MemoryBudget budget(std::nullopt);
    exec::ExecContext ctx{.pool = &budget, .budget = &budget};
    Run run;
    const auto status = Answer(p, way, ctx, run);
    if (!status.ok()) {
      state.SkipWithError(status.ToString());
      return;
    }
    part_ms += run.part_ms;
    merge_ms += run.merge_ms;
    after_ms += run.after_ms;
    last = run;
    peak = budget.max_memory();
  }
  const auto n = static_cast<double>(state.iterations());
  state.counters["groups"] = static_cast<double>(last.groups);
  state.counters["partials"] = static_cast<double>(last.partial_groups);
  state.counters["part_ms"] = part_ms / n;
  state.counters["merge_ms"] = merge_ms / n;
  state.counters["after_ms"] = after_ms / n;
  state.counters["peak_MiB"] = static_cast<double>(peak) / (1024.0 * 1024.0);
}

// ---- the engine's operators (ADR 0038's RU2 update) ----

// ROLLUP of every key of `p` through the engine's parallel operators, on `threads` threads (an
// executor even for one, as the engine runs), over parts of kBatchesPerPart batches: the finest set
// alone (PartGroupAggregateOperator), the two-level PartGroupingSetsOperator, and the same
// operator with every set aggregated from the input (`per_set`, DuckDB's design).
enum class Operated : std::uint8_t { kFinest, kTwoLevel, kPerSet };

arrow::Result<int64_t> Operate(Profile p, Operated mode, int threads, exec::MemoryBudget& budget) {
  static std::array<std::optional<std::vector<arrow::RecordBatchVector>>, 5> made;
  auto& slot = made.at(static_cast<std::size_t>(p));
  if (!slot.has_value()) {
    slot = PartsOf(Input(p));
  }
  const std::vector<arrow::RecordBatchVector>& parts = *slot;
  const int keys = KeysOf(p);
  const exec::PartPipeline pipeline =
      [&parts](int64_t part) -> arrow::Result<std::unique_ptr<exec::Operator>> {
    return std::make_unique<Source>(parts.at(static_cast<std::size_t>(part)));
  };
  const auto count = static_cast<int64_t>(parts.size());
  std::unique_ptr<exec::Operator> op;
  if (mode == Operated::kFinest) {
    const exec::GroupAggregateOperator serial(std::make_unique<Source>(parts.front()), Prefix(keys),
                                              InputCalls(keys));
    op = std::make_unique<exec::PartGroupAggregateOperator>(
        pipeline, count, keys + 1, Prefix(keys), InputCalls(keys), serial.output_schema());
  } else {
    std::vector<std::vector<std::size_t>> sets;
    for (int level = keys; level >= 0; --level) {
      std::vector<std::size_t> set(static_cast<std::size_t>(level));
      std::ranges::iota(set, std::size_t{0});
      sets.push_back(std::move(set));
    }
    op = std::make_unique<exec::PartGroupingSetsOperator>(
        pipeline, count, keys + 1, Prefix(keys), std::move(sets), InputCalls(keys),
        std::vector<plan::GroupingCall>{}, mode == Operated::kPerSet);
  }
  ARROW_ASSIGN_OR_RAISE(auto pool, arrow::internal::ThreadPool::Make(threads));
  exec::ExecContext ctx{
      .pool = &budget, .executor = pool.get(), .threads = threads, .budget = &budget};
  ARROW_RETURN_NOT_OK(op->Open(ctx));
  int64_t rows = 0;
  while (true) {
    ARROW_ASSIGN_OR_RAISE(const exec::Batch batch, op->Next());
    if (batch.end()) {
      break;
    }
    rows += batch.data->num_rows();
  }
  ARROW_RETURN_NOT_OK(op->Close());
  return rows;
}

void BM_Operator(benchmark::State& state, Profile p, Operated mode) {
  (void)Input(p);  // built outside the timed loop
  const auto threads = static_cast<int>(state.range(0));
  int64_t rows = 0;
  int64_t peak = 0;
  for (auto _ : state) {
    exec::MemoryBudget budget(std::nullopt);
    const auto result = Operate(p, mode, threads, budget);
    if (!result.ok()) {
      state.SkipWithError(result.status().ToString());
      return;
    }
    rows = *result;
    peak = budget.max_memory();
  }
  state.counters["rows"] = static_cast<double>(rows);
  state.counters["peak_MiB"] = static_cast<double>(peak) / (1024.0 * 1024.0);
}

#define ANTB1_OPERATOR(profile, mode)                                                      \
  BENCHMARK_CAPTURE(BM_Operator, profile##_##mode, Profile::k##profile, Operated::k##mode) \
      ->Unit(benchmark::kMillisecond)                                                      \
      ->UseRealTime()                                                                      \
      ->Iterations(3)                                                                      \
      ->Arg(1)                                                                             \
      ->Arg(14)

#define ANTB1_OPERATOR_ALL(profile)  \
  ANTB1_OPERATOR(profile, Finest);   \
  ANTB1_OPERATOR(profile, TwoLevel); \
  ANTB1_OPERATOR(profile, PerSet)

ANTB1_OPERATOR_ALL(HierLow);
ANTB1_OPERATOR_ALL(HierMid);
ANTB1_OPERATOR_ALL(HierHigh);
ANTB1_OPERATOR_ALL(IndepHigh);
ANTB1_OPERATOR_ALL(Wide8);

#define ANTB1_ROLLUP(profile, way)                                                \
  BENCHMARK_CAPTURE(BM_RollUp, profile##_##way, Profile::k##profile, Way::k##way) \
      ->Unit(benchmark::kMillisecond)                                             \
      ->UseRealTime()                                                             \
      ->Iterations(3)

#define ANTB1_ROLLUP_ALL(profile)      \
  ANTB1_ROLLUP(profile, Finest);       \
  ANTB1_ROLLUP(profile, Indep);        \
  ANTB1_ROLLUP(profile, Expand);       \
  ANTB1_ROLLUP(profile, StarPartial);  \
  ANTB1_ROLLUP(profile, ChainPartial); \
  ANTB1_ROLLUP(profile, StarFinal);    \
  ANTB1_ROLLUP(profile, ChainFinal)

ANTB1_ROLLUP_ALL(HierLow);
ANTB1_ROLLUP_ALL(HierMid);
ANTB1_ROLLUP_ALL(HierHigh);
ANTB1_ROLLUP_ALL(IndepHigh);
ANTB1_ROLLUP_ALL(Wide8);

}  // namespace
}  // namespace antb1::bench

BENCHMARK_MAIN();
