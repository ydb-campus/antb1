#include "binary_view.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <limits>
#include <memory>
#include <new>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include <arrow/api.h>
#include <arrow/compute/api_vector.h>
#include <arrow/compute/exec.h>
#include <arrow/util/binary_view_util.h>
#include <arrow/util/bit_util.h>

#include "antb1/common/narrow.h"
#include "antb1/exec/join_table.h"
#include "antb1/exec/memory_budget.h"
#include "antb1/exec/sort.h"

#include "gather.h"
#include "pool_vector.h"

namespace antb1::exec {
namespace {

using View = arrow::BinaryViewType::c_type;
constexpr int64_t kRetentionAllowance = int64_t{64} * 1024;
constexpr int64_t kRetentionRatio = 4;
constexpr int64_t kMaxOffset = std::numeric_limits<int32_t>::max();
constexpr int64_t kCopyBlockBytes = int64_t{64} * 1024;

template <class Body>
auto NoBadAlloc(const Body& body) -> decltype(body()) {
  try {
    return body();
  } catch (const std::bad_alloc&) {
    return arrow::Status::OutOfMemory("owned binary-view allocation");
  }
}

// Besides making descriptors immutable, this keeps their non-pool ownership metadata charged
// until their last array dies. Recognizing the exact ArrayData allows a buffering consumer to
// accept an already bounded result without gathering all its descriptors a second time. Slices
// and arrays with another payload-buffer list have different ArrayData and are checked again.
class OwnedViewBuffer final : public arrow::Buffer {
 public:
  OwnedViewBuffer(const std::shared_ptr<arrow::Buffer>& values, arrow::MemoryPool* pool,
                  bool bounded)
      : arrow::Buffer(values->data(), values->size(), values->memory_manager(), values),
        bounded_(bounded) {
    memory_.Reset(dynamic_cast<MemoryBudget*>(pool));
  }

  arrow::Status Charge(std::size_t owner_capacity) {
    constexpr std::size_t kFixed =
        sizeof(OwnedViewBuffer) + sizeof(arrow::BinaryViewArray) + sizeof(arrow::ArrayData);
    constexpr std::size_t kMaxCount =
        (static_cast<std::size_t>(std::numeric_limits<int64_t>::max()) - kFixed) /
        (2 * sizeof(std::shared_ptr<arrow::Buffer>));
    if (owner_capacity > kMaxCount - 2) {
      return arrow::Status::CapacityError("binary-view ownership metadata is too large");
    }
    const std::size_t count = owner_capacity + 2;
    // Allow for Arrow constructing its final buffer list from the passed variadic-buffer vector.
    return memory_.Resize(
        Narrow<int64_t>(kFixed + (count * 2 * sizeof(std::shared_ptr<arrow::Buffer>))));
  }

  void Remember(const std::shared_ptr<arrow::ArrayData>& data) { array_data_ = data; }
  [[nodiscard]] bool Describes(const std::shared_ptr<arrow::ArrayData>& data) const {
    return bounded_ && array_data_.lock() == data;
  }

 private:
  MemoryReservation memory_;
  std::weak_ptr<arrow::ArrayData> array_data_;
  bool bounded_;
};

bool IsBounded(const arrow::Array& array) {
  const auto* buffer = dynamic_cast<const OwnedViewBuffer*>(array.data()->buffers[1].get());
  return buffer != nullptr && buffer->Describes(array.data());
}

struct Source {
  const std::shared_ptr<arrow::Buffer>* owner = nullptr;
  const arrow::Buffer* root = nullptr;
  int64_t offset = 0;  // the source owner's beginning within its backing allocation
  int32_t output = -1;
  int64_t selected_bytes = 0;
  bool copy = true;
};

struct Range {
  const arrow::Buffer* root = nullptr;
  int64_t begin = 0;
  int64_t end = 0;
  int64_t row = 0;
};

struct Segment {
  const arrow::Buffer* root = nullptr;
  int64_t begin = 0;
  int64_t end = 0;
  int32_t buffer = 0;
  int32_t offset = 0;
};

struct InputRef {
  std::uint32_t chunk = 0;
  int64_t row = 0;
};

arrow::Status AddChecked(int64_t value, int64_t& total) {
  if (value < 0 || value > std::numeric_limits<int64_t>::max() - total) {
    return arrow::Status::CapacityError("binary-view capacity overflow");
  }
  total += value;
  return arrow::Status::OK();
}

arrow::Result<std::shared_ptr<arrow::Array>> FinishViews(
    int64_t length, const std::shared_ptr<OwnedViewBuffer>& immutable, arrow::BufferVector owners,
    std::shared_ptr<arrow::Buffer> validity, int64_t null_count) {
  auto array = std::make_shared<arrow::BinaryViewArray>(
      arrow::binary_view(), length, immutable, std::move(owners), std::move(validity), null_count);
  immutable->Remember(array->data());
  return array;
}

// Sources borrow their owner pointers only for this call. The resulting Arrow array takes shared
// ownership of every source it actually references. Scratch vectors live in the query pool.
arrow::Status SourcesOf(std::span<const arrow::Array* const> arrays, PoolVector<Source>& sources,
                        PoolVector<int64_t>& starts) {
  ARROW_RETURN_NOT_OK(starts.Resize(arrays.size() + 1));
  int64_t count = 0;
  for (std::size_t i = 0; i < arrays.size(); ++i) {
    if (arrays[i] == nullptr) {
      return arrow::Status::Invalid("binary-view gather has a null source array");
    }
    const auto type = arrays[i]->type_id();
    if (type != arrow::Type::BINARY && type != arrow::Type::BINARY_VIEW) {
      return arrow::Status::TypeError("binary-view gather needs BINARY or BINARY_VIEW sources");
    }
    starts[i] = count;
    const auto& buffers = arrays[i]->data()->buffers;
    const auto added =
        type == arrow::Type::BINARY ? int64_t{1} : Narrow<int64_t>(buffers.size() - 2);
    ARROW_RETURN_NOT_OK(AddChecked(added, count));
  }
  starts[arrays.size()] = count;
  if (count > kMaxOffset) {
    return arrow::Status::CapacityError("too many binary-view source buffers");
  }
  ARROW_RETURN_NOT_OK(sources.Resize(Narrow<std::size_t>(count)));
  for (std::size_t i = 0; i < arrays.size(); ++i) {
    const auto& buffers = arrays[i]->data()->buffers;
    for (int64_t j = starts[i]; j < starts[i + 1]; ++j) {
      const auto at = Narrow<std::size_t>(j - starts[i]) + 2;
      const std::shared_ptr<arrow::Buffer>& owner = buffers[at];
      Source source{.owner = &owner};
      if (owner != nullptr) {
        source.root = owner.get();
        while (source.root->parent() != nullptr) {
          source.root = source.root->parent().get();
        }
        if (owner->data() != source.root->data()) {
          source.offset = owner->data() - source.root->data();
        }
      }
      sources[Narrow<std::size_t>(j)] = source;
    }
  }
  return arrow::Status::OK();
}

// Exact distinct live byte ranges are important here: repeating the same long value many times
// must not make a sparse slice of a large source appear dense. Sorting scratch ranges does not
// change output order; each range remembers the descriptor it belongs to.
arrow::Result<int64_t> SegmentsOf(PoolVector<Range>& ranges, PoolVector<Segment>& segments,
                                  int64_t& retained) {
  std::sort(ranges.data(), ranges.data() + ranges.size(), [](const Range& a, const Range& b) {
    if (a.root != b.root) {
      return std::less<const arrow::Buffer*>{}(a.root, b.root);
    }
    return a.begin < b.begin || (a.begin == b.begin && a.end < b.end);
  });
  ARROW_RETURN_NOT_OK(segments.Reserve(ranges.size()));
  for (std::size_t i = 0; i < ranges.size(); ++i) {
    const Range& range = ranges[i];
    if (!segments.empty()) {
      Segment& last = segments[segments.size() - 1];
      if (last.root == range.root && range.begin <= last.end) {
        last.end = std::max(last.end, range.end);
        continue;
      }
    }
    if (segments.empty() || segments[segments.size() - 1].root != range.root) {
      ARROW_RETURN_NOT_OK(AddChecked(range.root->capacity(), retained));
    }
    segments.push_back({.root = range.root, .begin = range.begin, .end = range.end});
  }
  int64_t live = 0;
  for (std::size_t i = 0; i < segments.size(); ++i) {
    ARROW_RETURN_NOT_OK(AddChecked(segments[i].end - segments[i].begin, live));
  }
  return live;
}

arrow::Result<arrow::BufferVector> CopySegments(PoolVector<Segment>& segments,
                                                const PoolVector<Range>& ranges, View* views,
                                                OwnedViewBuffer& metadata,
                                                arrow::MemoryPool* pool) {
  PoolVector<int64_t> sizes(pool);
  ARROW_RETURN_NOT_OK(sizes.Reserve(segments.size()));
  for (std::size_t i = 0; i < segments.size(); ++i) {
    Segment& segment = segments[i];
    const int64_t length = segment.end - segment.begin;
    if (length > kMaxOffset) {
      return arrow::Status::CapacityError("compacted binary-view span exceeds offset range");
    }
    if (sizes.empty() || sizes[sizes.size() - 1] > kMaxOffset - length) {
      sizes.push_back(0);
    }
    segment.buffer = Narrow<int32_t>(sizes.size() - 1);
    segment.offset = Narrow<int32_t>(sizes[sizes.size() - 1]);
    sizes[sizes.size() - 1] += length;
  }
  arrow::BufferVector owners;
  ARROW_RETURN_NOT_OK(metadata.Charge(sizes.size()));
  owners.reserve(sizes.size());
  for (std::size_t i = 0; i < sizes.size(); ++i) {
    ARROW_ASSIGN_OR_RAISE(auto bytes, arrow::AllocateBuffer(sizes[i], pool));
    owners.push_back(std::move(bytes));
  }
  for (std::size_t i = 0; i < segments.size(); ++i) {
    const Segment& segment = segments[i];
    std::memcpy(owners[Narrow<std::size_t>(segment.buffer)]->mutable_data() + segment.offset,
                segment.root->data() + segment.begin,
                Narrow<std::size_t>(segment.end - segment.begin));
  }
  std::size_t segment_at = 0;
  for (std::size_t i = 0; i < ranges.size(); ++i) {
    const Range& range = ranges[i];
    while (segments[segment_at].root != range.root || segments[segment_at].end < range.end) {
      ++segment_at;
    }
    const Segment& segment = segments[segment_at];
    View& view = views[range.row];
    view.ref.buffer_index = segment.buffer;
    view.ref.offset = Narrow<int32_t>(segment.offset + range.begin - segment.begin);
  }
  return owners;
}

arrow::Result<arrow::BufferVector> ShareSources(PoolVector<Source>& sources, std::span<View> views,
                                                OwnedViewBuffer& metadata) {
  std::size_t count = 0;
  for (std::size_t i = 0; i < sources.size(); ++i) {
    count += sources[i].output >= 0 ? 1U : 0U;
  }
  ARROW_RETURN_NOT_OK(metadata.Charge(count));
  arrow::BufferVector owners;
  owners.reserve(count);
  for (std::size_t i = 0; i < sources.size(); ++i) {
    Source& source = sources[i];
    if (source.output < 0) {
      continue;
    }
    source.output = Narrow<int32_t>(owners.size());
    owners.push_back(*source.owner);
  }
  for (View& view : views) {
    if (!view.is_inline()) {
      view.ref.buffer_index = sources[Narrow<std::size_t>(view.ref.buffer_index)].output;
    }
  }
  return owners;
}

// Aggregate only source-buffer records, never one range per selected value. Several Arrow
// buffers may alias the same allocating parent; compare that parent's capacity once with all
// selected bytes across those aliases, including repeated output references.
arrow::Result<bool> ChooseHybridSources(PoolVector<Source>& sources, arrow::MemoryPool* pool) {
  PoolVector<Source*> referenced(pool);
  ARROW_RETURN_NOT_OK(referenced.Reserve(sources.size()));
  for (std::size_t i = 0; i < sources.size(); ++i) {
    if (sources[i].selected_bytes > 0) {
      referenced.push_back(&sources[i]);
    }
  }
  if (referenced.empty()) {
    return false;
  }
  std::sort(referenced.data(), referenced.data() + referenced.size(),
            [](const Source* a, const Source* b) {
              return std::less<const arrow::Buffer*>{}(a->root, b->root);
            });
  std::size_t begin = 0;
  bool needs_copy = false;
  while (begin < referenced.size()) {
    const arrow::Buffer* root = referenced[begin]->root;
    int64_t selected = 0;
    std::size_t end = begin;
    while (end < referenced.size() && referenced[end]->root == root) {
      ARROW_RETURN_NOT_OK(AddChecked(referenced[end]->selected_bytes, selected));
      ++end;
    }
    const bool copy = root->capacity() > selected;
    needs_copy = needs_copy || copy;
    for (std::size_t i = begin; i < end; ++i) {
      referenced[i]->copy = copy;
    }
    begin = end;
  }
  return needs_copy;
}

// Count and pack output bytes without tracking their source intervals. A long value occurs once
// for every output descriptor, including duplicates. The two linear passes avoid growing or
// relocating a published payload buffer. Oversized values get a block of their own. Hybrid
// sharing keeps original owners/offsets: an alias can be far beyond INT32_MAX in its parent.
arrow::Result<arrow::BufferVector> CopyValues(PoolVector<Source>& sources, std::span<View> views,
                                              OwnedViewBuffer& metadata, arrow::MemoryPool* pool) {
  PoolVector<int64_t> sizes(pool);
  for (const View& view : views) {
    if (view.is_inline() || !sources[Narrow<std::size_t>(view.ref.buffer_index)].copy) {
      continue;
    }
    const int64_t length = view.size();
    if (sizes.empty() || length > kCopyBlockBytes ||
        sizes[sizes.size() - 1] > kCopyBlockBytes - length) {
      if (sizes.size() >= Narrow<std::size_t>(kMaxOffset)) {
        return arrow::Status::CapacityError("too many copied binary-view buffers");
      }
      if (sizes.size() == sizes.capacity()) {
        ARROW_RETURN_NOT_OK(sizes.Reserve(std::max<std::size_t>(1, sizes.capacity() * 2)));
      }
      sizes.push_back(0);
    }
    sizes[sizes.size() - 1] += length;
  }
  std::size_t shared = 0;
  for (std::size_t i = 0; i < sources.size(); ++i) {
    shared += sources[i].output >= 0 && !sources[i].copy ? 1U : 0U;
  }
  if (sizes.size() > Narrow<std::size_t>(kMaxOffset) - shared) {
    return arrow::Status::CapacityError("too many retained binary-view buffers");
  }
  ARROW_RETURN_NOT_OK(metadata.Charge(shared + sizes.size()));
  arrow::BufferVector owners;
  owners.reserve(shared + sizes.size());
  for (std::size_t i = 0; i < sources.size(); ++i) {
    Source& source = sources[i];
    if (source.output >= 0 && !source.copy) {
      source.output = Narrow<int32_t>(owners.size());
      owners.push_back(*source.owner);
    }
  }
  for (std::size_t i = 0; i < sizes.size(); ++i) {
    ARROW_ASSIGN_OR_RAISE(auto bytes, arrow::AllocateBuffer(sizes[i], pool));
    owners.push_back(std::move(bytes));
  }
  std::size_t output = shared;
  int64_t offset = 0;
  for (View& view : views) {
    if (view.is_inline()) {
      continue;
    }
    const Source& source = sources[Narrow<std::size_t>(view.ref.buffer_index)];
    if (!source.copy) {
      view.ref.buffer_index = source.output;
      continue;
    }
    const int64_t length = view.size();
    if (length > owners[output]->size() - offset) {
      ++output;
      offset = 0;
    }
    std::memcpy(owners[output]->mutable_data() + offset, (*source.owner)->data() + view.ref.offset,
                Narrow<std::size_t>(length));
    view.ref.buffer_index = Narrow<int32_t>(output);
    view.ref.offset = Narrow<int32_t>(offset);
    offset += length;
  }
  return owners;
}

// The join producer shares ordinary Binary chunks. It needs neither backing-allocation ranges
// nor a source record for every buffer: record owners on first use and emit their final indices.
// Keep scratch lazy so empty, null and inline-only results retain no payload or owner map.
template <class RefAt>
arrow::Result<std::shared_ptr<arrow::Array>> GatherSharedBinary(
    std::span<const arrow::Array* const> arrays, int64_t length, const RefAt& ref_at,
    arrow::MemoryPool* pool) {
  if (arrays.size() > static_cast<std::size_t>(kMaxOffset)) {
    return arrow::Status::CapacityError("too many binary-view source buffers");
  }
  ARROW_ASSIGN_OR_RAISE(auto values,
                        arrow::AllocateBuffer(length * arrow::BinaryViewType::kSize, pool));
  auto* views = values->mutable_data_as<View>();
  auto immutable = std::make_shared<OwnedViewBuffer>(std::move(values), pool, false);
  ARROW_RETURN_NOT_OK(immutable->Charge(0));
  PoolVector<int32_t> owner_indices(pool);
  PoolVector<std::uint32_t> chunks(pool);
  std::shared_ptr<arrow::Buffer> valid;
  int64_t null_count = 0;
  for (int64_t row = 0; row < length; ++row) {
    ARROW_ASSIGN_OR_RAISE(const InputRef ref, ref_at(row));
    if (ref.chunk != kNoChunk && (std::cmp_greater_equal(ref.chunk, arrays.size()) || ref.row < 0 ||
                                  ref.row >= arrays[ref.chunk]->length())) {
      return arrow::Status::IndexError("binary-view gather reference outside input");
    }
    if (ref.chunk == kNoChunk || arrays[ref.chunk]->IsNull(ref.row)) {
      if (valid == nullptr) {
        ARROW_ASSIGN_OR_RAISE(valid, arrow::AllocateBitmap(length, pool));
        std::memset(valid->mutable_data(), 0xff, Narrow<std::size_t>(valid->size()));
      }
      arrow::bit_util::ClearBit(valid->mutable_data(), row);
      ++null_count;
      views[row] = {};
      continue;
    }
    const auto& binary = static_cast<const arrow::BinaryArray&>(*arrays[ref.chunk]);
    const int32_t size = binary.value_length(ref.row);
    if (size == 0) {
      views[row] = {};
      continue;
    }
    int32_t owner_index = 0;
    if (size > arrow::BinaryViewType::kInlineSize) {
      if (owner_indices.empty()) {
        ARROW_RETURN_NOT_OK(owner_indices.Assign(arrays.size(), -1));
        ARROW_RETURN_NOT_OK(chunks.Reserve(std::min(arrays.size(), Narrow<std::size_t>(length))));
      }
      int32_t& index = owner_indices[ref.chunk];
      if (index < 0) {
        index = Narrow<int32_t>(chunks.size());
        chunks.push_back(ref.chunk);
      }
      owner_index = index;
    }
    views[row] = arrow::util::ToBinaryView(binary.GetView(ref.row), owner_index,
                                           binary.value_offset(ref.row));
  }
  arrow::BufferVector owners;
  ARROW_RETURN_NOT_OK(immutable->Charge(chunks.size()));
  owners.reserve(chunks.size());
  for (std::size_t i = 0; i < chunks.size(); ++i) {
    owners.push_back(arrays[chunks[i]]->data()->buffers[2]);
  }
  return FinishViews(length, immutable, std::move(owners), std::move(valid), null_count);
}

template <class RefAt>
arrow::Result<std::shared_ptr<arrow::Array>> GatherImpl(std::span<const arrow::Array* const> arrays,
                                                        int64_t length, const RefAt& ref_at,
                                                        arrow::MemoryPool* pool,
                                                        BinaryViewRetention retention) {
  if (pool == nullptr || length < 0 ||
      length > std::numeric_limits<int64_t>::max() / arrow::BinaryViewType::kSize) {
    return arrow::Status::Invalid("invalid binary-view gather size or pool");
  }
  if (retention == BinaryViewRetention::kShare &&
      std::ranges::all_of(arrays, [](const arrow::Array* array) {
        return array != nullptr && array->type_id() == arrow::Type::BINARY;
      })) {
    return GatherSharedBinary(arrays, length, ref_at, pool);
  }
  PoolVector<Source> sources(pool);
  PoolVector<int64_t> starts(pool);
  ARROW_RETURN_NOT_OK(SourcesOf(arrays, sources, starts));
  ARROW_ASSIGN_OR_RAISE(auto values,
                        arrow::AllocateBuffer(length * arrow::BinaryViewType::kSize, pool));
  auto* views = values->mutable_data_as<View>();
  auto immutable = std::make_shared<OwnedViewBuffer>(
      std::move(values), pool,
      retention == BinaryViewRetention::kBounded || retention == BinaryViewRetention::kCopy);
  ARROW_RETURN_NOT_OK(immutable->Charge(0));
  std::shared_ptr<arrow::Buffer> valid;
  int64_t null_count = 0;
  PoolVector<Range> ranges(pool);
  for (int64_t row = 0; row < length; ++row) {
    ARROW_ASSIGN_OR_RAISE(const InputRef ref, ref_at(row));
    if (ref.chunk != kNoChunk && (std::cmp_greater_equal(ref.chunk, arrays.size()) || ref.row < 0 ||
                                  ref.row >= arrays[ref.chunk]->length())) {
      return arrow::Status::IndexError("binary-view gather reference outside input");
    }
    if (ref.chunk == kNoChunk || arrays[ref.chunk]->IsNull(ref.row)) {
      if (valid == nullptr) {
        ARROW_ASSIGN_OR_RAISE(valid, arrow::AllocateBitmap(length, pool));
        std::memset(valid->mutable_data(), 0xff, Narrow<std::size_t>(valid->size()));
      }
      arrow::bit_util::ClearBit(valid->mutable_data(), row);
      ++null_count;
      views[row] = {};
      continue;
    }
    const arrow::Array& input = *arrays[ref.chunk];
    int64_t source_at = starts[ref.chunk];
    if (input.type_id() == arrow::Type::BINARY) {
      const auto& binary = static_cast<const arrow::BinaryArray&>(input);
      const int32_t size = binary.value_length(ref.row);
      if (size == 0) {
        views[row] = {};
        continue;
      }
      views[row] = arrow::util::ToBinaryView(binary.GetView(ref.row), Narrow<int32_t>(source_at),
                                             binary.value_offset(ref.row));
    } else {
      views[row] = static_cast<const arrow::BinaryViewArray&>(input).raw_values()[ref.row];
      if (!views[row].is_inline()) {
        source_at += views[row].ref.buffer_index;
        views[row].ref.buffer_index = Narrow<int32_t>(source_at);
      }
    }
    if (!views[row].is_inline()) {
      Source& source = sources[Narrow<std::size_t>(source_at)];
      source.output = 0;  // referenced; assigned its output index after the retention decision
      if (retention == BinaryViewRetention::kHybrid) {
        ARROW_RETURN_NOT_OK(AddChecked(views[row].size(), source.selected_bytes));
      }
      if (retention == BinaryViewRetention::kBounded) {
        if (ranges.empty()) {
          ARROW_RETURN_NOT_OK(ranges.Reserve(Narrow<std::size_t>(length)));
        }
        const int64_t begin = source.offset + views[row].ref.offset;
        ranges.push_back(
            {.root = source.root, .begin = begin, .end = begin + views[row].size(), .row = row});
      }
    }
  }
  arrow::BufferVector owners;
  const std::span<View> output(views, Narrow<std::size_t>(length));
  if (retention == BinaryViewRetention::kShare) {
    ARROW_ASSIGN_OR_RAISE(owners, ShareSources(sources, output, *immutable));
  } else if (retention == BinaryViewRetention::kCopy) {
    ARROW_ASSIGN_OR_RAISE(owners, CopyValues(sources, output, *immutable, pool));
  } else if (retention == BinaryViewRetention::kHybrid) {
    ARROW_ASSIGN_OR_RAISE(const bool needs_copy, ChooseHybridSources(sources, pool));
    if (needs_copy) {
      ARROW_ASSIGN_OR_RAISE(owners, CopyValues(sources, output, *immutable, pool));
    } else {
      ARROW_ASSIGN_OR_RAISE(owners, ShareSources(sources, output, *immutable));
    }
  } else if (!ranges.empty()) {
    PoolVector<Segment> segments(pool);
    int64_t retained = 0;
    ARROW_ASSIGN_OR_RAISE(const int64_t live, SegmentsOf(ranges, segments, retained));
    const int64_t allowance = live > std::numeric_limits<int64_t>::max() / kRetentionRatio
                                  ? std::numeric_limits<int64_t>::max()
                                  : std::max(kRetentionAllowance, live * kRetentionRatio);
    if (retained > allowance) {
      ARROW_ASSIGN_OR_RAISE(owners, CopySegments(segments, ranges, views, *immutable, pool));
    } else {
      ARROW_ASSIGN_OR_RAISE(owners, ShareSources(sources, output, *immutable));
    }
  }
  return FinishViews(length, immutable, std::move(owners), std::move(valid), null_count);
}

arrow::Result<InputRef> IndexAt(const arrow::Array& indices, int64_t row, int64_t length) {
  if (indices.IsNull(row)) {
    return InputRef{.chunk = kNoChunk};
  }
  const int64_t index = indices.type_id() == arrow::Type::INT64
                            ? static_cast<const arrow::Int64Array&>(indices).Value(row)
                            : static_cast<const arrow::UInt32Array&>(indices).Value(row);
  if (index < 0 || index >= length) {
    return arrow::Status::IndexError("binary-view Take index outside input");
  }
  return InputRef{.row = index};
}

arrow::Result<std::shared_ptr<arrow::Array>> MaterializeArray(const arrow::BinaryViewArray& input,
                                                              arrow::MemoryPool* pool) {
  int64_t bytes = 0;
  for (int64_t row = 0; row < input.length(); ++row) {
    if (!input.IsNull(row)) {
      const int32_t length = input.raw_values()[row].size();
      if (length > kMaxOffset - bytes) {
        return arrow::Status::CapacityError("materialized binary values exceed offset range");
      }
      bytes += length;
    }
  }
  arrow::BinaryBuilder builder(pool);
  ARROW_RETURN_NOT_OK(builder.Reserve(input.length()));
  ARROW_RETURN_NOT_OK(builder.ReserveData(bytes));
  for (int64_t row = 0; row < input.length(); ++row) {
    if (input.IsNull(row)) {
      builder.UnsafeAppendNull();
    } else if (input.raw_values()[row].size() == 0) {
      builder.UnsafeAppend(std::string_view{""});
    } else {
      builder.UnsafeAppend(input.GetView(row));
    }
  }
  return builder.Finish();
}

}  // namespace

template <class Ref>
arrow::Result<std::shared_ptr<arrow::Array>> GatherBinaryViews(
    std::span<const arrow::Array* const> arrays, std::span<const Ref> refs, arrow::MemoryPool* pool,
    BinaryViewRetention retention) {
  return NoBadAlloc([&] -> arrow::Result<std::shared_ptr<arrow::Array>> {
    const auto length = TryNarrow<int64_t>(refs.size());
    if (!length.has_value()) {
      return arrow::Status::CapacityError("too many binary-view gather references");
    }
    return GatherImpl(
        arrays, *length,
        [&](int64_t row) -> arrow::Result<InputRef> {
          const Ref& ref = refs[Narrow<std::size_t>(row)];
          return InputRef{.chunk = ref.chunk, .row = ref.row};
        },
        pool, retention);
  });
}

template arrow::Result<std::shared_ptr<arrow::Array>> GatherBinaryViews<JoinRowRef>(
    std::span<const arrow::Array* const>, std::span<const JoinRowRef>, arrow::MemoryPool*,
    BinaryViewRetention);
template arrow::Result<std::shared_ptr<arrow::Array>> GatherBinaryViews<SortBuffer::RowRef>(
    std::span<const arrow::Array* const>, std::span<const SortBuffer::RowRef>, arrow::MemoryPool*,
    BinaryViewRetention);

bool HasBinaryViews(const arrow::Schema& schema) {
  return std::ranges::any_of(schema.fields(), [](const auto& field) {
    return field->type()->id() == arrow::Type::BINARY_VIEW;
  });
}

arrow::Result<std::shared_ptr<arrow::RecordBatch>> TakeViewBatch(
    const std::shared_ptr<arrow::RecordBatch>& batch, const std::shared_ptr<arrow::Array>& indices,
    arrow::MemoryPool* pool, BinaryViewRetention retention) {
  return NoBadAlloc([&] -> arrow::Result<std::shared_ptr<arrow::RecordBatch>> {
    if (batch == nullptr || indices == nullptr || pool == nullptr) {
      return arrow::Status::Invalid("binary-view Take needs a batch, indices and pool");
    }
    if (indices->type_id() != arrow::Type::INT64 && indices->type_id() != arrow::Type::UINT32) {
      return arrow::Status::TypeError("binary-view Take needs INT64 or UINT32 indices");
    }
    // Also validate indices for a zero-column batch, which otherwise dispatches no kernel.
    for (int64_t row = 0; row < indices->length(); ++row) {
      ARROW_RETURN_NOT_OK(IndexAt(*indices, row, batch->num_rows()).status());
    }
    arrow::compute::ExecContext kernels(pool);
    arrow::ArrayVector columns;
    columns.reserve(Narrow<std::size_t>(batch->num_columns()));
    for (const auto& column : batch->columns()) {
      if (column->type_id() == arrow::Type::BINARY_VIEW) {
        const arrow::Array* source = column.get();
        ARROW_ASSIGN_OR_RAISE(
            auto out, GatherImpl(
                          std::span(&source, 1), indices->length(),
                          [&](int64_t row) { return IndexAt(*indices, row, batch->num_rows()); },
                          pool, retention));
        columns.push_back(std::move(out));
      } else {
        ARROW_ASSIGN_OR_RAISE(
            const auto out,
            arrow::compute::Take(column, indices, arrow::compute::TakeOptions::NoBoundsCheck(),
                                 &kernels));
        columns.push_back(out.make_array());
      }
    }
    return arrow::RecordBatch::Make(batch->schema(), indices->length(), std::move(columns));
  });
}

arrow::Result<std::shared_ptr<arrow::RecordBatch>> FilterViewBatch(
    const std::shared_ptr<arrow::RecordBatch>& batch,
    const std::shared_ptr<arrow::BooleanArray>& selection, arrow::MemoryPool* pool,
    BinaryViewRetention retention) {
  return NoBadAlloc([&] -> arrow::Result<std::shared_ptr<arrow::RecordBatch>> {
    if (batch == nullptr || selection == nullptr || pool == nullptr ||
        selection->length() != batch->num_rows()) {
      return arrow::Status::Invalid("binary-view Filter mask length differs from its batch");
    }
    if (selection->true_count() == batch->num_rows()) {
      if (retention == BinaryViewRetention::kShare) {
        return batch;
      }
      if (retention == BinaryViewRetention::kCopy) {
        return CopyBinaryViewPayload(batch, pool);
      }
      if (retention == BinaryViewRetention::kHybrid) {
        return RetainBinaryViewPayload(batch, pool);
      }
      return CompactBinaryViews(batch, pool);
    }
    arrow::Int64Builder selected(pool);
    ARROW_RETURN_NOT_OK(selected.Reserve(selection->true_count()));
    for (int64_t row = 0; row < selection->length(); ++row) {
      if (!selection->IsNull(row) && selection->Value(row)) {
        selected.UnsafeAppend(row);
      }
    }
    ARROW_ASSIGN_OR_RAISE(auto indices, selected.Finish());
    return TakeViewBatch(batch, indices, pool, retention);
  });
}

arrow::Result<std::shared_ptr<arrow::RecordBatch>> CompactBinaryViews(
    const std::shared_ptr<arrow::RecordBatch>& batch, arrow::MemoryPool* pool) {
  return NoBadAlloc([&] -> arrow::Result<std::shared_ptr<arrow::RecordBatch>> {
    if (batch == nullptr || pool == nullptr) {
      return arrow::Status::Invalid("binary-view compaction needs a batch and pool");
    }
    arrow::ArrayVector columns;
    bool changed = false;
    columns.reserve(Narrow<std::size_t>(batch->num_columns()));
    for (const auto& column : batch->columns()) {
      if (column->type_id() != arrow::Type::BINARY_VIEW || IsBounded(*column)) {
        columns.push_back(column);
        continue;
      }
      changed = true;
      const arrow::Array* source = column.get();
      ARROW_ASSIGN_OR_RAISE(
          auto out, GatherImpl(
                        std::span(&source, 1), column->length(),
                        [](int64_t row) -> arrow::Result<InputRef> { return InputRef{.row = row}; },
                        pool, BinaryViewRetention::kBounded));
      columns.push_back(std::move(out));
    }
    return changed
               ? arrow::RecordBatch::Make(batch->schema(), batch->num_rows(), std::move(columns))
               : batch;
  });
}

arrow::Result<std::shared_ptr<arrow::RecordBatch>> CopyBinaryViewPayload(
    const std::shared_ptr<arrow::RecordBatch>& batch, arrow::MemoryPool* pool) {
  return NoBadAlloc([&] -> arrow::Result<std::shared_ptr<arrow::RecordBatch>> {
    if (batch == nullptr || pool == nullptr) {
      return arrow::Status::Invalid("binary-view payload copy needs a batch and pool");
    }
    if (!HasBinaryViews(*batch->schema())) {
      return batch;
    }
    arrow::ArrayVector columns;
    columns.reserve(Narrow<std::size_t>(batch->num_columns()));
    for (const auto& column : batch->columns()) {
      if (column->type_id() != arrow::Type::BINARY_VIEW) {
        columns.push_back(column);
        continue;
      }
      const arrow::Array* source = column.get();
      ARROW_ASSIGN_OR_RAISE(
          auto out, GatherImpl(
                        std::span(&source, 1), column->length(),
                        [](int64_t row) -> arrow::Result<InputRef> { return InputRef{.row = row}; },
                        pool, BinaryViewRetention::kCopy));
      columns.push_back(std::move(out));
    }
    return arrow::RecordBatch::Make(batch->schema(), batch->num_rows(), std::move(columns));
  });
}

arrow::Result<std::shared_ptr<arrow::RecordBatch>> RetainBinaryViewPayload(
    const std::shared_ptr<arrow::RecordBatch>& batch, arrow::MemoryPool* pool) {
  return NoBadAlloc([&] -> arrow::Result<std::shared_ptr<arrow::RecordBatch>> {
    if (batch == nullptr || pool == nullptr) {
      return arrow::Status::Invalid("binary-view payload retention needs a batch and pool");
    }
    if (!HasBinaryViews(*batch->schema())) {
      return batch;
    }
    arrow::ArrayVector columns;
    columns.reserve(Narrow<std::size_t>(batch->num_columns()));
    for (const auto& column : batch->columns()) {
      if (column->type_id() != arrow::Type::BINARY_VIEW) {
        columns.push_back(column);
        continue;
      }
      const arrow::Array* source = column.get();
      ARROW_ASSIGN_OR_RAISE(
          auto out, GatherImpl(
                        std::span(&source, 1), column->length(),
                        [](int64_t row) -> arrow::Result<InputRef> { return InputRef{.row = row}; },
                        pool, BinaryViewRetention::kHybrid));
      columns.push_back(std::move(out));
    }
    return arrow::RecordBatch::Make(batch->schema(), batch->num_rows(), std::move(columns));
  });
}

arrow::Result<std::shared_ptr<arrow::RecordBatch>> MaterializeBinaryViews(
    const std::shared_ptr<arrow::RecordBatch>& batch, arrow::MemoryPool* pool) {
  return NoBadAlloc([&] -> arrow::Result<std::shared_ptr<arrow::RecordBatch>> {
    if (batch == nullptr || pool == nullptr) {
      return arrow::Status::Invalid("binary-view materialization needs a batch and pool");
    }
    if (!HasBinaryViews(*batch->schema())) {
      return batch;
    }
    arrow::FieldVector fields = batch->schema()->fields();
    arrow::ArrayVector columns;
    columns.reserve(Narrow<std::size_t>(batch->num_columns()));
    for (int c = 0; c < batch->num_columns(); ++c) {
      const auto& input = batch->column(c);
      if (input->type_id() != arrow::Type::BINARY_VIEW) {
        columns.push_back(input);
        continue;
      }
      ARROW_ASSIGN_OR_RAISE(
          auto out, MaterializeArray(static_cast<const arrow::BinaryViewArray&>(*input), pool));
      columns.push_back(std::move(out));
      fields[Narrow<std::size_t>(c)] = fields[Narrow<std::size_t>(c)]->WithType(arrow::binary());
    }
    return arrow::RecordBatch::Make(arrow::schema(std::move(fields), batch->schema()->metadata()),
                                    batch->num_rows(), std::move(columns));
  });
}

}  // namespace antb1::exec
