#include "union_all.h"

#include <cstddef>
#include <memory>
#include <utility>
#include <vector>

#include <arrow/api.h>

#include "antb1/common/narrow.h"
#include "antb1/plan/types.h"

namespace antb1::exec {

arrow::Result<std::unique_ptr<UnionAllOperator>> UnionAllOperator::Make(
    std::vector<std::unique_ptr<Operator>> inputs, std::vector<std::vector<int>> columns,
    const std::vector<plan::LogicalType>& types) {
  if (inputs.size() < 2) {
    return arrow::Status::Invalid("UNION ALL of ", inputs.size(), " inputs");
  }
  if (columns.size() != inputs.size()) {
    return arrow::Status::Invalid("UNION ALL: ", columns.size(), " column lists for ",
                                  inputs.size(), " inputs");
  }
  arrow::FieldVector fields;
  fields.reserve(types.size());
  for (std::size_t k = 0; k < inputs.size(); ++k) {
    if (inputs[k] == nullptr) {
      return arrow::Status::Invalid("UNION ALL without its input ", k);
    }
    const arrow::Schema& schema = *inputs[k]->output_schema();
    if (columns[k].size() != types.size()) {
      return arrow::Status::Invalid("UNION ALL: input ", k, " has ", columns[k].size(),
                                    " columns for ", types.size());
    }
    for (std::size_t i = 0; i < types.size(); ++i) {
      const int column = columns[k][i];
      if (column < 0 || column >= schema.num_fields()) {
        return arrow::Status::Invalid("UNION ALL: column ", column, " of input ", k,
                                      " is outside its ", schema.num_fields(), " columns");
      }
      const std::shared_ptr<arrow::DataType> type = plan::ToArrow(types[i]);
      if (type == nullptr || !schema.field(column)->type()->Equals(*type)) {
        return arrow::Status::Invalid("UNION ALL: column ", i, " of input ", k, " is ",
                                      schema.field(column)->type()->ToString(), ", not ",
                                      type == nullptr ? "a type" : type->ToString());
      }
      if (k == 0) {
        fields.push_back(arrow::field(schema.field(column)->name(), type, /*nullable=*/true));
      }
    }
  }
  return std::unique_ptr<UnionAllOperator>(new UnionAllOperator(
      std::move(inputs), std::move(columns), arrow::schema(std::move(fields))));
}

UnionAllOperator::UnionAllOperator(std::vector<std::unique_ptr<Operator>> inputs,
                                   std::vector<std::vector<int>> columns,
                                   std::shared_ptr<arrow::Schema> schema)
    : inputs_(std::move(inputs)), columns_(std::move(columns)), schema_(std::move(schema)) {}

arrow::Status UnionAllOperator::Open(ExecContext& ctx) {
  ARROW_RETURN_NOT_OK(Close());
  ctx_ = ctx;
  opened_ = true;
  return arrow::Status::OK();
}

arrow::Result<Batch> UnionAllOperator::Next() {
  if (!opened_) {
    return arrow::Status::Invalid("UNION ALL: Next() before Open() or after Close()");
  }
  while (current_ < inputs_.size()) {
    Operator& input = *inputs_[current_];
    if (!input_open_) {
      input_open_ = true;  // Close closes it even if its Open fails
      ARROW_RETURN_NOT_OK(input.Open(ctx_));
    }
    ARROW_ASSIGN_OR_RAISE(Batch batch, input.Next());
    if (batch.end()) {
      // Closed before the next input opens: its builds and its parts' windows are gone by then.
      input_open_ = false;
      ++current_;
      ARROW_RETURN_NOT_OK(input.Close());
      continue;
    }
    const std::vector<int>& picked = columns_[current_];
    std::vector<std::shared_ptr<arrow::Array>> data;
    data.reserve(picked.size());
    for (std::size_t i = 0; i < picked.size(); ++i) {
      const int column = picked[i];
      if (column >= batch.data->num_columns() ||
          !batch.data->column(column)->type()->Equals(*schema_->field(Narrow<int>(i))->type())) {
        return arrow::Status::Invalid("UNION ALL: input ", current_, " gave a batch whose column ",
                                      column, " is not of its schema");
      }
      data.push_back(batch.data->column(column));
    }
    return Batch{.data = arrow::RecordBatch::Make(schema_, batch.data->num_rows(), std::move(data)),
                 .selection = std::move(batch.selection)};
  }
  return Batch{};
}

arrow::Status UnionAllOperator::Close() {
  arrow::Status status;
  if (input_open_) {
    input_open_ = false;
    status = inputs_[current_]->Close();  // stops its parts and waits for them
  }
  current_ = 0;
  opened_ = false;
  return status;
}

}  // namespace antb1::exec
