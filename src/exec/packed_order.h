#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "antb1/exec/sort.h"

namespace antb1::exec {

// Most-significant 128 bits of the SQL ordering. On a collision, comparison resumes at the
// first field whose full value was not encoded. The same layout applies to every input batch.
class PackedOrder {
 public:
  struct Entry {
    std::uint64_t high;
    std::uint64_t low;
    SortBuffer::RowRef ref;
  };

  static constexpr std::size_t kMinRows = 2048;
  PackedOrder(const RowComparator& comparator, std::span<const RowComparator::KeyArrays> batches,
              std::size_t first_key = 0);
  [[nodiscard]] bool empty() const { return count_ == 0; }
  [[nodiscard]] bool exact() const { return resume_ == comparator_.keys_.size(); }
  [[nodiscard]] Entry Encode(const RowComparator::KeyArrays& arrays, SortBuffer::RowRef ref) const;
  void Sort(std::span<Entry> entries, std::span<const RowComparator::KeyArrays> batches) const;

  // The input order is untouched if the query pool refuses scratch.
  arrow::Result<bool> TrySort(std::span<SortBuffer::RowRef> rows,
                              std::span<const RowComparator::KeyArrays> batches,
                              arrow::MemoryPool* pool) const;

 private:
  struct Field {
    std::size_t index;
    UInt128 mask;
    UInt128 null_tag;
    UInt128 valid_tag;
    std::uint64_t sign_flip;
    unsigned discard;
    unsigned shift;
    bool wide_decimal;
  };

  struct CompactField {
    std::size_t index;
    std::uint64_t base;
    std::uint64_t mask;
    unsigned shift;
    arrow::Type::type type;
  };
  bool CompactKey(const RowComparator::KeyArrays& arrays, SortBuffer::RowRef row,
                  std::span<const std::string_view> dictionary, std::uint64_t& packed) const;

  std::array<CompactField, 4> compact_;
  std::size_t compact_count_ = 0;
  const RowComparator& comparator_;
  // The narrowest encodable value occupies 16 bits.
  std::array<Field, 8> fields_;
  std::size_t count_ = 0;
  std::size_t resume_ = 0;
};

}  // namespace antb1::exec
