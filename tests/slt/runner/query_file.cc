#include "query_file.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "canonical.h"
#include "engine.h"
#include "result_diff.h"
#include "supported_features.h"

namespace antb1::slt {
namespace {

std::string_view Trim(std::string_view s) {
  const std::size_t begin = s.find_first_not_of(" \t\r");
  if (begin == std::string_view::npos) {
    return {};
  }
  const std::size_t end = s.find_last_not_of(" \t\r");
  return s.substr(begin, end - begin + 1);
}

std::expected<FeatureSet, std::string> ParseFeatures(std::string_view list) {
  FeatureSet features;
  std::size_t pos = 0;
  while (pos <= list.size()) {
    const std::size_t end = std::min(list.find_first_of(", \t", pos), list.size());
    const std::string_view name = list.substr(pos, end - pos);
    if (!name.empty()) {
      const auto f = FeatureByName(name);
      if (!f.has_value()) {
        return std::unexpected(std::format(
            "unknown feature '{}' (the names are FeatureName() in tests/slt/supported_features.h)",
            name));
      }
      features.Add(*f);
    }
    pos = end + 1;
  }
  if (features.empty()) {
    return std::unexpected("`-- features:` lists no feature");
  }
  return features;
}

struct Answer {
  enum class Kind : std::uint8_t { kCompared, kPending, kRejected, kFailed };
  Kind kind = Kind::kCompared;
  Discrepancy failure;
};

Answer Failed(Discrepancy d) {
  return Answer{.kind = Answer::Kind::kFailed, .failure = std::move(d)};
}

Answer Check(const Statement& s, FeatureSet supported_set, Engine& antb1, Engine& oracle) {
  const FeatureSet features = s.features.value_or(FeatureSet{});
  const bool supported = supported_set.Contains(features);
  const bool projection = features.Has(Feature::kColumns) || features.Has(Feature::kStar);
  const SortMode sort = projection ? SortMode::kRowSort : SortMode::kNoSort;
  const bool row_count_only = projection && features.Has(Feature::kLimit);
  const auto o = oracle.Execute(s.sql);
  if (!o.has_value()) {
    return Failed(ErrorDiscrepancy("DuckDB rejects the query (the file must be valid DuckDB SQL)",
                                   o.error()));
  }
  const auto a = antb1.Execute(s.sql);
  if (!a.has_value()) {
    const EngineError& e = a.error();
    if (e.internal) {
      return Failed(ErrorDiscrepancy("antb1 fails (internal error), DuckDB answers", e));
    }
    if (supported) {
      Discrepancy d = ErrorDiscrepancy(
          e.unsupported ? "antb1 reports Unsupported for a query that uses only supported features"
                        : std::format("antb1 fails ({} error), DuckDB answers", e.kind),
          e);
      d.detail += std::format("\n  declared supported (tests/slt/supported_features.h): {}",
                              supported_set.Names());
      return Failed(std::move(d));
    }
    if (e.unsupported) {
      return Answer{.kind = Answer::Kind::kPending, .failure = {}};
    }
    if (e.kind == "parse" || e.kind == "bind") {
      return Answer{.kind = Answer::Kind::kRejected, .failure = {}};
    }
    return Failed(ErrorDiscrepancy(
        std::format("antb1 fails with an {} error; a pending query must fail cleanly "
                    "(Unsupported, a parse or a bind error)",
                    e.kind),
        e));
  }
  if (auto d = CompareAnswers(*o, *a, sort, row_count_only)) {
    return Failed(*std::move(d));
  }
  if (!supported) {
    return Failed(Discrepancy{
        .what = std::format(
            "antb1 answers (equal to DuckDB), but the query uses features that "
            "tests/slt/supported_features.h does not declare: {}. Add them to kSupportedFeatures.",
            features.Minus(supported_set).Names())});
  }
  return {};
}

}  // namespace

std::optional<Feature> FeatureByName(std::string_view name) {
  for (std::size_t i = 0; i < kFeatureCount; ++i) {
    const auto f = static_cast<Feature>(i);
    if (FeatureName(f) == name) {
      return f;
    }
  }
  return std::nullopt;
}

std::expected<std::vector<Statement>, std::string> ParseSqlFile(std::string_view path,
                                                                std::string_view text) {
  std::vector<Statement> statements;
  std::optional<FeatureSet> features;
  int features_line = 0;
  std::optional<Statement> current;
  int line_no = 0;
  std::size_t pos = 0;
  while (pos < text.size()) {
    const std::size_t end = std::min(text.find('\n', pos), text.size());
    const std::string_view raw = text.substr(pos, end - pos);
    pos = end + 1;
    ++line_no;
    const std::string_view line = Trim(raw);
    if (!current.has_value()) {
      if (line.empty()) {
        continue;
      }
      if (line.starts_with("--")) {
        constexpr std::string_view kFeatures = "features:";
        const std::string_view comment = Trim(line.substr(2));
        if (comment.starts_with(kFeatures)) {
          if (features.has_value()) {
            return std::unexpected(
                std::format("{}:{}: a second `-- features:` line before a query", path, line_no));
          }
          auto parsed = ParseFeatures(comment.substr(kFeatures.size()));
          if (!parsed) {
            return std::unexpected(std::format("{}:{}: {}", path, line_no, parsed.error()));
          }
          features = *parsed;
          features_line = line_no;
        }
        continue;
      }
      current = Statement{.line = line_no, .sql = {}, .features = features};
      features.reset();
    } else {
      current->sql += '\n';
    }
    current->sql += raw;
    if (!line.starts_with("--") && line.ends_with(';')) {
      // Drop the terminating ';' and the whitespace around it: it ends the statement in the file
      // and is not part of the query.
      const std::size_t semicolon = current->sql.find_last_of(';');
      const std::size_t last = semicolon == 0
                                   ? std::string::npos
                                   : current->sql.find_last_not_of(" \t\r\n", semicolon - 1);
      current->sql.resize(last == std::string::npos ? 0 : last + 1);
      statements.push_back(*std::move(current));
      current.reset();
    }
  }
  if (current.has_value()) {
    return std::unexpected(
        std::format("{}:{}: the query does not end with ';'", path, current->line));
  }
  if (features.has_value()) {
    return std::unexpected(
        std::format("{}:{}: `-- features:` is not followed by a query", path, features_line));
  }
  return statements;
}

QueryFileStats RunQueryFile(std::string_view path, const std::vector<Statement>& statements,
                            FeatureSet supported, Engine& antb1, Engine& oracle,
                            const QueryFileOptions& options, std::string& out) {
  QueryFileStats stats;
  for (const auto& s : statements) {
    if (options.only_line.has_value() && std::cmp_not_equal(s.line, *options.only_line)) {
      continue;
    }
    ++stats.queries;
    const FeatureSet features = s.features.value_or(FeatureSet{});
    const Answer answer = Check(s, supported, antb1, oracle);
    switch (answer.kind) {
      case Answer::Kind::kCompared:
        ++stats.compared;
        continue;
      case Answer::Kind::kPending:
        ++stats.pending;
        continue;
      case Answer::Kind::kRejected:
        ++stats.rejected;
        continue;
      case Answer::Kind::kFailed:
        break;
    }
    ++stats.failed;
    out += std::format("FAIL {}:{}: {}\n", path, s.line, answer.failure.what);
    out += std::format("  features: {}\n", features.Names());
    const bool projection = features.Has(Feature::kColumns) || features.Has(Feature::kStar);
    AppendDiscrepancy(answer.failure, s.sql, projection ? SortMode::kRowSort : SortMode::kNoSort,
                      options.redact, out);
    out += options.redact ? "  repro (unredacted, prints values; run it locally):\n" : "  repro:\n";
    out += std::format("    {} --only {}\n", options.command, s.line);
  }
  if (options.only_line.has_value() && stats.queries == 0) {
    ++stats.failed;
    out += std::format("FAIL {}: no query starts on line {}\n", path, *options.only_line);
  }
  out +=
      std::format("QUERIES: {} file={} queries={} compared={} pending={} rejected={} failed={}\n",
                  stats.failed == 0 ? "PASS" : "FAIL", path, stats.queries, stats.compared,
                  stats.pending, stats.rejected, stats.failed);
  return stats;
}

}  // namespace antb1::slt
