#pragma once

#include <unordered_map>

#include "antb1/plan/logical_plan.h"

namespace antb1::plan {

// Union-find over the identities of bound (and therefore already coerced) columns.
class ColumnClasses {
 public:
  ColumnId Find(ColumnId id) {
    auto it = parent_.try_emplace(id, id).first;
    while (it->first != it->second) {
      const auto parent = parent_.find(it->second);
      it->second = parent->second;
      it = parent;
    }
    return it->first;
  }

  // True when this equality supplies a constraint the others did not already enforce.
  bool Merge(ColumnId a, ColumnId b) {
    const ColumnId left = Find(a);
    const ColumnId right = Find(b);
    if (left == right) {
      return false;
    }
    parent_.at(left) = right;
    return true;
  }

  bool Same(ColumnId a, ColumnId b) { return Find(a) == Find(b); }

 private:
  std::unordered_map<ColumnId, ColumnId> parent_;
};

}  // namespace antb1::plan
