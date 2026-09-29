#include "antb1/exec/memory_budget.h"

#include <array>
#include <cstdint>
#include <format>
#include <optional>
#include <string>

#include <arrow/memory_pool.h>
#include <arrow/status.h>

namespace antb1::exec {

MemoryBudget::MemoryBudget(std::optional<int64_t> limit, arrow::MemoryPool* backend)
    : limit_(limit), backend_(backend) {}

arrow::Status MemoryBudget::Charge(int64_t bytes) {
  const int64_t now = used_.fetch_add(bytes, std::memory_order_relaxed) + bytes;
  if (const std::optional<int64_t> limit = limit_; limit.has_value() && now > *limit) {
    Uncharge(bytes);
    return arrow::Status::OutOfMemory("the query needs more than the memory limit of ",
                                      FormatBytes(*limit), " (--memory-limit)");
  }
  int64_t peak = peak_.load(std::memory_order_relaxed);
  while (now > peak && !peak_.compare_exchange_weak(peak, now, std::memory_order_relaxed)) {
  }
  return arrow::Status::OK();
}

arrow::Status MemoryBudget::Allocate(int64_t size, int64_t alignment, uint8_t** out) {
  ARROW_RETURN_NOT_OK(Charge(size));
  const arrow::Status status = backend_->Allocate(size, alignment, out);
  if (!status.ok()) {
    Uncharge(size);
    return status;
  }
  total_.fetch_add(size, std::memory_order_relaxed);
  count_.fetch_add(1, std::memory_order_relaxed);
  return status;
}

arrow::Status MemoryBudget::Reallocate(int64_t old_size, int64_t new_size, int64_t alignment,
                                       uint8_t** ptr) {
  const int64_t growth = new_size - old_size;
  if (growth > 0) {
    ARROW_RETURN_NOT_OK(Charge(growth));
  }
  const arrow::Status status = backend_->Reallocate(old_size, new_size, alignment, ptr);
  if (!status.ok()) {
    if (growth > 0) {
      Uncharge(growth);
    }
    return status;
  }
  if (growth < 0) {
    Uncharge(-growth);
  } else {
    total_.fetch_add(growth, std::memory_order_relaxed);
  }
  count_.fetch_add(1, std::memory_order_relaxed);
  return status;
}

void MemoryBudget::Free(uint8_t* buffer, int64_t size, int64_t alignment) {
  backend_->Free(buffer, size, alignment);
  Uncharge(size);
}

arrow::Status MemoryBudget::Reserve(int64_t bytes) {
  if (bytes <= 0) {
    return arrow::Status::OK();
  }
  return Charge(bytes);
}

void MemoryBudget::Release(int64_t bytes) {
  if (bytes > 0) {
    Uncharge(bytes);
  }
}

arrow::Status MemoryReservation::Resize(int64_t bytes) {
  if (budget_ != nullptr) {
    if (bytes > bytes_) {
      ARROW_RETURN_NOT_OK(budget_->Reserve(bytes - bytes_));
    } else {
      budget_->Release(bytes_ - bytes);
    }
  }
  bytes_ = bytes;
  return arrow::Status::OK();
}

void MemoryReservation::Release() {
  if (budget_ != nullptr) {
    budget_->Release(bytes_);
  }
  bytes_ = 0;
}

std::string FormatBytes(int64_t bytes) {
  constexpr auto kUnits = std::to_array<const char*>({"bytes", "KB", "MB", "GB", "TB", "PB"});
  auto value = static_cast<double>(bytes);
  std::size_t unit = 0;
  while (value >= 1000 && unit + 1 < kUnits.size()) {
    value /= 1000;
    ++unit;
  }
  return unit == 0 ? std::format("{} bytes", bytes) : std::format("{:.2f} {}", value, kUnits[unit]);
}

}  // namespace antb1::exec
