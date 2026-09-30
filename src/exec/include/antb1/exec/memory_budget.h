#pragma once

#include <atomic>
#include <cstdint>
#include <optional>
#include <string>

#include <arrow/memory_pool.h>
#include <arrow/status.h>

namespace antb1::exec {

// The memory of a session (engine::SessionOptions::memory_limit, antb1 --memory-limit): an Arrow
// memory pool that counts every buffer allocated through it, plus the memory operators reserve
// for their own containers (Reserve), against an optional limit. An allocation or reservation
// that would pass the limit fails with Status::OutOfMemory and leaves nothing behind; the count
// is atomic, so concurrent threads never pass the limit together. Thread-safe. Buffers must not
// outlive the budget (hold it by std::shared_ptr).
class MemoryBudget final : public arrow::MemoryPool {
 public:
  explicit MemoryBudget(std::optional<int64_t> limit,
                        arrow::MemoryPool* backend = arrow::default_memory_pool());

  using arrow::MemoryPool::Allocate;
  using arrow::MemoryPool::Free;
  using arrow::MemoryPool::Reallocate;

  arrow::Status Allocate(int64_t size, int64_t alignment, uint8_t** out) override;
  arrow::Status Reallocate(int64_t old_size, int64_t new_size, int64_t alignment,
                           uint8_t** ptr) override;
  void Free(uint8_t* buffer, int64_t size, int64_t alignment) override;

  // Bytes in use: buffers and reservations.
  int64_t bytes_allocated() const override { return used_.load(std::memory_order_relaxed); }
  // The most bytes in use at once.
  int64_t max_memory() const override { return peak_.load(std::memory_order_relaxed); }
  // Starts a new peak from the bytes in use now (a profiled query's own peak).
  void ResetPeak() {
    peak_.store(used_.load(std::memory_order_relaxed), std::memory_order_relaxed);
  }
  int64_t total_bytes_allocated() const override { return total_.load(std::memory_order_relaxed); }
  int64_t num_allocations() const override { return count_.load(std::memory_order_relaxed); }
  std::string backend_name() const override { return backend_->backend_name(); }

  // Charges `bytes` of memory held outside Arrow buffers (e.g. the vectors of a grouped
  // aggregate), or fails with OutOfMemory. Release gives them back.
  arrow::Status Reserve(int64_t bytes);
  void Release(int64_t bytes);

  [[nodiscard]] std::optional<int64_t> limit() const { return limit_; }
  // More than half of the limit is in use: parts are then started one at a time.
  [[nodiscard]] bool under_pressure() const {
    return limit_.has_value() && bytes_allocated() > *limit_ / 2;
  }

 private:
  arrow::Status Charge(int64_t bytes);
  void Uncharge(int64_t bytes) { used_.fetch_sub(bytes, std::memory_order_relaxed); }

  std::optional<int64_t> limit_;
  arrow::MemoryPool* backend_;
  std::atomic<int64_t> used_ = 0;
  std::atomic<int64_t> peak_ = 0;
  std::atomic<int64_t> total_ = 0;
  std::atomic<int64_t> count_ = 0;
};

// The bytes an operator's own containers hold, charged to a budget: Resize(total) reserves the
// growth or gives back the shrinkage; Release, Reset and the destructor give everything back.
// Without a budget it only keeps the count. Single-threaded, like its operator.
class MemoryReservation {
 public:
  MemoryReservation() = default;
  MemoryReservation(const MemoryReservation&) = delete;
  MemoryReservation& operator=(const MemoryReservation&) = delete;
  MemoryReservation(MemoryReservation&&) = delete;
  MemoryReservation& operator=(MemoryReservation&&) = delete;
  ~MemoryReservation() { Release(); }

  // Gives back what is reserved and charges `budget` from now on (nullptr: no budget).
  void Reset(MemoryBudget* budget) {
    Release();
    budget_ = budget;
  }
  // Makes the reservation `bytes`; OutOfMemory (keeping the old reservation) past the limit.
  arrow::Status Resize(int64_t bytes);
  void Release();
  [[nodiscard]] int64_t bytes() const { return bytes_; }

 private:
  MemoryBudget* budget_ = nullptr;
  int64_t bytes_ = 0;
};

// "1.50 GB": bytes in decimal units, for messages.
std::string FormatBytes(int64_t bytes);

}  // namespace antb1::exec
