#include "decimal.h"

#include <cstddef>
#include <cstdint>
#include <format>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>

#include <arrow/api.h>

#include "antb1/common/int128.h"
#include "antb1/plan/literal.h"
#include "antb1/plan/logical_plan.h"
#include "antb1/plan/types.h"

namespace antb1::exec {
namespace {

using ArrayPtr = std::shared_ptr<arrow::Array>;

bool Holds(plan::CompareOp op, int order) {
  switch (op) {
    case plan::CompareOp::kEq:
      return order == 0;
    case plan::CompareOp::kNe:
      return order != 0;
    case plan::CompareOp::kLt:
      return order < 0;
    case plan::CompareOp::kLe:
      return order <= 0;
    case plan::CompareOp::kGt:
      return order > 0;
    case plan::CompareOp::kGe:
      return order >= 0;
  }
  return false;
}

}  // namespace

Int128 ToInt128(const arrow::Decimal128& d) {
  return static_cast<Int128>((static_cast<UInt128>(static_cast<uint64_t>(d.high_bits())) << 64U) |
                             d.low_bits());
}

arrow::Decimal128 FromInt128(Int128 v) {
  const auto bits = static_cast<UInt128>(v);
  return {static_cast<int64_t>(bits >> 64U), static_cast<uint64_t>(bits)};
}

arrow::Result<UnscaledValues> ReadUnscaled(const arrow::Array& array) {
  UnscaledValues out{.values = std::vector<Int128>(static_cast<std::size_t>(array.length())),
                     .scale = 0};
  const auto read = [&]<class ArrayType> {
    const auto& typed = static_cast<const ArrayType&>(array);
    for (int64_t i = 0; i < typed.length(); ++i) {
      if (typed.IsValid(i)) {
        if constexpr (std::is_same_v<ArrayType, arrow::Decimal128Array>) {
          out.values[static_cast<std::size_t>(i)] = ToInt128(arrow::Decimal128(typed.GetValue(i)));
        } else {
          out.values[static_cast<std::size_t>(i)] = typed.Value(i);
        }
      }
    }
  };
  switch (array.type_id()) {
    case arrow::Type::INT16:
      read.template operator()<arrow::Int16Array>();
      break;
    case arrow::Type::UINT16:
      read.template operator()<arrow::UInt16Array>();
      break;
    case arrow::Type::INT32:
      read.template operator()<arrow::Int32Array>();
      break;
    case arrow::Type::INT64:
      read.template operator()<arrow::Int64Array>();
      break;
    case arrow::Type::DECIMAL128:
      read.template operator()<arrow::Decimal128Array>();
      out.scale = static_cast<const arrow::Decimal128Type&>(*array.type()).scale();
      break;
    default:
      return arrow::Status::Invalid("exact comparison of ", array.type()->ToString());
  }
  return out;
}

arrow::Result<ArrayPtr> DecimalToDouble(const arrow::Array& values, arrow::MemoryPool* pool) {
  if (values.type_id() != arrow::Type::DECIMAL128) {
    return arrow::Status::Invalid("DECIMAL to DOUBLE of ", values.type()->ToString());
  }
  const auto& decimals = static_cast<const arrow::Decimal128Array&>(values);
  const auto& type = static_cast<const arrow::Decimal128Type&>(*values.type());
  arrow::DoubleBuilder builder(pool);
  ARROW_RETURN_NOT_OK(builder.Reserve(decimals.length()));
  for (int64_t i = 0; i < decimals.length(); ++i) {
    if (decimals.IsNull(i)) {
      builder.UnsafeAppendNull();
    } else {
      builder.UnsafeAppend(DuckDbDecimalToDouble(ToInt128(arrow::Decimal128(decimals.GetValue(i))),
                                                 type.precision(), type.scale()));
    }
  }
  ArrayPtr out;
  ARROW_RETURN_NOT_OK(builder.Finish(&out));
  return out;
}

arrow::Status DecimalCastError(Int128 value, plan::LogicalType from, plan::LogicalType to,
                               std::string_view column) {
  const std::string target = std::format("DECIMAL({},{})", to.width(), to.scale());
  const std::string suffix =
      column.empty() ? std::string() : std::format(" when casting from source column {}", column);
  if (from == plan::LogicalType::kDecimal) {
    return arrow::Status::ExecutionError(
        "Casting value \"", plan::FormatDecimal(value, from.width(), from.scale()), "\" to type ",
        target, " failed: value is out of range!", suffix);
  }
  return arrow::Status::ExecutionError("Could not cast value ", Int128ToString(value), " to ",
                                       target, suffix);
}

arrow::Result<ArrayPtr> CastToDecimal(const arrow::Array& values, plan::LogicalType from,
                                      plan::LogicalType to, std::string_view column,
                                      arrow::MemoryPool* pool) {
  ARROW_ASSIGN_OR_RAISE(const UnscaledValues read, ReadUnscaled(values));
  const plan::IntegerRange range = plan::RangeOf(to);
  arrow::Decimal128Builder builder(plan::ToArrow(to), pool);
  ARROW_RETURN_NOT_OK(builder.Reserve(values.length()));
  for (int64_t i = 0; i < values.length(); ++i) {
    if (values.IsNull(i)) {
      builder.UnsafeAppendNull();
      continue;
    }
    const Int128 value = read.values[static_cast<std::size_t>(i)];
    const std::optional<Int128> cast = Rescale(value, read.scale, to.scale());
    if (!cast.has_value() || *cast < range.min || *cast > range.max) {
      return DecimalCastError(value, from, to, column);
    }
    builder.UnsafeAppend(FromInt128(*cast));
  }
  ArrayPtr out;
  ARROW_RETURN_NOT_OK(builder.Finish(&out));
  return out;
}

arrow::Result<ArrayPtr> CompareExact(const arrow::Array& left, const arrow::Array& right,
                                     plan::CompareOp op, arrow::MemoryPool* pool) {
  if (left.length() != right.length()) {
    return arrow::Status::Invalid("exact comparison of arrays of ", left.length(), " and ",
                                  right.length(), " values");
  }
  ARROW_ASSIGN_OR_RAISE(const UnscaledValues l, ReadUnscaled(left));
  ARROW_ASSIGN_OR_RAISE(const UnscaledValues r, ReadUnscaled(right));
  arrow::BooleanBuilder builder(pool);
  ARROW_RETURN_NOT_OK(builder.Reserve(left.length()));
  for (int64_t i = 0; i < left.length(); ++i) {
    if (left.IsNull(i) || right.IsNull(i)) {
      builder.UnsafeAppendNull();
      continue;
    }
    const auto row = static_cast<std::size_t>(i);
    builder.UnsafeAppend(Holds(op, CompareScaled(l.values[row], l.scale, r.values[row], r.scale)));
  }
  ArrayPtr out;
  ARROW_RETURN_NOT_OK(builder.Finish(&out));
  return out;
}

}  // namespace antb1::exec
