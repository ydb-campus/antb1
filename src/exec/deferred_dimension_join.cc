#include "deferred_dimension_join.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <optional>
#include <utility>
#include <vector>

#include <arrow/record_batch.h>
#include <arrow/status.h>
#include <arrow/stl_allocator.h>
#include <arrow/type.h>
#include <arrow/util/byte_size.h>

#include "antb1/exec/profile.h"
#include "antb1/plan/shared_sub_plans.h"
#include "antb1/plan/table.h"

#include "key_table.h"
#include "profiled_operator.h"
#include "scan_finish.h"

namespace antb1::exec {
namespace {

bool SameTypes(const arrow::Schema& first, const arrow::Schema& second) {
  if (first.num_fields() != second.num_fields()) {
    return false;
  }
  for (int i = 0; i < first.num_fields(); ++i) {
    if (!first.field(i)->type()->Equals(*second.field(i)->type())) {
      return false;
    }
  }
  return true;
}

class DeferredDimensionJoinOperator final : public Operator {
 public:
  DeferredDimensionJoinOperator(plan::LogicalPlan fast, plan::LogicalPlan original,
                                DimensionPlanBuilder build, std::shared_ptr<arrow::Schema> schema,
                                std::unique_ptr<Operator> first)
      : fast_(std::move(fast)),
        original_(std::move(original)),
        build_(build),
        schema_(std::move(schema)),
        first_(std::move(first)) {}

  const std::shared_ptr<arrow::Schema>& output_schema() const override { return schema_; }

  arrow::Status Open(ExecContext& ctx) override {
    ARROW_RETURN_NOT_OK(Close());
    ctx_ = ctx;
    finish_fallback_ = ctx.scans == nullptr;
    started_ = false;
    return arrow::Status::OK();
  }

  arrow::Result<Batch> Next() override {
    if (!ctx_) {
      return arrow::Status::Invalid("deferred dimension join: Next() before Open()");
    }
    ExecContext& ctx = *ctx_;
    if (!started_) {
      started_ = true;
      ARROW_RETURN_NOT_OK(Start(ctx));
    }
    if (original_op_ != nullptr) {
      ARROW_ASSIGN_OR_RAISE(auto batch, original_op_->Next());
      if (batch.end()) {
        auto original = std::move(original_op_);
        ARROW_RETURN_NOT_OK(original->Close());
        if (finish_fallback_) {
          ARROW_RETURN_NOT_OK(FinishScans(ctx));
        }
      }
      return batch;
    }
    if (!output_ || next_ == output_->size()) {
      return Batch{};
    }
    return Batch{.data = std::move((*output_)[next_++]), .selection = nullptr};
  }

  arrow::Status Close() override {
    DiscardOutput();
    auto original = std::move(original_op_);
    const auto status = original == nullptr ? arrow::Status::OK() : original->Close();
    ctx_.reset();
    return status;
  }

 private:
  // A caller that never pulls the root must not open a dimension file.
  arrow::Status Start(ExecContext& ctx) {
    try {
      const auto kept = KeyTableNoBadAlloc([&] { return TryCandidate(ctx); });
      if (kept.ok() && *kept) {
        if (profile() != nullptr) {
          profile()->AddNoThrow("dimension_verified", MetricUnit::kCount, 1);
        }
        return arrow::Status::OK();
      }
      // Drop all speculative memory and scan validation state before the retry. Only the
      // original plan determines which error, if any, is visible.
      first_.reset();
      DiscardOutput();
      if (profile() != nullptr) {
        profile()->AddNoThrow("dimension_fallbacks", MetricUnit::kCount, 1);
      }
      const auto caller_scans = std::exchange(ctx.scans, std::make_shared<plan::ScanContext>());
      ARROW_ASSIGN_OR_RAISE(original_op_, build_(original_, Child(1)));
      auto opened = original_op_->Open(ctx);
      if (opened.ok() && caller_scans != nullptr) {
        // Opening a shared build may itself retry with a new context.
        opened = caller_scans->KeepForFinish(ctx.scans);
      }
      if (!opened.ok()) {
        static_cast<void>(Close());
      }
      return opened;
    } catch (const std::bad_alloc&) {
      static_cast<void>(Close());
      return arrow::Status::OutOfMemory("out of memory opening a deferred dimension join");
    }
  }

  ProfileNode* Child(std::size_t index) const {
    return profile() == nullptr ? nullptr : profile()->Child(index);
  }

  void DiscardOutput() {
    output_.reset();
    next_ = 0;
  }

  // The planner admits a blocking sort. Keep its final batches until every expression and scan
  // has succeeded: a failure even in a later projected batch can then retry without mixing rows.
  arrow::Result<bool> Collect(Operator& fast, ExecContext& run) {
    ARROW_RETURN_NOT_OK(fast.Open(run));
    auto& output = output_.emplace(OutputAllocator(run.pool));
    int64_t retained_bytes = 0;
    while (true) {
      ARROW_ASSIGN_OR_RAISE(auto batch, fast.Next());
      if (batch.end()) {
        return true;
      }
      ARROW_ASSIGN_OR_RAISE(auto rows, Materialize(batch, run.pool));
      if (rows->num_rows() == 0) {
        continue;
      }
      // The original group calls its outputs key0, key1, ...; the late scan names the table
      // fields. Preserve the operator schema, also for callers outside the SQL session.
      rows = arrow::RecordBatch::Make(schema_, rows->num_rows(), rows->columns());
      const int64_t bytes = arrow::util::TotalBufferSize(*rows);
      constexpr int64_t kEntryBytes = sizeof(std::shared_ptr<arrow::RecordBatch>);
      if (bytes > plan::kMaxSharedResultBytes - retained_bytes - kEntryBytes) {
        return false;
      }
      // The sort already allocated its result buffers from the query pool. Count their sizes
      // toward the result cap; only the vector's storage needs a new allocation from the pool.
      retained_bytes += bytes + kEntryBytes;
      output.push_back(std::move(rows));
    }
  }

  arrow::Result<bool> TryCandidate(const ExecContext& ctx) {
    ExecContext run = ctx;
    run.scans = std::make_shared<plan::ScanContext>();
    auto fast = std::move(first_);
    if (fast == nullptr) {
      ARROW_ASSIGN_OR_RAISE(fast, build_(fast_, Child(0)));
    }
    const auto kept = KeyTableNoBadAlloc([&] { return Collect(*fast, run); });
    const auto closed = fast->Close();
    if (!closed.ok() || !kept.ok() || !*kept) {
      return false;
    }
    ARROW_RETURN_NOT_OK(FinishScans(run));
    if (ctx.scans != nullptr) {
      ARROW_RETURN_NOT_OK(ctx.scans->KeepForFinish(std::move(run.scans)));
    }
    return true;
  }

  plan::LogicalPlan fast_;
  plan::LogicalPlan original_;
  DimensionPlanBuilder build_;
  std::shared_ptr<arrow::Schema> schema_;
  std::unique_ptr<Operator> first_;
  std::unique_ptr<Operator> original_op_;
  std::optional<ExecContext> ctx_;
  bool finish_fallback_ = false;
  bool started_ = false;
  using OutputAllocator = arrow::stl::allocator<std::shared_ptr<arrow::RecordBatch>>;
  std::optional<std::vector<std::shared_ptr<arrow::RecordBatch>, OutputAllocator>> output_;
  std::size_t next_ = 0;
};

}  // namespace

arrow::Result<std::unique_ptr<Operator>> MakeDeferredDimensionJoin(plan::LogicalPlan fast,
                                                                   plan::LogicalPlan original,
                                                                   ProfileNode* profile,
                                                                   DimensionPlanBuilder build) {
  ARROW_ASSIGN_OR_RAISE(auto ordinary, build(original, nullptr));
  auto schema = ordinary->output_schema();
  ordinary.reset();
  if (profile != nullptr) {
    profile->set_name("DeferredDimensionJoin");
    profile->set_detail("verify unique, non-NULL surrogate and business keys");
  }
  ProfileNode* child = profile == nullptr ? nullptr : profile->Child(0);
  auto first = KeyTableNoBadAlloc([&] { return build(fast, child); });
  if (!first.ok() || !SameTypes(*schema, *(*first)->output_schema())) {
    // An optional physical shape may cost more to construct than the ordinary tree.
    if (profile == nullptr) {
      return build(original, nullptr);
    }
    ARROW_ASSIGN_OR_RAISE(auto fallback, build(original, profile->Child(1)));
    profile->set_detail("original plan after planning fallback");
    return std::make_unique<ProfiledOperator>(std::move(fallback), profile);
  }
  auto guarded = std::make_unique<DeferredDimensionJoinOperator>(
      std::move(fast), std::move(original), build, std::move(schema), *std::move(first));
  if (profile == nullptr) {
    return guarded;
  }
  guarded->set_profile(profile);
  return std::make_unique<ProfiledOperator>(std::move(guarded), profile);
}

}  // namespace antb1::exec
