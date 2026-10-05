// std::bad_alloc in ForEach, the partition lanes and the join hash table
// (docs/adr/0022-joins-and-query-blocks.md): an allocation of the C++ heap that fails anywhere in a
// call gives OutOfMemory, never an exception (ForEach lets one out only when not even its
// OutOfMemory can be made, and only once its tasks ended); it stops the lanes as the serial order
// would, and fails a build for good, with no part counted twice and nothing left in the budget.
//
// The allocation hook. This executable replaces the global operator new and delete, so it is not
// part of the shared antb1_exec_tests binary. On a thread where a FailAllocations is armed,
// operator new lets `skip` allocations through and then throws std::bad_alloc for the next `count`,
// as an exhausted heap would; other threads, and this one when nothing is armed, allocate with
// malloc. A test arms it around one call on the test thread and sweeps `skip` from 0 until the call
// goes through untouched, so that every allocation the call makes fails once. Under
// ThreadSanitizer the executable links without the sanitizer's own operator new
// (src/exec/CMakeLists.txt); the sanitizer still sees every allocation, through malloc and free.

#include <algorithm>
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

#include "../partition_lanes.h"
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
using testing::ThrowingExecutor;

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

// ---- ForEach ----

// Runs every task on `pool`, except that spawn number `fail_at` (from 0) fails with `failure`:
// `before_fail` runs just before it returns.
class FailingExecutor final : public arrow::internal::Executor {
 public:
  FailingExecutor(arrow::internal::Executor* pool, int fail_at, arrow::Status failure,
                  std::function<void()> before_fail)
      : pool_(pool),
        fail_at_(fail_at),
        failure_(std::move(failure)),
        before_fail_(std::move(before_fail)) {}

  int GetCapacity() override { return pool_->GetCapacity(); }

 protected:
  arrow::Status SpawnReal(arrow::internal::TaskHints hints, arrow::internal::FnOnce<void()> task,
                          arrow::StopToken stop_token, StopCallback&& stop_callback) override {
    if (spawns_++ == fail_at_) {
      arrow::Status failure = failure_;
      before_fail_();
      return failure;
    }
    return pool_->Spawn(hints, std::move(task), std::move(stop_token), std::move(stop_callback));
  }

 private:
  arrow::internal::Executor* pool_;
  int fail_at_;
  arrow::Status failure_;
  std::function<void()> before_fail_;
  int spawns_ = 0;  // Submit is called from one thread
};

// A task that cannot be submitted (Submit throws std::bad_alloc), and then no memory for its
// OutOfMemory either: the std::bad_alloc leaves ForEach only once every task submitted has ended,
// as they use its frame and `fn`; the later tasks never run. The tasks start their work only when
// the second std::bad_alloc is thrown, and it takes them a while.
TEST(ForEachBadAllocTest, AnExceptionLeavesOnlyOnceTheTasksHaveEnded) {
  const auto pool = StartedPool();
  constexpr std::size_t kTasks = 8;
  constexpr std::size_t kThrowAt = 6;  // more tasks before it than threads: some wait to start
  constexpr int kSteps = 1 << 20;      // the work of a task
  std::mutex mu;
  std::condition_variable cv;
  bool thrown = false;
  const std::function<void()> throw_again = [&] {
    {
      const std::scoped_lock lock(mu);
      thrown = true;
    }
    cv.notify_all();
  };
  std::optional<FailAllocations> fail;  // armed by the throwing Submit, on this thread
  ThrowingExecutor executor(pool.get(), static_cast<int>(kThrowAt), [&] {
    fail.emplace(/*skip=*/0, /*count=*/1, throw_again);  // the OutOfMemory of the throw fails
  });
  std::vector<std::uint64_t> work(kTasks, 0);  // each task writes only its own
  std::vector<int> ended(kTasks, 0);
  const std::function<arrow::Status(std::size_t)> task = [&](std::size_t i) {
    {
      std::unique_lock lock(mu);
      cv.wait(lock, [&] { return thrown; });
    }
    std::uint64_t value = i + 1;
    for (int step = 0; step < kSteps; ++step) {
      value = (value * 6364136223846793005U) + 1442695040888963407U;
    }
    work[i] = value;
    ended[i] = 1;
    return arrow::Status::OK();
  };
  std::optional<arrow::Status> returned;
  bool threw = false;
  try {
    returned = ForEach(&executor, kTasks, task);
  } catch (const std::bad_alloc&) {
    threw = true;
  }
  const std::int64_t failed = fail.has_value() ? fail->failed() : 0;
  fail.reset();
  EXPECT_TRUE(threw) << (returned.has_value() ? returned->ToString() : "");
  EXPECT_EQ(failed, 1);
  for (std::size_t i = 0; i < kTasks; ++i) {
    EXPECT_EQ(ended[i], i < kThrowAt ? 1 : 0) << i;
    EXPECT_EQ(work[i] != 0, i < kThrowAt) << i;
  }
  EXPECT_EQ(executor.spawns(), static_cast<int>(kThrowAt) + 1);
}

// A Submit that fails with a status, and no memory left: ForEach waits for the tasks before it and
// returns that status, which it moves, so it allocates nothing more and nothing fails.
TEST(ForEachBadAllocTest, AFailedSubmitAllocatesNothingMore) {
  const auto pool = StartedPool();
  constexpr std::size_t kTasks = 8;
  constexpr int kFailAt = 3;
  const arrow::Status refused = arrow::Status::IOError("no task");
  // The first Result<T> made sets up Arrow's constant status of an empty Result, which moving a
  // status out of a Result swaps in: once in a process, and here.
  EXPECT_FALSE(arrow::Result<int>().ok());
  // Arrow's Submit copies the failure into the Result it returns: those allocations go through.
  std::int64_t copy = 0;
  {
    const FailAllocations count(/*skip=*/0, /*count=*/0);
    const arrow::Result<arrow::Future<>> copied(refused);
    copy = count.seen();
  }
  std::optional<FailAllocations> fail;
  FailingExecutor executor(pool.get(), kFailAt, refused, [&] { fail.emplace(copy); });
  std::vector<int> ran(kTasks, 0);  // each task writes only its own
  const std::function<arrow::Status(std::size_t)> task = [&ran](std::size_t i) {
    ran[i] = 1;
    return arrow::Status::OK();
  };
  std::optional<arrow::Status> returned;
  try {
    returned = ForEach(&executor, kTasks, task);
  } catch (const std::bad_alloc&) {
  }
  const std::int64_t failed = fail.has_value() ? fail->failed() : -1;
  fail.reset();
  ASSERT_TRUE(returned.has_value()) << "std::bad_alloc";
  EXPECT_EQ(returned->ToString(), refused.ToString());
  EXPECT_EQ(failed, 0);
  for (std::size_t i = 0; i < kTasks; ++i) {
    EXPECT_EQ(ran[i], std::cmp_less(i, kFailAt) ? 1 : 0) << i;
  }
}

// ---- The partition lanes ----

// A lane that cannot queue a part (std::bad_alloc in its queue) while it holds earlier parts merges
// them first: the failure of an earlier part there wins, whatever the timing, and Add returns it.
// The lane holds its parts while part 0's merge waits; the allocation that fails is the first one
// a part's queuing makes after the part's own, when the lane's queue needs more memory, and it
// lets part 0's merge go on.
TEST(PartitionLanesBadAllocTest, ALaneThatCannotQueueAPartStillMergesThePartsBeforeIt) {
  const auto pool = StartedPool();
  UnhookedSpawns spawns(pool.get());
  constexpr int64_t kMaxParts = 100000;
  std::mutex mu;
  std::condition_variable cv;
  bool open = false;
  const std::function<void()> open_gate = [&] {
    {
      const std::scoped_lock lock(mu);
      open = true;
    }
    cv.notify_all();
  };
  PartitionLanes lanes(1, &spawns, kMaxParts);
  struct OpenAtExit {  // so that no lane waits when a check ends the test early
    const std::function<void()>& open;
    ~OpenAtExit() { open(); }
  };
  const OpenAtExit open_at_exit{open_gate};
  ASSERT_TRUE(lanes
                  .Add(0,
                       [&](std::size_t) {
                         std::unique_lock lock(mu);
                         cv.wait(lock, [&] { return open; });
                         return arrow::Status::OK();
                       })
                  .ok());
  ASSERT_TRUE(lanes.Add(1, [](std::size_t) { return arrow::Status::Invalid("part 1"); }).ok());
  arrow::Status added;
  for (int64_t part = 2;; ++part) {
    ASSERT_LT(part, kMaxParts) << "queuing a part never allocated";
    PartitionLanes::Merge merge = [](std::size_t) { return arrow::Status::OK(); };
    bool fired = false;
    {
      const FailAllocations fail(/*skip=*/1, /*count=*/1, open_gate);
      added = lanes.Add(part, std::move(merge));
      fired = fail.failed() > 0;
    }
    if (fired) {
      break;
    }
    ASSERT_TRUE(added.ok()) << added.ToString();
  }
  EXPECT_EQ(added.ToString(), arrow::Status::Invalid("part 1").ToString());
  EXPECT_EQ(lanes.Finish().ToString(), added.ToString());
  EXPECT_EQ(lanes.pending(), 0);
}

// An allocation that fails in an Add, each in turn (the part, a lane's queue, Arrow's Submit of a
// lane's task; without an executor, a merge on this thread): Add returns OutOfMemory, and so do
// every later Add and Finish; every lane has merged the parts before, in order, and none after;
// every merge function is released. An Add that goes through has every lane merge the part.
TEST(PartitionLanesBadAllocTest, AnAddThatRunsOutOfMemoryStopsTheLanes) {
  const auto pool = StartedPool();
  UnhookedSpawns spawns(pool.get());
  constexpr std::size_t kLanes = 8;
  constexpr int64_t kAdded = 6;
  for (arrow::internal::Executor* executor :
       std::vector<arrow::internal::Executor*>{nullptr, &spawns}) {
    SCOPED_TRACE(executor == nullptr ? "here" : "pool");
    for (int64_t armed = 0; armed < kAdded; ++armed) {
      SCOPED_TRACE(armed);
      Sweep([&](std::int64_t skip) {
        // Each lane writes only its own; a merge allocates once, a part it keeps.
        std::vector<std::vector<std::unique_ptr<int64_t>>> seen(kLanes);
        for (std::vector<std::unique_ptr<int64_t>>& merged : seen) {
          merged.reserve(kAdded);
        }
        const auto held = std::make_shared<int>(0);
        bool fired = false;
        {
          PartitionLanes lanes(kLanes, executor, /*max_pending=*/2);
          std::optional<arrow::Status> failure;
          int64_t failed_part = kAdded;
          for (int64_t part = 0; part < kAdded; ++part) {
            PartitionLanes::Merge merge = [&seen, held, part](std::size_t lane) {
              seen[lane].push_back(std::make_unique<int64_t>(part));
              return arrow::Status::OK();
            };
            arrow::Status added;
            if (part == armed) {
              const FailAllocations fail(skip);
              added = lanes.Add(part, std::move(merge));
              fired = fail.failed() > 0;
            } else {
              added = lanes.Add(part, std::move(merge));
            }
            if (failure.has_value()) {
              EXPECT_EQ(added.ToString(), failure->ToString()) << part;
            } else if (!added.ok()) {
              EXPECT_TRUE(fired) << added.ToString();
              EXPECT_TRUE(added.IsOutOfMemory()) << added.ToString();
              failure = added;
              failed_part = part;
            }
          }
          EXPECT_EQ(lanes.Finish().ToString(), failure.has_value() ? failure->ToString() : "OK");
          EXPECT_EQ(lanes.pending(), 0);
          for (std::size_t lane = 0; lane < kLanes; ++lane) {
            const std::vector<std::unique_ptr<int64_t>>& merged = seen[lane];
            EXPECT_GE(std::ssize(merged), failed_part) << lane;
            EXPECT_LE(std::ssize(merged), std::min(failed_part + 1, kAdded)) << lane;
            for (std::size_t i = 0; i < merged.size(); ++i) {
              EXPECT_EQ(*merged[i], static_cast<int64_t>(i)) << lane;
            }
          }
        }
        EXPECT_EQ(held.use_count(), 1) << "every merge function released";
        return fired;
      });
    }
  }
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

// The parts of `input` added in `order` to a build on `executor`, the Add at `armed` with
// `failures` allocations failing in a row after `skip`: once an Add failed (OutOfMemory), every
// later call returns its failure (with more than one failing, an OutOfMemory); with none failed, the
// table is `reference`. Nothing is left in the budget. Whether an allocation failed.
bool AddParts(const BuildInput& input, const std::shared_ptr<const JoinBuildSpec>& spec,
              const Snapshot& reference, bool hashed, arrow::internal::Executor* executor,
              const std::vector<std::size_t>& order, std::size_t armed, std::int64_t failures,
              std::int64_t skip) {
  const auto expect_failure = [failures](const arrow::Status& status,
                                         const arrow::Status& failure) {
    if (failures == 1) {
      EXPECT_EQ(status.ToString(), failure.ToString());
    } else {
      EXPECT_TRUE(status.IsOutOfMemory()) << status.ToString();
    }
  };
  MemoryBudget budget(std::nullopt);
  bool fired = false;
  {
    const std::vector<std::shared_ptr<const JoinBuildPart>> parts =
        MakeParts(input, spec, &budget);
    auto builder = JoinTableBuilder::Make(spec, kParts, executor, kThreads, &budget);
    EXPECT_TRUE(builder.ok()) << builder.status().ToString();
    if (!builder.ok()) {
      return false;
    }
    std::optional<arrow::Status> failure;
    for (std::size_t i = 0; i < order.size(); ++i) {
      const auto part = static_cast<int64_t>(order[i]);
      arrow::Status added;
      if (i == armed) {
        const FailAllocations fail(skip, failures);
        added = (*builder)->Add(part, parts[order[i]]);
        fired = fail.failed() > 0;
      } else {
        added = (*builder)->Add(part, parts[order[i]]);
      }
      if (failure.has_value()) {
        expect_failure(added, *failure);
      } else if (!added.ok()) {
        EXPECT_TRUE(fired) << added.ToString();
        EXPECT_TRUE(added.IsOutOfMemory()) << added.ToString();
        failure = added;
      }
    }
    if (failure.has_value()) {
      expect_failure((*builder)->Merged(), *failure);
      expect_failure((*builder)->Finish().status(), *failure);
    } else {
      auto table = (*builder)->Finish();
      EXPECT_TRUE(table.ok()) << table.status().ToString();
      if (table.ok()) {
        EXPECT_EQ(SnapshotOf(**table, hashed), reference);
      }
    }
  }
  EXPECT_EQ(budget.bytes_allocated(), 0);
  return fired;
}

// An allocation that fails in an Add, each in turn (in Release, in the partition lanes, in a merge
// on this thread, in Arrow's Submit): that Add returns OutOfMemory, and every later Add, Merged()
// and Finish() returns the same failure, so that no part is released again and nothing is counted
// twice; an Add that goes through leaves the table an untouched build makes. With two failing in a
// row (the second while the first is handled, as when its OutOfMemory is made, so that Release
// throws once it has counted its part), the build fails for good too: every later call fails with
// OutOfMemory. Nothing is left in the budget.
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
          for (const std::int64_t failures : {1, 2}) {
            SCOPED_TRACE(::testing::Message() << "order " << order[0] << ", Add #" << armed
                                              << ", " << failures << " failing");
            Sweep(
                [&](std::int64_t skip) {
                  return AddParts(input, *spec, *reference, hashed, executor, order, armed,
                                  failures, skip);
                },
                Releases(order, armed));
          }
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

// A release that fails (std::bad_alloc while Release makes its merge function) while the merges of
// an earlier part fail on the executor, not known yet: the earlier part's failure is the build's,
// as in the serial order, from that Add on. The pool's only thread waits until the release fails,
// so part 0's merges run only then.
TEST_F(JoinTableBadAllocTest, AnEarlierMergeFailureWinsOverALaterFailedRelease) {
  const BuildInput input = Input(/*hashed=*/false);
  auto spec = JoinBuildSpec::Make(input.schema, input.keys);
  ASSERT_TRUE(spec.ok()) << spec.status().ToString();
  auto made = arrow::internal::ThreadPool::Make(1);
  ASSERT_TRUE(made.ok()) << made.status().ToString();
  const std::shared_ptr<arrow::internal::ThreadPool> pool = *made;
  std::mutex mu;
  std::condition_variable cv;
  bool open = false;
  const std::function<void()> open_gate = [&] {
    {
      const std::scoped_lock lock(mu);
      open = true;
    }
    cv.notify_all();
  };
  auto blocker = pool->Submit([&] {
    std::unique_lock lock(mu);
    cv.wait(lock, [&] { return open; });
  });
  ASSERT_TRUE(blocker.ok()) << blocker.status().ToString();
  UnhookedSpawns spawns(pool.get());
  MemoryBudget budget(kGiB);
  {
    const std::vector<std::shared_ptr<const JoinBuildPart>> parts =
        MakeParts(input, *spec, &budget);
    auto builder = JoinTableBuilder::Make(*spec, kParts, &spawns, kThreads, &budget);
    ASSERT_TRUE(builder.ok()) << builder.status().ToString();
    const int64_t pinned = kGiB - budget.bytes_allocated();
    ASSERT_TRUE(budget.Reserve(pinned).ok());  // no room for the partitions' runs
    const arrow::Status queued = (*builder)->Add(0, parts[0]);
    arrow::Status added;
    std::int64_t failed = 0;
    {
      const FailAllocations fail(/*skip=*/0, /*count=*/1, open_gate);  // part 1's merge function
      added = (*builder)->Add(1, parts[1]);
      failed = fail.failed();
    }
    open_gate();  // in case nothing failed: the pool's thread goes on
    EXPECT_TRUE(queued.ok()) << queued.ToString();
    EXPECT_EQ(failed, 1);
    EXPECT_TRUE(added.IsOutOfMemory()) << added.ToString();
    EXPECT_NE(added.message(), "out of memory in a join build");  // not the release's
    EXPECT_EQ((*builder)->Merged().ToString(), added.ToString());
    EXPECT_EQ((*builder)->Add(2, parts[2]).ToString(), added.ToString());
    EXPECT_EQ((*builder)->Finish().status().ToString(), added.ToString());
    builder->reset();
    budget.Release(pinned);
  }
  blocker->Wait();
  EXPECT_EQ(budget.bytes_allocated(), 0);
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
