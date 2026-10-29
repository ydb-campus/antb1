#pragma once

#include <cstdint>
#include <limits>
#include <memory>
#include <new>
#include <span>
#include <vector>

#include <arrow/buffer.h>
#include <arrow/compute/exec.h>
#include <arrow/result.h>
#include <arrow/status.h>
#include <arrow/type_fwd.h>

namespace arrow::compute {
class ExecContext;
}  // namespace arrow::compute

namespace antb1::exec {

// The group ids a KeyTable gave the rows of one call, and the groups the call added.
struct KeyIds {
  std::shared_ptr<arrow::Buffer> owner;  // holds `ids`
  std::span<const std::uint32_t> ids;    // one per row, valid while `owner` lives
  std::uint32_t before = 0;              // the table's groups before the call
  std::uint32_t after = 0;               // and after it: the new groups are [before, after)
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
  // Every group's keys in id order, of the types the table was made for (normalized, NULLs kept).
  // It can be called again; the arrays stay valid after later calls.
  [[nodiscard]] virtual arrow::Result<arrow::compute::ExecBatch> Uniques() const = 0;
  [[nodiscard]] virtual std::uint32_t num_groups() const = 0;
};

// Which table numbers the keys. kAuto is the engine's choice, a function of the key types only
// (never of the budget, the threads or the environment): kFixed for the keys a FixedKeyTable takes
// (one or two fixed-width keys, fixed_key_table.h), kGrouper for the others. The others name a
// table, for tests and benchmarks.
enum class KeyTableKind : std::uint8_t {
  kAuto,
  kGrouper,       // arrow::compute::Grouper, for every key shape
  kFixed,         // FixedKeyTable, growing in slot order
  kFixedIdOrder,  // FixedKeyTable, growing in id order: a policy only tests run
};

// The most groups a FixedKeyTable holds: ids are 32-bit, and one value above them is its marker.
inline constexpr std::uint32_t kMaxKeyTableGroups = std::numeric_limits<std::uint32_t>::max() - 1;

struct KeyTableOptions {
  KeyTableKind kind = KeyTableKind::kAuto;
  // Whether a FixedKeyTable sizes its slots from its own first two calls of rows. Off for a table
  // that grows by merging other tables' groups (a lane, the two-level merge), whose calls are each
  // distinct keys, so that its size follows only the groups merged so far.
  bool estimate = true;
  // The groups past which a FixedKeyTable fails with OutOfMemory; lower only in tests.
  std::uint32_t max_groups = kMaxKeyTableGroups;
};

// A table over keys of `key_types` (at least one) that allocates from `ctx`'s memory pool. `ctx`
// itself must outlive the table: arrow's Grouper keeps it. An explicit kind that does not take the
// keys is Invalid.
arrow::Result<std::unique_ptr<KeyTable>> MakeKeyTable(
    const std::vector<arrow::TypeHolder>& key_types, arrow::compute::ExecContext* ctx,
    const KeyTableOptions& options = {});

// `body()`, with a std::bad_alloc from an allocation outside the pool (Arrow's small objects) as
// OutOfMemory: no exception leaves a key table, and GroupAggregateOperator makes its table in
// Open, which nothing converts (as join_table.cc does for the join).
template <class Body>
auto NoBadAlloc(const Body& body) -> decltype(body()) {
  try {
    return body();
  } catch (const std::bad_alloc&) {
    return arrow::Status::OutOfMemory("out of memory in a GROUP BY's key table");
  }
}

}  // namespace antb1::exec
