#pragma once

#include <cstdint>
#include <memory>

#include <arrow/array/array_base.h>
#include <arrow/array/array_primitive.h>
#include <arrow/buffer.h>
#include <arrow/memory_pool.h>
#include <arrow/result.h>
#include <arrow/util/bit_run_reader.h>

// Private to exec: the rows of one batch that an aggregate reads.

namespace antb1::exec {

// The rows of a batch that are selected (Batch::selection) and, for an aggregate over a column,
// not NULL: the AND of the column's validity bitmap, the selection's values and the selection's
// validity (a NULL in a selection counts as false). Bitmaps are borrowed while one suffices, so the
// arrays passed to Make must outlive the mask.
class RowMask {
 public:
  // values: the aggregate's argument, or nullptr (COUNT(*)); selection: nullptr for every row.
  static arrow::Result<RowMask> Make(const arrow::Array* values,
                                     const arrow::BooleanArray* selection, int64_t length,
                                     arrow::MemoryPool* pool);

  [[nodiscard]] int64_t length() const { return length_; }
  [[nodiscard]] bool all() const { return bits_ == nullptr; }
  // The number of rows in the mask.
  [[nodiscard]] int64_t CountRows() const;

  // Calls visit(position, count) for every run of consecutive rows in the mask, in order.
  template <class Visit>
  void ForEachRun(Visit&& visit) const {
    if (length_ == 0) {
      return;
    }
    arrow::internal::VisitSetBitRunsVoid(bits_, offset_, length_, visit);
  }

 private:
  const uint8_t* bits_ = nullptr;  // nullptr: every row
  int64_t offset_ = 0;
  int64_t length_ = 0;
  std::shared_ptr<arrow::Buffer> owned_;  // the AND of several bitmaps
};

}  // namespace antb1::exec
