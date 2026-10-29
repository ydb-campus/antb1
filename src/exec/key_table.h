#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include <arrow/buffer.h>
#include <arrow/compute/exec.h>
#include <arrow/result.h>
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
// (never of the budget, the threads or the environment); the others name a table, for tests.
enum class KeyTableKind : std::uint8_t {
  kAuto,
  kGrouper,  // arrow::compute::Grouper, for every key shape
};

struct KeyTableOptions {
  KeyTableKind kind = KeyTableKind::kAuto;
};

// A table over keys of `key_types` (at least one) that allocates from `ctx`'s memory pool. `ctx`
// itself must outlive the table: arrow's Grouper keeps it.
arrow::Result<std::unique_ptr<KeyTable>> MakeKeyTable(
    const std::vector<arrow::TypeHolder>& key_types, arrow::compute::ExecContext* ctx,
    const KeyTableOptions& options = {});

}  // namespace antb1::exec
