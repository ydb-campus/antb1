#include "or_factoring.h"

#include <algorithm>
#include <cstddef>
#include <utility>
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

void FactorSharedConjuncts(const sql::Expr& element, std::vector<sql::Box<sql::Expr>>& owned,
                           std::vector<const sql::Expr*>& conjuncts,
                           std::vector<const sql::Expr*>& absorbed) {
  std::vector<const sql::Expr*> flat;
  Conjuncts(element, flat);
  for (const sql::Expr* conjunct : flat) {
    const auto* binary = std::get_if<sql::BinaryExpr>(conjunct);
    if (binary == nullptr || binary->op != sql::BinaryOp::kOr) {
      conjuncts.push_back(conjunct);
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
      conjuncts.push_back(conjunct);  // nothing to factor: the OR stands as it is
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
      conjuncts.push_back(s);
    }
    if (std::ranges::any_of(rest,
                            [](const std::vector<const sql::Expr*>& r) { return r.empty(); })) {
      // A branch of only shared conjuncts makes the whole OR true wherever they hold, so the shared
      // conjuncts alone are the conjunct. The OR is still bound, for its errors, and discarded.
      absorbed.push_back(conjunct);
      continue;
    }
    sql::Expr folded = Conjunction(rest.front());
    for (std::size_t b = 1; b < rest.size(); ++b) {
      folded = Disjunction(std::move(folded), Conjunction(rest[b]));
    }
    owned.push_back(sql::Box<sql::Expr>(std::move(folded)));
    conjuncts.push_back(&*owned.back());
  }
}

}  // namespace antb1::plan
