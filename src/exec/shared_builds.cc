#include "shared_builds.h"

#include <cstdint>
#include <memory>
#include <new>
#include <utility>

#include <arrow/record_batch.h>
#include <arrow/result.h>
#include <arrow/status.h>

#include "antb1/exec/operator.h"
#include "antb1/exec/profile.h"
#include "antb1/plan/table.h"

namespace antb1::exec {
namespace {

arrow::Result<bool> FillTable(Operator& producer, MaterializedTable& table,
                              const ExecContext& ctx) {
  while (true) {
    ARROW_ASSIGN_OR_RAISE(const Batch batch, producer.Next());
    if (batch.end()) {
      return true;
    }
    ARROW_ASSIGN_OR_RAISE(auto rows, Materialize(batch, ctx.pool));
    if (rows->num_rows() > 0) {
      ARROW_ASSIGN_OR_RAISE(const bool kept, table.TryAppend(std::move(rows)));
      if (!kept) {
        return false;
      }
    }
  }
}

arrow::Result<bool> Prepare(SharedBuild& build, ExecContext& ctx) {
  build.table->Reset(ctx.budget);
  auto producer = std::move(build.first_producer);
  bool kept = false;
  try {
    if (producer == nullptr) {
      auto rebuilt = build.make_producer();
      if (!rebuilt.ok()) {
        return false;
      }
      producer = std::move(*rebuilt);
    }
    const arrow::Status opened = producer->Open(ctx);
    if (opened.ok()) {
      const auto filled = FillTable(*producer, *build.table, ctx);
      kept = filled.ok() && *filled;
    }
    // A producer can fail when its original consumers would have skipped it. Let the original
    // plan decide which errors are visible, in the order it would have encountered them.
  } catch (const std::bad_alloc&) {
    // Retaining the result can also exhaust allocations outside the query budget.
    kept = false;
  }
  if (producer != nullptr) {
    ARROW_RETURN_NOT_OK(producer->Close());
  }
  if (kept) {
    build.table->Finish();
    return true;
  }
  return false;
}

}  // namespace

arrow::Status SharedBuildsOperator::Open(ExecContext& ctx) {
  use_fallback_ = false;
  int64_t shared_rows = 0;
  for (SharedBuild& build : builds_) {
    const ProfileTimer timer(profile(), "shared");
    const auto kept = Prepare(build, ctx);
    if (!kept.ok()) {
      ReleaseBuilds();
      return kept.status();
    }
    if (!*kept) {
      ReleaseBuilds();
      ARROW_RETURN_NOT_OK(PrepareFallback(ctx));
      break;
    }
    shared_rows += build.table->exact_row_count().value_or(0);
  }
  if (!use_fallback_ && profile() != nullptr) {
    profile()->Add("shared_rows", MetricUnit::kCount, shared_rows);
  }
  const arrow::Status opened = ActiveInput().Open(ctx);
  if (!opened.ok()) {
    static_cast<void>(Close());
  }
  return opened;
}

arrow::Status SharedBuildsOperator::PrepareFallback(ExecContext& ctx) {
  // No consumer has opened yet. Drop scan validation state from the abandoned producers as well
  // as their tables, so the original plan only validates the inputs it actually needed.
  try {
    if (ctx.scans != nullptr) {
      ctx.scans = std::make_shared<plan::ScanContext>();
    }
    if (unshared_ == nullptr) {
      ProfileNode* fallback_profile =
          profile() == nullptr ? nullptr : profile()->Child(profile()->children().size());
      ARROW_ASSIGN_OR_RAISE(unshared_, fallback_(fallback_profile));
    }
    use_fallback_ = true;
    if (profile() != nullptr) {
      profile()->Add("shared_fallbacks", MetricUnit::kCount, 1);
    }
    return arrow::Status::OK();
  } catch (const std::bad_alloc&) {
    return arrow::Status::OutOfMemory("out of memory preparing the unshared plan");
  }
}

arrow::Result<Batch> SharedBuildsOperator::Next() { return ActiveInput().Next(); }

arrow::Status SharedBuildsOperator::Close() {
  const arrow::Status status = ActiveInput().Close();
  ReleaseBuilds();
  return status;
}

void SharedBuildsOperator::ReleaseBuilds() {
  for (SharedBuild& build : builds_) {
    build.first_producer.reset();
    build.table->Release();
  }
}

}  // namespace antb1::exec
