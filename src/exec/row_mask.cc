#include "row_mask.h"

#include <cstdint>
#include <span>

#include <arrow/api.h>
#include <arrow/util/bitmap_ops.h>

namespace antb1::exec {

arrow::Result<RowMask> RowMask::Make(const arrow::Array* values,
                                     const arrow::BooleanArray* selection, int64_t length,
                                     arrow::MemoryPool* pool) {
  if (values == nullptr) {  // COUNT(*): an empty span, never a nullptr entry
    return Make(std::span<const arrow::Array* const>(), selection, length, pool);
  }
  return Make(std::span<const arrow::Array* const>(&values, 1), selection, length, pool);
}

arrow::Result<RowMask> RowMask::Make(std::span<const arrow::Array* const> values,
                                     const arrow::BooleanArray* selection, int64_t length,
                                     arrow::MemoryPool* pool) {
  RowMask mask;
  mask.length_ = length;
  if (length == 0) {
    return mask;  // nothing to visit (a zero-length array may have no buffers)
  }
  // Each bitmap as it is found: the first is borrowed, the AND with every other one is owned.
  const auto fold = [&mask, length, pool](const uint8_t* bits, int64_t offset) -> arrow::Status {
    if (mask.bits_ == nullptr) {
      mask.bits_ = bits;
      mask.offset_ = offset;
      return arrow::Status::OK();
    }
    ARROW_ASSIGN_OR_RAISE(mask.owned_, arrow::internal::BitmapAnd(pool, mask.bits_, mask.offset_,
                                                                  bits, offset, length,
                                                                  /*out_offset=*/0));
    mask.bits_ = mask.owned_->data();
    mask.offset_ = 0;
    return arrow::Status::OK();
  };
  for (const arrow::Array* array : values) {
    if (array != nullptr && array->null_count() > 0) {
      ARROW_RETURN_NOT_OK(fold(array->null_bitmap_data(), array->offset()));
    }
  }
  if (selection != nullptr) {
    ARROW_RETURN_NOT_OK(fold(selection->values()->data(), selection->offset()));
    if (selection->null_count() > 0) {
      ARROW_RETURN_NOT_OK(fold(selection->null_bitmap_data(), selection->offset()));
    }
  }
  return mask;
}

int64_t RowMask::CountRows() const {
  return bits_ == nullptr ? length_ : arrow::internal::CountSetBits(bits_, offset_, length_);
}

}  // namespace antb1::exec
