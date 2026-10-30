#include "join_order.h"

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <unordered_map>
#include <utility>
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

// A relation's rows: unknown counts as the largest, since unknown must never read as small, and a
// negative count as none.
int64_t BlockRows(std::span<const JoinRelation> relations, std::size_t relation) {
  return std::max<int64_t>(relations[relation].rows.value_or(kLargest), 0);
}

// What one edge divides by, and whether it is empty: the largest of its two sides' domains that the
// footers know. An unknown domain contributes NOTHING here rather than standing in its relation's
// rows -- the exact mirror of BlockRows above, and for the mirrored reason. Rows multiply, so an
// unknown that reads as the largest reads as the most rows, which is the safe direction. A domain
// DIVIDES, so an unknown that reads as the largest reads as the FEWEST rows, which is the one
// direction an estimate must never err in: under-stating the rows of a build is what materialises a
// side that does not fit (ADR 0026's C5), and under-stating an intermediate is what lets the search
// pick a plan that multiplies.
//
// The whole of a 145.2 s against 140.6 ms: a derived table's columns have no domain at all,
// because KeyDomainOf (src/plan/binder.cc) reads one only from a table, so an edge between a
// base-table column of 25 values and a derived column of 1,200,243 rows divided by 1,200,243
// instead of by 25, and a join the search ranked cheapest at 150,000 rows produced 6,697,748,923
// of them. ADR 0022 records both halves of that and never joined them up.
//
// Where neither side is known the edge divides by 1 and the join reads as the product of its
// inputs. That is deliberately the most pessimistic reading available: there is no value between
// "the largest domain possible" and "no information" that is not a guess, and a guess here is paid
// for in materialised rows.
struct EdgeDivisor {
  int64_t divisor = 1;  // 1: neither side's domain is known
  bool empty = false;   // a known domain of zero: that key has no value at all
};

EdgeDivisor DivisorOf(const JoinEdge& edge) {
  EdgeDivisor out;
  for (const std::optional<int64_t>& domain : {edge.a_domain, edge.b_domain}) {
    if (!domain.has_value()) {
      continue;
    }
    const int64_t values = std::max<int64_t>(*domain, 0);
    out.empty = out.empty || values == 0;
    out.divisor = std::max(out.divisor, values);
  }
  return out;
}

// `a * b`, saturated: a subset of a dozen relations can multiply past 128 bits before any domain
// divides it back down, and an estimate that large only has to stay large and stay deterministic.
Int128 Multiplied(Int128 a, int64_t b) {
  Int128 out = 0;
  return __builtin_mul_overflow(a, Int128{b}, &out) ? kInt128Max : out;
}

// The rows a join of inputs of `left` and `right` rows produces, from the edges between them, as
// everything that decides memory must read it: the largest domain of any of those edges divides, so
// a second key reads as no more selective than the stronger one. A composite key has at least as
// many distinct values as its most selective column, so this never over-states selectivity -- and
// under-stating the rows of a build is what materialises a side that does not fit. ADR 0026's C5
// records the query that proved it. This is the estimate a step carries and the one ChooseBuildSide
// reads; only the search's own cost reads the narrower, independence-assuming one.
int64_t JoinRowsOf(std::span<const JoinEdge> edges, int64_t left, int64_t right,
                   std::span<const std::size_t> crossing) {
  bool empty = left == 0 || right == 0;
  int64_t divisor = 1;
  for (const std::size_t e : crossing) {
    const EdgeDivisor edge = DivisorOf(edges[e]);
    empty = empty || edge.empty;
    divisor = std::max(divisor, edge.divisor);
  }
  return empty ? 0 : std::max<int64_t>(Saturated(Int128{left} * right / divisor), 1);
}

// Per pair of relations, what an equality between them divides by: the domains of the edges
// between that pair multiplied, capped at the larger of their row counts. 1 when no edge connects
// them; `empty` when a key of theirs has no value at all.
struct PairDivisors {
  std::vector<int64_t> divisor;  // relations x relations, 1 where no edge
  std::vector<bool> empty;
  std::size_t n = 0;

  [[nodiscard]] std::size_t At(std::size_t u, std::size_t v) const { return (u * n) + v; }
};

PairDivisors DivisorsOf(std::size_t n, std::span<const JoinEdge> edges) {
  PairDivisors out{
      .divisor = std::vector<int64_t>(n * n, 1), .empty = std::vector<bool>(n * n, false), .n = n};
  std::vector<Int128> product(n * n, 0);  // 0: no edge between the pair yet
  for (const JoinEdge& edge : edges) {
    ANTB1_CHECK(edge.a < n);
    ANTB1_CHECK(edge.b < n);
    ANTB1_CHECK(edge.a != edge.b);
    const EdgeDivisor one = DivisorOf(edge);
    const std::size_t at = out.At(std::min(edge.a, edge.b), std::max(edge.a, edge.b));
    if (one.empty) {
      out.empty[at] = true;  // a key with no value at all: the join keeps no row
    }
    // Within a pair the larger domain wins rather than the two multiplying: two keys between the
    // same two relations are one composite key, and its parts narrow together -- a foreign key
    // into a composite primary key is the common case, and multiplying there under-stated Q09's
    // largest join by 7.5x and made the search choose it first (ADR 0026's C5).
    product[at] = std::max({product[at], Int128{1}, Int128{one.divisor}});
  }
  for (std::size_t u = 0; u < n; ++u) {
    for (std::size_t v = u + 1; v < n; ++v) {
      const std::size_t at = out.At(u, v);
      out.divisor[at] = Saturated(std::max(product[at], Int128{1}));
      out.divisor[out.At(v, u)] = out.divisor[at];
      out.empty[out.At(v, u)] = out.empty[at];
    }
  }
  return out;
}

// Dynamic programming over connected subsets (DPccp, Moerkotte and Neumann 2006), minimising the
// rows the joins carry: cost(S) = cost(S1) + cost(S2) + rows(S) over the splits of S, and 0 for a
// single relation, whose scan happens in any order. It ignores what a build costs, which rule 5
// decides from its own estimate, and the scan, which every order pays alike (ADR 0026's C5).
//
// Edges are read as pairs of relation masks rather than pairs of indices. Every edge the binder
// can build today has one relation on each side, so the masks are singletons -- but it makes
// Connected already the test a hypergraph needs, and Neighbours and Connected are then the only
// two places the graph model is consulted, which is the seam DPhyp would replace.
class Searcher {
 public:
  using Mask = std::uint32_t;

  Searcher(std::span<const JoinRelation> relations, std::span<const JoinEdge> edges)
      : relations_(relations), edges_(edges), n_(relations.size()) {
    ANTB1_CHECK(n_ >= 2);
    ANTB1_CHECK(n_ <= kSearchRelations);
    adjacent_.assign(n_, 0);
    ends_.reserve(edges.size());
    for (const JoinEdge& edge : edges) {
      ends_.push_back(Ends{.u = Mask{1} << edge.a, .v = Mask{1} << edge.b});
      adjacent_[edge.a] |= Mask{1} << edge.b;
      adjacent_[edge.b] |= Mask{1} << edge.a;
    }
    rows_.assign(std::size_t{1} << n_, -1);
    memo_.assign(std::size_t{1} << n_, Entry{});
  }

  struct Entry {
    int64_t cost = -1;  // -1: no plan for this subset
    Mask left = 0;      // the split that won; 0 for a single relation
    Mask right = 0;
  };

  // Whether a plan was found for every relation at once. It is not when the relations are not
  // connected, which the binder never passes.
  bool Solve() {
    for (std::size_t r = 0; r < n_; ++r) {
      memo_[Mask{1} << r].cost = 0;
    }
    // The enumeration finds every connected complement pair exactly once; a second pass applies
    // them in order of the size of their union, so the programme can never read a sub-plan it has
    // not solved. Keeping the two apart is what makes the enumeration's bookkeeping testable
    // against a plain reference search rather than only reviewable.
    for (std::size_t i = n_; i-- > 0;) {
      const Mask seed = Mask{1} << i;
      FindComplements(seed);
      GrowSubgraph(seed, Below(i) | seed);
    }
    std::ranges::stable_sort(
        pairs_, [](const std::pair<Mask, Mask>& x, const std::pair<Mask, Mask>& y) {
          const Mask sx = x.first | x.second;
          const Mask sy = y.first | y.second;
          return std::popcount(sx) != std::popcount(sy) ? std::popcount(sx) < std::popcount(sy)
                                                        : sx < sy;
        });
    for (const auto& [left, right] : pairs_) {
      Consider(left, right);
    }
    return memo_[All()].cost >= 0;
  }

  [[nodiscard]] Mask All() const { return static_cast<Mask>((std::uint64_t{1} << n_) - 1); }

  // The steps of the plan that won, children before parents: a subset of one relation is an
  // operand rather than a step, and every other is the join of the split the programme chose.
  // Returns the operand that names the whole plan and the rows it carries.
  struct Emitted {
    JoinOperand operand;
    std::optional<int64_t> rows;  // a relation's own count, or a subtree's estimate
  };

  Emitted Emit(Mask s, int64_t ratio, std::vector<JoinStep>& steps) const {
    if (std::popcount(s) == 1) {
      const std::size_t r = Lowest(s);
      // The relation's own count, unknown and all: ChooseBuildSide must see that it is unknown
      // rather than a stand-in for it, since unknown may never read as small.
      return Emitted{.operand = JoinOperand{.step = false, .index = r}, .rows = relations_[r].rows};
    }
    const Entry& entry = memo_[s];
    const Emitted first = Emit(entry.left, ratio, steps);
    const Emitted second = Emit(entry.right, ratio, steps);
    // A side whose rows nothing knows is the left, so that the other is built: ChooseBuildSide
    // keeps the right on an unknown estimate, and building a relation that turns out to be huge is
    // the mistake the whole rule exists to avoid -- rule 1 kept such a relation as the probe, and
    // this is how the search keeps that. Otherwise the lower minimum relation is the left, which
    // Consider already settled. Orienting by the estimates instead would read better, but it would
    // put the larger side on the left always and so make "unless the right is decisively larger"
    // unreachable: the build-side ratio would silently stop doing anything.
    const bool unknown_right = !second.rows.has_value() && first.rows.has_value();
    const Emitted& left = unknown_right ? second : first;
    const Emitted& right = unknown_right ? first : second;
    const Mask left_mask = unknown_right ? entry.right : entry.left;
    const Mask right_mask = unknown_right ? entry.left : entry.right;
    std::vector<std::size_t> crossing;
    for (std::size_t e = 0; e < ends_.size(); ++e) {
      const Mask u = ends_[e].u;
      const Mask v = ends_[e].v;
      if (((u & left_mask) == u && (v & right_mask) == v) ||
          ((v & left_mask) == v && (u & right_mask) == u)) {
        crossing.push_back(e);
      }
    }
    const int64_t rows = JoinRowsOf(edges_, std::max<int64_t>(left.rows.value_or(kLargest), 0),
                                    std::max<int64_t>(right.rows.value_or(kLargest), 0), crossing);
    steps.push_back(JoinStep{.left = left.operand,
                             .right = right.operand,
                             .edges = std::move(crossing),
                             .build = ChooseBuildSide(left.rows, right.rows, ratio),
                             .estimate = rows});
    return Emitted{.operand = JoinOperand{.step = true, .index = steps.size() - 1}, .rows = rows};
  }

  // Memoised: one subset's rows are read once per split that reaches it.
  int64_t Rows(Mask s) {
    if (rows_[s] < 0) {
      RelationSet set;
      for (std::size_t r = 0; r < n_; ++r) {
        if ((s & (Mask{1} << r)) != 0) {
          set.set(r);
        }
      }
      rows_[s] = SubsetRows(relations_, edges_, set);
    }
    return rows_[s];
  }

 private:
  // The index of the lowest relation in `s`, which must hold one. Spelt with bit_width rather
  // than the obvious standard call, whose name the repository's spell check rejects.
  [[nodiscard]] static std::size_t Lowest(Mask s) {
    return static_cast<std::size_t>(std::bit_width(s & (~s + 1)) - 1);
  }
  // The relations of index below `i`.
  [[nodiscard]] static Mask Below(std::size_t i) { return (Mask{1} << i) - 1; }

  // The relations an edge connects to `s`, minus `s` itself and minus `forbidden`. One of the two
  // places the graph model is read.
  [[nodiscard]] Mask Neighbours(Mask s, Mask forbidden) const {
    Mask out = 0;
    for (std::size_t r = 0; r < n_; ++r) {
      if ((s & (Mask{1} << r)) != 0) {
        out |= adjacent_[r];
      }
    }
    return out & ~s & ~forbidden;
  }

  // Whether an edge joins the two sets: with pair edges, one end in each. The other place the
  // graph model is read, and already the shape a hyperedge needs (u subset of s1, v of s2).
  [[nodiscard]] bool Connected(Mask s1, Mask s2) const {
    return std::ranges::any_of(ends_, [s1, s2](const Ends& ends) {
      return ((ends.u & s1) == ends.u && (ends.v & s2) == ends.v) ||
             ((ends.v & s1) == ends.v && (ends.u & s2) == ends.u);
    });
  }

  // Every non-empty subset of `set`, in a fixed order.
  template <class Visit>
  static void ForEachSubset(Mask set, Visit&& visit) {
    for (Mask sub = set; sub != 0; sub = (sub - 1) & set) {
      visit(sub);
    }
  }

  // Grows a connected subgraph from `s`, emitting the complements of every one it reaches.
  void GrowSubgraph(Mask s, Mask forbidden) {
    const Mask neighbours = Neighbours(s, forbidden);
    ForEachSubset(neighbours, [&](Mask add) { FindComplements(s | add); });
    ForEachSubset(neighbours, [&](Mask add) { GrowSubgraph(s | add, forbidden | neighbours); });
  }

  // The connected complements of `s`, each paired with it.
  void FindComplements(Mask s) {
    const std::size_t lowest = Lowest(s);
    const Mask forbidden = s | Below(lowest);
    const Mask neighbours = Neighbours(s, forbidden);
    for (std::size_t v = n_; v-- > 0;) {
      const Mask bit = Mask{1} << v;
      if ((neighbours & bit) == 0) {
        continue;
      }
      pairs_.emplace_back(s, bit);
      GrowComplement(s, bit, forbidden | (neighbours & (Below(v) | bit)));
    }
  }

  // Grows the complement `s2` of `s1`, pairing the two whenever an edge still joins them.
  void GrowComplement(Mask s1, Mask s2, Mask forbidden) {
    const Mask neighbours = Neighbours(s2, forbidden);
    ForEachSubset(neighbours, [&](Mask add) {
      if (Connected(s1, s2 | add)) {
        pairs_.emplace_back(s1, s2 | add);
      }
    });
    ForEachSubset(neighbours,
                  [&](Mask add) { GrowComplement(s1, s2 | add, forbidden | neighbours); });
  }

  // The join of two solved subsets, if it is the cheapest plan for their union so far. Ties keep
  // the plan found first, and the pairs are applied in a fixed order, so the choice is a function
  // of the relations and the edges alone.
  void Consider(Mask s1, Mask s2) {
    const Entry& left = memo_[s1];
    const Entry& right = memo_[s2];
    ANTB1_CHECK(left.cost >= 0);  // applied in order of size: a sub-plan is always solved
    ANTB1_CHECK(right.cost >= 0);
    const Mask s = s1 | s2;
    // The rows the join produces, and the rows it has to hold to produce them: a hash join builds
    // one side into a table and streams the other past it, so a plan that carries few rows but
    // builds a huge side is not cheap. Counting only the output is the textbook C_out, and it
    // chose exactly such a plan for Q09 -- 79% slower than the greedy one (ADR 0026's C5).
    const Int128 cost = Int128{left.cost} + right.cost + Rows(s) + std::min(Rows(s1), Rows(s2));
    const int64_t total = Saturated(cost);
    Entry& out = memo_[s];
    if (out.cost >= 0 && out.cost <= total) {
      return;
    }
    // The lower minimum relation is the left input, so that a tree reads from where it begins.
    const bool flip = Lowest(s2) < Lowest(s1);
    out = Entry{.cost = total, .left = flip ? s2 : s1, .right = flip ? s1 : s2};
  }

  struct Ends {
    Mask u = 0;
    Mask v = 0;
  };

  std::span<const JoinRelation> relations_;
  std::span<const JoinEdge> edges_;
  std::size_t n_;
  std::vector<Mask> adjacent_;
  std::vector<Ends> ends_;
  std::vector<int64_t> rows_;
  std::vector<Entry> memo_;
  std::vector<std::pair<Mask, Mask>> pairs_;
};

// The greedy walk of OrderJoins.
class Orderer {
 public:
  Orderer(std::span<const JoinRelation> relations, std::span<const JoinEdge> edges,
          int64_t build_side_ratio, JoinOrderSearch search)
      : relations_(relations),
        edges_(edges),
        edges_of_(EdgesOf(relations.size(), edges)),
        joined_(relations.size(), false),
        ratio_(build_side_ratio),
        search_(search) {
    ANTB1_CHECK(build_side_ratio >= 100);
  }

  std::vector<JoinStep> Order() {
    if (search_ == JoinOrderSearch::kCost && relations_.size() >= 2 &&
        relations_.size() <= kSearchRelations) {
      Searcher searcher(relations_, edges_);
      if (searcher.Solve()) {
        std::vector<JoinStep> steps;
        searcher.Emit(searcher.All(), ratio_, steps);
        return steps;
      }
      // No plan for every relation at once: they are not connected, which the binder never
      // passes. Falling through lets the greedy walk's own check say so.
    }
    return Greedy();
  }

  std::vector<JoinStep> Greedy() {
    std::vector<JoinStep> steps;
    if (relations_.size() < 2) {
      if (relations_.size() == 1) {
        joined_[0] = true;
      }
      return steps;  // nothing to join: one relation is its own tree, none is no tree
    }
    std::size_t probe = 0;
    for (std::size_t r = 1; r < relations_.size(); ++r) {
      if (Rows(r) > Rows(probe)) {
        probe = r;
      }
    }
    // The probe is the left input of the first step rather than a step of its own: a step is a
    // join, and the probe joins nothing by itself.
    joined_[probe] = true;
    JoinOperand left{.step = false, .index = probe};
    int64_t joined_rows = Rows(probe);
    while (steps.size() + 1 < relations_.size()) {
      std::optional<std::size_t> next;
      int64_t smallest = 0;
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
      steps.push_back(Add(left, *next, smallest, joined_rows));
      // Every later step joins the result of this one: greedy is left-deep by construction.
      left = JoinOperand{.step = true, .index = steps.size() - 1};
      joined_rows = smallest;
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
  // The rows a join of inputs of `left` and `right` rows produces, from the edges between them,
  // as everything that decides memory must read it: the largest domain of any of those edges
  // divides, so a second key reads as no more selective than the stronger one. A composite key has
  // at least as many distinct values as its most selective column, so this never over-states
  // selectivity -- and under-stating the rows of a build is what materialises a side that does not
  // fit. ADR 0026's C5 records the query that proved it. This is the estimate a step carries and
  // the one ChooseBuildSide reads; only the search reads the narrower one below.
  // The edges between `candidate` and the relations joined so far, ascending.
  [[nodiscard]] std::vector<std::size_t> Crossing(std::size_t candidate) const {
    std::vector<std::size_t> out;
    for (const std::size_t e : edges_of_[candidate]) {
      const JoinEdge& edge = edges_[e];
      if (joined_[edge.a == candidate ? edge.b : edge.a]) {
        out.push_back(e);
      }
    }
    return out;
  }

  [[nodiscard]] std::optional<int64_t> Estimate(std::size_t candidate, int64_t rows) const {
    const std::vector<std::size_t> crossing = Crossing(candidate);
    if (crossing.empty()) {
      return std::nullopt;  // no edge to the joined relations: it cannot join yet
    }
    return JoinRowsOf(edges_, rows, Rows(candidate), crossing);
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

  // The step that joins `relation` to `left`, with its edges to the joined relations as keys.
  JoinStep Add(JoinOperand left, std::size_t relation, int64_t estimate,
               const std::optional<int64_t>& joined_rows) {
    JoinStep step{.left = left,
                  .right = JoinOperand{.step = false, .index = relation},
                  .edges = {},
                  .build = Build(relation, joined_rows),
                  .estimate = estimate};
    step.edges = Crossing(relation);
    joined_[relation] = true;
    return step;
  }

  std::span<const JoinRelation> relations_;
  std::span<const JoinEdge> edges_;
  std::vector<std::vector<std::size_t>> edges_of_;
  std::vector<bool> joined_;
  int64_t ratio_;  // hundredths of a factor, at least 100
  JoinOrderSearch search_;
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
  // Its inputs' rows added up, saturated; unknown where any input's is, since an unknown must never
  // read as small.
  std::optional<int64_t> operator()(const UnionAllNode& node) const {
    Int128 rows = 0;
    for (const LogicalNodePtr& input : node.inputs) {
      const std::optional<int64_t> input_rows = EstimateRows(*input);
      if (!input_rows.has_value()) {
        return std::nullopt;
      }
      rows += std::max<int64_t>(*input_rows, 0);
    }
    return Saturated(rows);
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

int64_t SubsetRows(std::span<const JoinRelation> relations, std::span<const JoinEdge> edges,
                   RelationSet set) {
  const PairDivisors pairs = DivisorsOf(relations.size(), edges);
  // In relation order, multiplying each relation's rows in and dividing by the divisor of every
  // pair it closes with a relation already taken in. Dividing as it goes keeps the running value
  // near a real cardinality, so nothing overflows in practice, and the order is fixed, so the
  // answer is the subset's and not any order's.
  Int128 running = 1;
  RelationSet done;
  bool empty = false;
  for (std::size_t r = 0; r < relations.size(); ++r) {
    if (!set.test(r)) {
      continue;
    }
    const int64_t rows = BlockRows(relations, r);
    running = Multiplied(running, rows);
    empty = empty || rows == 0;
    for (std::size_t u = 0; u < r; ++u) {
      if (!done.test(u)) {
        continue;
      }
      empty = empty || pairs.empty[pairs.At(u, r)];
      running /= pairs.divisor[pairs.At(u, r)];
    }
    done.set(r);
  }
  return empty ? 0 : std::max<int64_t>(Saturated(running), 1);
}

int64_t OrderCost(std::span<const JoinRelation> relations, std::span<const JoinEdge> edges,
                  std::span<const JoinStep> steps) {
  // Per step, the relations below it, so that its own rows can be read from the set it produces.
  std::vector<RelationSet> below(steps.size());
  Int128 cost = 0;
  for (std::size_t k = 0; k < steps.size(); ++k) {
    const auto set_of = [&](const JoinOperand& operand) {
      RelationSet out;
      if (operand.step) {
        out = below[operand.index];
      } else {
        out.set(operand.index);
      }
      return out;
    };
    const RelationSet left = set_of(steps[k].left);
    const RelationSet right = set_of(steps[k].right);
    below[k] = left | right;
    cost += SubsetRows(relations, edges, below[k]) +
            std::min(SubsetRows(relations, edges, left), SubsetRows(relations, edges, right));
  }
  return Saturated(cost);
}

std::vector<JoinStep> OrderJoins(std::span<const JoinRelation> relations,
                                 std::span<const JoinEdge> edges, int64_t build_side_ratio,
                                 JoinOrderSearch search) {
  return Orderer(relations, edges, build_side_ratio, search).Order();
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

namespace {

// Every column a Scan below `node` defines, as its table and field: the only columns whose domain
// the footers can give. Collected rather than looked up one at a time because one walk answers for
// every column of a pre-grouping.
void ScannedColumns(const LogicalNode& node,
                    std::unordered_map<ColumnId, std::pair<const Table*, int>>& out) {
  if (const auto* scan = std::get_if<ScanNode>(&node)) {
    for (std::size_t i = 0; i < scan->ids.size() && i < scan->fields.size(); ++i) {
      if (scan->ids[i] != kNoColumnId && scan->table != nullptr) {
        out.emplace(scan->ids[i], std::pair(scan->table.get(), scan->fields[i]));
      }
    }
    return;
  }
  for (const LogicalNodePtr& input : InputsOf(node)) {
    if (input != nullptr) {
      ScannedColumns(*input, out);
    }
  }
}

}  // namespace

std::vector<std::optional<int64_t>> EachDistinctValues(const LogicalNode& node,
                                                       std::span<const ColumnId> columns) {
  std::vector<std::optional<int64_t>> out;
  out.reserve(columns.size());
  if (columns.empty()) {
    return out;
  }
  std::unordered_map<ColumnId, std::pair<const Table*, int>> scanned;
  ScannedColumns(node, scanned);  // once, however many columns are asked about
  for (const ColumnId column : columns) {
    const auto found = scanned.find(column);
    if (found == scanned.end() || found->second.first == nullptr) {
      out.emplace_back(std::nullopt);  // computed, or from no Scan of this subtree
      continue;
    }
    out.push_back(KeyDomain(*found->second.first, found->second.second));
  }
  return out;
}

std::optional<int64_t> DistinctValues(const LogicalNode& node, std::span<const ColumnId> columns) {
  if (columns.empty()) {
    return std::nullopt;
  }
  Int128 product = 1;
  for (const std::optional<int64_t>& domain : EachDistinctValues(node, columns)) {
    if (!domain.has_value()) {
      return std::nullopt;
    }
    product = Multiplied(product, std::max<int64_t>(*domain, 1));
  }
  const int64_t groups = Saturated(product);
  const std::optional<int64_t> rows = EstimateRows(node);
  return rows.has_value() ? std::min(groups, std::max<int64_t>(*rows, 1)) : groups;
}

}  // namespace antb1::plan
