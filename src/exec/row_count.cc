#include "antb1/exec/row_count.h"

#include <memory>
#include <string>
#include <utility>

#include <arrow/api.h>

namespace antb1::exec {

RowCountOperator::RowCountOperator(std::string column_name, int64_t row_count)
    : schema_(arrow::schema(
          {arrow::field(std::move(column_name), arrow::int64(), /*nullable=*/false)})),
      row_count_(row_count) {}

arrow::Status RowCountOperator::Open(ExecContext& ctx) {
  pool_ = ctx.pool;
  done_ = false;
  return arrow::Status::OK();
}

arrow::Result<Batch> RowCountOperator::Next() {
  if (done_) {
    return Batch{};
  }
  done_ = true;
  arrow::Int64Builder builder(pool_);
  ARROW_RETURN_NOT_OK(builder.Append(row_count_));
  ARROW_ASSIGN_OR_RAISE(auto array, builder.Finish());
  return Batch{.data = arrow::RecordBatch::Make(schema_, 1, {std::move(array)}), .selection = {}};
}

}  // namespace antb1::exec
