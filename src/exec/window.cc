#include "window.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <numeric>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include <arrow/api.h>
#include <arrow/util/byte_size.h>

#include "antb1/common/narrow.h"
#include "antb1/exec/memory_budget.h"
#include "antb1/exec/profile.h"
#include "antb1/exec/sort.h"
#include "antb1/plan/sql_status.h"
#include "antb1/plan/types.h"

#include "gather.h"
#include "group_table.h"
#include "packed_order.h"
#include "partition_lanes.h"
#include "pool_vector.h"
#include "window_prefix.h"

namespace antb1::exec {
namespace {

using RowRef = SortBuffer::RowRef;
constexpr std::uint32_t kChunkRows = 64 * 1024;

arrow::Status Charge(MemoryReservation& memory, std::size_t count, std::size_t width) {
  const auto remaining =
      static_cast<std::size_t>(std::numeric_limits<int64_t>::max() - memory.bytes());
  if (count > remaining / width) {
    return arrow::Status::CapacityError("window container size overflow");
  }
  return memory.Resize(memory.bytes() + Narrow<int64_t>(count * width));
}

// Charge container growth before allocating it, including the brief coexistence of the old
// and new storage. Capacity is implementation-defined, so account for the actual allocation too.
template <class T>
arrow::Status Reserve(std::vector<T>& values, std::size_t size, MemoryReservation& memory) {
  if (size <= values.capacity()) {
    return arrow::Status::OK();
  }
  const std::size_t old = values.capacity();
  const std::size_t capacity = std::max(size, old + (old / 2));
  ARROW_RETURN_NOT_OK(Charge(memory, capacity, sizeof(T)));
  values.reserve(capacity);
  ARROW_RETURN_NOT_OK(Charge(memory, values.capacity() - capacity, sizeof(T)));
  return memory.Resize(memory.bytes() - Narrow<int64_t>(old * sizeof(T)));
}

bool SamePartition(const plan::WindowCall& a, const plan::WindowCall& b) {
  return std::ranges::equal(a.partition_by, b.partition_by,
                            [](const plan::BoundColumn& x, const plan::BoundColumn& y) {
                              return x.index == y.index && x.type == y.type;
                            });
}

bool SameSpecification(const plan::WindowCall& a, const plan::WindowCall& b) {
  const auto whole = [](const plan::WindowCall& call) {
    return call.frame == plan::WindowCall::Frame::kWholePartition;
  };
  return whole(a) == whole(b) && SamePartition(a, b) &&
         std::ranges::equal(a.order_by, b.order_by, [](const auto& x, const auto& y) {
           return x.column.index == y.column.index && x.column.type == y.column.type &&
                  x.descending == y.descending && x.nulls_first == y.nulls_first;
         });
}

// Arrow 26's ArrayData copy constructor is noexcept although it copies vectors. Array::Slice
// and RecordBatch::Slice use that constructor, terminating on a heap failure. Copy the buffer
// pointers as arguments to Make instead, where a failure can reach Next's error boundary.
arrow::Result<std::shared_ptr<arrow::Array>> SliceColumn(const arrow::Array& array, int64_t offset,
                                                         int64_t length) {
  const auto& data = *array.data();
  if (offset > std::numeric_limits<int64_t>::max() - data.offset) {
    return arrow::Status::CapacityError("window array offset overflow");
  }
  return arrow::MakeArray(arrow::ArrayData::Make(
      data.type, length, data.buffers, data.child_data, data.dictionary,
      array.null_count() == 0 ? 0 : arrow::kUnknownNullCount, data.offset + offset));
}

arrow::Result<std::shared_ptr<arrow::RecordBatch>> SliceBatch(
    const std::shared_ptr<arrow::RecordBatch>& batch, int64_t start, int64_t length) {
  length = std::min(length, batch->num_rows() - start);
  if (start == 0 && length == batch->num_rows()) {
    return batch;
  }
  arrow::ArrayVector columns;
  columns.reserve(static_cast<std::size_t>(batch->num_columns()));
  for (const auto& column : batch->columns()) {
    ARROW_ASSIGN_OR_RAISE(auto slice, SliceColumn(*column, start, length));
    columns.push_back(std::move(slice));
  }
  return arrow::RecordBatch::Make(batch->schema(), length, std::move(columns));
}

struct Results {
  std::vector<std::shared_ptr<arrow::Array>> chunks;
  std::vector<const arrow::Array*> arrays;
};

struct Specification {
  explicit Specification(arrow::MemoryPool* pool)
      : ids(pool), groups(pool), positions(pool), direct_first(pool) {}
  std::vector<std::size_t> calls;
  PoolVector<std::uint32_t> ids;  // input row -> group
  PoolVector<RowRef> groups;      // group -> finalized chunk/row
  PoolVector<RowRef> positions;   // input row -> running result chunk/row
  int64_t packed_rows = 0;
  std::vector<Results> results;
  std::vector<arrow::ArrayVector> direct;
  PoolVector<std::size_t> direct_first;
  std::shared_ptr<arrow::Array> ranks;  // shared by identical RANK calls, in input row order
};

struct DirectColumn {
  struct Storage {
    std::shared_ptr<arrow::Buffer> values;
    std::shared_ptr<arrow::Buffer> validity;
    int64_t size;
  };
  DirectColumn(const plan::WindowCall& value, arrow::MemoryPool* pool)
      : call(value), arguments(pool), first(pool), pieces(pool) {}
  const plan::WindowCall& call;
  PoolVector<const arrow::Array*> arguments;
  PoolVector<std::size_t> first;
  PoolVector<WindowPrefixPiece> pieces;
  std::vector<Storage> buffers;
  std::size_t width = 0;
  std::size_t batch_rows = 0;
};

arrow::Result<std::vector<DirectColumn>> MakeDirectColumns(
    const Specification& spec, const std::vector<plan::WindowCall>& calls,
    const arrow::RecordBatchVector& input, const ExecContext& ctx, MemoryReservation& scratch) {
  std::vector<DirectColumn> result;
  ARROW_RETURN_NOT_OK(Reserve(result, spec.calls.size(), scratch));
  for (auto index : spec.calls) {
    const auto& call = calls[index];
    const auto type = plan::ToArrow(call.type);
    if (call.frame != plan::WindowCall::Frame::kRowsPrefix || type->byte_width() <= 0) {
      return std::vector<DirectColumn>{};
    }
    auto& column = result.emplace_back(call, ctx.pool);
    column.width = Narrow<std::size_t>(type->byte_width());
    column.batch_rows =
        std::min(Narrow<std::size_t>(ctx.batch_size), static_cast<std::size_t>(kChunkRows));
    ARROW_RETURN_NOT_OK(column.first.Resize(input.size()));
    std::size_t pieces = 0;
    for (std::size_t j = 0; j < input.size(); ++j) {
      column.first[j] = pieces;
      const auto length = Narrow<std::size_t>(input[j]->num_rows());
      pieces += 1 + (length - 1) / column.batch_rows;
    }
    ARROW_RETURN_NOT_OK(column.pieces.Resize(pieces));
    ARROW_RETURN_NOT_OK(Reserve(column.buffers, pieces, scratch));
    for (const auto& batch : input) {
      for (std::size_t offset = 0; offset < Narrow<std::size_t>(batch->num_rows());
           offset += column.batch_rows) {
        const auto length =
            std::min(column.batch_rows, Narrow<std::size_t>(batch->num_rows()) - offset);
        ARROW_ASSIGN_OR_RAISE(
            auto values, arrow::AllocateBuffer(Narrow<int64_t>(length * column.width), ctx.pool));
        ARROW_ASSIGN_OR_RAISE(auto validity,
                              arrow::AllocateBuffer(Narrow<int64_t>((length + 7) / 8), ctx.pool));
        std::memset(validity->mutable_data(), 0xff, Narrow<std::size_t>(validity->size()));
        column.pieces[column.buffers.size()] = {values->mutable_data(), validity->mutable_data(),
                                                0};
        column.buffers.push_back({std::move(values), std::move(validity), Narrow<int64_t>(length)});
      }
    }
    if (!call.arguments.empty()) {
      ARROW_RETURN_NOT_OK(column.arguments.Resize(input.size()));
      for (std::size_t j = 0; j < input.size(); ++j) {
        column.arguments[j] =
            input[j]->columns()[Narrow<std::size_t>(call.arguments.front().index)].get();
      }
    }
  }
  return result;
}

struct OrderedLayout {
  explicit OrderedLayout(arrow::MemoryPool* pool)
      : permutation(pool), offsets(pool), starts(pool) {}
  int64_t packed_rows = 0;
  PoolVector<RowRef> permutation;
  PoolVector<std::size_t> offsets;
  PoolVector<std::uint8_t> starts;
  std::shared_ptr<arrow::Array> ranks;
};

struct WindowBuckets {
  const BatchPartition* source = nullptr;
  std::vector<std::size_t> chunks;  // one bucket per input batch, in stream order
};

arrow::Result<OrderedLayout> OrderRows(const plan::WindowCall& call,
                                       const arrow::RecordBatchVector& input, std::size_t rows,
                                       bool rank, const ExecContext& ctx,
                                       const WindowBuckets& buckets, bool local,
                                       std::span<DirectColumn> direct) {
  OrderedLayout layout(ctx.pool);
  int64_t* ranks = nullptr;
  if (rank) {
    if (rows > static_cast<std::size_t>(std::numeric_limits<int64_t>::max()) / sizeof(int64_t)) {
      return arrow::Status::CapacityError("window rank result size overflow");
    }
    ARROW_ASSIGN_OR_RAISE(auto buffer,
                          arrow::AllocateBuffer(Narrow<int64_t>(rows * sizeof(int64_t)), ctx.pool));
    ranks = buffer->mutable_data_as<int64_t>();
    layout.ranks = std::make_shared<arrow::Int64Array>(Narrow<int64_t>(rows), std::move(buffer));
  }
  MemoryReservation scratch;
  scratch.Reset(ctx.budget);
  std::vector<plan::SortKey> keys;
  ARROW_RETURN_NOT_OK(Reserve(keys, call.partition_by.size() + call.order_by.size(), scratch));
  for (const auto& column : call.partition_by) {
    keys.push_back({.column = {.index = column.index, .type = column.type}});
  }
  ARROW_ASSIGN_OR_RAISE(const auto partition,
                        RowComparator::Make(input.front()->schema(), keys, &scratch));
  for (const auto& key : call.order_by) {
    keys.push_back({.column = {.index = key.column.index, .type = key.column.type},
                    .descending = key.descending,
                    .nulls_first = key.nulls_first});
  }
  ARROW_ASSIGN_OR_RAISE(const auto order,
                        RowComparator::Make(input.front()->schema(), keys, &scratch));
  std::vector<RowComparator::KeyArrays> arrays;
  ARROW_RETURN_NOT_OK(Reserve(arrays, input.size(), scratch));
  ARROW_RETURN_NOT_OK(layout.offsets.Resize(input.size()));
  ARROW_RETURN_NOT_OK(layout.permutation.Resize(rows));
  ARROW_RETURN_NOT_OK(layout.starts.Resize(rows));
  const std::size_t count_buckets = buckets.source != nullptr ? buckets.source->count
                                    : local                   ? GroupTable::kPartitions
                                                              : 1;
  PoolVector<std::uint8_t> bucket_ids(ctx.pool);
  if (local) {
    ARROW_RETURN_NOT_OK(bucket_ids.Resize(rows));
  }
  PoolVector<std::size_t> ends(ctx.pool);
  ARROW_RETURN_NOT_OK(ends.Assign(count_buckets, 0));
  if (buckets.source != nullptr) {
    for (std::size_t i = 0; i < input.size(); ++i) {
      ends[buckets.chunks[i]] += static_cast<std::size_t>(input[i]->num_rows());
    }
  } else if (local) {
    std::size_t row = 0;
    for (const auto& batch : input) {
      MemoryReservation hash_memory;
      hash_memory.Reset(ctx.budget);
      ARROW_RETURN_NOT_OK(
          Charge(hash_memory, Narrow<std::size_t>(batch->num_rows()), sizeof(uint64_t)));
      const auto key = batch->column(call.partition_by.front().index);
      ARROW_ASSIGN_OR_RAISE(auto hashes,
                            KeyHashes(arrow::compute::ExecBatch({key}, batch->num_rows())));
      for (const auto hash : hashes) {
        const auto bucket = static_cast<std::uint8_t>(hash & (count_buckets - 1));
        bucket_ids[row++] = bucket;
        ++ends[bucket];
      }
    }
  }
  if (local || buckets.source != nullptr) {
    std::size_t start = 0;
    for (std::size_t b = 0; b < count_buckets; ++b) {
      const std::size_t length = ends[b];
      ends[b] = start;
      start += length;
    }
  }
  std::size_t offset = 0;
  for (std::size_t chunk = 0; chunk < input.size(); ++chunk) {
    const auto count = static_cast<std::size_t>(input[chunk]->num_rows());
    const auto chunk_id = TryNarrow<std::uint32_t>(chunk);
    if (!chunk_id || count > std::numeric_limits<std::uint32_t>::max()) {
      return arrow::Status::CapacityError("window sort row reference overflow");
    }
    layout.offsets[chunk] = offset;
    arrays.emplace_back();
    ARROW_RETURN_NOT_OK(Reserve(arrays.back(), keys.size(), scratch));
    for (const auto& key : keys) {
      arrays.back().push_back(input[chunk]->column(key.column.index).get());
    }
    const std::size_t chunk_bucket = buckets.source != nullptr ? buckets.chunks[chunk] : 0;
    for (std::size_t row = 0; row < count; ++row) {
      auto& cursor = ends[local ? bucket_ids[offset + row] : chunk_bucket];
      layout.permutation[cursor++] = {.chunk = *chunk_id, .row = Narrow<std::uint32_t>(row)};
    }
    offset += count;
  }
  bucket_ids.Reset();
  // Each bucket owns a disjoint range of the permutation and disjoint result rows.
  // Compare actual partition keys inside it, since distinct SQL partitions can collide.
  std::atomic<int64_t> packed_rows{0};
  const PackedOrder all_keys(order, arrays);
  const PackedOrder within_partition(order, arrays, call.partition_by.size());
  const auto order_bucket = [&](std::size_t bucket) -> arrow::Status {
    const std::size_t begin = bucket == 0 ? 0 : ends[bucket - 1];
    const std::size_t end = ends[bucket];
    if (begin == end) {
      return arrow::Status::OK();
    }
    std::size_t first_key = 0;
    if (buckets.source != nullptr) {
      const RowRef first = layout.permutation[begin];
      const auto same = [&](RowRef row) {
        return partition.Compare(arrays[first.chunk], first.row, arrays[row.chunk], row.row) == 0;
      };
      if (std::all_of(layout.permutation.data() + begin, layout.permutation.data() + end, same)) {
        first_key = call.partition_by.size();
      }
    }
    // Row identity stabilizes traversal without stable_sort's unbudgeted allocation. It does
    // not participate in partition/peer comparisons, including when RANK shares this layout.
    const auto& packing = first_key == 0 ? all_keys : within_partition;
    ARROW_ASSIGN_OR_RAISE(
        const bool packed,
        packing.TrySort({layout.permutation.data() + begin, end - begin}, arrays, ctx.pool));
    if (packed) {
      packed_rows.fetch_add(Narrow<int64_t>(end - begin), std::memory_order_relaxed);
    }
    if (!packed && !keys.empty()) {
      std::sort(layout.permutation.data() + begin, layout.permutation.data() + end,
                [&](RowRef a, RowRef b) {
                  const int compare =
                      order.Compare(arrays[a.chunk], a.row, arrays[b.chunk], b.row, first_key);
                  if (compare != 0) {
                    return compare < 0;
                  }
                  return a.chunk < b.chunk || (a.chunk == b.chunk && a.row < b.row);
                });
    }
    std::size_t partition_start = begin;
    int64_t current_rank = 1;
    for (std::size_t i = begin; i < end; ++i) {
      const RowRef current = layout.permutation[i];
      bool start = i == begin;
      if (!start) {
        const RowRef previous = layout.permutation[i - 1];
        start = first_key == 0 && partition.Compare(arrays[previous.chunk], previous.row,
                                                    arrays[current.chunk], current.row) != 0;
        if (start) {
          partition_start = i;
          current_rank = 1;
        } else if (ranks != nullptr &&
                   order.Compare(arrays[previous.chunk], previous.row, arrays[current.chunk],
                                 current.row, call.partition_by.size()) != 0) {
          current_rank = Narrow<int64_t>(i - partition_start + 1);
        }
      }
      layout.starts[i] = start ? 1 : 0;
      if (ranks != nullptr) {
        ranks[layout.offsets[current.chunk] + current.row] = current_rank;
      }
    }
    for (auto& column : direct) {
      WindowPrefixState state;
      WindowPrefixTarget target{{column.first.data(), column.first.size()},
                                {column.pieces.data(), column.pieces.size()},
                                column.width,
                                column.batch_rows};
      ARROW_RETURN_NOT_OK(
          WindowPrefixes(column.call, {column.arguments.data(), column.arguments.size()},
                         {layout.permutation.data() + begin, end - begin},
                         {layout.starts.data() + begin, end - begin}, state, ctx.pool, &target)
              .status());
    }
    return arrow::Status::OK();
  };
  auto* executor = count_buckets > 1 && rows >= kChunkRows ? ctx.executor : nullptr;
  ARROW_RETURN_NOT_OK(ForEach(executor, count_buckets, order_bucket));
  layout.packed_rows = packed_rows.load(std::memory_order_relaxed);
  return layout;
}

arrow::Status PrepareOrdered(Specification& spec, const std::vector<plan::WindowCall>& calls,
                             const arrow::RecordBatchVector& input, std::size_t rows,
                             MemoryReservation& memory, const ExecContext& ctx,
                             const WindowBuckets& buckets) {
  const bool rank = std::ranges::any_of(
      spec.calls, [&](std::size_t i) { return calls[i].kind == plan::WindowCall::Kind::kRank; });
  const bool prefix = std::ranges::any_of(spec.calls, [&](std::size_t i) {
    return calls[i].frame == plan::WindowCall::Frame::kRowsPrefix;
  });
  const auto& first = calls[spec.calls.front()];
  if (!prefix && first.order_by.empty()) {
    // Unordered RANK needs neither partitions nor a permutation.
    arrow::Int64Builder builder(ctx.pool);
    ARROW_RETURN_NOT_OK(builder.Reserve(Narrow<int64_t>(rows)));
    for (std::size_t i = 0; i < rows; ++i) {
      builder.UnsafeAppend(1);
    }
    ARROW_ASSIGN_OR_RAISE(spec.ranks, builder.Finish());
    return arrow::Status::OK();
  }
  // Raw DOUBLE hashes distinguish signed zeros and NaN payloads that the comparator treats
  // as peers. Other first keys route an entire SQL partition to the same worker.
  const bool local = !rank && buckets.source == nullptr && !first.partition_by.empty() &&
                     first.partition_by.front().type != plan::LogicalType::kDouble &&
                     ctx.executor != nullptr && ctx.threads > 1 && rows >= 4 * kChunkRows;
  MemoryReservation direct_memory;
  direct_memory.Reset(ctx.budget);
  std::vector<DirectColumn> direct;
  if (local) {
    auto made = MakeDirectColumns(spec, calls, input, ctx, direct_memory);
    if (made.ok()) {
      direct = *std::move(made);
    } else if (!made.status().IsOutOfMemory()) {
      return made.status();
    }
    if (direct.empty()) {
      direct_memory.Release();
    }
  }
  auto ordered = OrderRows(first, input, rows, rank, ctx, buckets, local, direct);
  if (local && ordered.status().IsOutOfMemory()) {
    std::vector<DirectColumn>().swap(direct);
    direct_memory.Release();
    ordered = OrderRows(first, input, rows, rank, ctx, buckets, false, {});
  }
  ARROW_ASSIGN_OR_RAISE(auto layout, std::move(ordered));
  spec.packed_rows = layout.packed_rows;
  if (!direct.empty()) {
    spec.direct_first = std::move(direct.front().first);
    ARROW_RETURN_NOT_OK(Reserve(spec.direct, direct.size(), memory));
    for (auto& column : direct) {
      auto& chunks = spec.direct.emplace_back();
      ARROW_RETURN_NOT_OK(Reserve(chunks, column.pieces.size(), memory));
      for (std::size_t j = 0; j < column.pieces.size(); ++j) {
        const auto& buffers = column.buffers[j];
        const int64_t nulls = column.pieces[j].nulls;
        chunks.push_back(arrow::MakeArray(
            arrow::ArrayData::Make(plan::ToArrow(column.call.type), buffers.size,
                                   {nulls ? buffers.validity : nullptr, buffers.values}, nulls)));
      }
    }
    return arrow::Status::OK();
  }
  spec.ranks = std::move(layout.ranks);
  if (!prefix) {
    return arrow::Status::OK();
  }
  ARROW_RETURN_NOT_OK(spec.positions.Resize(rows));
  ARROW_RETURN_NOT_OK(Reserve(spec.results, spec.calls.size(), memory));
  spec.results.resize(spec.calls.size());
  MemoryReservation scratch;
  scratch.Reset(ctx.budget);
  std::vector<WindowPrefixState> states;
  std::vector<WindowPrefixState> saved;
  std::vector<PoolVector<const arrow::Array*>> arguments;
  arrow::ArrayVector columns;
  ARROW_RETURN_NOT_OK(Reserve(states, spec.calls.size(), scratch));
  ARROW_RETURN_NOT_OK(Reserve(saved, spec.calls.size(), scratch));
  ARROW_RETURN_NOT_OK(Reserve(arguments, spec.calls.size(), scratch));
  ARROW_RETURN_NOT_OK(Reserve(columns, spec.calls.size(), scratch));
  states.resize(spec.calls.size());
  // Cache argument arrays once for every output range and capacity retry. The retained input
  // owns the arrays; prefix loops avoid RecordBatch's locking, reference-counted column accessor.
  for (const auto index : spec.calls) {
    auto& arrays = arguments.emplace_back(ctx.pool);
    const auto& call = calls[index];
    if (!call.arguments.empty()) {
      ARROW_RETURN_NOT_OK(arrays.Resize(input.size()));
      const auto column = Narrow<std::size_t>(call.arguments.front().index);
      for (std::size_t k = 0; k < input.size(); ++k) {
        arrays[k] = input[k]->columns()[column].get();
      }
    }
  }
  std::uint32_t chunk = 0;
  for (std::size_t begin = 0; begin < rows; ++chunk) {
    auto count = std::min<std::size_t>(kChunkRows, rows - begin);
    saved = states;
    while (true) {
      columns.clear();
      states = saved;
      arrow::Status status;
      for (std::size_t i = 0; i < spec.calls.size(); ++i) {
        const auto& call = calls[spec.calls[i]];
        if (call.kind == plan::WindowCall::Kind::kRank) {
          columns.push_back(nullptr);
          continue;
        }
        auto result = WindowPrefixes(call, {arguments[i].data(), arguments[i].size()},
                                     {layout.permutation.data() + begin, count},
                                     {layout.starts.data() + begin, count}, states[i], ctx.pool);
        if (!result.ok()) {
          status = result.status();
          break;
        }
        columns.push_back(*std::move(result));
      }
      if (status.ok()) {
        break;
      }
      if (!status.IsCapacityError() || count == 1) {
        return status;
      }
      count /= 2;
    }
    for (std::size_t i = 0; i < count; ++i) {
      const RowRef row = layout.permutation[begin + i];
      spec.positions[layout.offsets[row.chunk] + row.row] = {.chunk = chunk,
                                                             .row = Narrow<std::uint32_t>(i)};
    }
    for (std::size_t i = 0; i < columns.size(); ++i) {
      if (columns[i] == nullptr) {
        continue;
      }
      auto& result = spec.results[i];
      ARROW_RETURN_NOT_OK(Reserve(result.chunks, result.chunks.size() + 1, memory));
      ARROW_RETURN_NOT_OK(Reserve(result.arrays, result.arrays.size() + 1, memory));
      result.arrays.push_back(columns[i].get());
      result.chunks.push_back(std::move(columns[i]));
    }
    begin += count;
    if (begin != rows && chunk == std::numeric_limits<std::uint32_t>::max()) {
      return arrow::Status::CapacityError("window prefix result reference overflow");
    }
  }
  return arrow::Status::OK();
}

}  // namespace

struct WindowOperator::State {
  explicit State(ExecContext& context) : ctx(context), refs(context.pool) {
    memory.Reset(ctx.budget);
  }
  ExecContext ctx;
  MemoryReservation memory;
  arrow::RecordBatchVector input;
  WindowBuckets buckets;
  std::vector<Specification> specifications;
  PoolVector<RowRef> refs;
  std::size_t rows = 0;
  std::size_t chunk = 0;
  std::size_t row = 0;
  std::size_t offset = 0;
  bool prepared = false;
};

WindowOperator::WindowOperator(std::unique_ptr<Operator> input, std::vector<plan::WindowCall> calls)
    : input_(std::move(input)), calls_(std::move(calls)) {
  auto fields = input_->output_schema()->fields();
  for (const auto& call : calls_) {
    fields.push_back(arrow::field(call.name, plan::ToArrow(call.type)));
  }
  schema_ = arrow::schema(std::move(fields));
  if (!calls_.empty()) {
    for (const auto& key : calls_.front().partition_by) {
      if (RequestPartitioning(key.index, PartitionRequest::kAllowRouting) != nullptr) {
        break;
      }
    }
  }
}

WindowOperator::~WindowOperator() = default;

const BatchPartition* WindowOperator::RequestPartitioning(int column, PartitionRequest request) {
  if (input_partition_ != nullptr) {
    return column == distribution_column_ ? &output_partition_ : nullptr;
  }
  // A ranking window preserves peer ranks when grouping changes its output order.
  // Other window functions can observe the input order inside peers.
  if (request == PartitionRequest::kExisting || calls_.empty() ||
      !std::ranges::all_of(calls_, [column](const auto& call) {
        return call.kind == plan::WindowCall::Kind::kRank && !call.order_by.empty() &&
               std::ranges::contains(call.partition_by, column, &plan::BoundColumn::index);
      })) {
    return nullptr;
  }
  input_partition_ = input_->RequestPartitioning(column, request);
  if (input_partition_ == nullptr) {
    return nullptr;
  }
  distribution_column_ = column;
  output_partition_.count = input_partition_->count;
  return &output_partition_;
}

arrow::Status WindowOperator::Open(ExecContext& ctx) {
  ARROW_RETURN_NOT_OK(Close());
  done_ = false;
  try {
    const auto validate_column = [&](const plan::BoundColumn& column) {
      const auto& schema = input_->output_schema();
      if (column.index < 0 || column.index >= schema->num_fields() ||
          !schema->field(column.index)->type()->Equals(plan::ToArrow(column.type))) {
        return arrow::Status::Invalid("window: bound column does not match its input schema");
      }
      return arrow::Status::OK();
    };
    for (const auto& call : calls_) {
      if (call.kind == plan::WindowCall::Kind::kRank) {
        if (call.frame || !call.frame_omitted) {
          return plan::UnsupportedError("W8: explicit ranking frames are not supported",
                                        call.over_span);
        }
        if (!call.arguments.empty() || call.type != plan::LogicalType::kBigInt) {
          return arrow::Status::Invalid("window: invalid bound RANK call");
        }
      } else {
        const bool whole = call.frame == plan::WindowCall::Frame::kWholePartition &&
                           call.frame_omitted && call.order_by.empty();
        const bool prefix =
            call.frame == plan::WindowCall::Frame::kRowsPrefix && !call.frame_omitted;
        if ((!whole && !prefix) || call.aggregate == plan::AggKind::kCountDistinct) {
          return plan::UnsupportedError("W6–W10: deferred window execution", call.over_span);
        }
        if (call.arguments.size() != (call.aggregate == plan::AggKind::kCountStar ? 0U : 1U)) {
          return arrow::Status::Invalid("window: wrong aggregate argument count");
        }
      }
      if (!call.arguments.empty() && call.arguments.front().type == plan::LogicalType::kBoolean) {
        return plan::UnsupportedError("W11b: BOOLEAN window arguments are not supported",
                                      call.span);
      }
      if (std::ranges::any_of(
              call.partition_by,
              [](const auto& key) { return key.type == plan::LogicalType::kBoolean; }) ||
          std::ranges::any_of(call.order_by, [](const auto& key) {
            return key.column.type == plan::LogicalType::kBoolean;
          })) {
        return plan::UnsupportedError("W11b: BOOLEAN window keys are not supported",
                                      call.over_span);
      }
      for (const auto& argument : call.arguments) {
        ARROW_RETURN_NOT_OK(validate_column(argument));
      }
      for (const auto& key : call.partition_by) {
        ARROW_RETURN_NOT_OK(validate_column(key));
      }
      for (const auto& key : call.order_by) {
        ARROW_RETURN_NOT_OK(validate_column(key.column));
      }
      if (call.frame == plan::WindowCall::Frame::kRowsPrefix) {
        if (call.aggregate == plan::AggKind::kSumCounts ||
            call.aggregate == plan::AggKind::kSingleValue) {
          return arrow::Status::Invalid("window: internal aggregate is not a prefix function");
        }
        ARROW_RETURN_NOT_OK(
            MakeGroupedAggregateState(
                call.aggregate,
                call.arguments.empty() ? std::nullopt : std::optional(call.arguments.front().type),
                call.type, ctx.pool)
                .status());
      }
    }
    if (ctx.batch_size <= 0) {
      return arrow::Status::Invalid("window: batch size must be positive");
    }
    state_ = std::make_unique<State>(ctx);
    state_->buckets.source = input_partition_;
    input_open_ = true;  // even a partially opened upstream must be closed on failure
    arrow::Status status = input_->Open(ctx);
    if (status.ok()) {
      return status;
    }
    const arrow::Status closed = Close();
    return status & closed;
  } catch (const std::bad_alloc&) {
    const arrow::Status closed = Close();
    return arrow::Status::OutOfMemory("opening a window") & closed;
  }
}

arrow::Status WindowOperator::Prepare() {
  State& s = *state_;
  while (true) {
    ARROW_ASSIGN_OR_RAISE(const Batch batch, input_->Next());
    if (batch.end()) {
      break;
    }
    ARROW_ASSIGN_OR_RAISE(auto rows, Materialize(batch, s.ctx.pool));
    if (rows->num_rows() == 0) {
      continue;
    }
    const auto count = static_cast<std::size_t>(rows->num_rows());
    if (count > static_cast<std::size_t>(std::numeric_limits<int64_t>::max()) - s.rows) {
      return arrow::Status::CapacityError("window row count overflow");
    }
    // Sources may hand us buffers allocated outside the query pool. Conservatively charge
    // retained input as well as pooled allocations, as other blocking operators do; no size cap.
    ARROW_RETURN_NOT_OK(
        Charge(s.memory, static_cast<std::size_t>(arrow::util::TotalBufferSize(*rows)), 1));
    ARROW_RETURN_NOT_OK(Reserve(s.input, s.input.size() + 1, s.memory));
    if (s.buckets.source != nullptr) {
      ARROW_RETURN_NOT_OK(Reserve(s.buckets.chunks, s.input.size() + 1, s.memory));
      s.buckets.chunks.push_back(s.buckets.source->bucket);
    }
    s.input.push_back(std::move(rows));
    s.rows += count;
  }
  input_open_ = false;
  ARROW_RETURN_NOT_OK(input_->Close());
  if (s.rows == 0) {
    s.prepared = true;
    return arrow::Status::OK();
  }
  if (s.buckets.source != nullptr && profile() != nullptr) {
    PoolVector<std::uint8_t> used(s.ctx.pool);
    ARROW_RETURN_NOT_OK(used.Assign(s.buckets.source->count, 0));
    for (const auto bucket : s.buckets.chunks) {
      used[bucket] = 1;
    }
    profile()->Add("rank_buckets", MetricUnit::kCount,
                   std::count(used.data(), used.data() + used.size(), 1));
  }
  for (std::size_t i = 0; i < calls_.size(); ++i) {
    auto at = std::ranges::find_if(s.specifications, [&](const Specification& spec) {
      return SameSpecification(calls_[spec.calls.front()], calls_[i]);
    });
    if (at == s.specifications.end()) {
      ARROW_RETURN_NOT_OK(Reserve(s.specifications, s.specifications.size() + 1, s.memory));
      s.specifications.emplace_back(s.ctx.pool);
      at = s.specifications.end() - 1;
    }
    ARROW_RETURN_NOT_OK(Reserve(at->calls, at->calls.size() + 1, s.memory));
    at->calls.push_back(i);
  }
  for (Specification& spec : s.specifications) {
    if (calls_[spec.calls.front()].frame != plan::WindowCall::Frame::kWholePartition) {
      ARROW_RETURN_NOT_OK(
          PrepareOrdered(spec, calls_, s.input, s.rows, s.memory, s.ctx, s.buckets));
      if (profile() != nullptr && spec.packed_rows > 0) {
        profile()->Add("packed_sort_rows", MetricUnit::kCount, spec.packed_rows);
      }
      if (profile() != nullptr && !spec.direct.empty()) {
        profile()->Add("direct_prefix_columns", MetricUnit::kCount,
                       Narrow<int64_t>(spec.direct.size()));
      }
      continue;
    }
    MemoryReservation scratch;
    scratch.Reset(s.ctx.budget);
    std::vector<plan::AggregateCall> aggregates;
    ARROW_RETURN_NOT_OK(Reserve(aggregates, spec.calls.size(), scratch));
    for (const std::size_t i : spec.calls) {
      const auto& call = calls_[i];
      aggregates.push_back(
          {.kind = call.aggregate,
           .arg = call.arguments.empty() ? std::nullopt : std::optional(call.arguments.front()),
           .type = call.type,
           .id = call.id,
           .span = call.span});
    }
    ARROW_ASSIGN_OR_RAISE(
        auto table,
        GroupTable::Make(calls_[spec.calls.front()].partition_by, std::move(aggregates),
                         input_->output_schema()->num_fields(), s.ctx.pool, s.ctx.budget,
                         /*first_keys=*/false));
    ARROW_RETURN_NOT_OK(spec.ids.Resize(s.rows));
    std::vector<std::uint32_t> ids;
    ARROW_RETURN_NOT_OK(Reserve(ids, std::min<std::size_t>(s.rows, kChunkRows), scratch));
    // GroupTable's no-key path also temporarily holds one group ID per input row.
    ARROW_RETURN_NOT_OK(Charge(scratch, ids.capacity(), sizeof(std::uint32_t)));
    std::size_t offset = 0;
    for (const auto& batch : s.input) {
      for (int64_t start = 0; start < batch->num_rows(); start += kChunkRows) {
        ARROW_ASSIGN_OR_RAISE(const auto piece, SliceBatch(batch, start, kChunkRows));
        ARROW_RETURN_NOT_OK(table->Consume(*piece, &ids));
        std::ranges::copy(ids, spec.ids.data() + offset);
        offset += ids.size();
      }
    }
    ARROW_RETURN_NOT_OK(table->CheckFinalize());
    ARROW_RETURN_NOT_OK(spec.groups.Resize(table->num_groups()));
    ARROW_RETURN_NOT_OK(Reserve(spec.results, spec.calls.size(), s.memory));
    spec.results.resize(spec.calls.size());
    // Finalize in bounded ranges. VARCHAR results can exceed a binary array's offset width
    // even with few groups; retry a smaller range without retaining partially finalized columns.
    arrow::ArrayVector columns;
    ARROW_RETURN_NOT_OK(Reserve(columns, spec.calls.size(), scratch));
    for (std::uint32_t begin = 0; begin < table->num_groups();) {
      auto count = std::min(kChunkRows, table->num_groups() - begin);
      while (true) {
        columns.clear();
        arrow::Status status;
        for (std::size_t i = 0; i < spec.calls.size(); ++i) {
          auto result = table->state(i).Finalize(begin, begin + count, s.ctx.pool);
          if (!result.ok()) {
            status = result.status();
            break;
          }
          columns.push_back(*std::move(result));
        }
        if (status.ok()) {
          break;
        }
        if (!status.IsCapacityError() || count == 1) {
          return status;
        }
        count /= 2;
      }
      const auto chunk = Narrow<std::uint32_t>(spec.results.front().chunks.size());
      for (std::uint32_t row = 0; row < count; ++row) {
        spec.groups[begin + row] = {.chunk = chunk, .row = row};
      }
      for (std::size_t i = 0; i < columns.size(); ++i) {
        Results& results = spec.results[i];
        ARROW_RETURN_NOT_OK(Reserve(results.chunks, results.chunks.size() + 1, s.memory));
        ARROW_RETURN_NOT_OK(Reserve(results.arrays, results.arrays.size() + 1, s.memory));
        results.arrays.push_back(columns[i].get());
        results.chunks.push_back(std::move(columns[i]));
      }
      begin += count;
    }
    // table and its aggregate states die here; only finalized chunks and row mappings remain.
  }
  s.prepared = true;
  return arrow::Status::OK();
}

arrow::Result<Batch> WindowOperator::NextBatch() {
  State& s = *state_;
  if (!s.prepared) {
    ARROW_RETURN_NOT_OK(Prepare());
  }
  if (s.chunk == s.input.size()) {
    return Batch{};
  }
  const auto& input = s.input[s.chunk];
  auto count =
      std::min({static_cast<std::size_t>(input->num_rows()) - s.row,
                static_cast<std::size_t>(s.ctx.batch_size), static_cast<std::size_t>(kChunkRows)});
  while (true) {
    const auto piece_rows =
        std::min(Narrow<std::size_t>(s.ctx.batch_size), static_cast<std::size_t>(kChunkRows));
    count = std::min(count, piece_rows - s.row % piece_rows);
    ARROW_RETURN_NOT_OK(s.refs.Resize(count));
    MemoryReservation scratch;
    scratch.Reset(s.ctx.budget);
    arrow::ArrayVector columns;
    ARROW_RETURN_NOT_OK(Reserve(columns, static_cast<std::size_t>(schema_->num_fields()), scratch));
    columns.resize(static_cast<std::size_t>(schema_->num_fields()));
    const auto width = static_cast<std::size_t>(input->num_columns());
    for (std::size_t i = 0; i < width; ++i) {
      ARROW_ASSIGN_OR_RAISE(columns[i],
                            SliceColumn(*input->column(Narrow<int>(i)), Narrow<int64_t>(s.row),
                                        Narrow<int64_t>(count)));
    }
    arrow::Status status;
    for (const auto& spec : s.specifications) {
      if (!spec.direct.empty()) {
        for (std::size_t i = 0; i < spec.calls.size(); ++i) {
          const auto& chunk = spec.direct[i][spec.direct_first[s.chunk] + s.row / piece_rows];
          const auto offset = Narrow<int64_t>(s.row % piece_rows);
          if (offset == 0 && Narrow<int64_t>(count) == chunk->length()) {
            columns[width + spec.calls[i]] = chunk;
          } else {
            ARROW_ASSIGN_OR_RAISE(columns[width + spec.calls[i]],
                                  SliceColumn(*chunk, offset, Narrow<int64_t>(count)));
          }
        }
        continue;
      }
      if (spec.ranks) {
        ARROW_ASSIGN_OR_RAISE(const auto ranks, SliceColumn(*spec.ranks, Narrow<int64_t>(s.offset),
                                                            Narrow<int64_t>(count)));
        for (const auto call : spec.calls) {
          if (calls_[call].kind == plan::WindowCall::Kind::kRank) {
            columns[width + call] = ranks;
          }
        }
        if (spec.positions.empty()) {
          continue;
        }
      }
      for (std::size_t i = 0; i < count; ++i) {
        s.refs[i] = spec.positions.empty() ? spec.groups[spec.ids[s.offset + i]]
                                           : spec.positions[s.offset + i];
      }
      for (std::size_t i = 0; i < spec.calls.size(); ++i) {
        if (calls_[spec.calls[i]].kind == plan::WindowCall::Kind::kRank) {
          continue;
        }
        ARROW_ASSIGN_OR_RAISE(
            auto builder,
            arrow::MakeBuilder(plan::ToArrow(calls_[spec.calls[i]].type), s.ctx.pool));
        status = GatherRows<false, RowRef>(spec.results[i].arrays, std::span(s.refs.data(), count),
                                           *builder);
        if (!status.ok()) {
          break;
        }
        ARROW_ASSIGN_OR_RAISE(columns[width + spec.calls[i]], builder->Finish());
      }
      if (!status.ok()) {
        break;
      }
    }
    if (!status.ok()) {
      if (status.IsCapacityError() && count > 1) {
        count /= 2;
        continue;
      }
      return status;
    }
    auto batch = arrow::RecordBatch::Make(schema_, Narrow<int64_t>(count), std::move(columns));
    if (input_partition_ != nullptr) {
      output_partition_.bucket = s.buckets.chunks[s.chunk];
    }
    s.offset += count;
    s.row += count;
    if (std::cmp_equal(s.row, input->num_rows())) {
      s.input[s.chunk++].reset();
      s.row = 0;
    }
    return Batch{.data = std::move(batch), .selection = {}};
  }
}

arrow::Result<Batch> WindowOperator::Next() {
  if (state_ == nullptr) {
    return done_ ? arrow::Result<Batch>(Batch{})
                 : arrow::Status::Invalid("window: Next() before Open() or after Close()");
  }
  try {
    auto result = NextBatch();
    if (result.ok() && !result->end()) {
      return result;
    }
    const arrow::Status status = result.status();
    const arrow::Status closed = Close();
    done_ = status.ok() && closed.ok();
    ARROW_RETURN_NOT_OK(status & closed);
    return Batch{};
  } catch (const std::bad_alloc&) {
    const arrow::Status closed = Close();
    return arrow::Status::OutOfMemory("evaluating a window") & closed;
  }
}

arrow::Status WindowOperator::Close() {
  arrow::Status status;
  if (input_open_) {
    input_open_ = false;
    status = input_->Close();
  }
  state_.reset();
  done_ = false;
  return status;
}

}  // namespace antb1::exec
