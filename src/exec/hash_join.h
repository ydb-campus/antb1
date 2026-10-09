#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include <arrow/array.h>
#include <arrow/record_batch.h>
#include <arrow/result.h>
#include <arrow/status.h>
#include <arrow/type_fwd.h>

#include "antb1/exec/join_table.h"
#include "antb1/exec/memory_budget.h"
#include "antb1/exec/operator.h"
#include "antb1/plan/logical_plan.h"

#include "part_operators.h"

// The hash joins (docs/adr/0022-joins-and-query-blocks.md, "Execution"): inner, semi, anti,
// null-aware anti and one-row joins. A join's build side (JoinBuild, a JoinTable of its input), the
// operator that prepares a probe pipeline's builds before any part of the pipeline runs
// (BuildsFirstOperator), and the probe (HashJoinOperator), a streaming operator of the probe side's
// part pipeline or one over a serial input.
//
// Builds are prepared on the consumer thread only, never from a part task (a worker that waits for
// tasks of its own pool can deadlock it), by the operator that runs their probe pipeline, when it
// is first pulled (not in its Open): an unpulled query (LIMIT 0) runs no build. A build whose input
// probes builds of its own prepares those first (post-order). A probe whose build empties its join
// (EmptiesJoin) never opens its input, and the builds below it are not prepared (PrepareBuilds): no
// row could come out of it. Builds stay pinned until their probe pipeline's parts are done
// (PartSink::set_parts_done), or until it is closed.

namespace antb1::exec {

class ProfileNode;

// The build row of a one-row join as constant columns, one per column of the build input, each of
// `rows` rows (at most ExecContext::batch_size, fewer when the row's VARCHAR values are long: a
// window copies them once per row): made once by its build, then sliced by the probes of every
// part. Immutable.
struct OneRowValues {
  arrow::ArrayVector columns;
  int64_t rows = 0;
};

// The build side of a hash join: the JoinTable of its input, prepared by the operator that runs its
// probe pipeline and read by the probes of every part of that pipeline. Its join's kind decides
// what its probes do (HashJoinOperator) and whether its table empties the join (EmptiesJoin).
//
// Input: a part pipeline, whose parts run on ExecContext::executor through a PartScheduler (its
// window and memory pressure; a part that runs out of memory next to others runs again alone), or
// any other operator, drained on the consumer thread, whose batches are then the parts. Either way
// the parts reach a JoinTableBuilder in part order: the table depends on the input's rows only,
// never on the number of threads. A pipeline input that probes builds of its own (`builds`, in
// preparation order) prepares them first, and releases them once its table is built.
//
// Errors follow the serial order: the builds its input probes, then the input's first failing part
// in part order (a part whose merge failed comes before any later part's failure); a drained
// input's own error (its Open, then Next, then Close). std::bad_alloc is OutOfMemory. A failed
// Prepare holds nothing: no table, no part, no batch of a drained input, no table of the builds its
// input probes.
//
// Thread safety: Prepare and Release on the consumer thread only; table() and values() from any
// thread between a successful Prepare and the next Release.
//
// A one-row join's build (a keyless spec) must hold exactly one row: any other count is Invalid,
// from Prepare, before any probe part runs (a planner bug: SQL never sees it). Its values are then
// made once (OneRowValues), from ExecContext::pool.
//
// Profile (`profile`, nullptr: none): each Prepare's time and instance, the rows the table holds
// (one batch per chunk of the table), and the metrics parts, part_time, wait, lanes_tail, finish
// (the table, and a one-row join's values), null_keys, unique and direct (1 for a table of unique
// keys, of the direct layout).
class JoinBuild {
 public:
  // A build for a join of `kind` of the `num_parts` parts of `pipeline`, whose batches have
  // `spec`'s schema, which probes `builds`.
  JoinBuild(std::shared_ptr<const JoinBuildSpec> spec, PartPipeline pipeline, int64_t num_parts,
            std::vector<std::shared_ptr<JoinBuild>> builds, ProfileNode* profile,
            plan::JoinKind kind);
  // A build for a join of `kind` of the batches of `input`, whose schema is `spec`'s.
  JoinBuild(std::shared_ptr<const JoinBuildSpec> spec, std::unique_ptr<Operator> input,
            ProfileNode* profile, plan::JoinKind kind);
  JoinBuild(const JoinBuild&) = delete;
  JoinBuild& operator=(const JoinBuild&) = delete;
  JoinBuild(JoinBuild&&) = delete;
  JoinBuild& operator=(JoinBuild&&) = delete;
  ~JoinBuild();

  // Builds the table, releasing the one it holds first. Errors: the input's (in the serial order
  // above), OutOfMemory past the memory limit, Invalid for what a correct physical plan never sends
  // (JoinTableBuilder's checks, a one-row join's build of another row count than one).
  arrow::Status Prepare(ExecContext& ctx);
  // Releases the table, a one-row join's values and the tables of the builds its input probes.
  void Release();

  // The table: nullptr before Prepare, after a failed one and after Release.
  [[nodiscard]] const std::shared_ptr<const JoinTable>& table() const { return table_; }
  // A one-row join's values, while it holds its table; nullptr for any other kind.
  [[nodiscard]] const std::shared_ptr<const OneRowValues>& values() const { return values_; }
  [[nodiscard]] const std::shared_ptr<const JoinBuildSpec>& spec() const { return spec_; }
  [[nodiscard]] plan::JoinKind kind() const { return kind_; }

 private:
  arrow::Status BuildTable(ExecContext& ctx);
  // The batches of the drained input that have selected rows, their vector charged to `memory`.
  arrow::Result<std::vector<Batch>> DrainInput(ExecContext& ctx, MemoryReservation& memory);

  std::shared_ptr<const JoinBuildSpec> spec_;
  std::shared_ptr<const PartPipeline> pipeline_;  // a pipeline input, of num_parts_ parts
  int64_t num_parts_ = 0;
  std::unique_ptr<Operator> input_;                 // a drained input
  std::vector<std::shared_ptr<JoinBuild>> builds_;  // the builds the pipeline input probes
  ProfileNode* profile_;
  plan::JoinKind kind_;
  std::shared_ptr<const JoinTable> table_;
  std::shared_ptr<const OneRowValues> values_;
};

// Whether no row can come out of a join of `kind` whose build holds `table`, whatever its probe
// input holds: an inner or a semi join whose table holds no row. Every other kind reads its probe
// input: an anti join over no row keeps every selected row, a null-aware anti join over an empty
// input every selected row (NULL keys included), and one over an input with a NULL key none, but
// it reads every row, as DuckDB does.
bool EmptiesJoin(plan::JoinKind kind, const JoinTable& table);

// Prepares `builds` in order and stops after the first one that empties its join (EmptiesJoin):
// every later one belongs to a join below it in the probe pipeline, whose rows could not reach the
// output. On failure every build is released. Consumer thread only.
arrow::Status PrepareBuilds(std::span<const std::shared_ptr<JoinBuild>> builds, ExecContext& ctx);
// Releases every build of `builds`.
void ReleaseBuilds(std::span<const std::shared_ptr<JoinBuild>> builds);

// The sink of a part pipeline that probes `builds` (outermost join first: their preparation order).
// Open only records the context. The first Next prepares the builds (PrepareBuilds), then opens the
// sink: no part of the pipeline runs, and the sink's part scheduler does not exist, before the
// builds are done. The builds are released when the sink's parts are done (its parts-done callback)
// and at Close, after the sink's Close, which waits for its parts. Output: the sink's. Next before
// Open or after Close: Invalid.
class BuildsFirstOperator final : public Operator {
 public:
  BuildsFirstOperator(std::unique_ptr<PartSink> sink,
                      std::vector<std::shared_ptr<JoinBuild>> builds);
  ~BuildsFirstOperator() override;

  [[nodiscard]] const std::shared_ptr<arrow::Schema>& output_schema() const override {
    return sink_->output_schema();
  }
  arrow::Status Open(ExecContext& ctx) override;
  arrow::Result<Batch> Next() override;
  arrow::Status Close() override;

 private:
  std::vector<std::shared_ptr<JoinBuild>> builds_;
  std::unique_ptr<PartSink> sink_;  // destroyed before the builds: it may still hold their probes
  ExecContext ctx_;                 // a copy: the context of the first Next
  bool opened_ = false;
  bool sink_opened_ = false;  // the builds are prepared and the sink opened
};

// The probe of a hash join of its build's kind: for each batch of its input (the probe side),
// JoinTable::Find of the selected rows, then the join's rows in batches of at most
// max(ExecContext::batch_size, 1) rows, resuming inside a probe batch: rows keep the probe order,
// and a probe row's matches keep the build's (part, row) order.
//
// - Inner. Output: the left input's columns, then the right input's (whichever side builds); the
//   build's columns are nullable.
//   - A build of unique keys (the 1:1 path): each window of at most batch_size rows of a probe
//     batch is a slice of it, not a copy, selected where its rows matched (a row the probe batch
//     does not select matches nothing), with the build's columns gathered by match and NULL
//     elsewhere; a window without a match is skipped.
//   - Otherwise (1:N): the probe columns are taken (Take) and the build columns gathered for each
//     (probe row, match) pair; no selection.
//   - Residuals (BOOLEAN expressions over the output columns, in order): each is evaluated on the
//     rows that passed the ones before it, never on others (the selected rows are taken first),
//     and a row passes when every one is true (NULL is not): a later residual never sees a row an
//     earlier one dropped, as in a WHERE.
// - Semi, anti and null-aware anti (building on the right, without residuals). Output: the probe's
//   own schema object; each window of at most batch_size rows of a probe batch is a slice of it,
//   selected where it keeps a row, without a selection when it keeps every row, skipped when it
//   keeps none. Semi keeps the rows with a match (each once), anti the selected rows without one,
//   null-aware anti the selected rows without one and with a non-NULL key. Over a build without
//   rows, anti keeps every selected row; over an empty build input, null-aware anti keeps every
//   selected row, NULL keys included; over a build input with a NULL key, null-aware anti keeps
//   none, but reads its whole input. In these three cases nothing is looked up.
// - One-row (a keyless build of one row, on the right). Output: the probe's columns, then the
//   build row's values (nullable); each window of a probe batch is a slice of it with its
//   selection, the values sliced from the build's OneRowValues, so a window has at most their
//   rows; a window without a selected row is skipped. Nothing is looked up.
// - A build that empties its join (EmptiesJoin): the input is never opened, and Next returns the
//   end.
//
// In a part pipeline (prepares = false), Open takes the table prepared by the operator that runs
// the pipeline (a BuildsFirstOperator, or the JoinBuild whose input the pipeline is; Invalid if
// none) and opens the input unless the table empties the join. Over a serial input (prepares =
// true), Open only records the context; the first Next prepares the build, then opens the input
// unless the table empties the join; the input's end and Close release the build.
//
// Memory: its own vectors are charged to ExecContext::budget, its Arrow buffers come from
// ExecContext::pool. Profile: the metrics find (whenever it looks keys up), gather (the rows'
// columns: the build columns an inner join gathers, and on its 1:N path the probe rows it takes;
// the slices of a one-row join's values), residual and, on the 1:1 path of an inner join,
// window_rows (the rows of the windows its build columns were gathered for, against the rows it
// emits).
class HashJoinOperator final : public Operator {
 public:
  // The kind is the build's. Invalid without a probe or a build; for a LEFT join (not run yet); for
  // a kind other than inner building on the left; for a one-row join over a build with keys, or
  // any other kind over a keyless one; for a null-aware anti join of other than one key; for
  // residuals on any kind but inner; for probe keys (columns of the probe's output, one per build
  // key) outside the probe's output or of another type than their build key; and for a residual
  // that is not BOOLEAN or reads a column outside the probe's and the build's columns.
  static arrow::Result<std::unique_ptr<HashJoinOperator>> Make(std::unique_ptr<Operator> probe,
                                                               std::shared_ptr<JoinBuild> build,
                                                               std::vector<int> probe_keys,
                                                               plan::BuildSide build_side,
                                                               std::vector<plan::ExprPtr> residual,
                                                               bool prepares);
  ~HashJoinOperator() override;

  [[nodiscard]] const std::shared_ptr<arrow::Schema>& output_schema() const override {
    return schema_;
  }
  arrow::Status Open(ExecContext& ctx) override;
  arrow::Result<Batch> Next() override;
  arrow::Status Close() override;

 private:
  // The selected rows of a probe batch that a semi, anti, null-aware anti or one-row probe keeps,
  // decided per run from the build (Start): those with a match (semi), those without one (anti),
  // those without one and with a non-NULL key (null-aware anti), every one (one-row; anti over no
  // build row; null-aware anti over an empty build input) or none (null-aware anti over a build
  // input with a NULL key). An inner probe looks up as kMatched does.
  enum class Keep : std::uint8_t { kMatched, kUnmatched, kUnmatchedNotNull, kAll, kNone };

  HashJoinOperator(std::unique_ptr<Operator> probe, std::shared_ptr<JoinBuild> build,
                   std::vector<int> probe_keys, plan::BuildSide build_side,
                   std::vector<plan::ExprPtr> residual, bool prepares,
                   std::shared_ptr<arrow::Schema> schema);

  // Takes the table (prepared first when prepares_) and opens the input unless the table empties
  // the join.
  arrow::Status Start();
  // The next probe batch with selected rows and its matches (none when nothing is looked up), or
  // the end of the input.
  arrow::Status Pull();
  // The next output of the current probe batch, by the join's kind: on the 1:1 path or the 1:N
  // path of an inner join, of a semi, anti or null-aware anti join (NextSelected) or of a one-row
  // join: a batch, or nothing (no row came out of a window or its residuals; batch_ is cleared at
  // its end).
  arrow::Result<std::optional<Batch>> NextOutput();
  arrow::Result<std::optional<Batch>> NextWindow();
  arrow::Result<std::optional<Batch>> NextPairs();
  arrow::Result<std::optional<Batch>> NextSelected();
  arrow::Result<std::optional<Batch>> NextOneRow();
  // The next window of at most `limit` rows of the current probe batch, from window_ on: its first
  // row and its rows; nullopt (batch_ cleared) at the batch's end.
  std::optional<std::pair<int64_t, int64_t>> NextRange(int64_t limit);
  // Whether row `row` of the current probe batch is selected (a NULL selection value is not).
  [[nodiscard]] bool Selected(int64_t row) const;
  // The build's columns of build_rows_[0, rows).
  template <bool kWithNulls>
  arrow::Result<arrow::ArrayVector> GatherBuild(int64_t rows);
  // The output batch of these probe and build columns.
  std::shared_ptr<arrow::RecordBatch> Assemble(const arrow::ArrayVector& probe,
                                               const arrow::ArrayVector& build, int64_t rows) const;
  // `out` after the residuals: its rows that pass every one, without a selection; nothing when
  // none passes.
  arrow::Result<std::optional<Batch>> ApplyResiduals(Batch out);
  // The input ended, or the build holds no row: what the run holds goes.
  void End();

  std::unique_ptr<Operator> input_;
  std::shared_ptr<JoinBuild> build_;
  std::vector<int> probe_keys_;
  plan::BuildSide build_side_;
  std::vector<plan::ExprPtr> residual_;
  bool prepares_;
  std::shared_ptr<arrow::Schema> schema_;
  plan::JoinKind kind_;  // the build's

  ExecContext ctx_;  // a copy: the context of the first Next of a probe that prepares its build
  int64_t batch_size_ = 1;
  bool opened_ = false;
  bool started_ = false;       // Start succeeded
  bool input_opened_ = false;  // its Open was called: Close closes it
  bool done_ = false;          // the input ended, or the build empties the join
  Keep keep_ = Keep::kMatched;
  std::shared_ptr<const JoinTable> table_;
  std::shared_ptr<const OneRowValues> values_;       // a one-row join's
  Batch batch_;                                      // the probe batch being joined (no data: none)
  std::vector<std::shared_ptr<arrow::Array>> keys_;  // batch_'s key columns
  std::vector<JoinMatches> matches_;                 // the matches of batch_'s rows
  int64_t row_ = 0;                                  // 1:N: the next probe row,
  std::uint32_t taken_ = 0;                          // and how many of its matches were emitted
  int64_t window_ = 0;  // 1:1, semi, anti, null-aware anti, one-row: the next window's first row
  std::vector<std::uint32_t> probe_rows_;  // an output batch's probe rows
  std::vector<JoinRowRef> build_rows_;     // and build rows
  MemoryReservation memory_;               // matches_, probe_rows_ and build_rows_
};

}  // namespace antb1::exec
