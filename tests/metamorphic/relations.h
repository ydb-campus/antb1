#pragma once

#include <cstdint>
#include <functional>
#include <iosfwd>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "engine.h"
#include "supported_features.h"

// Metamorphic relations (label metamorphic): groups of antb1 queries whose answers must relate in a
// known way whatever the data, e.g. COUNT(*) over a split table equals the sum over its parts. They
// need no oracle, so they also cover what DuckDB cannot check: batch sizes, file layouts, variants.
//
// A relation declares every SQL feature its queries use. If they are all in kSupportedFeatures
// (tests/slt/supported_features.h) the relation is active and must hold. Otherwise it is pending:
// at least one of its queries must still be answered Unsupported (and the test reports it as
// skipped); once antb1 answers them all, the test fails until the PR declares the features, which
// activates the relation. Slice PRs add relations here (see AllRelations() in relations.cc).

namespace antb1::metamorphic {

inline constexpr int64_t kDefaultBatchSize = int64_t{64} * 1024;

struct Probe {
  std::string sql;                         // ${FIXTURES} is replaced by the fixtures directory
  int64_t batch_size = kDefaultBatchSize;  // engine::SessionOptions::batch_size of its session
};

// Returns a description of the violation, or std::nullopt if the answers (one per probe, in order)
// satisfy the relation.
using Check = std::function<std::optional<std::string>(std::span<const slt::ResultSet> answers)>;

struct Relation {
  std::string name;          // [A-Za-z0-9_]+: the gtest parameter name
  slt::FeatureSet features;  // every feature the probes use
  std::vector<Probe> probes;
  Check check;
};

// gtest prints a failing parameter by its name (instead of its bytes).
void PrintTo(const Relation& r, std::ostream* os);

// Every answer renders to the same rowsorted block (R columns within the slt tolerance).
Check AllEqual();
// Single-value answers: a0 == a1 + ... + an (integers, exact; NULL counts as no value).
Check FirstEqualsSumOfRest();
// Single-value answers: a0 == the minimum / maximum of the non-NULL a1..an (NULL if all are).
Check FirstEqualsMinOfRest();
Check FirstEqualsMaxOfRest();
// a0 is a single integer n; every other answer has exactly n rows.
Check RowCountsEqualFirst();
// a0 is a single integer n; answer i (i >= 1) has min(limits[i - 1], n) rows.
Check RowCountsAreMinOf(std::vector<int64_t> limits);

struct Verdict {
  enum class Kind : std::uint8_t {
    kHolds,     // active, every query answered, the check passed
    kViolated,  // active, every query answered, the check failed
    kPending,   // needs undeclared features; at least one query answered Unsupported
    kBroken,    // a query failed (other than a pending Unsupported), or a pending relation is
                // fully answered: its features must be declared
  };
  Kind kind = Kind::kHolds;
  std::string message;
  std::string redacted;  // the message without values or error texts (the data tests print it)
};

// Judges the answers (one per probe) of relation `r` given the declared `supported` features.
Verdict Evaluate(const Relation& r, const std::vector<slt::ExecResult>& answers,
                 slt::FeatureSet supported);

// The tables of tests/slt/tables.txt with their FROM '<path>' form (relative to ${FIXTURES}).
struct TablePath {
  std::string_view table;
  std::string_view path;
};
std::span<const TablePath> TablePaths();

std::vector<Relation> AllRelations();

}  // namespace antb1::metamorphic
