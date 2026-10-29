#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <type_traits>
#include <utility>

#include <arrow/buffer.h>
#include <arrow/memory_pool.h>
#include <arrow/result.h>
#include <arrow/status.h>

#include "antb1/common/check.h"

namespace antb1::exec {

// An array of trivially copyable values in a buffer of an Arrow memory pool, for the tables that
// charge their memory to the query's budget by allocating from it: when the pool is the session's
// MemoryBudget, as it is in the engine, every allocation is charged before it happens and refused
// with the budget's own OutOfMemory, so the table keeps no account of its own
// (docs/adr/0019-own-aggregation-hash-table.md). Unlike std::vector:
// - new elements are uninitialized (Assign fills them);
// - the capacity is the buffer's: Arrow rounds every reservation up to a multiple of 64 bytes, and
//   the pool is charged that capacity; it never shrinks until Reset;
// - growth is the caller's: Reserve sets the capacity, and push_back stays within it;
// - a refused allocation leaves the vector as it was;
// - Reserve, Resize and Assign may still throw std::bad_alloc for Arrow's small buffer object,
//   which is not the pool's: a caller's entry point turns that into OutOfMemory (key_table.cc).
// The 64-byte rounding hides small overreads from AddressSanitizer, so indexing is checked in
// debug builds instead. A moved-from vector is empty. Single-threaded.
template <class T>
class PoolVector {
  static_assert(std::is_trivially_copyable_v<T>);

 public:
  explicit PoolVector(arrow::MemoryPool* pool) : pool_(pool) {}
  PoolVector(const PoolVector&) = delete;
  PoolVector& operator=(const PoolVector&) = delete;
  PoolVector(PoolVector&& other) noexcept
      : pool_(other.pool_),
        buffer_(std::move(other.buffer_)),
        size_(std::exchange(other.size_, 0)) {}
  PoolVector& operator=(PoolVector&& other) noexcept {
    pool_ = other.pool_;
    buffer_ = std::move(other.buffer_);
    size_ = std::exchange(other.size_, 0);
    return *this;
  }
  ~PoolVector() = default;

  [[nodiscard]] std::size_t size() const { return size_; }
  [[nodiscard]] bool empty() const { return size_ == 0; }
  [[nodiscard]] std::size_t capacity() const {
    return buffer_ == nullptr ? 0 : static_cast<std::size_t>(buffer_->capacity()) / sizeof(T);
  }
  // The bytes the pool holds for this vector.
  [[nodiscard]] std::int64_t memory_usage() const {
    return buffer_ == nullptr ? 0 : buffer_->capacity();
  }

  [[nodiscard]] T* data() { return buffer_ == nullptr ? nullptr : buffer_->mutable_data_as<T>(); }
  [[nodiscard]] const T* data() const {
    return buffer_ == nullptr ? nullptr : buffer_->data_as<T>();
  }
  T& operator[](std::size_t i) {
    ANTB1_DCHECK(i < size_);
    return data()[i];
  }
  const T& operator[](std::size_t i) const {
    ANTB1_DCHECK(i < size_);
    return data()[i];
  }

  // Room for `n` elements in all; a smaller `n` changes nothing.
  arrow::Status Reserve(std::size_t n) {
    if (n <= capacity()) {
      return arrow::Status::OK();
    }
    if (n > static_cast<std::size_t>(std::numeric_limits<std::int64_t>::max()) / sizeof(T)) {
      return arrow::Status::OutOfMemory("an array of ", n, " values");
    }
    const auto bytes = static_cast<std::int64_t>(n * sizeof(T));
    if (buffer_ == nullptr) {  // one allocation, not an empty buffer and then its growth
      ARROW_ASSIGN_OR_RAISE(buffer_, arrow::AllocateResizableBuffer(bytes, pool_));
      return arrow::Status::OK();
    }
    return buffer_->Reserve(bytes);
  }
  // Exactly `n` elements: the ones past the old size are uninitialized, the capacity never shrinks.
  arrow::Status Resize(std::size_t n) {
    ARROW_RETURN_NOT_OK(Reserve(n));
    size_ = n;
    return arrow::Status::OK();
  }
  // Exactly `n` elements, each `value`.
  arrow::Status Assign(std::size_t n, const T& value) {
    ARROW_RETURN_NOT_OK(Resize(n));
    std::fill_n(data(), n, value);
    return arrow::Status::OK();
  }
  // Appends within the capacity Reserve made.
  void push_back(const T& value) {
    ANTB1_DCHECK(size_ < capacity());
    data()[size_++] = value;
  }
  // Gives the buffer back to the pool.
  void Reset() {
    buffer_.reset();
    size_ = 0;
  }

 private:
  arrow::MemoryPool* pool_;
  std::unique_ptr<arrow::ResizableBuffer> buffer_;
  std::size_t size_ = 0;
};

}  // namespace antb1::exec
