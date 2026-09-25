// antb1-data-metamorphic (ctest data.hits0.metamorphic, label data): the metamorphic relations of
// hits_relations.cc over the ClickBench `hits` table, plus antb1's COUNT(*) against the rows an
// independent reader (the Parquet library, not antb1) decodes from the files.
//
//   antb1-data-metamorphic --tables FILE [--redact] [--only NAME]
//
// FILE is a tables file (tests/slt/runner/tables.h) with a `hits` table; the data tests write it
// (cmake/scripts/RunDataTest.cmake). Each relation is reported as HOLDS, PENDING (its features are
// not all declared in tests/slt/supported_features.h: counted, never skipped silently) or FAIL.
// With --redact no value, SQL or error text is printed; a failure prints the unredacted command.
// Exit codes: 0 no failure, 1 failures, 2 usage, 3 setup error, 70 internal error.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <expected>
#include <filesystem>
#include <format>
#include <map>
#include <memory>
#include <print>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <CLI/CLI.hpp>
#include <arrow/api.h>
#include <arrow/io/file.h>
#include <parquet/arrow/reader.h>

#include "antb1/engine/session.h"

#include "antb1_engine.h"
#include "engine.h"
#include "hits_relations.h"
#include "relations.h"
#include "supported_features.h"
#include "tables.h"

namespace antb1::metamorphic {
namespace {

namespace fs = std::filesystem;

constexpr int kExitFailed = 1;
constexpr int kExitUsage = 2;
constexpr int kExitSetup = 3;
constexpr std::string_view kRowCountCheck = "hits_row_count_vs_parquet_scan";

struct Args {
  std::string tables;
  bool redact = false;
  std::string only;
};

// antb1 sessions over the tables, one per batch size.
class Engines {
 public:
  explicit Engines(std::vector<slt::TableDef> tables) : tables_(std::move(tables)) {}

  std::expected<slt::Engine*, std::string> For(int64_t batch_size) {
    auto& engine = engines_[batch_size];
    if (!engine) {
      engine::SessionOptions options;
      options.batch_size = batch_size;
      auto made = slt::Antb1Engine::Make(tables_, options);
      if (!made) {
        return std::unexpected(made.error());
      }
      engine = std::move(*made);
    }
    return engine.get();
  }

 private:
  std::vector<slt::TableDef> tables_;
  std::map<int64_t, std::unique_ptr<slt::Antb1Engine>> engines_;
};

// The rows of the table's files as the Parquet library decodes them (the first column of each).
arrow::Result<int64_t> ScanRows(const slt::TableDef& table) {
  int64_t rows = 0;
  for (const auto& file : table.files) {
    ARROW_ASSIGN_OR_RAISE(auto input, arrow::io::ReadableFile::Open(file));
    ARROW_ASSIGN_OR_RAISE(auto reader,
                          parquet::arrow::OpenFile(input, arrow::default_memory_pool()));
    std::shared_ptr<arrow::ChunkedArray> column;
    ARROW_RETURN_NOT_OK(reader->ReadColumn(0, &column));
    rows += column->length();
  }
  return rows;
}

struct Counts {
  int relations = 0;
  int holds = 0;
  int pending = 0;
  int failed = 0;
};

class Reporter {
 public:
  Reporter(const Args& args, std::string command) : args_(args), command_(std::move(command)) {}

  void Holds(std::string_view name) {
    ++counts_.holds;
    std::println("HOLDS {}", name);
  }

  void Pending(std::string_view name, std::string_view message) {
    ++counts_.pending;
    std::println("PENDING {}: {}", name, message);
  }

  // `redacted` is printed with --redact, `message` (and `details`) otherwise.
  void Fail(std::string_view name, std::string_view redacted, std::string_view message,
            std::string_view details = {}) {
    ++counts_.failed;
    std::println("FAIL {}: {}", name, args_.redact ? redacted : message);
    if (!args_.redact && !details.empty()) {
      std::print("{}", details);
    }
    std::println("  repro{}:\n    {} --only {}",
                 args_.redact ? " (unredacted, prints values; run it locally)" : "", command_,
                 name);
  }

  Counts& counts() { return counts_; }

 private:
  const Args& args_;
  std::string command_;
  Counts counts_;
};

std::string DescribeProbes(const Relation& r) {
  std::string out;
  for (std::size_t i = 0; i < r.probes.size(); ++i) {
    out += std::format("  [{}] batch_size={}: {}\n", i, r.probes[i].batch_size, r.probes[i].sql);
  }
  return out;
}

void CheckRowCount(const slt::TableDef& hits, Engines& engines, Reporter& report) {
  const auto scanned = ScanRows(hits);
  if (!scanned.ok()) {
    report.Fail(kRowCountCheck, "the Parquet library cannot read the files",
                "the Parquet library cannot read the files: " + scanned.status().ToString());
    return;
  }
  auto engine = engines.For(kDefaultBatchSize);
  if (!engine) {
    report.Fail(kRowCountCheck, "cannot start antb1", engine.error());
    return;
  }
  const auto answer = (*engine)->Execute("SELECT COUNT(*) FROM \"hits\"");
  if (!answer) {
    report.Fail(kRowCountCheck, std::format("COUNT(*) fails ({} error)", answer.error().kind),
                "COUNT(*) fails: " + answer.error().message);
    return;
  }
  const std::string expected = std::to_string(*scanned);
  const bool single = answer->rows.size() == 1 && answer->rows[0].size() == 1;
  if (!single || answer->rows[0][0] != expected) {
    report.Fail(
        kRowCountCheck,
        "antb1's COUNT(*) differs from the rows the Parquet library decodes (values not "
        "shown)",
        std::format("antb1's COUNT(*) is {}, the Parquet library decodes {} rows",
                    single ? answer->rows[0][0].value_or("NULL") : "not a single value", expected));
    return;
  }
  report.Holds(kRowCountCheck);
}

int Run(const Args& args, std::string_view program) {
  const auto tables = slt::LoadTables(args.tables, fs::path(args.tables).parent_path());
  if (!tables) {
    std::println(stderr, "antb1-data-metamorphic: {}", tables.error());
    return kExitSetup;
  }
  const auto hits = std::ranges::find(*tables, "hits", &slt::TableDef::name);
  if (hits == tables->end()) {
    std::println(stderr, "antb1-data-metamorphic: {} has no table `hits`", args.tables);
    return kExitSetup;
  }
  Engines engines(*tables);
  Reporter report(args, std::format("{} --tables {}", program, args.tables));
  for (const Relation& r : HitsRelations()) {
    if (!args.only.empty() && r.name != args.only) {
      continue;
    }
    ++report.counts().relations;
    std::vector<slt::ExecResult> answers;
    for (const auto& probe : r.probes) {
      auto engine = engines.For(probe.batch_size);
      if (!engine) {
        std::println(stderr, "antb1-data-metamorphic: {}", engine.error());
        return kExitSetup;
      }
      answers.push_back((*engine)->Execute(probe.sql));
    }
    const Verdict v = Evaluate(r, answers, slt::kSupportedFeatures);
    switch (v.kind) {
      case Verdict::Kind::kHolds:
        report.Holds(r.name);
        break;
      case Verdict::Kind::kPending:
        report.Pending(r.name, v.redacted);
        break;
      case Verdict::Kind::kViolated:
      case Verdict::Kind::kBroken:
        report.Fail(r.name, v.redacted, v.message, DescribeProbes(r));
        break;
    }
  }
  if (args.only.empty() || args.only == kRowCountCheck) {
    ++report.counts().relations;
    CheckRowCount(*hits, engines, report);
  }
  const Counts& c = report.counts();
  if (c.relations == 0) {
    std::println(stderr, "antb1-data-metamorphic: no relation named '{}'", args.only);
    return kExitUsage;
  }
  std::println("METAMORPHIC: {} relations={} holds={} pending={} failed={}",
               c.failed == 0 ? "PASS" : "FAIL", c.relations, c.holds, c.pending, c.failed);
  return c.failed == 0 ? 0 : kExitFailed;
}

int Main(std::span<char*> argv) {
  CLI::App app{"antb1-data-metamorphic: metamorphic relations over the ClickBench hits table",
               "antb1-data-metamorphic"};
  Args args;
  app.add_option("--tables", args.tables, "Tables file with a `hits` table")->required();
  app.add_flag("--redact", args.redact, "Never print values, SQL or error texts");
  app.add_option("--only", args.only, "Run only this relation (repro)");
  try {
    app.parse(static_cast<int>(argv.size()), argv.data());
  } catch (const CLI::ParseError& e) {
    return app.exit(e) == 0 ? 0 : kExitUsage;
  }
  return Run(args, argv[0]);
}

}  // namespace
}  // namespace antb1::metamorphic

int main(int argc, char** argv) {
  try {
    return antb1::metamorphic::Main(std::span(argv, static_cast<std::size_t>(argc)));
  } catch (const std::exception& e) {
    std::fputs("antb1-data-metamorphic: internal error: ", stderr);  // C stdio: cannot throw again
    std::fputs(e.what(), stderr);
    std::fputs("\n", stderr);
  } catch (...) {
    std::fputs("antb1-data-metamorphic: internal error\n", stderr);
  }
  return 70;
}
