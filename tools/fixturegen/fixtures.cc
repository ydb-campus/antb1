#include "fixtures.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <format>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>
#include <vector>

#include <arrow/api.h>
#include <arrow/io/file.h>
#include <parquet/arrow/schema.h>
#include <parquet/arrow/writer.h>
#include <parquet/exception.h>
#include <parquet/file_reader.h>
#include <parquet/metadata.h>
#include <parquet/properties.h>
#include <parquet/schema.h>
#include <parquet/types.h>

#include "hits_schema.h"

namespace antb1::fixturegen {
namespace {

namespace fs = std::filesystem;

constexpr uint64_t kSeed = 0x616E7462'31464758ULL;  // "antb1FGX"
constexpr uint64_t kNullSalt = 0x4E554C4CULL;       // decides NULL cells
constexpr uint64_t kShareSalt = 0x53484152ULL;      // decides zero / empty cells
constexpr int64_t kJuly2013 = 1'372'636'800;        // 2013-07-01T00:00:00Z
constexpr int64_t kSecondsPerDay = 86'400;
constexpr int64_t kTwoPow62 = 4'611'686'018'427'387'904;  // UserID-like values are in [1, 2^62]

constexpr uint64_t Fnv1a(std::string_view s) {
  uint64_t h = 0xCBF29CE484222325ULL;
  for (const char c : s) {
    h ^= static_cast<unsigned char>(c);
    h *= 0x100000001B3ULL;
  }
  return h;
}

// The row-th output of the splitmix64 stream owned by (column, salt): independent of how the rows
// are sliced into files, so every variant and split file shares the same values.
uint64_t Draw(std::string_view column, int64_t row, uint64_t salt = 0) {
  const uint64_t stream = SplitMix64::Mix(kSeed ^ Fnv1a(column) ^ (salt * SplitMix64::kGamma));
  return SplitMix64::Mix(stream + ((static_cast<uint64_t>(row) + 1U) * SplitMix64::kGamma));
}

// Uniform-ish integer in [lo, hi] (modulo reduction: deterministic, a tiny bias is irrelevant
// here).
int64_t InRange(uint64_t r, int64_t lo, int64_t hi) {
  const uint64_t span = static_cast<uint64_t>(hi) - static_cast<uint64_t>(lo) + 1U;  // 0: all 2^64
  const uint64_t offset = span == 0 ? r : r % span;
  return static_cast<int64_t>(static_cast<uint64_t>(lo) + offset);
}

bool Share(std::string_view column, int64_t row, unsigned percent) {
  return Draw(column, row, kShareSalt) % 100U < percent;
}

template <class T>
T Pick(std::span<const T> values, uint64_t r) {
  return values[static_cast<std::size_t>(r % values.size())];
}

// ---- integer columns ----

struct IntProfile {
  std::string_view name;
  int64_t lo = 0;
  int64_t hi = 0;
  unsigned zero_percent = 0;  // share of rows that are 0 instead
};

constexpr auto kIntProfiles = std::to_array<IntProfile>({
    {.name = "JavaEnable", .lo = 0, .hi = 1},
    {.name = "GoodEvent", .lo = 1, .hi = 1, .zero_percent = 2},
    {.name = "RegionID", .lo = 1, .hi = 20'000, .zero_percent = 3},
    {.name = "CounterClass", .lo = 0, .hi = 3},
    {.name = "OS", .lo = 0, .hi = 120},
    {.name = "UserAgent", .lo = 0, .hi = 60},
    {.name = "IsRefresh", .lo = 0, .hi = 1},
    {.name = "RefererCategoryID", .lo = 1, .hi = 20, .zero_percent = 40},
    {.name = "RefererRegionID", .lo = 1, .hi = 20'000, .zero_percent = 40},
    {.name = "URLCategoryID", .lo = 1, .hi = 20, .zero_percent = 10},
    {.name = "URLRegionID", .lo = 1, .hi = 20'000, .zero_percent = 10},
    {.name = "FlashMajor", .lo = 0, .hi = 11},
    {.name = "FlashMinor", .lo = 0, .hi = 9},
    {.name = "NetMajor", .lo = 0, .hi = 3},
    {.name = "NetMinor", .lo = 0, .hi = 9},
    {.name = "UserAgentMajor", .lo = 0, .hi = 40},
    {.name = "CookieEnable", .lo = 0, .hi = 1},
    {.name = "JavascriptEnable", .lo = 0, .hi = 1},
    {.name = "IsMobile", .lo = 0, .hi = 1},
    {.name = "MobilePhone", .lo = 1, .hi = 40, .zero_percent = 85},
    {.name = "IPNetworkID", .lo = 1, .hi = 4'000'000},
    {.name = "TraficSourceID", .lo = -1, .hi = 9},
    {.name = "SearchEngineID", .lo = 1, .hi = 30, .zero_percent = 80},
    {.name = "AdvEngineID", .lo = 1, .hi = 60, .zero_percent = 90},
    {.name = "IsArtifical", .lo = 0, .hi = 1},
    {.name = "WindowClientWidth", .lo = 1, .hi = 2560, .zero_percent = 5},
    {.name = "WindowClientHeight", .lo = 1, .hi = 1600, .zero_percent = 5},
    {.name = "ClientTimeZone", .lo = -12, .hi = 14},
    {.name = "SilverlightVersion1", .lo = 0, .hi = 5},
    {.name = "SilverlightVersion2", .lo = 0, .hi = 9},
    {.name = "SilverlightVersion3", .lo = 1, .hi = 9'999'999, .zero_percent = 50},
    {.name = "SilverlightVersion4", .lo = 0, .hi = 9},
    {.name = "CodeVersion", .lo = 1, .hi = 1000},
    {.name = "IsLink", .lo = 0, .hi = 1},
    {.name = "IsDownload", .lo = 0, .hi = 1},
    {.name = "IsNotBounce", .lo = 0, .hi = 1},
    {.name = "IsOldCounter", .lo = 0, .hi = 1},
    {.name = "IsEvent", .lo = 0, .hi = 1},
    {.name = "IsParameter", .lo = 0, .hi = 1},
    {.name = "DontCountHits", .lo = 0, .hi = 1},
    {.name = "WithHash", .lo = 0, .hi = 1},
    {.name = "Age", .lo = 0, .hi = 5},
    {.name = "Sex", .lo = 0, .hi = 2},
    {.name = "Income", .lo = 0, .hi = 4},
    {.name = "Robotness", .lo = 0, .hi = 10},
    {.name = "WindowName", .lo = -1, .hi = 1000},
    {.name = "OpenerName", .lo = -1, .hi = 1000},
    {.name = "HistoryLength", .lo = 0, .hi = 20},
    {.name = "HTTPError", .lo = 400, .hi = 599, .zero_percent = 95},
    {.name = "SendTiming", .lo = 0, .hi = 10'000},
    {.name = "DNSTiming", .lo = 0, .hi = 10'000},
    {.name = "ConnectTiming", .lo = 0, .hi = 10'000},
    {.name = "ResponseStartTiming", .lo = 0, .hi = 10'000},
    {.name = "ResponseEndTiming", .lo = 0, .hi = 10'000},
    {.name = "FetchTiming", .lo = 0, .hi = 10'000},
    {.name = "SocialSourceNetworkID", .lo = 1, .hi = 10, .zero_percent = 97},
    {.name = "ParamCurrencyID", .lo = 1, .hi = 5, .zero_percent = 95},
    {.name = "HasGCLID", .lo = 0, .hi = 1},
    {.name = "CLID", .lo = 1, .hi = 100, .zero_percent = 90},
});

constexpr auto kWidths = std::to_array<int64_t>({0, 1024, 1280, 1366, 1440, 1600, 1920, 2560});
constexpr auto kHeights = std::to_array<int64_t>({0, 768, 800, 900, 1024, 1050, 1080, 1440});
constexpr auto kDepths = std::to_array<int64_t>({16, 24, 32});

int64_t EventTime(int64_t row) {
  return kJuly2013 + InRange(Draw("EventTime", row), 0, (31 * kSecondsPerDay) - 1);
}

int64_t Full32(uint64_t r) { return static_cast<int32_t>(static_cast<uint32_t>(r & 0xFFFFFFFFU)); }

constexpr int16_t kI16Min = std::numeric_limits<int16_t>::min();
constexpr int16_t kI16Max = std::numeric_limits<int16_t>::max();
constexpr int32_t kI32Min = std::numeric_limits<int32_t>::min();
constexpr int32_t kI32Max = std::numeric_limits<int32_t>::max();
constexpr int64_t kI64Min = std::numeric_limits<int64_t>::min();
constexpr int64_t kI64Max = std::numeric_limits<int64_t>::max();
constexpr uint16_t kU16Max = std::numeric_limits<uint16_t>::max();

struct EdgeValue {
  std::string_view column;
  int64_t row = 0;
  int64_t value = 0;
};

// Type extremes in rows 1..3 of some columns; SUM(UserID) and SUM(WatchID) overflow int64.
constexpr auto kEdgeValues = std::to_array<EdgeValue>({
    {.column = "WatchID", .row = 1, .value = kI64Max},
    {.column = "WatchID", .row = 2, .value = kI64Min},
    {.column = "UserID", .row = 1, .value = kI64Max},
    {.column = "UserID", .row = 2, .value = kI64Max},
    {.column = "UserID", .row = 3, .value = kI64Min},
    {.column = "ClientIP", .row = 1, .value = kI32Max},
    {.column = "ClientIP", .row = 2, .value = kI32Min},
    {.column = "Interests", .row = 1, .value = kI16Max},
    {.column = "Interests", .row = 2, .value = kI16Min},
});

std::optional<int64_t> EdgeCell(std::string_view name, int64_t row) {
  for (const auto& e : kEdgeValues) {
    if (e.row == row && e.column == name) {
      return e.value;
    }
  }
  return std::nullopt;
}

// One integer cell of the hits-like data set.
int64_t IntCell(std::string_view name, int64_t row) {
  if (const auto edge = EdgeCell(name, row); edge.has_value()) {
    return *edge;
  }
  const uint64_t r = Draw(name, row);
  if (name == "WatchID" || name == "RefererHash" || name == "URLHash") {
    return static_cast<int64_t>(r);
  }
  if (name == "UserID" || name == "FUniqID") {
    return name == "FUniqID" && Share(name, row, 30) ? 0 : InRange(r, 1, kTwoPow62);
  }
  if (name == "EventTime") {
    return EventTime(row);
  }
  if (name == "EventDate") {
    return EventTime(row) / kSecondsPerDay;
  }
  if (name == "ClientEventTime") {
    return Share(name, row, 5) ? 0 : EventTime(row) + InRange(r, -3600, 3600);
  }
  if (name == "LocalEventTime") {
    return EventTime(row) + (3600 * InRange(r, -12, 14));
  }
  if (name == "ParamPrice") {
    return Share(name, row, 95) ? 0 : InRange(r, 1, 1'000'000'000);
  }
  if (name == "ClientIP" || name == "RemoteIP" || name == "HID") {
    return Full32(r);
  }
  if (name == "CounterID") {
    return 62 + (1000 * InRange(r, 0, 39));
  }
  if (name == "Interests") {
    return InRange(r, 0, 16'383);
  }
  if (name == "ResolutionWidth") {
    return Pick<int64_t>(kWidths, r);
  }
  if (name == "ResolutionHeight") {
    return Pick<int64_t>(kHeights, r);
  }
  if (name == "ResolutionDepth") {
    return Pick<int64_t>(kDepths, r);
  }
  for (const auto& p : kIntProfiles) {
    if (p.name == name) {
      return p.zero_percent > 0 && Share(name, row, p.zero_percent) ? 0 : InRange(r, p.lo, p.hi);
    }
  }
  return 0;  // not reached: every integer hits column has a rule above
}

// ---- string columns ----

constexpr auto kWords = std::to_array<std::string_view>({
    "alpha",  "beta",    "gamma", "delta",  "omega",  "river", "stone",  "cloud",
    "green",  "quick",   "orbit", "lemon",  "maple",  "pixel", "tundra", "violet",
    "погода", "новости", "книга", "музыка", "дорога", "λόγος", "café",   "naïve",
});
constexpr auto kCharsets = std::to_array<std::string_view>({"utf-8", "windows-1251", "koi8-r"});
constexpr auto kColors = std::to_array<std::string_view>({"5", "D", "E", "F"});
constexpr auto kLanguages = std::to_array<std::string_view>({"ru", "en", "de", "tr", "uk", "kk"});
constexpr auto kCountries = std::to_array<std::string_view>({"RU", "US", "DE", "TR", "UA", "KZ"});
constexpr auto kNetworks = std::to_array<std::string_view>({"vk", "ok", "fb", "tw"});
constexpr auto kActions = std::to_array<std::string_view>({"like", "share", "post"});
constexpr auto kCurrencies = std::to_array<std::string_view>({"RUB", "USD", "EUR"});
constexpr auto kMinors = std::to_array<std::string_view>({"0", "1", "2", "5", "10", "b1"});

enum class StrKind : std::uint8_t { kPhrase, kUrl, kDigits, kToken, kChoice };

struct StrProfile {
  std::string_view name;
  StrKind kind = StrKind::kToken;
  unsigned empty_percent = 0;
  std::span<const std::string_view> choices;
  int max_words = 3;
};

constexpr auto kStrProfiles = std::to_array<StrProfile>({
    {.name = "Title", .kind = StrKind::kPhrase, .empty_percent = 25, .max_words = 5},
    {.name = "URL", .kind = StrKind::kUrl, .empty_percent = 2},
    {.name = "Referer", .kind = StrKind::kUrl, .empty_percent = 40},
    {.name = "FlashMinor2", .kind = StrKind::kDigits, .empty_percent = 70},
    {.name = "UserAgentMinor", .kind = StrKind::kChoice, .empty_percent = 10, .choices = kMinors},
    {.name = "MobilePhoneModel", .kind = StrKind::kToken, .empty_percent = 85},
    {.name = "Params", .kind = StrKind::kToken, .empty_percent = 95},
    {.name = "SearchPhrase", .kind = StrKind::kPhrase, .empty_percent = 80, .max_words = 3},
    {.name = "PageCharset", .kind = StrKind::kChoice, .empty_percent = 5, .choices = kCharsets},
    {.name = "OriginalURL", .kind = StrKind::kUrl, .empty_percent = 90},
    {.name = "HitColor", .kind = StrKind::kChoice, .choices = kColors},
    {.name = "BrowserLanguage",
     .kind = StrKind::kChoice,
     .empty_percent = 3,
     .choices = kLanguages},
    {.name = "BrowserCountry", .kind = StrKind::kChoice, .empty_percent = 3, .choices = kCountries},
    {.name = "SocialNetwork", .kind = StrKind::kChoice, .empty_percent = 97, .choices = kNetworks},
    {.name = "SocialAction", .kind = StrKind::kChoice, .empty_percent = 99, .choices = kActions},
    {.name = "SocialSourcePage", .kind = StrKind::kUrl, .empty_percent = 98},
    {.name = "ParamOrderID", .kind = StrKind::kToken, .empty_percent = 98},
    {.name = "ParamCurrency",
     .kind = StrKind::kChoice,
     .empty_percent = 95,
     .choices = kCurrencies},
});

std::string Words(uint64_t r, int max_words) {
  SplitMix64 rng(r);
  const auto n = static_cast<int>(rng.Next() % static_cast<uint64_t>(max_words)) + 1;
  std::string out;
  for (int i = 0; i < n; ++i) {
    if (i > 0) {
      out += ' ';
    }
    out += Pick<std::string_view>(kWords, rng.Next());
  }
  return out;
}

std::string StrCell(std::string_view name, int64_t row) {
  const uint64_t r = Draw(name, row);
  if (name == "Title" && row == 7) {  // one long value
    return Words(r, 1) + std::string(700, 'x');
  }
  // Unlisted columns (Openstat*, UTM*, FromTag) are mostly empty tokens.
  StrProfile p{.name = name, .kind = StrKind::kToken, .empty_percent = 95};
  for (const auto& candidate : kStrProfiles) {
    if (candidate.name == name) {
      p = candidate;
    }
  }
  if (p.empty_percent > 0 && Share(name, row, p.empty_percent)) {
    return {};
  }
  switch (p.kind) {
    case StrKind::kPhrase:
      return Words(r, p.max_words);
    case StrKind::kUrl:
      return std::format("https://{}.example.org/{}/{}{}", name == "URL" ? "www" : "ref",
                         Pick<std::string_view>(kWords, r), r % 100'000U,
                         r % 4U == 0 ? "?q=" + Words(r >> 8U, 1) : std::string());
    case StrKind::kDigits:
      return std::to_string(r % 100U);
    case StrKind::kToken:
      return std::format("{}_{}", Pick<std::string_view>(kWords, r >> 16U), r % 1000U);
    case StrKind::kChoice:
      return std::string(Pick<std::string_view>(p.choices, r));
  }
  return {};
}

// ---- tables ----

bool IsNull(std::string_view name, Nulls nulls, int64_t row) {
  if (nulls == Nulls::kNone) {
    return false;
  }
  if (row == 0 || name == "SocialAction" || name == "HistoryLength") {
    return true;
  }
  if (name == "CounterID") {
    return false;  // NULL only in row 0
  }
  return Draw(name, row, kNullSalt) % 13U == 0;
}

template <class T>
T To(int64_t v) {
  if constexpr (std::is_same_v<T, int64_t>) {
    return v;
  } else {
    return static_cast<T>(v);
  }
}

template <class Builder, class T>
arrow::Result<std::shared_ptr<arrow::Array>> IntColumn(const HitsColumn& column, Nulls nulls,
                                                       int64_t first_row, int64_t num_rows) {
  Builder builder;
  ARROW_RETURN_NOT_OK(builder.Reserve(num_rows));
  for (int64_t row = first_row; row < first_row + num_rows; ++row) {
    if (IsNull(column.name, nulls, row)) {
      ARROW_RETURN_NOT_OK(builder.AppendNull());
    } else {
      ARROW_RETURN_NOT_OK(builder.Append(To<T>(IntCell(column.name, row))));
    }
  }
  return builder.Finish();
}

template <class Builder>
arrow::Result<std::shared_ptr<arrow::Array>> StrColumn(const HitsColumn& column, Nulls nulls,
                                                       int64_t first_row, int64_t num_rows) {
  Builder builder;
  ARROW_RETURN_NOT_OK(builder.Reserve(num_rows));
  for (int64_t row = first_row; row < first_row + num_rows; ++row) {
    if (IsNull(column.name, nulls, row)) {
      ARROW_RETURN_NOT_OK(builder.AppendNull());
    } else {
      ARROW_RETURN_NOT_OK(builder.Append(StrCell(column.name, row)));
    }
  }
  return builder.Finish();
}

arrow::Result<std::shared_ptr<arrow::Array>> HitsColumnArray(const HitsColumn& column,
                                                             HitsVariant variant, Nulls nulls,
                                                             int64_t first_row, int64_t num_rows) {
  switch (column.type) {
    case HitsType::kInt64:
      return IntColumn<arrow::Int64Builder, int64_t>(column, nulls, first_row, num_rows);
    case HitsType::kInt32:
      return IntColumn<arrow::Int32Builder, int32_t>(column, nulls, first_row, num_rows);
    case HitsType::kInt16:
      return IntColumn<arrow::Int16Builder, int16_t>(column, nulls, first_row, num_rows);
    case HitsType::kUInt16:
      return IntColumn<arrow::UInt16Builder, uint16_t>(column, nulls, first_row, num_rows);
    case HitsType::kBinary:
      if (variant == HitsVariant::kSingleFile) {
        return StrColumn<arrow::StringBuilder>(column, nulls, first_row, num_rows);
      }
      return StrColumn<arrow::BinaryBuilder>(column, nulls, first_row, num_rows);
  }
  return arrow::Status::Invalid("unknown hits column type");
}

// ---- edge table (column-wise; row i of every array is row i of edge.parquet) ----

constexpr std::size_t kEdgeRows = 12;
constexpr auto kNull = std::nullopt;

template <class T>
using EdgeColumnValues = std::array<std::optional<T>, kEdgeRows>;

constexpr EdgeColumnValues<int16_t> kEdgeI16{kI16Min, kI16Max, 0,     -1,     1,       kNull,
                                             100,     -100,    kNull, 12'345, -12'345, 7};
constexpr EdgeColumnValues<int32_t> kEdgeI32{kI32Min, kI32Max,  0, -1,    1,      kNull,
                                             100'000, -100'000, 7, kNull, 54'321, -7};
constexpr EdgeColumnValues<int64_t> kEdgeI64{
    kI64Min, kI64Max, 0,   -1, 1, kNull, 1'000'000'000'000'000, -1'000'000'000'000'000,
    kNull,   42,      -42, 7};
constexpr EdgeColumnValues<uint16_t> kEdgeU16{0,      kU16Max, 1, 32'768, 15'887, kNull,
                                              15'917, 40'000,  2, kNull,  12,     7};
// Doubles as numerator / denominator: IEEE division is correctly rounded on every platform.
constexpr EdgeColumnValues<std::pair<int64_t, int64_t>> kEdgeD{{
    {{-1, 2}},
    {{1, 3}},
    {{0, 1}},
    {{2, 3}},
    {{1, 10}},
    kNull,
    {{123'456'789'125, 1000}},
    {{-9'007'199'254'740'992, 1}},
    {{1, 1'000'000'007}},
    kNull,
    {{922'337'203'685'477'580, 1}},
    {{-1, 3}},
}};
constexpr EdgeColumnValues<std::string_view> kEdgeS{
    "",       "a",           "tab\there", "line\nbreak", "back\\slash", kNull, " leading space",
    "x\x01y", "Привет, мир", "NULL",      kNull,         "naïve café"};
constexpr EdgeColumnValues<std::string_view> kEdgeU{
    "",       "a",           "tab\there", "line\nbreak", "back\\slash", kNull, "trailing space ",
    "x\x7fy", "Привет, мир", "(empty)",   "日本語",      kNull};

template <class Builder, class T>
arrow::Result<std::shared_ptr<arrow::Array>> EdgeColumn(const EdgeColumnValues<T>& values) {
  Builder builder;
  for (const auto& v : values) {
    ARROW_RETURN_NOT_OK(v.has_value() ? builder.Append(*v) : builder.AppendNull());
  }
  return builder.Finish();
}

std::shared_ptr<parquet::WriterProperties> WriterProperties() {
  return parquet::WriterProperties::Builder().compression(parquet::Compression::SNAPPY)->build();
}

std::shared_ptr<parquet::ArrowWriterProperties> ArrowWriterProperties() {
  return parquet::ArrowWriterProperties::Builder().build();  // store_schema() stays off
}

std::string_view RepetitionName(parquet::Repetition::type repetition) {
  switch (repetition) {
    case parquet::Repetition::REQUIRED:
      return "REQUIRED";
    case parquet::Repetition::OPTIONAL:
      return "OPTIONAL";
    case parquet::Repetition::REPEATED:
      return "REPEATED";
    case parquet::Repetition::UNDEFINED:
      break;
  }
  return "UNDEFINED";
}

void CompareField(std::vector<std::string>& diffs, int column, std::string_view name,
                  std::string_view what, const std::string& expected, const std::string& actual) {
  if (expected != actual) {
    diffs.push_back(std::format("column {} ({}): {}: expected {}, got {}", column, name, what,
                                expected, actual));
  }
}

}  // namespace

arrow::Result<std::shared_ptr<arrow::Table>> MakeHitsTable(HitsVariant variant, Nulls nulls,
                                                           int64_t first_row, int64_t num_rows) {
  arrow::ArrayVector arrays;
  for (const auto& column : HitsColumns()) {
    ARROW_ASSIGN_OR_RAISE(auto array, HitsColumnArray(column, variant, nulls, first_row, num_rows));
    arrays.push_back(std::move(array));
  }
  return arrow::Table::Make(HitsArrowSchema(variant), arrays, num_rows);
}

arrow::Result<std::shared_ptr<arrow::Table>> MakeEdgeTable() {
  arrow::Int32Builder ids;
  arrow::DoubleBuilder doubles;
  for (std::size_t row = 0; row < kEdgeRows; ++row) {
    ARROW_RETURN_NOT_OK(ids.Append(static_cast<int32_t>(row)));
    const auto& ratio = kEdgeD[row];
    ARROW_RETURN_NOT_OK(ratio.has_value() ? doubles.Append(static_cast<double>(ratio->first) /
                                                           static_cast<double>(ratio->second))
                                          : doubles.AppendNull());
  }
  ARROW_ASSIGN_OR_RAISE(auto id_array, ids.Finish());
  ARROW_ASSIGN_OR_RAISE(auto d_array, doubles.Finish());
  ARROW_ASSIGN_OR_RAISE(auto i16, (EdgeColumn<arrow::Int16Builder, int16_t>(kEdgeI16)));
  ARROW_ASSIGN_OR_RAISE(auto i32, (EdgeColumn<arrow::Int32Builder, int32_t>(kEdgeI32)));
  ARROW_ASSIGN_OR_RAISE(auto i64, (EdgeColumn<arrow::Int64Builder, int64_t>(kEdgeI64)));
  ARROW_ASSIGN_OR_RAISE(auto u16, (EdgeColumn<arrow::UInt16Builder, uint16_t>(kEdgeU16)));
  ARROW_ASSIGN_OR_RAISE(auto s, (EdgeColumn<arrow::BinaryBuilder, std::string_view>(kEdgeS)));
  ARROW_ASSIGN_OR_RAISE(auto u, (EdgeColumn<arrow::StringBuilder, std::string_view>(kEdgeU)));
  auto schema = arrow::schema({
      arrow::field("id", arrow::int32(), /*nullable=*/false),
      arrow::field("i16", arrow::int16()),
      arrow::field("i32", arrow::int32()),
      arrow::field("i64", arrow::int64()),
      arrow::field("u16", arrow::uint16()),
      arrow::field("d", arrow::float64()),
      arrow::field("s", arrow::binary()),
      arrow::field("u", arrow::utf8()),
  });
  return arrow::Table::Make(std::move(schema), {id_array, i16, i32, i64, u16, d_array, s, u});
}

arrow::Status WriteParquet(const arrow::Table& table, const fs::path& path,
                           int64_t row_group_rows) {
  const fs::path tmp = fs::path(path).concat(".tmp");
  {
    ARROW_ASSIGN_OR_RAISE(auto out, arrow::io::FileOutputStream::Open(tmp.string()));
    ARROW_RETURN_NOT_OK(parquet::arrow::WriteTable(table, arrow::default_memory_pool(), out,
                                                   row_group_rows, WriterProperties(),
                                                   ArrowWriterProperties()));
    ARROW_RETURN_NOT_OK(out->Close());
  }
  std::error_code ec;
  fs::rename(tmp, path, ec);
  if (ec) {
    return arrow::Status::IOError("cannot rename '", tmp.string(), "' to '", path.string(),
                                  "': ", ec.message());
  }
  return arrow::Status::OK();
}

arrow::Result<std::vector<FixtureFile>> WriteAllFixtures(const fs::path& dir) {
  const fs::path split_dir = dir / "hits_like_split";
  std::error_code ec;
  fs::create_directories(split_dir, ec);
  if (ec) {
    return arrow::Status::IOError("cannot create '", split_dir.string(), "': ", ec.message());
  }
  // Stale parts from an older generator would be picked up by globs.
  for (const auto& entry : fs::directory_iterator(split_dir, ec)) {
    if (entry.path().extension() == ".parquet") {
      fs::remove(entry.path(), ec);
    }
  }

  std::vector<FixtureFile> written;
  auto write = [&](const arrow::Table& table, const std::string& rel,
                   int64_t row_group_rows) -> arrow::Status {
    ARROW_RETURN_NOT_OK(WriteParquet(table, dir / rel, row_group_rows));
    const int64_t rows = table.num_rows();
    // parquet::arrow writes one (empty) row group for an empty table.
    const auto groups =
        static_cast<int>(std::max<int64_t>(1, (rows + row_group_rows - 1) / row_group_rows));
    written.push_back(FixtureFile{.path = rel, .rows = rows, .row_groups = groups});
    return arrow::Status::OK();
  };

  constexpr int64_t kGroup = kHitsRows / 4;
  ARROW_ASSIGN_OR_RAISE(auto hits,
                        MakeHitsTable(HitsVariant::kPartitioned, Nulls::kNone, 0, kHitsRows));
  ARROW_RETURN_NOT_OK(write(*hits, "hits_like.parquet", kGroup));

  ARROW_ASSIGN_OR_RAISE(auto nulls,
                        MakeHitsTable(HitsVariant::kPartitioned, Nulls::kSprinkled, 0, kHitsRows));
  ARROW_RETURN_NOT_OK(write(*nulls, "hits_like_nulls.parquet", kGroup));

  constexpr auto kSplit = std::to_array<int64_t>({0, 1000, 4000, 6500, kHitsRows});
  for (std::size_t part = 0; part + 1 < kSplit.size(); ++part) {
    const auto slice = hits->Slice(kSplit[part], kSplit[part + 1] - kSplit[part]);
    ARROW_RETURN_NOT_OK(write(*slice, std::format("hits_like_split/part-{}.parquet", part), 1024));
  }

  ARROW_ASSIGN_OR_RAISE(auto required,
                        MakeHitsTable(HitsVariant::kSingleFile, Nulls::kNone, 0, kHitsRows));
  ARROW_RETURN_NOT_OK(write(*required, "hits_like_required.parquet", kHitsRows / 2));

  ARROW_ASSIGN_OR_RAISE(auto edge, MakeEdgeTable());
  ARROW_RETURN_NOT_OK(write(*edge, "edge.parquet", 5));

  ARROW_ASSIGN_OR_RAISE(auto empty, MakeHitsTable(HitsVariant::kPartitioned, Nulls::kNone, 0, 0));
  ARROW_RETURN_NOT_OK(write(*empty, "empty.parquet", kGroup));
  return written;
}

arrow::Result<std::vector<std::string>> CheckHitsSchema(const std::string& path) {
  const auto arrow_schema = HitsArrowSchema(HitsVariant::kPartitioned);
  std::shared_ptr<parquet::SchemaDescriptor> expected;
  ARROW_RETURN_NOT_OK(parquet::arrow::ToParquetSchema(arrow_schema.get(), *WriterProperties(),
                                                      *ArrowWriterProperties(), &expected));
  std::shared_ptr<parquet::FileMetaData> metadata;
  try {
    metadata = parquet::ParquetFileReader::OpenFile(path)->metadata();
  } catch (const parquet::ParquetException& e) {
    return arrow::Status::IOError("cannot read Parquet file '", path, "': ", e.what());
  } catch (const std::exception& e) {
    return arrow::Status::IOError("cannot read '", path, "': ", e.what());
  }
  const parquet::SchemaDescriptor* actual = metadata->schema();
  std::vector<std::string> diffs;
  if (actual->num_columns() != expected->num_columns()) {
    diffs.push_back(std::format("column count: expected {}, got {}", expected->num_columns(),
                                actual->num_columns()));
  }
  const int n = std::min(actual->num_columns(), expected->num_columns());
  for (int i = 0; i < n; ++i) {
    const parquet::ColumnDescriptor* e = expected->Column(i);
    const parquet::ColumnDescriptor* a = actual->Column(i);
    const std::string& name = e->name();
    CompareField(diffs, i, name, "path", e->path()->ToDotString(), a->path()->ToDotString());
    CompareField(diffs, i, name, "physical type", parquet::TypeToString(e->physical_type()),
                 parquet::TypeToString(a->physical_type()));
    CompareField(diffs, i, name, "logical type", e->logical_type()->ToString(),
                 a->logical_type()->ToString());
    CompareField(diffs, i, name, "converted type",
                 parquet::ConvertedTypeToString(e->converted_type()),
                 parquet::ConvertedTypeToString(a->converted_type()));
    CompareField(diffs, i, name, "repetition",
                 std::string(RepetitionName(e->schema_node()->repetition())),
                 std::string(RepetitionName(a->schema_node()->repetition())));
  }
  return diffs;
}

}  // namespace antb1::fixturegen
