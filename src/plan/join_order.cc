#include "join_order.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <variant>
#include <vector>

#include "antb1/common/check.h"
#include "antb1/common/int128.h"
#include "antb1/common/narrow.h"
#include "antb1/plan/logical_plan.h"
#include "antb1/plan/table.h"

#include "scope.h"

namespace antb1::plan {
namespace {

constexpr int64_t kLargest = std::numeric_limits<int64_t>::max();

// Per relation, the indices of its edges, ascending.
std::vector<std::vector<std::size_t>> EdgesOf(std::size_t relations,
                                              std::span<const JoinEdge> edges) {
  std::vector<std::vector<std::size_t>> out(relations);
  for (std::size_t e = 0; e < edges.size(); ++e) {
    const JoinEdge& edge = edges[e];
    ANTB1_CHECK(edge.a < relations);
    ANTB1_CHECK(edge.b < relations);
    ANTB1_CHECK(edge.a != edge.b);
    out[edge.a].push_back(e);
    out[edge.b].push_back(e);
  }
  return out;
}

// A count that is not negative, saturated at INT64_MAX.
int64_t Saturated(Int128 count) { return Int128ToInt64(count).value_or(kLargest); }

// The greedy walk of OrderJoins.
class Orderer {
 public:
  Orderer(std::span<const JoinRelation> relations, std::span<const JoinEdge> edges)
      : relations_(relations),
        edges_(edges),
        edges_of_(EdgesOf(relations.size(), edges)),
        joined_(relations.size(), false) {}

  std::vector<JoinStep> Order() {
    std::vector<JoinStep> steps;
    if (relations_.empty()) {
      return steps;
    }
    std::size_t probe = 0;
    for (std::size_t r = 1; r < relations_.size(); ++r) {
      if (Rows(r) > Rows(probe)) {
        probe = r;
      }
    }
    steps.push_back(Add(probe, Rows(probe)));
    while (steps.size() < relations_.size()) {
      std::optional<std::size_t> next;
      int64_t smallest = 0;
      for (std::size_t c = 0; c < relations_.size(); ++c) {
        if (joined_[c]) {
          continue;
        }
        const std::optional<int64_t> estimate = Estimate(c, steps.back().estimate);
        if (estimate.has_value() && (!next.has_value() || *estimate < smallest)) {
          next = c;
          smallest = *estimate;
        }
      }
      ANTB1_CHECK(next.has_value());  // the relations are connected (FirstUnconnected)
      steps.push_back(Add(*next, smallest));
    }
    return steps;
  }

 private:
  // A relation's rows: unknown counts as the most, and a negative count (no valid footer has one)
  // as none.
  [[nodiscard]] int64_t Rows(std::size_t relation) const {
    return std::max<int64_t>(relations_[relation].rows.value_or(kLargest), 0);
  }

  // The domain of an edge's side on `relation`: unknown is the relation's rows.
  [[nodiscard]] int64_t Domain(const std::optional<int64_t>& domain, std::size_t relation) const {
    return domain.has_value() ? std::max<int64_t>(*domain, 0) : Rows(relation);
  }

  // The estimated rows of joining `candidate` to the joined relations (`rows` of them), or
  // std::nullopt when no edge joins it to them.
  [[nodiscard]] std::optional<int64_t> Estimate(std::size_t candidate, int64_t rows) const {
    bool connected = false;
    bool empty = rows == 0 || Rows(candidate) == 0;
    int64_t divisor = 1;
    for (const std::size_t e : edges_of_[candidate]) {
      const JoinEdge& edge = edges_[e];
      const bool on_a = edge.a == candidate;
      const std::size_t other = on_a ? edge.b : edge.a;
      if (!joined_[other]) {
        continue;
      }
      connected = true;
      const int64_t mine = Domain(on_a ? edge.a_domain : edge.b_domain, candidate);
      const int64_t theirs = Domain(on_a ? edge.b_domain : edge.a_domain, other);
      empty = empty || mine == 0 || theirs == 0;
      divisor = std::max({divisor, mine, theirs});
    }
    if (!connected) {
      return std::nullopt;
    }
    if (empty) {
      return 0;
    }
    return std::max<int64_t>(Saturated(Int128{rows} * Rows(candidate) / divisor), 1);
  }

  // The step that joins `relation`, with its edges to the joined relations as keys.
  JoinStep Add(std::size_t relation, int64_t estimate) {
    JoinStep step{
        .relation = relation, .edges = {}, .build = BuildSide::kRight, .estimate = estimate};
    for (const std::size_t e : edges_of_[relation]) {
      const JoinEdge& edge = edges_[e];
      if (joined_[edge.a == relation ? edge.b : edge.a]) {
        step.edges.push_back(e);
      }
    }
    joined_[relation] = true;
    return step;
  }

  std::span<const JoinRelation> relations_;
  std::span<const JoinEdge> edges_;
  std::vector<std::vector<std::size_t>> edges_of_;
  std::vector<bool> joined_;
};

// One overload per node type: a node type without one fails to compile.
struct RowsOf {
  std::optional<int64_t> operator()(const ScanNode& node) const {
    return node.table->exact_row_count();
  }
  std::optional<int64_t> operator()(const FilterNode& node) const { return Input(node); }
  std::optional<int64_t> operator()(const ComputeNode& node) const { return Input(node); }
  std::optional<int64_t> operator()(const ProjectNode& node) const { return Input(node); }
  std::optional<int64_t> operator()(const AggregateNode& /*node*/) const { return 1; }
  std::optional<int64_t> operator()(const GroupAggregateNode& node) const { return Input(node); }
  std::optional<int64_t> operator()(const SortNode& node) const { return Input(node); }
  std::optional<int64_t> operator()(const LimitNode& node) const { return Input(node); }
  std::optional<int64_t> operator()(const RowCountNode& /*node*/) const { return 1; }
  std::optional<int64_t> operator()(const JoinNode& /*node*/) const { return std::nullopt; }

  template <class Node>
  static std::optional<int64_t> Input(const Node& node) {
    return EstimateRows(*node.input);
  }
};

}  // namespace

std::optional<std::size_t> FirstUnconnected(std::size_t relations,
                                            std::span<const JoinEdge> edges) {
  if (relations == 0) {
    return std::nullopt;
  }
  const std::vector<std::vector<std::size_t>> edges_of = EdgesOf(relations, edges);
  std::vector<bool> reached(relations, false);
  std::vector<std::size_t> pending = {0};
  reached[0] = true;
  while (!pending.empty()) {
    const std::size_t r = pending.back();
    pending.pop_back();
    for (const std::size_t e : edges_of[r]) {
      const std::size_t other = edges[e].a == r ? edges[e].b : edges[e].a;
      if (!reached[other]) {
        reached[other] = true;
        pending.push_back(other);
      }
    }
  }
  const auto first = std::ranges::find(reached, false);
  if (first == reached.end()) {
    return std::nullopt;
  }
  return Narrow<std::size_t>(first - reached.begin());
}

std::vector<JoinStep> OrderJoins(std::span<const JoinRelation> relations,
                                 std::span<const JoinEdge> edges) {
  return Orderer(relations, edges).Order();
}

std::optional<int64_t> KeyDomain(const Table& table, int field) {
  const int64_t parts = table.num_parts();
  if (parts <= 0) {
    return 0;
  }
  // 1. The integer range over the parts, capped at the non-NULL values: when every part has a
  // range or only NULLs.
  Int128 non_null = 0;
  std::optional<Int128> low;
  std::optional<Int128> high;
  bool exact = true;
  for (int64_t part = 0; part < parts && exact; ++part) {
    const std::optional<PartStats> stats = table.part_stats(part, field);
    if (!stats.has_value() || stats->null_count >= stats->rows) {
      exact = stats.has_value();  // with only NULLs: no value of the part counts
      continue;
    }
    if (!stats->min.has_value() || !stats->max.has_value()) {
      exact = false;
      continue;
    }
    non_null += stats->rows - std::max<int64_t>(stats->null_count, 0);
    low = low.has_value() ? std::min(*low, *stats->min) : *stats->min;
    high = high.has_value() ? std::max(*high, *stats->max) : *stats->max;
  }
  if (exact) {
    if (!low.has_value() || !high.has_value()) {
      return 0;  // every value is NULL
    }
    return Saturated(std::min(*high - *low + 1, non_null));
  }
  // 2. The largest hint, when every part has one.
  int64_t largest = 0;
  for (int64_t part = 0; part < parts; ++part) {
    const std::optional<int64_t> hint = table.part_distinct_count(part, field);
    if (!hint.has_value()) {
      return std::nullopt;
    }
    largest = std::max(largest, *hint);
  }
  return largest;
}

std::optional<int64_t> RelationRows(const Binding& binding) {
  if (const auto* table = std::get_if<TableSource>(&binding.source())) {
    return table->table->exact_row_count();
  }
  return EstimateRows(*std::get<PlanSource>(binding.source()).root);
}

std::optional<int64_t> EstimateRows(const LogicalNode& node) { return std::visit(RowsOf{}, node); }

}  // namespace antb1::plan
