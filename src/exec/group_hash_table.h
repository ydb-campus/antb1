#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include <arrow/array.h>
#include <arrow/compute/api.h>
#include <arrow/memory_pool.h>
#include <arrow/result.h>
#include <arrow/status.h>
#include <arrow/type.h>

#include "antb1/exec/memory_budget.h"

// An aggregation hash table of our own (docs/adr/0019-own-aggregation-hash-table.md), for the one
// thing arrow::compute::Grouper does inside exec::GroupTable: turning a batch of key rows into a
// group id per row. Deliberately the grouper's own shape -- Make, Consume, num_groups, GetUniques
// -- so that it is a drop-in there and nothing else about GroupTable has to move with it.

namespace antb1::exec {

// The group of a row of fixed-width keys, by open addressing over a byte of each slot's hash.
//
// Why a tag byte rather than the prototype's plain linear probing: that prototype was 1.17-1.30x
// the grouper with 1-4 M groups and **0.22-0.70x with 1000**, where the table is cached and Arrow's
// own probing compares eight slot tags at once. Eight tags are a 64-bit word here, matched with
// arithmetic rather than intrinsics, so one definition serves every architecture the engine builds
// for and the common case -- few groups, every probe a hit -- costs one word load and no branch per
// eight slots.
//
// Keys are packed one row per group: a NULL bitmap, then each key's bytes in order, zeroed where
// the key is NULL so that two rows NULL in the same keys compare equal. Only types of a fixed byte
// width
// (`Supports`); a VARCHAR key keeps the grouper until the layout of ADR 0027's S2 lands here.
//
// The hash is `KeyHashes` (group_table.h), which is what GroupTable::Partition, RouteRows and
// PrefixHashes agree through -- a group has to land in the same partition for every part, so the
// table cannot afford a hash of its own. Keys must already be normalized (NormalizeDoubleKey) as
// they are for the grouper, since the hash reads their bytes.
class GroupHashTable {
 public:
  // Whether a table can hold keys of these types: every one of a fixed byte width, BOOL excepted
  // (one bit, which the packing below does not hold).
  static bool Supports(const std::vector<arrow::TypeHolder>& types);

  // A table for keys of `types`, sized for `expected_groups` of them (0: a default) so that the
  // common case does not rehash -- which the grouper cannot be told at all, and whose doubling is
  // 5% of Q16's CPU. Memory comes from `pool` and is charged to `budget` (nullptr: uncharged).
  static arrow::Result<std::unique_ptr<GroupHashTable>> Make(std::vector<arrow::TypeHolder> types,
                                                             arrow::MemoryPool* pool,
                                                             MemoryBudget* budget,
                                                             int64_t expected_groups = 0);
  GroupHashTable(const GroupHashTable&) = delete;
  GroupHashTable& operator=(const GroupHashTable&) = delete;
  GroupHashTable(GroupHashTable&&) = delete;
  GroupHashTable& operator=(GroupHashTable&&) = delete;
  ~GroupHashTable() = default;

  // The group of each row of `keys`, which must be of the table's types and already normalized: the
  // id of the group it joins, new groups numbered from num_groups() up in first-seen order, as the
  // grouper numbers them. OutOfMemory past the budget's limit, and the table is then unchanged.
  arrow::Result<std::shared_ptr<arrow::UInt32Array>> Consume(const arrow::compute::ExecBatch& keys);

  [[nodiscard]] std::uint32_t num_groups() const { return num_groups_; }

  // Each group's keys, in group order: the normalized values the table was given, which is what the
  // grouper's GetUniques returns and what a partitioned merge re-groups through.
  [[nodiscard]] arrow::Result<arrow::compute::ExecBatch> GetUniques() const;

 private:
  GroupHashTable(std::vector<arrow::TypeHolder> types, std::vector<int> widths, std::size_t bitmap,
                 arrow::MemoryPool* pool);

  // Room for `capacity` slots, every one empty; the groups already in the table are reinserted by
  // their stored hashes, so growing never looks at a key again.
  arrow::Status Resize(std::size_t capacity);
  // The slot `hash` and `key` belong in: the one holding that group, or the first empty one of the
  // group of eight where the search ended. `found` says which.
  struct Probe {
    std::size_t slot = 0;
    bool found = false;
  };
  [[nodiscard]] Probe Find(std::uint64_t hash, const std::uint8_t* key) const;
  arrow::Status Reserve(std::uint32_t groups);

  std::vector<arrow::TypeHolder> types_;
  std::vector<int> widths_;  // per key, its byte width
  std::size_t bitmap_ = 0;   // the NULL bitmap's bytes at the head of a packed row
  std::size_t row_ = 0;      // a packed row's bytes: bitmap_ plus every width
  arrow::MemoryPool* pool_;
  MemoryReservation memory_;

  std::vector<std::uint8_t> tags_;     // one byte a slot: 0 empty, else 0x80 | 7 bits of the hash
  std::vector<std::uint32_t> slots_;   // the slot's group, kNoGroup when empty
  std::vector<std::uint8_t> keys_;     // row_ bytes a group, in group order
  std::vector<std::uint64_t> hashes_;  // a group's hash, so growing needs no key
  std::size_t mask_ = 0;               // capacity - 1
  std::uint32_t num_groups_ = 0;
};

}  // namespace antb1::exec
