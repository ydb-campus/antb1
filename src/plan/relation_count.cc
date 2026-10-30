#include "relation_count.h"

#include <algorithm>
#include <cstddef>
#include <deque>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <variant>
#include <vector>

#include "antb1/plan/catalog.h"
#include "antb1/sql/ast.h"

namespace antb1::plan {
namespace {

constexpr std::size_t kSaturated = kMaxRelations + 1;

std::size_t AddSaturating(std::size_t a, std::size_t b) {
  return a > kSaturated - b ? kSaturated : a + b;
}

// The CTEs a block can see: its own list up to the one being counted (a CTE's body sees the CTEs
// before it, never itself and never a later one), over the frames of the blocks around it.
struct CteFrame {
  std::vector<const sql::CommonTableExpr*> visible;
  const CteFrame* outer = nullptr;

  [[nodiscard]] const sql::CommonTableExpr* Find(std::string_view lower_name) const {
    for (const CteFrame* frame = this; frame != nullptr; frame = frame->outer) {
      // The innermost list wins; within a list a repeated name is a syntax error, so the first
      // match is the only one.
      const auto it =
          std::ranges::find_if(frame->visible, [lower_name](const sql::CommonTableExpr* cte) {
            return AsciiLower(cte->name) == lower_name;
          });
      if (it != frame->visible.end()) {
        return *it;
      }
    }
    return nullptr;
  }
};

// Every subquery expression `expr` holds, in the order it is written, passed to `fn`; not those of
// a nested query, which is a block of its own that the caller walks in the frame it binds in. A
// subquery inside another one's IN operand belongs to this expression, so the walk descends there.
template <class Fn>
void EachSubqueryOf(const sql::Expr& expr, Fn& fn);

template <class Fn>
struct SubqueryWalk {
  Fn& fn;  // NOLINT(misc-non-private-member-variables-in-classes): a visitor, not a class

  void Of(const sql::Expr& e) const { EachSubqueryOf(e, fn); }
  void Of(const std::optional<sql::Box<sql::Expr>>& e) const {
    if (e.has_value()) {
      EachSubqueryOf(**e, fn);
    }
  }

  void operator()(const sql::ColumnRef& /*c*/) const {}
  void operator()(const sql::Literal& /*l*/) const {}
  void operator()(const sql::AggregateCall& a) const { Of(a.arg); }
  void operator()(const sql::UnaryExpr& u) const { Of(*u.operand); }
  void operator()(const sql::BinaryExpr& b) const {
    Of(*b.left);
    Of(*b.right);
  }
  void operator()(const sql::LikeExpr& l) const {
    Of(*l.operand);
    Of(*l.pattern);
  }
  void operator()(const sql::InExpr& in) const {
    Of(*in.operand);
    for (const sql::Expr& value : in.list) {
      Of(value);
    }
  }
  void operator()(const sql::BetweenExpr& b) const {
    Of(*b.operand);
    Of(*b.low);
    Of(*b.high);
  }
  void operator()(const sql::FunctionCall& f) const {
    for (const sql::Expr& arg : f.args) {
      Of(arg);
    }
  }
  void operator()(const sql::CaseExpr& c) const {
    Of(c.operand);
    for (const sql::CaseBranch& branch : c.branches) {
      Of(*branch.when);
      Of(*branch.then);
    }
    Of(c.otherwise);
  }
  void operator()(const sql::ExtractExpr& e) const { Of(*e.source); }
  void operator()(const sql::CastExpr& c) const { Of(*c.operand); }
  // The IN operand first: its text precedes the operator this node is reported at.
  void operator()(const sql::SubqueryExpr& s) const {
    Of(s.operand);
    fn(s);
  }
};

template <class Fn>
void EachSubqueryOf(const sql::Expr& expr, Fn& fn) {
  std::visit(SubqueryWalk<Fn>{.fn = fn}, static_cast<const sql::ExprNode&>(expr));
}

// Every subquery expression of `block`'s own expressions, in query order: the select items, each
// FROM item's ON, WHERE, GROUP BY, HAVING, ORDER BY -- the order plan::CheckBlock reports in. Not
// the WITH list's bodies: a CTE is reached through the references to it, or not at all. Nor a
// compound's branches, which are blocks of their own: a compound's own expressions are its ORDER
// BY.
template <class Fn>
void EachSubqueryIn(const sql::SelectStatement& block, Fn fn) {
  static_assert(sql::kSelectStatementFields == 19, "revisit this walk");
  for (const sql::SelectItem& item : block.items) {
    EachSubqueryOf(item.expr, fn);
  }
  for (const sql::FromItem& item : block.from) {
    for (const sql::Expr& on : item.on) {
      EachSubqueryOf(on, fn);
    }
  }
  for (const std::vector<sql::Expr>* list : {&block.where, &block.group_by, &block.having}) {
    for (const sql::Expr& e : *list) {
      EachSubqueryOf(e, fn);
    }
  }
  for (const sql::OrderItem& item : block.order_by) {
    EachSubqueryOf(item.expr, fn);
  }
}

class Counter {
 public:
  // The relations `block` expands to, saturated; `outer` is the frame around it. A compound's are
  // its branches', each a block in the compound's frame (ADR 0036).
  std::size_t Count(const sql::SelectStatement& block, const CteFrame* outer) {
    static_assert(sql::kSelectStatementFields == 19, "revisit this walk");
    const CteFrame& frame = MakeFrame(block, outer);
    std::size_t total = 0;
    for (const sql::SetBranch& branch : block.set_operation) {
      total = AddSaturating(total, Count(*branch.query, &frame));
      if (total >= kSaturated) {
        return kSaturated;
      }
    }
    for (const sql::FromItem& item : block.from) {
      total = AddSaturating(total, Item(item, frame));
      if (total >= kSaturated) {
        return kSaturated;
      }
    }
    EachSubqueryIn(block, [&](const sql::SubqueryExpr& s) {
      total = AddSaturating(total, Subquery(s, frame));
    });
    return total;
  }

  // The outermost block: the first FROM item whose expansion carries the running total past the
  // limit, so a flat FROM list reports at its 257th item exactly as it did before sub-plans, then
  // its own subqueries, which have no FROM item to report at and carry a span instead.
  //
  // A compound's FROM items are its branches', so a running total runs across them in order and
  // reports at the item (its connector, or the item itself when it is first in its branch) or the
  // subquery that carries it past the limit -- by span, the outermost block having no item of its
  // own to index.
  std::optional<RelationOverflow> Top(const sql::SelectStatement& block) {
    static_assert(sql::kSelectStatementFields == 19, "revisit this walk");
    const CteFrame& frame = MakeFrame(block, nullptr);
    std::size_t total = 0;
    for (const sql::SetBranch& branch : block.set_operation) {
      const sql::SelectStatement& query = *branch.query;
      const CteFrame& inner = MakeFrame(query, &frame);
      for (const sql::FromItem& item : query.from) {
        total = AddSaturating(total, Item(item, inner));
        if (total > kMaxRelations) {
          return RelationOverflow{
              .span = item.connector == sql::Connector::kFirst ? item.span : item.connector_span};
        }
      }
      std::optional<RelationOverflow> over;
      EachSubqueryIn(query, [&](const sql::SubqueryExpr& s) {
        if (over.has_value()) {
          return;
        }
        total = AddSaturating(total, Subquery(s, inner));
        if (total > kMaxRelations) {
          over = RelationOverflow{.span = s.op_span};
        }
      });
      if (over.has_value()) {
        return over;
      }
    }
    for (std::size_t i = 0; i < block.from.size(); ++i) {
      total = AddSaturating(total, Item(block.from[i], frame));
      if (total > kMaxRelations) {
        return RelationOverflow{.item = i};
      }
    }
    std::optional<RelationOverflow> over;
    EachSubqueryIn(block, [&](const sql::SubqueryExpr& s) {
      if (over.has_value()) {
        return;  // the first one past the limit is the one reported
      }
      total = AddSaturating(total, Subquery(s, frame));
      if (total > kMaxRelations) {
        over = RelationOverflow{.span = s.op_span};
      }
    });
    return over;
  }

 private:
  // The frame `block` counts in, and one frame per CTE of its list holding the CTEs before it.
  // Frames live in a deque, so a reference stays valid as the walk pushes more.
  const CteFrame& MakeFrame(const sql::SelectStatement& block, const CteFrame* outer) {
    std::vector<const sql::CommonTableExpr*> visible;
    visible.reserve(block.with.size());
    for (const sql::CommonTableExpr& cte : block.with) {
      frames_.push_back(CteFrame{.visible = visible, .outer = outer});
      declared_in_.emplace(&cte, &frames_.back());
      visible.push_back(&cte);
    }
    frames_.push_back(CteFrame{.visible = std::move(visible), .outer = outer});
    return frames_.back();
  }

  // The relations one FROM item expands to, in the frame of the block that holds it.
  std::size_t Item(const sql::FromItem& item, const CteFrame& frame) {
    if (const auto* derived = std::get_if<sql::DerivedTable>(&item.source)) {
      return AddSaturating(1, Count(*derived->query, &frame));
    }
    const sql::TableRef* ref = item.table();
    if (ref == nullptr) {
      return 1;
    }
    // A CTE hides a catalog table of the same name (rule 8), and an item written as a string
    // literal names a CTE before it names a path.
    const sql::CommonTableExpr* cte = frame.Find(AsciiLower(ref->name));
    return cte == nullptr ? 1 : AddSaturating(1, Body(*cte));
  }

  // One subquery expression: 1 plus its query, as a derived table counts, since the roadmap binds
  // it as a join input of its own. Its query sees the CTEs of the block that holds it.
  std::size_t Subquery(const sql::SubqueryExpr& s, const CteFrame& frame) {
    return AddSaturating(1, Count(*s.query, &frame));
  }

  // A CTE's body, counted once however often it is referenced.
  std::size_t Body(const sql::CommonTableExpr& cte) {
    const auto memo = memo_.find(&cte);
    if (memo != memo_.end()) {
      return memo->second;
    }
    // A body that reached itself would recurse forever; the parser rejects a repeated name and a
    // body never sees itself, so this is a stop, not a reachable path.
    memo_.emplace(&cte, kSaturated);
    const auto declared = declared_in_.find(&cte);
    const CteFrame* outer = declared == declared_in_.end() ? nullptr : declared->second;
    const std::size_t count = Count(*cte.query, outer);
    memo_[&cte] = count;
    return count;
  }

  std::unordered_map<const sql::CommonTableExpr*, std::size_t> memo_;
  std::unordered_map<const sql::CommonTableExpr*, const CteFrame*> declared_in_;
  std::deque<CteFrame> frames_;
};

}  // namespace

std::size_t CountRelations(const sql::SelectStatement& block) {
  Counter counter;
  return counter.Count(block, nullptr);
}

std::optional<RelationOverflow> RelationsExceedLimit(const sql::SelectStatement& block) {
  Counter counter;
  return counter.Top(block);
}

namespace {

// The same frames as the count, walked for the blocks rather than for their size.
class Reach {
 public:
  void Walk(const sql::SelectStatement& block, const CteFrame* outer) {
    static_assert(sql::kSelectStatementFields == 19, "revisit this walk");
    const CteFrame& frame = MakeFrame(block, outer);
    if (!block.set_operation.empty()) {
      // A compound's branches are blocks of their own, in its frame, and they come before it: its
      // own clauses (ORDER BY, LIMIT, OFFSET) follow them in the query (ADR 0036).
      for (const sql::SetBranch& branch : block.set_operation) {
        Walk(*branch.query, &frame);
      }
      blocks_.push_back(&block);
      EachSubqueryIn(block, [&](const sql::SubqueryExpr& s) { Walk(*s.query, &frame); });
      return;
    }
    blocks_.push_back(&block);
    for (const sql::FromItem& item : block.from) {
      if (const auto* derived = std::get_if<sql::DerivedTable>(&item.source)) {
        Walk(*derived->query, &frame);
        continue;
      }
      const sql::TableRef* ref = item.table();
      if (ref == nullptr) {
        continue;
      }
      const sql::CommonTableExpr* cte = frame.Find(AsciiLower(ref->name));
      if (cte != nullptr && seen_.insert(cte).second) {
        const auto declared = declared_in_.find(cte);
        Walk(*cte->query, declared == declared_in_.end() ? nullptr : declared->second);
      }
    }
    // A subquery's query is a block of its own, after the blocks of this one's FROM list: a check
    // over the result reports at the first unsupported construct in query order.
    EachSubqueryIn(block, [&](const sql::SubqueryExpr& s) { Walk(*s.query, &frame); });
  }

  std::vector<const sql::SelectStatement*> take() { return std::move(blocks_); }

 private:
  const CteFrame& MakeFrame(const sql::SelectStatement& block, const CteFrame* outer) {
    std::vector<const sql::CommonTableExpr*> visible;
    visible.reserve(block.with.size());
    for (const sql::CommonTableExpr& cte : block.with) {
      frames_.push_back(CteFrame{.visible = visible, .outer = outer});
      declared_in_.emplace(&cte, &frames_.back());
      visible.push_back(&cte);
    }
    frames_.push_back(CteFrame{.visible = std::move(visible), .outer = outer});
    return frames_.back();
  }

  std::vector<const sql::SelectStatement*> blocks_;
  std::unordered_set<const sql::CommonTableExpr*> seen_;
  std::unordered_map<const sql::CommonTableExpr*, const CteFrame*> declared_in_;
  std::deque<CteFrame> frames_;
};

}  // namespace

std::vector<const sql::SelectStatement*> ReachableBlocks(const sql::SelectStatement& block) {
  Reach reach;
  reach.Walk(block, nullptr);
  return reach.take();
}

}  // namespace antb1::plan
