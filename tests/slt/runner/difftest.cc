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

#include "canonical.h"
#include "engine.h"
#include "query_gen.h"
#include "sha256.h"
#include "supported_features.h"

namespace antb1::slt {
namespace {

constexpr std::size_t kMaxDiffRows = 5;

std::string Letters(const ResultSet& r) {
  std::string letters;
  for (const auto c : r.classes) {
    letters += ClassLetter(c);
  }
  return letters;
}

std::string Types(const ResultSet& r) {
  std::string names;
  for (const auto& n : r.type_names) {
    names += (names.empty() ? "" : ", ") + n;
  }
  return std::format("{} ({})", Letters(r), names);
}

std::string Join(const std::vector<std::string>& lines) {
  std::string text;
  for (const auto& line : lines) {
    text += line;
    text += '\n';
  }
  return text;
}

struct Failure {
  std::string what;      // one line; safe to print in redacted mode
  std::string detail;    // unredacted details (error messages)
  std::string redacted;  // details that are safe to print
  bool mismatch = false;
  std::string types;                  // I/R/T per column (R compares with a tolerance)
  std::vector<std::string> expected;  // DuckDB's block
  std::vector<std::string> actual;    // antb1's block
  std::optional<std::size_t> first_row;
};

enum class Outcome : std::uint8_t { kCompared, kUnsupported, kRejected, kFailed };

constexpr std::size_t kMaxListedRejects = 5;

struct CaseResult {
  Outcome outcome = Outcome::kCompared;
  Failure failure;
};

CaseResult Fail(Failure f) {
  return CaseResult{.outcome = Outcome::kFailed, .failure = std::move(f)};
}

Failure ErrorFailure(std::string what, const EngineError& error) {
  return Failure{.what = std::move(what),
                 .detail = error.message,
                 .redacted = std::format("error kind: {}", error.kind)};
}

CaseResult Compare(const GeneratedQuery& q, const ResultSet& oracle, const ResultSet& antb1) {
  const std::string letters = Letters(oracle);
  // The engine types too, not only the I/R/T classes: e.g. an integer SUM must be HUGEINT, as in
  // DuckDB, even while its values still fit a BIGINT.
  if (letters != Letters(antb1) || oracle.type_names != antb1.type_names) {
    return Fail(Failure{.what = std::format("column types differ: DuckDB {}, antb1 {}",
                                            Types(oracle), Types(antb1))});
  }
  auto expected = RenderBlock(oracle, q.sort, 0);
  auto actual = RenderBlock(antb1, q.sort, 0);
  if (q.row_count_only) {
    if (expected.size() == actual.size()) {
      return {};
    }
    return Fail(Failure{
        .what = std::format("row counts differ (LIMIT on a projection: any {} rows are right)",
                            expected.size()),
        .mismatch = true,
        .types = letters,
        .expected = std::move(expected),
        .actual = std::move(actual),
        .first_row = std::nullopt});
  }
  if (auto diff = CompareBlocks(expected, actual, letters, q.sort, kDefaultRelTolerance)) {
    return Fail(Failure{.what = "result mismatch: " + diff->reason,
                        .mismatch = true,
                        .types = letters,
                        .expected = std::move(expected),
                        .actual = std::move(actual),
                        .first_row = diff->first_row});
  }
  return {};
}

CaseResult CheckCase(const GeneratedQuery& q, FeatureSet supported_set, Engine& antb1,
                     Engine& oracle) {
  const bool supported = supported_set.Contains(q.features);
  if (q.sql.empty()) {
    return Fail(Failure{.what = "the generator produced no query (a generator bug)"});
  }
  const auto a = antb1.Execute(q.sql);
  const auto o = oracle.Execute(q.sql);
  if (!o.has_value()) {
    return Fail(ErrorFailure(
        "DuckDB rejects the generated SQL (a generator bug: queries must be valid DuckDB SQL)",
        o.error()));
  }
  if (!a.has_value()) {
    if (a.error().unsupported && !supported) {
      return CaseResult{.outcome = Outcome::kUnsupported, .failure = {}};
    }
    if (a.error().unsupported) {
      Failure f = ErrorFailure(
          "antb1 reports Unsupported for a query that uses only supported features", a.error());
      f.detail += std::format("\n  declared supported (tests/slt/supported_features.h): {}",
                              supported_set.Names());
      return Fail(std::move(f));
    }
    if (!supported && !a.error().internal) {
      return CaseResult{.outcome = Outcome::kRejected, .failure = {}};
    }
    return Fail(ErrorFailure(std::format("antb1 fails ({} error), DuckDB answers", a.error().kind),
                             a.error()));
  }
  return Compare(q, *o, *a);
}

bool SameLine(const std::string& e, const std::string& a, std::string_view types, SortMode sort) {
  return !CompareBlocks({e}, {a}, types, sort, kDefaultRelTolerance).has_value();
}

void AppendDifferingRows(const Failure& f, SortMode sort, std::string& out) {
  out += std::format("  DuckDB: {} row(s), antb1: {} row(s); differing rows (at most {}):\n",
                     f.expected.size(), f.actual.size(), kMaxDiffRows);
  std::size_t shown = 0;
  const std::size_t n = std::max(f.expected.size(), f.actual.size());
  for (std::size_t i = 0; i < n && shown < kMaxDiffRows; ++i) {
    const bool has_e = i < f.expected.size();
    const bool has_a = i < f.actual.size();
    if (has_e && has_a && SameLine(f.expected[i], f.actual[i], f.types, sort)) {
      continue;
    }
    const std::string label = std::format("row {}:", i);
    out += std::format("    {} DuckDB {}\n", label, has_e ? f.expected[i] : "(no row)");
    out += std::format("    {:{}} antb1  {}\n", "", label.size(), has_a ? f.actual[i] : "(no row)");
    ++shown;
  }
}

void Report(const GeneratedQuery& q, uint64_t seed, bool supported, const Failure& f,
            const DiffOptions& options, std::string& out) {
  out += std::format("FAIL diff case {} (seed {}, {}): {}\n", q.index, seed,
                     supported ? "supported features" : "target grammar sample", f.what);
  out += std::format("  features: {}\n", q.features.Names());
  if (options.redact) {
    if (!f.redacted.empty()) {
      out += "  " + f.redacted + "\n";
    }
    if (f.mismatch) {
      out += std::format("  rows: DuckDB {}, antb1 {}", f.expected.size(), f.actual.size());
      if (f.first_row.has_value()) {
        out += std::format("; first differing row: {}", *f.first_row);
      }
      out += std::format("\n  sha256: DuckDB {}\n          antb1  {}\n",
                         Sha256Hex(Join(f.expected)), Sha256Hex(Join(f.actual)));
    }
    out += "  repro (unredacted, prints values; run it locally):\n";
  } else {
    if (!f.detail.empty()) {
      out += "  " + f.detail + "\n";
    }
    out += "  SQL:\n";
    std::size_t pos = 0;
    while (pos <= q.sql.size()) {
      const std::size_t end = std::min(q.sql.find('\n', pos), q.sql.size());
      out += "    " + q.sql.substr(pos, end - pos) + "\n";
      pos = end + 1;
    }
    if (f.mismatch) {
      AppendDifferingRows(f, q.sort, out);
    }
    out += "  repro:\n";
  }
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
