#include "difftest.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "engine.h"
#include "ordered_compare.h"
#include "query_gen.h"
#include "result_diff.h"
#include "supported_features.h"

namespace antb1::slt {
namespace {

enum class Outcome : std::uint8_t { kCompared, kUnsupported, kRejected, kFailed };

constexpr std::size_t kMaxListedCases = 5;

struct CaseResult {
  Outcome outcome = Outcome::kCompared;
  Discrepancy failure;
};

CaseResult Fail(Discrepancy d) {
  return CaseResult{.outcome = Outcome::kFailed, .failure = std::move(d)};
}

// The oracle of one case: `oracle`, until a query fails with a DuckDB internal error
// (IsDuckDbInternalError); from then on, for the rest of the case, the first of `retries` that
// answers it.
class OracleOfCase final : public Engine {
 public:
  OracleOfCase(Engine& oracle, std::span<Engine* const> retries)
      : oracle_(oracle), retries_(retries) {}

  [[nodiscard]] std::string_view name() const override { return oracle_.name(); }

  ExecResult Execute(const std::string& sql) override {
    ExecResult result = Current().Execute(sql);
    while (!result.has_value() && IsDuckDbInternalError(result.error())) {
      if (!first_internal_.has_value()) {
        first_internal_ = result.error();
      }
      if (level_ == retries_.size()) {
        break;
      }
      ++level_;
      result = Current().Execute(sql);
    }
    answered_by_retry_ = answered_by_retry_ || (level_ > 0 && result.has_value());
    return result;
  }

  // Back to `oracle`, for the next case.
  void Reset() {
    level_ = 0;
    answered_by_retry_ = false;
    first_internal_.reset();
  }
  // Whether the case asked a retry, and whether one answered.
  [[nodiscard]] bool retried() const { return level_ > 0; }
  [[nodiscard]] bool answered_by_retry() const { return answered_by_retry_; }
  // The case's first DuckDB internal error, if any.
  [[nodiscard]] const std::optional<EngineError>& first_internal() const { return first_internal_; }

 private:
  Engine& Current() { return level_ == 0 ? oracle_ : *retries_[level_ - 1]; }

  Engine& oracle_;
  std::span<Engine* const> retries_;
  std::size_t level_ = 0;
  bool answered_by_retry_ = false;
  std::optional<EngineError> first_internal_;
};

CaseResult CheckCase(const GeneratedQuery& q, FeatureSet supported_set, Engine& antb1,
                     OracleOfCase& oracle) {
  const bool supported = supported_set.Contains(q.features);
  if (q.sql.empty()) {
    return Fail(Discrepancy{.what = "the generator produced no query (a generator bug)"});
  }
  const auto a = antb1.Execute(q.sql);
  const auto o = oracle.Execute(q.sql);
  if (!o.has_value() && oracle.first_internal().has_value()) {
    // The internal error is the report's; a retry's other error, as its last word, goes with it.
    const EngineError& internal = *oracle.first_internal();
    Discrepancy d = ErrorDiscrepancy(
        std::format("DuckDB fails with an internal error{} (a DuckDB bug, not a generator bug)",
                    oracle.retried() ? ", and its retries with fewer optimizers fail" : ""),
        internal);
    if (o.error().message != internal.message) {
      d.detail += "\n  the last retry: " + o.error().message;
      d.redacted += std::format(", the last retry's: {}", o.error().kind);
    }
    return Fail(std::move(d));
  }
  if (!o.has_value()) {
    return Fail(ErrorDiscrepancy(
        "DuckDB rejects the generated SQL (a generator bug: queries must be valid DuckDB SQL)",
        o.error()));
  }
  if (!a.has_value()) {
    if (a.error().unsupported && !supported) {
      return CaseResult{.outcome = Outcome::kUnsupported, .failure = {}};
    }
    if (a.error().unsupported) {
      Discrepancy d = ErrorDiscrepancy(
          "antb1 reports Unsupported for a query that uses only supported features", a.error());
      d.detail += std::format("\n  declared supported (tests/slt/supported_features.h): {}",
                              supported_set.Names());
      return Fail(std::move(d));
    }
    if (!supported && !a.error().internal) {
      return CaseResult{.outcome = Outcome::kRejected, .failure = {}};
    }
    return Fail(ErrorDiscrepancy(
        std::format("antb1 fails ({} error), DuckDB answers", a.error().kind), a.error()));
  }
  // q.unordered_limit and q.sort follow from the SQL: CompareQueryAnswers derives the same.
  if (auto d = CompareQueryAnswers(q.sql, *o, *a, oracle, q.sort == SortMode::kRowSort, q.sort)) {
    return Fail(*std::move(d));
  }
  return {};
}

void Report(const GeneratedQuery& q, uint64_t seed, bool supported, const Discrepancy& d,
            const DiffOptions& options, std::string& out) {
  out += std::format("FAIL diff case {} (seed {}, {}): {}\n", q.index, seed,
                     supported ? "supported features" : "target grammar sample", d.what);
  out += std::format("  features: {}\n", q.features.Names());
  AppendDiscrepancy(d, q.sql, q.sort, options.redact, out);
  if (!options.pixi_repro && options.command.empty()) {
    return;
  }
  out += options.redact ? "  repro (unredacted, prints values; run it locally):\n" : "  repro:\n";
  if (options.pixi_repro) {
    out += std::format("    ANTB1_DIFF_SEED={} ANTB1_DIFF_ONLY={} pixi run diff-random{}\n", seed,
                       q.index, options.pixi_args);
  }
  if (!options.command.empty()) {
    out += std::format("    {} --only {}\n", options.command, q.index);
  }
}

}  // namespace

DiffStats RunDiff(const QueryGenerator& generator, Engine& antb1, Engine& oracle,
                  const DiffOptions& options, std::string& out,
                  std::span<Engine* const> oracle_retries) {
  DiffStats stats;
  OracleOfCase oracle_of_case(oracle, oracle_retries);
  const FeatureSet supported_set = generator.options().supported;
  const uint64_t first = options.only.value_or(0);
  const uint64_t last = options.only.has_value() ? first + 1 : options.count;
  for (uint64_t i = first; i < last; ++i) {
    const GeneratedQuery q = generator.Generate(i);
    const bool supported = supported_set.Contains(q.features);
    ++stats.queries;
    stats.supported += supported ? 1 : 0;
    stats.target += q.target_sample ? 1 : 0;
    oracle_of_case.Reset();
    CaseResult r = CheckCase(q, supported_set, antb1, oracle_of_case);
    if (oracle_of_case.answered_by_retry()) {
      ++stats.oracle_retried;
      if (stats.oracle_retried_cases.size() < kMaxListedCases) {
        stats.oracle_retried_cases.push_back(q.index);
      }
      if (r.outcome == Outcome::kFailed) {
        r.failure.what += " (DuckDB answered with fewer optimizers, after an internal error)";
      }
    }
    switch (r.outcome) {
      case Outcome::kCompared:
        ++stats.compared;
        stats.answered_outside += supported ? 0 : 1;
        break;
      case Outcome::kUnsupported: {
        ++stats.unsupported;
        const FeatureSet missing = q.features.Minus(supported_set);
        for (std::size_t f = 0; f < kFeatureCount; ++f) {
          stats.unsupported_by_feature[f] += missing.Has(static_cast<Feature>(f)) ? 1U : 0U;
        }
        break;
      }
      case Outcome::kRejected:
        ++stats.rejected;
        if (stats.rejected_cases.size() < kMaxListedCases) {
          stats.rejected_cases.push_back(q.index);
        }
        break;
      case Outcome::kFailed:
        if (stats.failed < options.max_reports) {
          Report(q, generator.seed(), supported, r.failure, options, out);
        }
        ++stats.failed;
        break;
    }
  }
  if (stats.failed > options.max_reports) {
    out += std::format("... {} more failure(s) not shown\n", stats.failed - options.max_reports);
  }
  out += std::format(
      "antb1-slt diff: seed={} queries={} supported={} target={} compared={} unsupported={} "
      "rejected={} answered_outside_supported={} failed={}\n",
      generator.seed(), stats.queries, stats.supported, stats.target, stats.compared,
      stats.unsupported, stats.rejected, stats.answered_outside, stats.failed);
  if (stats.unsupported > 0) {
    std::vector<std::pair<uint64_t, Feature>> counts;
    for (std::size_t f = 0; f < kFeatureCount; ++f) {
      if (stats.unsupported_by_feature[f] > 0) {
        counts.emplace_back(stats.unsupported_by_feature[f], static_cast<Feature>(f));
      }
    }
    std::ranges::stable_sort(counts,
                             [](const auto& a, const auto& b) { return a.first > b.first; });
    std::string line;
    for (const auto& [n, f] : counts) {
      line += std::format("{}{}={}", line.empty() ? "" : " ", FeatureName(f), n);
    }
    out +=
        "antb1-slt diff: Unsupported answers (not failures) by feature outside the supported "
        "set: " +
        line + "\n";
  }
  // "<first cases>; see one with <repro>", for a NOTE.
  const auto listed = [&](const std::vector<uint64_t>& indexes) {
    std::string cases;
    for (const uint64_t c : indexes) {
      cases += std::format("{}{}", cases.empty() ? "" : " ", c);
    }
    if (options.pixi_repro) {
      return std::format(
          "{}; see one with ANTB1_DIFF_SEED={} ANTB1_DIFF_ONLY=<case> pixi run "
          "diff-random{}",
          cases, generator.seed(), options.pixi_args);
    }
    if (!options.command.empty()) {
      return std::format("{}; see one with {} --only <case>", cases, options.command);
    }
    return cases;
  };
  if (stats.rejected > 0) {
    out += std::format(
        "antb1-slt diff: NOTE: antb1 rejected {} target-grammar quer{} with a query error instead "
        "of Unsupported (not failures; first cases: {})\n",
        stats.rejected, stats.rejected == 1 ? "y" : "ies", listed(stats.rejected_cases));
  }
  if (stats.oracle_retried > 0) {
    out += std::format(
        "antb1-slt diff: NOTE: DuckDB failed {} quer{} with an internal error, a DuckDB bug, and "
        "answered with fewer optimizers instead (first cases: {})\n",
        stats.oracle_retried, stats.oracle_retried == 1 ? "y" : "ies",
        listed(stats.oracle_retried_cases));
  }
  if (stats.answered_outside > 0) {
    out += std::format(
        "antb1-slt diff: NOTE: antb1 answered {} quer{} with features outside the supported set "
        "(compared with DuckDB). Once a feature is implemented, declare it in "
        "tests/slt/supported_features.h.\n",
        stats.answered_outside, stats.answered_outside == 1 ? "y" : "ies");
  }
  out += std::format("DIFF: {} seed={} queries={} failed={} unsupported={}\n",
                     stats.failed == 0 ? "PASS" : "FAIL", generator.seed(), stats.queries,
                     stats.failed, stats.unsupported);
  return stats;
}

}  // namespace antb1::slt
