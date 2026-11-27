#pragma once

#include <functional>
#include <memory>
#include <utility>
#include <vector>

#include <arrow/result.h>
#include <arrow/status.h>
#include <arrow/type.h>

#include "antb1/exec/operator.h"

#include "materialized_table.h"

// Computing a repeated sub-plan once (docs/adr/0030-shared-sub-plans.md, E2).

namespace antb1::exec {

// The planner needs the first producer's schema to plan the consumers. Each Open consumes an
// operator instance so its working memory can be reclaimed independently of its output table.
struct SharedBuild {
  std::shared_ptr<MaterializedTable> table;
  std::unique_ptr<Operator> first_producer;
  std::function<arrow::Result<std::unique_ptr<Operator>>()> make_producer;
  CombinedJoinFilter join_filter = CombinedJoinFilter::kOrdinary;
};

// Runs every shared sub-plan to completion before it opens `input`, and owns the results.
// If a producer fails or its result cannot be retained, it releases all shared builds and opens
// the original plan. The result stream always comes from one plan for the entire run.
//
// On the JoinBuild pattern (hash_join.cc's PrepareBuilds), and for the same reason: a consumer
// cannot read a result that is not there yet, and here the consumers may sit anywhere in the plan
// -- two references to one CTE can be a join's build and its probe -- so there is no one pipeline
// to hang them off. One owner above everything is the narrow version of that.
//
// **It owns the tables for the whole query** rather than counting references and freeing each when
// its last consumer finishes. Simpler, and it is the difference between this and a join build,
// which has exactly one consumer and so can be released by it. The cost is peak memory held to the
// end of the query, which the item's measurement reports rather than hides.
//
// MaterializableSubPlans excludes the inputs of streaming limits: those consumers can stop
// between batches or skip whole union branches, before an eager producer would have finished.
class SharedBuildsOperator final : public Operator {
 public:
  using Fallback = std::function<arrow::Result<std::unique_ptr<Operator>>(ProfileNode*)>;

  SharedBuildsOperator(std::unique_ptr<Operator> input, std::vector<SharedBuild> builds,
                       Fallback fallback)
      : input_(std::move(input)), builds_(std::move(builds)), fallback_(std::move(fallback)) {}

  [[nodiscard]] const std::shared_ptr<arrow::Schema>& output_schema() const override {
    return ActiveInput().output_schema();
  }
  arrow::Status Open(ExecContext& ctx) override;
  arrow::Result<Batch> Next() override;
  arrow::Status Close() override;
  // The deferred-dimension guard can retry this run with independent groups after dropping
  // the entire consumer and its retained output. PF5 may already have made that switch.
  [[nodiscard]] bool UsedCombinedDomains() const;

 private:
  [[nodiscard]] Operator& ActiveInput() const { return use_fallback_ ? *unshared_ : *input_; }
  arrow::Status PrepareFallback(ExecContext& ctx);
  void ReleaseBuilds();

  std::unique_ptr<Operator> input_;
  std::vector<SharedBuild> builds_;
  Fallback fallback_;
  std::unique_ptr<Operator> unshared_;
  bool use_fallback_ = false;
  bool retry_if_empty_ = false;
};

}  // namespace antb1::exec
