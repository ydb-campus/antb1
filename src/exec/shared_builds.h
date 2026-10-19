#pragma once

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

// One shared sub-plan: the operator that computes it, and the table its consumers read. The
// planner builds the widest member's sub-plan here and rewrites every member -- that one included
// -- to a scan of `table`.
struct SharedBuild {
  std::shared_ptr<MaterializedTable> table;
  std::unique_ptr<Operator> op;
};

// Runs every shared sub-plan to completion before it opens `input`, and owns the results.
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
// **Eager, so a consumer that would have been short-circuited still pays.** A LIMIT that stops a
// consumer early does not stop the shared sub-plan, because it has already run. None of the
// queries this item was built for is that shape, and the ones that are not targets are the control
// that would show it.
class SharedBuildsOperator final : public Operator {
 public:
  SharedBuildsOperator(std::unique_ptr<Operator> input, std::vector<SharedBuild> builds)
      : input_(std::move(input)), builds_(std::move(builds)) {}

  [[nodiscard]] const std::shared_ptr<arrow::Schema>& output_schema() const override {
    return input_->output_schema();
  }
  arrow::Status Open(ExecContext& ctx) override;
  arrow::Result<Batch> Next() override;
  arrow::Status Close() override;

 private:
  std::unique_ptr<Operator> input_;
  std::vector<SharedBuild> builds_;
};

}  // namespace antb1::exec
