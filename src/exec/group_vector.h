#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <limits>
#include <memory>
#include <new>
#include <type_traits>
#include <utility>
#include <vector>

#include <arrow/result.h>
#include <arrow/status.h>
#include <arrow/stl_allocator.h>

#include "antb1/common/narrow.h"
#include "antb1/exec/memory_budget.h"

namespace antb1::exec {

// Charge a concrete index or aggregate-state object to its lane before constructing it.
template <class Base, class Object, class... Args>
arrow::Result<std::unique_ptr<Base>> MakeGroupObject(MemoryReservation* memory, Args&&... args) {
  if (memory == nullptr) {
    return std::make_unique<Object>(std::forward<Args>(args)...);
  }
  const auto before = memory->bytes();
  constexpr auto kBytes = static_cast<std::int64_t>(sizeof(Object));
  if (before > std::numeric_limits<std::int64_t>::max() - kBytes) {
    return arrow::Status::CapacityError("local group metadata size overflow");
  }
  ARROW_RETURN_NOT_OK(memory->Resize(before + kBytes));
  try {
    return std::make_unique<Object>(std::forward<Args>(args)...);
  } catch (const arrow::stl::BadAlloc& e) {
    ARROW_RETURN_NOT_OK(memory->Resize(before));
    return arrow::Status::OutOfMemory(e.what());
  } catch (const std::bad_alloc&) {
    ARROW_RETURN_NOT_OK(memory->Resize(before));
    return arrow::Status::OutOfMemory("building a local group table");
  }
}

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
