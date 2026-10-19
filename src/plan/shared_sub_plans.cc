#include "shared_sub_plans.h"

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

// A column's plan-wide ordinal: two columns get the same one exactly when they come from the same
// place. This is the whole trick of the pass. A ColumnId is minted per reference, so two references
// to one CTE never share one, and BoundColumn::index is a position, which a scan reading one field
// fewer shifts in every node above it -- so neither can match the two sides. Where a column *comes
// from* is the same on both sides, and that is what this interns.
class Origins {
 public:
  static constexpr int kUnknown = -1;

  // Defines an ordinal for every column the plan's nodes mint, inputs first.
  void Walk(const LogicalNodePtr& node) {
    if (node == nullptr) {
      return;
    }
    for (const LogicalNodePtr& input : InputsOf(*node)) {
      Walk(input);
    }
    std::visit([this](const auto& n) { Define(n); }, *node);
  }

  [[nodiscard]] int Of(ColumnId id) const {
    const auto it = of_.find(static_cast<std::uint32_t>(id));
    return it == of_.end() ? kUnknown : it->second;
  }

  // A column reference as a key: its ordinal, or its position when it has no id (the executor's
  // positional plans, which never reach here from the binder).
  [[nodiscard]] std::string Key(const BoundColumn& column) const {
    if (column.id == kNoColumnId) {
      return std::format("@{}", column.index);
    }
    return std::format("c{}", Of(column.id));
  }
  [[nodiscard]] std::string Key(const std::optional<BoundColumn>& column) const {
    return column.has_value() ? Key(*column) : "-";
  }

 private:
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
  // through, and ids survive a change of position (logical_plan.h).
  void Define(const ScanNode& node) {
    for (std::size_t i = 0; i < node.ids.size() && i < node.fields.size(); ++i) {
      Set(node.ids[i], std::format("{}#{}", node.table_name, node.fields[i]));
    }
  }
  void Define(const ComputeNode& node);
  void Define(const ProjectNode& node) {
    for (std::size_t i = 0; i < node.ids.size(); ++i) {
      const std::optional<Constant>* constant =
          i < node.constants.size() ? &node.constants[i] : nullptr;
      if (constant != nullptr && constant->has_value()) {
        Set(node.ids[i], std::format("k{}", ToString(**constant)));
      } else if (i < node.columns.size()) {
        Set(node.ids[i], std::format("={}", Key(node.columns[i])));
      }
    }
  }
  void Define(const AggregateNode& node) {
    for (const AggregateCall& call : node.aggregates) {
      Set(call.id, CallOrigin(call));
    }
  }
  void Define(const GroupAggregateNode& node) {
    for (std::size_t i = 0; i < node.key_ids.size() && i < node.keys.size(); ++i) {
      Set(node.key_ids[i], std::format("key({})", Key(node.keys[i])));
    }
    for (const AggregateCall& call : node.aggregates) {
      Set(call.id, CallOrigin(call));
    }
  }
  void Define(const RowCountNode& node) {
    Set(node.id, std::format("rowcount({})", node.table_name));
  }
  void Define(const DependentJoinNode& node) { Set(node.mark, "mark"); }
  void Define(const FilterNode& /*node*/) {}
  void Define(const SortNode& /*node*/) {}
  void Define(const LimitNode& /*node*/) {}
  void Define(const JoinNode& /*node*/) {}

  [[nodiscard]] std::string CallOrigin(const AggregateCall& call) const {
    return std::format("agg({},{},{})", static_cast<int>(call.kind), Key(call.arg),
                       static_cast<int>(call.type.id()));
  }

  std::unordered_map<std::uint32_t, int> of_;
  std::unordered_map<std::string, int> interned_;
};

// `p` as a key. With `local`, its columns are the operand-local ones of a PredicateExpr (index 0
// and 1 into the operands) rather than columns of a node's input, so they are keyed by position.
std::string PredicateKey(const Predicate& p, const Origins& origins, bool local) {
  const auto column = [&](const std::optional<BoundColumn>& c) {
    if (!c.has_value()) {
      return std::string("-");
    }
    return local ? std::format("@{}", c->index) : origins.Key(*c);
  };
  std::string key = std::format("P{},{},{},{},{}", static_cast<int>(p.kind), column(p.column),
                                column(p.other), static_cast<int>(p.op), ToString(p.constant));
  for (const Constant& value : p.values) {
    key += ',';
    key += ToString(value);
  }
  return key;
}

// An expression as a key: its structure and type with every column reference by origin, and
// without its display name (which SameExpr ignores too, for the same reason).
std::string ExprKey(const ExprPtr& expr, const Origins& origins) {
  if (expr == nullptr) {
    return "_";
  }
  const auto args = [&origins](const std::vector<ExprPtr>& list) {
    std::string out;
    for (const ExprPtr& arg : list) {
      out += ExprKey(arg, origins);
      out += ',';
    }
    return out;
  };
  const std::string node = std::visit(
      [&](const auto& e) -> std::string {
        using T = std::decay_t<decltype(e)>;
        if constexpr (std::is_same_v<T, ColumnExpr>) {
          return e.id == kNoColumnId ? std::format("@{}", e.index)
                                     : std::format("c{}", origins.Of(e.id));
        } else if constexpr (std::is_same_v<T, ConstantExpr>) {
          return std::format("k{}", ToString(e.value));
        } else if constexpr (std::is_same_v<T, ArithExpr>) {
          return std::format("A{}({},{})", static_cast<int>(e.op), ExprKey(e.left, origins),
                             ExprKey(e.right, origins));
        } else if constexpr (std::is_same_v<T, NegateExpr>) {
          return std::format("N({})", ExprKey(e.operand, origins));
        } else if constexpr (std::is_same_v<T, FunctionExpr>) {
          return std::format("F{}({})", static_cast<int>(e.function), args(e.args));
        } else if constexpr (std::is_same_v<T, PredicateExpr>) {
          return std::format("E{}({})", PredicateKey(e.predicate, origins, /*local=*/true),
                             args(e.operands));
        } else if constexpr (std::is_same_v<T, BoolExpr>) {
          return std::format("B{}({})", static_cast<int>(e.op), args(e.args));
        } else if constexpr (std::is_same_v<T, CaseExpr>) {
          return std::format("W({}|{}|{})", args(e.whens), args(e.thens),
                             ExprKey(e.otherwise, origins));
        } else if constexpr (std::is_same_v<T, CastExpr>) {
          return std::format("X({})", ExprKey(e.operand, origins));
        } else {
          static_assert(false, "an expression type without a key");
        }
      },
      expr->node);
  return std::format("{}:{}", node, static_cast<int>(expr->type.id()));
}

void Origins::Define(const ComputeNode& node) {
  for (std::size_t i = 0; i < node.ids.size() && i < node.exprs.size(); ++i) {
    Set(node.ids[i], std::format("e{}", ExprKey(node.exprs[i], *this)));
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
      [&origins](const auto& n) -> std::optional<std::string> {
        using T = std::decay_t<decltype(n)>;
        if constexpr (std::is_same_v<T, ScanNode>) {
          return std::format("scan({})", n.table_name);
        } else if constexpr (std::is_same_v<T, FilterNode>) {
          std::string key = "filter(";
          for (const Predicate& p : n.predicates) {
            key += PredicateKey(p, origins, /*local=*/false);
            key += ';';
          }
          return key + ')';
        } else if constexpr (std::is_same_v<T, ComputeNode>) {
          std::string key = "compute(";
          for (const ExprPtr& expr : n.exprs) {
            key += ExprKey(expr, origins);
            key += ';';
          }
          return key + ')';
        } else if constexpr (std::is_same_v<T, ProjectNode>) {
          std::string key = "project(";
          for (std::size_t i = 0; i < n.columns.size(); ++i) {
            if (i < n.constants.size() && n.constants[i].has_value()) {
              key += std::format("k{};", ToString(*n.constants[i]));
            } else {
              key += std::format("{};", origins.Key(n.columns[i]));
            }
          }
          return key + ')';
        } else if constexpr (std::is_same_v<T, AggregateNode>) {
          std::string key = "aggregate(";
          for (const AggregateCall& call : n.aggregates) {
            key += std::format("{},{},{};", static_cast<int>(call.kind), origins.Key(call.arg),
                               static_cast<int>(call.type.id()));
          }
          return key + ')';
        } else if constexpr (std::is_same_v<T, GroupAggregateNode>) {
          std::string key = "group(";
          for (const BoundColumn& k : n.keys) {
            key += origins.Key(k);
            key += ',';
          }
          key += ';';
          for (const AggregateCall& call : n.aggregates) {
            key += std::format("{},{},{};", static_cast<int>(call.kind), origins.Key(call.arg),
                               static_cast<int>(call.type.id()));
          }
          return key + ')';
        } else if constexpr (std::is_same_v<T, JoinNode>) {
          std::string key = std::format("join({},", static_cast<int>(n.kind));
          for (const JoinKey& k : n.keys) {
            key += std::format("{}={},", origins.Key(k.left), origins.Key(k.right));
          }
          key += ';';
          for (const ExprPtr& expr : n.residual) {
            key += ExprKey(expr, origins);
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
        std::vector<LogicalNodePtr> members = by_key[*key];
        const int64_t fields = FieldUnion(members);  // before the move, which empties `members`
        out.push_back(SharedSubPlan{.members = std::move(members), .fields = fields});
      }
      return;  // no descending into a member: its children repeat too, and only this is maximal
    }
    for (const LogicalNodePtr& input : InputsOf(*node)) {
      emit(input);
    }
  };
  emit(plan.root);
  return out;
}

}  // namespace antb1::plan
