#include "row_mask.h"

#include <array>
#include <cstddef>
#include <cstdint>

#include <arrow/api.h>
#include <arrow/util/bitmap_ops.h>

namespace antb1::exec {
namespace {

struct Bitmap {
  const uint8_t* bits = nullptr;
  int64_t offset = 0;
};

}  // namespace

arrow::Result<RowMask> RowMask::Make(const arrow::Array* values,
                                     const arrow::BooleanArray* selection, int64_t length,
                                     arrow::MemoryPool* pool) {
  RowMask mask;
  mask.length_ = length;
  if (length == 0) {
    return mask;  // nothing to visit (a zero-length array may have no buffers)
  }
  std::array<Bitmap, 3> bitmaps{};
  std::size_t n = 0;
  if (values != nullptr && values->null_count() > 0) {
    bitmaps.at(n++) = Bitmap{.bits = values->null_bitmap_data(), .offset = values->offset()};
  }
  if (selection != nullptr) {
    bitmaps.at(n++) = Bitmap{.bits = selection->values()->data(), .offset = selection->offset()};
    if (selection->null_count() > 0) {
      bitmaps.at(n++) =
          Bitmap{.bits = selection->null_bitmap_data(), .offset = selection->offset()};
    }
  }
  if (n == 0) {
    return mask;
  }
  mask.bits_ = bitmaps[0].bits;
  mask.offset_ = bitmaps[0].offset;
  for (std::size_t i = 1; i < n; ++i) {
    ARROW_ASSIGN_OR_RAISE(
        mask.owned_, arrow::internal::BitmapAnd(pool, mask.bits_, mask.offset_, bitmaps.at(i).bits,
                                                bitmaps.at(i).offset, length, /*out_offset=*/0));
    mask.bits_ = mask.owned_->data();
    mask.offset_ = 0;
  }
  return mask;
}

int64_t RowMask::CountRows() const {
  return bits_ == nullptr ? length_ : arrow::internal::CountSetBits(bits_, offset_, length_);
}

}  // namespace antb1::exec
