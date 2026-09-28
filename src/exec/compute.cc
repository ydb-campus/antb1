#include "antb1/exec/compute.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
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
#include "antb1/plan/literal.h"
#include "antb1/plan/logical_plan.h"
#include "antb1/plan/types.h"

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

  arrow::Result<ArrayPtr> Evaluate(const plan::ArithExpr& arith, const plan::Expr& e) const {
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

ComputeOperator::ComputeOperator(std::unique_ptr<Operator> input, std::vector<plan::ExprPtr> exprs)
    : input_(std::move(input)),
      exprs_(std::move(exprs)),
      schema_(arrow::schema(ComputedFields(*input_->output_schema(), exprs_))) {}

arrow::Status ComputeOperator::Open(ExecContext& ctx) {
  pool_ = ctx.pool;
  return input_->Open(ctx);
}

arrow::Result<Batch> ComputeOperator::Next() {
  ARROW_ASSIGN_OR_RAISE(Batch in, input_->Next());
  if (in.end()) {
    return in;
  }
  // Only the selected rows are computed: an expression never fails on a row a filter dropped.
  ARROW_ASSIGN_OR_RAISE(auto rows, Materialize(in, pool_));
  arrow::ArrayVector columns = rows->columns();
  for (const plan::ExprPtr& expr : exprs_) {
    ARROW_ASSIGN_OR_RAISE(auto values, EvaluateExpr(*expr, *rows, pool_));
    columns.push_back(std::move(values));
  }
  return Batch{.data = arrow::RecordBatch::Make(schema_, rows->num_rows(), std::move(columns)),
               .selection = {}};
}

}  // namespace antb1::exec
