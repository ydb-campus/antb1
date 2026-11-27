#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <new>
#include <span>
#include <utility>
#include <vector>

#include <arrow/buffer.h>
#include <arrow/compute/exec.h>
#include <arrow/result.h>
#include <arrow/status.h>
#include <arrow/stl_allocator.h>
#include <arrow/type_fwd.h>

#include "antb1/exec/memory_budget.h"

namespace arrow::compute {
class ExecContext;
}  // namespace arrow::compute

namespace antb1::exec {

// A fixed-width column with no values buffer, which the Arrow format allows when it has no rows or
// only NULLs.
bool LacksValues(const arrow::ArrayData& data);

// The group ids a KeyTable gave the rows of one call, and the groups the call added.
struct KeyIds {
  std::shared_ptr<arrow::Buffer> owner;  // holds `ids`
  std::span<const std::uint32_t> ids;    // one per row, valid while `owner` lives
  std::uint32_t before = 0;              // the table's groups before the call
  std::uint32_t after = 0;               // and after it: the new groups are [before, after)
};

// Keep the charge for the batch's datum vector until that vector is destroyed. Individual
// arrays may outlive the batch and carry their own storage and metadata charges.
struct KeySnapshot {
  MemoryReservation datum_memory;
  arrow::compute::ExecBatch batch;
};

// The table that numbers a GroupTable's key rows: every distinct row of key values (NULL is a
// value) gets a dense id from 0 that never changes. A table may number the new groups of one call
// in an order of its own, so callers find a new group's rows by its id. Keys arrive as GroupTable
// gives them: DOUBLE keys normalized (double_key.h), at any array offset. A table allocates from
// the pool of the ExecContext it was made with, so the session's MemoryBudget charges it.
// Single-threaded like GroupTable: no two calls on one table may overlap, Uniques() included,
// since arrow's Grouper reuses scratch members in GetUniques.
class KeyTable {
 public:
  KeyTable() = default;
  KeyTable(const KeyTable&) = delete;
  KeyTable& operator=(const KeyTable&) = delete;
  KeyTable(KeyTable&&) = delete;
  KeyTable& operator=(KeyTable&&) = delete;
  virtual ~KeyTable() = default;

  // The group of each row of `keys`, adding the groups that are new.
  virtual arrow::Result<KeyIds> Consume(const arrow::compute::ExecBatch& keys) = 0;
  // The group of each row of `keys`, null for a row whose keys no group has: adds no group (a set
  // operation's inputs that only look up, docs/adr/0039-set-operations.md).
  virtual arrow::Result<std::shared_ptr<arrow::UInt32Array>> Lookup(
      const arrow::compute::ExecBatch& keys) = 0;
  // Every group's keys in id order, of the types the table was made for (normalized, NULLs kept).
  // It can be called again; the arrays stay valid after later calls.
  [[nodiscard]] virtual arrow::Result<KeySnapshot> Uniques() const = 0;
  [[nodiscard]] virtual std::uint32_t num_groups() const = 0;
};

// Called synchronously in input order, once per bounded chunk. The IDs are borrowed only for
// this call. offset is the first row (or selection position), after is the new total group count.
using KeyIdConsumer = std::function<arrow::Status(
    std::size_t offset, std::span<const std::uint32_t> ids, std::uint32_t after)>;

// Fixed-width tables assign consecutive IDs to new groups in first-seen order, including when
// merging a selection. They can update states from a bounded ID buffer and merge keys in their
// stored representation. Once the owner finishes inserting, multiple destinations may read the same
// source's stored groups concurrently. Only the destination uses mutable probe scratch.
class StoredKeyTable : public KeyTable {
 public:
  virtual arrow::Status ConsumeChunks(const arrow::compute::ExecBatch& keys,
                                      const KeyIdConsumer& consume) = 0;
  // As ConsumeChunks for selected input rows. IDs follow selection order, including repeated
  // rows; the callback offset indexes the selection. An empty selection adds no groups.
  virtual arrow::Status ConsumeSelected(const arrow::compute::ExecBatch& keys,
                                        std::span<const std::uint32_t> rows,
                                        const KeyIdConsumer& consume) = 0;
  // The source and destination must be distinct tables of the same key types. Read its groups
  // in the supplied order. Their keys are copied into this table only when a group is new.
  virtual arrow::Status MergeStored(const StoredKeyTable& source,
                                    std::span<const std::uint32_t> groups,
                                    const KeyIdConsumer& consume) = 0;
  // The same full-key hash as GroupTable::RouteRows. The caller provides a valid consecutive
  // range [first, first + out.size()) within num_groups().
  virtual void RoutingHashes(std::uint32_t first, std::span<std::uint64_t> out) const = 0;
  // Materialize a group-ID range at an output boundary. The caller owns the returned keys.
  [[nodiscard]] virtual arrow::Result<KeySnapshot> StoredUniques(std::uint32_t first,
                                                                 std::uint32_t count) const = 0;
  [[nodiscard]] virtual arrow::Result<KeySnapshot> GatherStored(
      std::span<const std::uint32_t> groups) const = 0;
  [[nodiscard]] arrow::Result<KeySnapshot> Uniques() const final {
    return StoredUniques(0, num_groups());
  }

 protected:
  StoredKeyTable(arrow::MemoryPool* pool, MemoryBudget* budget)
      : pool_(pool), budget_(budget != nullptr ? budget : dynamic_cast<MemoryBudget*>(pool)) {}
  static arrow::Status CheckSelection(std::span<const std::uint32_t> selected,
                                      std::size_t source_size);
  arrow::Status CheckStoredRange(std::uint32_t first, std::uint32_t count) const;
  // Allocate and charge the batch and column descriptors with the lifetime of their owners.
  arrow::Result<KeySnapshot> MakeSnapshot(std::size_t columns, std::int64_t length) const;
  arrow::Status AppendColumn(KeySnapshot& snapshot, const std::shared_ptr<arrow::DataType>& type,
                             std::shared_ptr<arrow::Buffer> validity,
                             std::shared_ptr<arrow::Buffer> values, std::int64_t nulls) const;
  arrow::MemoryPool* pool_;

 private:
  MemoryBudget* budget_;
};

// Which table numbers the keys. kAuto is the engine's choice, a function of the key types only
// (never of the budget, the threads or the environment); the others name a table, for tests.
enum class KeyTableKind : std::uint8_t {
  kAuto,
  kGrouper,        // arrow::compute::Grouper, for every key shape
  kPackedStrings,  // short STRING/BINARY keys; unsupported shapes use kGrouper
  kFixed,          // fixed-width candidates: packed one/two keys when eligible, otherwise columns
  kFixedIdOrder,   // id-order growth for the packed layout; same column layout for other keys
};

// One ID is reserved as the fixed table's missing-key marker.
inline constexpr std::uint32_t kMaxKeyTableGroups = std::numeric_limits<std::uint32_t>::max() - 1;

struct KeyTableOptions {
  KeyTableKind kind = KeyTableKind::kAuto;
  // Size the packed fixed-width table from its first two nonempty batches. Disable for merges.
  bool estimate = true;
  // Number new groups in encounter order, independent of how the input is batched.
  bool first_seen_order = false;
  // Lower only in tests; the fixed backend rejects an additional group with OutOfMemory.
  std::uint32_t max_groups = kMaxKeyTableGroups;
  // Stage selected stored column keys in bounded chunks and prefetch destination probe slots.
  bool prefetch_composite_merge = false;
  // Optional test/benchmark witness set when a nonempty column-key merge enters its probe loop.
  // A destination with a witness must have exclusive access to it for the table's lifetime.
  bool* prefetched_composite_merge = nullptr;
  // Construction-only. A local lane owns this reservation for its concrete index and other
  // persistent descriptors; it must outlive the index.
  MemoryReservation* table_metadata = nullptr;
};

// A table over keys of `key_types` (at least one) that allocates from `ctx`'s memory pool. `ctx`
// itself must outlive the table: arrow's Grouper keeps it. Pass the query budget separately
// when the pool wraps it, so heap metadata can be charged for the lifetime of returned keys.
arrow::Result<std::unique_ptr<KeyTable>> MakeKeyTable(
    const std::vector<arrow::TypeHolder>& key_types, arrow::compute::ExecContext* ctx,
    const KeyTableOptions& options = {}, MemoryBudget* budget = nullptr);

// Preserve a query pool's allocation diagnostic and convert ordinary heap failures to Status.
template <class Body>
auto KeyTableNoBadAlloc(const Body& body) -> decltype(body()) {
  try {
    return body();
  } catch (const arrow::stl::BadAlloc& error) {
    return arrow::Status::OutOfMemory(error.what());
  } catch (const std::bad_alloc&) {
    return arrow::Status::OutOfMemory("out of memory in a GROUP BY's key table");
  }
}

}  // namespace antb1::exec
