#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <string>
#include <string_view>
#include <utility>
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
// the known ones as a query runs, then any other by name. A hash join's probe has find, gather,
// residual and window_rows; its build parts, skipped, part_time, wait, lanes_tail and finish,
// then what its table holds: null_keys, unique, direct and cols_dropped.
std::vector<exec::ProfileMetric> Ordered(const exec::ProfileNode& node) {
  static constexpr std::array<std::string_view, 29> kOrder = {
      "parts",        "skipped",    "sample_parts", "heavy_keys",
      "part_time",    "raw_parts",  "raw_rows",     "cols_filter_only",
      "dict_pieces",  "wait",       "merge",        "find",
      "gather",       "residual",   "window_rows",  "late_columns",
      "late_parts",   "late_fetch", "lanes_tail",   "finish",
      "outer",        "build",      "sort",         "groups",
      "heavy_groups", "null_keys",  "unique",       "direct",
      "cols_dropped"};
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

// The text tree. A part pipeline prepares each HashBuild on the consumer, as a later child of that
// operator, while the HashJoin it belongs to sits inside the pipeline. The drawing moves that
// HashBuild under its HashJoin, and places it before the probe when the join builds on the left so
// the left input is always printed first. The profile tree itself, and the JSON, keep the build on
// the operator that prepares it.
struct Drawn {
  const exec::ProfileNode* node = nullptr;
  std::vector<Drawn> children;
};

bool HasBuildChild(const exec::ProfileNode& node) {
  return std::ranges::any_of(
      node.children(), [](const exec::ProfileNode* child) { return child->name() == "HashBuild"; });
}

// HashBuilds a part consumer prepares for joins inside its pipeline (child 0, marked per part).
// A HashJoin's own build is already its child and stays there.
void SplitBuilds(const exec::ProfileNode& node, std::vector<const exec::ProfileNode*>& own,
                 std::vector<const exec::ProfileNode*>& lifted) {
  const std::vector<const exec::ProfileNode*> kids = node.children();
  const bool pipeline = !kids.empty() && kids.front()->per_part() && node.name() != "HashJoin";
  for (std::size_t i = 0; i < kids.size(); ++i) {
    if (pipeline && i > 0 && kids[i]->name() == "HashBuild") {
      lifted.push_back(kids[i]);
    } else {
      own.push_back(kids[i]);
    }
  }
}

// `detail` is ExplainNode of a join: "Join <KIND> build=<side> keys=[...]". KIND may contain a
// space (NULL-AWARE ANTI). The side is that one token, not a later copy of the same text inside a
// residual or a quoted name.
enum class PrintedSide { kUnknown, kLeft, kRight };

PrintedSide JoinBuildSide(std::string_view detail) {
  constexpr std::string_view kJoin = "Join ";
  constexpr std::string_view kBuild = " build=";
  if (!detail.starts_with(kJoin)) {
    return PrintedSide::kUnknown;
  }
  const std::size_t at = detail.find(kBuild);
  if (at == std::string_view::npos || at <= kJoin.size()) {
    return PrintedSide::kUnknown;
  }
  const std::string_view side = detail.substr(at + kBuild.size());
  const auto matches = [](std::string_view text, std::string_view name) {
    return text.starts_with(name) && (text.size() == name.size() || text[name.size()] == ' ');
  };
  if (matches(side, "left")) {
    return PrintedSide::kLeft;
  }
  if (matches(side, "right")) {
    return PrintedSide::kRight;
  }
  return PrintedSide::kUnknown;
}

// The left input of a join is printed before the right one, as a logical plan prints them. The
// probe is the other side from the build, so a build=left join's HashBuild moves in front of it.
void OrderJoinSides(Drawn& drawn) {
  if (drawn.node->name() != "HashJoin" && drawn.node->name() != "RightJoin") {
    return;
  }
  const std::string_view detail = drawn.node->detail();
  const PrintedSide side = JoinBuildSide(detail);
  if (side == PrintedSide::kUnknown) {
    return;
  }
  const bool build_left = side == PrintedSide::kLeft;
  std::size_t build = drawn.children.size();
  for (std::size_t i = 0; i < drawn.children.size(); ++i) {
    const exec::ProfileNode& child = *drawn.children[i].node;
    if (child.name() == "HashBuild" && child.detail() == detail) {
      build = i;
      break;
    }
  }
  if (build == drawn.children.size() || (build_left && build == 0) ||
      (!build_left && build + 1 == drawn.children.size())) {
    return;
  }
  Drawn taken = std::move(drawn.children[build]);
  drawn.children.erase(drawn.children.begin() + static_cast<std::ptrdiff_t>(build));
  if (build_left) {
    drawn.children.insert(drawn.children.begin(), std::move(taken));
  } else {
    drawn.children.push_back(std::move(taken));
  }
}

Drawn Draw(const exec::ProfileNode& node, std::vector<const exec::ProfileNode*>& queue) {
  Drawn drawn{.node = &node};
  std::vector<const exec::ProfileNode*> own;
  std::vector<const exec::ProfileNode*> lifted;
  SplitBuilds(node, own, lifted);
  const bool owns = !lifted.empty();
  std::vector<const exec::ProfileNode*> merged;
  std::vector<const exec::ProfileNode*>* builds = &queue;
  if (owns) {
    merged.insert(merged.end(), queue.begin(), queue.end());
    queue.clear();
    merged.insert(merged.end(), lifted.begin(), lifted.end());
    builds = &merged;
  }
  for (const exec::ProfileNode* child : own) {
    const bool claim = child->name() == "HashJoin" && !HasBuildChild(*child) && !builds->empty() &&
                       builds->front()->detail() == child->detail();
    if (!claim) {
      drawn.children.push_back(Draw(*child, *builds));
      continue;
    }
    const exec::ProfileNode* build = builds->front();
    builds->erase(builds->begin());
    Drawn nested = Draw(*child, *builds);
    std::vector<const exec::ProfileNode*> none;
    nested.children.push_back(Draw(*build, none));
    OrderJoinSides(nested);
    drawn.children.push_back(std::move(nested));
  }
  if (owns) {
    while (!builds->empty()) {
      const exec::ProfileNode* build = builds->front();
      builds->erase(builds->begin());
      std::vector<const exec::ProfileNode*> none;
      drawn.children.push_back(Draw(*build, none));
    }
  }
  OrderJoinSides(drawn);
  return drawn;
}

// One line per operator, root first. Without `counters` -- a plan nobody ran
// (FormatPhysicalPlan) -- the line ends after the operator and its logical node, since rows, times
// and metrics are what a run measures and this node has never been through one.
void Write(const Drawn& drawn, std::string_view prefix, bool last, bool root, bool counters,
           std::string& out) {
  const exec::ProfileNode& node = *drawn.node;
  std::string line;
  if (!root) {
    line += prefix;
    line += last ? "└─" : "├─";
  }
  line += node.name();
  if (const std::string_view detail = DetailText(node); !detail.empty()) {
    line += ' ';
    line += detail;
  }
  if (counters) {
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
    line += ']';
  }
  out += line + '\n';
  std::string child_prefix(prefix);
  if (!root) {
    child_prefix += last ? "  " : "│ ";
  }
  for (std::size_t i = 0; i < drawn.children.size(); ++i) {
    Write(drawn.children[i], child_prefix, i + 1 == drawn.children.size(), false, counters, out);
  }
}

void Text(const exec::ProfileNode& node, bool counters, std::string& out) {
  std::vector<const exec::ProfileNode*> pending;
  Write(Draw(node, pending), "", false, true, counters, out);
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
    Text(*profile.root, /*counters=*/true, out);
  }
  return out;
}

std::string FormatPhysicalPlan(std::string_view output, const exec::ProfileNode& root) {
  std::string out = std::string(output) + '\n';
  Text(root, /*counters=*/false, out);
  return out;
}

}  // namespace antb1::engine
