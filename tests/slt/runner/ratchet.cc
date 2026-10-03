#include "ratchet.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "engine.h"
#include "ordered_compare.h"
#include "query_file.h"
#include "result_diff.h"

namespace antb1::slt {
namespace {

// A strict reader for the small JSON object of a status file: string keys, and values that are
// strings (without escapes) or arrays of non-negative integers.
class StatusReader {
 public:
  StatusReader(std::string_view text, std::string_view commit_key)
      : text_(text), commit_key_(commit_key) {}

  std::expected<RatchetStatus, std::string> Read() {
    RatchetStatus status;
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
      if (!commit_key_.empty() && *key == commit_key_ && !have_commit) {
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
    if (!have_pass || (!commit_key_.empty() && !have_commit)) {  // with a commit key
      return Error(std::format(R"(needs the keys "{}" and "pass")", commit_key_));
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
  std::string_view commit_key_;
  std::size_t pos_ = 0;
};

enum class Outcome : std::uint8_t { kPass, kUnsupported, kRejected, kWrong, kUnclean };

struct QueryOutcome {
  Outcome outcome = Outcome::kPass;
  std::string label;  // safe to print redacted: pass, unsupported, parse error, ...
  Discrepancy failure;
};

QueryOutcome Check(const Statement& q, const Workload& workload, Engine& antb1, Engine& oracle) {
  const auto a = antb1.Execute(q.sql);
  if (!a.has_value()) {
    const EngineError& e = a.error();
    if (e.unsupported) {
      return {.outcome = Outcome::kUnsupported, .label = "unsupported", .failure = {}};
    }
    const bool rejection = !e.internal && (e.kind == "parse" || e.kind == "bind");
    if (rejection && workload.rejections_clean) {
      return {.outcome = Outcome::kRejected, .label = e.kind + " error", .failure = {}};
    }
    return {
        .outcome = Outcome::kUnclean,
        .label = e.kind + " error",
        .failure = ErrorDiscrepancy(
            std::format(
                "antb1 fails with {} {} error; {}",
                std::string_view("aeiou").contains(e.kind.empty() ? 'x' : e.kind[0]) ? "an" : "a",
                e.kind, workload.unclean_rule),
            e)};
  }
  const auto o = oracle.Execute(q.sql);
  if (!o.has_value()) {
    return {.outcome = Outcome::kWrong,
            .label = "DuckDB error",
            .failure = ErrorDiscrepancy("antb1 answers, but DuckDB fails", o.error())};
  }
  // ORDER BY: in order, ties in any order; LIMIT without ORDER BY: any rows of the unlimited
  // answer; otherwise any row order.
  if (auto d = CompareQueryAnswers(q.sql, *o, *a, oracle, /*rows=*/true, SortMode::kRowSort)) {
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

std::expected<RatchetStatus, std::string> ParseRatchetStatus(std::string_view json,
                                                             const Workload& workload) {
  return StatusReader(json, workload.commit_key).Read();
}

RatchetStats RunRatchet(const std::vector<Statement>& queries, const RatchetStatus& status,
                        const Workload& workload, Engine& antb1, Engine& oracle,
                        const RatchetOptions& options,
                        const std::function<void(std::string_view)>& write) {
  RatchetStats stats;
  const auto count = static_cast<int64_t>(queries.size());
  const int64_t first = workload.first;
  const auto repro = [&](int64_t n) {
    write(options.redact ? "  repro (unredacted, prints values and query text; run it locally):\n"
                         : "  repro:\n");
    write(std::format("    {} --only {}\n", options.command, n));
  };
  const auto fail = [&](int64_t n, std::string_view what) {
    ++stats.failed;
    write(std::format("FAIL Q{}: {}\n", n, what));
  };
  for (const int64_t n : status.pass) {
    if (n < first || n - first >= count) {
      fail(n, std::format("{} lists Q{}, but the query file has {} queries", options.status_path, n,
                          count));
    }
  }
  if (options.only.has_value() && (std::cmp_less(*options.only, first) ||
                                   std::cmp_greater_equal(*options.only, first + count))) {
    fail(static_cast<int64_t>(*options.only),
         std::format("no such query (the file has {} queries)", count));
  }
  double log_sum = 0;  // of the passing queries' seconds
  for (int64_t i = 0; i < count; ++i) {
    const int64_t n = first + i;
    if (options.only.has_value() && std::cmp_not_equal(*options.only, n)) {
      continue;
    }
    ++stats.queries;
    if (workload.running_lines) {
      write(std::format("Q{}: running\n", n));
    }
    const double start = options.clock ? options.clock() : 0;
    const Statement& query = queries[static_cast<std::size_t>(i)];
    const QueryOutcome r = Check(query, workload, antb1, oracle);
    if (options.clock) {
      const double seconds = std::max(options.clock() - start, 1e-6);
      write(std::format("Q{}: {} ({:.3f} s)\n", n, r.label, seconds));
      if (r.outcome == Outcome::kPass) {
        log_sum += std::log(seconds);
      }
    } else {
      write(std::format("Q{}: {}\n", n, r.label));
    }
    const bool expected_pass = std::ranges::binary_search(status.pass, n);
    switch (r.outcome) {
      case Outcome::kPass:
        stats.passed.push_back(n);
        if (!expected_pass) {
          fail(n, std::format("unexpected pass: antb1 answers Q{0} and equals DuckDB, but the "
                              "ratchet {1} does not list it. Add {0} to \"pass\" there and mark "
                              "Q{0} `pass` in {2} of docs/sql-subset.md, in the same PR.",
                              n, options.status_path, workload.table));
        }
        continue;
      case Outcome::kUnsupported:
        ++stats.unsupported;
        break;
      case Outcome::kRejected:
        ++stats.rejected;
        break;
      case Outcome::kWrong:
      case Outcome::kUnclean: {
        fail(n, r.failure.what);
        std::string block;
        AppendDiscrepancy(r.failure, query.sql, SortMode::kRowSort, options.redact, block);
        write(block);
        repro(n);
        break;
      }
    }
    if (expected_pass && (r.outcome == Outcome::kUnsupported || r.outcome == Outcome::kRejected)) {
      fail(n, std::format("unexpected fail: the ratchet {} lists Q{}, but it no longer passes ({})",
                          options.status_path, n, r.label));
      repro(n);
    }
  }
  std::string summary = std::format(
      "{}: {} queries={} pass={} unsupported={} rejected={} failed={} ratchet={}", workload.summary,
      stats.failed == 0 ? "PASS" : "FAIL", stats.queries, List(stats.passed), stats.unsupported,
      stats.rejected, stats.failed, List(status.pass));
  if (!workload.commit_key.empty()) {
    summary += std::format(" {}={}", workload.commit_key, status.commit);
  }
  if (options.clock) {
    summary += stats.passed.empty()
                   ? std::string(" geomean=n/a")
                   : std::format(" geomean={:.3f}s",
                                 std::exp(log_sum / static_cast<double>(stats.passed.size())));
  }
  write(summary + "\n");
  return stats;
}

}  // namespace antb1::slt
