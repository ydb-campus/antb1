#include "variance.h"

#include <cmath>
#include <cstdint>
#include <memory>
#include <optional>

#include <arrow/api.h>

#include "antb1/plan/logical_plan.h"

#include "decimal.h"

namespace antb1::exec {
namespace {

template <class ArrowType>
arrow::Result<std::shared_ptr<arrow::DoubleArray>> IntegersToDoubles(const arrow::Array& values,
                                                                     arrow::MemoryPool* pool) {
  const auto& typed = static_cast<const arrow::NumericArray<ArrowType>&>(values);
  arrow::DoubleBuilder builder(pool);
  ARROW_RETURN_NOT_OK(builder.Reserve(values.length()));
  for (int64_t i = 0; i < values.length(); ++i) {
    if (values.IsNull(i)) {
      builder.UnsafeAppendNull();
    } else {
      builder.UnsafeAppend(static_cast<double>(typed.Value(i)));
    }
  }
  std::shared_ptr<arrow::DoubleArray> out;
  ARROW_RETURN_NOT_OK(builder.Finish(&out));
  return out;
}

}  // namespace

bool IsVarianceKind(plan::AggKind kind) {
  return kind == plan::AggKind::kStddevSamp || kind == plan::AggKind::kStddevPop ||
         kind == plan::AggKind::kVarSamp || kind == plan::AggKind::kVarPop;
}

arrow::Result<std::optional<double>> FinalizeVariance(plan::AggKind kind, const Moments& state) {
  const bool sample = kind == plan::AggKind::kStddevSamp || kind == plan::AggKind::kVarSamp;
  if (state.count == 0 || (sample && state.count == 1)) {
    return std::nullopt;
  }
  double result = 0;
  if (state.count > 1) {
    const auto divisor = static_cast<double>(sample ? state.count - 1 : state.count);
    result = state.dsquared / divisor;
    if (kind == plan::AggKind::kStddevSamp || kind == plan::AggKind::kStddevPop) {
      result = std::sqrt(result);
    }
  }
  if (!std::isfinite(result)) {
    switch (kind) {
      case plan::AggKind::kStddevSamp:
        return arrow::Status::ExecutionError("STDDEV_SAMP is out of range!");
      case plan::AggKind::kStddevPop:
        return arrow::Status::ExecutionError("STDDEV_POP is out of range!");
      case plan::AggKind::kVarSamp:
        return arrow::Status::ExecutionError("VARSAMP is out of range!");
      default:
        return arrow::Status::ExecutionError("VARPOP is out of range!");
    }
  }
  return result;
}

arrow::Result<std::shared_ptr<arrow::DoubleArray>> ToDuckDbDoubles(const arrow::Array& values,
                                                                   arrow::MemoryPool* pool) {
  switch (values.type_id()) {
    case arrow::Type::DOUBLE:
      return std::static_pointer_cast<arrow::DoubleArray>(arrow::MakeArray(values.data()));
    case arrow::Type::INT16:
      return IntegersToDoubles<arrow::Int16Type>(values, pool);
    case arrow::Type::INT32:
      return IntegersToDoubles<arrow::Int32Type>(values, pool);
    case arrow::Type::INT64:
      return IntegersToDoubles<arrow::Int64Type>(values, pool);
    case arrow::Type::UINT16:
      return IntegersToDoubles<arrow::UInt16Type>(values, pool);
    case arrow::Type::DECIMAL64:
    case arrow::Type::DECIMAL128: {
      ARROW_ASSIGN_OR_RAISE(const auto doubles, DecimalToDouble(values, pool));
      return std::static_pointer_cast<arrow::DoubleArray>(doubles);
    }
    default:
      return arrow::Status::Invalid("no DOUBLE of ", values.type()->ToString(),
                                    " for a variance aggregate");
  }
}

}  // namespace antb1::exec
