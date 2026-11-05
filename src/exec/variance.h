#pragma once

#include <cstdint>
#include <memory>
#include <optional>

#include <arrow/api.h>

#include "antb1/plan/logical_plan.h"

// The variance family (ADR 0037's AG2) as DuckDB 1.5.6 computes it, STDDevBaseOperation: a running
// count, mean and sum of squared deviations (Welford's update per value), partial states combined
// by Chan's formula, and one finalization per function. The arithmetic follows DuckDB's operation
// for operation, so a single-threaded run in the same order gives DuckDB's bits; the merge order of
// parallel runs differs between the engines, which the oracle's DOUBLE tolerance covers.

namespace antb1::exec {

struct Moments {
  std::uint64_t count = 0;
  double mean = 0;
  double dsquared = 0;
};

// DuckDB's Operation: one value into the running state.
inline void AddValue(Moments& state, double input) {
  ++state.count;
  const double mean_differential = (input - state.mean) / static_cast<double>(state.count);
  const double new_mean = state.mean + mean_differential;
  const double dsquared_increment = (input - new_mean) * (input - state.mean);
  state.dsquared += dsquared_increment;
  state.mean = new_mean;
}

// DuckDB's Combine: `source` folded into `target`.
inline void Combine(const Moments& source, Moments& target) {
  if (target.count == 0) {
    target = source;
    return;
  }
  if (source.count == 0) {
    return;
  }
  const std::uint64_t count = target.count + source.count;
  const auto target_count = static_cast<double>(target.count);
  const auto source_count = static_cast<double>(source.count);
  const auto total_count = static_cast<double>(count);
  const double mean = ((source_count * source.mean) + (target_count * target.mean)) / total_count;
  const double delta = source.mean - target.mean;
  target.dsquared = source.dsquared + target.dsquared +
                    (delta * delta * source_count * target_count / total_count);
  target.mean = mean;
  target.count = count;
}

// Whether `kind` is one of the family: kStddevSamp, kStddevPop, kVarSamp or kVarPop.
bool IsVarianceKind(plan::AggKind kind);

// The result of `kind` over `state`: std::nullopt for NULL (no value; one for the sample forms),
// or DuckDB's OutOfRange error (an ExecutionError) when the result is not finite.
arrow::Result<std::optional<double>> FinalizeVariance(plan::AggKind kind, const Moments& state);

// `values` (an integer, HUGEINT, DECIMAL or DOUBLE column) as DuckDB casts it to DOUBLE, NULLs
// kept: an integer as the nearest double, a HUGEINT or DECIMAL by DuckDbDecimalToDouble.
arrow::Result<std::shared_ptr<arrow::DoubleArray>> ToDuckDbDoubles(const arrow::Array& values,
                                                                   arrow::MemoryPool* pool);

}  // namespace antb1::exec
