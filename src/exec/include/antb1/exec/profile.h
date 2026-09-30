#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

// Query profiles (`antb1 explain --analyze`, docs/adr/0015-query-profiles.md): one ProfileNode per
// physical operator, filled in while the query runs. Profiling is off unless the physical plan is
// built with a profile root (BuildPhysicalPlan); then every operator is timed and counted by a
// wrapper, and operators add their own metrics (parts, waits, merge phases, groups) through
// Operator::profile(). The operators of a part pipeline run once per part, on several threads,
// and add up into the same nodes. Counts do not depend on the number of threads when every part is
// read once, which holds but for two cases: a LIMIT stops the parts it no longer needs, some of
// them already started, and a part that ran out of memory next to others runs again alone. Times
// are wall time on each thread, summed over the parts.

namespace antb1::exec {

enum class MetricUnit { kCount, kNanos, kBytes };

struct ProfileMetric {
  std::string name;
  MetricUnit unit = MetricUnit::kCount;
  int64_t value = 0;
};

class ProfileNode {
 public:
  ProfileNode() = default;
  ProfileNode(const ProfileNode&) = delete;
  ProfileNode& operator=(const ProfileNode&) = delete;
  ProfileNode(ProfileNode&&) = delete;
  ProfileNode& operator=(ProfileNode&&) = delete;
  ~ProfileNode() = default;

  // Set by the first build of the plan, before the query runs (the other builds of a part
  // pipeline, on worker threads while it runs, only read them).
  void set_name(std::string name) { name_ = std::move(name); }
  void set_detail(std::string detail) { detail_ = std::move(detail); }
  void set_per_part(bool per_part) { per_part_ = per_part; }
  // Input `index` (created when missing; the nodes of a part pipeline are created by its first
  // build and found again by the builds of the other parts). Thread-safe.
  ProfileNode* Child(std::size_t index);

  // While the query runs; thread-safe.
  void AddRows(int64_t rows) {
    rows_.fetch_add(rows, std::memory_order_relaxed);
    batches_.fetch_add(1, std::memory_order_relaxed);
  }
  void AddTime(std::chrono::nanoseconds time) {
    time_ns_.fetch_add(time.count(), std::memory_order_relaxed);
  }
  void AddInstance() { instances_.fetch_add(1, std::memory_order_relaxed); }
  // Adds `value` to metric `name` (created with this unit at its first use).
  void Add(std::string_view name, MetricUnit unit, int64_t value);
  // Keeps the largest `value` of metric `name`.
  void Max(std::string_view name, MetricUnit unit, int64_t value);

  // After the query.
  [[nodiscard]] const std::string& name() const { return name_; }
  [[nodiscard]] const std::string& detail() const { return detail_; }
  [[nodiscard]] bool per_part() const { return per_part_; }
  [[nodiscard]] int64_t rows() const { return rows_.load(std::memory_order_relaxed); }
  [[nodiscard]] int64_t batches() const { return batches_.load(std::memory_order_relaxed); }
  [[nodiscard]] std::chrono::nanoseconds time() const {
    return std::chrono::nanoseconds(time_ns_.load(std::memory_order_relaxed));
  }
  [[nodiscard]] int64_t instances() const { return instances_.load(std::memory_order_relaxed); }
  // In the order of their first use.
  [[nodiscard]] std::vector<ProfileMetric> metrics() const;
  [[nodiscard]] std::vector<const ProfileNode*> children() const;

 private:
  ProfileMetric& Find(std::string_view name, MetricUnit unit);  // under mutex_

  std::string name_;
  std::string detail_;
  bool per_part_ = false;
  std::atomic<int64_t> rows_{0};
  std::atomic<int64_t> batches_{0};
  std::atomic<int64_t> time_ns_{0};
  std::atomic<int64_t> instances_{0};
  mutable std::mutex mutex_;
  std::vector<ProfileMetric> metrics_;
  std::vector<std::unique_ptr<ProfileNode>> children_;
};

// Adds the time from its construction to its destruction to duration metric `name` of `node`;
// nothing without a node.
class ProfileTimer {
 public:
  ProfileTimer(ProfileNode* node, std::string_view name)
      : node_(node),
        name_(name),
        start_(node == nullptr ? std::chrono::steady_clock::time_point{}
                               : std::chrono::steady_clock::now()) {}
  ProfileTimer(const ProfileTimer&) = delete;
  ProfileTimer& operator=(const ProfileTimer&) = delete;
  ProfileTimer(ProfileTimer&&) = delete;
  ProfileTimer& operator=(ProfileTimer&&) = delete;
  ~ProfileTimer() {
    if (node_ != nullptr) {
      node_->Add(name_, MetricUnit::kNanos,
                 std::chrono::duration_cast<std::chrono::nanoseconds>(
                     std::chrono::steady_clock::now() - start_)
                     .count());
    }
  }

 private:
  ProfileNode* node_;
  std::string_view name_;
  std::chrono::steady_clock::time_point start_;
};

}  // namespace antb1::exec
