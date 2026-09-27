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

#include "antb1/common/utf8.h"
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

namespace {

// A canonical cell as a SQL literal; std::nullopt for text the harness cannot write (not UTF-8, or
// with a NUL byte).
std::optional<std::string> CellSql(const std::optional<std::string>& cell, char letter) {
  if (!cell.has_value()) {
    return "NULL";
  }
  const std::string& text = *cell;
  if (letter == 'I') {
    return std::format("CAST('{}' AS HUGEINT)", text);
  }
  if (letter == 'R') {
    return std::format("CAST('{}' AS DOUBLE)", text);  // also nan, inf and -inf
  }
  std::string out = "'";
  for (std::size_t i = 0; i < text.size();) {
    const bool ascii = static_cast<unsigned char>(text[i]) < 0x80;
    const std::size_t length = ascii ? 1 : Utf8SequenceLength(text, i);
    if (length == 0 || text[i] == '\0') {
      return std::nullopt;
    }
    for (std::size_t k = 0; k < length; ++k) {
      out += text[i + k];
      if (text[i + k] == '\'') {
        out += '\'';
      }
    }
    i += length;
  }
  return out + "'";
}

// `column` (of the augmented query) as compared with a literal of class `letter`: text classes
// compare as DuckDB prints them, which is the canonical text (dates too).
std::string Operand(std::string_view column, char letter) {
  return letter == 'T' ? std::format("CAST({} AS VARCHAR)", column) : std::string(column);
}

// The rows of DuckDB's augmented query whose ORDER BY keys equal `key` (a row of it) and whose I
// and T cells equal those of one of `wanted` (antb1 rows): everything needed to match those rows
// against a run of ties that is too long to fetch whole. std::nullopt if a value cannot be written
// as SQL.
std::optional<std::string> RunMembersSql(const OrderedQuery& q, std::string_view letters,
                                         std::string_view key_letters, const Cells& key,
                                         std::size_t width,
                                         const std::vector<const Cells*>& wanted) {
  std::string columns;
  for (std::size_t c = 0; c < width; ++c) {
    columns += std::format("{}c{}", c == 0 ? "" : ", ", c);
  }
  for (std::size_t k = 0; k < key_letters.size(); ++k) {
    columns += std::format("{}k{}", columns.empty() ? "" : ", ", k);
  }
  std::string where;
  for (std::size_t k = 0; k < key_letters.size(); ++k) {
    const std::string column = std::format("__antb1_a.k{}", k);
    const auto literal = CellSql(key[width + k], key_letters[k]);
    if (!literal.has_value()) {
      return std::nullopt;
    }
    std::string condition;
    if (!key[width + k].has_value()) {
      condition = column + " IS NULL";
    } else if (key_letters[k] == 'R') {
      condition = std::format(
          "(isnan({0}) AND isnan({1}) OR {0} = {1} OR abs({0} - {1}) <= {2} + {3} * "
          "greatest(abs({0}), abs({1})))",
          column, *literal, kAbsTolerance, kDefaultRelTolerance);
    } else {
      condition = std::format("{} = {}", Operand(column, key_letters[k]), *literal);
    }
    where += (where.empty() ? "" : " AND ") + condition;
  }
  std::vector<std::size_t> exact;
  for (std::size_t c = 0; c < width; ++c) {
    if (letters[c] != 'R') {
      exact.push_back(c);
    }
  }
  if (!exact.empty()) {
    std::string values;
    for (const Cells* row : wanted) {
      std::string tuple;
      for (const std::size_t c : exact) {
        const auto literal = CellSql((*row)[c], letters[c]);
        if (!literal.has_value()) {
          return std::nullopt;
        }
        tuple += (tuple.empty() ? "" : ", ") + *literal;
      }
      values += std::format("{}({})", values.empty() ? "" : ", ", tuple);
    }
    std::string names;
    std::string match;
    for (std::size_t n = 0; n < exact.size(); ++n) {
      names += std::format("{}v{}", n == 0 ? "" : ", ", n);
      match += std::format("{}{} IS NOT DISTINCT FROM __antb1_v.v{}", n == 0 ? "" : " AND ",
                           Operand(std::format("__antb1_a.c{}", exact[n]), letters[exact[n]]), n);
    }
    where += std::format("{}EXISTS (SELECT 1 FROM (VALUES {}) AS __antb1_v({}) WHERE {})",
                         where.empty() ? "" : " AND ", values, names, match);
  }
  return std::format("SELECT * FROM ({}) AS __antb1_a({}){}{}", q.augmented_sql, columns,
                     where.empty() ? "" : " WHERE ", where);
}

Discrepancy Mismatch(const ResultSet& oracle, const ResultSet& antb1, std::string_view letters,
                     std::size_t row) {
  return Discrepancy{
      .what =
          "antb1 returns a row that is not one of DuckDB's rows with the ORDER BY keys of its "
          "position (rows with equal keys may come in any order)",
      .mismatch = true,
      .types = std::string(letters),
      .expected = RenderBlock(oracle, SortMode::kNoSort, 0),
      .actual = RenderBlock(antb1, SortMode::kNoSort, 0),
      .first_row = row};
}

// Rows of `pool` by their run and their exact cells; a matched row leaves its bucket, so a long run
// of equal rows costs no rescans.
class Buckets {
 public:
  Buckets(const std::vector<Cells>& pool, std::string_view letters)
      : pool_(pool), letters_(letters) {}

  void Add(std::size_t run, std::size_t row) { buckets_[Key(run, pool_[row])].push_back(row); }
  // Takes an unused row of `run` equal to `row` (R cells within the tolerance).
  bool Take(std::size_t run, const Cells& row) {
    const auto bucket = buckets_.find(Key(run, row));
    if (bucket == buckets_.end()) {
      return false;
    }
    std::vector<std::size_t>& unused = bucket->second;
    for (std::size_t k = unused.size(); k > 0; --k) {
      if (SameCells(pool_[unused[k - 1]], row, 0, letters_)) {
        unused[k - 1] = unused.back();
        unused.pop_back();
        return true;
      }
    }
    return false;
  }

 private:
  [[nodiscard]] std::string Key(std::size_t run, const Cells& row) const {
    return std::to_string(run) + '\x01' + ExactText(row, letters_);
  }

  const std::vector<Cells>& pool_;
  std::string_view letters_;
  std::unordered_map<std::string, std::vector<std::size_t>> buckets_;
};

}  // namespace

std::optional<Discrepancy> CompareOrdered(
    const ResultSet& oracle, const ResultSet& antb1, const OrderedQuery& q,
    const std::function<ExecResult(const std::string&)>& run) {
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
  const auto check_shape = [&](const ResultSet& result) -> std::optional<Discrepancy> {
    const std::string all_letters = Letters(result);
    if (all_letters.size() != width + q.keys || !all_letters.starts_with(letters)) {
      return Discrepancy{
          .what = std::format("the query with its ORDER BY keys selected has the columns {}, not "
                              "the query's {} and {} key(s) (a harness bug)",
                              all_letters, letters, q.keys)};
    }
    return std::nullopt;
  };

  // The oracle's ranked rows, up to the end of the run of ties that holds the window's last row,
  // or up to q.max_rows (then that run, the last one fetched, may go on: `open_run`).
  ExecResult ranked;
  std::string key_letters;
  bool open_run = false;
  std::optional<int64_t> limit;
  if (window_end < std::numeric_limits<int64_t>::max()) {
    limit = std::min(SaturatingAdd(window_end, std::max(window_end, kMinExtraRows)),
                     std::max(q.max_rows, window_end + 1));
  }
  while (true) {
    ranked = run(WithLimit(q, limit));
    if (!ranked.has_value()) {
      return ErrorDiscrepancy("DuckDB fails on the query with its ORDER BY keys selected",
                              ranked.error());
    }
    if (auto d = check_shape(*ranked)) {
      return d;
    }
    key_letters = Letters(*ranked).substr(width);
    const std::vector<Cells>& rows = ranked->rows;
    if (!limit.has_value() || std::cmp_less(rows.size(), *limit) || window_end == 0 ||
        !SameCells(rows[static_cast<std::size_t>(*limit - 1)],
                   rows[static_cast<std::size_t>(window_end - 1)], width, key_letters)) {
      break;
    }
    if (*limit >= q.max_rows) {
      open_run = true;
      break;
    }
    limit = std::min(SaturatingMul(*limit, 4), std::max(q.max_rows, window_end + 1));
  }

  const std::vector<Cells>& rows = ranked->rows;
  std::vector<std::size_t> run_of(rows.size(), 0);
  Buckets buckets(rows, letters);
  for (std::size_t j = 0; j < rows.size(); ++j) {
    if (j > 0) {
      run_of[j] = run_of[j - 1] + (SameCells(rows[j - 1], rows[j], width, key_letters) ? 0U : 1U);
    }
    buckets.Add(run_of[j], j);
  }
  const std::size_t last_run = rows.empty() ? 0 : run_of.back();
  std::vector<std::size_t> in_open_run;  // antb1 rows at ranks of the open run
  for (std::size_t i = 0; i < antb1.rows.size(); ++i) {
    const auto rank = static_cast<std::size_t>(SaturatingAdd(q.offset, static_cast<int64_t>(i)));
    if (open_run && rank < rows.size() && run_of[rank] == last_run) {
      in_open_run.push_back(i);
      continue;
    }
    if (rank >= rows.size() || !buckets.Take(run_of[rank], antb1.rows[i])) {
      return Mismatch(oracle, antb1, letters, i);
    }
  }
  if (in_open_run.empty()) {
    return std::nullopt;
  }

  // The run is too long to fetch: fetch only its rows that can match antb1's rows there.
  std::vector<const Cells*> wanted;
  wanted.reserve(in_open_run.size());
  for (const std::size_t i : in_open_run) {
    wanted.push_back(&antb1.rows[i]);
  }
  const auto sql = RunMembersSql(q, letters, key_letters, rows.back(), width, wanted);
  if (!sql.has_value()) {
    return Discrepancy{
        .what = std::format("cannot check the rows at ranks tied beyond the first {} rows: a value "
                            "is not valid UTF-8 text (a harness limitation)",
                            rows.size())};
  }
  const ExecResult members = run(*sql);
  if (!members.has_value()) {
    return ErrorDiscrepancy("DuckDB fails on the query for the rows of a long run of ties",
                            members.error());
  }
  if (auto d = check_shape(*members)) {
    return d;
  }
  Buckets candidates(members->rows, letters);
  for (std::size_t j = 0; j < members->rows.size(); ++j) {
    candidates.Add(0, j);
  }
  for (const std::size_t i : in_open_run) {
    if (!candidates.Take(0, antb1.rows[i])) {
      return Mismatch(oracle, antb1, letters, i);
    }
  }
  return std::nullopt;
}

std::optional<Discrepancy> CompareQueryAnswers(std::string_view sql, const ResultSet& oracle_answer,
                                               const ResultSet& antb1_answer, Engine& oracle,
                                               bool rows, SortMode sort) {
  if (const auto ordered = MakeOrderedQuery(sql)) {
    return CompareOrdered(oracle_answer, antb1_answer, *ordered,
                          [&](const std::string& query) { return oracle.Execute(query); });
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
