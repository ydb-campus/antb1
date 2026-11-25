#include "union_distinct.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <numeric>
#include <utility>
#include <vector>

#include <arrow/api.h>
#include <arrow/compute/api_vector.h>

#include "antb1/common/narrow.h"
#include "antb1/exec/profile.h"

#include "double_key.h"
#include "group_table.h"
#include "partition_lanes.h"
#include "pool_vector.h"

namespace antb1::exec {
namespace {
constexpr std::size_t kMaxLanes = 16;
constexpr std::uint32_t kFirst = 1U << 31U;
constexpr int64_t kTileRows = 32'768;
constexpr int64_t kWindowRows = 16 * kTileRows;
arrow::Result<std::unique_ptr<arrow::Buffer>> Hashes(const arrow::compute::ExecBatch& keys,
                                                     arrow::MemoryPool* pool) {
  ARROW_ASSIGN_OR_RAISE(
      auto buffer,
      arrow::AllocateBuffer(keys.length * static_cast<int64_t>(sizeof(std::uint64_t)), pool));
  ARROW_RETURN_NOT_OK(KeyHashes(
      keys, std::span(buffer->mutable_data_as<std::uint64_t>(), Narrow<std::size_t>(keys.length))));
  return buffer;
}
}  // namespace

// Index a union's retained rows by hash. Equal hashes still compare the complete row.
// Each worker owns one directory. Dense entries let a window translate its newly inserted
// locations to the compact output without probing a second time.
class UnionDistinct::Index {
 public:
  struct Entry {
    std::uint64_t hash;
    std::uint32_t batch;
    std::uint32_t row;
  };
  explicit Index(arrow::MemoryPool* pool) : pool_(pool), slots_(pool), entries_(pool) {}
  std::size_t size() const { return entries_.size(); }
  void Prefetch(std::uint64_t hash) const {
    if (!slots_.empty()) {
      __builtin_prefetch(slots_.data() + (static_cast<std::size_t>(hash) & (slots_.size() - 1)));
    }
  }
  Entry& entry(std::size_t id) { return entries_[id]; }

  template <class Equal>
  arrow::Result<bool> Insert(Entry value, const Equal& equal) {
    if (slots_.empty()) {
      ARROW_RETURN_NOT_OK(slots_.Assign(256, 0));
    }
    const auto mask = slots_.size() - 1;
    auto slot = static_cast<std::size_t>(value.hash) & mask;
    const auto tag = value.hash & kTagMask;
    while (const auto packed = slots_[slot]) {
      if ((packed & kTagMask) == tag) {
        const auto& previous = entries_[static_cast<std::uint32_t>(packed) - 1];
        if (value.hash == previous.hash && equal(previous)) {
          return false;
        }
      }
      slot = (slot + 1) & mask;
    }
    if (entries_.size() >= kMaxKeyTableGroups) {
      return arrow::Status::OutOfMemory("too many distinct rows in a UNION partition");
    }
    if (entries_.size() == entries_.capacity()) {
      ARROW_RETURN_NOT_OK(entries_.Reserve(std::max<std::size_t>(256, 2 * entries_.size())));
    }
    if (entries_.size() >= slots_.size() - (slots_.size() / 4)) {
      PoolVector<std::uint64_t> larger(pool_);
      ARROW_RETURN_NOT_OK(larger.Assign(2 * slots_.size(), 0));
      for (std::size_t i = 0; i < entries_.size(); ++i) {
        larger[Vacant(entries_[i].hash, larger)] = (entries_[i].hash & kTagMask) | (i + 1);
      }
      slots_ = std::move(larger);
      slot = Vacant(value.hash, slots_);
    }
    entries_.push_back(value);
    slots_[slot] = tag | entries_.size();
    return true;
  }

 private:
  static std::size_t Vacant(std::uint64_t hash, const PoolVector<std::uint64_t>& slots) {
    const auto mask = slots.size() - 1;
    auto at = static_cast<std::size_t>(hash) & mask;
    while (slots[at] != 0) {
      at = (at + 1) & mask;
    }
    return at;
  }
  arrow::MemoryPool* pool_;
  static constexpr std::uint64_t kTagMask = 0xffffffff00000000ULL;
  PoolVector<std::uint64_t> slots_;
  PoolVector<Entry> entries_;
};

struct UnionDistinct::Piece {
  std::shared_ptr<arrow::RecordBatch> rows;
  arrow::RecordBatchVector fragments;
  int64_t length = 0;
  std::unique_ptr<arrow::Buffer> hashes;
  std::array<std::uint32_t, kMaxLanes + 1> offsets{};
  std::unique_ptr<arrow::Buffer> order;
  std::unique_ptr<arrow::Buffer> ranks;
  std::uint32_t output = 0;
};

UnionDistinct::~UnionDistinct() = default;

UnionDistinct::UnionDistinct(const ExecContext& ctx)
    : ctx_(ctx),
      kernels_(ctx.pool),
      output_(arrow::stl::allocator<std::shared_ptr<arrow::RecordBatch>>(ctx.pool)) {}

arrow::Status UnionDistinct::Append(const std::shared_ptr<arrow::RecordBatch>& rows) {
  for (int64_t start = 0; start < rows->num_rows();) {
    if (lanes_.empty()) {
      // Observe actual distinct rows before paying to repartition. A union running inside an
      // executor task must consume serially: nested waits could exhaust the shared executor.
      if (groups_ < kTileRows / 2 || ctx_.threads <= 1 || ctx_.executor == nullptr ||
          ctx_.executor->IsCurrentExecutor()) {
        const auto count = std::min(kTileRows, rows->num_rows() - start);
        ARROW_RETURN_NOT_OK(ConsumeSerial(rows->Slice(start, count)));
        start += count;
        continue;
      }
      ARROW_RETURN_NOT_OK(SeedLanes());
    }
    if (pending_.empty() || pending_.back().length == kTileRows ||
        pending_.back().fragments.size() >= 64) {
      pending_.emplace_back();
    }
    auto& piece = pending_.back();
    const auto count = std::min(kTileRows - piece.length, rows->num_rows() - start);
    piece.fragments.push_back(rows->Slice(start, count));
    start += count;
    piece.length += count;
    pending_rows_ += count;
    if (pending_rows_ >= kWindowRows || pending_.size() >= 64 ||
        (ctx_.budget != nullptr && ctx_.budget->under_pressure())) {
      ARROW_RETURN_NOT_OK(Flush());
    }
  }
  return arrow::Status::OK();
}

arrow::Status UnionDistinct::Flush() {
  if (pending_.empty()) {
    return arrow::Status::OK();
  }
  if (output_.size() > std::numeric_limits<std::uint32_t>::max() - pending_.size()) {
    return arrow::Status::OutOfMemory("too many batches of distinct UNION rows");
  }
  auto* executor = ctx_.executor;
  ARROW_RETURN_NOT_OK(ForEach(executor, pending_.size(), [&](std::size_t i) {
    auto& piece = pending_[i];
    if (piece.fragments.size() == 1) {
      piece.rows = std::move(piece.fragments.front());
    } else {
      ARROW_ASSIGN_OR_RAISE(piece.rows,
                            arrow::ConcatenateRecordBatches(piece.fragments, ctx_.pool));
    }
    piece.fragments.clear();
    // Box columns before more than one worker reads the batch.
    (void)piece.rows->columns();
    return arrow::Status::OK();
  }));
  ARROW_RETURN_NOT_OK(
      ForEach(executor, pending_.size(), [&](std::size_t i) { return Route(pending_[i]); }));
  std::array<std::size_t, kMaxLanes> before{};
  ARROW_RETURN_NOT_OK(ForEach(executor, lanes_.size(), [&](std::size_t lane) {
    before[lane] = lanes_[lane].size();
    return Insert(lane);
  }));
  ARROW_RETURN_NOT_OK(
      ForEach(executor, pending_.size(), [&](std::size_t i) { return Compact(pending_[i]); }));
  const auto base = output_.size();
  for (auto& piece : pending_) {
    if (piece.rows != nullptr) {
      piece.output = Narrow<std::uint32_t>(output_.size());
      groups_ += piece.rows->num_rows();
      output_.push_back(std::move(piece.rows));
    }
  }
  ARROW_RETURN_NOT_OK(ForEach(executor, lanes_.size(), [&](std::size_t lane) {
    auto& index = lanes_[lane];
    const auto end = index.size();
    for (auto i = before[lane]; i < end; ++i) {
      auto& ref = index.entry(i);
      const auto& piece = pending_[ref.batch - base];
      ref.batch = piece.output;
      ref.row = piece.ranks->data_as<std::uint32_t>()[ref.row];
    }
    return arrow::Status::OK();
  }));
  pending_.clear();
  pending_rows_ = 0;
  return arrow::Status::OK();
}

void UnionDistinct::Finish(ProfileNode* profile) {
  if (profile != nullptr) {
    profile->Add("groups", MetricUnit::kCount, groups_);
    profile->Add("distinct_lanes", MetricUnit::kCount,
                 Narrow<int64_t>(serial_ != nullptr ? 1U : lanes_.size()));
  }
  lanes_.clear();
  serial_.reset();
}
Batch UnionDistinct::Next() {
  if (next_ == output_.size()) {
    return {};
  }
  return {.data = std::move(output_[next_++]), .selection = nullptr};
}

std::size_t UnionDistinct::Lane(std::uint64_t hash) const {
  return (hash >> 32U) & (lanes_.size() - 1);
}
arrow::Status UnionDistinct::ConsumeSerial(const std::shared_ptr<arrow::RecordBatch>& rows) {
  ARROW_ASSIGN_OR_RAISE(auto keys, Keys(*rows));
  if (serial_ == nullptr) {
    ARROW_ASSIGN_OR_RAISE(serial_, MakeKeyTable(keys.GetTypes(), &kernels_, {}, ctx_.budget));
  }
  ARROW_ASSIGN_OR_RAISE(const auto ids, serial_->Consume(keys));
  const auto added = ids.after - ids.before;
  if (added == 0) {
    return arrow::Status::OK();
  }
  arrow::UInt32Builder selected(ctx_.pool);
  ARROW_RETURN_NOT_OK(selected.Reserve(added));
  ARROW_ASSIGN_OR_RAISE(auto seen, arrow::AllocateEmptyBitmap(added, ctx_.pool));
  for (std::uint32_t row = 0; row < ids.ids.size(); ++row) {
    const auto id = ids.ids[row];
    if (id >= ids.before && !arrow::bit_util::GetBit(seen->data(), id - ids.before)) {
      arrow::bit_util::SetBit(seen->mutable_data(), id - ids.before);
      selected.UnsafeAppend(row);
    }
  }
  ARROW_ASSIGN_OR_RAISE(const auto positions, selected.Finish());
  ARROW_ASSIGN_OR_RAISE(
      const auto taken,
      arrow::compute::Take(rows, positions, arrow::compute::TakeOptions::NoBoundsCheck(),
                           &kernels_));
  (void)taken.record_batch()->columns();
  output_.push_back(taken.record_batch());
  groups_ += added;
  return arrow::Status::OK();
}

arrow::Status UnionDistinct::SeedLanes() {
  // Reuse the first occurrences already retained for output. Release the serial hash table
  // before allocating its replacements under the same query memory limit.
  serial_.reset();
  const auto count = std::bit_ceil(std::min(kMaxLanes, Narrow<std::size_t>(ctx_.threads)));
  for (std::size_t i = 0; i < count; ++i) {
    lanes_.emplace_back(ctx_.pool);
  }
  for (std::size_t batch = 0; batch < output_.size(); ++batch) {
    ARROW_ASSIGN_OR_RAISE(const auto keys, Keys(*output_[batch]));
    ARROW_ASSIGN_OR_RAISE(const auto hashes, Hashes(keys, ctx_.pool));
    const auto count_rows = Narrow<std::uint32_t>(keys.length);
    for (std::uint32_t row = 0; row < count_rows; ++row) {
      const Index::Entry ref{.hash = hashes->data_as<std::uint64_t>()[row],
                             .batch = Narrow<std::uint32_t>(batch),
                             .row = row};
      ARROW_RETURN_NOT_OK(
          lanes_[Lane(ref.hash)].Insert(ref, [](const auto&) { return false; }).status());
    }
  }
  return arrow::Status::OK();
}

arrow::Result<arrow::compute::ExecBatch> UnionDistinct::Keys(const arrow::RecordBatch& rows) const {
  arrow::compute::ExecBatch keys(rows);
  for (auto& key : keys.values) {
    if (key.type()->id() == arrow::Type::DOUBLE) {
      ARROW_ASSIGN_OR_RAISE(auto normalized, NormalizeDoubleKey(key.make_array(), ctx_.pool));
      key = normalized;
    }
  }
  return keys;
}

arrow::Status UnionDistinct::Route(Piece& piece) {
  ARROW_ASSIGN_OR_RAISE(const auto keys, Keys(*piece.rows));
  ARROW_ASSIGN_OR_RAISE(piece.hashes, Hashes(keys, ctx_.pool));
  const auto hashes =
      std::span(piece.hashes->data_as<std::uint64_t>(), Narrow<std::size_t>(keys.length));
  const auto bytes = piece.rows->num_rows() * static_cast<int64_t>(sizeof(std::uint32_t));
  ARROW_ASSIGN_OR_RAISE(piece.ranks, arrow::AllocateBuffer(bytes, ctx_.pool));
  for (const auto hash : hashes) {
    ++piece.offsets[Lane(hash) + 1];
  }
  std::partial_sum(piece.offsets.begin(), piece.offsets.end(), piece.offsets.begin());
  auto next = piece.offsets;
  ARROW_ASSIGN_OR_RAISE(piece.order, arrow::AllocateBuffer(bytes, ctx_.pool));
  auto* order = piece.order->mutable_data_as<std::uint32_t>();
  for (std::uint32_t i = 0; i < hashes.size(); ++i) {
    order[next[Lane(hashes[i])]++] = i;
  }
  return arrow::Status::OK();
}

arrow::Status UnionDistinct::Insert(std::size_t lane) {
  const auto base = output_.size();
  auto& index = lanes_[lane];
  for (std::size_t j = 0; j < pending_.size(); ++j) {
    auto& piece = pending_[j];
    const auto& columns = piece.rows->columns();
    const auto batch_id = Narrow<std::uint32_t>(base + j);
    const auto* hashes = piece.hashes->data_as<std::uint64_t>();
    auto* order = piece.order->mutable_data_as<std::uint32_t>();
    for (auto i = piece.offsets[lane]; i < piece.offsets[lane + 1]; ++i) {
      constexpr std::uint32_t kPrefetch = 16;
      if (i + kPrefetch < piece.offsets[lane + 1]) {
        index.Prefetch(hashes[order[i + kPrefetch]]);
      }
      const auto row = order[i];
      const auto equal = [&](const Index::Entry& ref) {
        const auto& other =
            ref.batch < base ? *output_[ref.batch] : *pending_[ref.batch - base].rows;
        const auto options =
            arrow::EqualOptions::Defaults().nans_equal(true).signed_zeros_equal(true);
        const auto& other_columns = other.columns();
        for (std::size_t c = 0; c < columns.size(); ++c) {
          if (!columns[c]->RangeEquals(row, row + 1, ref.row, *other_columns[c], options)) {
            return false;
          }
        }
        return true;
      };
      ARROW_ASSIGN_OR_RAISE(
          const bool first,
          index.Insert({.hash = hashes[row], .batch = batch_id, .row = row}, equal));
      if (first) {
        order[i] |= kFirst;
      }
    }
  }
  return arrow::Status::OK();
}

arrow::Status UnionDistinct::Compact(Piece& piece) const {
  arrow::UInt32Builder selected(ctx_.pool);
  ARROW_RETURN_NOT_OK(selected.Reserve(piece.rows->num_rows()));
  auto* flags = piece.ranks->mutable_data_as<std::uint32_t>();
  const auto* order = piece.order->data_as<std::uint32_t>();
  for (int64_t i = 0; i < piece.length; ++i) {
    flags[order[i] & ~kFirst] = order[i] >> 31U;
  }
  for (std::uint32_t row = 0; row < piece.rows->num_rows(); ++row) {
    if (flags[row] != 0) {
      flags[row] = Narrow<std::uint32_t>(selected.length());
      selected.UnsafeAppend(row);
    }
  }
  if (selected.length() == 0) {
    piece.rows.reset();
  } else {
    ARROW_ASSIGN_OR_RAISE(const auto positions, selected.Finish());
    arrow::compute::ExecContext kernels(ctx_.pool);
    ARROW_ASSIGN_OR_RAISE(
        const auto taken,
        arrow::compute::Take(piece.rows, positions, arrow::compute::TakeOptions::NoBoundsCheck(),
                             &kernels));
    piece.rows = taken.record_batch();
    (void)piece.rows->columns();
  }
  return arrow::Status::OK();
}

}  // namespace antb1::exec
