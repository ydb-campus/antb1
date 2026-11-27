#include "antb1/plan/deferred_dimension_join.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <utility>
#include <vector>

#include <arrow/type.h>

#include "antb1/common/narrow.h"

namespace antb1::plan {
namespace {

LogicalNodePtr Node(LogicalNode value) {
  return std::make_shared<const LogicalNode>(std::move(value));
}

std::optional<std::size_t> Position(const std::vector<ColumnId>& ids, ColumnId id) {
  const auto it = std::ranges::find(ids, id);
  if (id == kNoColumnId || it == ids.end()) {
    return std::nullopt;
  }
  return static_cast<std::size_t>(it - ids.begin());
}

// The integer key travels with this input. Every dropped dimension column can be restored
// from its table field after all branches have matched the same dimension member.
struct Reduced {
  LogicalNodePtr node;
  BoundColumn surrogate;
  std::map<ColumnId, int> fields;
};

bool InnerEquality(const JoinNode& join) {
  return join.kind == JoinKind::kInner && join.keys.size() == 1 && !join.keys.front().null_equal &&
         !join.verify_dimension_business_field && join.left != nullptr && join.right != nullptr;
}

void Append(ProjectNode& project, BoundColumn source, ColumnId output,
            const std::optional<Constant>& constant = std::nullopt) {
  project.columns.push_back(std::move(source));
  project.ids.push_back(output);
  project.constants.push_back(constant);
}

// A single-input union is just a column mapping. Keep constants and non-dimension columns in
// order, and carry the integer key even if it wasn't in the original SELECT list.
Reduced Project(Reduced input, const ProjectNode& original) {
  ProjectNode project{.input = std::move(input.node), .span = original.span};
  std::map<ColumnId, int> fields;
  for (std::size_t i = 0; i < original.columns.size(); ++i) {
    const auto constant = original.constants.empty() ? std::nullopt : original.constants[i];
    const auto field = input.fields.find(original.columns[i].id);
    if (!constant && field != input.fields.end()) {
      fields.emplace(original.ids[i], field->second);
    } else {
      Append(project, original.columns[i], original.ids[i], constant);
    }
  }
  if (!std::ranges::contains(project.ids, input.surrogate.id)) {
    Append(project, input.surrogate, input.surrogate.id);
  }
  return {
      .node = Node(std::move(project)), .surrogate = input.surrogate, .fields = std::move(fields)};
}

bool IntegerKey(LogicalType type) { return IsInteger(type) && type != LogicalType::kHugeInt; }

bool HasType(const arrow::Schema& schema, int field, LogicalType type) {
  const auto logical = FromArrow(*schema.field(field)->type());
  return logical.ok() && *logical == type;
}

// A filter fixing this key still fixes it after a compute or an inner join. Stop at
// null-extending joins: they can introduce a second value even on a filtered input.
bool FixedByEquality(const LogicalNodePtr& node, const BoundColumn& key) {
  if (const auto* filter = std::get_if<FilterNode>(node.get())) {
    return std::ranges::any_of(filter->predicates,
                               [&](const Predicate& predicate) {
                                 return predicate.column && predicate.column->id == key.id &&
                                        predicate.column->type == key.type &&
                                        IsIntegerOrDateEquality(predicate);
                               }) ||
           FixedByEquality(filter->input, key);
  }
  if (const auto* compute = std::get_if<ComputeNode>(node.get())) {
    return FixedByEquality(compute->input, key);
  }
  if (const auto* join = std::get_if<JoinNode>(node.get());
      join != nullptr && join->kind == JoinKind::kInner) {
    return FixedByEquality(join->left, key) || FixedByEquality(join->right, key);
  }
  return false;
}

// A final over one partial must preserve both its value and its type. SUM of a NULL partial
// stays NULL. SUM_COUNTS returns zero for NULL, so it may pass through only a non-NULL count.
bool KeepsPartial(const AggregateCall& call, const AggregateCall& state) {
  if (call.type != state.type || call.type == LogicalType::kDouble) {
    return false;
  }
  if (call.kind == AggKind::kSumCounts) {
    return call.type == LogicalType::kBigInt &&
           (state.kind == AggKind::kCount || state.kind == AggKind::kCountStar);
  }
  if (call.kind != state.kind) {
    return false;
  }
  if (call.kind == AggKind::kSum) {
    return call.type == LogicalType::kHugeInt ||
           (call.type == LogicalType::kDecimal &&
            call.type.width() == LogicalType::kMaxDecimalWidth);
  }
  return call.kind == AggKind::kMin || call.kind == AggKind::kMax;
}

class Rewrite {
 public:
  std::optional<LogicalPlan> Run(const LogicalPlan& plan);

 private:
  std::optional<Reduced> Tree(const LogicalNodePtr& node);
  std::optional<Reduced> Group(const GroupAggregateNode& group, const ScanNode& dimension,
                               const BoundColumn& dimension_key, const LogicalNodePtr& facts,
                               BoundColumn fact_key);
  std::optional<Reduced> Join(const JoinNode& join);
  ScanNode dimension_;
  std::set<int> required_fields_;
  int surrogate_field_ = -1;
  int business_field_ = -1;
  const LogicalNode* root_ = nullptr;
  ColumnId last_id_ = kNoColumnId;
  bool exhausted_ = false;
  ColumnId Mint() {
    // Most sorted joins have no such branches. Walk the entire plan for a fresh ID only
    // after we have matched a dimension join and its complete pair of aggregations.
    if (last_id_ == kNoColumnId) {
      last_id_ = MaxColumnId(*root_);
    }
    if (std::to_underlying(last_id_) == std::numeric_limits<std::uint32_t>::max()) {
      exhausted_ = true;
      return kNoColumnId;
    }
    last_id_ = ColumnId{std::to_underlying(last_id_) + 1};
    return last_id_;
  }
};

std::optional<Reduced> Rewrite::Group(const GroupAggregateNode& group, const ScanNode& dimension,
                                      const BoundColumn& dimension_key, const LogicalNodePtr& facts,
                                      BoundColumn fact_key) {
  const auto* partial = std::get_if<GroupAggregateNode>(facts.get());
  const auto pk = Position(dimension.ids, dimension_key.id);
  if (partial == nullptr || !partial->input || !pk || !dimension.table ||
      !IntegerKey(dimension_key.type) || fact_key.type != dimension_key.type ||
      dimension.fields.size() != dimension.ids.size() ||
      group.keys.size() != group.key_ids.size() ||
      partial->keys.size() != partial->key_ids.size()) {
    return std::nullopt;
  }
  const auto fk = Position(partial->key_ids, fact_key.id);
  if (!fk || partial->keys[*fk].type != fact_key.type) {
    return std::nullopt;
  }
  const auto& schema = dimension.table->schema();
  if (!schema || std::ranges::any_of(dimension.fields, [&](int field) {
        return field < 0 || field >= schema->num_fields();
      })) {
    return std::nullopt;
  }
  const int pk_field = dimension.fields[*pk];
  if (!HasType(*schema, pk_field, dimension_key.type) ||
      (dimension_.table && (dimension.table != dimension_.table || pk_field != surrogate_field_))) {
    return std::nullopt;
  }
  // A unique dimension gives no guarantee that a fact key has a matching dimension row.
  // Require at most one group per surrogate in each branch: otherwise an orphan can make
  // a Cartesian product of fact groups before the final lookup discards them.
  for (std::size_t i = 0; i < partial->key_ids.size(); ++i) {
    const auto id = partial->key_ids[i];
    if (id != fact_key.id && (!std::ranges::contains(group.keys, id, &BoundColumn::id) ||
                              !FixedByEquality(partial->input, partial->keys[i]))) {
      return std::nullopt;
    }
  }
  // This one-input column mapping also stops key filters offered by the *new* integer joins.
  // Otherwise they could prune rows on which an original partial's computation raises an error.
  UnionAllNode reduced{.inputs = {facts}, .columns = {{}}, .span = group.span};
  auto& columns = reduced.columns.front();
  std::map<ColumnId, int> fields;
  for (std::size_t i = 0; i < group.keys.size(); ++i) {
    const auto& key = group.keys[i];
    if (key.type == LogicalType::kDouble) {
      return std::nullopt;  // retained representative of grouped signed zero / NaN
    }
    if (const auto f = Position(dimension.ids, key.id)) {
      const int field = dimension.fields[*f];
      if (!HasType(*schema, field, key.type)) {
        return std::nullopt;
      }
      fields.emplace(group.key_ids[i], field);
    } else if (const auto k = Position(partial->key_ids, key.id);
               k && partial->keys[*k].type == key.type) {
      columns.push_back(key);
      reduced.ids.push_back(group.key_ids[i]);
    } else {
      return std::nullopt;
    }
  }
  if (fields.empty()) {
    return std::nullopt;
  }
  for (const auto& call : group.aggregates) {
    if (!call.arg || call.type != call.arg->type) {
      return std::nullopt;
    }
    const auto state = std::ranges::find(partial->aggregates, call.arg->id, &AggregateCall::id);
    if (state == partial->aggregates.end() || !KeepsPartial(call, *state)) {
      return std::nullopt;
    }
    columns.push_back(*call.arg);
    reduced.ids.push_back(call.id);
  }
  columns.push_back(fact_key);
  fact_key.id = Mint();
  reduced.ids.push_back(fact_key.id);
  dimension_ = dimension;
  required_fields_.insert(dimension.fields.begin(), dimension.fields.end());
  surrogate_field_ = pk_field;
  return Reduced{.node = Node(std::move(reduced)),
                 .surrogate = std::move(fact_key),
                 .fields = std::move(fields)};
}

std::optional<Reduced> Rewrite::Join(const JoinNode& join) {
  if (!InnerEquality(join)) {
    return std::nullopt;
  }
  auto left = Tree(join.left);
  if (!left) {
    return std::nullopt;
  }
  auto right = Tree(join.right);
  if (!right) {
    return std::nullopt;
  }
  const auto& key = join.keys.front();
  auto l = left->fields.find(key.left.id);
  auto r = right->fields.find(key.right.id);
  if (key.left.type != LogicalType::kVarchar || key.right.type != LogicalType::kVarchar ||
      l == left->fields.end() || r == right->fields.end() || l->second != r->second ||
      (business_field_ != -1 && business_field_ != l->second)) {
    return std::nullopt;
  }
  business_field_ = l->second;
  left->fields.merge(right->fields);
  for (const auto& expr : join.residual) {
    if (expr == nullptr) {
      return std::nullopt;
    }
    std::vector<ColumnId> read;
    CollectColumnIds(*expr, read);
    if (std::ranges::any_of(read, [&](auto id) { return left->fields.contains(id); })) {
      return std::nullopt;
    }
  }
  JoinNode fast = join;
  fast.left = std::move(left->node);
  fast.right = std::move(right->node);
  fast.keys = {{.left = left->surrogate, .right = right->surrogate}};
  if (!fast.needed_above.empty()) {
    std::erase_if(fast.needed_above, [&](auto id) { return left->fields.contains(id); });
    fast.needed_above.push_back(left->surrogate.id);
  }
  left->node = Node(std::move(fast));
  return left;
}

std::optional<Reduced> Rewrite::Tree(const LogicalNodePtr& node) {
  if (const auto* join = std::get_if<JoinNode>(node.get())) {
    return Join(*join);
  }
  if (const auto* filter = std::get_if<FilterNode>(node.get())) {
    auto child = Tree(filter->input);
    if (!child) {
      return std::nullopt;
    }
    for (const auto& pred : filter->predicates) {
      if ((pred.column && child->fields.contains(pred.column->id)) ||
          (pred.other && child->fields.contains(pred.other->id))) {
        return std::nullopt;
      }
    }
    FilterNode fast = *filter;
    fast.input = std::move(child->node);
    fast.needed_above.reset();
    child->node = Node(std::move(fast));
    return child;
  }
  if (const auto* projection = std::get_if<ProjectNode>(node.get())) {
    auto child = Tree(projection->input);
    if (child && projection->ids.size() == projection->columns.size() &&
        (projection->constants.empty() || projection->constants.size() == projection->ids.size())) {
      return Project(std::move(*child), *projection);
    }
    return std::nullopt;
  }
  if (const auto* u = std::get_if<UnionAllNode>(node.get())) {
    if (u->inputs.size() == 1 && u->columns.size() == 1 && u->ids.size() == u->columns[0].size()) {
      if (auto child = Tree(u->inputs.front())) {
        return Project(std::move(*child),
                       {.columns = u->columns[0], .ids = u->ids, .span = u->span});
      }
    }
    return std::nullopt;
  }
  const auto* group = std::get_if<GroupAggregateNode>(node.get());
  const auto* join = group == nullptr ? nullptr : std::get_if<JoinNode>(group->input.get());
  if (join == nullptr || !InnerEquality(*join) || !join->residual.empty()) {
    return std::nullopt;
  }
  const auto& key = join->keys.front();
  if (const auto* dim = std::get_if<ScanNode>(join->left.get())) {
    return Group(*group, *dim, key.left, join->right, key.right);
  }
  if (const auto* dim = std::get_if<ScanNode>(join->right.get())) {
    return Group(*group, *dim, key.right, join->left, key.left);
  }
  return std::nullopt;
}

std::optional<LogicalPlan> Rewrite::Run(const LogicalPlan& plan) {
  const auto* output = std::get_if<ProjectNode>(plan.root.get());
  if (output == nullptr ||
      (!output->constants.empty() && output->constants.size() != output->columns.size())) {
    return std::nullopt;
  }
  const auto* limit = std::get_if<LimitNode>(output->input.get());
  if (limit != nullptr && limit->limit == 0) {
    return std::nullopt;
  }
  const auto* sort =
      std::get_if<SortNode>(limit != nullptr ? limit->input.get() : output->input.get());
  if (sort == nullptr || std::get_if<JoinNode>(sort->input.get()) == nullptr) {
    return std::nullopt;
  }
  // Sort consumes the whole candidate before emitting anything. Cover all observable values
  // as well: changing a hash join's keys can change the choice among otherwise tied rows.
  for (const auto& column : output->columns) {
    if (column.id != kNoColumnId && !std::ranges::any_of(sort->keys, [&](const SortKey& key) {
          return key.column.id == column.id;
        })) {
      return std::nullopt;
    }
  }
  root_ = plan.root.get();
  auto fast = Tree(sort->input);
  if (!fast || business_field_ == -1 || !dimension_.table || PositionMismatch(plan)) {
    return std::nullopt;
  }
  if (!HasType(*dimension_.table->schema(), business_field_, LogicalType::kVarchar)) {
    return std::nullopt;
  }
  // A hand-built reordering Project can pass the same input ID through twice at the root.
  // Restoring that dimension field would turn both copies into definitions of the same ID.
  if (std::set<ColumnId>(output->ids.begin(), output->ids.end()).size() != output->ids.size()) {
    return std::nullopt;
  }
  ScanNode scan = dimension_;
  scan.ids.clear();
  scan.fields.clear();
  // Read every field decoded by any original dimension branch. A grouping-only field can
  // still raise a scan error, even when the final projection does not return it.
  std::map<int, ColumnId> scan_ids;
  for (const int field : required_fields_) {
    const auto id = Mint();
    scan.fields.push_back(field);
    scan.ids.push_back(id);
    scan_ids.emplace(field, id);
  }
  const auto business_pos = std::ranges::find(scan.fields, business_field_) - scan.fields.begin();
  auto pk = fast->surrogate;
  pk.id = scan_ids.at(surrogate_field_);
  pk.name = scan.table->schema()->field(surrogate_field_)->name();
  pk.qualifier = scan.table_name;
  auto restore = [&](BoundColumn col) {
    if (auto field = fast->fields.find(col.id); field != fast->fields.end()) {
      col.id = scan_ids.at(field->second);
    }
    return col;
  };
  SortNode order = *sort;
  for (auto& key : order.keys) {
    key.column = restore(std::move(key.column));
  }
  ProjectNode projected = *output;
  for (auto& col : projected.columns) {
    col = restore(std::move(col));
  }
  order.input = Node(JoinNode{.kind = JoinKind::kInner,
                              .left = std::move(fast->node),
                              .right = Node(std::move(scan)),
                              .keys = {{.left = fast->surrogate, .right = pk}},
                              .verify_dimension_business_field = Narrow<int>(business_pos)});
  projected.input = Node(std::move(order));
  if (limit != nullptr) {
    projected.input = WithInputs(*limit, {projected.input});
  }
  if (exhausted_) {
    return std::nullopt;
  }
  return ResolvePositions({.root = Node(std::move(projected)), .output = plan.output});
}
}  // namespace

std::optional<LogicalPlan> FindDeferredDimensionJoin(const LogicalPlan& plan) {
  return Rewrite{}.Run(plan);
}

}  // namespace antb1::plan
