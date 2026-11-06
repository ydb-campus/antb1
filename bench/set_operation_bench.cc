// Prototype measurements for set operations (ADR 0039's X4s update); not engine code. The work of
// a three-input INTERSECT, a two-input EXCEPT and a three-input UNION over in-memory batches, on
// one thread, two ways (Way below): the engine's SetOperationOperator, and ADR 0036's sketch, a
// UnionAllOperator over the inputs with a marker column per input (1 in its own input's rows, NULL
// in the others'), a GroupAggregateOperator keyed on every column with a COUNT of each marker, and
// the markers' condition checked on its groups (the Filter's work, done by the driver's loop; a
// UNION's sketch is the GROUP BY alone, without markers). Both ways read the same batches, which
// hold the markers already, so the sketch's projection is not timed. Each input has
// SETOPS_ROWS rows (default 4,000,000) of two key columns, an INTEGER and a BIGINT that determines
// it, drawn from a key range of its own: input i's keys are in [i * D / 4, i * D / 4 + D), so all
// three overlap in a quarter of the range. D is 10,000 (few distinct rows) or SETOPS_ROWS (most
// rows distinct).

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
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

#include "set_operation.h"  // src/exec: the operators, timed directly
#include "union_all.h"

namespace antb1::bench {
namespace {

constexpr int64_t kBatch = int64_t{64} * 1024;
constexpr std::size_t kInputs = 3;
constexpr int kKeys = 2;

uint64_t Mix(uint64_t x) {
  x += 0x9E3779B97F4A7C15ULL;
  x = (x ^ (x >> 30U)) * 0xBF58476D1CE4E5B9ULL;
  x = (x ^ (x >> 27U)) * 0x94D049BB133111EBULL;
  return x ^ (x >> 31U);
}

int64_t Rows() {
  const char* rows = std::getenv("SETOPS_ROWS");
  return rows == nullptr ? int64_t{4'000'000} : std::atoll(rows);
}

enum class Profile : std::uint8_t { kFew, kMany };

uint64_t RangeOf(Profile p) {
  return p == Profile::kFew ? uint64_t{10'000} : static_cast<uint64_t>(Rows());
}

const std::array<plan::LogicalType, kKeys + kInputs> kTypes = {
    plan::LogicalType::kInteger, plan::LogicalType::kBigInt, plan::LogicalType::kInteger,
    plan::LogicalType::kInteger, plan::LogicalType::kInteger};

// k1, k2 and the markers m0..m2 of input `input`, in kBatch-row batches; built once per profile.
const arrow::RecordBatchVector& Input(Profile p, std::size_t input) {
  static std::array<std::array<std::optional<arrow::RecordBatchVector>, kInputs>, 2> made;
  auto& slot = made.at(static_cast<std::size_t>(p)).at(input);
  if (slot.has_value()) {
    return *slot;
  }
  const auto schema =
      arrow::schema({arrow::field("k1", arrow::int32()), arrow::field("k2", arrow::int64()),
                     arrow::field("m0", arrow::int32()), arrow::field("m1", arrow::int32()),
                     arrow::field("m2", arrow::int32())});
  const uint64_t range = RangeOf(p);
  const uint64_t offset = input * range / 4;
  arrow::RecordBatchVector batches;
  const int64_t rows = Rows();
  for (int64_t start = 0; start < rows; start += kBatch) {
    const int64_t n = std::min(kBatch, rows - start);
    arrow::Int32Builder k1;
    arrow::Int64Builder k2;
    std::array<arrow::Int32Builder, kInputs> markers;
    if (!k1.Reserve(n).ok() || !k2.Reserve(n).ok()) {
      std::abort();
    }
    for (auto& m : markers) {
      if (!m.Reserve(n).ok()) {
        std::abort();
      }
    }
    for (int64_t i = start; i < start + n; ++i) {
      const uint64_t key = offset + (Mix((static_cast<uint64_t>(i) * 7919U) + input) % range);
      k1.UnsafeAppend(static_cast<int32_t>(key / 1000));
      k2.UnsafeAppend(static_cast<int64_t>(key));
      for (std::size_t m = 0; m < kInputs; ++m) {
        if (m == input) {
          markers.at(m).UnsafeAppend(1);
        } else {
          markers.at(m).UnsafeAppendNull();
        }
      }
    }
    arrow::ArrayVector columns{k1.Finish().ValueOrDie(), k2.Finish().ValueOrDie()};
    for (auto& m : markers) {
      columns.push_back(m.Finish().ValueOrDie());
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

// 0 INTERSECT 1 INTERSECT 2, 0 EXCEPT 1, 0 UNION 1 UNION 2.
enum class Operation : std::uint8_t { kIntersect, kExcept, kUnion };

std::size_t InputsOf(Operation op) { return op == Operation::kExcept ? 2 : kInputs; }

std::vector<plan::SetTerm> FormulaOf(Operation op) {
  std::vector<plan::SetTerm> formula;
  const std::size_t inputs = InputsOf(op);
  for (std::size_t k = 0; k < inputs; ++k) {
    formula.push_back({.kind = plan::SetTermKind::kInput, .input = k, .operands = 0});
  }
  plan::SetTermKind kind = plan::SetTermKind::kUnion;
  if (op == Operation::kIntersect) {
    kind = plan::SetTermKind::kIntersect;
  } else if (op == Operation::kExcept) {
    kind = plan::SetTermKind::kExcept;
  }
  formula.push_back({.kind = kind, .input = 0, .operands = inputs});
  return formula;
}

enum class Way : std::uint8_t { kNode, kSketch };

// Whether a sketch's group, with counts `counts` of the markers, is in the result.
bool Kept(Operation op, const std::vector<const arrow::Int64Array*>& counts, int64_t row) {
  switch (op) {
    case Operation::kIntersect:
      for (const auto* c : counts) {
        if (c->Value(row) == 0) {
          return false;
        }
      }
      return true;
    case Operation::kExcept:
      return counts.at(0)->Value(row) > 0 && counts.at(1)->Value(row) == 0;
    case Operation::kUnion:
      return true;
  }
  return false;
}

// The operation's rows, the given way, on one thread.
arrow::Result<int64_t> Operate(Profile p, Operation op, Way way, exec::MemoryBudget& budget) {
  const std::size_t inputs = InputsOf(op);
  std::vector<std::unique_ptr<exec::Operator>> sources;
  for (std::size_t k = 0; k < inputs; ++k) {
    sources.push_back(std::make_unique<Source>(Input(p, k)));
  }
  const std::vector<plan::LogicalType> key_types(kTypes.begin(), kTypes.begin() + kKeys);
  std::unique_ptr<exec::Operator> top;
  if (way == Way::kNode) {
    std::vector<std::vector<int>> columns(inputs, std::vector<int>{0, 1});
    ARROW_ASSIGN_OR_RAISE(
        top, exec::SetOperationOperator::Make(std::move(sources), std::move(columns), key_types,
                                              FormulaOf(op)));
  } else {
    const bool markers = op != Operation::kUnion;
    std::vector<int> read{0, 1};
    std::vector<plan::LogicalType> types = key_types;
    if (markers) {
      for (std::size_t m = 0; m < inputs; ++m) {
        read.push_back(kKeys + Narrow<int>(m));
        types.push_back(plan::LogicalType::kInteger);
      }
    }
    std::vector<std::vector<int>> columns(inputs, read);
    ARROW_ASSIGN_OR_RAISE(
        auto all, exec::UnionAllOperator::Make(std::move(sources), std::move(columns), types));
    std::vector<plan::AggregateCall> calls;
    if (markers) {
      for (std::size_t m = 0; m < inputs; ++m) {
        calls.push_back({.kind = plan::AggKind::kCount,
                         .arg = Column(kKeys + Narrow<int>(m), plan::LogicalType::kInteger),
                         .type = plan::LogicalType::kBigInt});
      }
    }
    top = std::make_unique<exec::GroupAggregateOperator>(
        std::move(all),
        std::vector<plan::BoundColumn>{Column(0, plan::LogicalType::kInteger),
                                       Column(1, plan::LogicalType::kBigInt)},
        std::move(calls));
  }
  ARROW_ASSIGN_OR_RAISE(auto pool, arrow::internal::ThreadPool::Make(1));
  exec::ExecContext ctx{.pool = &budget, .executor = pool.get(), .threads = 1, .budget = &budget};
  ARROW_RETURN_NOT_OK(top->Open(ctx));
  int64_t rows = 0;
  while (true) {
    ARROW_ASSIGN_OR_RAISE(const exec::Batch batch, top->Next());
    if (batch.end()) {
      break;
    }
    if (way == Way::kNode || op == Operation::kUnion) {
      rows += batch.selected_rows();
      continue;
    }
    std::vector<const arrow::Int64Array*> counts;
    for (std::size_t m = 0; m < inputs; ++m) {
      counts.push_back(
          static_cast<const arrow::Int64Array*>(batch.data->column(kKeys + Narrow<int>(m)).get()));
    }
    for (int64_t row = 0; row < batch.data->num_rows(); ++row) {
      if ((batch.selection == nullptr || batch.selection->Value(row)) && Kept(op, counts, row)) {
        ++rows;
      }
    }
  }
  ARROW_RETURN_NOT_OK(top->Close());
  return rows;
}

void BM_SetOperation(benchmark::State& state, Profile p, Operation op, Way way) {
  for (std::size_t k = 0; k < InputsOf(op); ++k) {
    (void)Input(p, k);  // built outside the timed loop
  }
  int64_t rows = 0;
  int64_t peak = 0;
  for (auto _ : state) {
    exec::MemoryBudget budget(std::nullopt);
    const auto result = Operate(p, op, way, budget);
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

#define ANTB1_SETOP(profile, operation, way)                                             \
  BENCHMARK_CAPTURE(BM_SetOperation, profile##_##operation##_##way, Profile::k##profile, \
                    Operation::k##operation, Way::k##way)                                \
      ->Unit(benchmark::kMillisecond)                                                    \
      ->UseRealTime()                                                                    \
      ->Iterations(3)

#define ANTB1_SETOP_BOTH(profile, operation) \
  ANTB1_SETOP(profile, operation, Node);     \
  ANTB1_SETOP(profile, operation, Sketch)

ANTB1_SETOP_BOTH(Few, Intersect);
ANTB1_SETOP_BOTH(Few, Except);
ANTB1_SETOP_BOTH(Few, Union);
ANTB1_SETOP_BOTH(Many, Intersect);
ANTB1_SETOP_BOTH(Many, Except);
ANTB1_SETOP_BOTH(Many, Union);

}  // namespace
}  // namespace antb1::bench

BENCHMARK_MAIN();
