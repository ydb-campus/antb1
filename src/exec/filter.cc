#include "antb1/exec/filter.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <arrow/api.h>
#include <arrow/compute/api_scalar.h>
#include <arrow/compute/exec.h>

#include "antb1/plan/logical_plan.h"

namespace antb1::exec {
namespace {

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

FilterOperator::FilterOperator(std::unique_ptr<Operator> input,
                               std::vector<plan::Predicate> predicates)
    : input_(std::move(input)), predicates_(std::move(predicates)) {
  never_true_ = std::ranges::any_of(predicates_, [](const plan::Predicate& p) {
    return p.kind == plan::Predicate::Kind::kFalse;
  });
}

arrow::Status FilterOperator::Open(ExecContext& ctx) {
  pool_ = ctx.pool;
  columns_.clear();
  constants_.clear();
  const arrow::Schema& schema = *input_->output_schema();
  for (const plan::Predicate& p : predicates_) {
    if (p.kind == plan::Predicate::Kind::kFalse) {
      columns_.push_back(-1);  // never evaluated: the stream ends before any batch
      constants_.emplace_back();
      continue;
    }
    if (!p.column.has_value() || p.column->index < 0 || p.column->index >= schema.num_fields()) {
      return arrow::Status::Invalid("filter predicate on a column outside its input");
    }
    const int column = p.column->index;
    std::shared_ptr<arrow::Scalar> constant;
    if (p.kind == plan::Predicate::Kind::kCompare) {
      ARROW_ASSIGN_OR_RAISE(constant, plan::ToArrowScalar(p.constant));
      const auto& column_type = *schema.field(column)->type();
      if (!constant->type->Equals(column_type)) {
        return arrow::Status::Invalid("filter compares a ", column_type.ToString(),
                                      " column with a ", constant->type->ToString(), " constant");
      }
    }
    columns_.push_back(column);
    constants_.push_back(std::move(constant));
  }
  return input_->Open(ctx);
}

arrow::Result<std::shared_ptr<arrow::Array>> FilterOperator::Evaluate(
    const arrow::RecordBatch& batch) const {
  arrow::compute::ExecContext kernels(pool_);
  arrow::Datum mask;
  for (std::size_t i = 0; i < predicates_.size(); ++i) {
    const plan::Predicate& p = predicates_[i];
    const arrow::Datum column(batch.column(columns_[i]));
    arrow::Datum result;
    if (p.kind == plan::Predicate::Kind::kCompare) {
      ARROW_ASSIGN_OR_RAISE(result,
                            arrow::compute::CallFunction(std::string(KernelName(p.op)),
                                                         {column, constants_[i]}, &kernels));
    } else {  // kIsNotNull (kFalse never gets here)
      ARROW_ASSIGN_OR_RAISE(result, arrow::compute::CallFunction("is_valid", {column}, &kernels));
    }
    if (mask.is_value()) {
      ARROW_ASSIGN_OR_RAISE(mask,
                            arrow::compute::CallFunction("and_kleene", {mask, result}, &kernels));
    } else {
      mask = std::move(result);
    }
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
