#include "ordered_compare.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <format>
#include <functional>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#include "antb1/sql/ast.h"
#include "antb1/sql/parser.h"
#include "antb1/sql/unparse.h"

#include "canonical.h"
#include "engine.h"
#include "result_diff.h"
#include "unordered_limit.h"

namespace antb1::slt {
namespace {

// The first row count the augmented query is run with, beyond the window's end.
constexpr int64_t kMinExtraRows = 1024;

std::string Lower(std::string_view text) {
  std::string out(text);
  for (char& c : out) {
    if (c >= 'A' && c <= 'Z') {
      c = static_cast<char>(c - 'A' + 'a');
    }
  }
  return out;
}

int64_t SaturatingAdd(int64_t a, int64_t b) {
  int64_t sum = 0;
  return __builtin_add_overflow(a, b, &sum) ? std::numeric_limits<int64_t>::max() : sum;
}

int64_t SaturatingMul(int64_t a, int64_t b) {
  int64_t product = 0;
  return __builtin_mul_overflow(a, b, &product) ? std::numeric_limits<int64_t>::max() : product;
}

using Cells = std::vector<std::optional<std::string>>;

// Cells equal as CompareBlocks sees them: exactly, or an R cell within the tolerance.
bool SameCell(const std::optional<std::string>& a, const std::optional<std::string>& b,
              char letter) {
  if (a == b) {
    return true;
  }
  return letter == 'R' && a.has_value() && b.has_value() &&
         !CompareBlocks({*a}, {*b}, "R", SortMode::kNoSort, kDefaultRelTolerance).has_value();
}

// Cells [begin, begin + letters.size()) of two rows.
bool SameCells(const Cells& a, const Cells& b, std::size_t begin, std::string_view letters) {
  for (std::size_t c = 0; c < letters.size(); ++c) {
    if (!SameCell(a[begin + c], b[begin + c], letters[c])) {
      return false;
    }
  }
  return true;
}

// The first `letters.size()` cells of a row that are not R, as one string: rows with the same
// cells within the R tolerance have the same text.
std::string ExactText(const Cells& row, std::string_view letters) {
  std::string text;
  for (std::size_t c = 0; c < letters.size(); ++c) {
    if (letters[c] == 'R') {
      continue;
    }
    if (const auto& cell = row[c]; cell.has_value()) {
      text += '\x02';
      text += *cell;
    }
    text += '\x00';
  }
  return text;
}

bool SameAnswer(const ResultSet& a, const ResultSet& b, std::string_view letters) {
  if (a.rows.size() != b.rows.size()) {
    return false;
  }
  for (std::size_t i = 0; i < a.rows.size(); ++i) {
    if (a.rows[i].size() != letters.size() || b.rows[i].size() != letters.size() ||
        !SameCells(a.rows[i], b.rows[i], 0, letters)) {
      return false;
    }
  }
  return true;
}

}  // namespace

std::optional<OrderedQuery> MakeOrderedQuery(std::string_view sql) {
  auto stmt = sql::Parse(sql);
  if (!stmt.has_value() || stmt->order_by.empty()) {
    return std::nullopt;
  }
  std::vector<sql::SelectItem> extra;
  for (std::size_t i = 0; i < stmt->order_by.size(); ++i) {
    sql::SelectExpr expr = stmt->order_by[i].expr;
    if (const auto* ref = std::get_if<sql::ColumnRef>(&expr)) {
      // A select alias comes first (the last item with it), as in the binder and DuckDB.
      const std::string wanted = Lower(ref->name);
      for (std::size_t n = stmt->items.size(); n > 0; --n) {
        const auto& alias = stmt->items[n - 1].alias;
        if (alias.has_value() && Lower(*alias) == wanted) {
          expr = stmt->items[n - 1].expr;
          break;
        }
      }
    }
    extra.push_back(
        sql::SelectItem{.expr = std::move(expr), .alias = std::format("__antb1_key{}", i)});
  }
  OrderedQuery q{.augmented_sql = {},
                 .keys = extra.size(),
                 .limit = stmt->limit,
                 .offset = stmt->offset.value_or(0)};
  stmt->limit.reset();
  stmt->offset.reset();
  if (stmt->star) {
    stmt->star = false;
    stmt->items = std::move(extra);
    const std::string text = sql::ToSql(*stmt);
    q.augmented_sql = "SELECT *, " + text.substr(std::string_view("SELECT ").size());
  } else {
    stmt->items.insert(stmt->items.end(), extra.begin(), extra.end());
    q.augmented_sql = sql::ToSql(*stmt);
  }
  return q;
}

std::string WithLimit(const OrderedQuery& q, std::optional<int64_t> rows) {
  return rows.has_value() ? std::format("{} LIMIT {}", q.augmented_sql, *rows) : q.augmented_sql;
}

std::optional<Discrepancy> CompareOrdered(
    const ResultSet& oracle, const ResultSet& antb1, const OrderedQuery& q,
    const std::function<ExecResult(std::optional<int64_t>)>& run) {
  const std::string letters = Letters(oracle);
  if (letters != Letters(antb1) || oracle.type_names != antb1.type_names ||
      oracle.rows.size() != antb1.rows.size()) {
    // Different column types or row counts.
    return CompareAnswers(oracle, antb1, SortMode::kNoSort, /*row_count_only=*/true);
  }
  if (SameAnswer(oracle, antb1, letters)) {
    return std::nullopt;
  }
  const std::size_t width = letters.size();
  const int64_t window_end =
      q.limit.has_value() ? SaturatingAdd(q.offset, *q.limit) : std::numeric_limits<int64_t>::max();

  // The oracle's ranked rows, up to the end of the run of ties that holds the window's last row.
  ExecResult ranked;
  std::string key_letters;
  std::optional<int64_t> limit;
  if (window_end < std::numeric_limits<int64_t>::max()) {
    limit = SaturatingAdd(window_end, std::max(window_end, kMinExtraRows));
  }
  while (true) {
    ranked = run(limit);
    if (!ranked.has_value()) {
      return ErrorDiscrepancy("DuckDB fails on the query with its ORDER BY keys selected",
                              ranked.error());
    }
    const std::string all_letters = Letters(*ranked);
    if (all_letters.size() != width + q.keys || !all_letters.starts_with(letters)) {
      return Discrepancy{
          .what = std::format("the query with its ORDER BY keys selected has the columns {}, not "
                              "the query's {} and {} key(s) (a harness bug)",
                              all_letters, letters, q.keys)};
    }
    key_letters = all_letters.substr(width);
    const std::vector<Cells>& rows = ranked->rows;
    if (!limit.has_value() || std::cmp_less(rows.size(), *limit) || window_end == 0 ||
        !SameCells(rows[static_cast<std::size_t>(*limit - 1)],
                   rows[static_cast<std::size_t>(window_end - 1)], width, key_letters)) {
      break;
    }
    limit = *limit == std::numeric_limits<int64_t>::max() ? std::nullopt
                                                          : std::optional(SaturatingMul(*limit, 4));
  }

  // Runs of equal keys, and the rows of each run by their exact cells.
  const std::vector<Cells>& rows = ranked->rows;
  std::vector<std::size_t> run_of(rows.size(), 0);
  std::unordered_map<std::string, std::vector<std::size_t>> by_run_and_cells;
  for (std::size_t j = 0; j < rows.size(); ++j) {
    if (j > 0) {
      run_of[j] = run_of[j - 1] + (SameCells(rows[j - 1], rows[j], width, key_letters) ? 0U : 1U);
    }
    by_run_and_cells[std::to_string(run_of[j]) + '\x01' + ExactText(rows[j], letters)].push_back(j);
  }

  for (std::size_t i = 0; i < antb1.rows.size(); ++i) {
    const auto rank = static_cast<std::size_t>(SaturatingAdd(q.offset, static_cast<int64_t>(i)));
    std::optional<std::size_t> match;
    if (rank < rows.size()) {
      // A bucket holds the unused rows of one run with the same exact cells: a matched row leaves
      // it, so a long run of equal rows costs no rescans.
      const auto bucket = by_run_and_cells.find(std::to_string(run_of[rank]) + '\x01' +
                                                ExactText(antb1.rows[i], letters));
      if (bucket != by_run_and_cells.end()) {
        std::vector<std::size_t>& unused = bucket->second;
        for (std::size_t k = unused.size(); k > 0 && !match; --k) {
          if (SameCells(rows[unused[k - 1]], antb1.rows[i], 0, letters)) {
            match = unused[k - 1];
            unused[k - 1] = unused.back();
            unused.pop_back();
          }
        }
      }
    }
    if (!match) {
      return Discrepancy{
          .what =
              "antb1 returns a row that is not one of DuckDB's rows with the ORDER BY keys of its "
              "position (rows with equal keys may come in any order)",
          .mismatch = true,
          .types = letters,
          .expected = RenderBlock(oracle, SortMode::kNoSort, 0),
          .actual = RenderBlock(antb1, SortMode::kNoSort, 0),
          .first_row = i};
    }
  }
  return std::nullopt;
}

std::optional<Discrepancy> CompareQueryAnswers(std::string_view sql, const ResultSet& oracle_answer,
                                               const ResultSet& antb1_answer, Engine& oracle,
                                               bool rows, SortMode sort) {
  if (const auto ordered = MakeOrderedQuery(sql)) {
    return CompareOrdered(oracle_answer, antb1_answer, *ordered, [&](std::optional<int64_t> n) {
      return oracle.Execute(WithLimit(*ordered, n));
    });
  }
  if (rows) {
    if (const auto unlimited = UnlimitedSql(sql)) {
      return CompareLimited(oracle_answer, antb1_answer,
                            [&] { return oracle.Execute(*unlimited); });
    }
  }
  return CompareAnswers(oracle_answer, antb1_answer, sort, /*row_count_only=*/false);
}

}  // namespace antb1::slt
