#include "antb1/exec/compute.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include <arrow/api.h>
#include <arrow/compute/api_scalar.h>
#include <arrow/compute/api_vector.h>
#include <arrow/compute/cast.h>
#include <arrow/compute/exec.h>

#include "antb1/common/int128.h"
#include "antb1/exec/filter.h"
#include "antb1/plan/literal.h"
#include "antb1/plan/logical_plan.h"
#include "antb1/plan/types.h"

#include "parallel_compute.h"

namespace antb1::exec {
namespace {

using ArrayPtr = std::shared_ptr<arrow::Array>;

std::string_view Verb(plan::ArithOp op) {
  switch (op) {
    case plan::ArithOp::kAdd:
      return "addition";
    case plan::ArithOp::kSubtract:
      return "subtraction";
    case plan::ArithOp::kMultiply:
      return "multiplication";
    case plan::ArithOp::kDivide:
    case plan::ArithOp::kIntegerDivide:
    case plan::ArithOp::kModulo:
      break;
  }
  return "division";
}

// Arrow reports an overflow of its checked kernels as Invalid("overflow"): an execution error,
// named like DuckDB's.
arrow::Status Overflow(std::string_view what, plan::LogicalType type) {
  return arrow::Status::ExecutionError("Overflow in ", what, " of ", plan::ToString(type));
}

arrow::Result<ArrayPtr> CastTo(const ArrayPtr& values, const std::shared_ptr<arrow::DataType>& type,
                               arrow::compute::ExecContext* ctx) {
  if (values->type()->Equals(*type)) {
    return values;
  }
  // A DECIMAL (or HUGEINT) to DOUBLE converts as DuckDB converts it (ADR 0021 rule 8), which is
  // not Arrow's cast.
  if (values->type_id() == arrow::Type::DECIMAL128 && type->id() == arrow::Type::DOUBLE) {
    const auto& decimals = static_cast<const arrow::Decimal128Array&>(*values);
    const auto& decimal_type = static_cast<const arrow::Decimal128Type&>(*values->type());
    arrow::DoubleBuilder builder(ctx != nullptr ? ctx->memory_pool() : arrow::default_memory_pool());
    ARROW_RETURN_NOT_OK(builder.Reserve(decimals.length()));
    for (int64_t i = 0; i < decimals.length(); ++i) {
      if (decimals.IsNull(i)) {
        builder.UnsafeAppendNull();
      } else {
        const arrow::Decimal128 d(decimals.GetValue(i));
        const auto bits =
            (static_cast<UInt128>(static_cast<uint64_t>(d.high_bits())) << 64U) | d.low_bits();
        builder.UnsafeAppend(DuckDbDecimalToDouble(static_cast<Int128>(bits),
                                                   decimal_type.precision(), decimal_type.scale()));
      }
    }
    std::shared_ptr<arrow::Array> out;
    ARROW_RETURN_NOT_OK(builder.Finish(&out));
    return out;
  }
  // Integer casts only widen, or narrow a constant that fits; to DOUBLE a BIGINT beyond 2^53
  // rounds to the nearest double, as in DuckDB.
  arrow::compute::CastOptions options = arrow::compute::CastOptions::Safe();
  options.allow_float_truncate = type->id() == arrow::Type::DOUBLE;
  ARROW_ASSIGN_OR_RAISE(const arrow::Datum cast, arrow::compute::Cast(values, type, options, ctx));
  return cast.make_array();
}

// // and % of two arrays of one type, as DuckDB computes them.
template <class ArrayType, class BuilderType>
arrow::Result<ArrayPtr> DivideOrModulo(const arrow::Array& left, const arrow::Array& right,
                                       plan::ArithOp op, plan::LogicalType type,
                                       arrow::MemoryPool* pool) {
  using T = ArrayType::value_type;
  const auto& l = static_cast<const ArrayType&>(left);
  const auto& r = static_cast<const ArrayType&>(right);
  BuilderType builder(pool);
  ARROW_RETURN_NOT_OK(builder.Reserve(l.length()));
  for (int64_t i = 0; i < l.length(); ++i) {
    if (l.IsNull(i) || r.IsNull(i)) {
      builder.UnsafeAppendNull();
      continue;
    }
    const T x = l.Value(i);
    const T y = r.Value(i);
    if constexpr (std::is_floating_point_v<T>) {
      if (op == plan::ArithOp::kModulo) {
        builder.UnsafeAppend(std::fmod(x, y));  // NaN for a zero divisor, as in DuckDB
      } else if (y == 0) {
        builder.UnsafeAppendNull();
      } else {
        builder.UnsafeAppend(x / y);
      }
    } else {
      if (y == 0) {
        builder.UnsafeAppendNull();
        continue;
      }
      if constexpr (std::is_signed_v<T>) {
        if (x == std::numeric_limits<T>::min() && y == -1) {
          return Overflow("division", type);
        }
      }
      builder.UnsafeAppend(static_cast<T>(op == plan::ArithOp::kModulo ? x % y : x / y));
    }
  }
  std::shared_ptr<arrow::Array> out;
  ARROW_RETURN_NOT_OK(builder.Finish(&out));
  return out;
}

arrow::Result<ArrayPtr> DivideOrModulo(const arrow::Array& left, const arrow::Array& right,
                                       plan::ArithOp op, plan::LogicalType type,
                                       arrow::MemoryPool* pool) {
  switch (left.type_id()) {
    case arrow::Type::INT16:
      return DivideOrModulo<arrow::Int16Array, arrow::Int16Builder>(left, right, op, type, pool);
    case arrow::Type::INT32:
      return DivideOrModulo<arrow::Int32Array, arrow::Int32Builder>(left, right, op, type, pool);
    case arrow::Type::INT64:
      return DivideOrModulo<arrow::Int64Array, arrow::Int64Builder>(left, right, op, type, pool);
    case arrow::Type::UINT16:
      return DivideOrModulo<arrow::UInt16Array, arrow::UInt16Builder>(left, right, op, type, pool);
    case arrow::Type::DOUBLE:
      return DivideOrModulo<arrow::DoubleArray, arrow::DoubleBuilder>(left, right, op, type, pool);
    default:
      break;
  }
  return arrow::Status::Invalid("// or % of ", left.type()->ToString());
}

Int128 ToInt128(const arrow::Decimal128& d) {
  return static_cast<Int128>((static_cast<UInt128>(static_cast<uint64_t>(d.high_bits())) << 64U) |
                             d.low_bits());
}

arrow::Decimal128 FromInt128(Int128 v) {
  const auto bits = static_cast<UInt128>(v);
  return {static_cast<int64_t>(bits >> 64U), static_cast<uint64_t>(bits)};
}

// + - * and negation in HUGEINT (decimal128(38, 0)), exact, an overflow of its range an execution
// error. Arrow's decimal kernels would widen the precision instead (beyond 38 digits: an error).
arrow::Result<ArrayPtr> HugeIntArith(const arrow::Array& left, const arrow::Array* right,
                                     plan::ArithOp op, arrow::MemoryPool* pool) {
  const auto& l = static_cast<const arrow::Decimal128Array&>(left);
  const auto* r = static_cast<const arrow::Decimal128Array*>(right);
  const plan::IntegerRange range = plan::RangeOf(plan::LogicalType::kHugeInt);
  arrow::Decimal128Builder builder(plan::ToArrow(plan::LogicalType::kHugeInt), pool);
  ARROW_RETURN_NOT_OK(builder.Reserve(l.length()));
  for (int64_t i = 0; i < l.length(); ++i) {
    if (l.IsNull(i) || (r != nullptr && r->IsNull(i))) {
      builder.UnsafeAppendNull();
      continue;
    }
    const Int128 x = ToInt128(arrow::Decimal128(l.GetValue(i)));
    Int128 result = 0;
    bool overflow = false;
    if (r == nullptr) {
      overflow = __builtin_sub_overflow(Int128{0}, x, &result);
    } else {
      const Int128 y = ToInt128(arrow::Decimal128(r->GetValue(i)));
      switch (op) {
        case plan::ArithOp::kAdd:
          overflow = __builtin_add_overflow(x, y, &result);
          break;
        case plan::ArithOp::kSubtract:
          overflow = __builtin_sub_overflow(x, y, &result);
          break;
        default:
          overflow = __builtin_mul_overflow(x, y, &result);
          break;
      }
    }
    if (overflow || result < range.min || result > range.max) {
      return Overflow(r == nullptr ? "negation" : Verb(op), plan::LogicalType::kHugeInt);
    }
    builder.UnsafeAppend(FromInt128(result));
  }
  std::shared_ptr<arrow::Array> out;
  ARROW_RETURN_NOT_OK(builder.Finish(&out));
  return out;
}

// ---- DECIMAL + - * and negation (ADR 0021 rules 5 to 7), exact in Int128 ----

// One operand of DECIMAL arithmetic: its unscaled values (0 where NULL), validity and type, and its
// name when it is a column (DuckDB names that column in a failed cast).
struct DecimalOperand {
  std::vector<Int128> values;
  const arrow::Array* array = nullptr;
  ArrayPtr owner;  // keeps `array` alive
  plan::LogicalType type;
  std::string column;
};

arrow::Result<DecimalOperand> ReadDecimalOperand(const arrow::Array& array, plan::LogicalType type,
                                                 std::string column) {
  DecimalOperand operand{.values = std::vector<Int128>(static_cast<std::size_t>(array.length())),
                         .array = &array,
                         .type = type,
                         .column = std::move(column)};
  const auto read = [&]<class ArrayType> {
    const auto& typed = static_cast<const ArrayType&>(array);
    for (int64_t i = 0; i < typed.length(); ++i) {
      if (typed.IsValid(i)) {
        if constexpr (std::is_same_v<ArrayType, arrow::Decimal128Array>) {
          operand.values[static_cast<std::size_t>(i)] =
              ToInt128(arrow::Decimal128(typed.GetValue(i)));
        } else {
          operand.values[static_cast<std::size_t>(i)] = typed.Value(i);
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
      break;
    default:
      return arrow::Status::Invalid("DECIMAL arithmetic over ", array.type()->ToString());
  }
  return operand;
}

// Rescales the operand to the result's scale, as DuckDB casts it to the result type first (for +
// and
// -): every value must fit the result's width, else DuckDB's conversion error for the first that
// does not.
arrow::Status RescaleOperand(DecimalOperand& operand, plan::LogicalType result) {
  const bool decimal = operand.type == plan::LogicalType::kDecimal;
  const int from_scale = decimal ? operand.type.scale() : 0;
  const int to_scale = result.scale();
  Int128 factor = 1;
  for (int k = from_scale; k < to_scale; ++k) {
    factor *= 10;  // at most 10^38
  }
  const plan::IntegerRange range = plan::RangeOf(result);
  for (std::size_t i = 0; i < operand.values.size(); ++i) {
    if (operand.array->IsNull(static_cast<int64_t>(i))) {
      continue;
    }
    Int128 scaled = 0;
    if (__builtin_mul_overflow(operand.values[i], factor, &scaled) || scaled < range.min ||
        scaled > range.max) {
      const std::string target = std::format("DECIMAL({},{})", result.width(), result.scale());
      const std::string suffix =
          operand.column.empty() ? "" : " when casting from source column " + operand.column;
      if (decimal) {
        return arrow::Status::ExecutionError(
            "Casting value \"",
            plan::FormatDecimal(operand.values[i], operand.type.width(), operand.type.scale()),
            "\" to type ", target, " failed: value is out of range!", suffix);
      }
      return arrow::Status::ExecutionError(
          "Could not cast value ", Int128ToString(operand.values[i]), " to ", target, suffix);
    }
    operand.values[i] = scaled;
  }
  return arrow::Status::OK();
}

// DuckDB's overflow error: its 64-bit DECIMAL (a width capped to 18) or 128-bit one (to 38).
arrow::Status DecimalOverflow(plan::ArithOp op, Int128 x, Int128 y, plan::LogicalType result) {
  const int width = result.width() <= 18 ? 18 : 38;
  std::string_view verb = "addition";
  std::string_view symbol = "+";
  if (op == plan::ArithOp::kSubtract) {
    verb = "subtract";  // DuckDB's word
    symbol = "-";
  } else if (op == plan::ArithOp::kMultiply) {
    verb = "multiplication";
    symbol = "*";
  }
  std::string_view hint = ";";
  if (width == 18) {
    hint = ". You might want to add an explicit cast to a bigger decimal.";
  } else if (op == plan::ArithOp::kMultiply) {
    hint = ". You might want to add an explicit cast to a decimal with a smaller scale.";
  }
  return arrow::Status::ExecutionError("Overflow in ", verb, " of DECIMAL(", width, ") (",
                                       Int128ToString(x), " ", symbol, " ", Int128ToString(y), ")",
                                       hint);
}

// `left <op> right` in the DECIMAL `result`, the operands already rescaled for +, - and % (the
// caller computes and rescales the left operand before it computes the right one, as DuckDB does),
// * over the unscaled values (the scales add up); every result must fit the result's width (only a
// width capped to 18 or 38 can overflow; % never does).
arrow::Result<ArrayPtr> DecimalArith(const DecimalOperand& left, const DecimalOperand& right,
                                     plan::ArithOp op, plan::LogicalType result,
                                     arrow::MemoryPool* pool) {
  const plan::IntegerRange range = plan::RangeOf(result);
  arrow::Decimal128Builder builder(plan::ToArrow(result), pool);
  ARROW_RETURN_NOT_OK(builder.Reserve(left.array->length()));
  for (std::size_t i = 0; i < left.values.size(); ++i) {
    const auto row = static_cast<int64_t>(i);
    if (left.array->IsNull(row) || right.array->IsNull(row)) {
      builder.UnsafeAppendNull();
      continue;
    }
    const Int128 x = left.values[i];
    const Int128 y = right.values[i];
    Int128 value = 0;
    bool overflow = false;
    switch (op) {
      case plan::ArithOp::kAdd:
        overflow = __builtin_add_overflow(x, y, &value);
        break;
      case plan::ArithOp::kSubtract:
        overflow = __builtin_sub_overflow(x, y, &value);
        break;
      case plan::ArithOp::kMultiply:
        overflow = __builtin_mul_overflow(x, y, &value);
        break;
      case plan::ArithOp::kModulo:
        // The dividend's sign, NULL for a zero divisor (rule 9); |value| < |y| always fits.
        if (y == 0) {
          builder.UnsafeAppendNull();
          continue;
        }
        value = x % y;
        break;
      default:
        return arrow::Status::Invalid("DECIMAL arithmetic has no operator ",
                                      static_cast<int>(op));
    }
    if (overflow || value < range.min || value > range.max) {
      return DecimalOverflow(op, x, y, result);
    }
    builder.UnsafeAppend(FromInt128(value));
  }
  std::shared_ptr<arrow::Array> out;
  ARROW_RETURN_NOT_OK(builder.Finish(&out));
  return out;
}

// ---- TIMESTAMP and DATE fields and floors, in 64-bit civil arithmetic (Arrow's temporal kernels
// keep the year in 16 bits and wrap past 32767; DuckDB's range reaches 294247) ----

constexpr int64_t kMicrosPerDay = 86'400'000'000;
// DuckDB's lowest TIMESTAMP, 290309-12-22 (BC) 00:00:00; INT64_MAX and -INT64_MAX are its
// infinities (and INT32_MAX and -INT32_MAX a DATE's).
constexpr int64_t kMinTimestamp = -9'223'372'022'400'000'000;
constexpr int64_t kTimestampInfinity = std::numeric_limits<int64_t>::max();
constexpr int32_t kDateInfinity = std::numeric_limits<int32_t>::max();

int64_t FloorDiv(int64_t a, int64_t b) {
  return (a / b) - ((a % b != 0 && (a < 0) != (b < 0)) ? 1 : 0);
}
int64_t FloorMod(int64_t a, int64_t b) { return a - (FloorDiv(a, b) * b); }

struct CivilDate {
  int64_t year;  // astronomical: 0 is 1 BC, as DuckDB's EXTRACT gives it
  int64_t month;
  int64_t day;
};

// H. Hinnant's civil_from_days and days_from_civil, in 64 bits (as plan::FormatDate).
CivilDate CivilOf(int64_t days) {
  const int64_t z = days + 719'468;
  const int64_t era = FloorDiv(z, 146'097);
  const int64_t day_of_era = z - (era * 146'097);
  const int64_t year_of_era =
      (day_of_era - (day_of_era / 1'460) + (day_of_era / 36'524) - (day_of_era / 146'096)) / 365;
  const int64_t day_of_year =
      day_of_era - ((365 * year_of_era) + (year_of_era / 4) - (year_of_era / 100));
  const int64_t shifted_month = ((5 * day_of_year) + 2) / 153;  // 0 = March
  const int64_t day = day_of_year - (((153 * shifted_month) + 2) / 5) + 1;
  const int64_t month = shifted_month < 10 ? shifted_month + 3 : shifted_month - 9;
  return {.year = year_of_era + (era * 400) + (month <= 2 ? 1 : 0), .month = month, .day = day};
}

int64_t DaysOf(int64_t year, int64_t month, int64_t day) {
  const int64_t y = month <= 2 ? year - 1 : year;
  const int64_t era = FloorDiv(y, 400);
  const int64_t year_of_era = y - (era * 400);
  const int64_t day_of_year = ((((153 * (month > 2 ? month - 3 : month + 9)) + 2) / 5) + day) - 1;
  const int64_t day_of_era =
      (year_of_era * 365) + (year_of_era / 4) - (year_of_era / 100) + day_of_year;
  return (era * 146'097) + day_of_era - 719'468;
}

// A TIMESTAMP or DATE value as days and microseconds into the day; std::nullopt for an infinity.
struct DayTime {
  int64_t days;
  int64_t micros;  // 0 for a DATE
};

// The values of a TIMESTAMP or DATE array, row by row (`each(row, day_time or nullopt for an
// infinity)`), NULL rows skipped (`null(row)`).
template <class Each, class Null>
arrow::Status ForEachDayTime(const arrow::Array& values, const Each& each, const Null& null) {
  const bool date = values.type_id() == arrow::Type::DATE32;
  if (!date && values.type_id() != arrow::Type::TIMESTAMP) {
    return arrow::Status::Invalid("a date or time function of ", values.type()->ToString());
  }
  for (int64_t i = 0; i < values.length(); ++i) {
    if (values.IsNull(i)) {
      null();
      continue;
    }
    if (date) {
      const int32_t days = static_cast<const arrow::Date32Array&>(values).Value(i);
      if (days == kDateInfinity || days == -kDateInfinity) {
        ARROW_RETURN_NOT_OK(each(std::optional<DayTime>(), days > 0));
        continue;
      }
      ARROW_RETURN_NOT_OK(each(std::optional(DayTime{.days = days, .micros = 0}), false));
      continue;
    }
    const int64_t micros = static_cast<const arrow::TimestampArray&>(values).Value(i);
    if (micros == kTimestampInfinity || micros == -kTimestampInfinity) {
      ARROW_RETURN_NOT_OK(each(std::optional<DayTime>(), micros > 0));
      continue;
    }
    const int64_t days = FloorDiv(micros, kMicrosPerDay);
    ARROW_RETURN_NOT_OK(each(
        std::optional(DayTime{.days = days, .micros = micros - (days * kMicrosPerDay)}), false));
  }
  return arrow::Status::OK();
}

// EXTRACT(field FROM value) as DuckDB computes it: the civil field, NULL for an infinity.
// ISO 8601 week-numbering: the ISO year (astronomical) and week of a day, weeks from Monday.
int64_t WeekdayFromMonday(int64_t days) { return FloorMod(days + 3, 7); }  // 1970-01-01: Thursday
int64_t IsoYearOf(int64_t days) { return CivilOf(days - WeekdayFromMonday(days) + 3).year; }
int64_t IsoWeekOf(int64_t days) {
  const int64_t thursday = days - WeekdayFromMonday(days) + 3;
  return ((thursday - DaysOf(CivilOf(thursday).year, 1, 1)) / 7) + 1;
}
int64_t IsoYearStart(int64_t iso_year) {  // the Monday of ISO week 1
  const int64_t january4 = DaysOf(iso_year, 1, 4);
  return january4 - WeekdayFromMonday(january4);
}

// DuckDB's century and millennium: 1 to 100 is the first century; before year 1 the count goes
// down from -1 (years 0 to -99 are century -1).
int64_t CenturyLike(int64_t year, int64_t span) {
  return year > 0 ? ((year - 1) / span) + 1 : (year / span) - 1;
}

// A BIGINT EXTRACT field of a day and time of day (DuckDB's definitions, checked against DuckDB);
// std::nullopt for an unknown field.
std::optional<int64_t> IntegerPart(std::string_view field, const DayTime& t) {
  const CivilDate date = CivilOf(t.days);
  constexpr int64_t kMicrosPerMinute = 60'000'000;
  if (field == "year") {
    return date.year;
  }
  if (field == "month") {
    return date.month;
  }
  if (field == "day") {
    return date.day;
  }
  if (field == "hour") {
    return t.micros / 3'600'000'000;
  }
  if (field == "minute") {
    return (t.micros / kMicrosPerMinute) % 60;
  }
  if (field == "second") {
    return (t.micros / 1'000'000) % 60;
  }
  if (field == "millisecond") {  // the seconds too, as DuckDB counts them
    return (t.micros % kMicrosPerMinute) / 1'000;
  }
  if (field == "microsecond") {
    return t.micros % kMicrosPerMinute;
  }
  if (field == "quarter") {
    return ((date.month - 1) / 3) + 1;
  }
  if (field == "week") {
    return IsoWeekOf(t.days);
  }
  if (field == "isoyear") {
    return IsoYearOf(t.days);
  }
  if (field == "dow") {  // 0 is Sunday
    return FloorMod(t.days + 4, 7);
  }
  if (field == "isodow") {  // 1 is Monday, 7 Sunday
    return WeekdayFromMonday(t.days) + 1;
  }
  if (field == "doy") {
    return t.days - DaysOf(date.year, 1, 1) + 1;
  }
  if (field == "decade") {
    return date.year / 10;
  }
  if (field == "century") {
    return CenturyLike(date.year, 100);
  }
  if (field == "millennium") {
    return CenturyLike(date.year, 1'000);
  }
  return std::nullopt;
}

// EXTRACT(field FROM value) as DuckDB computes it: the civil field (BIGINT; epoch the seconds since
// 1970 as DOUBLE), NULL for an infinity.
arrow::Result<ArrayPtr> Extract(const arrow::Array& values, std::string_view field,
                                arrow::MemoryPool* pool) {
  const bool date = values.type_id() == arrow::Type::DATE32;
  std::shared_ptr<arrow::Array> out;
  if (field == "epoch") {
    arrow::DoubleBuilder builder(pool);
    ARROW_RETURN_NOT_OK(builder.Reserve(values.length()));
    ARROW_RETURN_NOT_OK(ForEachDayTime(
        values,
        [&](const std::optional<DayTime>& t, bool /*positive*/) -> arrow::Status {
          if (!t.has_value()) {
            builder.UnsafeAppendNull();
          } else if (date) {  // DuckDB: the whole seconds of the date, as a double
            builder.UnsafeAppend(static_cast<double>(t->days * 86'400));
          } else {  // the microseconds of the timestamp (they fit int64) over 10^6
            builder.UnsafeAppend(static_cast<double>((t->days * kMicrosPerDay) + t->micros) / 1e6);
          }
          return arrow::Status::OK();
        },
        [&] { builder.UnsafeAppendNull(); }));
    ARROW_RETURN_NOT_OK(builder.Finish(&out));
    return out;
  }
  arrow::Int64Builder builder(pool);
  ARROW_RETURN_NOT_OK(builder.Reserve(values.length()));
  ARROW_RETURN_NOT_OK(ForEachDayTime(
      values,
      [&](const std::optional<DayTime>& t, bool /*positive*/) -> arrow::Status {
        if (!t.has_value()) {
          builder.UnsafeAppendNull();
          return arrow::Status::OK();
        }
        const std::optional<int64_t> part = IntegerPart(field, *t);
        if (!part.has_value()) {
          return arrow::Status::Invalid("EXTRACT field ", field);
        }
        builder.UnsafeAppend(*part);
        return arrow::Status::OK();
      },
      [&] { builder.UnsafeAppendNull(); }));
  ARROW_RETURN_NOT_OK(builder.Finish(&out));
  return out;
}

// date_trunc('unit', value) as DuckDB computes it: the value floored to the unit (weeks start on
// Monday), an infinity unchanged, and a result outside the TIMESTAMP range an error.
arrow::Result<ArrayPtr> DateTrunc(const arrow::Array& values, std::string_view unit,
                                  arrow::MemoryPool* pool) {
  arrow::TimestampBuilder builder(plan::ToArrow(plan::LogicalType::kTimestamp), pool);
  ARROW_RETURN_NOT_OK(builder.Reserve(values.length()));
  ARROW_RETURN_NOT_OK(ForEachDayTime(
      values,
      [&](const std::optional<DayTime>& t, bool positive) -> arrow::Status {
        if (!t.has_value()) {
          builder.UnsafeAppend(positive ? kTimestampInfinity : -kTimestampInfinity);
          return arrow::Status::OK();
        }
        int64_t days = t->days;
        int64_t micros = t->micros;
        const CivilDate date = CivilOf(days);
        if (unit == "microsecond" || unit == "millisecond" || unit == "second" ||
            unit == "minute" || unit == "hour") {
          int64_t step = 3'600'000'000;
          if (unit == "microsecond") {
            step = 1;
          } else if (unit == "millisecond") {
            step = 1'000;
          } else if (unit == "second") {
            step = 1'000'000;
          } else if (unit == "minute") {
            step = 60'000'000;
          }
          micros -= micros % step;
        } else {
          micros = 0;
          // DuckDB truncates the (astronomical) year toward zero for decades and longer.
          const auto years = [&](int64_t span) { return DaysOf((date.year / span) * span, 1, 1); };
          if (unit == "week") {
            days -= WeekdayFromMonday(days);
          } else if (unit == "isoyear") {
            days = IsoYearStart(IsoYearOf(days));
          } else if (unit == "decade") {
            days = years(10);
          } else if (unit == "century") {
            days = years(100);
          } else if (unit == "millennium") {
            days = years(1'000);
          } else if (unit == "month") {
            days = DaysOf(date.year, date.month, 1);
          } else if (unit == "quarter") {
            days = DaysOf(date.year, (((date.month - 1) / 3) * 3) + 1, 1);
          } else if (unit == "year") {
            days = DaysOf(date.year, 1, 1);
          } else if (unit != "day") {
            return arrow::Status::Invalid("date_trunc unit ", unit);
          }
        }
        int64_t result = 0;
        if (__builtin_mul_overflow(days, kMicrosPerDay, &result) ||
            __builtin_add_overflow(result, micros, &result) || result < kMinTimestamp ||
            result >= kTimestampInfinity) {
          return arrow::Status::ExecutionError(
              "date_trunc: the result is not in the TIMESTAMP range");
        }
        builder.UnsafeAppend(result);
        return arrow::Status::OK();
      },
      [&] { builder.UnsafeAppendNull(); }));
  std::shared_ptr<arrow::Array> out;
  ARROW_RETURN_NOT_OK(builder.Finish(&out));
  return out;
}

struct Evaluator {
  const arrow::RecordBatch& batch;
  arrow::MemoryPool* pool;
  arrow::compute::ExecContext* ctx;

  arrow::Result<ArrayPtr> operator()(const plan::Expr& expr) const {
    return std::visit([&](const auto& node) { return Evaluate(node, expr); }, expr.node);
  }

  arrow::Result<ArrayPtr> Evaluate(const plan::ColumnExpr& column, const plan::Expr& /*e*/) const {
    if (column.index < 0 || column.index >= batch.num_columns()) {
      return arrow::Status::Invalid("expression reads column ", column.index, " of an input with ",
                                    batch.num_columns(), " columns");
    }
    return batch.column(column.index);
  }

  arrow::Result<ArrayPtr> Evaluate(const plan::ConstantExpr& constant,
                                   const plan::Expr& /*e*/) const {
    ARROW_ASSIGN_OR_RAISE(auto scalar, plan::ToArrowScalar(constant.value));
    return arrow::MakeArrayFromScalar(*scalar, batch.num_rows(), pool);
  }

  arrow::Result<ArrayPtr> Evaluate(const plan::NegateExpr& negate, const plan::Expr& e) const {
    ARROW_ASSIGN_OR_RAISE(ArrayPtr operand, (*this)(*negate.operand));
    if (e.type == plan::LogicalType::kDecimal) {
      // The type is kept, and its range is symmetric. A file may still hold a value beyond its
      // declared width (nothing checks it on read), so the 128-bit minimum is an overflow, not UB.
      ARROW_ASSIGN_OR_RAISE(DecimalOperand values, ReadDecimalOperand(*operand, e.type, {}));
      arrow::Decimal128Builder builder(plan::ToArrow(e.type), pool);
      ARROW_RETURN_NOT_OK(builder.Reserve(operand->length()));
      for (int64_t i = 0; i < operand->length(); ++i) {
        if (operand->IsNull(i)) {
          builder.UnsafeAppendNull();
          continue;
        }
        Int128 negated = 0;
        if (__builtin_sub_overflow(Int128{0}, values.values[static_cast<std::size_t>(i)],
                                   &negated)) {
          return Overflow("negation", e.type);
        }
        builder.UnsafeAppend(FromInt128(negated));
      }
      std::shared_ptr<arrow::Array> out;
      ARROW_RETURN_NOT_OK(builder.Finish(&out));
      return out;
    }
    ARROW_ASSIGN_OR_RAISE(operand, CastTo(operand, plan::ToArrow(e.type), ctx));
    if (e.type == plan::LogicalType::kHugeInt) {
      return HugeIntArith(*operand, nullptr, plan::ArithOp::kSubtract, pool);
    }
    const bool integer = plan::IsInteger(e.type);
    auto result =
        arrow::compute::CallFunction(integer ? "negate_checked" : "negate", {operand}, ctx);
    if (!result.ok()) {
      return result.status().IsInvalid() ? Overflow("negation", e.type) : result.status();
    }
    return result->make_array();
  }

  arrow::Result<ArrayPtr> Evaluate(const plan::FunctionExpr& function,
                                   const plan::Expr& /*e*/) const {
    if (function.args.empty()) {
      return arrow::Status::Invalid("function without arguments");
    }
    ARROW_ASSIGN_OR_RAISE(ArrayPtr text, (*this)(*function.args[0]));
    switch (function.function) {
      case plan::Function::kStrlen: {
        // Bytes, as DuckDB's strlen counts them (Arrow gives int32 for binary).
        ARROW_ASSIGN_OR_RAISE(const arrow::Datum length,
                              arrow::compute::CallFunction("binary_length", {text}, ctx));
        return CastTo(length.make_array(), arrow::int64(), ctx);
      }
      case plan::Function::kRegexpReplace:
        return RegexpReplace(function, text);
      case plan::Function::kEpochMs:
        return EpochMs(text);
      case plan::Function::kExtract: {
        ARROW_ASSIGN_OR_RAISE(const std::string field, ConstantArg(function, 1));
        return Extract(*text, field, pool);
      }
      case plan::Function::kDateTrunc: {
        ARROW_ASSIGN_OR_RAISE(const std::string unit, ConstantArg(function, 1));
        return DateTrunc(*text, unit, pool);
      }
    }
    return arrow::Status::Invalid("unknown function");
  }

  // The string constant argument `i` of a function (a field, a unit).
  static arrow::Result<std::string> ConstantArg(const plan::FunctionExpr& function, std::size_t i) {
    const auto* constant = i < function.args.size()
                               ? std::get_if<plan::ConstantExpr>(&function.args[i]->node)
                               : nullptr;
    const auto* value =
        constant != nullptr ? std::get_if<std::string>(&constant->value.value) : nullptr;
    if (value == nullptr) {
      return arrow::Status::Invalid(plan::ToString(function.function),
                                    " takes a constant string argument ", i + 1);
    }
    return *value;
  }

  // epoch_ms: milliseconds to a TIMESTAMP (microseconds), an error outside DuckDB's TIMESTAMP
  // range (290309-12-22 (BC) 00:00:00 up to the int64 microseconds; DuckDB: "Could not convert
  // Timestamp(MS) to Timestamp(US)", "Date out of range in timestamp conversion").
  arrow::Result<ArrayPtr> EpochMs(const ArrayPtr& millis) const {
    ARROW_ASSIGN_OR_RAISE(const ArrayPtr wide, CastTo(millis, arrow::int64(), ctx));
    const auto& values = static_cast<const arrow::Int64Array&>(*wide);
    arrow::TimestampBuilder builder(plan::ToArrow(plan::LogicalType::kTimestamp), pool);
    ARROW_RETURN_NOT_OK(builder.Reserve(values.length()));
    constexpr int64_t kMax = std::numeric_limits<int64_t>::max() / 1000;
    constexpr int64_t kMin = kMinTimestamp / 1000;
    for (int64_t i = 0; i < values.length(); ++i) {
      if (values.IsNull(i)) {
        builder.UnsafeAppendNull();
        continue;
      }
      const int64_t ms = values.Value(i);
      if (ms > kMax || ms < kMin) {
        return arrow::Status::ExecutionError("Could not convert epoch_ms ", ms,
                                             " to a TIMESTAMP: out of range");
      }
      builder.UnsafeAppend(ms * 1000);
    }
    std::shared_ptr<arrow::Array> out;
    ARROW_RETURN_NOT_OK(builder.Finish(&out));
    return out;
  }

  // RE2 in UTF-8 mode, as DuckDB runs it: the binary values are viewed as UTF-8 (not validated;
  // DuckDB's VARCHAR is valid UTF-8 anyway), the first match replaced, the result bytes again.
  // DuckDB calls RE2::Replace on the whole value. Arrow's replace_substring_regex with one
  // replacement re-matches the pattern on the matched substring alone, where \b, \B, ^ and $ see
  // other neighbors; so the pattern becomes ^(\C*?)(pattern), RE2's own unanchored search (\C is
  // any byte) with the text before the match in group 1, and every match of it (there is at most
  // one) is replaced with \1 and the replacement, its groups shifted by two.
  arrow::Result<ArrayPtr> RegexpReplace(const plan::FunctionExpr& function,
                                        const ArrayPtr& text) const {
    if (function.args.size() != 3) {
      return arrow::Status::Invalid("regexp_replace takes three arguments");
    }
    std::array<std::string, 2> pattern_and_replacement;
    for (std::size_t i = 0; i < 2; ++i) {
      const auto* constant = std::get_if<plan::ConstantExpr>(&function.args[i + 1]->node);
      const auto* value =
          constant != nullptr ? std::get_if<std::string>(&constant->value.value) : nullptr;
      if (value == nullptr) {
        return arrow::Status::Invalid("regexp_replace takes a constant pattern and replacement");
      }
      pattern_and_replacement[i] = *value;
    }
    const auto& [pattern, replacement] = pattern_and_replacement;
    std::string shifted = "\\1";
    for (std::size_t i = 0; i < replacement.size(); ++i) {
      shifted.push_back(replacement[i]);
      if (replacement[i] != '\\' || i + 1 == replacement.size()) {
        continue;
      }
      const char next = replacement[++i];
      if (next >= '0' && next <= '7') {
        shifted.push_back(static_cast<char>(next + 2));
      } else if (next >= '8' && next <= '9') {
        return arrow::Status::Invalid("regexp_replace: \\8 and \\9 are rejected by the binder");
      } else {
        shifted.push_back(next);  // \\, or an escape RE2 rejects as DuckDB's RE2 does
      }
    }
    arrow::compute::CastOptions to_utf8 = arrow::compute::CastOptions::Unsafe(arrow::utf8());
    ARROW_ASSIGN_OR_RAISE(const arrow::Datum utf8, arrow::compute::Cast(text, to_utf8, ctx));
    // The pattern alone first: an invalid one fails as in DuckDB, and a valid one cannot close
    // the group it is wrapped in.
    const arrow::compute::ReplaceSubstringOptions check(pattern, "", /*max_replacements=*/-1);
    // (Over one value: Arrow compiles no pattern for an empty input.)
    ARROW_ASSIGN_OR_RAISE(const ArrayPtr one,
                          arrow::MakeArrayFromScalar(arrow::StringScalar(""), 1, pool));
    auto valid = arrow::compute::CallFunction("replace_substring_regex", {one}, &check, ctx);
    if (!valid.ok()) {
      // An invalid pattern; running out of memory stays a memory error.
      return valid.status().IsOutOfMemory()
                 ? valid.status()
                 : arrow::Status::ExecutionError("regexp_replace: ", valid.status().message());
    }
    const arrow::compute::ReplaceSubstringOptions options("^(\\C*?)(" + pattern + ")", shifted,
                                                          /*max_replacements=*/-1);
    // Each distinct value once: the values of a batch repeat (URLs, referrer URLs), and the regex
    // is far dearer than hashing. The results go back to their rows with one Take (NULL stays
    // NULL).
    ARROW_ASSIGN_OR_RAISE(const arrow::Datum encoded,
                          arrow::compute::DictionaryEncode(
                              utf8, arrow::compute::DictionaryEncodeOptions::Defaults(), ctx));
    const std::shared_ptr<arrow::Array> encoded_array = encoded.make_array();
    const auto& dictionary = static_cast<const arrow::DictionaryArray&>(*encoded_array);
    auto replaced = arrow::compute::CallFunction("replace_substring_regex",
                                                 {dictionary.dictionary()}, &options, ctx);
    if (!replaced.ok() && replaced.status().message().starts_with("Invalid replacement string")) {
      // A replacement RE2 rejects (\1 without a group, a lone backslash): DuckDB ignores
      // RE2::Replace's failure and returns the text unchanged.
      return text;
    }
    if (!replaced.ok()) {
      // An invalid pattern: a query error, as in DuckDB (Invalid Input Error). Running out of
      // memory stays a memory error.
      return replaced.status().IsOutOfMemory()
                 ? replaced.status()
                 : arrow::Status::ExecutionError("regexp_replace: ", replaced.status().message());
    }
    ARROW_ASSIGN_OR_RAISE(const arrow::Datum rows,
                          arrow::compute::Take(*replaced, dictionary.indices(),
                                               arrow::compute::TakeOptions::NoBoundsCheck(), ctx));
    arrow::compute::CastOptions to_binary = arrow::compute::CastOptions::Unsafe(arrow::binary());
    ARROW_ASSIGN_OR_RAISE(const arrow::Datum bytes, arrow::compute::Cast(rows, to_binary, ctx));
    return bytes.make_array();
  }

  arrow::Result<ArrayPtr> Evaluate(const plan::ArithExpr& arith, const plan::Expr& e) const {
    if (e.type == plan::LogicalType::kDecimal) {
      // Each operand in its own type (DECIMAL(38,0) and HUGEINT share an Arrow type); for +, - and
      // % the left one is computed and rescaled before the right one is computed, as in DuckDB.
      const auto operand = [&](const plan::Expr& expr) -> arrow::Result<DecimalOperand> {
        ARROW_ASSIGN_OR_RAISE(ArrayPtr values, (*this)(expr));
        const std::string column =
            std::holds_alternative<plan::ColumnExpr>(expr.node) ? expr.name : std::string();
        ARROW_ASSIGN_OR_RAISE(DecimalOperand read, ReadDecimalOperand(*values, expr.type, column));
        read.owner = std::move(values);
        if (arith.op != plan::ArithOp::kMultiply) {
          ARROW_RETURN_NOT_OK(RescaleOperand(read, e.type));
        }
        return read;
      };
      ARROW_ASSIGN_OR_RAISE(const DecimalOperand l, operand(*arith.left));
      ARROW_ASSIGN_OR_RAISE(const DecimalOperand r, operand(*arith.right));
      return DecimalArith(l, r, arith.op, e.type, pool);
    }
    ARROW_ASSIGN_OR_RAISE(ArrayPtr left, (*this)(*arith.left));
    ARROW_ASSIGN_OR_RAISE(ArrayPtr right, (*this)(*arith.right));
    const auto type = plan::ToArrow(e.type);
    ARROW_ASSIGN_OR_RAISE(left, CastTo(left, type, ctx));
    ARROW_ASSIGN_OR_RAISE(right, CastTo(right, type, ctx));
    if (arith.op == plan::ArithOp::kIntegerDivide || arith.op == plan::ArithOp::kModulo) {
      return DivideOrModulo(*left, *right, arith.op, e.type, pool);
    }
    if (e.type == plan::LogicalType::kHugeInt) {
      return HugeIntArith(*left, right.get(), arith.op, pool);
    }
    const bool integer = plan::IsInteger(e.type);
    std::string function;
    switch (arith.op) {
      case plan::ArithOp::kAdd:
        function = "add";
        break;
      case plan::ArithOp::kSubtract:
        function = "subtract";
        break;
      case plan::ArithOp::kMultiply:
        function = "multiply";
        break;
      default:
        function = "divide";  // DOUBLE: IEEE 754, so x / 0 is +-inf and 0 / 0 NaN
        break;
    }
    if (integer) {
      function += "_checked";
    }
    auto result = arrow::compute::CallFunction(function, {left, right}, ctx);
    if (!result.ok()) {
      return integer && result.status().IsInvalid() ? Overflow(Verb(arith.op), e.type)
                                                    : result.status();
    }
    return result->make_array();
  }

  // The predicate over its operands' values (operand i is column i of a batch of them).
  arrow::Result<ArrayPtr> Evaluate(const plan::PredicateExpr& predicate,
                                   const plan::Expr& /*e*/) const {
    arrow::FieldVector fields;
    arrow::ArrayVector columns;
    for (const plan::ExprPtr& operand : predicate.operands) {
      ARROW_ASSIGN_OR_RAISE(ArrayPtr values, (*this)(*operand));
      fields.push_back(arrow::field("o" + std::to_string(fields.size()), values->type()));
      columns.push_back(std::move(values));
    }
    const auto schema = arrow::schema(std::move(fields));
    const auto operands = arrow::RecordBatch::Make(schema, batch.num_rows(), std::move(columns));
    ARROW_ASSIGN_OR_RAISE(const auto evaluator,
                          PredicateEvaluator::Make(predicate.predicate, *schema));
    ARROW_ASSIGN_OR_RAISE(const arrow::Datum result,
                          evaluator.Evaluate(*operands, pool, /*kleene=*/true));
    if (result.is_scalar()) {
      return arrow::MakeArrayFromScalar(*result.scalar(), batch.num_rows(), pool);
    }
    return result.make_array();
  }

  // AND and OR in three-valued logic (Arrow's _kleene kernels), NOT with NULL for NULL. As in
  // DuckDB, an argument is computed only for the rows the earlier ones left undecided (AND: not
  // false, OR: not true), so `x > 100 OR x * x > 0` never computes x * x where x > 100.
  arrow::Result<ArrayPtr> Evaluate(const plan::BoolExpr& boolean, const plan::Expr& /*e*/) const {
    if (boolean.args.empty()) {
      return arrow::Status::Invalid("boolean expression without arguments");
    }
    ARROW_ASSIGN_OR_RAISE(ArrayPtr result, (*this)(*boolean.args[0]));
    if (boolean.op == plan::BoolOp::kNot) {
      ARROW_ASSIGN_OR_RAISE(const arrow::Datum inverted,
                            arrow::compute::CallFunction("invert", {result}, ctx));
      return inverted.make_array();
    }
    const bool is_and = boolean.op == plan::BoolOp::kAnd;
    const std::string function = is_and ? "and_kleene" : "or_kleene";
    ARROW_ASSIGN_OR_RAISE(const ArrayPtr unknown,
                          arrow::MakeArrayOfNull(arrow::boolean(), batch.num_rows(), pool));
    for (std::size_t i = 1; i < boolean.args.size(); ++i) {
      // Undecided: AND true or NULL so far, OR false or NULL.
      ARROW_ASSIGN_OR_RAISE(
          arrow::Datum open,
          arrow::compute::CallFunction("coalesce", {result, arrow::BooleanScalar(is_and)}, ctx));
      if (!is_and) {
        ARROW_ASSIGN_OR_RAISE(open, arrow::compute::CallFunction("invert", {open}, ctx));
      }
      const ArrayPtr open_rows = open.make_array();
      ARROW_ASSIGN_OR_RAISE(
          const ArrayPtr compact,
          EvaluateSelected(*boolean.args[i], static_cast<const arrow::BooleanArray&>(*open_rows),
                           arrow::boolean()));
      // Decided rows get NULL, which the decided side absorbs (false AND NULL, true OR NULL).
      ARROW_ASSIGN_OR_RAISE(const ArrayPtr next, Scatter(unknown, open_rows, compact));
      ARROW_ASSIGN_OR_RAISE(const arrow::Datum both,
                            arrow::compute::CallFunction(function, {result, next}, ctx));
      result = both.make_array();
    }
    return result;
  }

  // The values of `expr` in the rows `mask` (no NULLs) selects, in row order; only those rows are
  // computed, so an expression never fails on a row it does not answer (as in DuckDB's CASE).
  arrow::Result<ArrayPtr> EvaluateSelected(const plan::Expr& expr, const arrow::BooleanArray& mask,
                                           const std::shared_ptr<arrow::DataType>& type) const {
    const int64_t selected = mask.true_count();
    if (selected == batch.num_rows()) {
      ARROW_ASSIGN_OR_RAISE(ArrayPtr values, (*this)(expr));
      return CastTo(values, type, ctx);
    }
    if (selected == 0) {
      return arrow::MakeArrayOfNull(type, 0, pool);
    }
    // Only the columns the expression reads are filtered; the others are NULL placeholders.
    std::vector<int> reads;
    plan::CollectColumns(expr, reads);
    arrow::ArrayVector columns;
    for (int i = 0; i < batch.num_columns(); ++i) {
      if (std::ranges::find(reads, i) == reads.end()) {
        ARROW_ASSIGN_OR_RAISE(auto placeholder,
                              arrow::MakeArrayOfNull(batch.column(i)->type(), selected, pool));
        columns.push_back(std::move(placeholder));
        continue;
      }
      ARROW_ASSIGN_OR_RAISE(const arrow::Datum rows,
                            arrow::compute::Filter(batch.column(i), arrow::Datum(mask.Slice(0)),
                                                   arrow::compute::FilterOptions::Defaults(), ctx));
      columns.push_back(rows.make_array());
    }
    const auto rows = arrow::RecordBatch::Make(batch.schema(), selected, std::move(columns));
    ARROW_ASSIGN_OR_RAISE(ArrayPtr values,
                          (Evaluator{.batch = *rows, .pool = pool, .ctx = ctx})(expr));
    return CastTo(values, type, ctx);
  }

  // `values` (one per true row of `mask`) put at those rows of `into`.
  arrow::Result<ArrayPtr> Scatter(const ArrayPtr& into, const ArrayPtr& mask,
                                  const ArrayPtr& values) const {
    if (values->length() == 0) {
      return into;
    }
    ARROW_ASSIGN_OR_RAISE(
        const arrow::Datum out,
        arrow::compute::CallFunction("replace_with_mask", {into, mask, values}, ctx));
    return out.make_array();
  }

  // CASE: branch by branch over the rows no earlier branch took, each WHEN computed only for those
  // rows and each THEN only for the rows it answers; a NULL condition does not take a row.
  arrow::Result<ArrayPtr> Evaluate(const plan::CaseExpr& c, const plan::Expr& e) const {
    if (c.whens.size() != c.thens.size()) {
      return arrow::Status::Invalid("CASE with ", c.whens.size(), " conditions and ",
                                    c.thens.size(), " values");
    }
    const auto type = plan::ToArrow(e.type);
    const int64_t n = batch.num_rows();
    ARROW_ASSIGN_OR_RAISE(ArrayPtr result, arrow::MakeArrayOfNull(type, n, pool));
    ARROW_ASSIGN_OR_RAISE(ArrayPtr remaining,
                          arrow::MakeArrayFromScalar(arrow::BooleanScalar(true), n, pool));
    ARROW_ASSIGN_OR_RAISE(const ArrayPtr no_condition,
                          arrow::MakeArrayOfNull(arrow::boolean(), n, pool));
    for (std::size_t i = 0; i < c.whens.size(); ++i) {
      const auto& open = static_cast<const arrow::BooleanArray&>(*remaining);
      ARROW_ASSIGN_OR_RAISE(ArrayPtr compact,
                            EvaluateSelected(*c.whens[i], open, arrow::boolean()));
      ARROW_ASSIGN_OR_RAISE(const ArrayPtr condition, Scatter(no_condition, remaining, compact));
      // Taken: the condition is true (NULL counts as false), on an open row.
      ARROW_ASSIGN_OR_RAISE(
          const arrow::Datum filled,
          arrow::compute::CallFunction("coalesce", {condition, arrow::BooleanScalar(false)}, ctx));
      ARROW_ASSIGN_OR_RAISE(const arrow::Datum taken,
                            arrow::compute::CallFunction("and", {filled, remaining}, ctx));
      const ArrayPtr taken_rows = taken.make_array();
      ARROW_ASSIGN_OR_RAISE(
          ArrayPtr values,
          EvaluateSelected(*c.thens[i], static_cast<const arrow::BooleanArray&>(*taken_rows),
                           type));
      ARROW_ASSIGN_OR_RAISE(result, Scatter(result, taken_rows, values));
      ARROW_ASSIGN_OR_RAISE(const arrow::Datum rest,
                            arrow::compute::CallFunction("and_not", {remaining, taken_rows}, ctx));
      remaining = rest.make_array();
    }
    if (c.otherwise != nullptr) {
      ARROW_ASSIGN_OR_RAISE(
          ArrayPtr values,
          EvaluateSelected(*c.otherwise, static_cast<const arrow::BooleanArray&>(*remaining),
                           type));
      ARROW_ASSIGN_OR_RAISE(result, Scatter(result, remaining, values));
    }
    return result;
  }
};

arrow::FieldVector ComputedFields(const arrow::Schema& input,
                                  const std::vector<plan::ExprPtr>& exprs) {
  arrow::FieldVector fields = input.fields();
  for (std::size_t i = 0; i < exprs.size(); ++i) {
    fields.push_back(arrow::field("e" + std::to_string(i), plan::ToArrow(exprs[i]->type)));
  }
  return fields;
}

}  // namespace

arrow::Result<std::shared_ptr<arrow::Array>> EvaluateExpr(const plan::Expr& expr,
                                                          const arrow::RecordBatch& batch,
                                                          arrow::MemoryPool* pool) {
  arrow::compute::ExecContext ctx(pool);
  ARROW_ASSIGN_OR_RAISE(ArrayPtr values,
                        (Evaluator{.batch = batch, .pool = pool, .ctx = &ctx})(expr));
  if (!values->type()->Equals(*plan::ToArrow(expr.type))) {
    return arrow::Status::Invalid("expression of type ", plan::ToString(expr.type),
                                  " evaluated to ", values->type()->ToString());
  }
  return values;
}

std::shared_ptr<arrow::Schema> ComputedSchema(const arrow::Schema& input,
                                              const std::vector<plan::ExprPtr>& exprs) {
  return arrow::schema(ComputedFields(input, exprs));
}

arrow::Result<Batch> ComputeBatch(const Batch& in, const std::vector<plan::ExprPtr>& exprs,
                                  const std::shared_ptr<arrow::Schema>& schema,
                                  arrow::MemoryPool* pool) {
  // Only the selected rows are computed: an expression never fails on a row a filter dropped.
  ARROW_ASSIGN_OR_RAISE(auto rows, Materialize(in, pool));
  arrow::ArrayVector columns = rows->columns();
  for (const plan::ExprPtr& expr : exprs) {
    ARROW_ASSIGN_OR_RAISE(auto values, EvaluateExpr(*expr, *rows, pool));
    columns.push_back(std::move(values));
  }
  return Batch{.data = arrow::RecordBatch::Make(schema, rows->num_rows(), std::move(columns)),
               .selection = {}};
}

ComputeOperator::ComputeOperator(std::unique_ptr<Operator> input, std::vector<plan::ExprPtr> exprs)
    : input_(std::move(input)),
      exprs_(std::move(exprs)),
      schema_(ComputedSchema(*input_->output_schema(), exprs_)) {}

arrow::Status ComputeOperator::Open(ExecContext& ctx) {
  pool_ = ctx.pool;
  return input_->Open(ctx);
}

arrow::Result<Batch> ComputeOperator::Next() {
  ARROW_ASSIGN_OR_RAISE(Batch in, input_->Next());
  if (in.end()) {
    return in;
  }
  return ComputeBatch(in, exprs_, schema_, pool_);
}

}  // namespace antb1::exec
