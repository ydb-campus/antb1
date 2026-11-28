#pragma once

#include <atomic>

#include "antb1/plan/logical_plan.h"

namespace antb1::engine {

// Drain finishes or discards the whole result before it is exposed. If the new plan runs out of
// budget (including buffers held by earlier queries), release it and run the original plan once.
// Other errors, including validation failures and arithmetic overflow, propagate directly.
template <class Run>
auto RunWithMemoryFallback(const plan::LogicalPlan& logical, const plan::LogicalPlan& fallback,
                           Run run) {
  auto attempt = [&run](const plan::LogicalPlan& chosen) {
    // Even with an unlimited query budget, the backend can refuse an allocation while a probe
    // holds speculative batches. Drop the attempted tree and rerun with serial probes.
    std::atomic<bool> used = false;
    auto result = run(chosen, &used);
    if (!result.ok() && result.status().IsOutOfMemory() && used.load(std::memory_order_relaxed)) {
      return run(chosen, nullptr);
    }
    return result;
  };
  auto result = attempt(logical);
  if (!result.ok() && result.status().IsOutOfMemory() && fallback.root) {
    return attempt(fallback);
  }
  return result;
}

}  // namespace antb1::engine
