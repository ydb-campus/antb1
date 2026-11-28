#include "antb1/exec/project.h"

#include <algorithm>
#include <cstddef>
#include <memory>
#include <utility>
#include <vector>

#include <arrow/api.h>

namespace antb1::exec {
namespace {

std::shared_ptr<arrow::Schema> ColumnsOf(
    const arrow::Schema& input, const std::vector<int>& columns,
    const std::vector<std::shared_ptr<arrow::Scalar>>& constants) {
  arrow::FieldVector fields;
  fields.reserve(columns.size());
  for (std::size_t i = 0; i < columns.size(); ++i) {
    if (i < constants.size() && constants[i] != nullptr) {
      fields.push_back(arrow::field("c" + std::to_string(i), constants[i]->type));
      continue;
    }
    const int c = columns[i];
    // An index outside the input makes Open fail.
    fields.push_back(c >= 0 && c < input.num_fields() ? input.field(c)
                                                      : arrow::field("?", arrow::null()));
  }
  return arrow::schema(std::move(fields));
}

}  // namespace

ProjectOperator::ProjectOperator(std::unique_ptr<Operator> input, std::vector<int> columns,
                                 std::vector<std::shared_ptr<arrow::Scalar>> constants)
    : input_(std::move(input)),
      columns_(std::move(columns)),
      constants_(std::move(constants)),
      schema_(ColumnsOf(*input_->output_schema(), columns_, constants_)) {}

const BatchPartition* ProjectOperator::RequestPartitioning(int column, PartitionRequest request) {
  if (column < 0 || std::cmp_greater_equal(column, columns_.size())) {
    return nullptr;
  }
  const auto field = static_cast<std::size_t>(column);
  if (field < constants_.size() && constants_[field] != nullptr) {
    return nullptr;
  }
  return input_->RequestPartitioning(columns_[field], request);
}

std::size_t ProjectOperator::ReadyBatches(std::size_t limit, int64_t min_rows,
                                          int64_t max_rows) const {
  // An unselected input only needs its existing arrays reordered. Broadcasting a constant
  // would allocate an array even when every input row is already materialized.
  if (std::ranges::any_of(constants_, [](const auto& value) { return value != nullptr; })) {
    return 0;
  }
  return input_->ReadyBatches(limit, min_rows, max_rows);
}

arrow::Status ProjectOperator::Open(ExecContext& ctx) {
  pool_ = ctx.pool;
  if (!constants_.empty() && constants_.size() != columns_.size()) {
    return arrow::Status::Invalid("projection with ", constants_.size(), " constants for ",
                                  columns_.size(), " columns");
  }
  const int width = input_->output_schema()->num_fields();
  for (std::size_t i = 0; i < columns_.size(); ++i) {
    const bool constant = !constants_.empty() && constants_[i] != nullptr;
    if (!constant && (columns_[i] < 0 || columns_[i] >= width)) {
      return arrow::Status::Invalid("projection of column ", columns_[i], " of an input with ",
                                    width, " columns");
    }
  }
  return input_->Open(ctx);
}

arrow::Result<Batch> ProjectOperator::Next() {
  while (true) {
    ARROW_ASSIGN_OR_RAISE(Batch in, input_->Next());
    if (in.end()) {
      return in;
    }
    const auto is_constant = [&](std::size_t i) {
      return !constants_.empty() && constants_[i] != nullptr;
    };
    // Only the listed input columns are materialized; a projection of constants only needs the
    // number of selected rows.
    arrow::FieldVector fields;
    arrow::ArrayVector listed;
    for (std::size_t i = 0; i < columns_.size(); ++i) {
      if (!is_constant(i)) {
        fields.push_back(schema_->field(static_cast<int>(i)));
        listed.push_back(in.data->column(columns_[i]));
      }
    }
    const Batch projected{.data = arrow::RecordBatch::Make(arrow::schema(std::move(fields)),
                                                           in.data->num_rows(), std::move(listed)),
                          .selection = in.selection};
    ARROW_ASSIGN_OR_RAISE(auto rows, Materialize(projected, pool_));
    if (rows->num_rows() == 0) {
      continue;
    }
    if (constants_.empty()) {
      return Batch{.data = arrow::RecordBatch::Make(schema_, rows->num_rows(), rows->columns()),
                   .selection = {}};
    }
    arrow::ArrayVector arrays;
    arrays.reserve(columns_.size());
    int next = 0;
    for (std::size_t i = 0; i < columns_.size(); ++i) {
      if (is_constant(i)) {
        ARROW_ASSIGN_OR_RAISE(auto constant,
                              arrow::MakeArrayFromScalar(*constants_[i], rows->num_rows(), pool_));
        arrays.push_back(std::move(constant));
      } else {
        arrays.push_back(rows->column(next++));
      }
    }
    return Batch{.data = arrow::RecordBatch::Make(schema_, rows->num_rows(), std::move(arrays)),
                 .selection = {}};
  }
}

}  // namespace antb1::exec
