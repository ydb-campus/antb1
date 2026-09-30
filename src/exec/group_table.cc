#include "group_table.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <numeric>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include <arrow/api.h>
#include <arrow/array/concatenate.h>
#include <arrow/compute/api_vector.h>
#include <arrow/compute/exec.h>
#include <arrow/compute/row/grouper.h>
#include <arrow/util/hashing.h>

#include "antb1/common/check.h"
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

arrow::Result<std::vector<std::uint64_t>> KeyHashes(const arrow::compute::ExecBatch& keys) {
  std::vector<std::uint64_t> hashes(static_cast<std::size_t>(keys.length), 0x9E3779B97F4A7C15ULL);
  for (const arrow::Datum& datum : keys.values) {
    const std::shared_ptr<arrow::Array> column = datum.make_array();
    const arrow::DataType& type = *column->type();
    const int width = type.byte_width();
    const auto* binary = dynamic_cast<const arrow::BinaryArray*>(column.get());
    for (std::int64_t row = 0; row < column->length(); ++row) {
      std::uint64_t value = 0x5BD1E995ULL;  // NULL
      if (column->IsValid(row)) {
        if (binary != nullptr) {
          const std::string_view bytes = binary->GetView(row);
          value = arrow::internal::ComputeStringHash<0>(bytes.data(),
                                                        static_cast<std::int64_t>(bytes.size()));
        } else if (type.id() == arrow::Type::BOOL) {
          value = static_cast<const arrow::BooleanArray&>(*column).Value(row) ? 1 : 2;
        } else if (width > 0) {
          const std::uint8_t* bytes =
              column->data()->buffers[1]->data() + ((column->offset() + row) * width);
          value = arrow::internal::ComputeStringHash<0>(bytes, width);
        } else {
          return arrow::Status::NotImplemented("GROUP BY partition of a ", type.ToString(), " key");
        }
      }
      auto& hash = hashes[static_cast<std::size_t>(row)];
      hash = (hash ^ value) * 0x100000001B3ULL;
    }
  }
  for (std::uint64_t& hash : hashes) {
    hash ^= hash >> 33U;
  }
  return hashes;
}

GroupTable::GroupTable(std::vector<plan::BoundColumn> keys,
                       std::vector<plan::AggregateCall> aggregates, arrow::MemoryPool* pool,
                       bool first_keys)
    : keys_(std::move(keys)),
      aggregates_(std::move(aggregates)),
      pool_(pool),
      keep_first_keys_(first_keys) {}

GroupTable::~GroupTable() = default;

arrow::Result<std::unique_ptr<GroupTable>> GroupTable::Make(
    std::vector<plan::BoundColumn> keys, std::vector<plan::AggregateCall> aggregates,
    int input_width, arrow::MemoryPool* pool, MemoryBudget* budget, bool first_keys) {
  std::unique_ptr<GroupTable> table(
      new GroupTable(std::move(keys), std::move(aggregates), pool, first_keys));
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
    if (after > num_groups_ && !keep_first_keys_) {
      ARROW_RETURN_NOT_OK(AddGroups(after, {}));
    } else if (after > num_groups_) {
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
  ANTB1_CHECK(keep_first_keys_);  // only the serial merges keep no first-seen keys
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
      std::vector<std::uint32_t> every(group_map.size());
      std::ranges::iota(every, 0U);
      ARROW_RETURN_NOT_OK(AddPartGroups(part, every, group_map, after));
    }
  }
  for (std::size_t i = 0; i < states_.size(); ++i) {
    ARROW_RETURN_NOT_OK(states_[i]->Merge(*part.states_[i], group_map));
  }
  return Account();
}

arrow::Status GroupTable::AddPartGroups(const GroupTable& part, std::span<const std::uint32_t> from,
                                        std::span<const std::uint32_t> to, std::uint32_t after) {
  // The part group of each new group (a new group keeps part's first-seen key values), found per
  // id as in Consume.
  std::vector<std::uint32_t> source(after - num_groups_, 0);
  for (std::size_t i = 0; i < from.size(); ++i) {
    if (to[i] >= num_groups_) {
      source[to[i] - num_groups_] = from[i];
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

arrow::Status GroupTable::Partition() { return Partition(keys_.size(), {}); }

arrow::Result<std::vector<std::uint64_t>> GroupTable::PrefixHashes(std::size_t prefix) const {
  ANTB1_CHECK(grouper_ != nullptr);
  ANTB1_CHECK(prefix <= keys_.size());
  ARROW_ASSIGN_OR_RAISE(arrow::compute::ExecBatch keys, grouper_->GetUniques());
  keys.values.resize(prefix);
  return KeyHashes(keys);
}

arrow::Result<arrow::compute::ExecBatch> GroupTable::uniques() const {
  ANTB1_CHECK(grouper_ != nullptr);
  return grouper_->GetUniques();
}

arrow::Status GroupTable::Partition(std::size_t prefix, std::span<const std::uint64_t> heavy) {
  ANTB1_CHECK(prefix <= keys_.size());
  partition_groups_.clear();
  partition_uniques_.clear();
  if (keys_.empty()) {  // one group at most, one partition
    partition_groups_.emplace_back();
    if (num_groups_ > 0) {
      partition_groups_.front().push_back(0);
    }
    return arrow::Status::OK();
  }
  partition_groups_.resize(kPartitions);
  ARROW_ASSIGN_OR_RAISE(const arrow::compute::ExecBatch uniques, grouper_->GetUniques());
  arrow::compute::ExecBatch prefix_keys = uniques;
  prefix_keys.values.resize(prefix);
  ARROW_ASSIGN_OR_RAISE(std::vector<std::uint64_t> hashes, KeyHashes(prefix_keys));
  if (prefix < keys_.size() && !heavy.empty()) {
    ARROW_ASSIGN_OR_RAISE(const std::vector<std::uint64_t> all, KeyHashes(uniques));
    for (std::size_t g = 0; g < hashes.size(); ++g) {
      if (std::ranges::binary_search(heavy, hashes[g])) {
        hashes[g] = all[g];
      }
    }
  }
  for (std::uint32_t g = 0; g < hashes.size(); ++g) {
    partition_groups_[hashes[g] % kPartitions].push_back(g);
  }
  // Every partition's keys with one Take per column (the groups in partition order), sliced per
  // partition without copying: 64 Takes per column cost more than the rows for small parts.
  arrow::UInt32Builder order(pool_);
  ARROW_RETURN_NOT_OK(order.Reserve(uniques.length));
  for (const std::vector<std::uint32_t>& groups : partition_groups_) {
    ARROW_RETURN_NOT_OK(order.AppendValues(groups));
  }
  ARROW_ASSIGN_OR_RAISE(const auto rows, order.Finish());
  std::vector<std::shared_ptr<arrow::Array>> columns;
  columns.reserve(uniques.values.size());
  for (const arrow::Datum& column : uniques.values) {
    ARROW_ASSIGN_OR_RAISE(
        const arrow::Datum taken,
        arrow::compute::Take(column, rows, arrow::compute::TakeOptions::NoBoundsCheck(),
                             kernels_.get()));
    columns.push_back(taken.make_array());
  }
  partition_uniques_.reserve(kPartitions);
  std::int64_t offset = 0;
  for (const std::vector<std::uint32_t>& groups : partition_groups_) {
    const auto length = static_cast<std::int64_t>(groups.size());
    std::vector<arrow::Datum> values;
    values.reserve(columns.size());
    for (const std::shared_ptr<arrow::Array>& column : columns) {
      values.emplace_back(column->Slice(offset, length));
    }
    partition_uniques_.emplace_back(std::move(values), length);
    offset += length;
  }
  return arrow::Status::OK();
}

arrow::Status GroupTable::MergePartition(const GroupTable& part, std::size_t partition) {
  if (partition >= part.partition_groups_.size()) {
    return arrow::Status::Invalid("merge of partition ", partition, " of a table with ",
                                  part.partition_groups_.size(), " partitions");
  }
  const std::vector<std::uint32_t>& groups = part.partition_groups_[partition];
  if (groups.empty()) {
    return arrow::Status::OK();
  }
  // The partition's groups of part (`groups`) and this table's groups they become (`to`): the
  // work is proportional to the partition, not to the part.
  std::vector<std::uint32_t> single;
  std::shared_ptr<arrow::UInt32Array> ids;
  std::span<const std::uint32_t> to;
  if (keys_.empty()) {
    if (num_groups_ == 0) {
      ARROW_RETURN_NOT_OK(AddGroups(1, {}));
    }
    single.assign(1, 0);
    to = single;
  } else {
    // The partition's groups of part, by their normalized keys, through this table's grouper.
    ARROW_ASSIGN_OR_RAISE(
        const arrow::Datum id_datum,
        grouper_->Consume(arrow::compute::ExecSpan(part.partition_uniques_[partition])));
    ids = std::static_pointer_cast<arrow::UInt32Array>(id_datum.make_array());
    to = IdsOf(*ids);
    const std::uint32_t after = grouper_->num_groups();
    if (after > num_groups_ && !keep_first_keys_) {
      ARROW_RETURN_NOT_OK(AddGroups(after, {}));
    } else if (after > num_groups_) {
      ARROW_RETURN_NOT_OK(AddPartGroups(part, groups, to, after));
    }
  }
  for (std::size_t i = 0; i < states_.size(); ++i) {
    ARROW_RETURN_NOT_OK(states_[i]->MergeGroups(*part.states_[i], groups, to));
  }
  return Account();
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
  ANTB1_CHECK(keep_first_keys_ || keys_.empty());
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
