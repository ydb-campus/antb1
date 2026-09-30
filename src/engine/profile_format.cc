#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <string>
#include <string_view>
#include <vector>

#include "antb1/engine/format.h"
#include "antb1/engine/session.h"
#include "antb1/exec/memory_budget.h"
#include "antb1/exec/profile.h"

namespace antb1::engine {
namespace {

// "1.234s", "12.345ms" or "0.123ms".
std::string Duration(int64_t ns) {
  constexpr int64_t kSecond = 1'000'000'000;
  if (ns >= kSecond) {
    return std::format("{:.3f}s", static_cast<double>(ns) / 1e9);
  }
  return std::format("{:.3f}ms", static_cast<double>(ns) / 1e6);
}

std::string MetricText(const exec::ProfileMetric& metric) {
  switch (metric.unit) {
    case exec::MetricUnit::kNanos:
      return Duration(metric.value);
    case exec::MetricUnit::kBytes:
      return exec::FormatBytes(metric.value);
    case exec::MetricUnit::kCount:
      break;
  }
  return std::to_string(metric.value);
}

std::string MetricKey(const exec::ProfileMetric& metric) {
  switch (metric.unit) {
    case exec::MetricUnit::kNanos:
      return metric.name + "_ns";
    case exec::MetricUnit::kBytes:
      return metric.name + "_bytes";
    case exec::MetricUnit::kCount:
      break;
  }
  return metric.name;
}

// The metrics in a fixed order (they are recorded in whatever order the threads reach them):
// the known ones as a query runs, then any other by name.
std::vector<exec::ProfileMetric> Ordered(const exec::ProfileNode& node) {
  static constexpr std::array<std::string_view, 18> kOrder = {
      "parts",      "skipped", "sample_parts", "heavy_keys",   "part_time",  "raw_parts",
      "raw_rows",   "wait",    "merge",        "late_columns", "late_parts", "late_fetch",
      "lanes_tail", "outer",   "build",        "sort",         "groups",     "heavy_groups"};
  const auto rank = [](const exec::ProfileMetric& metric) {
    const auto* it = std::ranges::find(kOrder, metric.name);
    return static_cast<std::size_t>(it - kOrder.begin());
  };
  std::vector<exec::ProfileMetric> metrics = node.metrics();
  std::ranges::stable_sort(metrics,
                           [&](const exec::ProfileMetric& a, const exec::ProfileMetric& b) {
                             return rank(a) != rank(b) ? rank(a) < rank(b) : a.name < b.name;
                           });
  return metrics;
}

// Whether `input` ran inside `node`'s calls, on the same thread: not the per-part pipeline of a
// part operator, whose parts run on other threads.
bool RanInside(const exec::ProfileNode& node, const exec::ProfileNode& input) {
  return input.per_part() == node.per_part();
}

// The node's own time: without the time of the inputs that ran inside it.
int64_t SelfTime(const exec::ProfileNode& node) {
  int64_t self = node.time().count();
  for (const exec::ProfileNode* child : node.children()) {
    if (RanInside(node, *child)) {
      self -= child->time().count();
    }
  }
  return self < 0 ? 0 : self;
}

bool HasInputInside(const exec::ProfileNode& node) {
  return std::ranges::any_of(
      node.children(), [&node](const exec::ProfileNode* child) { return RanInside(node, *child); });
}

// The detail without a first word that repeats the operator's name ("Filter x > 1" of Filter).
std::string_view DetailText(const exec::ProfileNode& node) {
  std::string_view detail = node.detail();
  if (detail.starts_with(node.name()) &&
      (detail.size() == node.name().size() || detail[node.name().size()] == ' ')) {
    detail.remove_prefix(std::min(detail.size(), node.name().size() + 1));
  }
  return detail;
}

void Text(const exec::ProfileNode& node, std::size_t depth, std::string& out) {
  std::string line = std::string(2 * depth, ' ') + node.name();
  if (const std::string_view detail = DetailText(node); !detail.empty()) {
    line += ' ';
    line += detail;
  }
  line += std::format("  [rows={} batches={}", node.rows(), node.batches());
  if (node.per_part()) {
    line += std::format(" parts={} time={} (summed over parts)", node.instances(),
                        Duration(node.time().count()));
  } else {
    line += std::format(" time={}", Duration(node.time().count()));
  }
  if (HasInputInside(node)) {
    line += std::format(" self={}", Duration(SelfTime(node)));
  }
  for (const exec::ProfileMetric& metric : Ordered(node)) {
    line += std::format(" {}={}", metric.name, MetricText(metric));
  }
  out += line + "]\n";
  for (const exec::ProfileNode* child : node.children()) {
    Text(*child, depth + 1, out);
  }
}

void Json(const exec::ProfileNode& node, std::string& out) {
  out += std::format(
      R"({{"name":"{}","detail":"{}","per_part":{},"runs":{},"rows":{},"batches":{},"time_ns":{},"self_time_ns":{},"metrics":{{)",
      JsonEscape(node.name()), JsonEscape(node.detail()), node.per_part() ? "true" : "false",
      node.instances(), node.rows(), node.batches(), node.time().count(), SelfTime(node));
  bool first = true;
  for (const exec::ProfileMetric& metric : Ordered(node)) {
    out +=
        std::format(R"({}"{}":{})", first ? "" : ",", JsonEscape(MetricKey(metric)), metric.value);
    first = false;
  }
  out += R"(},"inputs":[)";
  first = true;
  for (const exec::ProfileNode* child : node.children()) {
    out += first ? "" : ",";
    Json(*child, out);
    first = false;
  }
  out += "]}";
}

}  // namespace

std::string FormatProfile(const QueryProfile& profile, ProfileFormat format) {
  std::string out;
  if (format == ProfileFormat::kJson) {
    out += std::format(
        R"({{"output":"{}","time_ns":{},"rows":{},"peak_memory_bytes":{},"threads":{},"plan":)",
        JsonEscape(profile.output), profile.time.count(), profile.rows, profile.peak_memory,
        profile.threads);
    if (profile.root != nullptr) {
      Json(*profile.root, out);
    } else {
      out += "null";
    }
    out += "}\n";
    return out;
  }
  out += profile.output + '\n';
  out += std::format("Total: time={} rows={} peak_memory={} threads={}\n",
                     Duration(profile.time.count()), profile.rows,
                     exec::FormatBytes(profile.peak_memory), profile.threads);
  if (profile.root != nullptr) {
    Text(*profile.root, 0, out);
  }
  return out;
}

}  // namespace antb1::engine
