#include "antb1/plan/shared_sub_plans.h"

#include <algorithm>
#include <array>
#include <bitset>
#include <cstdint>
#include <format>
#include <functional>
#include <iterator>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

#include <arrow/type.h>

#include "antb1/common/narrow.h"
#include "antb1/plan/logical_plan.h"
#include "antb1/plan/types.h"

#include "join_order.h"
#include "predicate_domain.h"

namespace antb1::plan {
namespace {

// Defined below, and used by Origins: a constant as its type and its text.
std::string ConstantKey(const Constant& constant);

// A column's plan-wide ordinal: two columns get the same one exactly when they come from the same
// place. This is the whole trick of the pass. A ColumnId is minted per reference, so two references
// to one CTE never share one, and BoundColumn::index is a position, which a scan reading one field
// fewer shifts in every node above it -- so neither can match the two sides. Where a column *comes
// from* is the same on both sides, and that is what this interns.
//
// **"The same place" includes the way there.** A table's field is one origin wherever it is read,
// so the two sides of a self-join read columns of one origin, and so do the COUNT(*)s of two
// aggregations: an origin alone cannot tell `a.k` from `b.k`, and two sub-plans grouping one by
// `a.k` and the other by `b.k` once keyed as one and were answered from one result. So every
// reference -- in a node's key, and inside the origin of a column defined from it -- carries its
// path: the inputs it is read through, from the node that reads it down to the node that defines
// it. Equivalent sub-plans have one shape, so their references take the same paths; the two sides
// of a join take different ones.
class Origins {
 public:
  static constexpr int kUnknown = -1;

  // Defines an ordinal for every column the plan's nodes mint, and the path of every column each
  // node outputs, inputs first.
  void Walk(const LogicalNodePtr& node) {
    if (node == nullptr) {
      return;
    }
    for (const LogicalNodePtr& input : InputsOf(*node)) {
      Walk(input);
    }
    Trace(*node);
    std::visit([this, &node](const auto& n) { Define(n, *node); }, *node);
  }

  [[nodiscard]] int Of(ColumnId id) const {
    const auto it = of_.find(static_cast<std::uint32_t>(id));
    return it == of_.end() ? kUnknown : it->second;
  }

  // A column reference that `at` reads, as a key: its ordinal and its path, or its position when it
  // has no id (the executor's positional plans, which never reach here from the binder).
  [[nodiscard]] std::string Key(const BoundColumn& column, const LogicalNode& at,
                                bool include_layout = false) const {
    if (column.id == kNoColumnId) {
      return std::format("@{}", column.index);
    }
    const auto path = PathOf(column.id, at);
    if (include_layout) {
      return std::format("c{}{}@{}:{}", Of(column.id), path, column.index, ToString(column.type));
    }
    return std::format("c{}{}", Of(column.id), path);
  }
  [[nodiscard]] std::string Key(const std::optional<BoundColumn>& column, const LogicalNode& at,
                                bool include_layout = false) const {
    return column.has_value() ? Key(*column, at, include_layout) : "-";
  }

  // The path from `at` to the node that defines `id`, which `at` reads from one of its inputs: that
  // input's index, then the input's own path to it. "/?" for a column read from outside `at`'s
  // inputs, a free variable, which no plan the physical planner gets holds.
  [[nodiscard]] std::string PathOf(ColumnId id, const LogicalNode& at) const {
    const std::vector<LogicalNodePtr> inputs = InputsOf(at);
    for (std::size_t k = 0; k < inputs.size(); ++k) {
      if (const std::string* path = Within(inputs[k].get(), id); path != nullptr) {
        return std::format("/{}{}", k, *path);
      }
    }
    return "/?";
  }

  // The path of `id` inside `node`, which outputs it: empty where `node` defines it. Null where
  // `node` does not output it.
  [[nodiscard]] const std::string* Within(const LogicalNode* node, ColumnId id) const {
    const auto paths = paths_.find(node);
    if (paths == paths_.end()) {
      return nullptr;
    }
    const auto path = paths->second.find(static_cast<std::uint32_t>(id));
    return path == paths->second.end() ? nullptr : &path->second;
  }

 private:
  // The paths of `node`'s output columns: a column one of its inputs outputs passes through it --
  // that input's index, then its path there -- and any other is the node's own. None for a node
  // with an input missing, or with one missing anywhere below it: a malformed plan, which the
  // physical planner rejects after this pass, and whose output a walk could not even list. A read
  // through such a node has no path.
  void Trace(const LogicalNode& node) {
    const std::vector<LogicalNodePtr> inputs = InputsOf(node);
    for (const LogicalNodePtr& input : inputs) {
      if (input == nullptr || !paths_.contains(input.get())) {
        return;
      }
    }
    std::unordered_map<std::uint32_t, std::string>& own = paths_[&node];
    for (const ColumnId id : OutputIds(node)) {
      std::string path;
      for (std::size_t k = 0; k < inputs.size(); ++k) {
        if (const std::string* below = Within(inputs[k].get(), id); below != nullptr) {
          path = std::format("/{}{}", k, *below);
          break;
        }
      }
      own.emplace(static_cast<std::uint32_t>(id), std::move(path));
    }
  }

  int Intern(const std::string& origin) {
    const auto [it, added] = interned_.try_emplace(origin, static_cast<int>(interned_.size()));
    return it->second;
  }
  void Set(ColumnId id, const std::string& origin) {
    if (id != kNoColumnId) {
      of_[static_cast<std::uint32_t>(id)] = Intern(origin);
    }
  }

  // One overload per node type, so a new node type fails to compile. Only the nodes that *mint*
  // columns define anything: a Filter, a Sort, a Limit and a Join pass their inputs' columns
  // through, and ids survive a change of position (logical_plan.h). `at` is the node itself, from
  // which the paths of what it reads start.
  void Define(const ScanNode& node, const LogicalNode& /*at*/) {
    for (std::size_t i = 0; i < node.ids.size() && i < node.fields.size(); ++i) {
      Set(node.ids[i], std::format("{}#{}", node.table_name, node.fields[i]));
    }
  }
  void Define(const WindowNode& node, const LogicalNode& /*at*/) {
    for (const WindowCall& call : node.calls) {
      Set(call.id, std::format("window#{}", std::to_underlying(call.id)));
    }
  }
  void Define(const ComputeNode& node, const LogicalNode& at);
  void Define(const ProjectNode& node, const LogicalNode& at) {
    for (std::size_t i = 0; i < node.ids.size(); ++i) {
      const std::optional<Constant>* constant =
          i < node.constants.size() ? &node.constants[i] : nullptr;
      if (constant != nullptr && constant->has_value()) {
        Set(node.ids[i], std::format("k{}", ConstantKey(**constant)));
      } else if (i < node.columns.size()) {
        Set(node.ids[i], std::format("={}", Key(node.columns[i], at)));
      }
    }
  }
  void Define(const AggregateNode& node, const LogicalNode& at) {
    for (const AggregateCall& call : node.aggregates) {
      Set(call.id, CallOrigin(call, at));
    }
  }
  void Define(const GroupAggregateNode& node, const LogicalNode& at) {
    for (std::size_t i = 0; i < node.key_ids.size() && i < node.keys.size(); ++i) {
      Set(node.key_ids[i], std::format("key({})", Key(node.keys[i], at)));
    }
    for (const AggregateCall& call : node.aggregates) {
      Set(call.id, CallOrigin(call, at));
    }
  }
  void Define(const GroupingSetsNode& node, const LogicalNode& at) {
    for (std::size_t i = 0; i < node.key_ids.size() && i < node.keys.size(); ++i) {
      Set(node.key_ids[i], std::format("gkey({})", Key(node.keys[i], at)));
    }
    for (const AggregateCall& call : node.aggregates) {
      Set(call.id, std::format("g{}", CallOrigin(call, at)));
    }
    // A GROUPING column by its arguments' keys and the sets: what decides its values.
    std::string sets;
    for (const std::vector<std::size_t>& set : node.sets) {
      for (const std::size_t k : set) {
        sets += k < node.keys.size() ? Key(node.keys[k], at) : std::string("-");
        sets += ',';
      }
      sets += ';';
    }
    for (const GroupingCall& call : node.grouping) {
      std::string origin = "grouping(";
      for (const std::size_t k : call.args) {
        origin += k < node.keys.size() ? Key(node.keys[k], at) : std::string("-");
        origin += ',';
      }
      Set(call.id, std::format("{}|{})", origin, sets));
    }
  }
  void Define(const RowCountNode& node, const LogicalNode& /*at*/) {
    Set(node.id, std::format("rowcount({})", node.table_name));
  }
  void Define(const DependentJoinNode& node, const LogicalNode& /*at*/) { Set(node.mark, "mark"); }
  // A union's column comes from the columns it reads at its position, one per input, in order, each
  // by its path through its own input.
  void Define(const UnionAllNode& node, const LogicalNode& at) {
    for (std::size_t i = 0; i < node.ids.size(); ++i) {
      std::string origin = "u(";
      for (const std::vector<BoundColumn>& input : node.columns) {
        origin += i < input.size() ? Key(input[i], at) : std::string("-");
        origin += ',';
      }
      Set(node.ids[i], origin + ')');
    }
  }
  // A set operation's column likewise, under a mark of its own: no union's column is one of it.
  void Define(const SetOperationNode& node, const LogicalNode& at) {
    for (std::size_t i = 0; i < node.ids.size(); ++i) {
      std::string origin = "s(";
      for (const std::vector<BoundColumn>& input : node.columns) {
        origin += i < input.size() ? Key(input[i], at) : std::string("-");
        origin += ',';
      }
      Set(node.ids[i], origin + ')');
    }
  }
  void Define(const FilterNode& /*node*/, const LogicalNode& /*at*/) {}
  void Define(const SortNode& /*node*/, const LogicalNode& /*at*/) {}
  void Define(const LimitNode& /*node*/, const LogicalNode& /*at*/) {}
  void Define(const JoinNode& node, const LogicalNode& at);

  // A call's origin: what it computes over which argument. COUNT(*) has none, so every COUNT(*) of
  // the plan has one origin whatever it counts -- and a read of one is told from a read of another
  // by its path, as two sides' columns of one table are.
  [[nodiscard]] std::string CallOrigin(const AggregateCall& call, const LogicalNode& at) const {
    return std::format("agg({},{},{})", static_cast<int>(call.kind), Key(call.arg, at),
                       ToString(call.type));
  }

  std::unordered_map<std::uint32_t, int> of_;
  std::unordered_map<std::string, int> interned_;
  // Per node, the path of each column it outputs (Trace).
  std::unordered_map<const LogicalNode*, std::unordered_map<std::uint32_t, std::string>> paths_;
};

// A constant as a key: **its type and its text**, never the text alone. ToString is a display
// format, which renders 1 the same whatever integer type it has and says nothing about a DECIMAL's
// precision -- and a constant's type is part of what it means. On a DECIMAL column a predicate's
// constant is the column's type or DOUBLE, and those two compare differently (ADR 0021 rule 11); a
// Project's constant decides its output column's type outright. Keying on the text alone would let
// two sub-plans that compute different values be grouped, which is the one mistake this pass must
// not make -- and it would be the same error as hanging identity on a display format, which the
// column keys above deliberately avoid.
std::string ConstantKey(const Constant& constant) {
  return std::format("{}:{}", ToString(constant.type), ToString(constant));
}

// `p`, read by `at`, as a key. With `local`, its columns are the operand-local ones of a
// PredicateExpr (index 0 and 1 into the operands) rather than columns of a node's input, so they
// are keyed by position.
std::string PredicateKey(const Predicate& p, const Origins& origins, bool local,
                         const LogicalNode& at, bool include_layout = false) {
  const auto column = [&](const std::optional<BoundColumn>& c) {
    if (!c.has_value()) {
      return std::string("-");
    }
    if (local) {
      return include_layout ? std::format("@{}:{}", c->index, ToString(c->type))
                            : std::format("@{}", c->index);
    }
    return origins.Key(*c, at, include_layout);
  };
  std::string key = std::format("P{},{},{},{},{}", static_cast<int>(p.kind), column(p.column),
                                column(p.other), static_cast<int>(p.op), ConstantKey(p.constant));
  for (const Constant& value : p.values) {
    key += ',';
    key += ConstantKey(value);
  }
  return key;
}

// An expression that `at` computes, as a key: its structure and type with every column reference by
// origin and path, and without its display name (which SameExpr ignores too, for the same reason).
std::string ExprKey(const ExprPtr& expr, const Origins& origins, const LogicalNode& at,
                    bool include_layout = false) {
  if (expr == nullptr) {
    return "_";
  }
  const auto child = [&](const ExprPtr& input) {
    return ExprKey(input, origins, at, include_layout);
  };
  const auto args = [&child](const std::vector<ExprPtr>& list) {
    std::string out;
    for (const ExprPtr& arg : list) {
      out += child(arg);
      out += ',';
    }
    return out;
  };
  const std::string node = std::visit(
      [&](const auto& e) -> std::string {
        using T = std::decay_t<decltype(e)>;
        if constexpr (std::is_same_v<T, ColumnExpr>) {
          if (e.id == kNoColumnId) {
            return std::format("@{}", e.index);
          }
          const auto path = origins.PathOf(e.id, at);
          return include_layout ? std::format("c{}{}@{}", origins.Of(e.id), path, e.index)
                                : std::format("c{}{}", origins.Of(e.id), path);
        } else if constexpr (std::is_same_v<T, ConstantExpr>) {
          return std::format("k{}", ConstantKey(e.value));
        } else if constexpr (std::is_same_v<T, ArithExpr>) {
          return std::format("A{}({},{})", static_cast<int>(e.op), child(e.left), child(e.right));
        } else if constexpr (std::is_same_v<T, NegateExpr>) {
          return std::format("N({})", child(e.operand));
        } else if constexpr (std::is_same_v<T, FunctionExpr>) {
          return std::format("F{}({})", static_cast<int>(e.function), args(e.args));
        } else if constexpr (std::is_same_v<T, PredicateExpr>) {
          return std::format("E{}({})",
                             PredicateKey(e.predicate, origins, /*local=*/true, at, include_layout),
                             args(e.operands));
        } else if constexpr (std::is_same_v<T, BoolExpr>) {
          return std::format("B{}({})", static_cast<int>(e.op), args(e.args));
        } else if constexpr (std::is_same_v<T, CaseExpr>) {
          return std::format("W({}|{}|{})", args(e.whens), args(e.thens), child(e.otherwise));
        } else if constexpr (std::is_same_v<T, CastExpr>) {
          return std::format("X({})", child(e.operand));
        } else {
          static_assert(false, "an expression type without a key");
        }
      },
      expr->node);
  return std::format("{}:{}", node, ToString(expr->type));
}

void Origins::Define(const ComputeNode& node, const LogicalNode& at) {
  for (std::size_t i = 0; i < node.ids.size() && i < node.exprs.size(); ++i) {
    Set(node.ids[i], std::format("e{}", ExprKey(node.exprs[i], *this, at)));
  }
}

// A join passes its inputs' columns through; a mark join also mints its mark, which depends on its
// kind (a null-aware mark is three-valued, the other two-valued), its probe keys and its set's keys
// (ADR 0041's decision 13), and a mark's residuals (its SQ8), so that two marks of one subtree over
// other kinds, keys or conditions stay apart. Their subtrees' keys tell them apart already; this
// keeps the mark's own origin from resting on that.
void Origins::Define(const JoinNode& node, const LogicalNode& at) {
  std::string origin = std::format("mark({},", static_cast<int>(node.kind));
  for (const JoinKey& key : node.keys) {
    origin += Key(key.left, at) + (key.null_equal ? "<=>" : "=") + Key(key.right, at) + ',';
  }
  origin += ';';
  for (const ExprPtr& expr : node.residual) {
    origin += ExprKey(expr, *this, at);
    origin += ';';
  }
  Set(node.mark, origin + ')');
}

// One node's own content as a key, or none when a sub-plan holding it is not shareable.
//
// A Scan's fields are deliberately absent: selecting fields does not change which rows a scan
// returns, so two scans of one table differing only there compute the same rows and the shared
// sub-plan reads the union. A Join's build side and estimate are absent too -- both are choices
// about *how* to run it, and neither changes the rows or their order.
std::optional<std::string> NodeKey(const LogicalNode& node, const Origins& origins,
                                   const Predicate* partition) {
  return std::visit(
      [&](const auto& n) -> std::optional<std::string> {
        using T = std::decay_t<decltype(n)>;
        if constexpr (std::is_same_v<T, ScanNode>) {
          if (n.table == nullptr) {
            return std::nullopt;  // sharing a valid member must not mask a malformed scan
          }
          if (partition == nullptr) {
            return std::format("scan({})", n.table_name);
          }
          // The combined producer must decode every field an individual reader would visit.
          std::string key =
              std::format("scan({},{},", n.table_name, static_cast<const void*>(n.table.get()));
          for (const int field : n.fields) {
            key += std::format("{},", field);
          }
          return key + ')';
        } else if constexpr (std::is_same_v<T, FilterNode>) {
          std::string key = "filter(";
          for (const Predicate& p : n.predicates) {
            key += &p == partition
                       ? std::format("partition({})", origins.Key(p.column, node, true))
                       : PredicateKey(p, origins, /*local=*/false, node, partition != nullptr);
            key += ';';
          }
          return key + ')';
        } else if constexpr (std::is_same_v<T, ComputeNode>) {
          std::string key = "compute(";
          for (const ExprPtr& expr : n.exprs) {
            key += ExprKey(expr, origins, node, partition != nullptr);
            key += ';';
          }
          return key + ')';
        } else if constexpr (std::is_same_v<T, ProjectNode>) {
          std::string key = "project(";
          for (std::size_t i = 0; i < n.columns.size(); ++i) {
            if (i < n.constants.size() && n.constants[i].has_value()) {
              key += std::format("k{};", ConstantKey(*n.constants[i]));
            } else {
              key += std::format("{};", origins.Key(n.columns[i], node));
            }
          }
          return key + ')';
        } else if constexpr (std::is_same_v<T, AggregateNode>) {
          std::string key = "aggregate(";
          for (const AggregateCall& call : n.aggregates) {
            key +=
                std::format("{},{},{};", static_cast<int>(call.kind),
                            origins.Key(call.arg, node, partition != nullptr), ToString(call.type));
          }
          return key + ')';
        } else if constexpr (std::is_same_v<T, GroupAggregateNode>) {
          std::string key = "group(";
          for (const BoundColumn& k : n.keys) {
            key += origins.Key(k, node, partition != nullptr);
            key += ',';
          }
          key += ';';
          for (const AggregateCall& call : n.aggregates) {
            key +=
                std::format("{},{},{};", static_cast<int>(call.kind),
                            origins.Key(call.arg, node, partition != nullptr), ToString(call.type));
          }
          return key + ')';
        } else if constexpr (std::is_same_v<T, JoinNode>) {
          std::string key = std::format("join({},", static_cast<int>(n.kind));
          if (partition != nullptr) {
            key += std::format("build{},", static_cast<int>(n.build));
          }
          for (const JoinKey& k : n.keys) {
            key += std::format("{}{}{},", origins.Key(k.left, node, partition != nullptr),
                               k.null_equal ? "<=>" : "=",
                               origins.Key(k.right, node, partition != nullptr));
          }
          key += ';';
          for (const ExprPtr& expr : n.residual) {
            key += ExprKey(expr, origins, node, partition != nullptr);
            key += ';';
          }
          return key + ')';
        } else if constexpr (std::is_same_v<T, UnionAllNode> ||
                             std::is_same_v<T, SetOperationNode>) {
          // A shared producer may remap a filter through every branch. Leave malformed maps
          // to the physical planner, which reports the missing list or incorrect width.
          if (n.columns.size() != n.inputs.size() ||
              std::ranges::any_of(n.columns,
                                  [&](const auto& cols) { return cols.size() != n.ids.size(); })) {
            return std::nullopt;
          }
          // SubtreeKey appends the inputs in order. A distinct set operation also needs its
          // formula.
          std::string key;
          if constexpr (std::is_same_v<T, SetOperationNode>) {
            key = "setop(";
            for (const SetTerm& term : n.formula) {
              key += std::format("{}:{}:{},", std::to_underlying(term.kind), term.input,
                                 term.operands);
            }
            key += ';';
          } else {
            key = "union(";
          }
          for (const std::vector<BoundColumn>& input : n.columns) {
            for (const BoundColumn& column : input) {
              key += origins.Key(column, node);
              key += ',';
            }
            key += ';';
          }
          return key + ')';
        } else if constexpr (std::is_same_v<T, SortNode> || std::is_same_v<T, LimitNode> ||
                             std::is_same_v<T, RowCountNode> ||
                             std::is_same_v<T, DependentJoinNode> ||
                             std::is_same_v<T, GroupingSetsNode> || std::is_same_v<T, WindowNode>) {
          // Not shareable, so no sub-plan holding one is a candidate (shared_sub_plans.h).
          return std::nullopt;
        } else {
          static_assert(false, "a node type without a key");
        }
      },
      node);
}

// The key of a whole sub-tree, memoised by node pointer: two references to one CTE are distinct
// pointers, so the memo never conflates them.
using KeyMemo = std::unordered_map<const LogicalNode*, std::optional<std::string>>;

const std::optional<std::string>& SubtreeKey(const LogicalNodePtr& node, const Origins& origins,
                                             KeyMemo& memo, const Predicate* partition = nullptr) {
  if (node == nullptr) {
    // Keyed by nullptr rather than returning a reference to a static: a missing input is not
    // shareable, which is what an empty key says.
    return memo.try_emplace(nullptr, std::nullopt).first->second;
  }
  const auto found = memo.find(node.get());
  if (found != memo.end()) {
    return found->second;
  }
  std::optional<std::string> key = NodeKey(*node, origins, partition);
  if (key.has_value()) {
    for (const LogicalNodePtr& input : InputsOf(*node)) {
      const std::optional<std::string>& child = SubtreeKey(input, origins, memo, partition);
      if (!child.has_value()) {
        key.reset();
        break;
      }
      *key += '[';
      *key += *child;
      *key += ']';
    }
  }
  return memo.emplace(node.get(), std::move(key)).first->second;
}

// The fields the one shared sub-plan would read: per scan, the union over `members`. Their keys are
// equal, so their shapes are, and a lockstep walk meets the same scan in each.
int64_t FieldUnion(const std::vector<LogicalNodePtr>& members) {
  if (members.empty() || members.front() == nullptr) {
    return 0;
  }
  if (const auto* scan = std::get_if<ScanNode>(members.front().get()); scan != nullptr) {
    std::set<int> fields;
    for (const LogicalNodePtr& member : members) {
      if (const auto* other = std::get_if<ScanNode>(member.get()); other != nullptr) {
        fields.insert(other->fields.begin(), other->fields.end());
      }
    }
    return static_cast<int64_t>(fields.size());
  }
  int64_t total = 0;
  const std::vector<LogicalNodePtr> inputs = InputsOf(*members.front());
  for (std::size_t i = 0; i < inputs.size(); ++i) {
    std::vector<LogicalNodePtr> nth;
    nth.reserve(members.size());
    for (const LogicalNodePtr& member : members) {
      const std::vector<LogicalNodePtr> theirs = InputsOf(*member);
      if (i < theirs.size()) {
        nth.push_back(theirs[i]);
      }
    }
    total += FieldUnion(nth);
  }
  return total;
}

// ADR 0030: below this threshold, rescanning costs less than materializing a result and scanning
// it twice. Unknown input row counts decline sharing. Keep the total to compare it with the
// output of a compound candidate, saturating only when it exceeds the representable row count.
constexpr int64_t kMinSharedRows = 10'000'000;
// Fixed-width IN evaluates one comparison per value. The scan selectivity heuristic expects
// ten equality values to cover the domain; avoid adding that work below a group.
constexpr std::size_t kMaxSharedFilterValues = 9;

std::optional<int64_t> InputRows(const LogicalNode& node) {
  if (const auto* scan = std::get_if<ScanNode>(&node)) {
    return scan->table == nullptr ? std::nullopt : scan->table->exact_row_count();
  }
  int64_t rows = 0;
  for (const LogicalNodePtr& input : InputsOf(node)) {
    const auto child = input == nullptr ? std::nullopt : InputRows(*input);
    if (!child.has_value()) {
      return std::nullopt;
    }
    rows += std::clamp(*child, int64_t{0}, std::numeric_limits<int64_t>::max() - rows);
  }
  return rows;
}

int64_t FixedBytes(LogicalType type) {
  const int64_t bits = ToArrow(type)->bit_width();
  return bits > 0 ? (bits + 7) / 8 : 0;  // VARCHAR reports no fixed width.
}

struct AggregateSize {
  const GroupAggregateNode* grouped = nullptr;  // a global aggregate has one output row
  int64_t fixed_row_bytes = 0;
};

// Only aggregate results qualify: scans, joins and set operations can produce more data than
// they read. Look through row-preserving projects and computes, accounting for their output.
std::optional<AggregateSize> SharedAggregateSize(const LogicalNodePtr& node) {
  if (const auto* project = std::get_if<ProjectNode>(node.get())) {
    auto size = SharedAggregateSize(project->input);
    if (size.has_value()) {
      size->fixed_row_bytes = 0;
      for (std::size_t i = 0; i < project->columns.size(); ++i) {
        LogicalType type = project->columns[i].type;
        if (i < project->constants.size()) {
          if (const auto& constant = project->constants[i]; constant.has_value()) {
            type = constant->type;
          }
        }
        size->fixed_row_bytes += FixedBytes(type);
      }
    }
    return size;
  }
  if (const auto* compute = std::get_if<ComputeNode>(node.get())) {
    auto size = SharedAggregateSize(compute->input);
    if (size.has_value()) {
      for (const ExprPtr& expr : compute->exprs) {
        size->fixed_row_bytes += FixedBytes(expr->type);
      }
    }
    return size;
  }
  AggregateSize size;
  const std::vector<AggregateCall>* calls = nullptr;
  if (const auto* group = std::get_if<GroupAggregateNode>(node.get())) {
    size.grouped = group;
    for (const BoundColumn& key : group->keys) {
      size.fixed_row_bytes += FixedBytes(key.type);
    }
    calls = &group->aggregates;
  } else if (const auto* global = std::get_if<AggregateNode>(node.get())) {
    calls = &global->aggregates;
  } else {
    return std::nullopt;
  }
  for (const AggregateCall& call : *calls) {
    size.fixed_row_bytes += FixedBytes(call.type);
  }
  return size;
}

// Every tuple from this scan belongs to a row in one part. Bound it by the table rows,
// the product of the table-wide domains, or the per-part product times the number of
// parts. The latter counts the parts once even when several keys come from the same scan.
int64_t AfterScan(const ScanNode& scan, std::span<const ScanKeyDomain> sources, int64_t limit) {
  const auto divide = [](int64_t value, std::optional<int64_t> bound) -> int64_t {
    return bound.has_value() ? value / std::max<int64_t>(*bound, 1) : 0;
  };
  int64_t by_table = limit;
  int64_t by_part = divide(limit, scan.table->num_parts());
  for (const auto& source : sources) {
    if (source.scan == &scan) {
      by_table = divide(by_table, source.max_values);
      by_part = divide(
          by_part, source.max_part_values.has_value() ? source.max_part_values : source.max_values);
    }
  }
  return std::max({by_table, by_part, divide(limit, scan.table->exact_row_count())});
}

// A filter on one scan can bound several of that scan's grouping keys together. Keys from
// other scans need a table-wide bound. Compare by division so a product cannot overflow.
struct GroupSizeLimit {
  std::span<const ColumnId> keys;
  std::span<const ScanKeyDomain> sources;
  int64_t max_groups;
  int& remaining;

  bool Fits(const FilterNode& filter, const ScanNode* scan) const {
    std::array<PredicateDomainKey, kMaxPredicateDomainKeys> local;
    std::size_t count = 0;
    int64_t limit = max_groups;
    for (std::size_t i = 0; i < keys.size(); ++i) {
      if (sources[i].scan == scan) {
        local[count++] = {.id = keys[i], .max_values = sources[i].max_values};
      } else if (sources[i].scan == nullptr) {
        return false;
      } else if (!std::ranges::contains(sources.first(i), sources[i].scan, &ScanKeyDomain::scan)) {
        limit = AfterScan(*sources[i].scan, sources, limit);
      }
    }
    const auto bound = PredicateTupleBound(filter, std::span(local).first(count), remaining);
    return bound.has_value() && *bound <= limit;
  }
};

struct FilteredScan {
  const ScanNode* scan = nullptr;  // set only for a branch over a single scan
  bool eligible = false;
  bool fits = false;
};

FilteredScan CheckFilteredGroups(const LogicalNode& node, const GroupSizeLimit& limit) {
  if (limit.remaining == 0) {
    return {};
  }
  --limit.remaining;
  if (const auto* scan = std::get_if<ScanNode>(&node)) {
    return {.scan = scan, .eligible = true};
  }
  if (const auto* filter = std::get_if<FilterNode>(&node)) {
    auto child = CheckFilteredGroups(*filter->input, limit);
    if (child.scan != nullptr && !child.fits) {
      child.fits = limit.Fits(*filter, child.scan);
    }
    return child;
  }
  if (const auto* compute = std::get_if<ComputeNode>(&node)) {
    return CheckFilteredGroups(*compute->input, limit);
  }
  if (const auto* project = std::get_if<ProjectNode>(&node)) {
    return CheckFilteredGroups(*project->input, limit);
  }
  // An inner join can repeat a filtered tuple but cannot introduce a new one. A null-extending
  // join can introduce grouping values absent from the filtered branch.
  if (const auto* join = std::get_if<JoinNode>(&node);
      join != nullptr && join->kind == JoinKind::kInner) {
    const auto left = CheckFilteredGroups(*join->left, limit);
    if (left.eligible) {
      const auto right = CheckFilteredGroups(*join->right, limit);
      return {.eligible = right.eligible, .fits = right.eligible && (left.fits || right.fits)};
    }
  }
  return {};
}

bool FitsSharedResult(const AggregateSize& size, int64_t columns, int& remaining) {
  if (size.grouped == nullptr) {
    return size.fixed_row_bytes <= kMaxSharedResultBytes;
  }
  if (size.fixed_row_bytes == 0) {
    return true;
  }
  const auto& group = *size.grouped;
  std::vector<ColumnId> keys;
  keys.reserve(group.keys.size());
  for (const auto& key : group.keys) {
    keys.push_back(key.id);
  }
  const int64_t limit = kMaxSharedResultBytes / size.fixed_row_bytes;
  if (remaining <= 0 || keys.size() > kMaxPredicateDomainKeys) {
    return DistinctValues(*group.input, keys).value_or(0) <= limit;
  }
  std::array<ScanKeyDomain, kMaxPredicateDomainKeys> storage;
  auto sources = std::span(storage).first(keys.size());
  const auto estimates = EachDistinctValues(*group.input, keys, sources);
  if (GroupCountFromDomains(*group.input, estimates).value_or(0) <= limit) {
    return true;
  }
  // Reversing the original refusal must leave room for validity buffers too. Each output
  // batch rounds each column's bitmap up to whole bytes: one byte per row and column
  // covers even a one-row batch, independently of which output columns turn out nullable.
  const int64_t refined_limit = kMaxSharedResultBytes / (size.fixed_row_bytes + columns);
  return CheckFilteredGroups(*group.input, {.keys = keys,
                                            .sources = sources,
                                            .max_groups = refined_limit,
                                            .remaining = remaining})
      .fits;
}

// The group of `members`, or none when their outputs do not nest -- when no member's output columns
// contain every other's, so that one result could not serve them all. Overlapping-but-not-nested
// outputs are possible in principle (two scans reading {0,1} and {1,2}) and are reported as nothing
// rather than as a group a consumer could not read.
std::optional<SharedSubPlan> GroupOf(std::vector<LogicalNodePtr> members, const Origins& origins) {
  if (members.size() < 2) {
    return std::nullopt;
  }
  // Each member's output columns as origin ordinals and paths inside the member: two columns match
  // when they come from the same place by the same way, which is the only sense in which the
  // members' outputs can be compared -- their ColumnIds are minted per reference and never equal,
  // and an ordinal alone would match a join's two sides' columns of one table to each other.
  std::vector<std::vector<std::string>> outputs;
  outputs.reserve(members.size());
  for (const LogicalNodePtr& member : members) {
    std::vector<std::string> columns;
    for (const ColumnId id : OutputIds(*member)) {
      const int ordinal = origins.Of(id);
      const std::string* path = origins.Within(member.get(), id);
      if (ordinal == Origins::kUnknown || path == nullptr) {
        return std::nullopt;  // a column of no known origin matches nothing
      }
      columns.push_back(std::format("{}{}", ordinal, *path));
    }
    outputs.push_back(std::move(columns));
  }
  // The widest is the candidate container; every other output must be found inside it.
  std::size_t widest = 0;
  for (std::size_t i = 1; i < outputs.size(); ++i) {
    if (outputs[i].size() > outputs[widest].size()) {
      widest = i;
    }
  }
  std::vector<std::vector<int>> columns;
  columns.reserve(members.size());
  for (const std::vector<std::string>& output : outputs) {
    std::vector<int> positions;
    positions.reserve(output.size());
    for (const std::string& column : output) {
      const auto at = std::ranges::find(outputs[widest], column);
      if (at == outputs[widest].end()) {
        return std::nullopt;  // not nested, so not one result's to serve
      }
      positions.push_back(static_cast<int>(at - outputs[widest].begin()));
    }
    columns.push_back(std::move(positions));
  }
  const int64_t fields = FieldUnion(members);  // before the move, which empties `members`
  return SharedSubPlan{.members = std::move(members),
                       .widest = widest,
                       .columns = std::move(columns),
                       .fields = fields};
}

}  // namespace

SharedConsumerFilters::SharedConsumerFilters(const LogicalPlan& plan) {
  const auto visit = [&](const auto& self, const LogicalNodePtr& node,
                         const FilterNode* parent) -> void {
    if (node == nullptr) {
      return;
    }
    const auto [at, first] = parents_.emplace(node.get(), parent);
    if (!first) {
      at->second = nullptr;
      return;
    }
    for (const auto& input : InputsOf(*node)) {
      self(self, input, std::get_if<FilterNode>(node.get()));
    }
  };
  visit(visit, plan.root, nullptr);
}

LogicalNodePtr SharedConsumerFilters::Producer(const SharedSubPlan& group) const {
  if (group.combined_producer) {
    return group.combined_producer;
  }
  const auto& widest = group.members[group.widest];
  // Intersect the constrained output positions across readers, unioning their allowed values.
  // Map by origin-derived producer position: every reference has its own ids and projection.
  using Domains = std::map<int, std::set<Int128>>;
  Domains common;
  std::map<int, BoundColumn> producer_columns;
  for (std::size_t i = 0; i < group.members.size(); ++i) {
    const auto parent = parents_.find(group.members[i].get());
    if (parent == parents_.end() || parent->second == nullptr) {
      return widest;
    }
    const auto ids = OutputIds(*group.members[i]);
    Domains local;
    for (const Predicate& p : parent->second->predicates) {
      if (!p.column.has_value()) {
        continue;
      }
      const auto& column = *p.column;
      if ((!IsInteger(column.type) && column.type != LogicalType::kDate) ||
          column.id == kNoColumnId) {
        continue;
      }
      const auto at = std::ranges::find(ids, column.id);
      if (at == ids.end()) {
        continue;
      }
      const int position = group.columns[i][static_cast<std::size_t>(at - ids.begin())];
      if (i == group.widest) {
        producer_columns.try_emplace(position, column);
      }
      if (IsIntegerOrDateEquality(p)) {
        local[position] = {std::get<Int128>(p.constant.value)};
      } else if (p.kind == Predicate::Kind::kIn && !p.values.empty() &&
                 p.values.size() <= kMaxSharedFilterValues &&
                 std::ranges::all_of(p.values, [&](const Constant& v) {
                   return v.type == column.type && std::holds_alternative<Int128>(v.value);
                 })) {
        auto& values = local[position];
        // All predicates of this reader are conjuncts, so its shortest list suffices.
        if (values.empty() || p.values.size() < values.size()) {
          values.clear();
          for (const auto& v : p.values) {
            values.insert(std::get<Int128>(v.value));
          }
        }
      }
    }
    if (i == 0) {
      common = std::move(local);
    } else {
      std::erase_if(common, [&](const auto& entry) { return !local.contains(entry.first); });
      for (auto& [pos, values] : common) {
        values.merge(local.at(pos));
      }
      std::erase_if(common,
                    [](const auto& entry) { return entry.second.size() > kMaxSharedFilterValues; });
    }
    if (common.empty()) {
      return widest;
    }
  }
  for (const auto& [pos, values] : common) {
    const auto& column = producer_columns.at(pos);
    Predicate combined{.kind = Predicate::Kind::kIn, .column = column};
    for (const Int128 v : values) {
      combined.values.push_back(Constant{.type = column.type, .value = v});
    }
    auto filtered = PushGroupKey(widest, combined);
    if (filtered != widest) {
      return filtered;
    }
  }
  return widest;
}

bool WorthSharing(const SharedSubPlan& group, int& remaining) {
  if (group.members.size() < 2 || group.widest >= group.members.size() ||
      group.columns.size() != group.members.size()) {
    return false;
  }
  const LogicalNodePtr& widest = group.members[group.widest];
  const auto size = SharedAggregateSize(widest);
  return size.has_value() && InputRows(*widest).value_or(0) >= kMinSharedRows &&
         FitsSharedResult(*size, std::ssize(group.columns[group.widest]), remaining);
}

bool WorthSharing(const SharedSubPlan& group) {
  int remaining = kSharingEstimateBudget;
  return WorthSharing(group, remaining);
}

namespace {

// A DISTINCT set whose entire key is constrained by an inner join can match each scan row at
// most once. Bound the resulting dimension keys by the dimension's own row count.
std::optional<int64_t> SetJoinRows(const JoinNode& join, const LogicalNode& set_input,
                                   const LogicalNode& scan_input, bool set_on_left) {
  const auto* set = std::get_if<SetOperationNode>(&set_input);
  const auto* scan = std::get_if<ScanNode>(&scan_input);
  if (set == nullptr || scan == nullptr || scan->table == nullptr || set->formula.empty() ||
      set->formula.back().kind != SetTermKind::kIntersect || set->ids.size() != join.keys.size()) {
    return std::nullopt;
  }
  const bool covers_key = std::ranges::all_of(set->ids, [&](ColumnId id) {
    return std::ranges::any_of(join.keys, [&](const JoinKey& key) {
      return (set_on_left ? key.left.id : key.right.id) == id;
    });
  });
  return covers_key ? scan->table->exact_row_count() : std::nullopt;
}

std::optional<int64_t> CompoundRows(const LogicalNode& node) {
  if (const auto* project = std::get_if<ProjectNode>(&node)) {
    return CompoundRows(*project->input);
  }
  if (const auto* join = std::get_if<JoinNode>(&node);
      join != nullptr && join->kind == JoinKind::kInner) {
    auto rows = SetJoinRows(*join, *join->left, *join->right, /*set_on_left=*/true);
    return rows.has_value() ? rows
                            : SetJoinRows(*join, *join->right, *join->left, /*set_on_left=*/false);
  }
  if (std::holds_alternative<UnionAllNode>(node) ||
      std::holds_alternative<SetOperationNode>(node)) {
    return EstimateRows(node);
  }
  return std::nullopt;
}

std::span<const BoundColumn> CompoundOutput(const LogicalNode& node) {
  if (const auto* project = std::get_if<ProjectNode>(&node)) {
    return project->columns;
  }
  if (const auto* union_all = std::get_if<UnionAllNode>(&node);
      union_all != nullptr && !union_all->columns.empty()) {
    return union_all->columns.front();
  }
  if (const auto* set = std::get_if<SetOperationNode>(&node);
      set != nullptr && !set->columns.empty()) {
    return set->columns.front();
  }
  return {};
}

bool WorthSharingCompound(const LogicalNode& node) {
  const auto columns = CompoundOutput(node);
  if (columns.empty()) {
    return false;
  }
  // InputRows also checks that each scan has a table before CompoundRows asks for its metadata.
  const int64_t input_rows = InputRows(node).value_or(0);
  if (input_rows < kMinSharedRows) {
    return false;
  }
  const auto rows = CompoundRows(node);
  if (!rows.has_value() || *rows >= input_rows) {
    return false;
  }
  int64_t width = 0;
  for (const BoundColumn& column : columns) {
    const int64_t bytes = FixedBytes(column.type);
    if (bytes == 0) {
      return false;
    }
    width += bytes + 1;  // room for validity even in one-row batches
  }
  return *rows <= kMaxSharedResultBytes / width;
}

void Cover(const LogicalNodePtr& node, std::unordered_set<const LogicalNode*>& covered) {
  if (node != nullptr && covered.insert(node.get()).second) {
    for (const LogicalNodePtr& input : InputsOf(*node)) {
      Cover(input, covered);
    }
  }
}

bool Overlaps(const LogicalNodePtr& node, const std::unordered_set<const LogicalNode*>& covered) {
  return node != nullptr && (covered.contains(node.get()) ||
                             std::ranges::any_of(InputsOf(*node), [&](const auto& input) {
                               return Overlaps(input, covered);
                             }));
}

struct GroupPartition {
  LogicalNodePtr member;
  const Predicate* equality;
  std::size_t key;
  std::bitset<kMaxPredicateDomainKeys> fixed_keys;
  bool prunes_only_scan = false;
};

bool ReadsSlot(const BoundColumn& column, std::span<const ColumnId> ids) {
  const auto slot = TryNarrow<std::size_t>(column.index);
  return slot && *slot < ids.size() && column.id != kNoColumnId && ids[*slot] == column.id;
}

bool PrunesScanParts(const FilterNode& filter) {
  const auto* input = filter.input.get();
  while (const auto* next = std::get_if<FilterNode>(input)) {
    input = next->input.get();
  }
  return std::get_if<ScanNode>(input) != nullptr;
}

// A retained input key preserves its equality through row-wise nodes and inner joins. An
// aggregate, a compound input, or a null-extending join would need a different proof.
std::vector<GroupPartition> PartitionsOf(const LogicalNodePtr& member,
                                         const GroupAggregateNode& group) {
  std::vector<GroupPartition> partitions;
  std::bitset<kMaxPredicateDomainKeys> fixed_keys;
  bool joined = false;
  const auto visit = [&](const auto& self, const LogicalNodePtr& node) -> bool {
    if (const auto* filter = std::get_if<FilterNode>(node.get())) {
      const auto ids = OutputIds(*filter->input);
      for (const auto& p : filter->predicates) {
        if (!IsIntegerOrDateEquality(p) || !ReadsSlot(*p.column, ids)) {
          continue;
        }
        const auto key = std::ranges::find(group.keys, p.column->id, &BoundColumn::id);
        if (key != group.keys.end() && key->type == p.column->type) {
          // Bound alternative fingerprints per group. One filter reached twice is two
          // occurrences in the fingerprint, so changing just one would not reproduce it.
          if (partitions.size() == kMaxPredicateDomainKeys ||
              std::ranges::contains(partitions, &p, &GroupPartition::equality)) {
            return false;
          }
          const auto index = Narrow<std::size_t>(key - group.keys.begin());
          fixed_keys.set(index);
          partitions.push_back({.member = member,
                                .equality = &p,
                                .key = index,
                                .fixed_keys = {},
                                .prunes_only_scan = PrunesScanParts(*filter)});
        }
      }
    } else if (const auto* join = std::get_if<JoinNode>(node.get())) {
      // Several joins can offer different dynamic filters on the same scan after widening.
      if (joined || join->kind != JoinKind::kInner || join->verify_dimension_business_field) {
        return false;
      }
      joined = true;
    } else if (std::get_if<ComputeNode>(node.get()) == nullptr &&
               std::get_if<ScanNode>(node.get()) == nullptr) {
      return false;
    }
    return std::ranges::all_of(InputsOf(*node),
                               [&](const auto& input) { return self(self, input); });
  };
  const auto input_ids = OutputIds(*group.input);
  if (!std::ranges::all_of(group.keys,
                           [&](const auto& key) { return ReadsSlot(key, input_ids); }) ||
      !visit(visit, group.input)) {
    return {};
  }
  for (auto& partition : partitions) {
    partition.fixed_keys = fixed_keys;
    partition.prunes_only_scan = partition.prunes_only_scan && !joined;
  }
  return partitions;
}

using PartitionReaders = std::map<Int128, std::size_t>;

// A direct scan already skips parts whose bounds exclude its equality. Sharing can save
// a part read only if two readers may visit the same part, including two with the same value.
bool HasSharedPart(const Table& table, int field, const PartitionReaders& readers) {
  for (int64_t part = 0; part < table.num_parts(); ++part) {
    const auto stats = table.part_stats(part, field);
    if (!stats || stats->rows <= 0) {
      return true;
    }
    if (stats->null_count >= stats->rows) {
      continue;
    }
    if (!stats->min || !stats->max || *stats->min > *stats->max) {
      return true;
    }
    const auto first = readers.lower_bound(*stats->min);
    const auto end = readers.upper_bound(*stats->max);
    if (first != end && (first->second > 1 || std::next(first) != end)) {
      return true;
    }
  }
  return false;
}

// Unlike the cost estimate for an exact match, widening an input needs a bound for the whole
// producer. The varying key passes the IN list and the other equalities are the same in
// every reader. Neither admits NULL. Unconstrained bounds include NULL and cover disjoint parts.
bool WorthSharingPartitions(const GroupPartition& partition, const PartitionReaders& readers) {
  const auto& group = std::get<GroupAggregateNode>(*partition.member);
  std::vector<ColumnId> keys;
  int64_t width = 0;
  for (const auto& column : group.keys) {
    width += FixedBytes(column.type) + 1;
    keys.push_back(column.id);
  }
  for (const auto& call : group.aggregates) {
    width += FixedBytes(call.type) + 1;
  }
  std::vector<ScanKeyDomain> sources(keys.size());
  EachDistinctValues(*group.input, keys, sources);
  for (std::size_t i = 0; i < sources.size(); ++i) {
    if (partition.fixed_keys.test(i)) {
      const int64_t bound = i == partition.key ? std::ssize(readers) : 1;
      auto& source = sources[i];
      source.max_values = std::min(source.max_values.value_or(bound), bound);
      if (source.max_part_values) {
        source.max_part_values = std::min(*source.max_part_values, bound);
      }
    }
  }
  int64_t limit = kMaxSharedResultBytes / width;
  for (std::size_t i = 0; i < sources.size(); ++i) {
    const auto* scan = sources[i].scan;
    if (scan == nullptr || scan->ids.size() != scan->fields.size() || !scan->table->schema()) {
      return false;  // including a computed key: its domain isn't the raw scan's
    }
    const auto pos =
        std::ranges::find(scan->ids, keys[i]);  // EachDistinctValues found this id here
    const int field = scan->fields[Narrow<std::size_t>(pos - scan->ids.begin())];
    if (field < 0 || field >= scan->table->schema()->num_fields()) {
      return false;
    }
    const auto type = FromArrow(*scan->table->schema()->field(field)->type());
    if (!type.ok() || *type != group.keys[i].type) {
      return false;
    }
    if (i == partition.key && partition.prunes_only_scan && HasComparableStatistics(*type) &&
        !HasSharedPart(*scan->table, field, readers)) {
      return false;
    }
    if (!std::ranges::contains(std::span(sources).first(i), scan, &ScanKeyDomain::scan)) {
      limit = AfterScan(*scan, sources, limit);
    }
  }
  return limit > 0;
}

LogicalNodePtr WithPartitions(const LogicalNodePtr& node, const Predicate* equality,
                              const PartitionReaders& values, bool& widens_join_build) {
  if (const auto* filter = std::get_if<FilterNode>(node.get())) {
    for (std::size_t i = 0; i < filter->predicates.size(); ++i) {
      if (&filter->predicates[i] == equality) {
        FilterNode copy = *filter;
        Predicate combined{
            .kind = Predicate::Kind::kIn, .column = equality->column, .span = equality->span};
        for (const auto& entry : values) {
          combined.values.push_back({.type = equality->constant.type, .value = entry.first});
        }
        copy.predicates[i] = std::move(combined);
        return std::make_shared<LogicalNode>(std::move(copy));
      }
    }
  }
  auto inputs = InputsOf(*node);
  for (auto& input : inputs) {
    auto next = WithPartitions(input, equality, values, widens_join_build);
    if (next != input) {
      if (const auto* join = std::get_if<JoinNode>(node.get())) {
        widens_join_build = input == (join->build == BuildSide::kLeft ? join->left : join->right);
      }
      input = std::move(next);
      return WithInputs(*node, std::move(inputs));
    }
  }
  return node;
}

class FilteredGroups {
 public:
  void Collect(const LogicalNodePtr& node, const Origins& origins) {
    const auto* group = std::get_if<GroupAggregateNode>(node.get());
    if (group == nullptr || group->keys.empty() || group->keys.size() > kMaxPredicateDomainKeys ||
        group->key_ids.size() != group->keys.size() || candidates_.contains(node.get()) ||
        !std::ranges::all_of(group->keys,
                             [](const auto& key) {
                               return key.id != kNoColumnId && FixedBytes(key.type) > 0 &&
                                      key.type != LogicalType::kDouble;
                             }) ||
        !std::ranges::all_of(group->aggregates, [](const auto& call) {
          return FixedBytes(call.type) > 0 && call.type != LogicalType::kDouble &&
                 (call.kind == AggKind::kCountStar || call.kind == AggKind::kCount ||
                  call.kind == AggKind::kSum || call.kind == AggKind::kMin ||
                  call.kind == AggKind::kMax);
        })) {
      return;
    }
    for (const auto& partition : PartitionsOf(node, *group)) {
      // Elide exactly one literal, preserving its type, column origin and path, predicate order,
      // every other filter, and every expression in the input and aggregate.
      KeyMemo memo;
      const auto& key = SubtreeKey(node, origins, memo, partition.equality);
      if (key.has_value()) {
        auto& bucket = by_key_[*key];
        bucket.push_back(partition);
        candidates_[node.get()].push_back(&bucket);
      }
    }
  }

  std::optional<SharedSubPlan> Select(const LogicalNodePtr& node, const Origins& origins,
                                      const std::unordered_set<const LogicalNode*>& covered) {
    const auto found = candidates_.find(node.get());
    if (found == candidates_.end()) {
      return std::nullopt;
    }
    for (auto* bucket : found->second) {
      auto partitions = std::exchange(*bucket, {});  // try each candidate only once
      if (!covered.empty()) {
        std::erase_if(partitions, [&](const auto& part) { return Overlaps(part.member, covered); });
      }
      if (partitions.size() < 2) {
        continue;
      }
      PartitionReaders values;
      std::vector<LogicalNodePtr> members;
      for (const auto& part : partitions) {
        ++values[std::get<Int128>(part.equality->constant.value)];
        if (values.size() > kMaxSharedFilterValues) {
          break;
        }
        members.push_back(part.member);
      }
      if (values.size() < 2 || values.size() > kMaxSharedFilterValues ||
          InputRows(*members.front()).value_or(0) < kMinSharedRows) {
        continue;
      }
      auto group = GroupOf(std::move(members), origins);
      if (!group) {
        continue;
      }
      const auto& widest = partitions[group->widest];
      if (!WorthSharingPartitions(widest, values)) {
        continue;
      }
      group->combined_producer =
          WithPartitions(widest.member, widest.equality, values, group->widens_join_build);
      for (const auto& part : partitions) {
        const auto& aggregate = std::get<GroupAggregateNode>(*part.member);
        Predicate reader = *part.equality;
        reader.column = aggregate.keys[part.key];
        reader.column->id = aggregate.key_ids[part.key];
        reader.column->index = Narrow<int>(part.key);
        group->reader_predicates.push_back(std::move(reader));
      }
      return group;
    }
    return std::nullopt;
  }

 private:
  using Bucket = std::vector<GroupPartition>;
  // Bucket addresses survive rehashing, and a member need not store another copy of the
  // whole subtree fingerprint for each possible partition.
  std::unordered_map<std::string, Bucket> by_key_;
  std::unordered_map<const LogicalNode*, std::vector<Bucket*>> candidates_;
};

std::vector<SharedSubPlan> FindSharedSubPlans(const LogicalPlan& plan, bool executable,
                                              bool share_filtered_groups = false) {
  if (plan.root == nullptr) {
    return {};
  }
  Origins origins;
  origins.Walk(plan.root);
  KeyMemo memo;
  FilteredGroups filtered;
  std::unordered_set<const LogicalNode*> covered;

  // Every candidate sub-tree by key, in the order a walk from the root meets it. A node that
  // appears twice as the same pointer is already shared and is counted once.
  std::unordered_map<std::string, std::vector<LogicalNodePtr>> by_key;
  const auto collect = [&](const auto& self, const LogicalNodePtr& node,
                           bool discover_partitions) -> void {
    if (node == nullptr) {
      return;
    }
    if (const auto* limit = std::get_if<LimitNode>(node.get());
        executable && limit != nullptr && limit->limit.has_value() &&
        (*limit->limit == 0 || std::get_if<SortNode>(limit->input.get()) == nullptr)) {
      // A streaming LIMIT can omit whole UNION ALL branches or stop between their batches.
      // Preparing a producer here would open scans the limit may skip. Replacing it with a
      // scan could also make a consumer evaluate a whole part before yielding its first batch.
      // A positive LIMIT on a Sort consumes its input for top-N; OFFSET alone drains its input.
      Cover(node, covered);
      return;
    }
    if (const std::optional<std::string>& key = SubtreeKey(node, origins, memo); key.has_value()) {
      if (discover_partitions) {
        filtered.Collect(node, origins);
      }
      std::vector<LogicalNodePtr>& members = by_key[*key];
      if (std::ranges::none_of(
              members, [&node](const LogicalNodePtr& m) { return m.get() == node.get(); })) {
        members.push_back(node);
      }
    }
    // Keep the structural report's compound boundary. Physical planning can share an eligible
    // branch now that a shared build can fall back to recomputation (ADR 0037, PF5).
    if (!executable && (std::holds_alternative<UnionAllNode>(*node) ||
                        std::holds_alternative<SetOperationNode>(*node))) {
      return;
    }
    // The first grouped node in each deferred dimension branch is its verified fact partial.
    // Deeper groups can feed an order-sensitive computation even when the final sort is complete.
    const bool descend = discover_partitions && !std::holds_alternative<GroupAggregateNode>(*node);
    for (const LogicalNodePtr& input : InputsOf(*node)) {
      self(self, input, descend);
    }
  };
  collect(collect, plan.root, share_filtered_groups);

  // Maximal groups only: where a sub-tree repeats so does everything inside it, and the outermost
  // is the one a later item would materialise. A pre-order walk that stops descending at a member
  // reports exactly those, in the order the plan meets them.
  std::vector<SharedSubPlan> out;
  std::set<std::string> reported;
  int remaining = kSharingEstimateBudget;
  const std::function<void(const LogicalNodePtr&)> emit = [&](const LogicalNodePtr& node) {
    if (node == nullptr || covered.contains(node.get())) {
      return;
    }
    if (share_filtered_groups) {
      if (auto group = filtered.Select(node, origins, covered)) {
        for (const auto& member : group->members) {
          Cover(member, covered);
        }
        out.push_back(std::move(*group));
        return;
      }
    }
    const std::optional<std::string>& key = SubtreeKey(node, origins, memo);
    if (key.has_value() && by_key[*key].size() > 1) {
      if (reported.insert(*key).second) {
        auto members = by_key[*key];
        if (executable && !covered.empty()) {
          std::erase_if(members, [&](const auto& member) { return Overlaps(member, covered); });
        }
        auto group = GroupOf(std::move(members), origins);
        if (group.has_value() && (!executable || WorthSharing(*group, remaining) ||
                                  WorthSharingCompound(*group->members[group->widest]))) {
          if (executable) {
            for (const LogicalNodePtr& member : group->members) {
              Cover(member, covered);
            }
          }
          out.push_back(std::move(*group));
        }
      }
      // This occurrence might contain a previously selected member. Other occurrences of the
      // same key can still form a group, while its own uncovered children remain candidates.
      if (!executable || covered.contains(node.get())) {
        return;
      }
    }
    // Match the structural report's discovery boundary.
    if (!executable && (std::holds_alternative<UnionAllNode>(*node) ||
                        std::holds_alternative<SetOperationNode>(*node))) {
      return;
    }
    for (const LogicalNodePtr& input : InputsOf(*node)) {
      emit(input);
    }
  };
  emit(plan.root);
  return out;
}

}  // namespace

std::vector<SharedSubPlan> SharedSubPlans(const LogicalPlan& plan) {
  return FindSharedSubPlans(plan, /*executable=*/false);
}

std::vector<SharedSubPlan> MaterializableSubPlans(const LogicalPlan& plan,
                                                  bool share_filtered_groups) {
  return FindSharedSubPlans(plan, /*executable=*/true, share_filtered_groups);
}

}  // namespace antb1::plan
