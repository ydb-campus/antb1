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
    case arrow::Type::DECIMAL128: {
      const auto& decimal = static_cast<const arrow::Decimal128Type&>(*field.type());
      c.kind = ValueKind::kDecimal;
      c.precision = decimal.precision();
      c.scale = decimal.scale();
      break;
    }
    // A FLOAT column's results are DOUBLE on antb1 but FLOAT on DuckDB: divergence D11 in
    // docs/sql-subset.md. (WHERE compares alike; tests/slt/cases/where/float.slt covers it.)
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

// An unscaled DECIMAL value ("-1234") as a literal of `scale` fraction digits ("-12.34", "0.05"):
// always with a digit before the point, which every SQL dialect reads.
std::string DecimalLiteralText(std::string unscaled, int scale) {
  const bool negative = unscaled.starts_with('-');
  if (negative) {
    unscaled.erase(0, 1);
  }
  const auto digits = static_cast<std::size_t>(scale);
  if (unscaled.size() <= digits) {
    unscaled.insert(0, digits + 1 - unscaled.size(), '0');
  }
  if (digits > 0) {
    unscaled.insert(unscaled.size() - digits, ".");
  }
  return negative ? "-" + unscaled : unscaled;
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
    case ValueKind::kDecimal: {
      const arrow::Decimal128 v(static_cast<const arrow::Decimal128Array&>(a).GetValue(row));
      return DecimalLiteralText(v.ToIntegerString(), c.scale);
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

// Widens the column's data range by the non-NULL values of `a` (an integer array).
void AddRange(GenColumn& c, const arrow::Array& a) {
  const auto add = [&c](int64_t v) {
    c.data_min = std::min(c.data_min.value_or(v), v);
    c.data_max = std::max(c.data_max.value_or(v), v);
  };
  const auto each = [&]<class ArrayType> {
    const auto& values = static_cast<const ArrayType&>(a);
    for (int64_t i = 0; i < values.length(); ++i) {
      if (values.IsValid(i)) {
        add(static_cast<int64_t>(values.Value(i)));
      }
    }
  };
  switch (a.type_id()) {
    case arrow::Type::INT8:
      each.template operator()<arrow::Int8Array>();
      break;
    case arrow::Type::UINT8:
      each.template operator()<arrow::UInt8Array>();
      break;
    case arrow::Type::INT16:
      each.template operator()<arrow::Int16Array>();
      break;
    case arrow::Type::UINT16:
      each.template operator()<arrow::UInt16Array>();
      break;
    case arrow::Type::INT32:
      each.template operator()<arrow::Int32Array>();
      break;
    case arrow::Type::UINT32:
      each.template operator()<arrow::UInt32Array>();
      break;
    case arrow::Type::INT64:
      each.template operator()<arrow::Int64Array>();
      break;
    case arrow::Type::DECIMAL128: {
      const auto& values = static_cast<const arrow::Decimal128Array&>(a);
      for (int64_t i = 0; i < values.length(); ++i) {
        if (values.IsValid(i)) {
          const arrow::Decimal128 v(values.GetValue(i));
          const auto bits =
              (static_cast<UInt128>(static_cast<uint64_t>(v.high_bits())) << 64U) | v.low_bits();
          const auto value = static_cast<Int128>(bits);
          // A file may hold the 128-bit minimum, beyond any width: the largest Int128 stands in,
          // which no SUM or arithmetic bound accepts.
          Int128 magnitude = value;
          if (value < 0 && __builtin_sub_overflow(Int128{0}, value, &magnitude)) {
            magnitude = kInt128Max;
          }
          c.abs_max = std::max(c.abs_max.value_or(0), magnitude);
        }
      }
      break;
    }
    default:
      break;
  }
}

// 10^n - 1, the largest unscaled value of n digits (n <= 38).
Int128 MaxOfDigits(int digits) {
  Int128 power = 1;
  for (int i = 0; i < digits; ++i) {
    power *= 10;
  }
  return power - 1;
}

// A literal operand of DECIMAL arithmetic as DuckDB types it: an INTEGER counts as DECIMAL(10,0), a
// decimal literal is DECIMAL(digits, fraction digits) (ADR 0021 rules 3 and 4).
struct LiteralOperand {
  std::string text;
  int width = 0;
  int scale = 0;
  Int128 unscaled = 0;

  static LiteralOperand Integer(int64_t k) {
    return {.text = std::to_string(k), .width = 10, .scale = 0, .unscaled = k};
  }
};

// Whether SUM of a DECIMAL column over every row stays within DECIMAL(38,s): an overflow fails
// antb1, while DuckDB returns up to 39 digits (a divergence).
bool Summable(const GenColumn& c, int64_t rows) {
  if (c.kind != ValueKind::kDecimal) {
    return false;
  }
  const Int128 bound = c.abs_max.value_or(0);
  return bound == 0 || bound <= MaxOfDigits(38) / std::max<int64_t>(rows, 1);
}

// ---- query building ----

enum class Shape : std::uint8_t { kAggregates, kColumns, kStar };
enum class Agg : std::uint8_t { kCountStar, kCount, kSum, kAvg, kMin, kMax, kCountDistinct };

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
    case ValueKind::kDecimal:
      return Feature::kDecimalColumns;
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
    case Agg::kCountDistinct:
      return Feature::kCountDistinct;
  }
  return Feature::kCountStar;
}

std::string_view AggName(Agg agg) {
  switch (agg) {
    case Agg::kCountStar:
    case Agg::kCount:
    case Agg::kCountDistinct:
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
        return allowed_.Has(Feature::kDateLiteral) || allowed_.Has(Feature::kCastDate) ||
               allowed_.Has(Feature::kStringLiteral);
      case ValueKind::kDecimal:  // an integer always fits DuckDB's common type (DecimalText)
        return allowed_.Has(Feature::kIntegerLiteral);
    }
    return false;
  }

  // Appends the date `quoted` (a string literal) typed DATE: DATE 'd' for `spelling` 0 and 1,
  // CAST('d' AS DATE) for 2 and 'd'::DATE for 3, among the spellings `allowed_` permits (the
  // keyword without kCastDate). Returns whether it wrote a cast.
  bool TypedDate(std::vector<Token>& out, std::string quoted, std::size_t spelling) const {
    if (!allowed_.Has(Feature::kCastDate) ||
        (spelling < 2 && allowed_.Has(Feature::kDateLiteral))) {
      out.push_back({.kind = Token::Kind::kKeyword, .text = "DATE"});
      out.push_back({.kind = Token::Kind::kLiteral, .text = std::move(quoted)});
      return false;
    }
    if (spelling % 2 == 0) {
      out.push_back({.kind = Token::Kind::kKeyword, .text = "CAST"});
      out.push_back({.kind = Token::Kind::kSymbol, .text = "("});
      out.push_back({.kind = Token::Kind::kLiteral, .text = std::move(quoted)});
      out.push_back({.kind = Token::Kind::kKeyword, .text = "AS"});
      out.push_back({.kind = Token::Kind::kKeyword, .text = "DATE"});
      out.push_back({.kind = Token::Kind::kSymbol, .text = ")"});
    } else {
      out.push_back({.kind = Token::Kind::kLiteral, .text = std::move(quoted)});
      out.push_back({.kind = Token::Kind::kSymbol, .text = "::"});
      out.push_back({.kind = Token::Kind::kKeyword, .text = "DATE"});
    }
    return true;
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
    rows_ = t.rows;
    std::vector<const GenColumn*> cols;
    std::vector<const GenColumn*> numeric;
    for (const auto& c : t.columns) {
      if (Usable(c, by_path)) {
        cols.push_back(&c);
        // SUM and AVG arguments: numbers, and DECIMAL columns whose sum fits DECIMAL(38,s).
        if (IsNumeric(c.kind) || Summable(c, t.rows)) {
          numeric.push_back(&c);
        }
      }
    }
    std::vector<Agg> aggs;
    if (allowed_.Has(Feature::kCountStar)) {
      aggs.push_back(Agg::kCountStar);
    }
    for (const Agg agg : {Agg::kCount, Agg::kMin, Agg::kMax, Agg::kCountDistinct}) {
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

    // GROUP BY: 1 or 2 keys of the aggregate shape (no DOUBLE key: both engines group -0.0 with
    // 0.0, but which of the two a group prints is the first seen, an order the engines need not
    // share).
    std::vector<const GenColumn*> keys;
    if (shape == Shape::kAggregates && allowed_.Has(Feature::kGroupBy)) {
      std::vector<const GenColumn*> candidates;
      for (const auto* c : cols) {
        if (c->kind != ValueKind::kDouble) {
          candidates.push_back(c);
        }
      }
      if (!candidates.empty() && rng_.Percent(35)) {
        const std::size_t count = 1 + rng_.Below(2);
        for (std::size_t i = 0; i < count; ++i) {
          const GenColumn* key = rng_.Pick(candidates);
          if (std::ranges::find(keys, key) == keys.end()) {
            keys.push_back(key);
          }
        }
      }
    }
    std::vector<const GenColumn*> selected_keys;
    if (allowed_.Has(Feature::kColumns)) {
      for (const auto* key : keys) {
        if (rng_.Percent(70)) {
          selected_keys.push_back(key);
        }
      }
    }

    comparable_.clear();
    for (const auto* c : cols) {
      if (CanLiteral(c->kind)) {
        comparable_.push_back(c);
      }
    }
    tokens_.clear();
    used_ = FeatureSet{};
    order_aliases_.clear();
    aggregate_aliases_.clear();
    order_positions_.clear();
    key_positions_.clear();
    constant_positions_.clear();
    aggregate_emitted_ = false;
    Keyword("SELECT");
    SelectList(shape, t, cols, numeric, aggs, selected_keys);
    Keyword("FROM");
    if (by_path) {
      used_.Add(Feature::kTablePath);
      tokens_.push_back({.kind = Token::Kind::kLiteral, .text = SqlString(t.path)});
    } else {
      used_.Add(Feature::kTableName);
      tokens_.push_back({.kind = Token::Kind::kIdentifier, .text = t.name});
    }
    Where();
    // GROUP BY a constant's position alone still groups: no row over no input rows.
    const bool constant_group =
        shape == Shape::kAggregates && keys.empty() && allowed_.Has(Feature::kGroupBy) &&
        allowed_.Has(Feature::kPosition) && !constant_positions_.empty() && rng_.Percent(30);
    for (std::size_t i = 0; i < keys.size(); ++i) {
      if (i == 0) {
        used_.Add(Feature::kGroupBy);
        Keyword("GROUP");
        Keyword("BY");
      } else {
        Symbol(",");
      }
      const auto selected =
          std::ranges::find_if(key_positions_, [&](const auto& kp) { return kp.first == keys[i]; });
      if (selected != key_positions_.end() && allowed_.Has(Feature::kPosition) &&
          rng_.Percent(40)) {
        Position(selected->second);
      } else {
        Column(*keys[i]);
      }
    }
    if (constant_group) {
      used_.Add(Feature::kGroupBy);
      Keyword("GROUP");
      Keyword("BY");
      Position(rng_.Pick(constant_positions_));
    }
    const bool having = Having(shape, t, cols, numeric, aggs, keys);
    const bool ordered = OrderBy(shape, cols, numeric, aggs, keys);
    const bool limited = Limit(shape == Shape::kStar && t.rows > kStarMaxRows);
    const bool offset = Offset(limited);
    if (allowed_.Has(Feature::kSemicolon) && rng_.Percent(15)) {
      used_.Add(Feature::kSemicolon);
      Symbol(";");
    }
    GeneratedQuery q;
    q.sql = Render();
    q.table = t.name;
    q.features = used_;
    // A select list of constants only (no aggregate emitted) has a row per table row, unless HAVING
    // makes the query aggregate.
    const bool rows = shape != Shape::kAggregates || !keys.empty() || constant_group ||
                      (!aggregate_emitted_ && !having);
    q.sort = rows && !ordered ? SortMode::kRowSort : SortMode::kNoSort;
    q.unordered_limit = rows && !ordered && (limited || offset);
    return q;
  }

  // `keys`: GROUP BY keys to select too, before or after the aggregates.
  void SelectList(Shape shape, const GenTable& t, const std::vector<const GenColumn*>& cols,
                  const std::vector<const GenColumn*>& numeric, const std::vector<Agg>& aggs,
                  const std::vector<const GenColumn*>& keys) {
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
    std::size_t emitted = 0;
    const auto emit_keys = [&] {
      for (const auto* key : keys) {
        if (emitted++ > 0) {
          Symbol(",");
        }
        used_.Add(Feature::kColumns);
        Column(*key);
        key_positions_.emplace_back(key, emitted);
        order_positions_.push_back(emitted);
      }
    };
    const bool keys_first = rng_.Percent(60);
    if (keys_first) {
      emit_keys();
    }
    for (std::size_t i = 0; i < items; ++i) {
      if (emitted++ > 0) {
        Symbol(",");
      }
      bool orderable = true;
      std::optional<std::pair<Agg, const GenColumn*>> call;
      if (allowed_.Has(Feature::kConstant) && rng_.Percent(10)) {
        // A constant item; a decimal is DECIMAL(digits, fraction digits) (ADR 0021 rule 3).
        static constexpr auto kConstants = std::to_array<std::string_view>(
            {"1", "-7", "42", "3000000000", "'k'", "'it''s'", "DATE '2020-01-02'"});
        static constexpr auto kDecimalConstants =
            std::to_array<std::string_view>({"2.5", "-0.25", "007.50", ".125"});
        used_.Add(Feature::kConstant);
        // One roll picks the constant as Pick did (the roll modulo the size) and the spelling of
        // the date, so that every seed keeps generating the queries it did before kCastDate.
        const std::size_t roll = rng_.Below(4 * kConstants.size());
        std::string_view constant = kConstants[roll % kConstants.size()];
        if (allowed_.Has(Feature::kDecimalLiteral) && rng_.Percent(20)) {
          constant = rng_.Pick(kDecimalConstants);
          used_.Add(Feature::kDecimalLiteral);
        }
        if (constant.starts_with("DATE ")) {
          if (TypedDate(tokens_, std::string(constant.substr(5)), roll / kConstants.size())) {
            used_.Add(Feature::kCastDate);
          }
        } else if (constant.starts_with('-')) {
          Symbol("-");
          tokens_.push_back(
              {.kind = Token::Kind::kLiteral, .text = std::string(constant.substr(1))});
        } else {
          tokens_.push_back({.kind = Token::Kind::kLiteral, .text = std::string(constant)});
        }
        constant_positions_.push_back(emitted);
      } else if (shape == Shape::kColumns) {
        const GenColumn& c = *rng_.Pick(cols);
        std::optional<bool> exact;
        if (rng_.Percent(8)) {
          exact = Case(c);
        } else if (rng_.Percent(8) && Temporal(c, /*number=*/false)) {
          exact = true;
        } else if (c.kind == ValueKind::kVarchar && rng_.Percent(20)) {
          if (StringFunction(c, rng_.Percent(50)).has_value()) {
            exact = true;
          }
        } else if (rng_.Percent(20)) {
          exact = Arithmetic(c);
        }
        if (exact.has_value()) {
          orderable = *exact;
        } else {
          used_.Add(Feature::kColumns);
          Column(c);
        }
      } else {
        const Agg agg = rng_.Pick(aggs);
        const GenColumn* arg = agg == Agg::kCountStar ? nullptr : ArgumentOf(agg, cols, numeric);
        orderable = Orderable(agg, arg);
        const bool arithmetic = arg != nullptr && rng_.Percent(20);
        const std::optional<bool> exact = Aggregate(agg, arg, arithmetic);
        if (exact.has_value() && !*exact) {
          orderable = false;
        }
        aggregate_emitted_ = true;
        // A string or timestamp function changes the argument's values and type: HAVING has no
        // literals for them.
        if (!arg_retyped_) {
          call.emplace(agg, arg);
        }
      }
      if (allowed_.Has(Feature::kAlias) && rng_.Percent(15)) {
        used_.Add(Feature::kAlias);
        if (rng_.Percent(50)) {
          Keyword("AS");
        }
        std::string alias = std::format("a{}", i + 1);
        if (orderable) {
          order_aliases_.push_back(alias);
          if (call.has_value()) {
            aggregate_aliases_.push_back(AggregateAlias{.alias = alias, .call = *call});
          }
        }
        tokens_.push_back({.kind = Token::Kind::kAlias, .text = std::move(alias)});
      }
      if (orderable) {
        order_positions_.push_back(emitted);
      }
    }
    if (!keys_first) {
      emit_keys();
    }
  }

  // A select item's aggregate argument: a number for SUM and AVG, now and then a DATE for AVG
  // (a TIMESTAMP, as in DuckDB), any column for the others.
  const GenColumn* ArgumentOf(Agg agg, const std::vector<const GenColumn*>& cols,
                              const std::vector<const GenColumn*>& numeric) {
    if (agg == Agg::kAvg && allowed_.Has(Feature::kTimestamps) && rng_.Percent(15)) {
      std::vector<const GenColumn*> dates;
      for (const GenColumn* c : cols) {
        if (c->kind == ValueKind::kDate) {
          dates.push_back(c);
        }
      }
      if (!dates.empty()) {
        used_.Add(Feature::kTimestamps);
        return rng_.Pick(dates);
      }
    }
    return rng_.Pick(agg == Agg::kSum || agg == Agg::kAvg ? numeric : cols);
  }

  // A position in the select list (1-based) as a GROUP BY or ORDER BY item.
  void Position(std::size_t position) {
    used_.Add(Feature::kPosition);
    tokens_.push_back({.kind = Token::Kind::kLiteral, .text = std::to_string(position)});
  }

  // `arithmetic`: the argument is an arithmetic expression of the column, when one fits; returns
  // whether its values are exact then (std::nullopt: the plain column).
  std::optional<bool> Aggregate(Agg agg, const GenColumn* arg, bool arithmetic = false) {
    used_.Add(AggFeature(agg));
    Keyword(AggName(agg));
    Symbol("(");
    std::optional<bool> exact;
    arg_retyped_ = false;
    if (arg == nullptr) {
      Symbol("*");
    } else {
      if (agg == Agg::kCountDistinct) {
        Keyword("DISTINCT");
      }
      if (arithmetic && rng_.Percent(30)) {
        exact = Case(*arg);
      }
      // SUM takes only EXTRACT's BIGINT; AVG a TIMESTAMP too.
      if (arithmetic && !exact.has_value() && rng_.Percent(25) &&
          Temporal(*arg, /*number=*/agg == Agg::kSum)) {
        exact = true;
        arg_retyped_ = true;
      }
      if (arithmetic && !exact.has_value()) {
        if (arg->kind == ValueKind::kVarchar) {
          const std::optional<ValueKind> kind = StringFunction(*arg, rng_.Percent(50));
          exact = kind.has_value() ? std::optional(true) : std::nullopt;
          arg_retyped_ = exact.has_value();
        } else {
          exact = Arithmetic(*arg, /*aggregated=*/true,
                             /*summed=*/agg == Agg::kSum || agg == Agg::kAvg);
        }
      }
      if (!exact.has_value()) {
        Column(*arg);
      }
    }
    Symbol(")");
    return exact;
  }

  // Whether the aggregate's values compare exactly in both engines (an I or T result): a sort key
  // with R values could order near-equal values differently.
  static bool Orderable(Agg agg, const GenColumn* arg) {
    switch (agg) {
      case Agg::kCountStar:
      case Agg::kCount:
      case Agg::kCountDistinct:
        return true;
      case Agg::kSum:  // an integer SUM is HUGEINT, a DECIMAL one DECIMAL(38,s)
        return arg != nullptr &&
               (arg->kind == ValueKind::kInteger || arg->kind == ValueKind::kDecimal);
      case Agg::kAvg:
        return false;
      case Agg::kMin:
      case Agg::kMax:
        return arg != nullptr && arg->kind != ValueKind::kDouble;
    }
    return false;
  }

  // ORDER BY 1 to 3 items: select aliases, and columns (a projection: any column; GROUP BY: its
  // keys) or aggregates with I or T values (an aggregate query). Returns whether it wrote one.
  bool OrderBy(Shape shape, const std::vector<const GenColumn*>& cols,
               const std::vector<const GenColumn*>& numeric, const std::vector<Agg>& aggs,
               const std::vector<const GenColumn*>& keys) {
    if (!allowed_.Has(Feature::kOrderBy) || !rng_.Percent(35)) {
      return false;
    }
    const bool aggregate = shape == Shape::kAggregates;
    const std::vector<const GenColumn*>& columns = aggregate ? keys : cols;
    std::vector<std::pair<Agg, const GenColumn*>> calls;
    if (aggregate) {
      for (const Agg agg : aggs) {
        if (agg == Agg::kCountStar) {
          calls.emplace_back(agg, nullptr);
          continue;
        }
        const GenColumn* arg = rng_.Pick(agg == Agg::kSum || agg == Agg::kAvg ? numeric : cols);
        if (Orderable(agg, arg)) {
          calls.emplace_back(agg, arg);
        }
      }
    }
    const std::vector<std::size_t> positions =
        allowed_.Has(Feature::kPosition) ? order_positions_ : std::vector<std::size_t>{};
    if (columns.empty() && calls.empty() && order_aliases_.empty() && positions.empty()) {
      return false;
    }
    used_.Add(Feature::kOrderBy);
    Keyword("ORDER");
    Keyword("BY");
    const std::size_t items = 1 + rng_.Below(3);
    for (std::size_t i = 0; i < items; ++i) {
      if (i > 0) {
        Symbol(",");
      }
      const std::size_t pick =
          rng_.Below(columns.size() + calls.size() + order_aliases_.size() + positions.size());
      if (pick < columns.size()) {
        Column(*columns[pick]);
      } else if (pick < columns.size() + calls.size()) {
        const auto& [agg, arg] = calls[pick - columns.size()];
        Aggregate(agg, arg);
      } else if (pick < columns.size() + calls.size() + order_aliases_.size()) {
        tokens_.push_back({.kind = Token::Kind::kAlias,
                           .text = order_aliases_[pick - columns.size() - calls.size()]});
      } else {
        Position(positions[pick - columns.size() - calls.size() - order_aliases_.size()]);
      }
      const std::size_t direction = rng_.Below(10);
      if (direction < 3) {
        Keyword("DESC");
      } else if (direction < 5) {
        Keyword("ASC");
      }
      if (allowed_.Has(Feature::kNullsOrder) && rng_.Percent(20)) {
        used_.Add(Feature::kNullsOrder);
        Keyword("NULLS");
        Keyword(rng_.Percent(50) ? "FIRST" : "LAST");
      }
    }
    return true;
  }

  // The values an aggregate with I or T values takes, as a column for MakeLiteral: a count is
  // between 0 and the row count, an integer SUM gets its argument's literals, MIN and MAX are their
  // argument. Over a DECIMAL the literals take 38 digits without spare ones: SUM is DECIMAL(38,s),
  // and MIN or MAX of an arithmetic argument can be wider than the column.
  static GenColumn ValuesOf(const GenTable& t, Agg agg, const GenColumn* arg) {
    switch (agg) {
      case Agg::kCountStar:
      case Agg::kCount:
      case Agg::kCountDistinct:
        return GenColumn{.name = {}, .kind = ValueKind::kInteger, .min = 0, .max = t.rows};
      case Agg::kSum:
      case Agg::kAvg:
      case Agg::kMin:
      case Agg::kMax:
        break;
    }
    GenColumn values = *arg;
    if (values.kind == ValueKind::kDecimal) {
      values.precision = 38;  // SUM is DECIMAL(38,s); an argument keeps the column's scale
    }
    return values;
  }

  // HAVING 1 or 2 conditions of an aggregate query (AND): a GROUP BY key, an aggregate call with I
  // or T values or the select alias of one, compared with a literal of its type (or [NOT] LIKE, or
  // [NOT] IN, as in WHERE). Returns whether it wrote one.
  bool Having(Shape shape, const GenTable& t, const std::vector<const GenColumn*>& cols,
              const std::vector<const GenColumn*>& numeric, const std::vector<Agg>& aggs,
              const std::vector<const GenColumn*>& keys) {
    if (shape != Shape::kAggregates || !allowed_.Has(Feature::kHaving) || !rng_.Percent(25)) {
      return false;
    }
    struct Operand {
      const GenColumn* key = nullptr;
      std::optional<std::pair<Agg, const GenColumn*>> call;
      std::string alias;
      GenColumn values;
    };
    std::vector<Operand> operands;
    for (const GenColumn* key : keys) {
      if (CanLiteral(key->kind)) {
        operands.push_back(Operand{.key = key, .call = {}, .alias = {}, .values = *key});
      }
    }
    for (const Agg agg : aggs) {
      const GenColumn* arg = agg == Agg::kCountStar
                                 ? nullptr
                                 : rng_.Pick(agg == Agg::kSum || agg == Agg::kAvg ? numeric : cols);
      GenColumn values = ValuesOf(t, agg, arg);
      if (Orderable(agg, arg) && CanLiteral(values.kind)) {
        operands.push_back(Operand{
            .key = nullptr, .call = std::pair(agg, arg), .alias = {}, .values = std::move(values)});
      }
    }
    for (const AggregateAlias& alias : aggregate_aliases_) {
      GenColumn values = ValuesOf(t, alias.call.first, alias.call.second);
      if (CanLiteral(values.kind)) {
        operands.push_back(
            Operand{.key = nullptr, .call = {}, .alias = alias.alias, .values = std::move(values)});
      }
    }
    if (operands.empty()) {
      return false;
    }
    used_.Add(Feature::kHaving);
    Keyword("HAVING");
    const auto leaf = [&] {
      const Operand& operand = operands[rng_.Below(operands.size())];
      const auto write_operand = [&] {
        if (operand.key != nullptr) {
          Column(*operand.key);
        } else if (operand.call.has_value()) {
          Aggregate(operand.call->first, operand.call->second);
        } else {
          tokens_.push_back({.kind = Token::Kind::kAlias, .text = operand.alias});
        }
      };
      if (operand.values.kind == ValueKind::kVarchar && allowed_.Has(Feature::kLike) &&
          rng_.Percent(25)) {
        write_operand();
        Like(operand.values);
        return;
      }
      if (allowed_.Has(Feature::kIn) && rng_.Percent(15)) {
        write_operand();
        In(operand.values);
        return;
      }
      const std::string_view op = rng_.Pick(kOps);
      Literal lit = MakeLiteral(operand.values);
      used_.Add(lit.features);
      if (allowed_.Has(Feature::kLiteralFirst) && rng_.Percent(20)) {
        used_.Add(Feature::kLiteralFirst);
        tokens_.insert(tokens_.end(), lit.tokens.begin(), lit.tokens.end());
        Symbol(Flip(op));
        write_operand();
      } else {
        write_operand();
        Symbol(op);
        tokens_.insert(tokens_.end(), lit.tokens.begin(), lit.tokens.end());
      }
    };
    const std::size_t terms = 1 + rng_.Below(2);
    for (std::size_t i = 0; i < terms; ++i) {
      if (i > 0) {
        Keyword("AND");
      }
      Compound(leaf);
    }
    return true;
  }

  // A condition of `leaf`s: now and then a parenthesized OR/AND mix of two or three of them, or
  // NOT of one (kBooleanExpressions), else one leaf.
  template <class Leaf>
  void Compound(const Leaf& leaf) {
    if (!allowed_.Has(Feature::kBooleanExpressions) || !rng_.Percent(25)) {
      leaf();
      return;
    }
    used_.Add(Feature::kBooleanExpressions);
    const auto maybe_not = [&] {
      if (rng_.Percent(20)) {
        Keyword("NOT");
        Symbol("(");
        leaf();
        Symbol(")");
      } else {
        leaf();
      }
    };
    if (rng_.Percent(30)) {
      Keyword("NOT");
      Symbol("(");
      leaf();
      Symbol(")");
      return;
    }
    Symbol("(");
    const std::size_t n = 2 + rng_.Below(2);
    for (std::size_t j = 0; j < n; ++j) {
      if (j > 0) {
        Keyword(rng_.Percent(60) ? "OR" : "AND");
      }
      maybe_not();
    }
    Symbol(")");
  }

  // OFFSET m, after (or before) the LIMIT. Returns whether it wrote one.
  bool Offset(bool limited) {
    if (!allowed_.Has(Feature::kOffset) || !rng_.Percent(limited ? 30 : 10)) {
      return false;
    }
    static constexpr auto kOffsets = std::to_array<int>({0, 1, 2, 5, 100});
    used_.Add(Feature::kOffset);
    std::vector<Token> offset;
    offset.push_back({.kind = Token::Kind::kKeyword, .text = "OFFSET"});
    offset.push_back({.kind = Token::Kind::kLiteral, .text = std::to_string(rng_.Pick(kOffsets))});
    // Before the LIMIT (the parser takes either order) now and then.
    const auto at = limited && rng_.Percent(20) ? tokens_.end() - 2 : tokens_.end();
    tokens_.insert(at, offset.begin(), offset.end());
    return true;
  }

  void Where() {
    if (!allowed_.Has(Feature::kWhere) || comparable_.empty() || !rng_.Percent(45)) {
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
      Condition();
    }
  }

  // A condition over the table: WHERE's forms, or a compound of them.
  void Condition() {
    Compound([this] { WhereLeaf(); });
  }

  // column <op> literal (in either order, with arithmetic or strlen on the column now and then),
  // [NOT] LIKE or [NOT] IN over a column comparable_ holds.
  void WhereLeaf() {
    {
      const GenColumn& c = *rng_.Pick(comparable_);
      if (c.kind == ValueKind::kVarchar && allowed_.Has(Feature::kLike) && rng_.Percent(30)) {
        Column(c);
        Like(c);
        return;
      }
      if (allowed_.Has(Feature::kIn) && rng_.Percent(15)) {
        Column(c);
        In(c);
        return;
      }
      // One draw picks the operator (roll % 7, as Pick did) and, with kBetween, now and then a
      // [NOT] BETWEEN instead of the plain comparison below (roll / 7), so that every seed keeps
      // generating the queries it did before kBetween, apart from those.
      const std::size_t op_roll = rng_.Below(kOps.size() * 10);
      const std::string_view op = kOps[op_roll % kOps.size()];
      const std::size_t between = op_roll / kOps.size();  // 1: BETWEEN, 2: NOT BETWEEN
      if (rng_.Percent(6) && TimestampComparison(c, op)) {
        return;
      }
      if (rng_.Percent(6) && allowed_.Has(Feature::kIntegerLiteral) &&
          Temporal(c, /*number=*/true)) {
        used_.Add(Feature::kIntegerLiteral);
        Symbol(op);
        tokens_.push_back({.kind = Token::Kind::kLiteral, .text = std::to_string(rng_.Below(60))});
        return;
      }
      if (c.kind == ValueKind::kVarchar && rng_.Percent(10) &&
          allowed_.Has(Feature::kIntegerLiteral) && StringFunction(c, true).has_value()) {
        used_.Add(Feature::kIntegerLiteral);
        Symbol(op);
        tokens_.push_back({.kind = Token::Kind::kLiteral, .text = std::to_string(rng_.Below(25))});
        return;
      }
      if (IsNumeric(c.kind) && rng_.Percent(15)) {
        if (const std::optional<bool> exact = Arithmetic(c)) {
          // An exact result takes the column's literals; a DOUBLE one those of a DOUBLE column.
          Literal lit = MakeLiteral(*exact ? c : GenColumn{.kind = ValueKind::kDouble});
          used_.Add(lit.features);
          Symbol(op);
          tokens_.insert(tokens_.end(), lit.tokens.begin(), lit.tokens.end());
          return;
        }
      }
      Literal lit = MakeLiteral(c);
      used_.Add(lit.features);
      const bool literal_first = allowed_.Has(Feature::kLiteralFirst) && rng_.Percent(20);
      if (allowed_.Has(Feature::kBetween) && (between == 1 || between == 2)) {
        Between(c, lit, /*negated=*/between == 2);
        return;
      }
      if (literal_first) {
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

  // c [NOT] BETWEEN lit AND high. The upper bound comes from the literal without another draw: an
  // integer plus 5, any other literal itself (a single-value range). Not plus 5 for a DECIMAL of
  // more than 36 digits: one more integer digit could leave DuckDB's 38-digit common type
  // (DecimalText).
  void Between(const GenColumn& c, const Literal& lit, bool negated) {
    used_.Add(Feature::kBetween);
    Column(c);
    if (negated) {
      Keyword("NOT");
    }
    Keyword("BETWEEN");
    tokens_.insert(tokens_.end(), lit.tokens.begin(), lit.tokens.end());
    Keyword("AND");
    std::vector<Token> high = lit.tokens;
    if (high.size() == 1 && high[0].kind == Token::Kind::kLiteral &&
        (c.kind != ValueKind::kDecimal || c.precision <= 36)) {
      const std::string& text = high[0].text;
      int64_t value = 0;
      const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
      if (error == std::errc{} && end == text.data() + text.size() && value < 1'000'000'000) {
        high[0].text = std::to_string(value + 5);
      }
    }
    tokens_.insert(tokens_.end(), high.begin(), high.end());
  }

  // [NOT] LIKE 'pattern' after an operand with the values of `c`: a part of a sample value between
  // %s, a prefix or a suffix, with _ now and then, or an edge pattern. Only the ASCII bytes of
  // samples are used, so the SQL text stays valid UTF-8.
  void Like(const GenColumn& c) {
    used_.Add(Feature::kLike);
    std::string sample;
    if (!c.samples.empty()) {
      for (const char ch : rng_.Pick(c.samples)) {
        if (static_cast<unsigned char>(ch) >= 0x20 && static_cast<unsigned char>(ch) < 0x7F &&
            ch != '%' && ch != '_') {
          sample += ch;
        }
      }
    }
    std::string pattern;
    if (sample.empty() || rng_.Percent(15)) {
      static constexpr auto kEdges = std::to_array<std::string_view>(
          {"%", "", "_", "%_%", "%a%", "a%", "%e", "__%", "%\\%", "%\xC3\xA9%", "%.%", "%/%"});
      pattern = std::string(rng_.Pick(kEdges));
    } else {
      const std::size_t begin = rng_.Below(sample.size());
      const std::size_t length = 1 + rng_.Below(std::min<std::size_t>(6, sample.size() - begin));
      std::string part = sample.substr(begin, length);
      if (rng_.Percent(25)) {
        part[rng_.Below(part.size())] = '_';
      }
      switch (rng_.Below(4)) {
        case 0:
          pattern = "%" + part + "%";
          break;
        case 1:
          pattern = sample.substr(0, length) + "%";
          break;
        case 2:
          pattern = "%" + sample.substr(sample.size() - length);
          break;
        default:
          pattern = rng_.Percent(50) ? sample : "%" + part + "%" + part + "%";
          break;
      }
    }
    if (rng_.Percent(30)) {
      Keyword("NOT");
    }
    Keyword("LIKE");
    tokens_.push_back({.kind = Token::Kind::kLiteral, .text = SqlString(pattern)});
  }

  // [NOT] IN (1 to 4 literals of the type of `c`, as comparisons get them) after an operand.
  void In(const GenColumn& c) {
    used_.Add(Feature::kIn);
    if (rng_.Percent(30)) {
      Keyword("NOT");
    }
    Keyword("IN");
    Symbol("(");
    for (std::size_t n = 1 + rng_.Below(4), i = 0; i < n; ++i) {
      if (i > 0) {
        Symbol(",");
      }
      Literal lit = MakeLiteral(c);
      used_.Add(lit.features);
      tokens_.insert(tokens_.end(), lit.tokens.begin(), lit.tokens.end());
    }
    Symbol(")");
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

  // A literal for a DECIMAL(p,s) column: a sample value, the type's extremes, 0 and +-1, an
  // integer part, and, for p <= 36 only, one more integer digit (out of range) or one more fraction
  // digit (between two values). DuckDB compares in one DECIMAL of max(integer digits) + max(scale)
  // digits, capped at 38: within the cap it is exact, beyond it a column value fails the cast
  // (divergence D13). With p <= 36, two literals with a spare digit each stay within 38 digits
  // (an IN list shares one type), and an integer literal's type (INTEGER, BIGINT, HUGEINT) only
  // shortens the integer digits the cap leaves to 38 - s >= p - s.
  std::string DecimalText(const GenColumn& c) {
    const auto int_digits = static_cast<std::size_t>(c.precision - c.scale);
    const auto scale = static_cast<std::size_t>(c.scale);
    const bool spare = c.precision <= 36;
    const std::string fraction = scale > 0 ? "." + std::string(scale, '9') : "";
    const std::string max = (int_digits > 0 ? std::string(int_digits, '9') : "0") + fraction;
    const std::string sample = c.samples.empty() ? std::string("0") : rng_.Pick(c.samples);
    std::vector<std::string> choices = {sample, sample, sample, max, "-" + max, "0"};
    if (int_digits > 0) {
      choices.emplace_back("1");
      choices.emplace_back("-1");
      choices.push_back(sample.substr(0, sample.find('.')));  // its integer part
    }
    if (spare) {
      const std::string outside = "1" + std::string(int_digits, '0');
      choices.push_back(outside);
      choices.push_back("-" + outside);
      choices.push_back(sample + (scale > 0 ? "5" : ".5"));
    }
    return rng_.Pick(choices);
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
      case ValueKind::kDecimal: {
        std::string text = DecimalText(c);
        if (text.starts_with('-') && !allowed_.Has(Feature::kNegativeLiteral)) {
          text.erase(0, 1);
        }
        if (text.contains('.') && !allowed_.Has(Feature::kDecimalLiteral)) {
          text.resize(text.find('.'));  // the integer part (CanLiteral: integers are allowed)
        }
        if (text == "-0") {
          text = "0";
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
        // A typed date 70% of the time: the roll modulo 100 is the old Percent(70) roll, and the
        // rest picks the spelling (see the constants in SelectList).
        const bool typed = allowed_.Has(Feature::kDateLiteral) || allowed_.Has(Feature::kCastDate);
        const std::size_t roll =
            typed && allowed_.Has(Feature::kStringLiteral) ? rng_.Below(400) : 0;
        if (typed && roll % 100 < 70) {
          lit.features.Add(TypedDate(lit.tokens, SqlString(text), roll / 100)
                               ? Feature::kCastDate
                               : Feature::kDateLiteral);
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

  // toDateTime(c) <op> TIMESTAMP '...' for an integer column of seconds whose values times 1000
  // fit its type, the literal a second inside the column's data range and in years 0001 to 9999
  // (what a TIMESTAMP literal spells). Returns whether it wrote one.
  bool TimestampComparison(const GenColumn& c, std::string_view op) {
    // 0001-01-01 00:00:00: year 0 prints as 0001-01-01 (BC), which a literal does not spell.
    constexpr int64_t kFirst = -62'135'596'800;
    constexpr int64_t kLast = 253'402'300'799;  // 9999-12-31 23:59:59
    if (!allowed_.Has(Feature::kTimestamps) || c.kind != ValueKind::kInteger ||
        !c.data_min.has_value() || !c.data_max.has_value() || *c.data_max > c.max / 1000 ||
        *c.data_min < c.min / 1000) {
      return false;
    }
    const int64_t low = std::max(*c.data_min, kFirst);
    const int64_t high = std::min(*c.data_max, kLast);
    if (low > high) {
      return false;
    }
    const int64_t seconds =
        low + static_cast<int64_t>(rng_.Below(static_cast<uint64_t>(high - low) + 1));
    used_.Add(Feature::kTimestamps);
    Keyword("toDateTime");
    Symbol("(");
    Column(c);
    Symbol(")");
    Symbol(op);
    Keyword("TIMESTAMP");
    tokens_.push_back({.kind = Token::Kind::kLiteral,
                       .text = SqlString(CanonicalTimestamp(seconds * 1'000'000))});
    return true;
  }

  // toDateTime(c) of an integer column (seconds) whose values times 1000 fit its type (both
  // engines fail on the overflow), or a DATE column; as is (TIMESTAMP), under EXTRACT (BIGINT) or
  // under date_trunc (TIMESTAMP). `number`: only EXTRACT. Returns whether it wrote one.
  bool Temporal(const GenColumn& c, bool number) {
    if (!allowed_.Has(Feature::kTimestamps)) {
      return false;
    }
    const bool date = c.kind == ValueKind::kDate;
    const bool seconds = c.kind == ValueKind::kInteger && c.data_min.has_value() &&
                         c.data_max.has_value() && *c.data_max <= c.max / 1000 &&
                         *c.data_min >= c.min / 1000;
    if (!date && !seconds) {
      return false;
    }
    used_.Add(Feature::kTimestamps);
    const auto source = [&] {
      if (date) {
        Column(c);
        return;
      }
      Keyword("toDateTime");
      Symbol("(");
      Column(c);
      Symbol(")");
    };
    const bool trunc = !number && allowed_.Has(Feature::kStringLiteral) && rng_.Percent(40);
    if (number || (!trunc && (date || rng_.Percent(50)))) {
      // BIGINT fields only (epoch is a DOUBLE), in a few of DuckDB's spellings.
      static constexpr auto kFields = std::to_array<std::string_view>(
          {"year", "month", "day", "hour", "minute", "second", "quarter", "week", "isoyear", "dow",
           "isodow", "doy", "millisecond", "decade", "century", "millennium", "yrs", "mins"});
      Keyword("EXTRACT");
      Symbol("(");
      Keyword(rng_.Pick(kFields));
      Keyword("FROM");
      source();
      Symbol(")");
      return true;
    }
    if (trunc) {
      static constexpr auto kUnits = std::to_array<std::string_view>(
          {"year", "quarter", "month", "week", "day", "hour", "minute", "second", "millisecond",
           "decade", "century", "millennium", "isoyear", "dow", "epoch", "Hours"});
      used_.Add(Feature::kStringLiteral);
      Keyword("date_trunc");
      Symbol("(");
      tokens_.push_back({.kind = Token::Kind::kLiteral, .text = SqlString(rng_.Pick(kUnits))});
      Symbol(",");
      source();
      Symbol(")");
      return true;
    }
    source();
    return true;
  }

  // CASE WHEN <condition> THEN c [WHEN <condition> THEN c or a literal] [ELSE c or a literal] END
  // over a column: the values share c's kind (a literal of it, as DuckDB types CASE). Returns
  // whether the result is exact (not DOUBLE), or std::nullopt (nothing written).
  std::optional<bool> Case(const GenColumn& c) {
    // DECIMAL CASE values are unsupported until roadmap PR D4b (ADR 0021).
    if (!allowed_.Has(Feature::kCase) || comparable_.empty() || c.kind == ValueKind::kDecimal) {
      return std::nullopt;
    }
    used_.Add(Feature::kCase);
    const auto value = [&](bool column) {
      if (column || !CaseLiteral(c)) {
        Column(c);
      }
    };
    Keyword("CASE");
    const std::size_t branches = 1 + rng_.Below(2);
    for (std::size_t b = 0; b < branches; ++b) {
      Keyword("WHEN");
      Condition();
      Keyword("THEN");
      value(b == 0 || rng_.Percent(50));
    }
    if (rng_.Percent(70)) {
      Keyword("ELSE");
      value(rng_.Percent(30));
    }
    Keyword("END");
    return c.kind != ValueKind::kDouble;
  }

  // A literal of the column's kind for a CASE value; false if none is allowed (nothing written).
  bool CaseLiteral(const GenColumn& c) {
    const auto emit = [&](std::string text, Feature feature) {
      if (!allowed_.Has(feature)) {
        return false;
      }
      used_.Add(feature);
      tokens_.push_back({.kind = Token::Kind::kLiteral, .text = std::move(text)});
      return true;
    };
    switch (c.kind) {
      case ValueKind::kInteger: {
        static constexpr auto kValues = std::to_array<std::string_view>({"0", "7", "100000"});
        if (rng_.Percent(20) && allowed_.Has(Feature::kNegativeLiteral) &&
            allowed_.Has(Feature::kIntegerLiteral)) {
          used_.Add(Feature::kNegativeLiteral);
          Symbol("-");
          return emit("1", Feature::kIntegerLiteral);
        }
        return emit(std::string(rng_.Pick(kValues)), Feature::kIntegerLiteral);
      }
      case ValueKind::kDouble:
        return rng_.Percent(50) ? emit("0.5", Feature::kDecimalLiteral)
                                : emit("2", Feature::kIntegerLiteral);
      case ValueKind::kVarchar:
        return emit(rng_.Percent(50) ? "''" : "'k'", Feature::kStringLiteral);
      case ValueKind::kDecimal:  // never: Case() skips DECIMAL
        return false;
      case ValueKind::kDate: {
        // As in MakeLiteral: the roll modulo 100 is the old Percent(50) roll.
        const std::size_t roll = rng_.Below(400);
        if (roll % 100 < 50 &&
            (allowed_.Has(Feature::kDateLiteral) || allowed_.Has(Feature::kCastDate))) {
          used_.Add(TypedDate(tokens_, "'2013-07-15'", roll / 100) ? Feature::kCastDate
                                                                   : Feature::kDateLiteral);
          return true;
        }
        return emit("'2013-07-16'", Feature::kStringLiteral);
      }
    }
    return false;
  }

  // strlen(c) (`length`: an exact BIGINT) or regexp_replace(c, 'pattern', 'replacement') of a
  // VARCHAR column. Returns the result's kind, or std::nullopt (nothing written).
  std::optional<ValueKind> StringFunction(const GenColumn& c, bool length) {
    if (!allowed_.Has(Feature::kStringFunctions) || c.kind != ValueKind::kVarchar ||
        (!length && !allowed_.Has(Feature::kStringLiteral))) {
      return std::nullopt;
    }
    used_.Add(Feature::kStringFunctions);
    if (length) {
      Keyword("STRLEN");
      Symbol("(");
      Column(c);
      Symbol(")");
      return ValueKind::kInteger;
    }
    static constexpr auto kPatterns =
        std::to_array<std::string_view>({"a", "[0-9]+", "^(.)", "(.)$", "e.", "^([a-z]+)://([^/]*)",
                                         "\\s+", "e\\B", "(e)$|(e)", "\\b"});
    static constexpr auto kReplacements =
        std::to_array<std::string_view>({"", "X", "\\1", "<\\0>"});
    used_.Add(Feature::kStringLiteral);
    Keyword("REGEXP_REPLACE");
    Symbol("(");
    Column(c);
    Symbol(",");
    tokens_.push_back({.kind = Token::Kind::kLiteral, .text = SqlString(rng_.Pick(kPatterns))});
    Symbol(",");
    tokens_.push_back({.kind = Token::Kind::kLiteral, .text = SqlString(rng_.Pick(kReplacements))});
    Symbol(")");
    return ValueKind::kVarchar;
  }

  // A numeric column with a constant under an operator that both engines compute alike and
  // without an overflow (the column's data range decides): c + k, c - k, c * k, c / k (DOUBLE),
  // c // k, c % k, -c. `aggregated`: an aggregate's argument, where a DECIMAL keeps its scale (no
  // decimal literal), since HAVING writes literals of the column's scale (ValuesOf); `summed`: the
  // argument of a SUM or AVG, which gets no new inexact DOUBLE (a DECIMAL divided, an integer
  // divided by a decimal literal), as the sum's rounding follows the order of the additions.
  // Returns whether the result is exact (I values; / and DOUBLE give R), or std::nullopt when none
  // fits (nothing written).
  std::optional<bool> Arithmetic(const GenColumn& c, bool aggregated = false, bool summed = false) {
    if (!allowed_.Has(Feature::kArithmetic)) {
      return std::nullopt;
    }
    struct Choice {
      std::string_view op;
      std::string literal;  // empty: the unary minus
      bool exact = true;
    };
    std::vector<Choice> choices;
    const bool integers = allowed_.Has(Feature::kIntegerLiteral);
    const bool decimals = allowed_.Has(Feature::kDecimalLiteral);
    if (c.kind == ValueKind::kDouble) {
      if (decimals) {
        choices.push_back({.op = "+", .literal = "1.5", .exact = false});
        choices.push_back({.op = "-", .literal = "0.25", .exact = false});
      }
      if (integers) {
        for (const std::string_view op : {"*", "/", "//", "%"}) {
          choices.push_back({.op = op, .literal = op == "%" ? "3" : "4", .exact = false});
        }
      }
      choices.push_back({.op = "-", .literal = "", .exact = false});
    } else if (c.kind == ValueKind::kInteger && integers) {
      for (const int64_t k : {2, 3, 7}) {
        choices.push_back({.op = "//", .literal = std::to_string(k), .exact = true});
        choices.push_back({.op = "%", .literal = std::to_string(k), .exact = true});
      }
      choices.push_back({.op = "/", .literal = "4", .exact = false});
      if (c.data_min.has_value() && c.data_max.has_value()) {
        for (const int64_t k : {1, 7, 100}) {
          if (*c.data_max <= c.max - k) {
            choices.push_back({.op = "+", .literal = std::to_string(k), .exact = true});
          }
          if (*c.data_min >= c.min + k) {
            choices.push_back({.op = "-", .literal = std::to_string(k), .exact = true});
          }
        }
        for (const int64_t k : {2, 3}) {
          if (*c.data_max <= c.max / k && *c.data_min >= c.min / k) {
            choices.push_back({.op = "*", .literal = std::to_string(k), .exact = true});
          }
        }
        if (c.min < 0 && *c.data_min > c.min) {  // not USMALLINT: DuckDB wraps its negation
          choices.push_back({.op = "-", .literal = "", .exact = true});
        }
      }
      if (decimals) {
        // DECIMAL(k+1, 1) or (k+2, 2) and wider than any integer value: never an overflow (ADR
        // 0021 rules 3 to 6); / and // are DOUBLE, % exact.
        choices.push_back({.op = "+", .literal = "1.5", .exact = true});
        choices.push_back({.op = "-", .literal = "0.25", .exact = true});
        choices.push_back({.op = "*", .literal = "1.5", .exact = true});
        choices.push_back({.op = "%", .literal = "2.5", .exact = true});
        if (!summed) {  // inexact doubles: a sum's rounding follows the order of the additions
          choices.push_back({.op = "/", .literal = "1.5", .exact = false});
          choices.push_back({.op = "//", .literal = "2.5", .exact = false});
        }
      }
    } else if (c.kind == ValueKind::kDecimal && (integers || decimals)) {
      // DuckDB's type of c <op> k with an INTEGER k (DECIMAL(10,0)) or a decimal literal k
      // (DECIMAL(digits, fraction digits)), and whether every value, and its sum over every row
      // (the argument of a SUM), stays inside it (ADR 0021 rules 3 to 7).
      const Int128 bound = c.abs_max.value_or(0);
      const auto power = [](int n) { return MaxOfDigits(n) + 1; };
      const auto fits = [&](bool multiply, const LiteralOperand& k) {
        int width = 0;
        Int128 largest = 0;
        if (multiply) {
          if (c.scale + k.scale > 38) {
            return false;  // a bind error in both engines
          }
          width = c.precision + k.width;
          if (width > 18 && c.precision <= 18 && k.width <= 18 && c.scale + k.scale < 18) {
            width = 18;
          }
          if (__builtin_mul_overflow(bound, k.unscaled, &largest)) {
            return false;
          }
        } else {
          const int scale = std::max(c.scale, k.scale);
          width = std::max(c.precision - c.scale, k.width - k.scale) + scale + 1;
          if (width > 18 && c.precision <= 18 && k.width <= 18) {
            width = 18;
          }
          Int128 scaled_bound = 0;
          Int128 scaled_k = 0;
          if (__builtin_mul_overflow(bound, power(scale - c.scale), &scaled_bound) ||
              __builtin_mul_overflow(k.unscaled, power(scale - k.scale), &scaled_k) ||
              __builtin_add_overflow(scaled_bound, scaled_k, &largest)) {
            return false;
          }
        }
        width = std::min(width, 38);
        return largest <= MaxOfDigits(width) &&
               largest <= MaxOfDigits(38) / std::max<int64_t>(rows_, 1);
      };
      // % computes in the common type, which DuckDB makes DOUBLE beyond 38 digits (unsupported).
      const auto modulo_fits = [&](const LiteralOperand& k) {
        return std::max(c.precision - c.scale, k.width - k.scale) + std::max(c.scale, k.scale) <=
               38;
      };
      std::vector<LiteralOperand> addends;
      std::vector<LiteralOperand> factors;
      std::vector<LiteralOperand> divisors;
      if (integers) {
        for (const int64_t k : {1, 7, 100}) {
          addends.push_back(LiteralOperand::Integer(k));
        }
        for (const int64_t k : {2, 3}) {
          factors.push_back(LiteralOperand::Integer(k));
        }
        for (const int64_t k : {3, 7}) {
          divisors.push_back(LiteralOperand::Integer(k));
        }
      }
      if (decimals && !aggregated) {
        addends.push_back({.text = "0.5", .width = 2, .scale = 1, .unscaled = 5});
        addends.push_back({.text = "0.25", .width = 3, .scale = 2, .unscaled = 25});
        factors.push_back({.text = "1.5", .width = 2, .scale = 1, .unscaled = 15});
        divisors.push_back({.text = "2.5", .width = 2, .scale = 1, .unscaled = 25});
      }
      for (const LiteralOperand& k : addends) {
        if (fits(false, k)) {
          choices.push_back({.op = "+", .literal = k.text, .exact = true});
          choices.push_back({.op = "-", .literal = k.text, .exact = true});
        }
      }
      for (const LiteralOperand& k : factors) {
        if (fits(true, k)) {
          choices.push_back({.op = "*", .literal = k.text, .exact = true});
        }
      }
      for (const LiteralOperand& k : divisors) {
        // / and // are DOUBLE (rule 8), % exact in DECIMAL (rule 9). The doubles are inexact, so
        // not under SUM or AVG, whose rounding follows the order of the additions.
        if (!summed) {
          choices.push_back({.op = "/", .literal = k.text, .exact = false});
          choices.push_back({.op = "//", .literal = k.text, .exact = false});
        }
        if (modulo_fits(k)) {
          choices.push_back({.op = "%", .literal = k.text, .exact = true});
        }
      }
      choices.push_back({.op = "-", .literal = "", .exact = true});  // the type is kept
    }
    if (choices.empty()) {
      return std::nullopt;
    }
    const Choice& pick = rng_.Pick(choices);
    used_.Add(Feature::kArithmetic);
    if (pick.literal.empty()) {
      Symbol("-");
      Column(c);
      return pick.exact;
    }
    Column(c);
    Symbol(pick.op);
    used_.Add(pick.literal.contains('.') ? Feature::kDecimalLiteral : Feature::kIntegerLiteral);
    tokens_.push_back({.kind = Token::Kind::kLiteral, .text = pick.literal});
    return pick.exact;
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
                            tok.text != ";" && tok.text != "::" && tokens_[i - 1].text != "(" &&
                            tokens_[i - 1].text != "::";
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
  std::vector<const GenColumn*> comparable_;  // the query's columns WHERE can compare
  int64_t rows_ = 0;                          // the query's table's row count
  bool arg_retyped_ =
      false;  // the last Aggregate() wrapped its column in a string or time function
  std::vector<std::string> order_aliases_;  // select aliases of items with I or T values
  struct AggregateAlias {
    std::string alias;
    std::pair<Agg, const GenColumn*> call;
  };
  std::vector<AggregateAlias> aggregate_aliases_;  // those of them that name aggregates
  std::vector<std::size_t> order_positions_;  // positions of items with I or T values (keys too)
  std::vector<std::pair<const GenColumn*, std::size_t>> key_positions_;  // selected GROUP BY keys
  std::vector<std::size_t> constant_positions_;  // positions of constant items
  bool aggregate_emitted_ = false;  // the select list has an aggregate (constants alone: rows)
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
    std::vector<int> fields;  // per column of t: its field in the file
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
      fields.push_back(i);
    }
    // The integer and DECIMAL columns' value ranges over every file (all files have the first one's
    // schema).
    for (const auto& f : def.files) {
      auto file = f == def.files.front() ? data : ReadFile(f);
      if (!file) {
        return std::unexpected(file.error());
      }
      for (std::size_t k = 0; k < t.columns.size(); ++k) {
        if ((t.columns[k].kind != ValueKind::kInteger &&
             t.columns[k].kind != ValueKind::kDecimal) ||
            fields[k] >= (*file)->num_columns()) {
          continue;
        }
        for (const auto& chunk : (*file)->column(fields[k])->chunks()) {
          AddRange(t.columns[k], *chunk);
        }
      }
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
