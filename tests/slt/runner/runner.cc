#include "runner.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <map>
#include <memory>
#include <optional>
#include <regex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "canonical.h"
#include "engine.h"
#include "sha256.h"
#include "slt_file.h"

namespace antb1::slt {
namespace {

constexpr std::size_t kMaxPrintedLines = 20;

class MutatingEngine final : public Engine {
 public:
  MutatingEngine(Engine& inner, Mutation mutation) : inner_(inner), mutation_(mutation) {}

  [[nodiscard]] std::string_view name() const override { return inner_.name(); }

  ExecResult Execute(const std::string& sql) override {
    auto result = inner_.Execute(sql);
    switch (mutation_) {
      case Mutation::kNone:
        break;
      case Mutation::kError:
        return std::unexpected(EngineError{.kind = "execution", .message = "execution: mutated"});
      case Mutation::kUnsupported:
        return std::unexpected(EngineError{
            .kind = "unsupported", .message = "unsupported: mutated", .unsupported = true});
      case Mutation::kSucceed:
        if (!result.has_value()) {
          return ResultSet{};
        }
        break;
      case Mutation::kValue:
      case Mutation::kNull:
      case Mutation::kDropRow:
      case Mutation::kExtraRow:
      case Mutation::kExtraColumn:
      case Mutation::kCanary:
        if (result.has_value()) {
          Corrupt(*result);
        }
        break;
    }
    return result;
  }

 private:
  void Corrupt(ResultSet& r) const {
    auto& rows = r.rows;
    switch (mutation_) {
      case Mutation::kValue:
        if (!rows.empty() && !rows[0].empty()) {
          auto& v = rows[0][0];
          v = v.has_value() ? *v + "1" : "0";
        }
        break;
      case Mutation::kNull:
        if (!rows.empty() && !rows[0].empty()) {
          rows[0][0] = rows[0][0].has_value() ? std::nullopt : std::optional<std::string>("0");
        }
        break;
      case Mutation::kDropRow:
        if (!rows.empty()) {
          rows.pop_back();
        }
        break;
      case Mutation::kExtraRow:
        if (rows.empty()) {
          rows.emplace_back(r.classes.size());
        } else {
          rows.push_back(rows.front());
        }
        break;
      case Mutation::kExtraColumn:
        r.classes.push_back(ColumnClass::kInteger);
        r.type_names.emplace_back("MUTATED");
        for (auto& row : rows) {
          row.emplace_back("0");
        }
        break;
      case Mutation::kCanary:
        if (rows.empty()) {
          rows.emplace_back(r.classes.size());
        }
        if (!rows[0].empty()) {
          rows[0][0] = std::string(kCanaryValue);
        }
        break;
      case Mutation::kNone:
      case Mutation::kError:
      case Mutation::kUnsupported:
      case Mutation::kSucceed:
        break;
    }
  }

  Engine& inner_;
  Mutation mutation_;
};

std::string_view SortName(SortMode sort) {
  switch (sort) {
    case SortMode::kNoSort:
      return "nosort";
    case SortMode::kRowSort:
      return "rowsort";
    case SortMode::kValueSort:
      return "valuesort";
  }
  return "?";
}

// The record header without anything that could carry data (no regex, no label).
std::string SafeHeader(const Record& r) {
  switch (r.kind) {
    case RecordKind::kStatementOk:
      return "statement ok";
    case RecordKind::kStatementError:
      return "statement error";
    case RecordKind::kQuery:
      return std::format("query {} {}", r.types, SortName(r.sort));
    case RecordKind::kHalt:
      return "halt";
    case RecordKind::kHashThreshold:
      return "hash-threshold";
  }
  return "?";
}

std::string Letters(const ResultSet& r) {
  std::string letters;
  for (const auto c : r.classes) {
    letters += ClassLetter(c);
  }
  return letters;
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
  std::string what;             // one line, safe to print in redacted mode
  std::string detail;           // unredacted details (messages, values)
  std::string redacted_detail;  // details that are safe to print
  bool unsupported = false;
  bool is_mismatch = false;
  std::vector<std::string> expected;
  std::vector<std::string> actual;
  std::optional<std::size_t> first_row;
};

Failure ErrorFailure(const EngineError& error, std::string_view engine, std::string_view context) {
  Failure f;
  if (error.unsupported) {
    f.unsupported = true;
    f.what = std::format("{} reports Unsupported", engine);
    f.detail = std::format(
        "{}\n  Records reflect current support: an Unsupported result is a failure. Guard the "
        "record with\n"
        "  `onlyif duckdb` until {} supports it (then remove the guard).",
        error.message, engine);
  } else {
    // DuckDB kinds already read "Catalog Error"; antb1 kinds are "parse", "bind", ...
    f.what = std::format("{} ({}{})", context, error.kind,
                         error.kind.ends_with(" Error") ? "" : " error");
    f.detail = error.message;
  }
  f.redacted_detail = std::format("error kind: {}", error.kind);
  return f;
}

using Labels = std::map<std::string, std::vector<std::string>, std::less<>>;

std::optional<Failure> CheckQuery(const Record& r, const ResultSet& rs, int64_t hash_threshold,
                                  Labels& labels) {
  const std::string letters = Letters(rs);
  if (letters != r.types) {
    Failure f;
    std::string names;
    for (const auto& n : rs.type_names) {
      names += (names.empty() ? "" : ", ") + n;
    }
    f.what = std::format("column types differ: expected {}, got {} ({})", r.types, letters, names);
    return f;
  }
  if (!r.has_separator) {
    return Failure{.what = "no expected results ('----'): write them with `pixi run slt-complete`"};
  }
  auto block = RenderBlock(rs, r.sort, hash_threshold);
  const double tolerance = r.tolerance.value_or(kDefaultRelTolerance);
  if (auto diff = CompareBlocks(r.expected, block, r.types, r.sort, tolerance)) {
    return Failure{.what = "result mismatch: " + diff->reason,
                   .is_mismatch = true,
                   .expected = r.expected,
                   .actual = std::move(block),
                   .first_row = diff->first_row};
  }
  if (!r.label.empty()) {
    const auto [it, inserted] = labels.try_emplace(r.label, block);
    if (!inserted && it->second != block) {
      return Failure{
          .what = std::format("result differs from the earlier query labelled '{}'", r.label),
          .is_mismatch = true,
          .expected = it->second,
          .actual = std::move(block)};
    }
  }
  return std::nullopt;
}

std::optional<Failure> CheckRecord(const Record& r, Engine& engine, int64_t hash_threshold,
                                   Labels& labels) {
  auto result = engine.Execute(r.sql);
  switch (r.kind) {
    case RecordKind::kStatementOk:
      if (!result.has_value()) {
        return ErrorFailure(result.error(), engine.name(), "statement failed");
      }
      return std::nullopt;
    case RecordKind::kStatementError: {
      if (result.has_value()) {
        return Failure{.what = "statement succeeded, but an error was expected"};
      }
      const EngineError& error = result.error();
      if (error.unsupported || error.internal) {
        return ErrorFailure(error, engine.name(), "internal error instead of the expected error");
      }
      if (!r.error_regex.empty() && !std::regex_search(error.message, r.error_pattern)) {
        Failure f;
        f.what = "the error does not match the expected regex";
        f.detail = std::format("regex: {}\n  error: {}", r.error_regex, error.message);
        f.redacted_detail = std::format("error kind: {}", error.kind);
        return f;
      }
      return std::nullopt;
    }
    case RecordKind::kQuery:
      if (!result.has_value()) {
        return ErrorFailure(result.error(), engine.name(), "query failed");
      }
      return CheckQuery(r, *result, hash_threshold, labels);
    case RecordKind::kHalt:
    case RecordKind::kHashThreshold:
      break;
  }
  return std::nullopt;
}

void AppendLines(std::string& out, std::string_view title, const std::vector<std::string>& lines) {
  out += std::format("  {} ({} line{}):\n", title, lines.size(), lines.size() == 1 ? "" : "s");
  for (std::size_t i = 0; i < lines.size() && i < kMaxPrintedLines; ++i) {
    out += "    " + lines[i] + "\n";
  }
  if (lines.size() > kMaxPrintedLines) {
    out += std::format("    ... {} more\n", lines.size() - kMaxPrintedLines);
  }
}

void Report(const SltFile& file, const Record& r, std::string_view engine, const Failure& f,
            const RunOptions& options, std::string& out) {
  out += std::format("FAIL {}:{}: {} [{}]: {}\n", file.path, r.line, SafeHeader(r), engine, f.what);
  if (options.redact) {
    if (!f.redacted_detail.empty()) {
      out += "  " + f.redacted_detail + "\n";
    }
    if (f.is_mismatch) {
      out += std::format("  rows: expected {}, actual {}", f.expected.size(), f.actual.size());
      if (f.first_row.has_value()) {
        out += std::format("; first differing row: {}", *f.first_row);
      }
      out += std::format("\n  sha256: expected {}\n          actual   {}\n",
                         Sha256Hex(Join(f.expected)), Sha256Hex(Join(f.actual)));
    }
    out += "  repro (unredacted, prints values; run it locally):\n" + options.repro;
    return;
  }
  if (!f.detail.empty()) {
    out += "  " + f.detail + "\n";
  }
  out += "  SQL:\n";
  std::size_t pos = 0;
  while (pos <= r.sql.size()) {
    const std::size_t end = std::min(r.sql.find('\n', pos), r.sql.size());
    out += "    " + r.sql.substr(pos, end - pos) + "\n";
    pos = end + 1;
  }
  if (f.is_mismatch) {
    AppendLines(out, "expected", f.expected);
    AppendLines(out, "actual", f.actual);
    if (f.first_row.has_value()) {
      out += std::format("  first difference at line {} of the block\n", *f.first_row);
    }
  }
  out += "  repro:\n" + options.repro;
}

}  // namespace

std::optional<Mutation> ParseMutation(std::string_view name) {
  static constexpr std::array<std::pair<std::string_view, Mutation>, 10> kNames = {{
      {"none", Mutation::kNone},
      {"value", Mutation::kValue},
      {"null", Mutation::kNull},
      {"drop-row", Mutation::kDropRow},
      {"extra-row", Mutation::kExtraRow},
      {"extra-column", Mutation::kExtraColumn},
      {"error", Mutation::kError},
      {"unsupported", Mutation::kUnsupported},
      {"succeed", Mutation::kSucceed},
      {"canary", Mutation::kCanary},
  }};
  for (const auto& [n, m] : kNames) {
    if (n == name) {
      return m;
    }
  }
  return std::nullopt;
}

std::unique_ptr<Engine> MakeMutatingEngine(Engine& inner, Mutation mutation) {
  return std::make_unique<MutatingEngine>(inner, mutation);
}

void SubstituteVariables(SltFile& file, std::string_view fixtures_dir) {
  constexpr std::string_view kFixtures = "${FIXTURES}";
  for (auto& r : file.records) {
    std::size_t pos = 0;
    while ((pos = r.sql.find(kFixtures, pos)) != std::string::npos) {
      r.sql.replace(pos, kFixtures.size(), fixtures_dir);
      pos += fixtures_dir.size();
    }
  }
}

RunStats RunFile(const SltFile& file, Engine& engine, const RunOptions& options, std::string& out) {
  RunStats stats;
  int64_t hash_threshold = 0;
  Labels labels;
  for (const auto& r : file.records) {
    if (!r.RunsOn(engine.name())) {
      stats.skipped += r.kind == RecordKind::kHalt || r.kind == RecordKind::kHashThreshold ? 0 : 1;
      continue;
    }
    if (r.kind == RecordKind::kHashThreshold) {
      hash_threshold = r.hash_threshold;
      continue;
    }
    if (r.kind == RecordKind::kHalt) {
      stats.halted = true;
      out += std::format("{}:{}: halt: the remaining records are not run on {}\n", file.path,
                         r.line, engine.name());
      break;
    }
    ++stats.records;
    const auto failure = CheckRecord(r, engine, hash_threshold, labels);
    if (failure.has_value()) {
      ++stats.failed;
      stats.unsupported += failure->unsupported ? 1 : 0;
      Report(file, r, engine.name(), *failure, options, out);
    } else {
      ++stats.passed;
    }
  }
  out += std::format(
      "antb1-slt: {}: engine={} records={} passed={} failed={} skipped={} unsupported={}{}\n",
      file.path, engine.name(), stats.records, stats.passed, stats.failed, stats.skipped,
      stats.unsupported, stats.halted ? " (halted)" : "");
  return stats;
}

CompleteStats CompleteFile(const SltFile& file, Engine& oracle, Engine& antb1,
                           std::string& new_text, std::string& out) {
  CompleteStats stats;
  std::map<std::size_t, BlockUpdate> updates;
  struct Flow {
    Engine* engine;
    int64_t hash_threshold = 0;
    bool halted = false;
  };
  std::array<Flow, 2> flows = {{{.engine = &oracle}, {.engine = &antb1}}};
  auto problem = [&](const Record& r, std::string_view engine, const std::string& message) {
    ++stats.errors;
    out += std::format("ERROR {}:{}: {} [{}]: {}\n", file.path, r.line, SafeHeader(r), engine,
                       message);
  };
  for (std::size_t i = 0; i < file.records.size(); ++i) {
    const Record& r = file.records[i];
    if (r.kind == RecordKind::kHashThreshold || r.kind == RecordKind::kHalt) {
      for (auto& flow : flows) {
        if (r.RunsOn(flow.engine->name())) {
          flow.hash_threshold =
              r.kind == RecordKind::kHashThreshold ? r.hash_threshold : flow.hash_threshold;
          flow.halted = flow.halted || r.kind == RecordKind::kHalt;
        }
      }
      continue;
    }
    // The oracle writes every record it runs; antb1 only those DuckDB does not run.
    const Flow* flow = nullptr;
    if (r.RunsOn(oracle.name())) {
      flow = flows.data();
    } else if (r.RunsOn(antb1.name())) {
      flow = &flows[1];
    }
    if (flow == nullptr || flow->halted) {
      continue;
    }
    Engine& engine = *flow->engine;
    auto result = engine.Execute(r.sql);
    if (r.kind == RecordKind::kStatementOk) {
      if (!result.has_value()) {
        problem(r, engine.name(), "statement failed: " + result.error().message);
      }
      continue;
    }
    if (r.kind == RecordKind::kStatementError) {
      if (result.has_value()) {
        problem(r, engine.name(), "statement succeeded, but an error is expected");
      } else if (!r.error_regex.empty() &&
                 !std::regex_search(result.error().message, r.error_pattern)) {
        problem(r, engine.name(), "the error does not match the regex: " + result.error().message);
      }
      continue;
    }
    if (!result.has_value()) {
      problem(r, engine.name(), "query failed: " + result.error().message);
      continue;
    }
    BlockUpdate update{.lines = RenderBlock(*result, r.sort, flow->hash_threshold),
                       .new_types = std::nullopt};
    if (const std::string letters = Letters(*result); letters != r.types) {
      out +=
          std::format("NOTE {}:{}: column types {} -> {}\n", file.path, r.line, r.types, letters);
      update.new_types = letters;
    }
    if (&engine == &antb1) {
      ++stats.from_antb1;
      out += std::format(
          "REVIEW {}:{}: onlyif antb1: expected results written by antb1, not by the oracle\n",
          file.path, r.line);
    }
    ++stats.queries;
    updates.emplace(i, std::move(update));
  }
  new_text = RewriteSlt(file, updates);
  return stats;
}

}  // namespace antb1::slt
