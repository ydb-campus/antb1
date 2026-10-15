#include "or_factoring.h"

#include <algorithm>
#include <cstddef>
#include <utility>
#include <variant>
#include <vector>

#include "antb1/sql/ast.h"

namespace antb1::plan {
namespace {

// The branches of a top-level OR chain, flattened; a branch is whatever is not an OR.
void Branches(const sql::Expr& expr, std::vector<const sql::Expr*>& out) {
  const auto* binary = std::get_if<sql::BinaryExpr>(&expr);
  if (binary != nullptr && binary->op == sql::BinaryOp::kOr) {
    Branches(*binary->left, out);
    Branches(*binary->right, out);
    return;
  }
  out.push_back(&expr);
}

// `left OR right`, spanning both, as the parser builds one (left-deep).
sql::Expr Disjunction(sql::Expr left, sql::Expr right) {
  const SourceSpan span{.offset = left.span().offset,
                        .length = right.span().offset + right.span().length - left.span().offset};
  return sql::Expr{sql::BinaryExpr{.op = sql::BinaryOp::kOr,
                                   .left = sql::Box<sql::Expr>(std::move(left)),
                                   .right = sql::Box<sql::Expr>(std::move(right)),
                                   .op_span = span,
                                   .span = span}};
}

// `parts` AND-ed in order, as one expression; `parts` must not be empty.
sql::Expr Conjunction(const std::vector<const sql::Expr*>& parts) {
  sql::Expr out = *parts.front();
  for (std::size_t i = 1; i < parts.size(); ++i) {
    const SourceSpan span{
        .offset = out.span().offset,
        .length = parts[i]->span().offset + parts[i]->span().length - out.span().offset};
    out = sql::Expr{sql::BinaryExpr{.op = sql::BinaryOp::kAnd,
                                    .left = sql::Box<sql::Expr>(std::move(out)),
                                    .right = sql::Box<sql::Expr>(*parts[i]),
                                    .op_span = span,
                                    .span = span}};
  }
  return out;
}

// One column an OR constrains to constants, and the constants each branch allows it.
struct Constrained {
  const sql::Expr* column;               // the operand every branch compares, by structure
  std::vector<const sql::Expr*> values;  // the literals, in the order the branches write them
};

// The literals `conjunct` constrains `column` to, appended to `out`; false when it constrains it to
// something that is not a set of literals, in which case nothing can be derived for that column.
// Only `=` and `IN` bound a column to constants: `<>`, `<`, `NOT IN` and a nested OR do not.
bool ValuesOf(const sql::Expr& conjunct, const sql::Expr& column,
              std::vector<const sql::Expr*>& out) {
  if (const auto* binary = std::get_if<sql::BinaryExpr>(&conjunct)) {
    if (binary->op != sql::BinaryOp::kEq) {
      return false;
    }
    // `c = literal` or `literal = c`: either side may be the column.
    const bool left_is_column = sql::EqualIgnoringSpans(*binary->left, column);
    const sql::Expr& other = left_is_column ? *binary->right : *binary->left;
    if (!left_is_column && !sql::EqualIgnoringSpans(*binary->right, column)) {
      return false;
    }
    if (!std::holds_alternative<sql::Literal>(other)) {
      return false;
    }
    out.push_back(&other);
    return true;
  }
  if (const auto* in = std::get_if<sql::InExpr>(&conjunct)) {
    if (in->negated || !sql::EqualIgnoringSpans(*in->operand, column)) {
      return false;
    }
    for (const sql::Expr& value : in->list) {
      if (!std::holds_alternative<sql::Literal>(value)) {
        return false;
      }
      out.push_back(&value);
    }
    return !in->list.empty();
  }
  return false;
}

// Mixing literal kinds would make the derived IN a bind error on a query that binds today, so a
// column is only derived for when every branch writes the same kind of literal.
bool SameKind(const std::vector<const sql::Expr*>& values) {
  const sql::Literal::Kind kind = std::get<sql::Literal>(*values.front()).kind;
  return std::ranges::all_of(
      values, [kind](const sql::Expr* v) { return std::get<sql::Literal>(*v).kind == kind; });
}

// The IN lists a top-level OR implies, appended to `out`: where every branch constrains one column
// to literals, their union is implied, since a row that satisfies the OR satisfies some branch and
// so takes one of that branch's values. `(c = 'A' AND x) OR (c = 'B' AND y)` therefore implies
// `c IN ('A', 'B')` (ADR 0022's "filters implied by ORs").
//
// The derived conjunct is redundant by construction -- the OR stays where it is -- so it can only
// change the plan, never the answer: a single-relation IN reaches its scan and its estimate, where
// the OR, spanning two relations, is a join residual that neither can see.
void DeriveImpliedInLists(const std::vector<std::vector<const sql::Expr*>>& per_branch,
                          const std::vector<const sql::Expr*>& already,
                          std::vector<sql::Box<sql::Expr>>& owned,
                          std::vector<FactoredConjunct>& out) {
  std::vector<Constrained> candidates;
  // The first branch's columns are the only ones that can be constrained by every branch.
  for (const sql::Expr* conjunct : per_branch.front()) {
    const sql::Expr* column = nullptr;
    if (const auto* binary = std::get_if<sql::BinaryExpr>(conjunct);
        binary != nullptr && binary->op == sql::BinaryOp::kEq) {
      column =
          std::holds_alternative<sql::Literal>(*binary->right) ? &*binary->left : &*binary->right;
    } else if (const auto* in = std::get_if<sql::InExpr>(conjunct); in != nullptr && !in->negated) {
      column = &*in->operand;
    }
    if (column == nullptr || std::holds_alternative<sql::Literal>(*column)) {
      continue;
    }
    const bool seen = std::ranges::any_of(candidates, [column](const Constrained& c) {
      return sql::EqualIgnoringSpans(*c.column, *column);
    });
    if (!seen) {
      candidates.push_back({.column = column, .values = {}});
    }
  }
  for (Constrained& candidate : candidates) {
    bool every_branch = true;
    for (const std::vector<const sql::Expr*>& branch : per_branch) {
      const std::size_t before = candidate.values.size();
      for (const sql::Expr* conjunct : branch) {
        // A branch may constrain the column more than once; every such conjunct must be a set of
        // literals, since one that is not leaves the column unbounded in that branch.
        if (ValuesOf(*conjunct, *candidate.column, candidate.values)) {
          continue;
        }
      }
      if (candidate.values.size() == before) {
        every_branch = false;  // this branch does not bound the column
        break;
      }
    }
    if (!every_branch || candidate.values.size() < 2 || !SameKind(candidate.values)) {
      continue;
    }
    // Deduplicate, keeping the written order so that the derived list is deterministic.
    sql::InExpr derived{.operand = sql::Box<sql::Expr>(*candidate.column),
                        .list = {},
                        .negated = false,
                        .op_span = {},
                        .span = {}};
    for (const sql::Expr* value : candidate.values) {
      const bool repeated = std::ranges::any_of(derived.list, [value](const sql::Expr& kept) {
        return sql::EqualIgnoringSpans(kept, *value);
      });
      if (!repeated) {
        derived.list.push_back(*value);
      }
    }
    if (derived.list.size() < 2) {
      continue;  // every branch allows the same single value: the shared-conjunct path has it
    }
    // The element may already hold this conjunct, either because the writer wrote it or because
    // the rewrite ran before: deriving it again would duplicate it and break idempotence.
    const sql::Expr as_expr{derived};
    const auto equal = [&as_expr](const sql::Expr* e) {
      return sql::EqualIgnoringSpans(*e, as_expr);
    };
    if (std::ranges::any_of(already, equal) ||
        std::ranges::any_of(out, [&equal](const FactoredConjunct& c) { return equal(c.expr); })) {
      continue;
    }
    owned.emplace_back(as_expr);
    out.push_back({.expr = &*owned.back()});
  }
}

}  // namespace

void Conjuncts(const sql::Expr& expr, std::vector<const sql::Expr*>& out) {
  const auto* binary = std::get_if<sql::BinaryExpr>(&expr);
  if (binary != nullptr && binary->op == sql::BinaryOp::kAnd) {
    Conjuncts(*binary->left, out);
    Conjuncts(*binary->right, out);
    return;
  }
  out.push_back(&expr);
}

std::vector<const sql::Expr*> Conjuncts(const std::vector<sql::Expr>& predicate) {
  std::vector<const sql::Expr*> out;
  for (const sql::Expr& expr : predicate) {
    Conjuncts(expr, out);
  }
  return out;
}

void FactorSharedConjuncts(const sql::Expr& element, const std::vector<const sql::Expr*>& siblings,
                           std::vector<sql::Box<sql::Expr>>& owned,
                           std::vector<FactoredConjunct>& out) {
  std::vector<const sql::Expr*> flat;
  Conjuncts(element, flat);
  for (const sql::Expr* conjunct : flat) {
    const auto* binary = std::get_if<sql::BinaryExpr>(conjunct);
    if (binary == nullptr || binary->op != sql::BinaryOp::kOr) {
      out.push_back({.expr = conjunct});
      continue;
    }
    std::vector<const sql::Expr*> branches;
    Branches(*conjunct, branches);
    // Per branch, its own conjuncts; the first branch's are the candidates to share.
    std::vector<std::vector<const sql::Expr*>> per_branch(branches.size());
    for (std::size_t b = 0; b < branches.size(); ++b) {
      Conjuncts(*branches[b], per_branch[b]);
    }
    std::vector<const sql::Expr*> shared;
    for (const sql::Expr* candidate : per_branch.front()) {
      const auto has = [candidate](const std::vector<const sql::Expr*>& branch) {
        return std::ranges::any_of(branch, [candidate](const sql::Expr* e) {
          return sql::EqualIgnoringSpans(*e, *candidate);
        });
      };
      // A conjunct the first branch repeats is shared once, not twice.
      const bool already = std::ranges::any_of(shared, [candidate](const sql::Expr* e) {
        return sql::EqualIgnoringSpans(*e, *candidate);
      });
      if (!already && std::ranges::all_of(per_branch, has)) {
        shared.push_back(candidate);
      }
    }
    if (shared.empty()) {
      out.push_back({.expr = conjunct});  // nothing to factor: the OR stands as it is
      DeriveImpliedInLists(per_branch, siblings, owned, out);
      continue;
    }
    // What is left of each branch: its conjuncts that are not shared.
    std::vector<std::vector<const sql::Expr*>> rest(branches.size());
    for (std::size_t b = 0; b < branches.size(); ++b) {
      for (const sql::Expr* e : per_branch[b]) {
        const bool is_shared = std::ranges::any_of(
            shared, [e](const sql::Expr* s) { return sql::EqualIgnoringSpans(*s, *e); });
        if (!is_shared) {
          rest[b].push_back(e);
        }
      }
    }
    for (const sql::Expr* s : shared) {
      out.push_back({.expr = s});
    }
    if (std::ranges::any_of(rest,
                            [](const std::vector<const sql::Expr*>& r) { return r.empty(); })) {
      // A branch of only shared conjuncts makes the whole OR true wherever they hold, so the shared
      // conjuncts alone are the conjunct. The OR stays here, in its own place, to be bound for its
      // errors and discarded.
      out.push_back({.expr = conjunct, .bind_only = true});
      continue;
    }
    sql::Expr folded = Conjunction(rest.front());
    for (std::size_t b = 1; b < rest.size(); ++b) {
      folded = Disjunction(std::move(folded), Conjunction(rest[b]));
    }
    owned.emplace_back(std::move(folded));
    out.push_back({.expr = &*owned.back()});
    DeriveImpliedInLists(rest, siblings, owned, out);
  }
}

}  // namespace antb1::plan
