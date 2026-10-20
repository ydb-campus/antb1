#include "antb1/exec/compute.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <format>
#include <limits>
#include <memory>
#include <span>
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
#include <arrow/util/utf8.h>

#include "antb1/common/int128.h"
#include "antb1/exec/filter.h"
#include "antb1/plan/literal.h"
#include "antb1/plan/logical_plan.h"
#include "antb1/plan/types.h"

#include "decimal.h"
#include "decimal_bytes.h"
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
    return DecimalToDouble(*values,
                           ctx != nullptr ? ctx->memory_pool() : arrow::default_memory_pool());
  }
  // Integer casts only widen, or narrow a constant that fits; to DOUBLE a BIGINT beyond 2^53
  // rounds to the nearest double, as in DuckDB.
  arrow::compute::CastOptions options = arrow::compute::CastOptions::Safe();
  options.allow_float_truncate = type->id() == arrow::Type::DOUBLE;
  ARROW_ASSIGN_OR_RAISE(const arrow::Datum cast, arrow::compute::Cast(values, type, options, ctx));
  return cast.make_array();
}

// // and % of two arrays of one type, as DuckDB computes them. A DOUBLE % gives NaN for a zero
// divisor, or NULL with null_for_zero (DuckDB's % of DECIMALs beyond 38 digits, computed in
// DOUBLE).
template <class ArrayType, class BuilderType>
arrow::Result<ArrayPtr> DivideOrModulo(const arrow::Array& left, const arrow::Array& right,
                                       plan::ArithOp op, plan::LogicalType type, bool null_for_zero,
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
      if (y == 0 && (op != plan::ArithOp::kModulo || null_for_zero)) {
        builder.UnsafeAppendNull();
      } else if (op == plan::ArithOp::kModulo) {
        builder.UnsafeAppend(std::fmod(x, y));  // NaN for a zero divisor, as in DuckDB
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
                                       plan::ArithOp op, plan::LogicalType type, bool null_for_zero,
                                       arrow::MemoryPool* pool) {
  switch (left.type_id()) {
    case arrow::Type::INT16:
      return DivideOrModulo<arrow::Int16Array, arrow::Int16Builder>(left, right, op, type,
                                                                    null_for_zero, pool);
    case arrow::Type::INT32:
      return DivideOrModulo<arrow::Int32Array, arrow::Int32Builder>(left, right, op, type,
                                                                    null_for_zero, pool);
    case arrow::Type::INT64:
      return DivideOrModulo<arrow::Int64Array, arrow::Int64Builder>(left, right, op, type,
                                                                    null_for_zero, pool);
    case arrow::Type::UINT16:
      return DivideOrModulo<arrow::UInt16Array, arrow::UInt16Builder>(left, right, op, type,
                                                                      null_for_zero, pool);
    case arrow::Type::DOUBLE:
      return DivideOrModulo<arrow::DoubleArray, arrow::DoubleBuilder>(left, right, op, type,
                                                                      null_for_zero, pool);
    default:
      break;
  }
  return arrow::Status::Invalid("// or % of ", left.type()->ToString());
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
// One operand of a DECIMAL operation, read without copying it wherever that is possible. An
// operand is read once per operation and then once per row, so materialising it as a vector of
// Int128 costs an allocation, a pass to fill it and a pass to read it back -- about 64 of the 176
// bytes a row that one addition of two columns used to move (ADR 0026's C9). Three shapes:
//
//   `scalar`   a constant: one value, whatever the row (its array is never built either)
//   `bytes`    a decimal128 array: its buffer, 16 bytes a row, read in place
//   `owned`    anything that had to be converted -- an integer array widened to Int128, or an
//              operand a rescale moved -- which is the only shape that allocates
//
// `checked` says a rescale has already confirmed every value fits the result, so the arithmetic
// does not have to: an operand it skipped (the scales agreed) carries whatever a file holds, which
// may be beyond the operand's own declared width, and DecimalArith checks those rows itself.
struct DecimalOperand {
  Int128 scalar = 0;
  const std::uint8_t* bytes = nullptr;
  std::vector<Int128> owned;
  const arrow::Array* array = nullptr;
  ArrayPtr owner;  // keeps `array`, and so `bytes`, alive
  plan::LogicalType type;
  std::string column;
  bool checked = false;

  [[nodiscard]] bool is_scalar() const { return array == nullptr; }
  // The unscaled value of row `row`; 0 where it is NULL, as the vector held before.
  [[nodiscard]] Int128 At(std::size_t row) const {
    if (bytes != nullptr) {
      return ToInt128(arrow::Decimal128(bytes + (row * sizeof(Int128))));
    }
    return owned.empty() ? scalar : owned[row];
  }
};

// Whether a decimal128 array holds its values where At() can read them: Arrow stores them as 16
// contiguous bytes a row, which is what arrow::Decimal128's byte constructor reads.
//
// This is also the physical-type guard for everything below it, which is why this file keeps raw
// casts where the rest of exec goes through decimal_bytes.h's AsDecimals: the test already happens
// here, once per operand per batch, and routing the casts through the helper as well measured
// 1.049x of this operator's own time on the heaviest decimal query -- repeatably, in both orders,
// and for a reason I could not attribute. A guard that is already present is not worth paying for
// twice (ADR 0021's N3).
bool ReadableInPlace(const arrow::Array& array) {
  return array.type_id() == arrow::Type::DECIMAL128;
}

arrow::Result<DecimalOperand> ReadDecimalOperand(const arrow::Array& array, plan::LogicalType type,
                                                 std::string column) {
  DecimalOperand operand{.array = &array, .type = type, .column = std::move(column)};
  if (ReadableInPlace(array)) {
    operand.bytes = static_cast<const arrow::Decimal128Array&>(array).GetValue(0);
    return operand;
  }
  auto unscaled = ReadUnscaled(array);  // an integer operand, widened into a vector
  if (!unscaled.ok()) {
    return arrow::Status::Invalid("DECIMAL arithmetic over ", array.type()->ToString());
  }
  operand.owned = std::move(unscaled->values);
  // An integer array's values are within its physical type, which the type rules nest in the
  // result: only a decimal128 column can hold what its declared width does not admit.
  operand.checked = true;
  return operand;
}

// The same for a constant, which needs no array at all.
DecimalOperand ScalarDecimalOperand(Int128 value, plan::LogicalType type) {
  // The binder folds a constant within its own type, which the type rules nest in the result.
  return DecimalOperand{.scalar = value, .type = type, .checked = true};
}

// The unscaled value of a constant that a DECIMAL operation can read as one value for every row,
// for the types ReadUnscaled accepts: an integer (scale 0), a DECIMAL (whose Constant already holds
// its unscaled value) or a HUGEINT. Nothing for anything else, so such an operand goes the way it
// went before. A Constant holding an Int128 is never NULL (plan::ToArrowScalar), so the
// arithmetic's NULL handling does not have to reach a scalar operand.
std::optional<Int128> UnscaledConstant(const plan::Expr& expr) {
  const auto* constant = std::get_if<plan::ConstantExpr>(&expr.node);
  if (constant == nullptr) {
    return std::nullopt;
  }
  switch (expr.type.id()) {
    case plan::LogicalType::kSmallInt:
    case plan::LogicalType::kInteger:
    case plan::LogicalType::kBigInt:
    case plan::LogicalType::kUSmallInt:
    case plan::LogicalType::kHugeInt:
    case plan::LogicalType::kDecimal:
      break;
    default:
      return std::nullopt;
  }
  const auto* value = std::get_if<Int128>(&constant->value.value);
  return value == nullptr ? std::nullopt : std::optional<Int128>(*value);
}

// Rescales the operand to the result's scale, as DuckDB casts it to the result type first (for +,
// - and %): every value must fit the result's width, else DuckDB's conversion error for the first
// that does not. Nothing to rescale when the scales already agree -- the factor is then 1 -- so the
// walk is skipped and the operand stays where it is, `checked` false: the type rules make the
// result at least as wide as an operand of its own scale, but a file may hold a value beyond its
// declared width (nothing checks that on read), and DecimalArith tests those rows as it reads them
// rather than in a pass of its own (ADR 0026's C9).
arrow::Status RescaleOperand(DecimalOperand& operand, plan::LogicalType result) {
  const bool decimal = operand.type == plan::LogicalType::kDecimal;
  const int from_scale = decimal ? operand.type.scale() : 0;
  const int to_scale = result.scale();
  if (from_scale == to_scale) {
    return arrow::Status::OK();
  }
  operand.checked = true;
  Int128 factor = 1;
  for (int k = from_scale; k < to_scale; ++k) {
    factor *= 10;  // at most 10^38
  }
  const plan::IntegerRange range = plan::RangeOf(result);
  const auto rescale = [&](Int128 value, Int128& out) {
    return !__builtin_mul_overflow(value, factor, &out) && out >= range.min && out <= range.max;
  };
  if (operand.is_scalar()) {
    Int128 scaled = 0;
    if (!rescale(operand.scalar, scaled)) {
      return DecimalCastError(operand.scalar, operand.type, result, operand.column);
    }
    operand.scalar = scaled;
    return arrow::Status::OK();
  }
  // A rescale rewrites every value, so this is where an operand read in place is copied out.
  const auto rows = static_cast<std::size_t>(operand.array->length());
  std::vector<Int128> scaled(rows);
  for (std::size_t i = 0; i < rows; ++i) {
    if (operand.array->IsNull(static_cast<int64_t>(i))) {
      continue;
    }
    const Int128 value = operand.At(i);
    if (!rescale(value, scaled[i])) {
      return DecimalCastError(value, operand.type, result, operand.column);
    }
  }
  operand.owned = std::move(scaled);
  operand.bytes = nullptr;
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

// ---- a DECIMAL arithmetic tree in one pass (ADR 0026's C10) ----

// A tree of DECIMAL +, - and * over columns and constants, compiled once per batch and then run a
// row at a time. Evaluating it node by node, as the recursive Evaluate below does, writes an array
// per node and reads it back for the node above: `ep * (1 - disc) * (1 + tax)` moves about 160
// bytes a row that way, where reading its three columns and writing one result is 64. The steps are
// the same for every row, so the dispatch below predicts perfectly and what is left is the
// arithmetic.
//
// The semantics are the recursive path's, step for step: an operand of a +, - or % is rescaled to
// its step's scale and must fit its range (DuckDB casts it to the result type first), the left one
// before the right one, and a * takes the unscaled values because the scales add up.
// The digits an int64 holds with room for a rescale: rule 6 caps a product of two operands of at
// most this many digits at this many, which is what makes the int64 form of a step common.
constexpr std::uint8_t kMaxNarrowDigits = 18;

constexpr std::size_t kMaxProgramSteps = 16;

struct DecimalProgram {
  // Where a step reads an operand: a column of the batch, a constant, or an earlier step's result.
  struct Source {
    enum class Kind : std::uint8_t { kColumn, kConstant, kStep };
    Kind kind = Kind::kConstant;
    std::size_t at = 0;  // the column, the constant or the step
  };
  // Widest first, so the step carries no padding worth naming.
  struct Step {
    Int128 left_factor = 1;  // the rescale to this step's scale, 1 for none (and for *)
    Int128 right_factor = 1;
    plan::IntegerRange range;  // this step's own: its result and its operands must fit it
    Source left;
    Source right;
    std::string left_column;  // the column name the operand names, empty when it is not one
    std::string right_column;
    plan::ArithOp op = plan::ArithOp::kAdd;
    // Whether the operand still has to be checked against this step's range: a rescale would have
    // done it, and a * rescales nothing and checks nothing (ADR 0026's C9). A constant and an
    // earlier step's result are within their types already -- only a column can hold what its
    // declared width does not admit.
    bool check_left = false;
    bool check_right = false;
    plan::LogicalType type;       // the step's result type, the cast error's target
    plan::LogicalType left_type;  // the operand types the cast error names
    plan::LogicalType right_type;
    // The same step in int64, where this step's result type has at most 18 digits so every value
    // the step may legitimately hold fits one -- rule 6's cap is what makes that common, since a
    // product of two operands of at most 18 digits caps at 18 (ADR 0021's decimal64 item, N1). The
    // factors are powers of ten up to 10^18, which fits an int64 too. An operand is not assumed to
    // fit: a column can hold a value past its declared width, so each is tested as it is read.
    std::int64_t left_factor64 = 1;
    std::int64_t right_factor64 = 1;
    std::int64_t min64 = 0;
    std::int64_t max64 = 0;
    bool narrow = false;
  };

  std::vector<DecimalOperand> columns;  // one per column *occurrence*, each read in place
  std::vector<Int128> constants;
  std::vector<Step> steps;  // in evaluation order; the last one's result is the tree's
};

// `left <op> right` in the DECIMAL `result`, the operands already rescaled for +, - and % (the
// caller computes and rescales the left operand before it computes the right one, as DuckDB does),
// * over the unscaled values (the scales add up); every result must fit the result's width (only a
// width capped to 18 or 38 can overflow; % never does).
arrow::Result<ArrayPtr> DecimalArith(const DecimalOperand& left, const DecimalOperand& right,
                                     plan::ArithOp op, plan::LogicalType result,
                                     arrow::MemoryPool* pool) {
  const plan::IntegerRange range = plan::RangeOf(result);
  // One of the two is an array (an operation of two constants is folded before it reaches here),
  // and its length is the result's.
  const arrow::Array* const sized = left.is_scalar() ? right.array : left.array;
  const auto rows = static_cast<std::size_t>(sized->length());
  DecimalColumnBuilder builder(result, pool);
  ARROW_RETURN_NOT_OK(builder.Reserve(sized->length()));
  // A constant is never NULL here and an array with no NULL needs no test, so a row is only looked
  // up in the bitmaps that have one.
  const arrow::Array* const left_nulls =
      !left.is_scalar() && left.array->null_count() != 0 ? left.array : nullptr;
  const arrow::Array* const right_nulls =
      !right.is_scalar() && right.array->null_count() != 0 ? right.array : nullptr;
  for (std::size_t i = 0; i < rows; ++i) {
    const auto row = static_cast<int64_t>(i);
    if ((left_nulls != nullptr && left_nulls->IsNull(row)) ||
        (right_nulls != nullptr && right_nulls->IsNull(row))) {
      builder.UnsafeAppendNull();
      continue;
    }
    const Int128 x = left.At(i);
    const Int128 y = right.At(i);
    // The rescale a +, - or % would have done confirms its operands fit the result; where it had
    // nothing to rescale it left them unchecked, and a file may hold a value beyond the operand's
    // own declared width. Checked here, where the value is already read, rather than in a pass of
    // its own: without it such a value would come out of `x + -x` as 0 where DuckDB raises its
    // conversion error (ADR 0026's C9). A multiply rescales neither operand, so neither is checked.
    if (op != plan::ArithOp::kMultiply) {
      if (!left.checked && (x < range.min || x > range.max)) {
        return DecimalCastError(x, left.type, result, left.column);
      }
      if (!right.checked && (y < range.min || y > range.max)) {
        return DecimalCastError(y, right.type, result, right.column);
      }
    }
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
        return arrow::Status::Invalid("DECIMAL arithmetic has no operator ", static_cast<int>(op));
    }
    if (overflow || value < range.min || value > range.max) {
      return DecimalOverflow(op, x, y, result);
    }
    builder.UnsafeAppend(value);
  }
  std::shared_ptr<arrow::Array> out;
  ARROW_RETURN_NOT_OK(builder.Finish(&out));
  return out;
}

// The rescale a step applies to an operand of `from` to reach `to`'s scale: the factor, and whether
// the step rescales at all (* does not, and equal scales need no factor).
Int128 ScaleFactor(plan::LogicalType from, plan::LogicalType to, plan::ArithOp op) {
  if (op == plan::ArithOp::kMultiply) {
    return 1;
  }
  const int from_scale = from == plan::LogicalType::kDecimal ? from.scale() : 0;
  const int to_scale = to.scale();
  Int128 factor = 1;
  for (int k = from_scale; k < to_scale; ++k) {
    factor *= 10;  // at most 10^38
  }
  return factor;
}

// Runs `program` over `rows` rows, writing its last step's result. `valid` is the rows every column
// the tree reads has a value for (nullptr: all of them); a row without one is NULL and no step runs
// for it, so an operand a NULL row holds can never raise.
//
// One consequence of going row by row rather than node by node: where more than one row would
// raise, this reports the first such row, while the recursive path reports the first failing row of
// the *left subtree*, having evaluated all of it before the right. Both raise, and each is
// deterministic for a given plan and input, so what differs is which values the message names. The
// alternative is to evaluate every step over every row, which is the pass this exists to remove.
// `v` as an int64, or false when it needs more. A column can hold a value its declared width does
// not admit, so no operand is assumed to fit however narrow its type says it is.
bool FitsInt64(const Int128& v, std::int64_t* out) {
  const auto narrow = static_cast<std::int64_t>(v);
  if (static_cast<Int128>(narrow) != v) {
    return false;
  }
  *out = narrow;
  return true;
}

// One step of a narrow program in int64 (ADR 0021's decimal64 item, N1): an int64 multiply costs
// about a quarter of an Int128 one, and this engine's own capping rule puts most steps within
// reach of it.
//
// False on any overflow and on any value outside the step's range, without saying which -- the
// caller then runs the Int128 path, which decides whether that is an error and raises with the
// row's own values. So this can change neither an answer nor a message: it either agrees with the
// Int128 path exactly, or it stands aside. For a step whose range fits an int64 the two cannot
// disagree about what is an error anyway, since a result past int64 is past the range as well, but
// the design does not rest on that argument.
bool NarrowStep(const DecimalProgram::Step& step, std::int64_t x, std::int64_t y,
                std::int64_t* out) {
  if (step.left_factor64 != 1 && __builtin_mul_overflow(x, step.left_factor64, &x)) {
    return false;
  }
  if ((step.left_factor64 != 1 || step.check_left) && (x < step.min64 || x > step.max64)) {
    return false;
  }
  if (step.right_factor64 != 1 && __builtin_mul_overflow(y, step.right_factor64, &y)) {
    return false;
  }
  if ((step.right_factor64 != 1 || step.check_right) && (y < step.min64 || y > step.max64)) {
    return false;
  }
  std::int64_t value = 0;
  switch (step.op) {
    case plan::ArithOp::kAdd:
      if (__builtin_add_overflow(x, y, &value)) {
        return false;
      }
      break;
    case plan::ArithOp::kSubtract:
      if (__builtin_sub_overflow(x, y, &value)) {
        return false;
      }
      break;
    case plan::ArithOp::kMultiply:
      if (__builtin_mul_overflow(x, y, &value)) {
        return false;
      }
      break;
    default:
      return false;  // % keeps the Int128 path; Compile admits none, so this is a guard
  }
  if (value < step.min64 || value > step.max64) {
    return false;
  }
  *out = value;
  return true;
}

// One unscaled value into a column of whichever physical width plan::ToArrow asked for. A narrow
// column takes the low 64 bits, which is the whole value: every result the program appends has been
// range-checked against its step's type, and a type of at most 18 digits cannot hold more (ADR
// 0021's N3).
//
// Overloads rather than a branch, and the loop below is a template rather than a switch on each
// value, so neither width pays for the other's existence in a per-row test.
void AppendUnscaled(arrow::Decimal128Builder& builder, const Int128& value) {
  builder.UnsafeAppend(FromInt128(value));
}
void AppendUnscaled(arrow::Decimal64Builder& builder, const Int128& value) {
  builder.UnsafeAppend(arrow::Decimal64(static_cast<std::int64_t>(value)));
}

template <class Builder>
arrow::Result<ArrayPtr> RunProgramInto(const DecimalProgram& program, int64_t rows,
                                       const std::uint8_t* valid, Builder& builder);

arrow::Result<ArrayPtr> RunDecimalProgram(const DecimalProgram& program, int64_t rows,
                                          const std::uint8_t* valid, plan::LogicalType result,
                                          arrow::MemoryPool* pool) {
  // One test per call, not per row: the width is a property of the result type.
  std::shared_ptr<arrow::DataType> type = plan::ToArrow(result);
  if (type->id() == arrow::Type::DECIMAL64) {
    arrow::Decimal64Builder narrow(type, pool);
    return RunProgramInto(program, rows, valid, narrow);
  }
  arrow::Decimal128Builder wide(type, pool);
  return RunProgramInto(program, rows, valid, wide);
}

template <class Builder>
arrow::Result<ArrayPtr> RunProgramInto(const DecimalProgram& program, int64_t rows,
                                       const std::uint8_t* valid, Builder& builder) {
  ARROW_RETURN_NOT_OK(builder.Reserve(rows));
  std::array<Int128, kMaxProgramSteps> slots{};
  const std::span<const DecimalProgram::Step> steps(program.steps);
  const std::size_t last = steps.size() - 1;  // Compile returns a step, so there is one
  const auto read = [&](const DecimalProgram::Source& source, std::size_t row) {
    switch (source.kind) {
      case DecimalProgram::Source::Kind::kColumn:
        return program.columns[source.at].At(row);
      case DecimalProgram::Source::Kind::kConstant:
        return program.constants[source.at];
      default:
        return slots[source.at];
    }
  };
  for (int64_t row = 0; row < rows; ++row) {
    if (valid != nullptr && !arrow::bit_util::GetBit(valid, static_cast<std::uint64_t>(row))) {
      builder.UnsafeAppendNull();
      continue;
    }
    const auto at = static_cast<std::size_t>(row);
    bool null = false;  // a % by zero, which is NULL rather than an error
    for (std::size_t s = 0; s < steps.size(); ++s) {
      const DecimalProgram::Step& step = steps[s];
      Int128 x = read(step.left, at);
      Int128 y = read(step.right, at);
      if (step.narrow) {
        std::int64_t xs = 0;
        std::int64_t ys = 0;
        std::int64_t value = 0;
        if (FitsInt64(x, &xs) && FitsInt64(y, &ys) && NarrowStep(step, xs, ys, &value)) {
          slots[s] = value;
          continue;
        }
      }
      // The left operand is rescaled and checked before the right one, as the recursive path does.
      if (step.left_factor != 1) {
        if (__builtin_mul_overflow(x, step.left_factor, &x) || x < step.range.min ||
            x > step.range.max) {
          return DecimalCastError(read(step.left, at), step.left_type, step.type, step.left_column);
        }
      } else if (step.check_left && (x < step.range.min || x > step.range.max)) {
        return DecimalCastError(x, step.left_type, step.type, step.left_column);
      }
      if (step.right_factor != 1) {
        if (__builtin_mul_overflow(y, step.right_factor, &y) || y < step.range.min ||
            y > step.range.max) {
          return DecimalCastError(read(step.right, at), step.right_type, step.type,
                                  step.right_column);
        }
      } else if (step.check_right && (y < step.range.min || y > step.range.max)) {
        return DecimalCastError(y, step.right_type, step.type, step.right_column);
      }
      Int128 value = 0;
      bool overflow = false;
      switch (step.op) {
        case plan::ArithOp::kAdd:
          overflow = __builtin_add_overflow(x, y, &value);
          break;
        case plan::ArithOp::kSubtract:
          overflow = __builtin_sub_overflow(x, y, &value);
          break;
        case plan::ArithOp::kMultiply:
          overflow = __builtin_mul_overflow(x, y, &value);
          break;
        default:  // kModulo
          if (y == 0) {
            null = true;
            break;
          }
          value = x % y;
          break;
      }
      if (null) {
        break;
      }
      if (overflow || value < step.range.min || value > step.range.max) {
        return DecimalOverflow(step.op, x, y, step.type);
      }
      slots[s] = value;
    }
    if (null) {
      builder.UnsafeAppendNull();
      continue;
    }
    AppendUnscaled(builder, slots[last]);
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

// Whether a pattern's `.*$` tail can be run byte-wise, which takes two things of the pattern.
// It must *have* one: the last three characters, with the dot the any-character class and not an
// escaped one. Conservative on purpose -- `(.*)$` and `.{0,}$` keep the dot, which only costs
// speed.
// And the tail must start where a character starts. The values are whole characters (see
// ValuesAreWholeCharacters), but a pattern can still leave the cursor inside one, and there the
// two tails diverge: `.` matches no lone continuation byte, so `.*$` fails and the value comes
// back unchanged, where `\C*$` eats the rest of the character and the replacement cuts it in half.
// Two RE2 constructs can do that, and only these two: `\C`, which consumes one byte, and `\B`,
// which is a word boundary over bytes and so holds between the bytes of a character. `\b` cannot,
// since no byte of a multi-byte character is an ASCII word character, so it sees non-word on both
// sides and fails; `^`, `$`, `\A`, `\z` and their `(?m)` forms are all at a character boundary (a
// newline is one byte, and in any case a value holding one does not take this path); and every
// construct that matches a rune consumes it whole. The wrapper's own `\C*?` can stop mid-character,
// but then what follows it must match there, which without `\C` or `\B` nothing can.
bool TailCanRunByteWise(std::string_view pattern) {
  if (!pattern.ends_with(".*$")) {
    return false;
  }
  bool escaped_dot = false;
  for (std::size_t i = 0; i + 1 < pattern.size(); ++i) {
    if (pattern[i] != '\\') {
      continue;
    }
    if (pattern[i + 1] == 'C' || pattern[i + 1] == 'B') {
      return false;
    }
    escaped_dot = escaped_dot || i + 1 == pattern.size() - 3;
    ++i;  // the escaped character, so that `\\C` is a backslash and a C and not the any byte
  }
  return !escaped_dot;
}

// Whether `.` and `\C` read these values the same way: `.` matches a character, so an invalid
// UTF-8 byte stops it where `\C`, any byte, consumes it. Validated in one pass over all the values
// at once -- `Array::ValidateFull` reads the same bytes per value and costs 27% of q28. The
// concatenation validating is weaker than each value validating, since a sequence split across two
// adjacent values validates as the pair it forms; so no value may begin inside a sequence, which is
// exactly a continuation byte (10xxxxxx) as its first byte. Valid bytes and no boundary inside a
// sequence means every value is a whole number of characters.
bool ValuesAreWholeCharacters(const arrow::StringArray& values) {
  const int64_t rows = values.length();
  if (rows == 0) {
    return true;
  }
  const auto* offsets = values.raw_value_offsets();
  const int64_t to = offsets[rows];
  if (to == offsets[0]) {
    return true;  // every value is empty, so there is nothing to read; the buffer may even be null
  }
  const auto* data = values.raw_data();
  [[maybe_unused]] static const bool tables = [] {
    arrow::util::InitializeUTF8();  // required before validating, and idempotent
    return true;
  }();
  if (!arrow::util::ValidateUTF8(data + offsets[0], to - offsets[0])) {
    return false;
  }
  for (int64_t row = 0; row < rows; ++row) {
    if (offsets[row] == to) {
      continue;  // an empty value at the very end, with no first byte to read
    }
    const std::uint8_t first = data[offsets[row]];
    if (first >= 0x80 && first <= 0xBF) {  // a continuation byte, so this value begins mid-sequence
      return false;
    }
  }
  return true;
}

// The values a `\C*$` tail reads differently from a `.*$` one: those holding a newline, which `.`
// cannot match and `\C` can. Null when no value does, so data without one pays a single memchr over
// all the values and no mask at all. Per value and not per batch on purpose: one newline among the
// ~20 K distinct values of a batch would otherwise cost every one of them the slower pattern, which
// over the ~2,200 such values of ClickBench is more than half the batches and 57% of the win.
arrow::Result<std::shared_ptr<arrow::Array>> ValuesWithANewline(const arrow::StringArray& values,
                                                                arrow::MemoryPool* pool) {
  const int64_t rows = values.length();
  if (rows == 0) {
    return nullptr;
  }
  const auto* offsets = values.raw_value_offsets();
  const auto bytes = static_cast<std::size_t>(offsets[rows] - offsets[0]);
  if (bytes == 0) {
    return nullptr;  // every value is empty, and the buffer of no bytes may be null
  }
  const auto* data = values.raw_data();
  if (std::memchr(data + offsets[0], '\n', bytes) == nullptr) {
    return nullptr;
  }
  arrow::BooleanBuilder mask(pool);
  ARROW_RETURN_NOT_OK(mask.Reserve(rows));
  for (int64_t row = 0; row < rows; ++row) {
    const auto length = static_cast<std::size_t>(offsets[row + 1] - offsets[row]);
    mask.UnsafeAppend(std::memchr(data + offsets[row], '\n', length) != nullptr);
  }
  return mask.Finish();
}

// An expression of a Compute node that has already been computed, with its values. A later
// expression of the same node that *is* this one reads the values instead of computing them again.
// ADR 0026's C10 compiles one program per expression, and `ep * (1 - disc)` is the whole of one of
// the two expressions a SUM-heavy grouped query computes and a subtree of the other, so it was
// compiled and run twice: six steps a row where four do.
//
// Only an expression already materialised as one of the node's outputs qualifies, which is what
// makes this free of C10's objection to evaluating node by node: no array is written that was not
// written anyway, so it moves no extra bytes. It also leaves expression order alone, so a batch
// still fails on its first failing expression, as DuckDB does -- and a shared subtree that raises
// is raised by the earlier expression, with the same message and at the same row.
struct ComputedExpr {
  const plan::Expr* expr;
  ArrayPtr values;
};

struct Evaluator {
  const arrow::RecordBatch& batch;
  arrow::MemoryPool* pool;
  arrow::compute::ExecContext* ctx;
  // The node's earlier expressions, in evaluation order. Empty for an expression evaluated alone.
  std::span<const ComputedExpr> computed;

  arrow::Result<ArrayPtr> operator()(const plan::Expr& expr) const {
    if (const ArrayPtr* done = Reused(expr); done != nullptr) {
      return *done;
    }
    return std::visit([&](const auto& node) { return Evaluate(node, expr); }, expr.node);
  }

  // The values of an earlier expression of this node that is `expr`, or null. DECIMAL only: it is
  // the program path that recompiles a whole subtree, and keeping the test there keeps every other
  // type on exactly the path it had.
  [[nodiscard]] const ArrayPtr* Reused(const plan::Expr& expr) const {
    if (expr.type != plan::LogicalType::kDecimal) {
      return nullptr;
    }
    for (const ComputedExpr& done : computed) {
      if (plan::SameExpr(*done.expr, expr)) {
        return &done.values;
      }
    }
    return nullptr;
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
      DecimalColumnBuilder builder(e.type, pool);
      ARROW_RETURN_NOT_OK(builder.Reserve(operand->length()));
      for (int64_t i = 0; i < operand->length(); ++i) {
        if (operand->IsNull(i)) {
          builder.UnsafeAppendNull();
          continue;
        }
        Int128 negated = 0;
        if (__builtin_sub_overflow(Int128{0}, values.At(static_cast<std::size_t>(i)), &negated)) {
          return Overflow("negation", e.type);
        }
        builder.UnsafeAppend(negated);
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
      case plan::Function::kSubstring:
        return Substring(function, text);
    }
    return arrow::Status::Invalid("unknown function");
  }

  // The codepoints of `value`, counting as DuckDB counts them: a byte that is no UTF-8 continuation
  // byte starts one, and a continuation byte belongs to the codepoint before it. The first byte
  // always starts a codepoint, even when it is a continuation byte -- an unannotated Parquet
  // BYTE_ARRAY is VARCHAR here and may hold any bytes (divergence D15), and no byte of a value may
  // fall outside every codepoint, or a full window would not return the value it covers.
  static int64_t CodepointCount(std::string_view value) {
    int64_t n = 0;
    for (std::size_t byte = 0; byte < value.size(); ++byte) {
      n += byte == 0 || (static_cast<unsigned char>(value[byte]) & 0xC0U) != 0x80U ? 1 : 0;
    }
    return n;
  }

  // The byte offset where codepoint `index` begins, 1-based, or the size of `value` for the
  // codepoint one past its last (CodepointCount(value) + 1).
  static std::size_t ByteOfCodepoint(std::string_view value, int64_t index) {
    int64_t seen = 0;
    for (std::size_t byte = 0; byte < value.size(); ++byte) {
      const bool starts = byte == 0 || (static_cast<unsigned char>(value[byte]) & 0xC0U) != 0x80U;
      if (starts && ++seen == index) {
        return byte;
      }
    }
    return value.size();
  }

  // substring(value, start[, length]), by DuckDB's rule, which one window covers: a negative start
  // counts from the end, a negative length extends the window backwards, and the window is clipped
  // to the value. NULL in any argument gives NULL, and a start or a length outside
  // [-4294967296, 4294967295] is the execution error DuckDB raises for it, one message per side.
  //
  // Where no length is written DuckDB does not reach the end of the value: it defaults the length
  // to 4294967295, which shows only at the very bottom of the start's range --
  // `substring(v, -4294967296)` loses the last codepoint where `substring(v, -4294967295)` keeps
  // the value whole.
  //
  // Counted in codepoints, as DuckDB counts them -- where antb1's strlen counts bytes (divergence
  // D15) -- so a byte that is no UTF-8 continuation byte starts one and an invalid byte counts as
  // one of its own.
  arrow::Result<ArrayPtr> Substring(const plan::FunctionExpr& function,
                                    const ArrayPtr& text) const {
    if (function.args.size() < 2 || function.args.size() > 3) {
      return arrow::Status::Invalid("substring takes two or three arguments");
    }
    ARROW_ASSIGN_OR_RAISE(const ArrayPtr start_arg, (*this)(*function.args[1]));
    ARROW_ASSIGN_OR_RAISE(const ArrayPtr starts, CastTo(start_arg, arrow::int64(), ctx));
    ArrayPtr lengths;
    if (function.args.size() == 3) {
      ARROW_ASSIGN_OR_RAISE(const ArrayPtr length_arg, (*this)(*function.args[2]));
      ARROW_ASSIGN_OR_RAISE(lengths, CastTo(length_arg, arrow::int64(), ctx));
    }
    const auto& values = static_cast<const arrow::BinaryArray&>(*text);
    const auto& from = static_cast<const arrow::Int64Array&>(*starts);
    const auto* count =
        lengths == nullptr ? nullptr : &static_cast<const arrow::Int64Array&>(*lengths);
    constexpr int64_t kLimit = 4294967295;   // DuckDB's own bound on an offset or a length
    constexpr int64_t kFloor = -kLimit - 1;  // and on a negative one, one step further out
    arrow::BinaryBuilder out(pool);
    ARROW_RETURN_NOT_OK(out.Reserve(values.length()));
    ARROW_RETURN_NOT_OK(out.ReserveData(values.total_values_length()));
    for (int64_t row = 0; row < values.length(); ++row) {
      if (values.IsNull(row) || from.IsNull(row) || (count != nullptr && count->IsNull(row))) {
        ARROW_RETURN_NOT_OK(out.AppendNull());
        continue;
      }
      const int64_t start = from.Value(row);
      if (start > kLimit || start < kFloor) {
        return arrow::Status::Invalid(
            std::format("Substring offset outside of supported range ({} {})",
                        start > kLimit ? '>' : '<', start > kLimit ? kLimit : kFloor));
      }
      if (count != nullptr && (count->Value(row) > kLimit || count->Value(row) < kFloor)) {
        const int64_t length = count->Value(row);
        return arrow::Status::Invalid(
            std::format("Substring length outside of supported range ({} {})",
                        length > kLimit ? '>' : '<', length > kLimit ? kLimit : kFloor));
      }
      const std::string_view value = values.GetView(row);
      const int64_t n = CodepointCount(value);
      const int64_t s = start < 0 ? n + 1 + start : start;
      // Both arguments are inside [kFloor, kLimit] by now and n is one batch value's size, so every
      // sum here stays far inside int64. A missing length is kLimit, not the end of the value.
      const int64_t length = count == nullptr ? kLimit : count->Value(row);
      const int64_t lo = std::max<int64_t>(length < 0 ? s + length : s, 1);
      const int64_t hi = std::min<int64_t>(length < 0 ? s : s + length, n + 1);
      if (hi <= lo) {
        ARROW_RETURN_NOT_OK(out.Append("", 0));
        continue;
      }
      // Two walks of the value's bytes and no allocation of our own: the offsets of every codepoint
      // would be a vector the size of the value, outside the memory budget this operator reports.
      const std::size_t begin = ByteOfCodepoint(value, lo);
      const std::size_t end = ByteOfCodepoint(value, hi);
      ARROW_RETURN_NOT_OK(out.Append(value.data() + begin, static_cast<int32_t>(end - begin)));
    }
    return out.Finish();
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
  //
  // Except when the pattern is already anchored at ^, where the wrapper is not only unnecessary but
  // expensive. A leading ^ can match at one place only, so the lazy \C*? before it can match
  // nothing: ^(\C*?)(^P) accepts exactly what ^P accepts, with group 1 always empty. But \C*?
  // hides P's literal prefix from RE2, which can no longer reject a value that cannot match without
  // trying every position -- and a value that does not match is the common case for the patterns
  // this is used with. Over values of which 82% match, dropping the wrapper is 0.79x; over values
  // of which none match, 0.29x; over values that all match, 0.93x. The binder's restrictions stay
  // as they are either way (\Q in a pattern, \8 and \9 in a replacement, plan/binder.cc), so
  // which form runs is invisible apart from the speed.
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
    // A leading ^ forces the wrapper's prefix to be empty, so the pattern runs as the user wrote
    // it, with their replacement and their group numbers -- but only where that ^ provably anchors
    // the *whole* pattern, which takes more than a leading character:
    //   - no alternation anywhere. A top-level `|` leaves a branch unanchored, so `^a|b` matches a
    //     `b` at any position and the kernel, replacing every match, would replace them all:
    //     'abab' would come back 'XXaX' where DuckDB gives 'Xbab'. Rejecting every `|`, not only a
    //     top-level one, keeps this a character scan: `^(a|b)c` is anchored too and keeps the
    //     wrapper, which costs only speed.
    //   - no repetition of the anchor. `^*b` and `^?b` make it optional, so they match a `b`
    //     anywhere as well.
    // Anything else that could bypass the first term of a concatenation is one of those two, and a
    // `(?m)` can only follow the ^ here, where it no longer applies to it.
    const bool anchored = pattern.starts_with('^') && !pattern.contains('|') &&
                          (pattern.size() == 1 || !std::string_view("*+?{").contains(pattern[1]));
    // Each distinct value once: the values of a batch repeat (URLs, referrer URLs), and the regex
    // is far dearer than hashing. The results go back to their rows with one Take (NULL stays
    // NULL). Encoded before the pattern is settled, because the tail rewrite below reads these
    // values.
    ARROW_ASSIGN_OR_RAISE(const arrow::Datum encoded,
                          arrow::compute::DictionaryEncode(
                              utf8, arrow::compute::DictionaryEncodeOptions::Defaults(), ctx));
    const std::shared_ptr<arrow::Array> encoded_array = encoded.make_array();
    const auto& dictionary = static_cast<const arrow::DictionaryArray&>(*encoded_array);
    const auto& values = static_cast<const arrow::StringArray&>(*dictionary.dictionary());
    // The pattern the kernel runs, wrapped unless the user anchored it themselves.
    const auto options_for = [&](const std::string& body) {
      return arrow::compute::ReplaceSubstringOptions(anchored ? body : "^(\\C*?)(" + body + ")",
                                                     anchored ? replacement : shifted,
                                                     /*max_replacements=*/-1);
    };
    // A trailing `.*$` does no work. Its match is replaced along with the rest, so every byte it
    // reads is thrown away, and all it establishes is that those bytes are characters and none of
    // them is a newline. `\C*$` -- RE2's any byte -- establishes nothing and skips bytes instead of
    // decoding characters: over ClickBench's Referer values, whose ~82 bytes are about 60 of tail,
    // 0.44 us a value becomes 0.20. The two accept the same value when it is a whole number of
    // characters and holds no newline, so the values that hold one run the pattern as written.
    std::string body = pattern;
    std::shared_ptr<arrow::Array> newlines;
    if (TailCanRunByteWise(body) && ValuesAreWholeCharacters(values)) {
      ARROW_ASSIGN_OR_RAISE(newlines, ValuesWithANewline(values, pool));
      body.replace(body.size() - 3, 3, "\\C*$");
    }
    const arrow::compute::ReplaceSubstringOptions options = options_for(body);
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
    if (newlines != nullptr) {
      // The few values a newline makes the two tails disagree on, run again as the user wrote the
      // pattern and put back where they belong. `\C*$` crossed that newline where `.*$` stops at
      // it, which for a pattern anchored before the tail means no match at all and the value
      // unchanged -- what DuckDB's RE2 does, so the rewrite stays invisible.
      ARROW_ASSIGN_OR_RAISE(const arrow::Datum crossed,
                            arrow::compute::Filter(dictionary.dictionary(), newlines));
      const arrow::compute::ReplaceSubstringOptions as_written = options_for(pattern);
      ARROW_ASSIGN_OR_RAISE(
          const arrow::Datum kept,
          arrow::compute::CallFunction("replace_substring_regex", {crossed}, &as_written, ctx));
      ARROW_ASSIGN_OR_RAISE(replaced,
                            arrow::compute::ReplaceWithMask(*replaced, newlines, kept, ctx));
    }
    ARROW_ASSIGN_OR_RAISE(const arrow::Datum rows,
                          arrow::compute::Take(*replaced, dictionary.indices(),
                                               arrow::compute::TakeOptions::NoBoundsCheck(), ctx));
    arrow::compute::CastOptions to_binary = arrow::compute::CastOptions::Unsafe(arrow::binary());
    ARROW_ASSIGN_OR_RAISE(const arrow::Datum bytes, arrow::compute::Cast(rows, to_binary, ctx));
    return bytes.make_array();
  }

  // The rows every column the program reads has a value for, as one bitmap; nullptr when none of
  // them has a NULL, which is the common case and lets the run test no bitmap at all. A row without
  // a value is NULL whatever the arithmetic would have made of it, so no step runs for it.
  arrow::Result<std::shared_ptr<arrow::Buffer>> ProgramValidity(
      const DecimalProgram& program) const {
    const arrow::Array* only = nullptr;
    std::size_t nullable = 0;
    for (const DecimalOperand& column : program.columns) {
      if (column.array->null_count() != 0) {
        ++nullable;
        only = column.array;
      }
    }
    if (nullable == 0) {
      return nullptr;
    }
    // One nullable column at no offset: its own bitmap, whose bits are then the rows. A sliced
    // array's bitmap starts at the buffer, not at the array's first row, so that one is combined
    // below through IsValid, which reads the offset.
    if (nullable == 1 && only->offset() == 0) {
      return only->null_bitmap();
    }
    const int64_t rows = batch.num_rows();
    ARROW_ASSIGN_OR_RAISE(std::shared_ptr<arrow::Buffer> valid, arrow::AllocateBitmap(rows, pool));
    std::uint8_t* const bits = valid->mutable_data();
    for (int64_t row = 0; row < rows; ++row) {
      bool ok = true;
      for (const DecimalOperand& column : program.columns) {
        ok = ok && column.array->IsValid(row);
      }
      arrow::bit_util::SetBitTo(bits, row, ok);
    }
    return valid;
  }

  // Adds `expr` to `program` as a step, with the steps its operands need below it, and says where
  // its result is read from. Nothing when the tree is not one the program covers: a node that is
  // not a DECIMAL +, - or * over a column, a constant or another such node, or a tree deeper than
  // the slots allow. The caller then evaluates it the recursive way (ADR 0026's C10).
  std::optional<DecimalProgram::Source> Compile(const plan::Expr& expr,
                                                DecimalProgram& program) const {
    using Source = DecimalProgram::Source;
    // An earlier expression of this node that is this subtree: read its array as a column of the
    // program.
    //
    // `checked` only for an arithmetic root, and this is the one thing here that must not be got
    // wrong. A DECIMAL +, - or * has its result range-tested against its own type before anyone
    // sees it (RunDecimalProgram's step check, and DecimalArith on the recursive path), so an
    // operand reading it needs no second test. Nothing else here gives that: a DECIMAL negation
    // keeps its operand's type and tests only 128 bits -- Evaluate(NegateExpr) says so in as many
    // words, "a file may still hold a value beyond its declared width (nothing checks it on
    // read)" -- and a CASE whose branch type is already its own converts nothing. Either can
    // therefore carry a value no value of that type should be, which is exactly what the step
    // above has to keep checking. Marking those `checked` turns a cast error into an answer.
    if (const ArrayPtr* done = Reused(expr); done != nullptr && ReadableInPlace(**done)) {
      if (auto read = ReadDecimalOperand(**done, expr.type, std::string()); read.ok()) {
        read->owner = *done;
        read->checked = std::holds_alternative<plan::ArithExpr>(expr.node);
        program.columns.push_back(*std::move(read));
        return Source{.kind = Source::Kind::kColumn, .at = program.columns.size() - 1};
      }
    }
    if (const std::optional<Int128> value = UnscaledConstant(expr); value.has_value()) {
      program.constants.push_back(*value);
      return Source{.kind = Source::Kind::kConstant, .at = program.constants.size() - 1};
    }
    if (const auto* column = std::get_if<plan::ColumnExpr>(&expr.node)) {
      if (expr.type != plan::LogicalType::kDecimal || column->index < 0 ||
          column->index >= batch.num_columns()) {
        return std::nullopt;  // an integer column would have to be widened, which copies anyway
      }
      const ArrayPtr& values = batch.column(column->index);
      // A decimal128 column is read in place. A decimal64 one is widened into the operand's own
      // vector, which is one pass a batch rather than a test a row, and leaves this file's hot loop
      // exactly as it was (ADR 0021's N3). An integer column would have to be widened as well and
      // keeps the recursive path, as before.
      if (!ReadableInPlace(*values) && values->type_id() != arrow::Type::DECIMAL64) {
        return std::nullopt;
      }
      auto read = ReadDecimalOperand(*values, expr.type, expr.name);
      if (!read.ok()) {
        return std::nullopt;
      }
      read->owner = values;
      program.columns.push_back(*std::move(read));
      return Source{.kind = Source::Kind::kColumn, .at = program.columns.size() - 1};
    }
    const auto* arith = std::get_if<plan::ArithExpr>(&expr.node);
    if (arith == nullptr || expr.type != plan::LogicalType::kDecimal ||
        program.steps.size() + 1 > kMaxProgramSteps) {
      return std::nullopt;
    }
    if (arith->op != plan::ArithOp::kAdd && arith->op != plan::ArithOp::kSubtract &&
        arith->op != plan::ArithOp::kMultiply) {
      return std::nullopt;  // % and the divisions keep the recursive path
    }
    // The operands first, left before right, so the steps come out in evaluation order.
    const std::optional<Source> left = Compile(*arith->left, program);
    if (!left.has_value()) {
      return std::nullopt;
    }
    const std::optional<Source> right = Compile(*arith->right, program);
    if (!right.has_value() || program.steps.size() + 1 > kMaxProgramSteps) {
      return std::nullopt;
    }
    const auto name = [](const plan::Expr& operand) {
      return std::holds_alternative<plan::ColumnExpr>(operand.node) ? operand.name : std::string();
    };
    const Int128 left_factor = ScaleFactor(arith->left->type, expr.type, arith->op);
    const Int128 right_factor = ScaleFactor(arith->right->type, expr.type, arith->op);
    const bool rescales = arith->op != plan::ArithOp::kMultiply;
    // A column whose values a rescale has not already confirmed has to be checked against this
    // step's range. A reused expression's array carries `checked`, so it is not re-checked.
    const auto unchecked = [&](const Source& source, const Int128& factor) {
      return rescales && source.kind == Source::Kind::kColumn && factor == 1 &&
             !program.columns[source.at].checked;
    };
    program.steps.push_back(DecimalProgram::Step{.left_factor = left_factor,
                                                 .right_factor = right_factor,
                                                 .range = plan::RangeOf(expr.type),
                                                 .left = *left,
                                                 .right = *right,
                                                 .left_column = name(*arith->left),
                                                 .right_column = name(*arith->right),
                                                 .op = arith->op,
                                                 .check_left = unchecked(*left, left_factor),
                                                 .check_right = unchecked(*right, right_factor),
                                                 .type = expr.type,
                                                 .left_type = arith->left->type,
                                                 .right_type = arith->right->type});
    // The int64 form of this step, where its own range admits one. The step's result type decides:
    // an operand still has to be tested as it is read, because a column can hold a value its
    // declared width does not admit, and an operand of a wider type simply fails that test and
    // keeps the Int128 path for its row.
    DecimalProgram::Step& added = program.steps.back();
    if (expr.type == plan::LogicalType::kDecimal && expr.type.width() <= kMaxNarrowDigits) {
      added.narrow = FitsInt64(added.range.min, &added.min64) &&
                     FitsInt64(added.range.max, &added.max64) &&
                     FitsInt64(left_factor, &added.left_factor64) &&
                     FitsInt64(right_factor, &added.right_factor64);
    }
    return Source{.kind = Source::Kind::kStep, .at = program.steps.size() - 1};
  }

  arrow::Result<ArrayPtr> Evaluate(const plan::ArithExpr& arith, const plan::Expr& e) const {
    if (e.type == plan::LogicalType::kDecimal) {
      // The whole tree in one pass where it is one the program covers, which is what keeps a chain
      // of DECIMAL arithmetic from writing an array per operation and reading it back for the one
      // above (ADR 0026's C10).
      DecimalProgram program;
      if (const std::optional<DecimalProgram::Source> root = Compile(e, program);
          root.has_value() && root->kind == DecimalProgram::Source::Kind::kStep &&
          !program.columns.empty()) {
        ARROW_ASSIGN_OR_RAISE(const std::shared_ptr<arrow::Buffer> valid, ProgramValidity(program));
        return RunDecimalProgram(program, batch.num_rows(),
                                 valid == nullptr ? nullptr : valid->data(), e.type, pool);
      }
      // Each operand in its own type (DECIMAL(38,0) and HUGEINT share an Arrow type); for +, - and
      // % the left one is computed and rescaled before the right one is computed, as in DuckDB.
      // A constant is one value for every row, so it is read as one rather than built into an array
      // of copies of itself and then widened into a vector of them (ADR 0026's C9). Not when both
      // operands are constants: the result is then as long as the batch, which only an array of one
      // of them says, and the binder folds such an operation long before this anyway.
      const bool both_constant =
          UnscaledConstant(*arith.left).has_value() && UnscaledConstant(*arith.right).has_value();
      const auto operand = [&](const plan::Expr& expr) -> arrow::Result<DecimalOperand> {
        DecimalOperand read;
        const std::optional<Int128> value = both_constant ? std::nullopt : UnscaledConstant(expr);
        if (value.has_value()) {
          read = ScalarDecimalOperand(*value, expr.type);
        } else {
          ARROW_ASSIGN_OR_RAISE(ArrayPtr values, (*this)(expr));
          const std::string column =
              std::holds_alternative<plan::ColumnExpr>(expr.node) ? expr.name : std::string();
          ARROW_ASSIGN_OR_RAISE(read, ReadDecimalOperand(*values, expr.type, column));
          read.owner = std::move(values);
        }
        if (arith.op != plan::ArithOp::kMultiply) {
          ARROW_RETURN_NOT_OK(RescaleOperand(read, e.type));
        }
        return read;
      };
      ARROW_ASSIGN_OR_RAISE(const DecimalOperand l, operand(*arith.left));
      ARROW_ASSIGN_OR_RAISE(const DecimalOperand r, operand(*arith.right));
      return DecimalArith(l, r, arith.op, e.type, pool);
    }
    const auto type = plan::ToArrow(e.type);
    // Arrow's kernels take a scalar beside an array, so a constant operand goes as the one value it
    // is. Materialising it instead -- an array of copies of itself, and then a second array when it
    // has to be cast -- cost more than the arithmetic it feeds: over 100M rows one such literal was
    // 0.068s against 0.037s for four additions (ADR 0026's C11). The operations below this that do
    // not go through a kernel (an integer division, a %, HUGEINT) read arrays, and two constants
    // would leave the result a scalar rather than a column, which is why exactly one is taken here;
    // the binder folds an operation of two constants long before this anyway.
    if (arith.op != plan::ArithOp::kIntegerDivide && arith.op != plan::ArithOp::kModulo &&
        e.type != plan::LogicalType::kHugeInt) {
      ARROW_ASSIGN_OR_RAISE(const std::optional<arrow::Datum> left_value,
                            ScalarOperand(*arith.left, type));
      ARROW_ASSIGN_OR_RAISE(const std::optional<arrow::Datum> right_value,
                            ScalarOperand(*arith.right, type));
      if (left_value.has_value() != right_value.has_value()) {
        const plan::Expr& column = left_value.has_value() ? *arith.right : *arith.left;
        ARROW_ASSIGN_OR_RAISE(ArrayPtr values, (*this)(column));
        ARROW_ASSIGN_OR_RAISE(values, CastTo(values, type, ctx));
        const arrow::Datum other{values};
        return KernelArith(left_value.value_or(other), right_value.value_or(other), arith.op,
                           e.type);
      }
    }
    ARROW_ASSIGN_OR_RAISE(ArrayPtr left, (*this)(*arith.left));
    ARROW_ASSIGN_OR_RAISE(ArrayPtr right, (*this)(*arith.right));
    ARROW_ASSIGN_OR_RAISE(left, CastTo(left, type, ctx));
    ARROW_ASSIGN_OR_RAISE(right, CastTo(right, type, ctx));
    if (arith.op == plan::ArithOp::kIntegerDivide || arith.op == plan::ArithOp::kModulo) {
      // DuckDB computes % of DECIMALs beyond 38 digits in DOUBLE, NULL for a zero divisor; NaN
      // only with a DOUBLE operand.
      const bool null_for_zero = arith.left->type != plan::LogicalType::kDouble &&
                                 arith.right->type != plan::LogicalType::kDouble;
      return DivideOrModulo(*left, *right, arith.op, e.type, null_for_zero, pool);
    }
    if (e.type == plan::LogicalType::kHugeInt) {
      return HugeIntArith(*left, right.get(), arith.op, pool);
    }
    return KernelArith(left, right, arith.op, e.type);
  }

  // A constant operand as the one scalar value it is, of `type`; nothing when the operand is not a
  // constant. Where it has to be converted it goes through CastTo on an array of itself alone, so
  // that it converts exactly as a column of it would -- a DECIMAL to DOUBLE is DuckDB's conversion
  // and not Arrow's cast (ADR 0021 rule 8), and a decimal literal beside a DOUBLE column is the
  // common way to meet it (`f + 1.5` is DOUBLE + DECIMAL(2,1)). One row costs nothing beside the
  // batch-long array it replaces.
  arrow::Result<std::optional<arrow::Datum>> ScalarOperand(
      const plan::Expr& expr, const std::shared_ptr<arrow::DataType>& type) const {
    const auto* constant = std::get_if<plan::ConstantExpr>(&expr.node);
    if (constant == nullptr) {
      return std::nullopt;
    }
    ARROW_ASSIGN_OR_RAISE(const std::shared_ptr<arrow::Scalar> scalar,
                          plan::ToArrowScalar(constant->value));
    if (scalar->type->Equals(*type)) {
      return arrow::Datum{scalar};
    }
    ARROW_ASSIGN_OR_RAISE(const ArrayPtr one, arrow::MakeArrayFromScalar(*scalar, 1, pool));
    const arrow::Result<ArrayPtr> converted = CastTo(one, type, ctx);
    if (!converted.ok()) {
      // A constant the result's type cannot hold. The array path converts the batch's own rows, so
      // it raises this for a batch with rows and not for an empty one, where there is no value to
      // convert; one row of it would raise either way. Rather than reason about which batches can
      // be empty, the operand goes back the way it went before and raises what it raised before.
      return std::nullopt;
    }
    ARROW_ASSIGN_OR_RAISE(const std::shared_ptr<arrow::Scalar> value, (*converted)->GetScalar(0));
    return arrow::Datum{value};
  }

  // `left <op> right` through Arrow's own kernel, both already of the operation's type. Either may
  // be a scalar, which the kernels take: that is how a constant operand reaches one without being
  // built into an array of copies of itself (ADR 0026's C11).
  arrow::Result<ArrayPtr> KernelArith(const arrow::Datum& left, const arrow::Datum& right,
                                      plan::ArithOp op, plan::LogicalType type) const {
    const bool integer = plan::IsInteger(type);
    std::string function;
    switch (op) {
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
      return integer && result.status().IsInvalid() ? Overflow(Verb(op), type) : result.status();
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
                           plan::LogicalType::kBoolean));
      // Decided rows get NULL, which the decided side absorbs (false AND NULL, true OR NULL).
      ARROW_ASSIGN_OR_RAISE(const ArrayPtr next, Scatter(unknown, open_rows, compact));
      ARROW_ASSIGN_OR_RAISE(const arrow::Datum both,
                            arrow::compute::CallFunction(function, {result, next}, ctx));
      result = both.make_array();
    }
    return result;
  }

  // The values of `expr` in the rows `mask` (no NULLs) selects, in row order, as `target`; only
  // those rows are computed and cast, so an expression never fails on a row it does not answer (as
  // in DuckDB's CASE).
  arrow::Result<ArrayPtr> EvaluateSelected(const plan::Expr& expr, const arrow::BooleanArray& mask,
                                           plan::LogicalType target) const {
    const int64_t selected = mask.true_count();
    if (selected == batch.num_rows()) {
      ARROW_ASSIGN_OR_RAISE(ArrayPtr values, (*this)(expr));
      return Convert(values, expr, target);
    }
    if (selected == 0) {
      return arrow::MakeArrayOfNull(plan::ToArrow(target), 0, pool);
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
    return Convert(values, expr, target);
  }

  // A join key's cast (ADR 0022): an exact widening, by Convert. A value beyond its own declared
  // width (a file can hold one) still fails a DECIMAL cast, with DuckDB's conversion error; any
  // other cast is a malformed plan.
  arrow::Result<ArrayPtr> Evaluate(const plan::CastExpr& cast, const plan::Expr& e) const {
    if (cast.operand == nullptr) {
      return arrow::Status::Invalid("a cast without an operand");
    }
    if (!plan::IsExactWidening(cast.operand->type, e.type)) {
      return arrow::Status::Invalid("a cast of ", plan::ToString(cast.operand->type), " to ",
                                    plan::ToString(e.type), " is not an exact widening");
    }
    ARROW_ASSIGN_OR_RAISE(const ArrayPtr values, (*this)(*cast.operand));
    return Convert(values, *cast.operand, e.type);
  }

  // The values of `expr` as `target`: to a DECIMAL as DuckDB casts them (CASE values, ADR 0021 rule
  // 10; a failed cast names a column), otherwise as CastTo converts them.
  arrow::Result<ArrayPtr> Convert(const ArrayPtr& values, const plan::Expr& expr,
                                  plan::LogicalType target) const {
    if (target == plan::LogicalType::kDecimal && expr.type != target) {
      const std::string column =
          std::holds_alternative<plan::ColumnExpr>(expr.node) ? expr.name : std::string();
      return CastToDecimal(*values, expr.type, target, column, pool);
    }
    return CastTo(values, plan::ToArrow(target), ctx);
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
    const int64_t n = batch.num_rows();
    ARROW_ASSIGN_OR_RAISE(ArrayPtr result, arrow::MakeArrayOfNull(plan::ToArrow(e.type), n, pool));
    ARROW_ASSIGN_OR_RAISE(ArrayPtr remaining,
                          arrow::MakeArrayFromScalar(arrow::BooleanScalar(true), n, pool));
    ARROW_ASSIGN_OR_RAISE(const ArrayPtr no_condition,
                          arrow::MakeArrayOfNull(arrow::boolean(), n, pool));
    for (std::size_t i = 0; i < c.whens.size(); ++i) {
      const auto& open = static_cast<const arrow::BooleanArray&>(*remaining);
      ARROW_ASSIGN_OR_RAISE(ArrayPtr compact,
                            EvaluateSelected(*c.whens[i], open, plan::LogicalType::kBoolean));
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
                           e.type));
      ARROW_ASSIGN_OR_RAISE(result, Scatter(result, taken_rows, values));
      ARROW_ASSIGN_OR_RAISE(const arrow::Datum rest,
                            arrow::compute::CallFunction("and_not", {remaining, taken_rows}, ctx));
      remaining = rest.make_array();
    }
    if (c.otherwise != nullptr) {
      ARROW_ASSIGN_OR_RAISE(
          ArrayPtr values,
          EvaluateSelected(*c.otherwise, static_cast<const arrow::BooleanArray&>(*remaining),
                           e.type));
      ARROW_ASSIGN_OR_RAISE(result, Scatter(result, remaining, values));
    }
    return result;
  }
};

// `expr` over `batch`, reusing the values of any of `computed` that it is. The public EvaluateExpr
// is this with nothing computed yet.
arrow::Result<ArrayPtr> EvaluateReusing(const plan::Expr& expr, const arrow::RecordBatch& batch,
                                        std::span<const ComputedExpr> computed,
                                        arrow::MemoryPool* pool) {
  arrow::compute::ExecContext ctx(pool);
  ARROW_ASSIGN_OR_RAISE(
      ArrayPtr values,
      (Evaluator{.batch = batch, .pool = pool, .ctx = &ctx, .computed = computed})(expr));
  if (!values->type()->Equals(*plan::ToArrow(expr.type))) {
    return arrow::Status::Invalid("expression of type ", plan::ToString(expr.type),
                                  " evaluated to ", values->type()->ToString());
  }
  return values;
}

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
  return EvaluateReusing(expr, batch, {}, pool);
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
  // Each expression may read the values of an earlier one of this node that it is, rather than
  // computing the same subtree again (ComputedExpr). Order is the node's own, so the error a batch
  // raises does not move.
  std::vector<ComputedExpr> computed;
  computed.reserve(exprs.size());
  for (const plan::ExprPtr& expr : exprs) {
    ARROW_ASSIGN_OR_RAISE(auto values, EvaluateReusing(*expr, *rows, computed, pool));
    computed.push_back(ComputedExpr{.expr = expr.get(), .values = values});
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
