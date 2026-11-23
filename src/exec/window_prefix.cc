#include "window_prefix.h"

#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
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

// A bucket owns whole SQL partitions. Other buckets can write different bits of the same byte.
class DirectWriter {
 public:
  explicit DirectWriter(WindowPrefixTarget& target) : target_(target) {}
  void Select(RowRef row) {
    std::size_t piece = target_.first[row.chunk];
    slot_ = row.row;
    if (slot_ >= target_.batch_rows) {
      piece += slot_ / target_.batch_rows;
      slot_ %= target_.batch_rows;
    }
    piece_ = &target_.pieces[piece];
  }
  arrow::Status Reserve(int64_t) { return arrow::Status::OK(); }
  arrow::Status AppendNull() {
    auto& byte = piece_->validity[slot_ / 8];
    std::atomic_ref<std::uint8_t>(byte).fetch_and(static_cast<std::uint8_t>(~(1U << (slot_ % 8))),
                                                  std::memory_order_relaxed);
    std::memset(piece_->values + slot_ * target_.width, 0, target_.width);
    std::atomic_ref<int64_t>(piece_->nulls).fetch_add(1, std::memory_order_relaxed);
    return arrow::Status::OK();
  }
  template <class T>
  arrow::Status Append(const T& value) {
    auto* dst = piece_->values + slot_ * target_.width;
    if constexpr (std::is_same_v<T, arrow::Decimal128> || std::is_same_v<T, arrow::Decimal64>) {
      value.ToBytes(dst);
    } else if constexpr (std::is_arithmetic_v<T>) {
      ANTB1_DCHECK(sizeof(T) == target_.width);
      std::memcpy(dst, &value, sizeof(T));
    } else {
      return arrow::Status::Invalid("variable-width direct window result");
    }
    return arrow::Status::OK();
  }
  arrow::Result<std::shared_ptr<arrow::Array>> Finish() { return std::shared_ptr<arrow::Array>{}; }

 private:
  WindowPrefixTarget& target_;
  WindowPrefixPiece* piece_ = nullptr;
  std::size_t slot_ = 0;
};

template <class Builder, class Step>
arrow::Result<std::shared_ptr<arrow::Array>> EmitPrefixes(std::span<const RowRef> rows,
                                                          std::span<const std::uint8_t> starts,
                                                          WindowPrefixState& state,
                                                          Builder& builder, const Step& step) {
  ARROW_RETURN_NOT_OK(builder.Reserve(Narrow<int64_t>(rows.size())));
  for (std::size_t i = 0; i < rows.size(); ++i) {
    if (starts[i] != 0) {
      state = {};
    }
    if constexpr (std::is_same_v<Builder, DirectWriter>) {
      builder.Select(rows[i]);
    }
    ARROW_RETURN_NOT_OK(step(rows[i], builder));
  }
  return builder.Finish();
}

template <class Builder, class Step>
arrow::Result<std::shared_ptr<arrow::Array>> PrefixLoop(std::span<const RowRef> rows,
                                                        std::span<const std::uint8_t> starts,
                                                        WindowPrefixState& state, Builder& builder,
                                                        WindowPrefixTarget* target,
                                                        const Step& step) {
  if (target != nullptr) {
    DirectWriter output(*target);
    return EmitPrefixes(rows, starts, state, output, step);
  }
  return EmitPrefixes(rows, starts, state, builder, step);
}

template <class ArrowType>
arrow::Result<std::shared_ptr<arrow::Array>> Extremes(
    const plan::WindowCall& call, std::span<const arrow::Array* const> arrays,
    std::span<const RowRef> rows, std::span<const std::uint8_t> starts, WindowPrefixState& state,
    arrow::MemoryPool* pool, WindowPrefixTarget* target) {
  typename arrow::TypeTraits<ArrowType>::BuilderType builder(plan::ToArrow(call.type), pool);
  const auto get = [&](RowRef row) { return ValueAt<ArrowType>(*arrays[row.chunk], row.row); };
  const bool minimum = call.aggregate == plan::AggKind::kMin;
  return PrefixLoop(rows, starts, state, builder, target,
                    [&](RowRef row, auto& out) -> arrow::Status {
                      if (arrays[row.chunk]->IsValid(row.row)) {
                        bool replace = !state.best;
                        if (state.best) {
                          const auto value = get(row);
                          const auto best = get(*state.best);
                          if constexpr (std::is_same_v<ArrowType, arrow::DoubleType>) {
                            replace = !std::isnan(value) &&
                                      (std::isnan(best) || (minimum ? value < best : best < value));
                          } else {
                            replace = minimum ? value < best : best < value;
                          }
                        }
                        if (replace) {
                          state.best = row;
                        }
                      }
                      if (!state.best) {
                        return out.AppendNull();
                      }
                      if constexpr (std::is_same_v<ArrowType, arrow::Decimal128Type>) {
                        return out.Append(DecimalOf(get(*state.best)));
                      } else if constexpr (std::is_same_v<ArrowType, arrow::Decimal64Type>) {
                        return out.Append(arrow::Decimal64(static_cast<int64_t>(get(*state.best))));
                      } else {
                        return out.Append(get(*state.best));
                      }
                    });
}

template <class ArrowType, class Builder, class Finalize>
arrow::Result<std::shared_ptr<arrow::Array>> Sums(std::span<const arrow::Array* const> arrays,
                                                  std::span<const RowRef> rows,
                                                  std::span<const std::uint8_t> starts,
                                                  WindowPrefixState& state, Builder& builder,
                                                  WindowPrefixTarget* target,
                                                  const Finalize& finalize) {
  return PrefixLoop(rows, starts, state, builder, target,
                    [&](RowRef row, auto& out) -> arrow::Status {
                      const auto& values = *arrays[row.chunk];
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
                            return arrow::Status::ExecutionError(
                                "SUM/AVG overflow: the exact sum exceeds 128 bits");
                          }
                        }
                        ++state.count;
                      }
                      return state.count == 0 ? out.AppendNull() : finalize(out);
                    });
}

template <class ArrowType>
arrow::Result<std::shared_ptr<arrow::Array>> Numbers(
    const plan::WindowCall& call, std::span<const arrow::Array* const> arrays,
    std::span<const RowRef> rows, std::span<const std::uint8_t> starts, WindowPrefixState& state,
    arrow::MemoryPool* pool, WindowPrefixTarget* target) {
  const auto type = call.arguments.front().type;
  if (IsVarianceKind(call.aggregate)) {
    arrow::DoubleBuilder builder(pool);
    return PrefixLoop(
        rows, starts, state, builder, target, [&](RowRef row, auto& out) -> arrow::Status {
          const auto& values = *arrays[row.chunk];
          if (values.IsValid(row.row)) {
            const auto value = ValueAt<ArrowType>(values, row.row);
            double number = 0;
            if constexpr (std::is_same_v<ArrowType, arrow::Decimal128Type> ||
                          std::is_same_v<ArrowType, arrow::Decimal64Type>) {
              number = DuckDbDecimalToDouble(
                  value, type == plan::LogicalType::kDecimal ? type.width() : 38,
                  type == plan::LogicalType::kDecimal ? type.scale() : 0);
            } else {
              number = static_cast<double>(value);
            }
            AddValue(state.moments, number);
          }
          ARROW_ASSIGN_OR_RAISE(const auto result, FinalizeVariance(call.aggregate, state.moments));
          return result ? out.Append(*result) : out.AppendNull();
        });
  }
  if constexpr (std::is_same_v<ArrowType, arrow::Date32Type> ||
                std::is_same_v<ArrowType, arrow::TimestampType>) {
    arrow::TimestampBuilder builder(plan::ToArrow(call.type), pool);
    return Sums<ArrowType>(arrays, rows, starts, state, builder, target, [&](auto& out) {
      return out.Append(AverageMicros(state.exact_sum, state.count));
    });
  } else if constexpr (std::is_same_v<ArrowType, arrow::DoubleType>) {
    arrow::DoubleBuilder builder(pool);
    return Sums<ArrowType>(arrays, rows, starts, state, builder, target, [&](auto& out) {
      return out.Append(call.aggregate == plan::AggKind::kAvg
                            ? state.sum / static_cast<double>(state.count)
                            : state.sum);
    });
  } else {
    if (call.aggregate == plan::AggKind::kAvg) {
      arrow::DoubleBuilder builder(pool);
      return Sums<ArrowType>(arrays, rows, starts, state, builder, target, [&](auto& out) {
        return out.Append(
            type == plan::LogicalType::kDecimal
                ? DuckDbDecimalAverage(state.exact_sum, state.count, type.width(), type.scale())
                : DuckDbIntegerAverage(state.exact_sum, state.count,
                                       type == plan::LogicalType::kSmallInt));
      });
    }
    arrow::Decimal128Builder builder(plan::ToArrow(call.type), pool);
    const auto range = plan::RangeOf(call.type);
    return Sums<ArrowType>(arrays, rows, starts, state, builder, target,
                           [&](auto& out) -> arrow::Status {
                             if (state.exact_sum < range.min || state.exact_sum > range.max) {
                               return arrow::Status::ExecutionError(
                                   "SUM overflow: the result is outside the range of ",
                                   plan::ToString(call.type), " (38 decimal digits)");
                             }
                             return out.Append(DecimalOf(state.exact_sum));
                           });
  }
}

template <class ArrowType>
arrow::Result<std::shared_ptr<arrow::Array>> TypedPrefixes(
    const plan::WindowCall& call, std::span<const arrow::Array* const> arrays,
    std::span<const RowRef> rows, std::span<const std::uint8_t> starts, WindowPrefixState& state,
    arrow::MemoryPool* pool, WindowPrefixTarget* target) {
  if (call.aggregate == plan::AggKind::kMin || call.aggregate == plan::AggKind::kMax) {
    return Extremes<ArrowType>(call, arrays, rows, starts, state, pool, target);
  }
  if constexpr (!std::is_same_v<ArrowType, arrow::BinaryType>) {
    return Numbers<ArrowType>(call, arrays, rows, starts, state, pool, target);
  }
  return arrow::Status::Invalid("window: invalid VARCHAR prefix aggregate");
}

}  // namespace

arrow::Result<std::shared_ptr<arrow::Array>> WindowPrefixes(
    const plan::WindowCall& call, std::span<const arrow::Array* const> arrays,
    std::span<const RowRef> rows, std::span<const std::uint8_t> starts, WindowPrefixState& state,
    arrow::MemoryPool* pool, WindowPrefixTarget* target) {
  if (call.aggregate == plan::AggKind::kCountStar || call.aggregate == plan::AggKind::kCount) {
    arrow::Int64Builder builder(pool);
    return PrefixLoop(rows, starts, state, builder, target, [&](RowRef row, auto& out) {
      if (call.arguments.empty() || arrays[row.chunk]->IsValid(row.row)) {
        ++state.count;
      }
      return out.Append(state.count);
    });
  }
  switch (arrays.front()->type_id()) {
    case arrow::Type::INT16:
      return TypedPrefixes<arrow::Int16Type>(call, arrays, rows, starts, state, pool, target);
    case arrow::Type::INT32:
      return TypedPrefixes<arrow::Int32Type>(call, arrays, rows, starts, state, pool, target);
    case arrow::Type::INT64:
      return TypedPrefixes<arrow::Int64Type>(call, arrays, rows, starts, state, pool, target);
    case arrow::Type::UINT16:
      return TypedPrefixes<arrow::UInt16Type>(call, arrays, rows, starts, state, pool, target);
    case arrow::Type::DOUBLE:
      return TypedPrefixes<arrow::DoubleType>(call, arrays, rows, starts, state, pool, target);
    case arrow::Type::DECIMAL128:
      return TypedPrefixes<arrow::Decimal128Type>(call, arrays, rows, starts, state, pool, target);
    case arrow::Type::DECIMAL64:
      return TypedPrefixes<arrow::Decimal64Type>(call, arrays, rows, starts, state, pool, target);
    case arrow::Type::DATE32:
      return TypedPrefixes<arrow::Date32Type>(call, arrays, rows, starts, state, pool, target);
    case arrow::Type::TIMESTAMP:
      return TypedPrefixes<arrow::TimestampType>(call, arrays, rows, starts, state, pool, target);
    case arrow::Type::BINARY:
      return TypedPrefixes<arrow::BinaryType>(call, arrays, rows, starts, state, pool, target);
    default:
      return arrow::Status::Invalid("window: invalid prefix argument type");
  }
}

}  // namespace antb1::exec
