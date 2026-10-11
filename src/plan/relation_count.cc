#include "relation_count.h"

#include <algorithm>
#include <cstddef>
#include <deque>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
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

class Counter {
 public:
  // The relations `block` expands to, saturated; `outer` is the frame around it.
  std::size_t Count(const sql::SelectStatement& block, const CteFrame* outer) {
    const CteFrame& frame = MakeFrame(block, outer);
    std::size_t total = 0;
    for (const sql::FromItem& item : block.from) {
      total = AddSaturating(total, Item(item, frame));
      if (total >= kSaturated) {
        return kSaturated;
      }
    }
    return total;
  }

  // The outermost block: the first FROM item whose expansion carries the running total past the
  // limit, so a flat FROM list reports at its 257th item exactly as it did before sub-plans.
  std::optional<RelationOverflow> Top(const sql::SelectStatement& block) {
    const CteFrame& frame = MakeFrame(block, nullptr);
    std::size_t total = 0;
    for (std::size_t i = 0; i < block.from.size(); ++i) {
      total = AddSaturating(total, Item(block.from[i], frame));
      if (total > kMaxRelations) {
        return RelationOverflow{.item = i};
      }
    }
    return std::nullopt;
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

}  // namespace antb1::plan
