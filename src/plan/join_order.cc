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

// The default factors PredicateRows takes where the footers say nothing: the textbook ones, out of
// `kDefaultScale`. They only have to order two relations, never to be right: ADR 0026 measured the
// build sides it must separate as differing by factors of 10 and more, and the ratio keeps every
// near-tie as it was, so a default that is off by two or three still decides correctly or not at
// all. They are deliberately coarse and deliberately not tuned to one workload.
constexpr int64_t kDefaultScale = 100;
constexpr int64_t kEqualDefault = 10;    // column = constant, with no domain to divide by
constexpr int64_t kRangeDefault = 33;    // column < constant, with no range to compare against
constexpr int64_t kLikeDefault = 25;     // column LIKE pattern
constexpr int64_t kNotNullDefault = 90;  // column IS NOT NULL, with no null count
constexpr int64_t kOtherDefault = 50;    // two columns compared, or a computed BOOLEAN

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
  Orderer(std::span<const JoinRelation> relations, std::span<const JoinEdge> edges,
          int64_t build_side_ratio)
      : relations_(relations),
        edges_(edges),
        edges_of_(EdgesOf(relations.size(), edges)),
        joined_(relations.size(), false),
        ratio_(build_side_ratio) {
    ANTB1_CHECK(build_side_ratio >= 100);
  }

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
    // The first step is the probe itself and joins nothing, so it has no side to choose.
    steps.push_back(Add(probe, Rows(probe), std::nullopt));
    while (steps.size() < relations_.size()) {
      std::optional<std::size_t> next;
      int64_t smallest = 0;
      const int64_t joined_rows = steps.back().estimate;
      for (std::size_t c = 0; c < relations_.size(); ++c) {
        if (joined_[c]) {
          continue;
        }
        const std::optional<int64_t> estimate = Estimate(c, joined_rows);
        if (estimate.has_value() && (!next.has_value() || *estimate < smallest)) {
          next = c;
          smallest = *estimate;
        }
      }
      ANTB1_CHECK(next.has_value());  // the relations are connected (FirstUnconnected)
      steps.push_back(Add(*next, smallest, joined_rows));
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

  // The side the step that adds `relation` builds on (rule 5): the relation it adds, unless its
  // rows exceed `joined_rows`, the estimate of the relations joined so far, by more than the ratio.
  // An unknown count on either side, and an estimate of no row at all, keep the relation it adds:
  // neither may be read as small, since building on a side that turns out to be large is the very
  // mistake this rule exists to avoid.
  [[nodiscard]] BuildSide Build(std::size_t relation,
                                const std::optional<int64_t>& joined_rows) const {
    // The left input is the relations joined so far, the right the relation this step adds.
    return ChooseBuildSide(joined_rows, relations_[relation].rows, ratio_);
  }

  // The step that joins `relation`, with its edges to the joined relations as keys.
  JoinStep Add(std::size_t relation, int64_t estimate, const std::optional<int64_t>& joined_rows) {
    JoinStep step{.relation = relation,
                  .edges = {},
                  .build = Build(relation, joined_rows),
                  .estimate = estimate};
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
  int64_t ratio_;  // hundredths of a factor, at least 100
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
  // The join order's own estimate for this join, kept on the node (ADR 0026): the domains rule 3
  // divides by are not reachable from a plan node, so nothing here could recompute it.
  std::optional<int64_t> operator()(const JoinNode& node) const { return node.estimate; }
  // A dependent join outputs at most its left input's rows, for every kind the unnesting pass
  // admits: a semi or an anti join keeps a subset of them, and a one-row join pairs each with the
  // subquery's single row (ADR 0023 refuses every other kind). The pass rewrites top-down, so the
  // outer of two nested dependent joins is rewritten while its left input still holds the inner
  // one -- and reading that as unknown left the outer join building on its subquery however
  // lopsided it was (ADR 0026). Over-estimating a left input makes ChooseBuildSide flip less
  // readily, which is the safe direction.
  std::optional<int64_t> operator()(const DependentJoinNode& node) const {
    return EstimateRows(*node.left);
  }

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
                                 std::span<const JoinEdge> edges, int64_t build_side_ratio) {
  return Orderer(relations, edges, build_side_ratio).Order();
}

BuildSide ChooseBuildSide(const std::optional<int64_t>& left, const std::optional<int64_t>& right,
                          int64_t build_side_ratio) {
  ANTB1_CHECK(build_side_ratio >= 100);
  if (!left.has_value() || !right.has_value() || *right < 0 || *left <= 0) {
    return BuildSide::kRight;
  }
  const bool decisive = Int128{*right} * 100 > Int128{build_side_ratio} * Int128{*left};
  return decisive ? BuildSide::kLeft : BuildSide::kRight;
}

std::optional<int64_t> ColumnDomain(const Table& table, int field, const ColumnRange& range) {
  if (table.num_parts() <= 0) {
    return 0;  // a table without parts has no value
  }
  // 1. The integer range over the parts, capped at the non-NULL values, when every part has a
  // range (its min at most its max, as in every valid footer) or only NULLs.
  if (range.exact) {
    if (!range.min.has_value() || !range.max.has_value()) {
      return 0;  // every value is NULL
    }
    // max - min + 1, saturated: the range of 128-bit values (HUGEINT) can exceed an Int128.
    Int128 span = kInt128Max;
    if (Int128 difference = 0; !__builtin_sub_overflow(*range.max, *range.min, &difference)) {
      span = CheckedAdd(difference, 1).value_or(kInt128Max);
    }
    return Saturated(std::min(span, Int128{range.non_null}));
  }
  // 2. The largest hint, when every part has one. Only this rule needs the parts again.
  int64_t largest = 0;
  for (int64_t part = 0; part < table.num_parts(); ++part) {
    const std::optional<int64_t> hint = table.part_distinct_count(part, field);
    if (!hint.has_value()) {
      return std::nullopt;
    }
    largest = std::max(largest, *hint);
  }
  return largest;
}

std::optional<int64_t> KeyDomain(const Table& table, int field) {
  return ColumnDomain(table, field, ColumnRangeOf(table, field));
}

ColumnRange ColumnRangeOf(const Table& table, int field) {
  const int64_t parts = table.num_parts();
  ColumnRange out;
  if (parts <= 0) {
    return out;  // nothing to read: not exact, so every predicate takes its default factor
  }
  Int128 non_null = 0;
  for (int64_t part = 0; part < parts; ++part) {
    const std::optional<PartStats> stats = table.part_stats(part, field);
    if (!stats.has_value()) {
      return {};  // a column whose type has no statistics (ADR 0021), or a part without them
    }
    if (stats->null_count >= stats->rows) {
      continue;  // only NULLs: no value of the part counts, and the part is still exact
    }
    if (!stats->min.has_value() || !stats->max.has_value() || *stats->min > *stats->max) {
      return {};
    }
    non_null += std::max<int64_t>(stats->rows - std::max<int64_t>(stats->null_count, 0), 0);
    out.min = out.min.has_value() ? std::min(*out.min, *stats->min) : *stats->min;
    out.max = out.max.has_value() ? std::max(*out.max, *stats->max) : *stats->max;
  }
  out.non_null = Saturated(non_null);
  out.exact = true;
  return out;
}

int64_t PredicateRows(int64_t rows, const Predicate& predicate, const ColumnRange& range,
                      std::optional<int64_t> domain) {
  ANTB1_CHECK(rows >= 0);
  // `rows` of a relation that has any, never estimated at none: a relation estimated at no row is
  // ordered first and makes every estimate after it 0 too, so a predicate that keeps few rows must
  // not be rounded down to keeping none.
  const auto at_least_one = [rows](Int128 kept) {
    return std::clamp<int64_t>(Saturated(kept), rows == 0 ? 0 : 1, rows);
  };
  // `rows` times `numerator` over `denominator`, rounded down but never to no row at all unless the
  // numerator is 0.
  const auto scaled = [rows, &at_least_one](Int128 numerator, Int128 denominator) {
    if (numerator <= 0) {
      return int64_t{0};
    }
    // `rows` times a numerator that a 128-bit column's range can make very large overflows before
    // the division brings it back. Every caller passes a numerator no larger than its denominator,
    // so the true quotient is at most `rows`: keeping every row is the answer that errs toward the
    // plan this relation already had.
    Int128 product = 0;
    if (__builtin_mul_overflow(Int128{rows}, numerator, &product)) {
      return rows;
    }
    return at_least_one(product / std::max<Int128>(denominator, 1));
  };
  // What a predicate's complement keeps: the rest of the rows, and never none of them. A column of
  // one distinct value would otherwise estimate `col <> that value` at exactly 0.
  const auto complement = [rows, &at_least_one](int64_t matched) {
    return at_least_one(Int128{rows} - Int128{matched});
  };
  // The values of the column, when the footers give them exactly: its range, and one value's share.
  // The span is guarded like KeyDomain's: the range of 128-bit values can exceed an Int128.
  bool ranged = range.exact && range.min.has_value() && range.max.has_value();
  Int128 span = 0;
  if (ranged) {
    Int128 difference = 0;
    if (__builtin_sub_overflow(*range.max, *range.min, &difference)) {
      ranged = false;
    } else {
      const std::optional<Int128> widened = CheckedAdd(difference, 1);
      ranged = widened.has_value();
      span = widened.value_or(0);
    }
  }
  const auto* constant = std::get_if<Int128>(&predicate.constant.value);
  // Whether the column's range provably excludes a value, so that no row can equal it. Only an
  // integer-valued constant can be placed against the range; anything else may match for all the
  // footers say.
  const auto outside = [&range, ranged](const Constant& value) {
    const auto* integer = std::get_if<Int128>(&value.value);
    return ranged && integer != nullptr && (*integer < *range.min || *integer > *range.max);
  };

  switch (predicate.kind) {
    case Predicate::Kind::kFalse:
      return 0;
    case Predicate::Kind::kIsNotNull:
      return range.exact ? std::min(rows, range.non_null) : scaled(kNotNullDefault, kDefaultScale);
    case Predicate::Kind::kCompare: {
      switch (predicate.op) {
        case CompareOp::kEq:
          // A constant the column's range excludes matches no row, as a range comparison outside
          // the range admits none.
          if (outside(predicate.constant)) {
            return 0;
          }
          return domain.has_value() ? scaled(1, *domain) : scaled(kEqualDefault, kDefaultScale);
        case CompareOp::kNe:
          // The mirror: a constant the range excludes excludes no row, so every row passes. Without
          // this the complement of a column of one value estimates a relation at almost none of its
          // rows, which would order it first and build on the wrong side of its joins.
          if (outside(predicate.constant)) {
            return rows;
          }
          return domain.has_value() ? complement(scaled(1, *domain))
                                    : scaled(kDefaultScale - kEqualDefault, kDefaultScale);
        case CompareOp::kLt:
        case CompareOp::kLe:
        case CompareOp::kGt:
        case CompareOp::kGe: {
          if (!ranged || constant == nullptr) {
            return scaled(kRangeDefault, kDefaultScale);
          }
          // How many of the column's values the comparison admits, clamped to its range: over
          // [min, max] and a constant c, "< c" admits [min, c - 1], "<= c" admits [min, c], "> c"
          // admits [c + 1, max] and ">= c" admits [c, max]. A constant outside the range admits
          // all of them or none. Guarded like `span` is: a column whose range spans the 128-bit
          // values can put the two ends far enough apart that the subtraction overflows, which is
          // undefined rather than merely wrong, and an overflow leaves the predicate to its default
          // factor. No Parquet column reaches that today -- `part_stats` has no DECIMAL and so no
          // 128-bit column -- so this is the same defensive arithmetic KeyDomain already carries,
          // not a live case.
          const bool below = predicate.op == CompareOp::kLt || predicate.op == CompareOp::kLe;
          const bool closed = predicate.op == CompareOp::kLe || predicate.op == CompareOp::kGe;
          Int128 admitted = 0;
          if (__builtin_sub_overflow(below ? *constant : *range.max, below ? *range.min : *constant,
                                     &admitted)) {
            return scaled(kRangeDefault, kDefaultScale);
          }
          if (closed) {
            const std::optional<Int128> inclusive = CheckedAdd(admitted, 1);
            if (!inclusive.has_value()) {
              return scaled(kRangeDefault, kDefaultScale);
            }
            admitted = *inclusive;
          }
          return scaled(std::clamp<Int128>(admitted, 0, span), span);
        }
      }
      return scaled(kRangeDefault, kDefaultScale);
    }
    case Predicate::Kind::kIn: {
      // Only a value the column's range admits can match, so a list of values it all excludes
      // matches no row.
      const int64_t matching = std::ranges::count_if(
          predicate.values, [&outside](const Constant& value) { return !outside(value); });
      if (matching == 0) {
        return 0;
      }
      return domain.has_value() ? scaled(matching, *domain) : scaled(kEqualDefault, kDefaultScale);
    }
    case Predicate::Kind::kNotIn: {
      const int64_t matching = std::ranges::count_if(
          predicate.values, [&outside](const Constant& value) { return !outside(value); });
      if (matching == 0) {
        return rows;  // the mirror of kIn: nothing to exclude
      }
      return domain.has_value() ? complement(scaled(matching, *domain))
                                : scaled(kDefaultScale - kEqualDefault, kDefaultScale);
    }
    case Predicate::Kind::kLike:
      return scaled(kLikeDefault, kDefaultScale);
    case Predicate::Kind::kNotLike:
      return scaled(kDefaultScale - kLikeDefault, kDefaultScale);
    case Predicate::Kind::kCompareColumns:
    case Predicate::Kind::kIsTrue:
      return scaled(kOtherDefault, kDefaultScale);
  }
  return rows;
}

std::optional<int64_t> RelationRows(const Binding& binding) {
  if (const auto* table = std::get_if<TableSource>(&binding.source())) {
    return table->table->exact_row_count();
  }
  return EstimateRows(*std::get<PlanSource>(binding.source()).root);
}

std::optional<int64_t> EstimateRows(const LogicalNode& node) { return std::visit(RowsOf{}, node); }

}  // namespace antb1::plan
