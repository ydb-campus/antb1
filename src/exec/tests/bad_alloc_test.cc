// std::bad_alloc in the join hash table (docs/adr/0022-joins-and-query-blocks.md): an allocation of
// the C++ heap that fails anywhere in a call gives OutOfMemory, never an exception, and fails the
// build for good, with no part counted twice and nothing left in the budget.
//
// The allocation hook. This executable replaces the global operator new and delete, so it is not
// part of the shared antb1_exec_tests binary. On a thread where a FailAllocations is armed,
// operator new lets `skip` allocations through and then throws std::bad_alloc for the next `count`,
// as an exhausted heap would; other threads, and this one when nothing is armed, allocate with
// malloc. A test arms it around one call on the test thread and sweeps `skip` from 0 until the call
// goes through untouched, so that every allocation the call makes fails once. Under
// ThreadSanitizer the executable links without the sanitizer's own operator new
// (src/exec/CMakeLists.txt); the sanitizer still sees every allocation, through malloc and free.

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <utility>
#include <vector>

#include <arrow/api.h>
#include <arrow/util/future.h>
#include <arrow/util/thread_pool.h>
#include <gtest/gtest.h>

#include "antb1/exec/join_table.h"
#include "antb1/exec/memory_budget.h"
#include "antb1/exec/operator.h"
#include "antb1/plan/logical_plan.h"

#include "exec_test_util.h"

namespace antb1::exec {
namespace {

// What a FailAllocations armed: `skip` allocations go through, then `count` fail.
struct AllocationHook {
  std::int64_t skip = 0;
  std::int64_t count = 0;
  std::int64_t seen = 0;                          // allocations made since it was armed
  std::int64_t failed = 0;                        // and those that failed
  const std::function<void()>* before = nullptr;  // runs, unarmed, before the first failure
};

// The hook armed on this thread, if any.
constinit thread_local AllocationHook* armed_hook = nullptr;

// Whether the allocation operator new is making fails.
bool FailAllocation() {
  AllocationHook* const hook = armed_hook;
  if (hook == nullptr) {
    return false;
  }
  ++hook->seen;
  if (hook->skip > 0) {
    --hook->skip;
    return false;
  }
  if (hook->count == 0) {
    return false;
  }
  --hook->count;
  if (hook->failed++ == 0 && hook->before != nullptr) {
    armed_hook = nullptr;  // what it allocates goes through
    (*hook->before)();
    armed_hook = hook;
  }
  return true;
}

void* Allocate(std::size_t size) {
  if (FailAllocation()) {
    throw std::bad_alloc();
  }
  void* memory = std::malloc(size == 0 ? 1 : size);
  if (memory == nullptr) {
    throw std::bad_alloc();
  }
  return memory;
}

void* AllocateOrNull(std::size_t size) noexcept {
  try {
    return Allocate(size);
  } catch (...) {
    return nullptr;
  }
}

}  // namespace
}  // namespace antb1::exec

// The replaceable allocation and deallocation functions ([new.delete]) but the aligned ones, which
// keep the library's (or the sanitizer's) own pairs.
void* operator new(std::size_t size) { return antb1::exec::Allocate(size); }
void* operator new[](std::size_t size) { return antb1::exec::Allocate(size); }
void* operator new(std::size_t size, const std::nothrow_t& /*tag*/) noexcept {
  return antb1::exec::AllocateOrNull(size);
}
void* operator new[](std::size_t size, const std::nothrow_t& /*tag*/) noexcept {
  return antb1::exec::AllocateOrNull(size);
}
void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete[](void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t /*size*/) noexcept { std::free(memory); }
void operator delete[](void* memory, std::size_t /*size*/) noexcept { std::free(memory); }
void operator delete(void* memory, const std::nothrow_t& /*tag*/) noexcept { std::free(memory); }
void operator delete[](void* memory, const std::nothrow_t& /*tag*/) noexcept { std::free(memory); }

namespace antb1::exec {
namespace {

using plan::LogicalType;
using testing::Int64s;

constexpr int kThreads = 4;
constexpr std::size_t kParts = 4;
constexpr int64_t kRows = 6;  // per batch, two batches per part
constexpr int64_t kGiB = int64_t{1024} * 1024 * 1024;
// More than any call here allocates: a sweep that gets there never ends.
constexpr std::int64_t kMaxSkips = 100000;

// Arms the hook of this thread until it goes: `skip` allocations go through, then `count` fail
// with std::bad_alloc (0: none, it only counts), the first of them after `before` (if any) ran.
class FailAllocations {
 public:
  explicit FailAllocations(std::int64_t skip, std::int64_t count = 1,
                           std::function<void()> before = {})
      : before_(std::move(before)),
        hook_{.skip = skip,
              .count = count,
              .seen = 0,
              .failed = 0,
              .before = before_ ? &before_ : nullptr} {
    armed_hook = &hook_;
  }
  FailAllocations(const FailAllocations&) = delete;
  FailAllocations& operator=(const FailAllocations&) = delete;
  FailAllocations(FailAllocations&&) = delete;
  FailAllocations& operator=(FailAllocations&&) = delete;
  ~FailAllocations() { armed_hook = nullptr; }

  // The allocations made since it was armed, and those that failed.
  [[nodiscard]] std::int64_t seen() const { return hook_.seen; }
  [[nodiscard]] std::int64_t failed() const { return hook_.failed; }

 private:
  std::function<void()> before_;
  AllocationHook hook_;
};

// Runs every task on `pool`, whose Spawn sees the hook disarmed: Arrow's thread pool allocates
// there in a noexcept function of OpenTelemetry, where std::bad_alloc ends the process. Arrow's
// Submit around it (the task, the future) still meets the hook.
class UnhookedSpawns final : public arrow::internal::Executor {
 public:
  explicit UnhookedSpawns(arrow::internal::Executor* pool) : pool_(pool) {}

  int GetCapacity() override { return pool_->GetCapacity(); }

 protected:
  arrow::Status SpawnReal(arrow::internal::TaskHints hints, arrow::internal::FnOnce<void()> task,
                          arrow::StopToken stop_token, StopCallback&& stop_callback) override {
    AllocationHook* const hook = armed_hook;
    armed_hook = nullptr;
    arrow::Status spawned =
        pool_->Spawn(hints, std::move(task), std::move(stop_token), std::move(stop_callback));
    armed_hook = hook;
    return spawned;
  }

 private:
  arrow::internal::Executor* pool_;
};

// Calls attempt(skip) for skip = 0, 1, ... until an attempt reports that no allocation failed (its
// call made fewer than it let through): every allocation of the call fails once. A call that
// `allocates` fails one in the first attempt (else the hook is not in place); another, none.
template <class Attempt>
void Sweep(const Attempt& attempt, bool allocates = true) {
  for (std::int64_t skip = 0; skip < kMaxSkips; ++skip) {
    if (!attempt(skip)) {
      EXPECT_EQ(skip > 0, allocates) << "allocations failed in " << skip << " attempts";
      return;
    }
  }
  ADD_FAILURE() << "every attempt failed an allocation";
}

// A pool of kThreads threads that have all started: none starts during a test, as starting one
// allocates on the thread that submits.
std::shared_ptr<arrow::internal::ThreadPool> StartedPool() {
  auto made = arrow::internal::ThreadPool::Make(kThreads);
  EXPECT_TRUE(made.ok()) << made.status().ToString();
  std::shared_ptr<arrow::internal::ThreadPool> pool = made.ValueOrDie();
  std::mutex mu;
  std::condition_variable cv;
  int started = 0;
  std::vector<arrow::Future<>> tasks;
  tasks.reserve(kThreads);
  for (int i = 0; i < kThreads; ++i) {
    auto task = pool->Submit([&] {  // each waits for the others: each needs a thread of its own
      std::unique_lock lock(mu);
      ++started;
      cv.notify_all();
      cv.wait(lock, [&] { return started == kThreads; });
    });
    EXPECT_TRUE(task.ok()) << task.status().ToString();
    tasks.push_back(task.ValueOrDie());
  }
  for (const arrow::Future<>& task : tasks) {
    task.Wait();
  }
  return pool;
}

// ---- The join build ----

class JoinTableBadAllocTest : public testing::ExecTest {};

// A key value of the build input: `value` itself (dense keys, the direct layout) or spread out
// (hashed).
int64_t KeyOf(int64_t value, bool hashed) { return hashed ? (value * 7919) + 3 : value; }

// A build input: kParts parts of two batches of a BIGINT key k (a quarter of the rows repeat a key,
// a seventh have none) and a BIGINT id that numbers the rows.
struct BuildInput {
  std::shared_ptr<arrow::Schema> schema;
  std::vector<plan::BoundColumn> keys;
  std::vector<std::vector<Batch>> parts;
};

BuildInput Input(bool hashed) {
  BuildInput input{.schema = arrow::schema(
                       {arrow::field("k", arrow::int64()), arrow::field("id", arrow::int64())}),
                   .keys = {testing::Column(0, "k", LogicalType::kBigInt)},
                   .parts = {}};
  int64_t id = 0;
  for (std::size_t part = 0; part < kParts; ++part) {
    std::vector<Batch>& batches = input.parts.emplace_back();
    for (int b = 0; b < 2; ++b) {
      std::vector<std::optional<int64_t>> keys;
      std::vector<std::optional<int64_t>> ids;
      for (int64_t row = 0; row < kRows; ++row) {
        const int64_t value = id % 4 == 0 ? id / 4 : id;
        keys.push_back(id % 7 == 3 ? std::nullopt : std::optional(KeyOf(value, hashed)));
        ids.emplace_back(id);
        ++id;
      }
      batches.push_back(
          Batch{.data = arrow::RecordBatch::Make(input.schema, kRows, {Int64s(keys), Int64s(ids)}),
                .selection = nullptr});
    }
  }
  return input;
}

// The parts of `input`, appended with `budget` (nullptr: none) as their budget and pool.
std::vector<std::shared_ptr<const JoinBuildPart>> MakeParts(
    const BuildInput& input, const std::shared_ptr<const JoinBuildSpec>& spec,
    MemoryBudget* budget) {
  std::vector<std::shared_ptr<const JoinBuildPart>> parts;
  parts.reserve(input.parts.size());
  for (const std::vector<Batch>& batches : input.parts) {
    auto part = std::make_shared<JoinBuildPart>(spec, budget);
    for (const Batch& batch : batches) {
      const arrow::Status appended =
          part->Append(batch, budget != nullptr ? budget : arrow::default_memory_pool());
      EXPECT_TRUE(appended.ok()) << appended.ToString();
    }
    parts.push_back(std::move(part));
  }
  return parts;
}

// What a finished table holds: its layout, counts and flags, its rows as (chunk, row), the ids of
// each chunk, and the ids each probe key matches (every key of the input's range, and a NULL).
struct Snapshot {
  JoinTable::Layout layout = JoinTable::Layout::kHashed;
  int64_t input_rows = 0;
  int64_t null_key_rows = 0;
  int64_t num_rows = 0;
  bool unique = false;
  std::vector<std::pair<std::uint32_t, std::uint32_t>> rows;
  std::vector<std::vector<int64_t>> chunk_ids;
  std::vector<std::vector<int64_t>> matches;

  bool operator==(const Snapshot&) const = default;
};

Snapshot SnapshotOf(const JoinTable& table, bool hashed) {
  Snapshot snapshot{.layout = table.layout(),
                    .input_rows = table.input_rows(),
                    .null_key_rows = table.null_key_rows(),
                    .num_rows = table.num_rows(),
                    .unique = table.unique(),
                    .rows = {},
                    .chunk_ids = {},
                    .matches = {}};
  const auto ids_of = [&table](std::uint32_t chunk) -> const arrow::Int64Array& {
    return static_cast<const arrow::Int64Array&>(*table.chunks()[chunk]->column(1));
  };
  for (const JoinRowRef& ref : table.rows()) {
    snapshot.rows.emplace_back(ref.chunk, ref.row);
  }
  for (std::uint32_t chunk = 0; chunk < table.chunks().size(); ++chunk) {
    std::vector<int64_t>& ids = snapshot.chunk_ids.emplace_back();
    for (int64_t row = 0; row < ids_of(chunk).length(); ++row) {
      ids.push_back(ids_of(chunk).Value(row));
    }
  }
  std::vector<std::optional<int64_t>> keys;
  for (int64_t value = -1; value <= static_cast<int64_t>(kParts) * 2 * kRows; ++value) {
    keys.emplace_back(KeyOf(value, hashed));
  }
  keys.emplace_back(std::nullopt);
  const std::vector<std::shared_ptr<arrow::Array>> probe = {Int64s(keys)};
  std::vector<JoinMatches> found(keys.size());
  const arrow::Status status = table.Find(probe, nullptr, arrow::default_memory_pool(), found);
  EXPECT_TRUE(status.ok()) << status.ToString();
  for (const JoinMatches& matches : found) {
    std::vector<int64_t>& ids = snapshot.matches.emplace_back();
    for (std::uint32_t i = matches.begin; i < matches.end; ++i) {
      const JoinRowRef ref = table.rows()[i];
      ids.push_back(ids_of(ref.chunk).Value(ref.row));
    }
  }
  return snapshot;
}

// The table of `input` built without the hook, on this thread.
std::optional<Snapshot> Reference(const BuildInput& input, bool hashed) {
  auto spec = JoinBuildSpec::Make(input.schema, input.keys);
  EXPECT_TRUE(spec.ok()) << spec.status().ToString();
  const std::vector<std::shared_ptr<const JoinBuildPart>> parts = MakeParts(input, *spec, nullptr);
  auto builder = JoinTableBuilder::Make(*spec, kParts, nullptr, kThreads, nullptr);
  EXPECT_TRUE(builder.ok()) << builder.status().ToString();
  for (std::size_t p = 0; p < kParts; ++p) {
    EXPECT_TRUE((*builder)->Add(static_cast<int64_t>(p), parts[p]).ok());
  }
  auto table = (*builder)->Finish();
  EXPECT_TRUE(table.ok()) << table.status().ToString();
  if (!table.ok()) {
    return std::nullopt;
  }
  EXPECT_EQ((*table)->layout(), hashed ? JoinTable::Layout::kHashed : JoinTable::Layout::kDirect);
  return SnapshotOf(**table, hashed);
}

// The orders the tests add the parts in: in part order (each Add releases its part), and one where
// two Adds release nothing and another three parts.
std::vector<std::vector<std::size_t>> Orders() { return {{0, 1, 2, 3}, {2, 3, 0, 1}}; }

// Whether the Add at `position` of `order` releases a part: the part it adds is the next one in
// part order once the parts before it in `order` are added.
bool Releases(const std::vector<std::size_t>& order, std::size_t position) {
  std::vector<bool> added(order.size(), false);
  std::size_t next = 0;
  for (std::size_t i = 0; i <= position; ++i) {
    added[order[i]] = true;
    const std::size_t before = next;
    while (next < added.size() && added[next]) {
      ++next;
    }
    if (i == position) {
      return next > before;
    }
  }
  return false;
}

// JoinBuildSpec::Make and JoinTableBuilder::Make without memory: OutOfMemory, never an exception; a
// builder made goes on to the table an untouched build makes. Nothing is left in the budget.
TEST_F(JoinTableBadAllocTest, MakingASpecOrABuilderRunsOutOfMemoryCleanly) {
  const BuildInput input = Input(/*hashed=*/false);
  const std::optional<Snapshot> reference = Reference(input, /*hashed=*/false);
  ASSERT_TRUE(reference.has_value());
  Sweep([&](std::int64_t skip) {
    std::shared_ptr<arrow::Schema> schema = input.schema;
    std::vector<plan::BoundColumn> keys = input.keys;
    std::optional<arrow::Result<std::shared_ptr<const JoinBuildSpec>>> spec;
    bool fired = false;
    {
      const FailAllocations fail(skip);
      spec.emplace(JoinBuildSpec::Make(std::move(schema), std::move(keys)));
      fired = fail.failed() > 0;
    }
    if (!spec->ok()) {
      EXPECT_TRUE(fired) << spec->status().ToString();
      EXPECT_TRUE(spec->status().IsOutOfMemory()) << spec->status().ToString();
    } else {
      EXPECT_TRUE((**spec)->direct_candidate());
    }
    return fired;
  });
  auto spec = JoinBuildSpec::Make(input.schema, input.keys);
  ASSERT_TRUE(spec.ok()) << spec.status().ToString();
  const auto pool = StartedPool();
  UnhookedSpawns spawns(pool.get());
  for (arrow::internal::Executor* executor :
       std::vector<arrow::internal::Executor*>{nullptr, &spawns}) {
    SCOPED_TRACE(executor == nullptr ? "here" : "pool");
    Sweep([&](std::int64_t skip) {
      MemoryBudget budget(std::nullopt);
      bool fired = false;
      {
        std::shared_ptr<const JoinBuildSpec> shared = *spec;
        std::optional<arrow::Result<std::unique_ptr<JoinTableBuilder>>> builder;
        {
          const FailAllocations fail(skip);
          builder.emplace(
              JoinTableBuilder::Make(std::move(shared), kParts, executor, kThreads, &budget));
          fired = fail.failed() > 0;
        }
        if (!builder->ok()) {
          EXPECT_TRUE(fired) << builder->status().ToString();
          EXPECT_TRUE(builder->status().IsOutOfMemory()) << builder->status().ToString();
        } else {
          const std::vector<std::shared_ptr<const JoinBuildPart>> parts =
              MakeParts(input, *spec, &budget);
          for (std::size_t p = 0; p < kParts; ++p) {
            EXPECT_TRUE((**builder)->Add(static_cast<int64_t>(p), parts[p]).ok());
          }
          auto table = (**builder)->Finish();
          EXPECT_TRUE(table.ok()) << table.status().ToString();
          if (table.ok()) {
            EXPECT_EQ(SnapshotOf(**table, /*hashed=*/false), *reference);
          }
        }
      }
      EXPECT_EQ(budget.bytes_allocated(), 0);
      return fired;
    });
  }
}

// An allocation that fails in an Add, each in turn (in Release, in the partition lanes, in a merge
// on this thread, in Arrow's Submit): that Add returns OutOfMemory, and every later Add, Merged()
// and Finish() returns the same failure, so that no part is released again and nothing is counted
// twice; an Add that goes through leaves the table an untouched build makes. Nothing is left in
// the budget.
TEST_F(JoinTableBadAllocTest, AnAddThatRunsOutOfMemoryFailsTheBuild) {
  const auto pool = StartedPool();
  UnhookedSpawns spawns(pool.get());
  for (const bool hashed : {false, true}) {
    SCOPED_TRACE(hashed);
    const BuildInput input = Input(hashed);
    const std::optional<Snapshot> reference = Reference(input, hashed);
    ASSERT_TRUE(reference.has_value());
    auto spec = JoinBuildSpec::Make(input.schema, input.keys);
    ASSERT_TRUE(spec.ok()) << spec.status().ToString();
    for (arrow::internal::Executor* executor :
         std::vector<arrow::internal::Executor*>{nullptr, &spawns}) {
      SCOPED_TRACE(executor == nullptr ? "here" : "pool");
      for (const std::vector<std::size_t>& order : Orders()) {
        for (std::size_t armed = 0; armed < order.size(); ++armed) {
          SCOPED_TRACE(::testing::Message() << "order " << order[0] << ", Add #" << armed);
          Sweep(
              [&](std::int64_t skip) {
                MemoryBudget budget(std::nullopt);
                bool fired = false;
                {
                  const std::vector<std::shared_ptr<const JoinBuildPart>> parts =
                      MakeParts(input, *spec, &budget);
                  auto builder = JoinTableBuilder::Make(*spec, kParts, executor, kThreads, &budget);
                  EXPECT_TRUE(builder.ok()) << builder.status().ToString();
                  if (!builder.ok()) {
                    return false;
                  }
                  std::optional<arrow::Status> failure;
                  for (std::size_t i = 0; i < order.size(); ++i) {
                    const auto part = static_cast<int64_t>(order[i]);
                    arrow::Status added;
                    if (i == armed) {
                      const FailAllocations fail(skip);
                      added = (*builder)->Add(part, parts[order[i]]);
                      fired = fail.failed() > 0;
                    } else {
                      added = (*builder)->Add(part, parts[order[i]]);
                    }
                    if (failure.has_value()) {
                      EXPECT_EQ(added.ToString(), failure->ToString()) << part;
                    } else if (!added.ok()) {
                      EXPECT_TRUE(fired) << added.ToString();
                      EXPECT_TRUE(added.IsOutOfMemory()) << added.ToString();
                      failure = added;
                    }
                  }
                  if (failure.has_value()) {
                    EXPECT_EQ((*builder)->Merged().ToString(), failure->ToString());
                    EXPECT_EQ((*builder)->Finish().status().ToString(), failure->ToString());
                  } else {
                    auto table = (*builder)->Finish();
                    EXPECT_TRUE(table.ok()) << table.status().ToString();
                    if (table.ok()) {
                      EXPECT_EQ(SnapshotOf(**table, hashed), *reference);
                    }
                  }
                }
                EXPECT_EQ(budget.bytes_allocated(), 0);
                return fired;
              },
              Releases(order, armed));
        }
      }
    }
  }
}

// An allocation that fails in Finish, each in turn (the table's containers, the partitions'
// directories on this thread, Arrow's Submit of a partition's task): OutOfMemory, and a second
// Finish is Invalid; a Finish that goes through gives the table an untouched build gives. Nothing
// is left in the budget.
TEST_F(JoinTableBadAllocTest, AFinishThatRunsOutOfMemoryFails) {
  const auto pool = StartedPool();
  UnhookedSpawns spawns(pool.get());
  for (const bool hashed : {false, true}) {
    SCOPED_TRACE(hashed);
    const BuildInput input = Input(hashed);
    const std::optional<Snapshot> reference = Reference(input, hashed);
    ASSERT_TRUE(reference.has_value());
    auto spec = JoinBuildSpec::Make(input.schema, input.keys);
    ASSERT_TRUE(spec.ok()) << spec.status().ToString();
    for (arrow::internal::Executor* executor :
         std::vector<arrow::internal::Executor*>{nullptr, &spawns}) {
      SCOPED_TRACE(executor == nullptr ? "here" : "pool");
      Sweep([&](std::int64_t skip) {
        MemoryBudget budget(std::nullopt);
        bool fired = false;
        {
          const std::vector<std::shared_ptr<const JoinBuildPart>> parts =
              MakeParts(input, *spec, &budget);
          auto builder = JoinTableBuilder::Make(*spec, kParts, executor, kThreads, &budget);
          EXPECT_TRUE(builder.ok()) << builder.status().ToString();
          if (!builder.ok()) {
            return false;
          }
          for (std::size_t p = 0; p < kParts; ++p) {
            EXPECT_TRUE((*builder)->Add(static_cast<int64_t>(p), parts[p]).ok());
          }
          EXPECT_TRUE((*builder)->Merged().ok());
          std::optional<arrow::Result<std::shared_ptr<const JoinTable>>> table;
          {
            const FailAllocations fail(skip);
            table.emplace((*builder)->Finish());
            fired = fail.failed() > 0;
          }
          if (table->ok()) {
            EXPECT_EQ(SnapshotOf(***table, hashed), *reference);
          } else {
            EXPECT_TRUE(fired) << table->status().ToString();
            EXPECT_TRUE(table->status().IsOutOfMemory()) << table->status().ToString();
          }
          EXPECT_TRUE((*builder)->Finish().status().IsInvalid());
        }
        EXPECT_EQ(budget.bytes_allocated(), 0);
        return fired;
      });
    }
  }
}

// A merge that failed (the budget is full), then no memory to copy its failure: Merged() returns
// OutOfMemory, never an exception, and the build keeps its failure, which the next calls return.
// Nothing is left in the budget. Finish is not swept here: it returns an arrow::Result, whose
// constructor from a Status copies it in a noexcept function (Arrow's), where std::bad_alloc ends
// the process.
TEST_F(JoinTableBadAllocTest, NoMemoryToReportAMergeFailure) {
  const BuildInput input = Input(/*hashed=*/false);
  auto spec = JoinBuildSpec::Make(input.schema, input.keys);
  ASSERT_TRUE(spec.ok()) << spec.status().ToString();
  Sweep([&](std::int64_t skip) {
    MemoryBudget budget(kGiB);
    bool fired = false;
    {
      const std::vector<std::shared_ptr<const JoinBuildPart>> parts =
          MakeParts(input, *spec, &budget);
      auto builder = JoinTableBuilder::Make(*spec, kParts, nullptr, kThreads, &budget);
      EXPECT_TRUE(builder.ok()) << builder.status().ToString();
      if (!builder.ok()) {
        return false;
      }
      const int64_t pinned = kGiB - budget.bytes_allocated();
      EXPECT_TRUE(budget.Reserve(pinned).ok());  // no room for the partitions' runs
      const arrow::Status failure = (*builder)->Add(0, parts[0]);
      EXPECT_TRUE(failure.IsOutOfMemory()) << failure.ToString();
      arrow::Status reported;
      {
        const FailAllocations fail(skip);
        reported = (*builder)->Merged();
        fired = fail.failed() > 0;
      }
      EXPECT_TRUE(reported.IsOutOfMemory()) << reported.ToString();
      if (!fired) {
        EXPECT_EQ(reported.ToString(), failure.ToString());
      }
      EXPECT_EQ((*builder)->Merged().ToString(), failure.ToString());
      EXPECT_EQ((*builder)->Add(1, parts[1]).ToString(), failure.ToString());
      EXPECT_EQ((*builder)->Finish().status().ToString(), failure.ToString());
      budget.Release(pinned);
    }
    EXPECT_EQ(budget.bytes_allocated(), 0);
    return fired;
  });
}

}  // namespace
}  // namespace antb1::exec
