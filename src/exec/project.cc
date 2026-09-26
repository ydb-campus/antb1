#include "antb1/exec/project.h"

#include <memory>
#include <utility>
#include <vector>

#include <arrow/api.h>

namespace antb1::exec {
namespace {

std::shared_ptr<arrow::Schema> ColumnsOf(const arrow::Schema& input,
                                         const std::vector<int>& columns) {
  arrow::FieldVector fields;
  fields.reserve(columns.size());
  for (const int c : columns) {
    // An index outside the input makes Open fail.
    fields.push_back(c >= 0 && c < input.num_fields() ? input.field(c)
                                                      : arrow::field("?", arrow::null()));
  }
  return arrow::schema(std::move(fields));
}

}  // namespace

ProjectOperator::ProjectOperator(std::unique_ptr<Operator> input, std::vector<int> columns)
    : input_(std::move(input)),
      columns_(std::move(columns)),
      schema_(ColumnsOf(*input_->output_schema(), columns_)) {}

arrow::Status ProjectOperator::Open(ExecContext& ctx) {
  pool_ = ctx.pool;
  const int width = input_->output_schema()->num_fields();
  for (const int c : columns_) {
    if (c < 0 || c >= width) {
      return arrow::Status::Invalid("projection of column ", c, " of an input with ", width,
                                    " columns");
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
    arrow::ArrayVector arrays;
    arrays.reserve(columns_.size());
    for (const int c : columns_) {
      arrays.push_back(in.data->column(c));
    }
    const Batch projected{
        .data = arrow::RecordBatch::Make(schema_, in.data->num_rows(), std::move(arrays)),
        .selection = in.selection};
    ARROW_ASSIGN_OR_RAISE(auto rows, Materialize(projected, pool_));
    if (rows->num_rows() > 0) {
      return Batch{.data = std::move(rows), .selection = {}};
    }
  }
}

}  // namespace antb1::exec
