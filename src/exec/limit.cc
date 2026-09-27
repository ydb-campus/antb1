#include "antb1/exec/limit.h"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <optional>
#include <utility>

#include <arrow/api.h>

namespace antb1::exec {
namespace {

// The rows `skip` .. `skip + take - 1` of those that take part in `in` (0 < take, skip + take <=
// in.selected_rows()), as a batch over the smallest range of rows that holds them.
Batch Window(const Batch& in, int64_t skip, int64_t take) {
  if (in.selection == nullptr) {
    return Batch{.data = in.data->Slice(skip, take), .selection = {}};
  }
  const arrow::BooleanArray& selection = *in.selection;
  int64_t first = -1;
  int64_t last = -1;
  int64_t seen = 0;
  for (int64_t i = 0; i < selection.length() && last < 0; ++i) {
    if (!selection.Value(i)) {
      continue;
    }
    if (seen == skip) {
      first = i;
    }
    if (seen == skip + take - 1) {
      last = i;
    }
    ++seen;
  }
  const int64_t length = last - first + 1;
  return Batch{.data = in.data->Slice(first, length),
               .selection = std::static_pointer_cast<arrow::BooleanArray>(
                   in.selection->Slice(first, length))};
}

}  // namespace

LimitOperator::LimitOperator(std::unique_ptr<Operator> input, std::optional<int64_t> limit,
                             int64_t offset)
    : input_(std::move(input)), limit_(limit), offset_(offset) {}

arrow::Status LimitOperator::Open(ExecContext& ctx) {
  if (limit_.has_value() && *limit_ < 0) {
    return arrow::Status::Invalid("LIMIT ", *limit_, " is negative");
  }
  if (offset_ < 0) {
    return arrow::Status::Invalid("OFFSET ", offset_, " is negative");
  }
  skipped_ = 0;
  emitted_ = 0;
  return input_->Open(ctx);
}

arrow::Result<Batch> LimitOperator::Next() {
  while (!limit_.has_value() || emitted_ < *limit_) {
    ARROW_ASSIGN_OR_RAISE(Batch in, input_->Next());
    if (in.end()) {
      return in;
    }
    const int64_t rows = in.selected_rows();
    const int64_t skip = std::min(offset_ - skipped_, rows);
    skipped_ += skip;
    int64_t take = rows - skip;
    if (limit_.has_value()) {
      take = std::min(take, *limit_ - emitted_);
    }
    if (take == 0) {
      continue;
    }
    emitted_ += take;
    if (skip == 0 && take == rows) {
      return in;
    }
    return Window(in, skip, take);
  }
  return Batch{};  // the limit is reached: the input is not pulled again
}

}  // namespace antb1::exec
