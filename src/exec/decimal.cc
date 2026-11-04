#include "decimal.h"

#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <format>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <vector>

#include <arrow/api.h>

#include "antb1/common/int128.h"
#include "antb1/common/narrow.h"
#include "antb1/plan/literal.h"
#include "antb1/plan/logical_plan.h"
#include "antb1/plan/types.h"

#include "decimal_bytes.h"

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

DecimalColumnBuilder::DecimalColumnBuilder(plan::LogicalType type, arrow::MemoryPool* pool)
    : narrow_(plan::ToArrow(type)->id() == arrow::Type::DECIMAL64),
      narrow_builder_(narrow_ ? plan::ToArrow(type) : arrow::decimal64(1, 0), pool),
      wide_builder_(narrow_ ? arrow::decimal128(1, 0) : plan::ToArrow(type), pool) {}

arrow::Status DecimalColumnBuilder::Reserve(std::int64_t rows) {
  return narrow_ ? narrow_builder_.Reserve(rows) : wide_builder_.Reserve(rows);
}

void DecimalColumnBuilder::UnsafeAppend(Int128 value) {
  if (narrow_) {
    narrow_builder_.UnsafeAppend(arrow::Decimal64(static_cast<std::int64_t>(value)));
  } else {
    wide_builder_.UnsafeAppend(FromInt128(value));
  }
}

void DecimalColumnBuilder::UnsafeAppendNull() {
  if (narrow_) {
    narrow_builder_.UnsafeAppendNull();
  } else {
    wide_builder_.UnsafeAppendNull();
  }
}

arrow::Status DecimalColumnBuilder::Finish(std::shared_ptr<arrow::Array>* out) {
  return narrow_ ? narrow_builder_.Finish(out) : wide_builder_.Finish(out);
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
        } else if constexpr (std::is_same_v<ArrayType, arrow::Decimal64Array>) {
          // A decimal64's eight bytes are its unscaled value, two's complement and little-endian
          // on both targets, so they widen by sign extension (ADR 0021's N3).
          std::int64_t value = 0;
          std::memcpy(&value, typed.GetValue(i), sizeof(value));
          out.values[static_cast<std::size_t>(i)] = value;
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
    case arrow::Type::DECIMAL64:
      read.template operator()<arrow::Decimal64Array>();
      out.scale = static_cast<const arrow::Decimal64Type&>(*array.type()).scale();
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
  if (values.type_id() != arrow::Type::DECIMAL128 && values.type_id() != arrow::Type::DECIMAL64) {
    return arrow::Status::Invalid("DECIMAL to DOUBLE of ", values.type()->ToString());
  }
  // Through ReadUnscaled rather than the bytes, so this reads either physical width (ADR 0021's
  // decimal64 item). It is one pass a batch on a path no measured query takes; the formula below
  // is DuckDB's and does not change with the width.
  ARROW_ASSIGN_OR_RAISE(const UnscaledValues read, ReadUnscaled(values));
  const auto& type = static_cast<const arrow::DecimalType&>(*values.type());
  arrow::DoubleBuilder builder(pool);
  ARROW_RETURN_NOT_OK(builder.Reserve(values.length()));
  for (int64_t i = 0; i < values.length(); ++i) {
    if (values.IsNull(i)) {
      builder.UnsafeAppendNull();
    } else {
      builder.UnsafeAppend(DuckDbDecimalToDouble(read.values[static_cast<std::size_t>(i)],
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
  DecimalColumnBuilder builder(to, pool);
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
    builder.UnsafeAppend(*cast);
  }
  ArrayPtr out;
  ARROW_RETURN_NOT_OK(builder.Finish(&out));
  return out;
}

std::string DuckDbDoubleText(double value) {
  if (std::isnan(value)) {
    return "nan";
  }
  if (std::isinf(value)) {
    return value > 0 ? "inf" : "-inf";
  }
  std::array<char, 64> buf{};
  const auto scientific =
      std::to_chars(buf.data(), buf.data() + buf.size(), value, std::chars_format::scientific);
  std::string text(buf.data(), scientific.ptr);
  const std::size_t e = text.find('e');
  const int exponent = e == std::string::npos ? 0 : std::stoi(text.substr(e + 1));
  if (exponent < -4 || exponent >= 16) {
    return text;  // 1e+20, 1.25e-05: two exponent digits at least, as DuckDB prints them
  }
  const auto fixed =
      std::to_chars(buf.data(), buf.data() + buf.size(), value, std::chars_format::fixed);
  text.assign(buf.data(), fixed.ptr);
  return text.contains('.') ? text : text + ".0";
}

namespace {

// DuckDB's physical name of an integer type, as its conversion errors print it.
std::string_view PhysicalName(plan::LogicalType type) {
  switch (type.id()) {
    case plan::LogicalType::kSmallInt:
      return "INT16";
    case plan::LogicalType::kInteger:
      return "INT32";
    case plan::LogicalType::kBigInt:
      return "INT64";
    case plan::LogicalType::kHugeInt:
      return "INT128";
    case plan::LogicalType::kUSmallInt:
      return "UINT16";
    default:
      return "?";
  }
}

// The integer of each row of `values` that `convert` gives (a NULL row stays NULL), appended to
// `builder` as it is computed: one pass, no buffer of the values.
template <class Builder, class Convert>
arrow::Result<ArrayPtr> BuildIntegers(Builder builder, const arrow::Array& values,
                                      const Convert& convert) {
  ARROW_RETURN_NOT_OK(builder.Reserve(values.length()));
  for (int64_t i = 0; i < values.length(); ++i) {
    if (values.IsNull(i)) {
      builder.UnsafeAppendNull();
      continue;
    }
    ARROW_ASSIGN_OR_RAISE(const Int128 value, convert(i));
    if constexpr (std::is_same_v<Builder, DecimalColumnBuilder>) {
      builder.UnsafeAppend(value);
    } else {
      builder.UnsafeAppend(static_cast<Builder::value_type>(value));
    }
  }
  ArrayPtr out;
  ARROW_RETURN_NOT_OK(builder.Finish(&out));
  return out;
}

// The integers `convert` gives for `values`, as a column of the integer type `to`.
template <class Convert>
arrow::Result<ArrayPtr> IntegersOf(const arrow::Array& values, plan::LogicalType to,
                                   arrow::MemoryPool* pool, const Convert& convert) {
  switch (to.id()) {
    case plan::LogicalType::kSmallInt:
      return BuildIntegers(arrow::Int16Builder(pool), values, convert);
    case plan::LogicalType::kInteger:
      return BuildIntegers(arrow::Int32Builder(pool), values, convert);
    case plan::LogicalType::kBigInt:
      return BuildIntegers(arrow::Int64Builder(pool), values, convert);
    case plan::LogicalType::kUSmallInt:
      return BuildIntegers(arrow::UInt16Builder(pool), values, convert);
    case plan::LogicalType::kHugeInt:
      return BuildIntegers(DecimalColumnBuilder(plan::LogicalType::Decimal(38, 0), pool), values,
                           convert);
    default:
      return arrow::Status::Invalid("a cast to ", plan::ToString(to), ", which is no integer type");
  }
}

std::string SourceSuffix(std::string_view column) {
  return column.empty() ? std::string()
                        : std::format(" when casting from source column {}", column);
}

}  // namespace

arrow::Result<ArrayPtr> CastToInteger(const arrow::Array& values, plan::LogicalType from,
                                      plan::LogicalType to, std::string_view column,
                                      arrow::MemoryPool* pool) {
  const plan::IntegerRange range = plan::RangeOf(to);
  if (from == plan::LogicalType::kDouble) {
    if (to == plan::LogicalType::kHugeInt) {
      // DuckDB's HUGEINT reaches 2^127, antb1's 38 digits: IsExecutableCast admits no such cast.
      return arrow::Status::Invalid("a cast of DOUBLE to HUGEINT, which the executor does not run");
    }
    const auto& doubles = static_cast<const arrow::DoubleArray&>(values);
    // The range as doubles: [min, max + 1), each a power of two or zero for these types, so exact,
    // as DuckDB checks a DOUBLE before it rounds it.
    const auto low = static_cast<double>(range.min);
    const double high = static_cast<double>(range.max) + 1.0;
    return IntegersOf(values, to, pool, [&](int64_t i) -> arrow::Result<Int128> {
      const double value = doubles.Value(i);
      // Half to even, as DuckDB rounds a DOUBLE. A value just below max + 1 that rounds up to it
      // has no value of the type: DuckDB's conversion of it depends on the platform (32767.5 to
      // SMALLINT is -32768 on arm64), and antb1 fails it as out of range (divergence D28).
      const double rounded = std::nearbyint(value);
      if (std::isnan(value) || value < low || value >= high || rounded >= high) {
        return arrow::Status::ExecutionError(
            "Type DOUBLE with value ", DuckDbDoubleText(value),
            " can't be cast because the value is out of range for the destination type ",
            PhysicalName(to), SourceSuffix(column));
      }
      return static_cast<Int128>(rounded);
    });
  }
  ARROW_ASSIGN_OR_RAISE(const UnscaledValues read, ReadUnscaled(values));
  return IntegersOf(values, to, pool, [&](int64_t i) -> arrow::Result<Int128> {
    const Int128 value = read.values[static_cast<std::size_t>(i)];
    // A DECIMAL rounds half away from zero (Rescale); an integer is kept.
    const std::optional<Int128> integer = Rescale(value, read.scale, 0);
    if (integer.has_value() && *integer >= range.min && *integer <= range.max) {
      return *integer;
    }
    if (from == plan::LogicalType::kDecimal) {
      return arrow::Status::ExecutionError("Failed to cast decimal value ",
                                           Int128ToString(integer.value_or(value)), " to type ",
                                           PhysicalName(to), SourceSuffix(column));
    }
    return arrow::Status::ExecutionError(
        "Type ", PhysicalName(from), " with value ", Int128ToString(value),
        " can't be cast because the value is out of range for the destination type ",
        PhysicalName(to), SourceSuffix(column));
  });
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
