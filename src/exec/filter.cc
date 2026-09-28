#include "antb1/exec/filter.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <arrow/api.h>
#include <arrow/compute/api_scalar.h>
#include <arrow/compute/cast.h>
#include <arrow/compute/exec.h>

#include "antb1/plan/logical_plan.h"

namespace antb1::exec {
namespace {

using ArrayPtr = std::shared_ptr<arrow::Array>;

std::string_view KernelName(plan::CompareOp op) {
  switch (op) {
    case plan::CompareOp::kEq:
      return "equal";
    case plan::CompareOp::kNe:
      return "not_equal";
    case plan::CompareOp::kLt:
      return "less";
    case plan::CompareOp::kLe:
      return "less_equal";
    case plan::CompareOp::kGt:
      return "greater";
    case plan::CompareOp::kGe:
      return "greater_equal";
  }
  return "equal";
}

}  // namespace

arrow::Result<PredicateEvaluator> PredicateEvaluator::Make(const plan::Predicate& p,
                                                           const arrow::Schema& schema) {
  PredicateEvaluator out;
  out.predicate_ = p;
  const auto in_range = [&](int index) { return index >= 0 && index < schema.num_fields(); };
  if (!p.column.has_value()) {
    if (p.kind == plan::Predicate::Kind::kFalse) {
      return out;
    }
    return arrow::Status::Invalid("filter predicate without a column");
  }
  if (!in_range(p.column->index)) {
    return arrow::Status::Invalid("filter predicate on a column outside its input");
  }
  out.column_ = p.column->index;
  const arrow::DataType& column_type = *schema.field(out.column_)->type();
  if (p.kind == plan::Predicate::Kind::kCompareColumns) {
    if (!p.other.has_value() || !in_range(p.other->index)) {
      return arrow::Status::Invalid("filter compares a column outside its input");
    }
    out.other_ = p.other->index;
  }
  if (p.kind == plan::Predicate::Kind::kIsTrue && column_type.id() != arrow::Type::BOOL) {
    return arrow::Status::Invalid("IS TRUE of a ", column_type.ToString(), " column");
  }
  if (p.kind == plan::Predicate::Kind::kCompare) {
    ARROW_ASSIGN_OR_RAISE(out.constant_, plan::ToArrowScalar(p.constant));
    if (!out.constant_->type->Equals(column_type)) {
      return arrow::Status::Invalid("filter compares a ", column_type.ToString(), " column with a ",
                                    out.constant_->type->ToString(), " constant");
    }
  }
  if (p.kind == plan::Predicate::Kind::kLike || p.kind == plan::Predicate::Kind::kNotLike) {
    const auto* text = std::get_if<std::string>(&p.constant.value);
    if (text == nullptr || column_type.id() != arrow::Type::BINARY) {
      return arrow::Status::Invalid("LIKE needs a VARCHAR column and a VARCHAR pattern");
    }
    out.pattern_.emplace(*text);
  }
  if (p.kind == plan::Predicate::Kind::kIn || p.kind == plan::Predicate::Kind::kNotIn) {
    if (p.values.empty()) {
      return arrow::Status::Invalid("IN without values");
    }
    for (const plan::Constant& value : p.values) {
      ARROW_ASSIGN_OR_RAISE(auto scalar, plan::ToArrowScalar(value));
      if (!scalar->type->Equals(column_type)) {
        return arrow::Status::Invalid("filter compares a ", column_type.ToString(),
                                      " column with a ", scalar->type->ToString(), " IN value");
      }
      out.values_.push_back(std::move(scalar));
    }
  }
  return out;
}

arrow::Result<arrow::Datum> PredicateEvaluator::Evaluate(const arrow::RecordBatch& batch,
                                                         arrow::MemoryPool* pool,
                                                         bool kleene) const {
  arrow::compute::ExecContext kernels(pool);
  const plan::Predicate& p = predicate_;
  if (column_ < 0) {  // kFalse without a column
    return arrow::Datum(std::make_shared<arrow::BooleanScalar>(false));
  }
  arrow::Datum column(batch.column(column_));
  switch (p.kind) {
    case plan::Predicate::Kind::kCompare:
      return arrow::compute::CallFunction(std::string(KernelName(p.op)), {column, constant_},
                                          &kernels);
    case plan::Predicate::Kind::kCompareColumns: {
      // Arrow compares numbers of two types in their common type, as DuckDB does; with a DOUBLE
      // that is DOUBLE, and a BIGINT beyond 2^53 rounds to it (Arrow's implicit cast refuses).
      arrow::Datum left = column;
      arrow::Datum right(batch.column(other_));
      const bool left_double = left.type()->id() == arrow::Type::DOUBLE;
      const bool right_double = right.type()->id() == arrow::Type::DOUBLE;
      if (left_double != right_double) {
        arrow::compute::CastOptions to_double = arrow::compute::CastOptions::Safe(arrow::float64());
        to_double.allow_float_truncate = true;
        to_double.allow_decimal_truncate = true;
        ARROW_ASSIGN_OR_RAISE(
            (left_double ? right : left),
            arrow::compute::Cast(left_double ? right : left, to_double, &kernels));
      }
      return arrow::compute::CallFunction(std::string(KernelName(p.op)), {left, right}, &kernels);
    }
    case plan::Predicate::Kind::kLike:
    case plan::Predicate::Kind::kNotLike: {
      if (!pattern_.has_value()) {
        return arrow::Status::Invalid("LIKE without a pattern");
      }
      ARROW_ASSIGN_OR_RAISE(
          const ArrayPtr result,
          pattern_->Evaluate(static_cast<const arrow::BinaryArray&>(*batch.column(column_)),
                             p.kind == plan::Predicate::Kind::kNotLike, pool));
      return arrow::Datum(result);
    }
    case plan::Predicate::Kind::kIn:
    case plan::Predicate::Kind::kNotIn: {  // equal to any value (Kleene OR)
      arrow::Datum any;
      for (const auto& value : values_) {
        ARROW_ASSIGN_OR_RAISE(arrow::Datum equal,
                              arrow::compute::CallFunction("equal", {column, value}, &kernels));
        if (any.is_value()) {
          ARROW_ASSIGN_OR_RAISE(any,
                                arrow::compute::CallFunction("or_kleene", {any, equal}, &kernels));
        } else {
          any = std::move(equal);
        }
      }
      if (p.kind == plan::Predicate::Kind::kNotIn) {
        return arrow::compute::CallFunction("invert", {any}, &kernels);
      }
      return any;
    }
    case plan::Predicate::Kind::kIsTrue:
      return column;
    case plan::Predicate::Kind::kIsNotNull:
    case plan::Predicate::Kind::kFalse: {
      ARROW_ASSIGN_OR_RAISE(arrow::Datum valid,
                            arrow::compute::CallFunction("is_valid", {column}, &kernels));
      const bool is_not_null = p.kind == plan::Predicate::Kind::kIsNotNull;
      if (!kleene) {
        return is_not_null ? valid : arrow::Datum(std::make_shared<arrow::BooleanScalar>(false));
      }
      // True (kIsNotNull) or false (kFalse) where the column is not NULL, else NULL.
      return arrow::compute::CallFunction(
          "if_else",
          {valid, std::make_shared<arrow::BooleanScalar>(is_not_null),
           std::make_shared<arrow::BooleanScalar>()},
          &kernels);
    }
  }
  return arrow::Status::Invalid("unknown predicate kind");
}

FilterOperator::FilterOperator(std::unique_ptr<Operator> input,
                               std::vector<plan::Predicate> predicates)
    : input_(std::move(input)), predicates_(std::move(predicates)) {
  never_true_ = std::ranges::any_of(predicates_, [](const plan::Predicate& p) {
    return p.kind == plan::Predicate::Kind::kFalse;
  });
}

arrow::Status FilterOperator::Open(ExecContext& ctx) {
  pool_ = ctx.pool;
  evaluators_.clear();
  if (!never_true_) {  // else never evaluated: the stream ends before any batch
    for (const plan::Predicate& p : predicates_) {
      ARROW_ASSIGN_OR_RAISE(auto evaluator, PredicateEvaluator::Make(p, *input_->output_schema()));
      evaluators_.push_back(std::move(evaluator));
    }
  }
  return input_->Open(ctx);
}

arrow::Result<std::shared_ptr<arrow::Array>> FilterOperator::Evaluate(
    const arrow::RecordBatch& batch) const {
  arrow::compute::ExecContext kernels(pool_);
  arrow::Datum mask;
  for (const PredicateEvaluator& evaluator : evaluators_) {
    ARROW_ASSIGN_OR_RAISE(arrow::Datum result, evaluator.Evaluate(batch, pool_, /*kleene=*/false));
    if (mask.is_value()) {
      ARROW_ASSIGN_OR_RAISE(mask,
                            arrow::compute::CallFunction("and_kleene", {mask, result}, &kernels));
    } else {
      mask = std::move(result);
    }
  }
  if (mask.is_scalar()) {  // a single kIsNotNull cannot be a scalar; kFalse ends the stream
    ARROW_ASSIGN_OR_RAISE(auto array,
                          arrow::MakeArrayFromScalar(*mask.scalar(), batch.num_rows(), pool_));
    return array;
  }
  return mask.make_array();
}

arrow::Result<Batch> FilterOperator::Next() {
  if (never_true_) {
    return Batch{};  // no row can pass: nothing is read
  }
  while (true) {
    ARROW_ASSIGN_OR_RAISE(Batch in, input_->Next());
    if (in.end() || predicates_.empty()) {
      return in;
    }
    ARROW_ASSIGN_OR_RAISE(std::shared_ptr<arrow::Array> mask, Evaluate(*in.data));
    arrow::compute::ExecContext kernels(pool_);
    if (in.selection != nullptr) {
      ARROW_ASSIGN_OR_RAISE(
          const arrow::Datum both,
          arrow::compute::CallFunction("and_kleene", {in.selection, mask}, &kernels));
      mask = both.make_array();
    }
    if (mask->null_count() > 0) {
      // NULL rejects the row: NULL AND FALSE is FALSE, and is_valid is FALSE exactly there.
      ARROW_ASSIGN_OR_RAISE(const arrow::Datum valid,
                            arrow::compute::CallFunction("is_valid", {mask}, &kernels));
      ARROW_ASSIGN_OR_RAISE(const arrow::Datum normalized,
                            arrow::compute::CallFunction("and_kleene", {mask, valid}, &kernels));
      mask = normalized.make_array();
    }
    auto selection = std::static_pointer_cast<arrow::BooleanArray>(std::move(mask));
    const int64_t selected = selection->true_count();
    if (selected == 0) {
      continue;
    }
    if (selected == in.data->num_rows()) {
      return Batch{.data = std::move(in.data), .selection = {}};
    }
    return Batch{.data = std::move(in.data), .selection = std::move(selection)};
  }
}

}  // namespace antb1::exec
