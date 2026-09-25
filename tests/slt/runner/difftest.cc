#include "difftest.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "engine.h"
#include "query_gen.h"
#include "result_diff.h"
#include "supported_features.h"

namespace antb1::slt {
namespace {

enum class Outcome : std::uint8_t { kCompared, kUnsupported, kRejected, kFailed };

constexpr std::size_t kMaxListedRejects = 5;

struct CaseResult {
  Outcome outcome = Outcome::kCompared;
  Discrepancy failure;
};

CaseResult Fail(Discrepancy d) {
  return CaseResult{.outcome = Outcome::kFailed, .failure = std::move(d)};
}

CaseResult CheckCase(const GeneratedQuery& q, FeatureSet supported_set, Engine& antb1,
                     Engine& oracle) {
  const bool supported = supported_set.Contains(q.features);
  if (q.sql.empty()) {
    return Fail(Discrepancy{.what = "the generator produced no query (a generator bug)"});
  }
  const auto a = antb1.Execute(q.sql);
  const auto o = oracle.Execute(q.sql);
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
  if (auto d = CompareAnswers(*o, *a, q.sort, q.row_count_only)) {
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
  out += options.redact ? "  repro (unredacted, prints values; run it locally):\n" : "  repro:\n";
  out += std::format("    ANTB1_DIFF_SEED={} ANTB1_DIFF_ONLY={} pixi run diff-random\n", seed,
                     q.index);
  if (!options.command.empty()) {
    out += std::format("    {} --only {}\n", options.command, q.index);
  }
}

}  // namespace

DiffStats RunDiff(const QueryGenerator& generator, Engine& antb1, Engine& oracle,
                  const DiffOptions& options, std::string& out) {
  DiffStats stats;
  const FeatureSet supported_set = generator.options().supported;
  const uint64_t first = options.only.value_or(0);
  const uint64_t last = options.only.has_value() ? first + 1 : options.count;
  for (uint64_t i = first; i < last; ++i) {
    const GeneratedQuery q = generator.Generate(i);
    const bool supported = supported_set.Contains(q.features);
    ++stats.queries;
    stats.supported += supported ? 1 : 0;
    stats.target += q.target_sample ? 1 : 0;
    const CaseResult r = CheckCase(q, supported_set, antb1, oracle);
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
        if (stats.rejected_cases.size() < kMaxListedRejects) {
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
  if (stats.rejected > 0) {
    std::string cases;
    for (const uint64_t c : stats.rejected_cases) {
      cases += std::format("{}{}", cases.empty() ? "" : " ", c);
    }
    out += std::format(
        "antb1-slt diff: NOTE: antb1 rejected {} target-grammar quer{} with a query error instead "
        "of Unsupported (not failures; first cases: {}; see one with ANTB1_DIFF_SEED={} "
        "ANTB1_DIFF_ONLY=<case> pixi run diff-random)\n",
        stats.rejected, stats.rejected == 1 ? "y" : "ies", cases, generator.seed());
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
