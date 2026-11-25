#include "group_table.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
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
#include <arrow/util/byte_size.h>
#include <arrow/util/hashing.h>

#include "antb1/common/check.h"
#include "antb1/common/narrow.h"
#include "antb1/exec/grouped_aggregate_state.h"
#include "antb1/exec/memory_budget.h"
#include "antb1/plan/logical_plan.h"
#include "antb1/plan/types.h"

#include "double_key.h"
#include "gather.h"
#include "group_vector.h"
#include "grouped_sum_alias.h"
#include "key_routing_hash.h"

namespace antb1::exec {
namespace {

// For each group new since `before` (ids before .. after - 1), the first position in `ids` that
// maps to it: a key table may number new groups in an order of its own (arrow's Grouper does), so
// they are found per id.
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

// Mixes `value_of(row_at(row))` into every row's hash, or kNullGroupKeyHash where the column is
// null. The type of a column and the base of its values are decided once here, not per row, and a
// column without nulls skips the validity check: a row of a fixed-width key spent more of its time
// reaching its bytes (two indirections through the array's buffers) than hashing them.
template <typename ValueOf, typename RowAt>
void MixKeyColumn(const arrow::Array& column, const ValueOf& value_of, const RowAt& row_at,
                  std::span<std::uint64_t> hashes) {
  const auto rows = hashes.size();
  if (column.null_count() == 0) {
    for (std::size_t row = 0; row < rows; ++row) {
      hashes[row] = MixGroupKeyHash(hashes[row], value_of(row_at(row)));
    }
    return;
  }
  for (std::size_t row = 0; row < rows; ++row) {
    const std::uint64_t value = column.IsValid(static_cast<std::int64_t>(row_at(row)))
                                    ? value_of(row_at(row))
                                    : kNullGroupKeyHash;
    hashes[row] = MixGroupKeyHash(hashes[row], value);
  }
}

template <typename RowAt>
arrow::Status HashGroupRows(const arrow::compute::ExecBatch& keys, const RowAt& row_at,
                            std::span<std::uint64_t> hashes) {
  std::ranges::fill(hashes, kGroupKeySeed);
  for (const arrow::Datum& datum : keys.values) {
    const std::shared_ptr<arrow::Array> column = datum.make_array();
    const arrow::DataType& type = *column->type();
    const int width = type.byte_width();
    if (const auto* binary = dynamic_cast<const arrow::BinaryArray*>(column.get());
        binary != nullptr) {
      MixKeyColumn(
          *column,
          [binary](std::size_t row) {
            const std::string_view bytes = binary->GetView(static_cast<std::int64_t>(row));
            return arrow::internal::ComputeStringHash<0>(bytes.data(),
                                                         static_cast<std::int64_t>(bytes.size()));
          },
          row_at, hashes);
    } else if (type.id() == arrow::Type::BOOL) {
      const auto& bools = static_cast<const arrow::BooleanArray&>(*column);
      MixKeyColumn(
          *column,
          [&bools](std::size_t row) -> std::uint64_t {
            return bools.Value(static_cast<std::int64_t>(row)) ? 1 : 2;
          },
          row_at, hashes);
    } else if (width > 0) {
      // GetValues, not buffers[1]->data(): a fixed-width column of no rows, or one whose every row
      // is NULL, is allowed to carry no values buffer at all, and this base is taken once for the
      // column rather than inside the row loop. GetValues gives nullptr for that column and
      // `value_of` is never called on it -- no row of it is valid.
      const auto* base = column->data()->GetValues<std::uint8_t>(1, column->offset() * width);
      MixKeyColumn(
          *column,
          [base, width](std::size_t row) {
            return arrow::internal::ComputeStringHash<0>(
                base + (static_cast<std::ptrdiff_t>(row) * width), width);
          },
          row_at, hashes);
    } else {
      return arrow::Status::NotImplemented("GROUP BY partition of a ", type.ToString(), " key");
    }
  }
  for (std::uint64_t& hash : hashes) {
    hash = FinishGroupKeyHash(hash);
  }
  return arrow::Status::OK();
}

arrow::Result<std::vector<std::uint64_t>> RoutingHashes(const arrow::compute::ExecBatch& keys,
                                                        std::optional<std::size_t> key) {
  if (key && keys.values.size() > 1) {
    return KeyHashes(arrow::compute::ExecBatch({keys.values.at(*key)}, keys.length));
  }
  return KeyHashes(keys);
}

}  // namespace

arrow::Status KeyHashes(const arrow::compute::ExecBatch& keys, std::span<std::uint64_t> hashes) {
  ANTB1_DCHECK(Narrow<std::size_t>(keys.length) == hashes.size());
  return HashGroupRows(keys, std::identity{}, hashes);
}

arrow::Result<std::vector<std::uint64_t>> KeyHashes(const arrow::compute::ExecBatch& keys) {
  std::vector<std::uint64_t> hashes(Narrow<std::size_t>(keys.length));
  ARROW_RETURN_NOT_OK(KeyHashes(keys, hashes));
  return hashes;
}

arrow::Status SelectedGroupHashes(const arrow::compute::ExecBatch& keys,
                                  std::span<const std::uint32_t> rows,
                                  std::span<std::uint64_t> hashes) {
  ANTB1_DCHECK(rows.size() == hashes.size());
  return HashGroupRows(keys, [rows](std::size_t i) { return rows[i]; }, hashes);
}

GroupTable::GroupTable(std::vector<plan::BoundColumn> keys,
                       std::vector<plan::AggregateCall> aggregates, arrow::MemoryPool* pool,
                       bool first_keys)
    : keys_(std::move(keys)),
      aggregates_(std::move(aggregates)),
      pool_(pool),
      keep_first_keys_(first_keys) {}

GroupTable::~GroupTable() = default;

arrow::compute::ExecBatch ProjectKeys(const arrow::compute::ExecBatch& keys,
                                      std::span<const std::size_t> columns) {
  std::vector<arrow::Datum> values;
  values.reserve(columns.size());
  for (const std::size_t column : columns) {
    values.push_back(keys.values.at(column));
  }
  return {std::move(values), keys.length};
}

arrow::Result<KeyBuckets> SplitByKeys(const arrow::compute::ExecBatch& keys, std::size_t partitions,
                                      arrow::MemoryPool* pool,
                                      std::optional<std::size_t> routing_key) {
  ANTB1_CHECK(std::has_single_bit(partitions));
  KeyBuckets out;
  out.rows.resize(partitions);
  ARROW_ASSIGN_OR_RAISE(const std::vector<std::uint64_t> hashes, RoutingHashes(keys, routing_key));
  for (std::uint32_t row = 0; row < hashes.size(); ++row) {
    out.rows[static_cast<std::size_t>(hashes[row]) & (partitions - 1)].push_back(row);
  }
  // One Take per column with the rows in partition order, sliced per partition.
  arrow::UInt32Builder order(pool);
  ARROW_RETURN_NOT_OK(order.Reserve(keys.length));
  for (const std::vector<std::uint32_t>& rows : out.rows) {
    ARROW_RETURN_NOT_OK(order.AppendValues(rows));
  }
  ARROW_ASSIGN_OR_RAISE(const auto indices, order.Finish());
  arrow::compute::ExecContext kernels(pool);
  std::vector<std::shared_ptr<arrow::Array>> columns;
  columns.reserve(keys.values.size());
  for (const arrow::Datum& column : keys.values) {
    ARROW_ASSIGN_OR_RAISE(
        const arrow::Datum taken,
        arrow::compute::Take(column, indices, arrow::compute::TakeOptions::NoBoundsCheck(),
                             &kernels));
    columns.push_back(taken.make_array());
  }
  out.keys.reserve(partitions);
  std::int64_t offset = 0;
  for (const std::vector<std::uint32_t>& rows : out.rows) {
    const auto length = static_cast<std::int64_t>(rows.size());
    std::vector<arrow::Datum> values;
    values.reserve(columns.size());
    for (const std::shared_ptr<arrow::Array>& column : columns) {
      values.emplace_back(column->Slice(offset, length));
    }
    out.keys.emplace_back(std::move(values), length);
    offset += length;
  }
  return out;
}

arrow::Result<std::unique_ptr<GroupTable>> GroupTable::Make(
    std::vector<plan::BoundColumn> keys, std::vector<plan::AggregateCall> aggregates,
    int input_width, arrow::MemoryPool* pool, MemoryBudget* budget, bool first_keys,
    std::size_t partitions, const KeyTableOptions& key_table, GroupTableMode mode) {
  std::unique_ptr<GroupTable> table(
      new GroupTable(std::move(keys), std::move(aggregates), pool, first_keys));
  ANTB1_CHECK(std::has_single_bit(partitions));  // PartitionOf masks
  table->partitions_ = partitions;
  table->memory_.Reset(budget);
  std::vector<arrow::TypeHolder> key_types;
  for (const plan::BoundColumn& key : table->keys_) {
    if (key.index < 0 || key.index >= input_width) {
      return arrow::Status::Invalid("grouping key outside its input");
    }
    key_types.emplace_back(plan::ToArrow(key.type));
  }
  // A SUM and an AVG over one column hold the same sums and counts, so the second of them reads
  // the first's accumulator instead of adding every row again (grouped_sum_alias.h). The state is
  // still made first, so the result-type check every call gets does not move.
  struct Summed {
    int column = 0;
    plan::LogicalType type;
    std::size_t state = 0;
  };
  std::vector<Summed> summed;
  auto* const metadata = key_table.table_metadata;
  if (metadata != nullptr) {
    table->states_.reserve(table->aggregates_.size());
    const auto extra = table->states_.capacity() - table->aggregates_.size();
    ARROW_RETURN_NOT_OK(metadata->Resize(
        metadata->bytes() + Narrow<std::int64_t>(extra * sizeof(table->states_.front()))));
  }
  for (const plan::AggregateCall& call : table->aggregates_) {
    std::optional<plan::LogicalType> input;
    if (call.arg.has_value()) {
      if (call.arg->index < 0 || call.arg->index >= input_width) {
        return arrow::Status::Invalid("aggregate over a column outside its input");
      }
      input = call.arg->type;
    }
    const auto before = metadata == nullptr ? 0 : metadata->bytes();
    ARROW_ASSIGN_OR_RAISE(auto state,
                          MakeGroupedAggregateState(call.kind, input, call.type, pool, metadata));
    const bool sums = call.kind == plan::AggKind::kSum || call.kind == plan::AggKind::kAvg;
    if (sums && call.arg.has_value()) {
      const auto seen = std::ranges::find_if(summed, [&call](const Summed& other) {
        return other.column == call.arg->index && other.type == call.arg->type;
      });
      if (seen != summed.end()) {
        const auto owner_bytes = metadata == nullptr ? 0 : metadata->bytes() - before;
        ARROW_ASSIGN_OR_RAISE(auto alias, MakeSumAlias(*table->states_[seen->state],
                                                       call.kind == plan::AggKind::kAvg, metadata));
        if (alias != nullptr) {
          state = std::move(alias);
          if (metadata != nullptr) {
            ARROW_RETURN_NOT_OK(metadata->Resize(metadata->bytes() - owner_bytes));
          }
        }
      } else {
        summed.push_back(Summed{
            .column = call.arg->index, .type = call.arg->type, .state = table->states_.size()});
      }
    }
    table->states_.push_back(std::move(state));
  }
  table->kernels_ = std::make_unique<arrow::compute::ExecContext>(pool);
  if (!table->keys_.empty()) {
    ARROW_ASSIGN_OR_RAISE(table->key_table_,
                          MakeKeyTable(key_types, table->kernels_.get(), key_table, budget));
  }
  if (mode == GroupTableMode::kBatch) {
    if (auto* stored = dynamic_cast<StoredKeyTable*>(table->key_table_.get()); stored != nullptr) {
      ARROW_ASSIGN_OR_RAISE(
          auto program, GroupedAggregateProgram::Make(table->aggregates_, table->states_, pool));
      if (program.supports_updates()) {
        table->program_.emplace(std::move(program));
        if (budget == nullptr) {
          table->memory_.Reset(dynamic_cast<MemoryBudget*>(pool));
        }
        table->stored_keys_ = stored;
        // For fixed keys only DOUBLE can spell the same normalized key several ways.
        table->stored_output_ =
            first_keys && std::ranges::none_of(table->keys_, [](const auto& key) {
              return key.type == plan::LogicalType::kDouble;
            });
      }
    }
  }
  if (!table->program_) {
    table->first_keys_.resize(table->keys_.size());
  } else if (table->KeepsKeyChunks()) {
    ARROW_RETURN_NOT_OK(
        ReserveGroupVector(table->first_keys_, table->keys_.size(), table->memory_));
    table->first_keys_.resize(table->keys_.size());
  }
  return table;
}

arrow::Status GroupTable::AddGroups(std::uint32_t after,
                                    std::vector<std::shared_ptr<arrow::Array>> keys) {
  ARROW_RETURN_NOT_OK(ResizeStates(after));
  if (!program_.has_value() || KeepsKeyChunks()) {
    if (program_) {
      ARROW_RETURN_NOT_OK(ReserveGroupVector(chunk_ends_, chunk_ends_.size() + 1, memory_));
      for (auto& chunks : first_keys_) {
        ARROW_RETURN_NOT_OK(ReserveGroupVector(chunks, chunks.size() + 1, memory_));
      }
    }
    for (std::size_t k = 0; k < keys.size(); ++k) {
      first_keys_[k].push_back(std::move(keys[k]));
    }
    chunk_ends_.push_back(after);
  }
  num_groups_ = after;
  return arrow::Status::OK();
}

arrow::Status GroupTable::ResizeStates(std::uint32_t after) {
  if (program_.has_value()) {
    return program_->Resize(after, memory_);
  }
  for (const auto& state : states_) {
    state->Resize(after);
  }
  return arrow::Status::OK();
}

arrow::Result<std::shared_ptr<arrow::UInt32Array>> GroupTable::Lookup(
    const arrow::RecordBatch& rows) {
  ARROW_RETURN_NOT_OK(program_error_);
  if (frozen_) {
    return arrow::Status::Invalid("lookup on a frozen group table");
  }
  ANTB1_CHECK(!keys_.empty());
  ARROW_ASSIGN_OR_RAISE(auto keys, InputKeys(rows));
  return key_table_->Lookup(keys);
}

arrow::Result<arrow::compute::ExecBatch> GroupTable::InputKeys(const arrow::RecordBatch& rows,
                                                               bool normalize_doubles) const {
  std::vector<arrow::Datum> keys;
  keys.reserve(keys_.size());
  for (const plan::BoundColumn& key : keys_) {
    std::shared_ptr<arrow::Array> column = rows.column(key.index);
    if (normalize_doubles && key.type == plan::LogicalType::kDouble) {
      ARROW_ASSIGN_OR_RAISE(column, NormalizeDoubleKey(column, pool_));
    }
    keys.emplace_back(std::move(column));
  }
  return arrow::compute::ExecBatch(std::move(keys), rows.num_rows());
}

arrow::Status GroupTable::Consume(const arrow::RecordBatch& rows,
                                  std::vector<std::uint32_t>* group_ids_out) {
  return Update([&] { return ConsumeRows(rows, group_ids_out); });
}

arrow::Status GroupTable::ConsumeRows(const arrow::RecordBatch& rows,
                                      std::vector<std::uint32_t>* group_ids_out) {
  const std::int64_t n = rows.num_rows();
  std::vector<std::uint32_t> no_keys;
  KeyIds ids;  // holds the group ids until the states have consumed the rows
  std::span<const std::uint32_t> group_ids;
  if (keys_.empty()) {  // one group of every row
    if (num_groups_ == 0) {
      ARROW_RETURN_NOT_OK(AddGroups(1, {}));
    }
    no_keys.assign(static_cast<std::size_t>(n), 0);
    group_ids = no_keys;
  } else {
    ARROW_ASSIGN_OR_RAISE(const auto key_batch, InputKeys(rows));
    if (program_ && group_ids_out == nullptr) {
      return ConsumeProgram(rows, key_batch, {}, {});
    }
    ARROW_ASSIGN_OR_RAISE(ids, key_table_->Consume(key_batch));
    group_ids = ids.ids;
    const std::uint32_t after = ids.after;
    if (after > num_groups_ && !KeepsKeyChunks()) {
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
  if (group_ids_out != nullptr) {
    group_ids_out->assign(group_ids.begin(), group_ids.end());
  }
  return Account();
}

arrow::Status GroupTable::Merge(const GroupTable& part) {
  ARROW_RETURN_NOT_OK(part.program_error_);
  return Update([&] { return MergeTable(part); });
}

arrow::Status GroupTable::MergeTable(const GroupTable& part) {
  ANTB1_CHECK(keep_first_keys_);  // only the serial merges keep no first-seen keys
  if (part.num_groups_ == 0) {
    return arrow::Status::OK();
  }
  if (program_ && part.program_) {
    return MergeStoredChunks(part, std::nullopt);
  }
  std::vector<std::uint32_t> no_keys;
  KeyIds ids;
  std::span<const std::uint32_t> group_map;
  if (keys_.empty()) {
    if (num_groups_ == 0) {
      ARROW_RETURN_NOT_OK(AddGroups(1, {}));
    }
    no_keys.assign(1, 0);
    group_map = no_keys;
  } else {
    // Part's groups in its own order, by their normalized keys: the map from its groups to ours.
    ARROW_ASSIGN_OR_RAISE(const auto uniques, part.uniques());
    ARROW_ASSIGN_OR_RAISE(ids, key_table_->Consume(uniques.batch));
    group_map = ids.ids;
    const std::uint32_t after = ids.after;
    if (after > num_groups_ && stored_output_) {
      ARROW_RETURN_NOT_OK(AddGroups(after, {}));
    } else if (after > num_groups_ && !part.KeepsKeyChunks()) {
      ARROW_RETURN_NOT_OK(AddUniqueGroups(uniques.batch, group_map, after));
    } else if (after > num_groups_) {
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
  return AddPartKeyRows(part, source);
}

arrow::Status GroupTable::AddPartKeyRows(const GroupTable& part,
                                         std::span<const std::uint32_t> source) {
  // chunk_ends_ is built as groups arrive. Resolve only the requested IDs, once for all columns.
  // Consecutive groups in a source chunk reuse the previous lookup. A partition can skip chunks
  // and a backend can assign destination IDs out of source order.
  MemoryReservation scratch;
  scratch.Reset(memory_.budget());
  std::vector<GroupKeyRow> refs;
  ARROW_RETURN_NOT_OK(ReserveGroupVector(refs, std::min(source.size(), kMaxMergeChunk), scratch));
  std::uint32_t chunk = 0;
  std::uint32_t start = 0;
  std::uint32_t end = 0;
  while (!source.empty()) {
    const auto slice = source.first(std::min(kMaxMergeChunk, source.size()));
    refs.clear();
    for (const auto group : slice) {
      if (group < start || group >= end) {
        const auto pos = std::ranges::upper_bound(part.chunk_ends_, group);
        ANTB1_DCHECK(pos != part.chunk_ends_.end());
        chunk = Narrow<std::uint32_t>(pos - part.chunk_ends_.begin());
        start = chunk == 0 ? 0 : part.chunk_ends_[chunk - 1];
        end = *pos;
      }
      refs.push_back({.chunk = chunk, .row = group - start});
    }
    MemoryReservation columns_memory;
    columns_memory.Reset(memory_.budget());
    arrow::ArrayVector first;
    ARROW_RETURN_NOT_OK(ReserveGroupVector(first, keys_.size(), columns_memory));
    for (const auto& chunks : part.first_keys_) {
      ARROW_ASSIGN_OR_RAISE(auto keys, GatherGroupKeys(chunks, refs, pool_));
      first.push_back(std::move(keys));
    }
    ARROW_RETURN_NOT_OK(
        AddGroups(num_groups_ + Narrow<std::uint32_t>(slice.size()), std::move(first)));
    source = source.subspan(slice.size());
  }
  return arrow::Status::OK();
}

arrow::Status GroupTable::AddUniqueGroups(const arrow::compute::ExecBatch& keys,
                                          std::span<const std::uint32_t> to, std::uint32_t after) {
  // The row of `keys` each new group's keys are in, by group: a key table may number new groups in
  // an order of its own, not the order it first sees them, so they are placed per id as in Consume.
  // A row index, like a group, is a uint32: `keys` is one part's groups, or one partition of them.
  const auto groups = static_cast<std::size_t>(after - num_groups_);
  std::vector<std::uint32_t> positions(groups, 0);
  std::size_t found = 0;
  for (std::size_t i = 0; i < to.size(); ++i) {
    if (to[i] >= num_groups_) {
      positions[to[i] - num_groups_] = Narrow<std::uint32_t>(i);
      ++found;
    }
  }
  // Exactly one row per new group, since the keys of a part are unique: with as many rows found as
  // there are new groups, every position was written.
  ANTB1_CHECK(found == groups);
  return AddKeyRows(keys, positions);
}

arrow::Status GroupTable::AddKeyRows(const arrow::compute::ExecBatch& keys,
                                     std::span<const std::uint32_t> positions) {
  const std::size_t groups = positions.size();
  // One index array for every chunk, sliced rather than rebuilt: a chunk's Take reads its own rows
  // out of it and a slice allocates nothing.
  arrow::UInt32Builder indices(pool_);
  ARROW_RETURN_NOT_OK(indices.AppendValues(positions.data(), Narrow<std::int64_t>(groups)));
  ARROW_ASSIGN_OR_RAISE(const auto rows, indices.Finish());
  for (std::size_t begin = 0; begin < groups; begin += kMaxMergeChunk) {
    const std::size_t end = std::min(groups, begin + kMaxMergeChunk);
    const std::shared_ptr<arrow::Array> chunk_rows =
        rows->Slice(static_cast<std::int64_t>(begin), static_cast<std::int64_t>(end - begin));
    std::vector<std::shared_ptr<arrow::Array>> first;
    first.reserve(keys.values.size());
    for (const arrow::Datum& column : keys.values) {
      ARROW_ASSIGN_OR_RAISE(
          const arrow::Datum taken,
          arrow::compute::Take(column, chunk_rows, arrow::compute::TakeOptions::NoBoundsCheck(),
                               kernels_.get()));
      first.push_back(taken.make_array());
    }
    ARROW_RETURN_NOT_OK(
        AddGroups(num_groups_ + static_cast<std::uint32_t>(end - begin), std::move(first)));
  }
  return arrow::Status::OK();
}

arrow::Status GroupTable::MergeProjected(const GroupTable& source,
                                         const arrow::compute::ExecBatch& keys,
                                         std::span<const std::uint32_t> from) {
  ARROW_RETURN_NOT_OK(source.program_error_);
  return Update([&] { return MergeProjectedGroups(source, keys, from); });
}

arrow::Status GroupTable::MergeProjectedChunks(const GroupTable& source,
                                               std::span<const std::size_t> columns) {
  ANTB1_CHECK(source.KeepsKeyChunks());
  ANTB1_CHECK(source.next_group_ == 0);
  PoolVector<std::uint32_t> ids(pool_);
  arrow::compute::ExecBatch keys;
  keys.values.reserve(columns.size());
  std::uint32_t first = 0;
  for (std::size_t chunk = 0; chunk < source.chunk_ends_.size(); ++chunk) {
    const std::uint32_t end = source.chunk_ends_[chunk];
    ARROW_RETURN_NOT_OK(ids.Resize(end - first));
    const std::span groups(ids.data(), ids.size());
    std::ranges::iota(groups, first);
    keys.length = end - first;
    keys.values.clear();
    for (const auto column : columns) {
      keys.values.emplace_back(source.first_keys_[column][chunk]);
    }
    ARROW_RETURN_NOT_OK(MergeProjected(source, keys, groups));
    first = end;
  }
  return arrow::Status::OK();
}

arrow::Status GroupTable::MergeProjectedGroups(const GroupTable& source,
                                               const arrow::compute::ExecBatch& keys,
                                               std::span<const std::uint32_t> from) {
  ANTB1_CHECK(source.states_.size() == states_.size());
  if (from.empty()) {
    return arrow::Status::OK();
  }
  std::vector<std::uint32_t> no_keys;
  KeyIds ids;
  std::span<const std::uint32_t> to;
  if (keys_.empty()) {
    ARROW_RETURN_NOT_OK(EnsureGroup());
    no_keys.assign(from.size(), 0);
    to = no_keys;
  } else {
    ANTB1_CHECK(keys.values.size() == keys_.size());
    ANTB1_CHECK(std::cmp_equal(keys.length, from.size()));
    ANTB1_CHECK(keep_first_keys_);  // its groups are emitted: NextChunk reads the first keys
    ARROW_ASSIGN_OR_RAISE(ids, key_table_->Consume(keys));
    to = ids.ids;
    const std::uint32_t after = ids.after;
    if (after > num_groups_ && stored_output_) {
      ARROW_RETURN_NOT_OK(AddGroups(after, {}));
    } else if (after > num_groups_) {
      // Many source groups make one group here: its keys are the first such source group's.
      constexpr std::uint32_t kUnset = std::numeric_limits<std::uint32_t>::max();
      std::vector<std::uint32_t> positions(after - num_groups_, kUnset);
      std::size_t found = 0;
      for (std::size_t i = 0; i < to.size() && found < positions.size(); ++i) {
        const std::uint32_t id = to[i];
        if (id >= num_groups_ && positions[id - num_groups_] == kUnset) {
          positions[id - num_groups_] = Narrow<std::uint32_t>(i);
          ++found;
        }
      }
      ANTB1_CHECK(found == positions.size());
      ARROW_RETURN_NOT_OK(AddKeyRows(keys, positions));
    }
  }
  for (std::size_t i = 0; i < states_.size(); ++i) {
    ARROW_RETURN_NOT_OK(states_[i]->MergeGroups(*source.states_[i], from, to));
  }
  return Account();
}

arrow::Status GroupTable::EnsureGroup() {
  ANTB1_CHECK(keys_.empty());
  if (num_groups_ == 0) {
    ARROW_RETURN_NOT_OK(AddGroups(1, {}));
  }
  return Account();
}

arrow::Status GroupTable::Partition() {
  ARROW_RETURN_NOT_OK(program_error_);
  if (frozen_) {
    return arrow::Status::OK();
  }
  if (program_) {
    return KeyTableNoBadAlloc([&] { return FreezePartitions(); });
  }
  return Partition(keys_.size(), {});
}

arrow::Result<std::vector<std::uint64_t>> GroupTable::PrefixHashes(std::size_t prefix) const {
  ANTB1_CHECK(prefix <= keys_.size());
  ARROW_ASSIGN_OR_RAISE(auto keys, uniques());
  keys.batch.values.resize(prefix);
  return KeyHashes(keys.batch);
}

arrow::Result<std::vector<std::shared_ptr<arrow::RecordBatch>>> GroupTable::RouteRows(
    const arrow::RecordBatch& rows) const {
  ANTB1_CHECK(!keys_.empty());
  ARROW_ASSIGN_OR_RAISE(const auto keys, InputKeys(rows));
  ARROW_ASSIGN_OR_RAISE(const auto hashes, RoutingHashes(keys, routing_key_));
  // The rows in partition order (a stable counting sort), one Take, then a slice per partition.
  std::vector<std::int64_t> start(partitions_ + 1, 0);
  for (const std::uint64_t hash : hashes) {
    ++start[PartitionOf(hash) + 1];
  }
  for (std::size_t p = 1; p <= partitions_; ++p) {
    start[p] += start[p - 1];
  }
  std::vector<std::int64_t> next(start.begin(), start.end() - 1);
  std::vector<std::int64_t> order(hashes.size());
  for (std::size_t r = 0; r < hashes.size(); ++r) {
    order[static_cast<std::size_t>(next[PartitionOf(hashes[r])]++)] = static_cast<std::int64_t>(r);
  }
  arrow::Int64Builder indices(pool_);
  ARROW_RETURN_NOT_OK(indices.AppendValues(order));
  ARROW_ASSIGN_OR_RAISE(const auto positions, indices.Finish());
  arrow::ArrayVector columns;
  columns.reserve(static_cast<std::size_t>(rows.num_columns()));
  for (const auto& column : rows.columns()) {
    ARROW_ASSIGN_OR_RAISE(
        const arrow::Datum taken,
        arrow::compute::Take(column, positions, arrow::compute::TakeOptions::NoBoundsCheck(),
                             kernels_.get()));
    columns.push_back(taken.make_array());
  }
  const std::shared_ptr<arrow::RecordBatch> sorted =
      arrow::RecordBatch::Make(rows.schema(), rows.num_rows(), std::move(columns));
  std::vector<std::shared_ptr<arrow::RecordBatch>> out;
  out.reserve(partitions_);
  for (std::size_t p = 0; p < partitions_; ++p) {
    out.push_back(sorted->Slice(start[p], start[p + 1] - start[p]));
  }
  return out;
}

arrow::Result<KeySnapshot> GroupTable::uniques() const {
  ARROW_RETURN_NOT_OK(program_error_);
  if (frozen_) {
    return frozen_->keys->StoredUniques(0, num_groups_);
  }
  ANTB1_CHECK(key_table_ != nullptr);
  return key_table_->Uniques();
}

arrow::Status GroupTable::Partition(std::size_t prefix, std::span<const std::uint64_t> heavy) {
  ARROW_RETURN_NOT_OK(program_error_);
  if (frozen_) {
    return arrow::Status::Invalid("repartition of a frozen group table");
  }
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
  partition_groups_.resize(partitions_);
  ARROW_ASSIGN_OR_RAISE(const auto snapshot, key_table_->Uniques());
  const auto& uniques = snapshot.batch;
  arrow::compute::ExecBatch prefix_keys = uniques;
  prefix_keys.values.resize(prefix);
  ARROW_ASSIGN_OR_RAISE(std::vector<std::uint64_t> hashes,
                        RoutingHashes(prefix_keys, routing_key_));
  if (prefix < keys_.size() && !heavy.empty()) {
    ARROW_ASSIGN_OR_RAISE(const std::vector<std::uint64_t> all, KeyHashes(uniques));
    for (std::size_t g = 0; g < hashes.size(); ++g) {
      if (std::ranges::binary_search(heavy, hashes[g])) {
        hashes[g] = all[g];
      }
    }
  }
  for (std::uint32_t g = 0; g < hashes.size(); ++g) {
    partition_groups_[PartitionOf(hashes[g])].push_back(g);
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
  partition_uniques_.reserve(partitions_);
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
  ARROW_RETURN_NOT_OK(part.program_error_);
  return Update([&] { return MergePartitionGroups(part, partition); });
}

arrow::Status GroupTable::MergePartitionGroups(const GroupTable& part, std::size_t partition) {
  if (partition >= part.num_partitions()) {
    return arrow::Status::Invalid("merge of partition ", partition, " of a table with ",
                                  part.num_partitions(), " partitions");
  }
  const auto groups = part.partition_groups(partition);
  if (groups.empty()) {
    return arrow::Status::OK();
  }
  if (program_ && part.frozen_) {
    return local_metadata_ ? MergeStoredChunks(part, groups) : MergeStoredGroups(part, groups);
  }
  // The partition's groups of part (`groups`) and this table's groups they become (`to`): the
  // work is proportional to the partition, not to the part.
  std::vector<std::uint32_t> single;
  KeyIds ids;
  std::span<const std::uint32_t> to;
  if (keys_.empty()) {
    if (num_groups_ == 0) {
      ARROW_RETURN_NOT_OK(AddGroups(1, {}));
    }
    single.assign(1, 0);
    to = single;
  } else {
    // The partition's groups of part, by their normalized keys, through this table's key table.
    // An explicitly different backend can merge a frozen producer as well.
    std::optional<KeySnapshot> gathered;
    if (part.frozen_) {
      ARROW_ASSIGN_OR_RAISE(auto snapshot, part.frozen_->keys->GatherStored(groups));
      gathered.emplace(std::move(snapshot));
    }
    const auto& keys = gathered ? gathered->batch : part.partition_uniques_[partition];
    ARROW_ASSIGN_OR_RAISE(ids, key_table_->Consume(keys));
    to = ids.ids;
    const std::uint32_t after = ids.after;
    if (after > num_groups_ && !KeepsKeyChunks()) {
      ARROW_RETURN_NOT_OK(AddGroups(after, {}));
    } else if (after > num_groups_ && !part.KeepsKeyChunks()) {
      ARROW_RETURN_NOT_OK(AddUniqueGroups(keys, to, after));
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
  if (program_) {
    // ReserveGroupVector already charged these capacities, including each growth overlap.
    return arrow::Status::OK();
  }
  auto bytes = Narrow<std::int64_t>(chunk_ends_.capacity() * sizeof(std::uint32_t));
  for (const auto& state : states_) {
    bytes += state->memory_usage();
  }
  return memory_.Resize(bytes);
}

arrow::Status GroupTable::CheckFinalize() const {
  ARROW_RETURN_NOT_OK(program_error_);
  for (const auto& state : states_) {
    ARROW_RETURN_NOT_OK(state->CheckFinalize());
  }
  return arrow::Status::OK();
}

arrow::Result<std::shared_ptr<arrow::RecordBatch>> GroupTable::NextChunk(
    const std::shared_ptr<arrow::Schema>& schema) {
  ARROW_RETURN_NOT_OK(program_error_);
  ANTB1_CHECK(keep_first_keys_ || keys_.empty());
  if (program_) {
    return KeyTableNoBadAlloc([&] { return NextOutputChunk(schema); });
  }
  return NextOutputChunk(schema);
}

std::uint32_t GroupTable::OutputRows(std::size_t& end) const {
  if (stored_output_) {
    std::size_t bytes_per_key = 0;
    for (const auto& key : keys_) {
      bytes_per_key += 1 + Narrow<std::size_t>(std::max(1, plan::ToArrow(key.type)->byte_width()));
    }
    const auto limit =
        std::max<std::size_t>(1, Narrow<std::size_t>(kMaxJoinedKeyBytes) / bytes_per_key);
    return Narrow<std::uint32_t>(
        std::min<std::size_t>({num_groups_ - next_group_, kMaxMergeChunk, limit}));
  }
  // Consecutive chunks together, up to kMaxMergeChunk groups and kMaxJoinedKeyBytes of keys (chunks
  // of a few groups each come from rows consumed a few at a time): their keys joined, every state
  // finalized once over the range. The byte bound keeps a joined VARCHAR key far from the 2 GiB of
  // one binary array.
  std::uint32_t rows = 0;
  int64_t bytes = 0;
  while (end < chunk_ends_.size()) {
    int64_t chunk_bytes = 0;
    for (const auto& chunks : first_keys_) {
      chunk_bytes += arrow::util::TotalBufferSize(*chunks[end]);
    }
    const auto through_chunk = chunk_ends_[end] - next_group_;
    if (end > next_chunk_ &&
        (through_chunk > kMaxMergeChunk || bytes + chunk_bytes > kMaxJoinedKeyBytes)) {
      break;
    }
    rows = through_chunk;
    bytes += chunk_bytes;
    ++end;
  }
  return rows;
}

arrow::Status GroupTable::AppendFirstKeyColumns(std::size_t end, arrow::ArrayVector& columns) {
  const bool retain = program_.has_value() || frozen_.has_value();
  for (auto& chunks : first_keys_) {
    if (end - next_chunk_ == 1) {
      columns.push_back(retain ? chunks[next_chunk_] : std::move(chunks[next_chunk_]));
      continue;
    }
    const auto begin_chunk = chunks.begin() + static_cast<std::ptrdiff_t>(next_chunk_);
    const auto end_chunk = chunks.begin() + static_cast<std::ptrdiff_t>(end);
    MemoryReservation piece_memory;
    arrow::ArrayVector pieces;
    if (program_) {
      // A failed concatenate or finalizer must leave every first-seen spelling available to retry.
      piece_memory.Reset(memory_.budget());
      ARROW_RETURN_NOT_OK(ReserveGroupVector(pieces, end - next_chunk_, piece_memory));
    }
    if (retain) {
      pieces.assign(begin_chunk, end_chunk);
    } else {
      pieces.assign(std::make_move_iterator(begin_chunk), std::make_move_iterator(end_chunk));
    }
    ARROW_ASSIGN_OR_RAISE(auto joined, arrow::Concatenate(pieces, pool_));
    columns.push_back(std::move(joined));
  }
  return arrow::Status::OK();
}

}  // namespace antb1::exec
