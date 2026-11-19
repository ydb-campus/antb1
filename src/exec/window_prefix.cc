#include "window_prefix.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <type_traits>

#include <arrow/api.h>

#include "antb1/common/narrow.h"
#include "antb1/plan/literal.h"
#include "antb1/plan/types.h"

#include "temporal_average.h"

namespace antb1::exec {
namespace {

using RowRef = SortBuffer::RowRef;

template <class ArrowType>
auto ValueAt(const arrow::Array& array, int64_t row) {
  const auto& values = static_cast<const arrow::TypeTraits<ArrowType>::ArrayType&>(array);
  if constexpr (std::is_same_v<ArrowType, arrow::Decimal128Type>) {
    const arrow::Decimal128 value(values.GetValue(row));
    return static_cast<Int128>(
        (static_cast<UInt128>(static_cast<uint64_t>(value.high_bits())) << 64U) | value.low_bits());
  } else if constexpr (std::is_same_v<ArrowType, arrow::Decimal64Type>) {
    return Int128{arrow::Decimal64(values.GetValue(row)).value()};
  } else if constexpr (std::is_same_v<ArrowType, arrow::BinaryType>) {
    return values.GetView(row);
  } else {
    return values.Value(row);
  }
}

arrow::Decimal128 DecimalOf(Int128 value) {
  const auto bits = static_cast<UInt128>(value);
  return {static_cast<int64_t>(bits >> 64U), static_cast<uint64_t>(bits)};
}

template <class Builder, class Step>
arrow::Result<std::shared_ptr<arrow::Array>> PrefixLoop(std::span<const RowRef> rows,
                                                        std::span<const std::uint8_t> starts,
                                                        WindowPrefixState& state, Builder& builder,
                                                        const Step& step) {
  ARROW_RETURN_NOT_OK(builder.Reserve(Narrow<int64_t>(rows.size())));
  for (std::size_t i = 0; i < rows.size(); ++i) {
    if (starts[i] != 0) {
      state = {};
    }
    ARROW_RETURN_NOT_OK(step(rows[i]));
  }
  return builder.Finish();
}

template <class ArrowType>
arrow::Result<std::shared_ptr<arrow::Array>> Extremes(const plan::WindowCall& call,
                                                      const arrow::RecordBatchVector& input,
                                                      std::span<const RowRef> rows,
                                                      std::span<const std::uint8_t> starts,
                                                      WindowPrefixState& state,
                                                      arrow::MemoryPool* pool) {
  typename arrow::TypeTraits<ArrowType>::BuilderType builder(plan::ToArrow(call.type), pool);
  const int column = call.arguments.front().index;
  const auto get = [&](RowRef row) {
    return ValueAt<ArrowType>(*input[row.chunk]->column(column), row.row);
  };
  const bool minimum = call.aggregate == plan::AggKind::kMin;
  return PrefixLoop(rows, starts, state, builder, [&](RowRef row) -> arrow::Status {
    if (input[row.chunk]->column(column)->IsValid(row.row)) {
      bool replace = !state.best;
      if (state.best) {
        const auto value = get(row);
        const auto best = get(*state.best);
        if constexpr (std::is_same_v<ArrowType, arrow::DoubleType>) {
          replace =
              !std::isnan(value) && (std::isnan(best) || (minimum ? value < best : best < value));
        } else {
          replace = minimum ? value < best : best < value;
        }
      }
      if (replace) {
        state.best = row;
      }
    }
    if (!state.best) {
      return builder.AppendNull();
    }
    if constexpr (std::is_same_v<ArrowType, arrow::Decimal128Type>) {
      return builder.Append(DecimalOf(get(*state.best)));
    } else if constexpr (std::is_same_v<ArrowType, arrow::Decimal64Type>) {
      return builder.Append(arrow::Decimal64(static_cast<int64_t>(get(*state.best))));
    } else {
      return builder.Append(get(*state.best));
    }
  });
}

template <class ArrowType, class Builder, class Finalize>
arrow::Result<std::shared_ptr<arrow::Array>> Sums(const plan::WindowCall& call,
                                                  const arrow::RecordBatchVector& input,
                                                  std::span<const RowRef> rows,
                                                  std::span<const std::uint8_t> starts,
                                                  WindowPrefixState& state, Builder& builder,
                                                  const Finalize& finalize) {
  const int column = call.arguments.front().index;
  return PrefixLoop(rows, starts, state, builder, [&](RowRef row) -> arrow::Status {
    const auto& values = *input[row.chunk]->column(column);
    if (values.IsValid(row.row)) {
      if constexpr (std::is_same_v<ArrowType, arrow::DoubleType>) {
        state.sum += ValueAt<ArrowType>(values, row.row);
      } else {
        Int128 value = 0;
        if constexpr (std::is_same_v<ArrowType, arrow::Date32Type> ||
                      std::is_same_v<ArrowType, arrow::TimestampType>) {
          ARROW_ASSIGN_OR_RAISE(value, TemporalMicros(values, row.row));
        } else {
          value = ValueAt<ArrowType>(values, row.row);
        }
        if (__builtin_add_overflow(state.exact_sum, value, &state.exact_sum)) {
          return arrow::Status::ExecutionError("SUM/AVG overflow: the exact sum exceeds 128 bits");
        }
      }
      ++state.count;
    }
    return state.count == 0 ? builder.AppendNull() : finalize();
  });
}

template <class ArrowType>
arrow::Result<std::shared_ptr<arrow::Array>> Numbers(const plan::WindowCall& call,
                                                     const arrow::RecordBatchVector& input,
                                                     std::span<const RowRef> rows,
                                                     std::span<const std::uint8_t> starts,
                                                     WindowPrefixState& state,
                                                     arrow::MemoryPool* pool) {
  const auto type = call.arguments.front().type;
  if (IsVarianceKind(call.aggregate)) {
    arrow::DoubleBuilder builder(pool);
    return PrefixLoop(rows, starts, state, builder, [&](RowRef row) -> arrow::Status {
      const auto& values = *input[row.chunk]->column(call.arguments.front().index);
      if (values.IsValid(row.row)) {
        const auto value = ValueAt<ArrowType>(values, row.row);
        double number = 0;
        if constexpr (std::is_same_v<ArrowType, arrow::Decimal128Type> ||
                      std::is_same_v<ArrowType, arrow::Decimal64Type>) {
          number =
              DuckDbDecimalToDouble(value, type == plan::LogicalType::kDecimal ? type.width() : 38,
                                    type == plan::LogicalType::kDecimal ? type.scale() : 0);
        } else {
          number = static_cast<double>(value);
        }
        AddValue(state.moments, number);
      }
      ARROW_ASSIGN_OR_RAISE(const auto result, FinalizeVariance(call.aggregate, state.moments));
      return result ? builder.Append(*result) : builder.AppendNull();
    });
  }
  if constexpr (std::is_same_v<ArrowType, arrow::Date32Type> ||
                std::is_same_v<ArrowType, arrow::TimestampType>) {
    arrow::TimestampBuilder builder(plan::ToArrow(call.type), pool);
    return Sums<ArrowType>(call, input, rows, starts, state, builder, [&] {
      return builder.Append(AverageMicros(state.exact_sum, state.count));
    });
  } else if constexpr (std::is_same_v<ArrowType, arrow::DoubleType>) {
    arrow::DoubleBuilder builder(pool);
    return Sums<ArrowType>(call, input, rows, starts, state, builder, [&] {
      return builder.Append(call.aggregate == plan::AggKind::kAvg
                                ? state.sum / static_cast<double>(state.count)
                                : state.sum);
    });
  } else {
    if (call.aggregate == plan::AggKind::kAvg) {
      arrow::DoubleBuilder builder(pool);
      return Sums<ArrowType>(call, input, rows, starts, state, builder, [&] {
        return builder.Append(
            type == plan::LogicalType::kDecimal
                ? DuckDbDecimalAverage(state.exact_sum, state.count, type.width(), type.scale())
                : DuckDbIntegerAverage(state.exact_sum, state.count,
                                       type == plan::LogicalType::kSmallInt));
      });
    }
    arrow::Decimal128Builder builder(plan::ToArrow(call.type), pool);
    const auto range = plan::RangeOf(call.type);
    return Sums<ArrowType>(call, input, rows, starts, state, builder, [&] -> arrow::Status {
      if (state.exact_sum < range.min || state.exact_sum > range.max) {
        return arrow::Status::ExecutionError("SUM overflow: the result is outside the range of ",
                                             plan::ToString(call.type), " (38 decimal digits)");
      }
      return builder.Append(DecimalOf(state.exact_sum));
    });
  }
}

template <class ArrowType>
arrow::Result<std::shared_ptr<arrow::Array>> TypedPrefixes(const plan::WindowCall& call,
                                                           const arrow::RecordBatchVector& input,
                                                           std::span<const RowRef> rows,
                                                           std::span<const std::uint8_t> starts,
                                                           WindowPrefixState& state,
                                                           arrow::MemoryPool* pool) {
  if (call.aggregate == plan::AggKind::kMin || call.aggregate == plan::AggKind::kMax) {
    return Extremes<ArrowType>(call, input, rows, starts, state, pool);
  }
  if constexpr (!std::is_same_v<ArrowType, arrow::BinaryType>) {
    return Numbers<ArrowType>(call, input, rows, starts, state, pool);
  }
  return arrow::Status::Invalid("window: invalid VARCHAR prefix aggregate");
}

}  // namespace

arrow::Result<std::shared_ptr<arrow::Array>> WindowPrefixes(const plan::WindowCall& call,
                                                            const arrow::RecordBatchVector& input,
                                                            std::span<const RowRef> rows,
                                                            std::span<const std::uint8_t> starts,
                                                            WindowPrefixState& state,
                                                            arrow::MemoryPool* pool) {
  if (call.aggregate == plan::AggKind::kCountStar || call.aggregate == plan::AggKind::kCount) {
    arrow::Int64Builder builder(pool);
    return PrefixLoop(rows, starts, state, builder, [&](RowRef row) {
      if (call.arguments.empty() ||
          input[row.chunk]->column(call.arguments.front().index)->IsValid(row.row)) {
        ++state.count;
      }
      return builder.Append(state.count);
    });
  }
  switch (input.front()->column(call.arguments.front().index)->type_id()) {
    case arrow::Type::INT16:
      return TypedPrefixes<arrow::Int16Type>(call, input, rows, starts, state, pool);
    case arrow::Type::INT32:
      return TypedPrefixes<arrow::Int32Type>(call, input, rows, starts, state, pool);
    case arrow::Type::INT64:
      return TypedPrefixes<arrow::Int64Type>(call, input, rows, starts, state, pool);
    case arrow::Type::UINT16:
      return TypedPrefixes<arrow::UInt16Type>(call, input, rows, starts, state, pool);
    case arrow::Type::DOUBLE:
      return TypedPrefixes<arrow::DoubleType>(call, input, rows, starts, state, pool);
    case arrow::Type::DECIMAL128:
      return TypedPrefixes<arrow::Decimal128Type>(call, input, rows, starts, state, pool);
    case arrow::Type::DECIMAL64:
      return TypedPrefixes<arrow::Decimal64Type>(call, input, rows, starts, state, pool);
    case arrow::Type::DATE32:
      return TypedPrefixes<arrow::Date32Type>(call, input, rows, starts, state, pool);
    case arrow::Type::TIMESTAMP:
      return TypedPrefixes<arrow::TimestampType>(call, input, rows, starts, state, pool);
    case arrow::Type::BINARY:
      return TypedPrefixes<arrow::BinaryType>(call, input, rows, starts, state, pool);
    default:
      return arrow::Status::Invalid("window: invalid prefix argument type");
  }
}

}  // namespace antb1::exec
