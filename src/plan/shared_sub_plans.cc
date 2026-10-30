#include "antb1/plan/shared_sub_plans.h"

#include <algorithm>
#include <cstdint>
#include <format>
#include <functional>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#include "antb1/plan/logical_plan.h"
#include "antb1/plan/types.h"

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
  [[nodiscard]] std::string Key(const BoundColumn& column, const LogicalNode& at) const {
    if (column.id == kNoColumnId) {
      return std::format("@{}", column.index);
    }
    return std::format("c{}{}", Of(column.id), PathOf(column.id, at));
  }
  [[nodiscard]] std::string Key(const std::optional<BoundColumn>& column,
                                const LogicalNode& at) const {
    return column.has_value() ? Key(*column, at) : "-";
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
  void Define(const FilterNode& /*node*/, const LogicalNode& /*at*/) {}
  void Define(const SortNode& /*node*/, const LogicalNode& /*at*/) {}
  void Define(const LimitNode& /*node*/, const LogicalNode& /*at*/) {}
  void Define(const JoinNode& /*node*/, const LogicalNode& /*at*/) {}

  // A call's origin: what it computes over which argument. COUNT(*) has none, so every COUNT(*) of
  // the plan has one origin whatever it counts -- and a read of one is told from a read of another
  // by its path, as two sides' columns of one table are.
  [[nodiscard]] std::string CallOrigin(const AggregateCall& call, const LogicalNode& at) const {
    return std::format("agg({},{},{})", static_cast<int>(call.kind), Key(call.arg, at),
                       static_cast<int>(call.type.id()));
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
  return std::format("{}:{}", static_cast<int>(constant.type.id()), ToString(constant));
}

// `p`, read by `at`, as a key. With `local`, its columns are the operand-local ones of a
// PredicateExpr (index 0 and 1 into the operands) rather than columns of a node's input, so they
// are keyed by position.
std::string PredicateKey(const Predicate& p, const Origins& origins, bool local,
                         const LogicalNode& at) {
  const auto column = [&](const std::optional<BoundColumn>& c) {
    if (!c.has_value()) {
      return std::string("-");
    }
    return local ? std::format("@{}", c->index) : origins.Key(*c, at);
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
std::string ExprKey(const ExprPtr& expr, const Origins& origins, const LogicalNode& at) {
  if (expr == nullptr) {
    return "_";
  }
  const auto args = [&origins, &at](const std::vector<ExprPtr>& list) {
    std::string out;
    for (const ExprPtr& arg : list) {
      out += ExprKey(arg, origins, at);
      out += ',';
    }
    return out;
  };
  const std::string node = std::visit(
      [&](const auto& e) -> std::string {
        using T = std::decay_t<decltype(e)>;
        if constexpr (std::is_same_v<T, ColumnExpr>) {
          return e.id == kNoColumnId
                     ? std::format("@{}", e.index)
                     : std::format("c{}{}", origins.Of(e.id), origins.PathOf(e.id, at));
        } else if constexpr (std::is_same_v<T, ConstantExpr>) {
          return std::format("k{}", ConstantKey(e.value));
        } else if constexpr (std::is_same_v<T, ArithExpr>) {
          return std::format("A{}({},{})", static_cast<int>(e.op), ExprKey(e.left, origins, at),
                             ExprKey(e.right, origins, at));
        } else if constexpr (std::is_same_v<T, NegateExpr>) {
          return std::format("N({})", ExprKey(e.operand, origins, at));
        } else if constexpr (std::is_same_v<T, FunctionExpr>) {
          return std::format("F{}({})", static_cast<int>(e.function), args(e.args));
        } else if constexpr (std::is_same_v<T, PredicateExpr>) {
          return std::format("E{}({})", PredicateKey(e.predicate, origins, /*local=*/true, at),
                             args(e.operands));
        } else if constexpr (std::is_same_v<T, BoolExpr>) {
          return std::format("B{}({})", static_cast<int>(e.op), args(e.args));
        } else if constexpr (std::is_same_v<T, CaseExpr>) {
          return std::format("W({}|{}|{})", args(e.whens), args(e.thens),
                             ExprKey(e.otherwise, origins, at));
        } else if constexpr (std::is_same_v<T, CastExpr>) {
          return std::format("X({})", ExprKey(e.operand, origins, at));
        } else {
          static_assert(false, "an expression type without a key");
        }
      },
      expr->node);
  return std::format("{}:{}", node, static_cast<int>(expr->type.id()));
}

void Origins::Define(const ComputeNode& node, const LogicalNode& at) {
  for (std::size_t i = 0; i < node.ids.size() && i < node.exprs.size(); ++i) {
    Set(node.ids[i], std::format("e{}", ExprKey(node.exprs[i], *this, at)));
  }
}

// One node's own content as a key, or none when a sub-plan holding it is not shareable.
//
// A Scan's fields are deliberately absent: selecting fields does not change which rows a scan
// returns, so two scans of one table differing only there compute the same rows and the shared
// sub-plan reads the union. A Join's build side and estimate are absent too -- both are choices
// about *how* to run it, and neither changes the rows or their order.
std::optional<std::string> NodeKey(const LogicalNode& node, const Origins& origins) {
  return std::visit(
      [&origins, &node](const auto& n) -> std::optional<std::string> {
        using T = std::decay_t<decltype(n)>;
        if constexpr (std::is_same_v<T, ScanNode>) {
          return std::format("scan({})", n.table_name);
        } else if constexpr (std::is_same_v<T, FilterNode>) {
          std::string key = "filter(";
          for (const Predicate& p : n.predicates) {
            key += PredicateKey(p, origins, /*local=*/false, node);
            key += ';';
          }
          return key + ')';
        } else if constexpr (std::is_same_v<T, ComputeNode>) {
          std::string key = "compute(";
          for (const ExprPtr& expr : n.exprs) {
            key += ExprKey(expr, origins, node);
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
            key += std::format("{},{},{};", static_cast<int>(call.kind),
                               origins.Key(call.arg, node), static_cast<int>(call.type.id()));
          }
          return key + ')';
        } else if constexpr (std::is_same_v<T, GroupAggregateNode>) {
          std::string key = "group(";
          for (const BoundColumn& k : n.keys) {
            key += origins.Key(k, node);
            key += ',';
          }
          key += ';';
          for (const AggregateCall& call : n.aggregates) {
            key += std::format("{},{},{};", static_cast<int>(call.kind),
                               origins.Key(call.arg, node), static_cast<int>(call.type.id()));
          }
          return key + ')';
        } else if constexpr (std::is_same_v<T, JoinNode>) {
          std::string key = std::format("join({},", static_cast<int>(n.kind));
          for (const JoinKey& k : n.keys) {
            key += std::format("{}={},", origins.Key(k.left, node), origins.Key(k.right, node));
          }
          key += ';';
          for (const ExprPtr& expr : n.residual) {
            key += ExprKey(expr, origins, node);
            key += ';';
          }
          return key + ')';
        } else if constexpr (std::is_same_v<T, UnionAllNode>) {
          // Keyed by its inputs in order, which SubtreeKey appends: a union's rows come in its
          // inputs' order, so two unions of the same inputs in two orders are not one sub-plan.
          // Keyed so that a union repeated whole -- a CTE referenced twice -- is reported as one
          // group, which exec declines to hold (WorthSharing); nothing below a union is collected
          // at all (SharedSubPlans, ADR 0036).
          std::string key = "union(";
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
                             std::is_same_v<T, DependentJoinNode>) {
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
                                             KeyMemo& memo) {
  if (node == nullptr) {
    // Keyed by nullptr rather than returning a reference to a static: a missing input is not
    // shareable, which is what an empty key says.
    return memo.try_emplace(nullptr, std::nullopt).first->second;
  }
  const auto found = memo.find(node.get());
  if (found != memo.end()) {
    return found->second;
  }
  std::optional<std::string> key = NodeKey(*node, origins);
  if (key.has_value()) {
    for (const LogicalNodePtr& input : InputsOf(*node)) {
      const std::optional<std::string>& child = SubtreeKey(input, origins, memo);
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

// The group of `members`, or none when their outputs do not nest -- when no member's output columns
// contain every other's, so that one result could not serve them all. Overlapping-but-not-nested
// outputs are possible in principle (two scans reading {0,1} and {1,2}) and are reported as nothing
// rather than as a group a consumer could not read.
std::optional<SharedSubPlan> GroupOf(std::vector<LogicalNodePtr> members, const Origins& origins) {
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

std::vector<SharedSubPlan> SharedSubPlans(const LogicalPlan& plan) {
  if (plan.root == nullptr) {
    return {};
  }
  Origins origins;
  origins.Walk(plan.root);
  KeyMemo memo;

  // Every candidate sub-tree by key, in the order a walk from the root meets it. A node that
  // appears twice as the same pointer is already shared and is counted once.
  std::unordered_map<std::string, std::vector<LogicalNodePtr>> by_key;
  const std::function<void(const LogicalNodePtr&)> collect = [&](const LogicalNodePtr& node) {
    if (node == nullptr) {
      return;
    }
    if (const std::optional<std::string>& key = SubtreeKey(node, origins, memo); key.has_value()) {
      std::vector<LogicalNodePtr>& members = by_key[*key];
      if (std::ranges::none_of(
              members, [&node](const LogicalNodePtr& m) { return m.get() == node.get(); })) {
        members.push_back(node);
      }
    }
    // **Nothing under a union is a candidate** (ADR 0036). The union itself is: repeated whole and
    // with a key, it is one group, which exec declines to hold (WorthSharing). Below it nothing is
    // collected, so a branch is never held on its own either -- whatever the copies hold that has
    // no key (a Sort, a RowCount), and in whatever order they list their branches. An
    // aggregate-topped branch held alone can outgrow MaterializedTable::kMaxBytes, and a result
    // past the cap fails the query.
    if (std::holds_alternative<UnionAllNode>(*node)) {
      return;
    }
    for (const LogicalNodePtr& input : InputsOf(*node)) {
      collect(input);
    }
  };
  collect(plan.root);

  // Maximal groups only: where a sub-tree repeats so does everything inside it, and the outermost
  // is the one a later item would materialise. A pre-order walk that stops descending at a member
  // reports exactly those, in the order the plan meets them.
  std::vector<SharedSubPlan> out;
  std::set<std::string> reported;
  const std::function<void(const LogicalNodePtr&)> emit = [&](const LogicalNodePtr& node) {
    if (node == nullptr) {
      return;
    }
    const std::optional<std::string>& key = SubtreeKey(node, origins, memo);
    if (key.has_value() && by_key[*key].size() > 1) {
      if (reported.insert(*key).second) {
        if (std::optional<SharedSubPlan> group = GroupOf(by_key[*key], origins);
            group.has_value()) {
          out.push_back(*std::move(group));
        }
      }
      return;  // no descending into a member: its children repeat too, and only this is maximal
    }
    // Nor below a union, where no member is: a copy of a sub-plan under a union would otherwise
    // report a group whose members another group already holds, which exec would then compute and
    // hold for no consumer.
    if (std::holds_alternative<UnionAllNode>(*node)) {
      return;
    }
    for (const LogicalNodePtr& input : InputsOf(*node)) {
      emit(input);
    }
  };
  emit(plan.root);
  return out;
}

}  // namespace antb1::plan
