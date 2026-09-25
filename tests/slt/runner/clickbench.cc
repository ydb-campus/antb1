#include "clickbench.h"

#include <algorithm>
#include <cctype>
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
#include "query_file.h"
#include "result_diff.h"

namespace antb1::slt {
namespace {

// A strict reader for the small JSON object of the status file: string keys, and values that are
// strings (without escapes) or arrays of non-negative integers.
class StatusReader {
 public:
  explicit StatusReader(std::string_view text) : text_(text) {}

  std::expected<ClickBenchStatus, std::string> Read() {
    ClickBenchStatus status;
    bool have_commit = false;
    bool have_pass = false;
    if (!Consume('{')) {
      return Error("expected '{'");
    }
    while (true) {
      auto key = String();
      if (!key) {
        return std::unexpected(key.error());
      }
      if (!Consume(':')) {
        return Error("expected ':'");
      }
      if (*key == "clickbench_commit" && !have_commit) {
        auto commit = String();
        if (!commit) {
          return std::unexpected(commit.error());
        }
        status.commit = *std::move(commit);
        have_commit = true;
      } else if (*key == "pass" && !have_pass) {
        auto pass = IntegerArray();
        if (!pass) {
          return std::unexpected(pass.error());
        }
        status.pass = *std::move(pass);
        have_pass = true;
      } else {
        return Error(std::format("unexpected or repeated key \"{}\"", *key));
      }
      if (Consume('}')) {
        break;
      }
      if (!Consume(',')) {
        return Error("expected ',' or '}'");
      }
    }
    SkipSpace();
    if (pos_ != text_.size()) {
      return Error("text after the object");
    }
    if (!have_commit || !have_pass) {
      return Error(R"(needs the keys "clickbench_commit" and "pass")");
    }
    if (!std::ranges::is_sorted(status.pass) ||
        std::ranges::adjacent_find(status.pass) != status.pass.end()) {
      return Error("\"pass\" must be sorted and without duplicates");
    }
    return status;
  }

 private:
  std::unexpected<std::string> Error(std::string_view what) const {
    return std::unexpected(std::format("offset {}: {}", pos_, what));
  }

  void SkipSpace() {
    while (pos_ < text_.size() && std::isspace(static_cast<unsigned char>(text_[pos_])) != 0) {
      ++pos_;
    }
  }

  bool Consume(char c) {
    SkipSpace();
    if (pos_ < text_.size() && text_[pos_] == c) {
      ++pos_;
      return true;
    }
    return false;
  }

  std::expected<std::string, std::string> String() {
    if (!Consume('"')) {
      return Error("expected a string");
    }
    const std::size_t end = text_.find('"', pos_);
    if (end == std::string_view::npos) {
      return Error("unterminated string");
    }
    std::string value(text_.substr(pos_, end - pos_));
    if (value.contains('\\')) {
      return Error("escapes are not supported");
    }
    pos_ = end + 1;
    return value;
  }

  std::expected<std::vector<int64_t>, std::string> IntegerArray() {
    std::vector<int64_t> values;
    if (!Consume('[')) {
      return Error("expected an array");
    }
    if (Consume(']')) {
      return values;
    }
    while (true) {
      SkipSpace();
      int64_t value = 0;
      const std::size_t start = pos_;
      while (pos_ < text_.size() && std::isdigit(static_cast<unsigned char>(text_[pos_])) != 0) {
        if (value > 1'000'000) {
          return Error("number too large");
        }
        value = (value * 10) + (text_[pos_] - '0');
        ++pos_;
      }
      if (pos_ == start) {
        return Error("expected a non-negative integer");
      }
      values.push_back(value);
      if (Consume(']')) {
        return values;
      }
      if (!Consume(',')) {
        return Error("expected ',' or ']'");
      }
    }
  }

  std::string_view text_;
  std::size_t pos_ = 0;
};

enum class Outcome : std::uint8_t { kPass, kUnsupported, kRejected, kWrong, kUnclean };

struct QueryOutcome {
  Outcome outcome = Outcome::kPass;
  std::string label;  // safe to print redacted: pass, unsupported, parse error, ...
  Discrepancy failure;
};

QueryOutcome Check(const Statement& q, Engine& antb1, Engine& oracle) {
  const auto a = antb1.Execute(q.sql);
  if (!a.has_value()) {
    const EngineError& e = a.error();
    if (e.unsupported) {
      return {.outcome = Outcome::kUnsupported, .label = "unsupported", .failure = {}};
    }
    if (!e.internal && (e.kind == "parse" || e.kind == "bind")) {
      return {.outcome = Outcome::kRejected, .label = e.kind + " error", .failure = {}};
    }
    return {.outcome = Outcome::kUnclean,
            .label = e.kind + " error",
            .failure = ErrorDiscrepancy(
                std::format("antb1 fails with an {} error; a query outside the supported subset "
                            "must fail cleanly (Unsupported, a parse or a bind error)",
                            e.kind),
                e)};
  }
  const auto o = oracle.Execute(q.sql);
  if (!o.has_value()) {
    return {.outcome = Outcome::kWrong,
            .label = "DuckDB error",
            .failure = ErrorDiscrepancy("antb1 answers, but DuckDB fails", o.error())};
  }
  if (auto d = CompareAnswers(*o, *a, SortMode::kRowSort, /*row_count_only=*/false)) {
    return {.outcome = Outcome::kWrong, .label = "wrong answer", .failure = *std::move(d)};
  }
  return {.outcome = Outcome::kPass, .label = "pass", .failure = {}};
}

std::string List(const std::vector<int64_t>& values) {
  std::string out;
  for (const int64_t v : values) {
    out += std::format("{}{}", out.empty() ? "" : ", ", v);
  }
  return "[" + out + "]";
}

}  // namespace

std::expected<ClickBenchStatus, std::string> ParseClickBenchStatus(std::string_view json) {
  return StatusReader(json).Read();
}

ClickBenchStats RunClickBench(const std::vector<Statement>& queries, const ClickBenchStatus& status,
                              Engine& antb1, Engine& oracle, const ClickBenchOptions& options,
                              std::string& out) {
  ClickBenchStats stats;
  const auto repro = [&](uint64_t n) {
    out += options.redact ? "  repro (unredacted, prints values and query text; run it locally):\n"
                          : "  repro:\n";
    out += std::format("    {} --only {}\n", options.command, n);
  };
  const auto fail = [&](uint64_t n, std::string_view what) {
    ++stats.failed;
    out += std::format("FAIL Q{}: {}\n", n, what);
  };
  for (const int64_t n : status.pass) {
    if (std::cmp_greater_equal(n, queries.size())) {
      fail(static_cast<uint64_t>(n), std::format("{} lists Q{}, but the query file has {} queries",
                                                 options.status_path, n, queries.size()));
    }
  }
  if (options.only.has_value() && *options.only >= queries.size()) {
    fail(*options.only, std::format("no such query (the file has {} queries)", queries.size()));
  }
  for (std::size_t i = 0; i < queries.size(); ++i) {
    if (options.only.has_value() && *options.only != i) {
      continue;
    }
    ++stats.queries;
    const QueryOutcome r = Check(queries[i], antb1, oracle);
    out += std::format("Q{}: {}\n", i, r.label);
    const bool expected_pass = std::ranges::binary_search(status.pass, static_cast<int64_t>(i));
    switch (r.outcome) {
      case Outcome::kPass:
        stats.passed.push_back(static_cast<int64_t>(i));
        if (!expected_pass) {
          fail(i, std::format("unexpected pass: antb1 answers Q{0} and equals DuckDB, but the "
                              "ratchet {1} does not list it. Add {0} to \"pass\" there and mark "
                              "Q{0} `pass` in the ClickBench status table of docs/sql-subset.md, "
                              "in the same PR.",
                              i, options.status_path));
        }
        continue;
      case Outcome::kUnsupported:
        ++stats.unsupported;
        break;
      case Outcome::kRejected:
        ++stats.rejected;
        break;
      case Outcome::kWrong:
      case Outcome::kUnclean:
        fail(i, r.failure.what);
        AppendDiscrepancy(r.failure, queries[i].sql, SortMode::kRowSort, options.redact, out);
        repro(i);
        break;
    }
    if (expected_pass && (r.outcome == Outcome::kUnsupported || r.outcome == Outcome::kRejected)) {
      fail(i, std::format("unexpected fail: the ratchet {} lists Q{}, but it no longer passes ({})",
                          options.status_path, i, r.label));
      repro(i);
    }
  }
  out += std::format(
      "CLICKBENCH: {} queries={} pass={} unsupported={} rejected={} failed={} ratchet={} "
      "clickbench_commit={}\n",
      stats.failed == 0 ? "PASS" : "FAIL", stats.queries, List(stats.passed), stats.unsupported,
      stats.rejected, stats.failed, List(status.pass), status.commit);
  return stats;
}

}  // namespace antb1::slt
