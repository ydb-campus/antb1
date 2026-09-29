#include "group_table.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include <arrow/api.h>
#include <arrow/array/concatenate.h>
#include <arrow/compute/api_vector.h>
#include <arrow/compute/exec.h>
#include <arrow/compute/row/grouper.h>

#include "antb1/exec/grouped_aggregate_state.h"
#include "antb1/exec/memory_budget.h"
#include "antb1/plan/logical_plan.h"
#include "antb1/plan/types.h"

#include "double_key.h"

namespace antb1::exec {
namespace {

std::span<const std::uint32_t> IdsOf(const arrow::UInt32Array& ids) {
  return {ids.raw_values(), static_cast<std::size_t>(ids.length())};
}

// For each group new since `before` (ids before .. after - 1), the first position in `ids` that
// maps to it: the grouper numbers new groups in an order of its own, so they are found per id.
arrow::Result<std::shared_ptr<arrow::Array>> FirstPositions(std::span<const std::uint32_t> ids,
                                                            std::uint32_t before,
                                                            std::uint32_t after,
                                                            arrow::MemoryPool* pool) {
  std::vector<std::int64_t> first(after - before, -1);
  std::uint32_t found = 0;
  for (std::size_t i = 0; i < ids.size() && found < after - before; ++i) {
    const std::uint32_t id = ids[i];
    if (id >= before && first[id - before] < 0) {
      first[id - before] = static_cast<std::int64_t>(i);
      ++found;
    }
  }
  arrow::Int64Builder positions(pool);
  ARROW_RETURN_NOT_OK(positions.AppendValues(first));
  return positions.Finish();
}

}  // namespace

GroupTable::GroupTable(std::vector<plan::BoundColumn> keys,
                       std::vector<plan::AggregateCall> aggregates, arrow::MemoryPool* pool)
    : keys_(std::move(keys)), aggregates_(std::move(aggregates)), pool_(pool) {}

GroupTable::~GroupTable() = default;

arrow::Result<std::unique_ptr<GroupTable>> GroupTable::Make(
    std::vector<plan::BoundColumn> keys, std::vector<plan::AggregateCall> aggregates,
    int input_width, arrow::MemoryPool* pool, MemoryBudget* budget) {
  std::unique_ptr<GroupTable> table(new GroupTable(std::move(keys), std::move(aggregates), pool));
  table->memory_.Reset(budget);
  std::vector<arrow::TypeHolder> key_types;
  for (const plan::BoundColumn& key : table->keys_) {
    if (key.index < 0 || key.index >= input_width) {
      return arrow::Status::Invalid("grouping key outside its input");
    }
    key_types.emplace_back(plan::ToArrow(key.type));
  }
  for (const plan::AggregateCall& call : table->aggregates_) {
    std::optional<plan::LogicalType> input;
    if (call.arg.has_value()) {
      if (call.arg->index < 0 || call.arg->index >= input_width) {
        return arrow::Status::Invalid("aggregate over a column outside its input");
      }
      input = call.arg->type;
    }
    ARROW_ASSIGN_OR_RAISE(auto state, MakeGroupedAggregateState(call.kind, input, call.type, pool));
    table->states_.push_back(std::move(state));
  }
  table->kernels_ = std::make_unique<arrow::compute::ExecContext>(pool);
  if (!table->keys_.empty()) {
    ARROW_ASSIGN_OR_RAISE(table->grouper_,
                          arrow::compute::Grouper::Make(key_types, table->kernels_.get()));
  }
  table->first_keys_.assign(table->keys_.size(), {});
  return table;
}

arrow::Status GroupTable::AddGroups(std::uint32_t after,
                                    std::vector<std::shared_ptr<arrow::Array>> keys) {
  for (std::size_t k = 0; k < keys.size(); ++k) {
    first_keys_[k].push_back(std::move(keys[k]));
  }
  chunk_groups_.push_back(after - num_groups_);
  num_groups_ = after;
  for (const auto& state : states_) {
    state->Resize(num_groups_);
  }
  return arrow::Status::OK();
}

arrow::Status GroupTable::Consume(const arrow::RecordBatch& rows) {
  const std::int64_t n = rows.num_rows();
  std::vector<std::uint32_t> no_keys;
  std::shared_ptr<arrow::UInt32Array> ids;
  std::span<const std::uint32_t> group_ids;
  if (keys_.empty()) {  // one group of every row
    if (num_groups_ == 0) {
      ARROW_RETURN_NOT_OK(AddGroups(1, {}));
    }
    no_keys.assign(static_cast<std::size_t>(n), 0);
    group_ids = no_keys;
  } else {
    std::vector<arrow::Datum> keys;
    keys.reserve(keys_.size());
    for (const plan::BoundColumn& key : keys_) {
      std::shared_ptr<arrow::Array> column = rows.column(key.index);
      if (key.type == plan::LogicalType::kDouble) {
        ARROW_ASSIGN_OR_RAISE(column, NormalizeDoubleKey(column, pool_));
      }
      keys.emplace_back(std::move(column));
    }
    const arrow::compute::ExecBatch key_batch(std::move(keys), n);
    ARROW_ASSIGN_OR_RAISE(const arrow::Datum id_datum,
                          grouper_->Consume(arrow::compute::ExecSpan(key_batch)));
    ids = std::static_pointer_cast<arrow::UInt32Array>(id_datum.make_array());
    group_ids = IdsOf(*ids);
    const std::uint32_t after = grouper_->num_groups();
    if (after > num_groups_) {
      // The original key values (not the normalized ones) of each new group's first row.
      ARROW_ASSIGN_OR_RAISE(const auto positions,
                            FirstPositions(group_ids, num_groups_, after, pool_));
      std::vector<std::shared_ptr<arrow::Array>> first;
      first.reserve(keys_.size());
      for (const plan::BoundColumn& key : keys_) {
        ARROW_ASSIGN_OR_RAISE(
            const arrow::Datum taken,
            arrow::compute::Take(rows.column(key.index), positions,
                                 arrow::compute::TakeOptions::NoBoundsCheck(), kernels_.get()));
        first.push_back(taken.make_array());
      }
      ARROW_RETURN_NOT_OK(AddGroups(after, std::move(first)));
    }
  }
  for (std::size_t i = 0; i < aggregates_.size(); ++i) {
    const plan::AggregateCall& call = aggregates_[i];
    const arrow::Array* values =
        call.arg.has_value() ? rows.column(call.arg->index).get() : nullptr;
    ARROW_RETURN_NOT_OK(states_[i]->Consume(values, group_ids));
  }
  return Account();
}

arrow::Status GroupTable::Merge(const GroupTable& part) {
  if (part.num_groups_ == 0) {
    return arrow::Status::OK();
  }
  std::vector<std::uint32_t> no_keys;
  std::shared_ptr<arrow::UInt32Array> ids;
  std::span<const std::uint32_t> group_map;
  if (keys_.empty()) {
    if (num_groups_ == 0) {
      ARROW_RETURN_NOT_OK(AddGroups(1, {}));
    }
    no_keys.assign(1, 0);
    group_map = no_keys;
  } else {
    // Part's groups in its own order, by their normalized keys: the map from its groups to ours.
    ARROW_ASSIGN_OR_RAISE(const arrow::compute::ExecBatch uniques, part.grouper_->GetUniques());
    ARROW_ASSIGN_OR_RAISE(const arrow::Datum id_datum,
                          grouper_->Consume(arrow::compute::ExecSpan(uniques)));
    ids = std::static_pointer_cast<arrow::UInt32Array>(id_datum.make_array());
    group_map = IdsOf(*ids);
    const std::uint32_t after = grouper_->num_groups();
    if (after > num_groups_) {
      ARROW_RETURN_NOT_OK(AddPartGroups(part, group_map, after));
    }
  }
  for (std::size_t i = 0; i < states_.size(); ++i) {
    ARROW_RETURN_NOT_OK(states_[i]->Merge(*part.states_[i], group_map));
  }
  return Account();
}

arrow::Status GroupTable::AddPartGroups(const GroupTable& part,
                                        std::span<const std::uint32_t> group_map,
                                        std::uint32_t after) {
  // The part group of each new group (a new group keeps part's first-seen key values), found per
  // id as in Consume.
  std::vector<std::uint32_t> source(after - num_groups_, 0);
  for (std::uint32_t g = 0; g < group_map.size(); ++g) {
    if (group_map[g] >= num_groups_) {
      source[group_map[g] - num_groups_] = g;
    }
  }
  // Where each part group's keys are: its chunk and row there.
  std::vector<std::uint32_t> chunk_start;
  chunk_start.reserve(part.chunk_groups_.size());
  std::uint32_t start = 0;
  for (const std::uint32_t groups : part.chunk_groups_) {
    chunk_start.push_back(start);
    start += groups;
  }
  // New groups in chunks of at most kMaxMergeChunk, like the chunks Consume makes, so that no
  // key array (nor a Finalize range) grows with the part.
  for (std::size_t begin = 0; begin < source.size(); begin += kMaxMergeChunk) {
    const std::size_t end = std::min(source.size(), begin + kMaxMergeChunk);
    // Per part chunk, the rows this slice takes from it; `order` puts them back in slice order.
    std::vector<std::vector<std::int64_t>> rows(part.chunk_groups_.size());
    std::vector<std::pair<std::size_t, std::size_t>> at;  // (chunk, index in rows[chunk])
    at.reserve(end - begin);
    for (std::size_t i = begin; i < end; ++i) {
      const auto chunk = static_cast<std::size_t>(std::ranges::upper_bound(chunk_start, source[i]) -
                                                  chunk_start.begin() - 1);
      at.emplace_back(chunk, rows[chunk].size());
      rows[chunk].push_back(static_cast<std::int64_t>(source[i] - chunk_start[chunk]));
    }
    std::vector<std::int64_t> offsets(rows.size(), 0);
    std::int64_t total = 0;
    for (std::size_t c = 0; c < rows.size(); ++c) {
      offsets[c] = total;
      total += static_cast<std::int64_t>(rows[c].size());
    }
    arrow::Int64Builder order_builder(pool_);
    for (const auto& [chunk, index] : at) {
      ARROW_RETURN_NOT_OK(order_builder.Append(offsets[chunk] + static_cast<std::int64_t>(index)));
    }
    ARROW_ASSIGN_OR_RAISE(const auto order, order_builder.Finish());
    std::vector<std::shared_ptr<arrow::Array>> first;
    first.reserve(keys_.size());
    for (const auto& chunks : part.first_keys_) {
      arrow::ArrayVector pieces;
      for (std::size_t c = 0; c < rows.size(); ++c) {
        if (rows[c].empty()) {
          continue;
        }
        arrow::Int64Builder indices(pool_);
        ARROW_RETURN_NOT_OK(indices.AppendValues(rows[c]));
        ARROW_ASSIGN_OR_RAISE(const auto taken_rows, indices.Finish());
        ARROW_ASSIGN_OR_RAISE(
            const arrow::Datum piece,
            arrow::compute::Take(chunks[c], taken_rows,
                                 arrow::compute::TakeOptions::NoBoundsCheck(), kernels_.get()));
        pieces.push_back(piece.make_array());
      }
      ARROW_ASSIGN_OR_RAISE(const auto slice, arrow::Concatenate(pieces, pool_));
      ARROW_ASSIGN_OR_RAISE(
          const arrow::Datum ordered,
          arrow::compute::Take(slice, order, arrow::compute::TakeOptions::NoBoundsCheck(),
                               kernels_.get()));
      first.push_back(ordered.make_array());
    }
    ARROW_RETURN_NOT_OK(
        AddGroups(num_groups_ + static_cast<std::uint32_t>(end - begin), std::move(first)));
  }
  return arrow::Status::OK();
}

arrow::Status GroupTable::Account() {
  auto bytes = static_cast<std::int64_t>(chunk_groups_.capacity() * sizeof(std::uint32_t));
  for (const auto& state : states_) {
    bytes += state->memory_usage();
  }
  return memory_.Resize(bytes);
}

arrow::Result<std::shared_ptr<arrow::RecordBatch>> GroupTable::NextChunk(
    const std::shared_ptr<arrow::Schema>& schema) {
  if (next_chunk_ >= chunk_groups_.size()) {
    return nullptr;
  }
  const std::uint32_t rows = chunk_groups_[next_chunk_];
  arrow::ArrayVector columns;
  columns.reserve(keys_.size() + states_.size());
  for (auto& chunks : first_keys_) {
    columns.push_back(std::move(chunks[next_chunk_]));
  }
  for (const auto& state : states_) {
    ARROW_ASSIGN_OR_RAISE(auto column, state->Finalize(next_group_, next_group_ + rows, pool_));
    columns.push_back(std::move(column));
  }
  ++next_chunk_;
  next_group_ += rows;
  return arrow::RecordBatch::Make(schema, rows, std::move(columns));
}

}  // namespace antb1::exec
