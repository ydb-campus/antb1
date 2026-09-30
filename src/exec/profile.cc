#include "antb1/exec/profile.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace antb1::exec {

ProfileNode* ProfileNode::Child(std::size_t index) {
  const std::scoped_lock lock(mutex_);
  while (children_.size() <= index) {
    children_.push_back(std::make_unique<ProfileNode>());
  }
  return children_[index].get();
}

ProfileMetric& ProfileNode::Find(std::string_view name, MetricUnit unit) {
  for (ProfileMetric& metric : metrics_) {
    if (metric.name == name) {
      return metric;
    }
  }
  metrics_.push_back(ProfileMetric{.name = std::string(name), .unit = unit, .value = 0});
  return metrics_.back();
}

void ProfileNode::Add(std::string_view name, MetricUnit unit, int64_t value) {
  const std::scoped_lock lock(mutex_);
  Find(name, unit).value += value;
}

void ProfileNode::Max(std::string_view name, MetricUnit unit, int64_t value) {
  const std::scoped_lock lock(mutex_);
  ProfileMetric& metric = Find(name, unit);
  metric.value = std::max(metric.value, value);
}

std::vector<ProfileMetric> ProfileNode::metrics() const {
  const std::scoped_lock lock(mutex_);
  return metrics_;
}

std::vector<const ProfileNode*> ProfileNode::children() const {
  const std::scoped_lock lock(mutex_);
  std::vector<const ProfileNode*> out;
  out.reserve(children_.size());
  for (const auto& child : children_) {
    out.push_back(child.get());
  }
  return out;
}

}  // namespace antb1::exec
