#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <limits>
#include <new>
#include <type_traits>
#include <vector>

#include <arrow/status.h>

#include "antb1/common/narrow.h"
#include "antb1/exec/memory_budget.h"

namespace antb1::exec {

// Grow geometrically, charging the replacement while the old allocation is live. A refused
// reservation or allocation leaves the current vector and its charge intact.
template <class T>
arrow::Status ReserveGroupVector(std::vector<T>& values, std::size_t groups,
                                 MemoryReservation& memory) {
  if (groups <= values.capacity()) {
    return arrow::Status::OK();
  }
  const auto capacity = std::max(groups, values.capacity() + (values.capacity() / 2));
  const auto before = memory.bytes();
  const auto old_bytes = Narrow<std::int64_t>(values.capacity() * sizeof(T));
  const auto available =
      static_cast<std::size_t>(std::numeric_limits<std::int64_t>::max() - before);
  if (capacity > available / sizeof(T) || capacity > values.max_size()) {
    return arrow::Status::CapacityError("grouped aggregate state size overflow");
  }
  ARROW_RETURN_NOT_OK(memory.Resize(before + Narrow<std::int64_t>(capacity * sizeof(T))));
  // Stage the replacement so even an implementation that provides extra capacity can be
  // refused without releasing the old state or leaving the replacement uncharged.
  const auto replace = [&] -> arrow::Status {
    std::vector<T> next;
    try {
      next.reserve(capacity);
    } catch (const std::bad_alloc&) {
      return arrow::Status::OutOfMemory("growing a grouped aggregate state");
    }
    if (next.capacity() > available / sizeof(T)) {
      return arrow::Status::CapacityError("grouped aggregate state size overflow");
    }
    ARROW_RETURN_NOT_OK(memory.Resize(before + Narrow<std::int64_t>(next.capacity() * sizeof(T))));
    static_assert(std::is_nothrow_move_constructible_v<T>);
    next.insert(next.end(), std::make_move_iterator(values.begin()),
                std::make_move_iterator(values.end()));
    next.swap(values);
    return arrow::Status::OK();
  };
  auto grown = replace();
  // The discarded vector (old on success, replacement on refusal) has been destroyed.
  ARROW_RETURN_NOT_OK(memory.Resize(
      grown.ok() ? before - old_bytes + Narrow<std::int64_t>(values.capacity() * sizeof(T))
                 : before));
  return grown;
}

}  // namespace antb1::exec
