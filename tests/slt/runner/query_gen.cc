#include "query_gen.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <expected>
#include <format>
#include <limits>
#include <memory>
#include <numeric>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <arrow/api.h>
#include <arrow/io/file.h>
#include <parquet/arrow/reader.h>
#include <parquet/exception.h>
#include <parquet/file_reader.h>
#include <parquet/metadata.h>

#include "antb1/common/int128.h"
#include "antb1/plan/catalog.h"

#include "canonical.h"
#include "supported_features.h"
#include "tables.h"

namespace antb1::slt {
namespace {

// ---- random numbers: splitmix64 (the same generator as tools/fixturegen) ----

constexpr uint64_t kGamma = 0x9E3779B97F4A7C15ULL;

constexpr uint64_t Mix(uint64_t z) {
  z = (z ^ (z >> 30U)) * 0xBF58476D1CE4E5B9ULL;
  z = (z ^ (z >> 27U)) * 0x94D049BB133111EBULL;
  return z ^ (z >> 31U);
}

class Rng {
 public:
  explicit Rng(uint64_t seed) : state_(seed) {}

  uint64_t Next() {
    state_ += kGamma;
    return Mix(state_);
  }
  std::size_t Below(std::size_t n) {
    if (n == 0) {
      return 0;
    }
    const std::size_t r = Next() % n;  // uint64_t and size_t: both 64 bits on every target
    return r;
  }
  bool Percent(unsigned percent) { return Next() % 100U < percent; }
  template <class T>
  const T& Pick(const std::vector<T>& values) {
    return values[Below(values.size())];
  }
  template <class T, std::size_t N>
  const T& Pick(const std::array<T, N>& values) {
    return values[Below(N)];
  }

 private:
  uint64_t state_;
};

// ---- tables ----

constexpr std::size_t kMaxSamples = 12;
constexpr std::size_t kSampleProbes = 32;
constexpr std::size_t kMaxSampleBytes = 40;

std::string ParquetError(const std::string& path, const std::exception& e) {
  return std::format("cannot read Parquet file '{}': {}", path, e.what());
}

std::expected<int64_t, std::string> CountRows(const std::string& path) {
  try {
    return parquet::ParquetFileReader::OpenFile(path)->metadata()->num_rows();
  } catch (const std::exception& e) {
    return std::unexpected(ParquetError(path, e));
  }
}

std::expected<std::shared_ptr<arrow::Table>, std::string> ReadFile(const std::string& path) {
  try {
    auto input = arrow::io::ReadableFile::Open(path);
    if (!input.ok()) {
      return std::unexpected(input.status().ToString());
    }
    auto reader = parquet::arrow::OpenFile(*input, arrow::default_memory_pool());
    if (!reader.ok()) {
      return std::unexpected(reader.status().ToString());
    }
    auto table = (*reader)->ReadTable();
    if (!table.ok()) {
      return std::unexpected(table.status().ToString());
    }
    return (*table)->CombineChunks().ValueOr(*table);
  } catch (const std::exception& e) {
    return std::unexpected(ParquetError(path, e));
  }
}

std::optional<GenColumn> ColumnOf(const arrow::Field& field, bool clickbench) {
  GenColumn c;
  c.name = field.name();
  auto integer = [&c](int64_t lo, int64_t hi) {
    c.kind = ValueKind::kInteger;
    c.min = lo;
    c.max = hi;
  };
  switch (field.type()->id()) {
    case arrow::Type::INT8:
      integer(std::numeric_limits<int8_t>::min(), std::numeric_limits<int8_t>::max());
      break;
    case arrow::Type::UINT8:
      integer(0, std::numeric_limits<uint8_t>::max());
      break;
    case arrow::Type::INT16:
      integer(std::numeric_limits<int16_t>::min(), std::numeric_limits<int16_t>::max());
      break;
    case arrow::Type::UINT16:
      integer(0, std::numeric_limits<uint16_t>::max());
      break;
    case arrow::Type::INT32:
      integer(std::numeric_limits<int32_t>::min(), std::numeric_limits<int32_t>::max());
      break;
    case arrow::Type::UINT32:
      integer(0, std::numeric_limits<uint32_t>::max());
      break;
    case arrow::Type::INT64:
      integer(std::numeric_limits<int64_t>::min(), std::numeric_limits<int64_t>::max());
      break;
    case arrow::Type::DOUBLE:
      c.kind = ValueKind::kDouble;
      break;
    case arrow::Type::BINARY:
    case arrow::Type::STRING:
      c.kind = ValueKind::kVarchar;
      break;
    case arrow::Type::DATE32:
      c.kind = ValueKind::kDate;
      break;
    // FLOAT is DOUBLE on antb1 (results, and WHERE in double precision) but FLOAT on DuckDB, which
    // also compares most literals with it in FLOAT: divergence D11 in docs/sql-subset.md.
    case arrow::Type::FLOAT:
    default:
      return std::nullopt;  // a type neither engine reads the same way: never referenced
  }
  // tables.txt `clickbench`: EventDate (days since 1970-01-01) reads as DATE on both engines.
  if (clickbench && plan::AsciiLower(c.name) == "eventdate" && c.kind == ValueKind::kInteger) {
    c.kind = ValueKind::kDate;
    c.via_override = true;
  }
  return c;
}

std::optional<int64_t> IntegerAt(const arrow::Array& a, int64_t row) {
  switch (a.type_id()) {
    case arrow::Type::INT8:
      return static_cast<const arrow::Int8Array&>(a).Value(row);
    case arrow::Type::UINT8:
      return static_cast<const arrow::UInt8Array&>(a).Value(row);
    case arrow::Type::INT16:
      return static_cast<const arrow::Int16Array&>(a).Value(row);
    case arrow::Type::UINT16:
      return static_cast<const arrow::UInt16Array&>(a).Value(row);
    case arrow::Type::INT32:
      return static_cast<const arrow::Int32Array&>(a).Value(row);
    case arrow::Type::UINT32:
      return static_cast<const arrow::UInt32Array&>(a).Value(row);
    case arrow::Type::INT64:
      return static_cast<const arrow::Int64Array&>(a).Value(row);
    case arrow::Type::DATE32:
      return static_cast<const arrow::Date32Array&>(a).Value(row);
    default:
      return std::nullopt;
  }
}

// The shortest round-trip text of a double in fixed notation ("0.5", "-9007199254740992").
std::string FixedDouble(double value) {
  std::array<char, 512> buf{};
  const auto [end, ec] =
      std::to_chars(buf.data(), buf.data() + buf.size(), value, std::chars_format::fixed);
  return ec == std::errc{} ? std::string(buf.data(), end) : std::string("0");
}

std::optional<std::string> SampleAt(const arrow::Array& a, int64_t row, const GenColumn& c) {
  if (a.IsNull(row)) {
    return std::nullopt;
  }
  switch (c.kind) {
    case ValueKind::kInteger: {
      const auto v = IntegerAt(a, row);
      return v.has_value() ? std::optional(std::to_string(*v)) : std::nullopt;
    }
    case ValueKind::kDate: {
      const auto v = IntegerAt(a, row);
      return v.has_value() ? std::optional(CanonicalDate(static_cast<int32_t>(*v))) : std::nullopt;
    }
    case ValueKind::kDouble: {
      const double v = static_cast<const arrow::DoubleArray&>(a).Value(row);
      return std::isfinite(v) ? std::optional(FixedDouble(v)) : std::nullopt;
    }
    case ValueKind::kVarchar: {
      std::string v(static_cast<const arrow::BinaryArray&>(a).GetView(row));
      // Only text that reads the same as an slt cell: valid UTF-8, no control characters, no
      // edge spaces; both engines then parse the literal to the same bytes.
      if (v.empty() || v.size() > kMaxSampleBytes || SltCell(v) != v) {
        return std::nullopt;
      }
      return v;
    }
  }
  return std::nullopt;
}

void AddSamples(GenColumn& c, const arrow::Array& a) {
  const int64_t rows = a.length();
  const int64_t stride = std::max<int64_t>(1, rows / static_cast<int64_t>(kSampleProbes));
  for (int64_t row = 0; row < rows && c.samples.size() < kMaxSamples; row += stride) {
    auto v = SampleAt(a, row, c);
    if (v.has_value() && !std::ranges::contains(c.samples, *v)) {
      c.samples.push_back(std::move(*v));
    }
  }
}

// ---- query building ----

enum class Shape : std::uint8_t { kAggregates, kColumns, kStar };
enum class Agg : std::uint8_t { kCountStar, kCount, kSum, kAvg, kMin, kMax };

// Row count above which SELECT * gets a LIMIT (keeps results small).
constexpr int64_t kStarMaxRows = 50;

Feature TypeFeature(ValueKind kind) {
  switch (kind) {
    case ValueKind::kInteger:
      return Feature::kIntegerColumns;
    case ValueKind::kDouble:
      return Feature::kDoubleColumns;
    case ValueKind::kVarchar:
      return Feature::kVarcharColumns;
    case ValueKind::kDate:
      return Feature::kDateColumns;
  }
  return Feature::kIntegerColumns;
}

Feature AggFeature(Agg agg) {
  switch (agg) {
    case Agg::kCountStar:
      return Feature::kCountStar;
    case Agg::kCount:
      return Feature::kCountColumn;
    case Agg::kSum:
      return Feature::kSum;
    case Agg::kAvg:
      return Feature::kAvg;
    case Agg::kMin:
      return Feature::kMin;
    case Agg::kMax:
      return Feature::kMax;
  }
  return Feature::kCountStar;
}

std::string_view AggName(Agg agg) {
  switch (agg) {
    case Agg::kCountStar:
    case Agg::kCount:
      return "COUNT";
    case Agg::kSum:
      return "SUM";
    case Agg::kAvg:
      return "AVG";
    case Agg::kMin:
      return "MIN";
    case Agg::kMax:
      return "MAX";
  }
  return "COUNT";
}

bool IsNumeric(ValueKind kind) { return kind == ValueKind::kInteger || kind == ValueKind::kDouble; }

std::string SqlString(std::string_view s) {
  std::string out = "'";
  for (const char c : s) {
    out += c;
    if (c == '\'') {
      out += '\'';
    }
  }
  return out + "'";
}

std::string QuoteIdentifier(std::string_view s) {
  std::string out = "\"";
  for (const char c : s) {
    out += c;
    if (c == '"') {
      out += '"';
    }
  }
  return out + "\"";
}

// Whether both engines convert the decimal text to the same double: the digits form an integer of
// at most 2^53 and the scale is at most 22, so the value is one correctly rounded division of two
// exact doubles (DuckDB's DECIMAL -> DOUBLE cast) and also what strtod returns.
bool ExactDecimalDouble(std::string_view text) {
  uint64_t mantissa = 0;
  std::size_t scale = 0;
  bool after_point = false;
  for (const char c : text) {
    if (c == '-') {
      continue;
    }
    if (c == '.') {
      after_point = true;
      continue;
    }
    const auto digit = static_cast<uint64_t>(c - '0');
    if (mantissa > ((uint64_t{1} << 53U) - digit) / 10U) {
      return false;
    }
    mantissa = (mantissa * 10U) + digit;
    scale += after_point ? 1 : 0;
  }
  return scale <= 22;
}

struct Token {
  enum class Kind : std::uint8_t { kKeyword, kIdentifier, kAlias, kSymbol, kLiteral };
  Kind kind = Kind::kSymbol;
  std::string text;
};

struct Literal {
  std::vector<Token> tokens;  // one token, or DATE + string
  FeatureSet features;
};

constexpr auto kOps = std::to_array<std::string_view>({"=", "<>", "!=", "<", "<=", ">", ">="});

std::string_view Flip(std::string_view op) {
  if (op == "<") {
    return ">";
  }
  if (op == "<=") {
    return ">=";
  }
  if (op == ">") {
    return "<";
  }
  if (op == ">=") {
    return "<=";
  }
  return op;  // = <> != are symmetric
}

class Builder {
 public:
  Builder(const std::vector<GenTable>& tables, Rng& rng, FeatureSet allowed)
      : tables_(tables), rng_(rng), allowed_(allowed) {}

  // A query over the first table (in random order) that admits one under `allowed`.
  std::optional<GeneratedQuery> Build() {
    std::vector<std::size_t> order(tables_.size());
    std::ranges::iota(order, std::size_t{0});
    for (std::size_t i = order.size(); i > 1; --i) {
      std::swap(order[i - 1], order[rng_.Below(i)]);
    }
    for (const std::size_t t : order) {
      if (auto q = TryTable(tables_[t])) {
        return q;
      }
    }
    return std::nullopt;
  }

 private:
  [[nodiscard]] bool Usable(const GenColumn& c, bool by_path) const {
    return (!by_path || !c.via_override) && allowed_.Has(TypeFeature(c.kind));
  }

  [[nodiscard]] bool CanLiteral(ValueKind kind) const {
    switch (kind) {
      case ValueKind::kInteger:
      case ValueKind::kDouble:
        return allowed_.Has(Feature::kIntegerLiteral) || allowed_.Has(Feature::kDecimalLiteral);
      case ValueKind::kVarchar:
        return allowed_.Has(Feature::kStringLiteral);
      case ValueKind::kDate:
        return allowed_.Has(Feature::kDateLiteral) || allowed_.Has(Feature::kStringLiteral);
    }
    return false;
  }

  // By name or by path (as rolled; the other form if the rolled one admits no query).
  std::optional<GeneratedQuery> TryTable(const GenTable& t) {
    const bool can_name = allowed_.Has(Feature::kTableName);
    const bool can_path = allowed_.Has(Feature::kTablePath) && !t.path.empty();
    if (!can_name && !can_path) {
      return std::nullopt;
    }
    const bool by_path = can_path && (!can_name || rng_.Percent(20));
    if (auto q = TryTableAs(t, by_path)) {
      return q;
    }
    const bool other_form_allowed = by_path ? can_name : can_path;
    if (!other_form_allowed) {
      return std::nullopt;
    }
    return TryTableAs(t, !by_path);
  }

  std::optional<GeneratedQuery> TryTableAs(const GenTable& t, bool by_path) {
    std::vector<const GenColumn*> cols;
    std::vector<const GenColumn*> numeric;
    for (const auto& c : t.columns) {
      if (Usable(c, by_path)) {
        cols.push_back(&c);
        if (IsNumeric(c.kind)) {
          numeric.push_back(&c);
        }
      }
    }
    std::vector<Agg> aggs;
    if (allowed_.Has(Feature::kCountStar)) {
      aggs.push_back(Agg::kCountStar);
    }
    for (const Agg agg : {Agg::kCount, Agg::kMin, Agg::kMax}) {
      if (allowed_.Has(AggFeature(agg)) && !cols.empty()) {
        aggs.push_back(agg);
      }
    }
    for (const Agg agg : {Agg::kSum, Agg::kAvg}) {
      if (allowed_.Has(AggFeature(agg)) && !numeric.empty()) {
        aggs.push_back(agg);
      }
    }
    const bool star_ok = allowed_.Has(Feature::kStar) && !t.columns.empty() && !t.other_columns &&
                         cols.size() == t.columns.size() &&
                         (t.rows <= kStarMaxRows || allowed_.Has(Feature::kLimit));
    const std::array<unsigned, 3> weights = {
        aggs.empty() ? 0U : 60U,
        allowed_.Has(Feature::kColumns) && !cols.empty() ? 25U : 0U,
        star_ok ? 15U : 0U,
    };
    const unsigned total = weights[0] + weights[1] + weights[2];
    if (total == 0) {
      return std::nullopt;
    }
    auto roll = static_cast<unsigned>(rng_.Below(total));
    Shape shape = Shape::kStar;
    if (roll < weights[0]) {
      shape = Shape::kAggregates;
    } else if (roll - weights[0] < weights[1]) {
      shape = Shape::kColumns;
    }

    tokens_.clear();
    used_ = FeatureSet{};
    Keyword("SELECT");
    SelectList(shape, t, cols, numeric, aggs);
    Keyword("FROM");
    if (by_path) {
      used_.Add(Feature::kTablePath);
      tokens_.push_back({.kind = Token::Kind::kLiteral, .text = SqlString(t.path)});
    } else {
      used_.Add(Feature::kTableName);
      tokens_.push_back({.kind = Token::Kind::kIdentifier, .text = t.name});
    }
    Where(cols);
    const bool limited = Limit(shape == Shape::kStar && t.rows > kStarMaxRows);
    if (allowed_.Has(Feature::kSemicolon) && rng_.Percent(15)) {
      used_.Add(Feature::kSemicolon);
      Symbol(";");
    }
    GeneratedQuery q;
    q.sql = Render();
    q.table = t.name;
    q.features = used_;
    q.sort = shape == Shape::kAggregates ? SortMode::kNoSort : SortMode::kRowSort;
    q.row_count_only = shape != Shape::kAggregates && limited;
    return q;
  }

  void SelectList(Shape shape, const GenTable& t, const std::vector<const GenColumn*>& cols,
                  const std::vector<const GenColumn*>& numeric, const std::vector<Agg>& aggs) {
    if (shape == Shape::kStar) {
      used_.Add(Feature::kStar);
      for (const auto& c : t.columns) {
        used_.Add(TypeFeature(c.kind));
      }
      Symbol("*");
      return;
    }
    std::size_t items = 1;
    if (allowed_.Has(Feature::kMultipleItems) && rng_.Percent(35)) {
      used_.Add(Feature::kMultipleItems);
      items = 2 + rng_.Below(2);
    }
    for (std::size_t i = 0; i < items; ++i) {
      if (i > 0) {
        Symbol(",");
      }
      if (shape == Shape::kColumns) {
        used_.Add(Feature::kColumns);
        Column(*rng_.Pick(cols));
      } else {
        const Agg agg = rng_.Pick(aggs);
        used_.Add(AggFeature(agg));
        Keyword(AggName(agg));
        Symbol("(");
        if (agg == Agg::kCountStar) {
          Symbol("*");
        } else {
          Column(*rng_.Pick(agg == Agg::kSum || agg == Agg::kAvg ? numeric : cols));
        }
        Symbol(")");
      }
      if (allowed_.Has(Feature::kAlias) && rng_.Percent(15)) {
        used_.Add(Feature::kAlias);
        if (rng_.Percent(50)) {
          Keyword("AS");
        }
        tokens_.push_back({.kind = Token::Kind::kAlias, .text = std::format("a{}", i + 1)});
      }
    }
  }

  void Where(const std::vector<const GenColumn*>& cols) {
    std::vector<const GenColumn*> comparable;
    for (const auto* c : cols) {
      if (CanLiteral(c->kind)) {
        comparable.push_back(c);
      }
    }
    if (!allowed_.Has(Feature::kWhere) || comparable.empty() || !rng_.Percent(45)) {
      return;
    }
    used_.Add(Feature::kWhere);
    std::size_t terms = 1;
    if (allowed_.Has(Feature::kWhereAnd) && rng_.Percent(40)) {
      used_.Add(Feature::kWhereAnd);
      terms = 2 + rng_.Below(2);
    }
    Keyword("WHERE");
    for (std::size_t i = 0; i < terms; ++i) {
      if (i > 0) {
        Keyword("AND");
      }
      const GenColumn& c = *rng_.Pick(comparable);
      const std::string_view op = rng_.Pick(kOps);
      Literal lit = MakeLiteral(c);
      used_.Add(lit.features);
      if (allowed_.Has(Feature::kLiteralFirst) && rng_.Percent(20)) {
        used_.Add(Feature::kLiteralFirst);
        tokens_.insert(tokens_.end(), lit.tokens.begin(), lit.tokens.end());
        Symbol(Flip(op));
        Column(c);
      } else {
        Column(c);
        Symbol(op);
        tokens_.insert(tokens_.end(), lit.tokens.begin(), lit.tokens.end());
      }
    }
  }

  bool Limit(bool required) {
    if (!allowed_.Has(Feature::kLimit) || (!required && !rng_.Percent(20))) {
      return false;
    }
    static constexpr auto kSmall = std::to_array<int>({0, 1, 2, 5, 10});
    static constexpr auto kAny = std::to_array<int>({0, 1, 2, 3, 5, 10, 100, 1000, 100000});
    used_.Add(Feature::kLimit);
    Keyword("LIMIT");
    tokens_.push_back({.kind = Token::Kind::kLiteral,
                       .text = std::to_string(required ? rng_.Pick(kSmall) : rng_.Pick(kAny))});
    return true;
  }

  // An integer literal in the column's range, at its edges or just outside.
  std::string IntegerText(const GenColumn& c) {
    if (!c.samples.empty() && rng_.Percent(50)) {
      return rng_.Pick(c.samples);
    }
    const Int128 lo = c.min;
    const Int128 hi = c.max;
    if (rng_.Percent(60)) {
      const std::array<Int128, 7> edges = {lo, hi, lo - 1, hi + 1, 0, 1, -1};
      return Int128ToString(rng_.Pick(edges));
    }
    const auto span = static_cast<UInt128>(hi - lo) + 1U;
    return Int128ToString(lo + static_cast<Int128>(static_cast<UInt128>(rng_.Next()) % span));
  }

  std::string DoubleText(const GenColumn& c) {
    static constexpr auto kEdges =
        std::to_array<std::string_view>({"0", "0.5", "-1.5", "1000000", "-0.25", "123456789.125"});
    if (!c.samples.empty() && rng_.Percent(60)) {
      const std::string& s = rng_.Pick(c.samples);
      if (ExactDecimalDouble(s)) {
        return s;
      }
    }
    return std::string(rng_.Pick(kEdges));
  }

  Literal MakeLiteral(const GenColumn& c) {
    Literal lit;
    auto single = [&lit](std::string text, Feature feature) {
      lit.tokens.push_back({.kind = Token::Kind::kLiteral, .text = std::move(text)});
      lit.features.Add(feature);
    };
    switch (c.kind) {
      case ValueKind::kInteger:
      case ValueKind::kDouble: {
        std::string text = c.kind == ValueKind::kInteger ? IntegerText(c) : DoubleText(c);
        const bool want_decimal = c.kind == ValueKind::kInteger &&
                                  allowed_.Has(Feature::kDecimalLiteral) &&
                                  (!allowed_.Has(Feature::kIntegerLiteral) || rng_.Percent(20));
        if (want_decimal) {
          static constexpr auto kFractions = std::to_array<std::string_view>({".5", ".25", ".0"});
          text += rng_.Pick(kFractions);
        }
        const bool decimal = text.contains('.');
        if (!allowed_.Has(decimal ? Feature::kDecimalLiteral : Feature::kIntegerLiteral)) {
          text = decimal ? "0" : "0.5";  // the other literal kind is allowed (CanLiteral)
        }
        if (text.starts_with('-') && !allowed_.Has(Feature::kNegativeLiteral)) {
          text = text.contains('.') ? "0.5" : "0";
        }
        if (text.starts_with('-')) {
          lit.features.Add(Feature::kNegativeLiteral);
        }
        const Feature kind =
            text.contains('.') ? Feature::kDecimalLiteral : Feature::kIntegerLiteral;
        single(std::move(text), kind);
        break;
      }
      case ValueKind::kVarchar: {
        static constexpr auto kEdges =
            std::to_array<std::string_view>({"", "a", "zzzz", "~", "O'Brien", "alpha beta", "é"});
        const std::string text = !c.samples.empty() && rng_.Percent(60)
                                     ? rng_.Pick(c.samples)
                                     : std::string(rng_.Pick(kEdges));
        single(SqlString(text), Feature::kStringLiteral);
        break;
      }
      case ValueKind::kDate: {
        static constexpr auto kEdges =
            std::to_array<std::string_view>({"2013-06-30", "2013-07-01", "2013-07-15", "2013-07-31",
                                             "2013-08-01", "1970-01-01", "2100-12-31"});
        const std::string text = !c.samples.empty() && rng_.Percent(60)
                                     ? rng_.Pick(c.samples)
                                     : std::string(rng_.Pick(kEdges));
        const bool date_keyword = allowed_.Has(Feature::kDateLiteral) &&
                                  (!allowed_.Has(Feature::kStringLiteral) || rng_.Percent(70));
        if (date_keyword) {
          lit.tokens.push_back({.kind = Token::Kind::kKeyword, .text = "DATE"});
          single(SqlString(text), Feature::kDateLiteral);
        } else {
          single(SqlString(text), Feature::kStringLiteral);
        }
        break;
      }
    }
    return lit;
  }

  void Keyword(std::string_view k) {
    tokens_.push_back({.kind = Token::Kind::kKeyword, .text = std::string(k)});
  }
  void Symbol(std::string_view s) {
    tokens_.push_back({.kind = Token::Kind::kSymbol, .text = std::string(s)});
  }
  void Column(const GenColumn& c) {
    used_.Add(TypeFeature(c.kind));
    tokens_.push_back({.kind = Token::Kind::kIdentifier, .text = c.name});
  }

  std::string RandomCase(std::string_view text, bool all_lower) {
    std::string out(text);
    for (char& ch : out) {
      const bool lower = all_lower || rng_.Percent(50);
      if (ch >= 'A' && ch <= 'Z' && lower) {
        ch = static_cast<char>(ch - 'A' + 'a');
      } else if (ch >= 'a' && ch <= 'z' && !lower) {
        ch = static_cast<char>(ch - 'a' + 'A');
      }
    }
    return out;
  }

  // Joins the tokens, applying the lexical variants that `allowed` permits.
  std::string Render() {
    const bool keyword_case = allowed_.Has(Feature::kKeywordCase) && rng_.Percent(25);
    const bool identifier_case = allowed_.Has(Feature::kIdentifierCase) && rng_.Percent(20);
    const bool quoted = allowed_.Has(Feature::kQuotedIdentifier) && rng_.Percent(15);
    const bool layout = allowed_.Has(Feature::kLayout) && rng_.Percent(15);
    static constexpr auto kSpaced =
        std::to_array<std::string_view>({" ", "  ", "\n", "\t", "\n  ", " /* c */ ", " -- c\n"});
    static constexpr auto kTight = std::to_array<std::string_view>({"", " ", "\n"});
    std::string sql;
    for (std::size_t i = 0; i < tokens_.size(); ++i) {
      const Token& tok = tokens_[i];
      if (i > 0) {
        const bool spaced = tok.text != "(" && tok.text != ")" && tok.text != "," &&
                            tok.text != ";" && tokens_[i - 1].text != "(";
        std::string_view gap = spaced ? " " : "";
        if (layout) {
          gap = spaced ? rng_.Pick(kSpaced) : rng_.Pick(kTight);
          if (gap != (spaced ? " " : "")) {
            used_.Add(Feature::kLayout);
          }
        }
        sql += gap;
      }
      std::string text = tok.text;
      if (tok.kind == Token::Kind::kKeyword && keyword_case) {
        text = RandomCase(text, rng_.Percent(50));
        if (text != tok.text) {
          used_.Add(Feature::kKeywordCase);
        }
      } else if (tok.kind == Token::Kind::kIdentifier) {
        if (identifier_case) {
          text = RandomCase(text, rng_.Percent(30));
          if (text != tok.text) {
            used_.Add(Feature::kIdentifierCase);
          }
        }
        if (quoted && rng_.Percent(60)) {
          text = QuoteIdentifier(text);
          used_.Add(Feature::kQuotedIdentifier);
        }
      }
      sql += text;
    }
    return sql;
  }

  const std::vector<GenTable>& tables_;
  Rng& rng_;
  FeatureSet allowed_;
  std::vector<Token> tokens_;
  FeatureSet used_;
};

}  // namespace

std::expected<std::vector<GenTable>, std::string> LoadGenTables(
    const std::vector<TableDef>& tables) {
  std::vector<GenTable> out;
  for (const auto& def : tables) {
    GenTable t;
    t.name = def.name;
    if (def.patterns.size() == 1) {
      t.path = def.patterns.front();
    }
    if (def.files.empty()) {
      return std::unexpected(std::format("table '{}' has no files", def.name));
    }
    for (const auto& f : def.files) {
      auto rows = CountRows(f);
      if (!rows) {
        return std::unexpected(rows.error());
      }
      t.rows += *rows;
    }
    auto data = ReadFile(def.files.front());
    if (!data) {
      return std::unexpected(data.error());
    }
    const arrow::Table& table = **data;
    for (int i = 0; i < table.num_columns(); ++i) {
      auto column = ColumnOf(*table.schema()->field(i), def.clickbench);
      if (!column.has_value()) {
        t.other_columns = true;
        continue;
      }
      if (table.column(i)->num_chunks() == 1) {
        AddSamples(*column, *table.column(i)->chunk(0));
      }
      t.columns.push_back(std::move(*column));
    }
    out.push_back(std::move(t));
  }
  return out;
}

std::expected<QueryGenerator, std::string> QueryGenerator::Make(std::vector<GenTable> tables,
                                                                uint64_t seed,
                                                                GeneratorOptions options) {
  if (tables.empty()) {
    return std::unexpected("no tables to generate queries for");
  }
  if (options.target_percent > 100) {
    return std::unexpected("the target grammar share must be at most 100 percent");
  }
  Rng rng(seed);
  if (!Builder(tables, rng, options.supported).Build().has_value()) {
    return std::unexpected(
        std::format("no query can be built from the supported features ({}) over these tables",
                    options.supported.Names()));
  }
  return QueryGenerator(std::move(tables), seed, options);
}

GeneratedQuery QueryGenerator::Generate(uint64_t index) const {
  Rng rng(Mix(seed_ + Mix(index + kGamma)));
  const bool target = rng.Percent(options_.target_percent);
  const FeatureSet allowed = target ? FeatureSet::All() : options_.supported;
  // Make() proved that the supported set admits a query over these tables; All() is a superset.
  GeneratedQuery q = Builder(tables_, rng, allowed).Build().value_or(GeneratedQuery{});
  q.index = index;
  q.target_sample = target;
  return q;
}

}  // namespace antb1::slt
