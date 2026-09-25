#include "hits_schema.h"

#include <array>
#include <memory>
#include <span>
#include <string>
#include <utility>

#include <arrow/api.h>

namespace antb1::fixturegen {
namespace {

using enum HitsType;

constexpr std::array<HitsColumn, kHitsColumnCount> kColumns{{
    {.name = "WatchID", .type = kInt64},
    {.name = "JavaEnable", .type = kInt16},
    {.name = "Title", .type = kBinary},
    {.name = "GoodEvent", .type = kInt16},
    {.name = "EventTime", .type = kInt64},
    {.name = "EventDate", .type = kUInt16},
    {.name = "CounterID", .type = kInt32},
    {.name = "ClientIP", .type = kInt32},
    {.name = "RegionID", .type = kInt32},
    {.name = "UserID", .type = kInt64},
    {.name = "CounterClass", .type = kInt16},
    {.name = "OS", .type = kInt16},
    {.name = "UserAgent", .type = kInt16},
    {.name = "URL", .type = kBinary},
    {.name = "Referer", .type = kBinary},
    {.name = "IsRefresh", .type = kInt16},
    {.name = "RefererCategoryID", .type = kInt16},
    {.name = "RefererRegionID", .type = kInt32},
    {.name = "URLCategoryID", .type = kInt16},
    {.name = "URLRegionID", .type = kInt32},
    {.name = "ResolutionWidth", .type = kInt16},
    {.name = "ResolutionHeight", .type = kInt16},
    {.name = "ResolutionDepth", .type = kInt16},
    {.name = "FlashMajor", .type = kInt16},
    {.name = "FlashMinor", .type = kInt16},
    {.name = "FlashMinor2", .type = kBinary},
    {.name = "NetMajor", .type = kInt16},
    {.name = "NetMinor", .type = kInt16},
    {.name = "UserAgentMajor", .type = kInt16},
    {.name = "UserAgentMinor", .type = kBinary},
    {.name = "CookieEnable", .type = kInt16},
    {.name = "JavascriptEnable", .type = kInt16},
    {.name = "IsMobile", .type = kInt16},
    {.name = "MobilePhone", .type = kInt16},
    {.name = "MobilePhoneModel", .type = kBinary},
    {.name = "Params", .type = kBinary},
    {.name = "IPNetworkID", .type = kInt32},
    {.name = "TraficSourceID", .type = kInt16},
    {.name = "SearchEngineID", .type = kInt16},
    {.name = "SearchPhrase", .type = kBinary},
    {.name = "AdvEngineID", .type = kInt16},
    {.name = "IsArtifical", .type = kInt16},
    {.name = "WindowClientWidth", .type = kInt16},
    {.name = "WindowClientHeight", .type = kInt16},
    {.name = "ClientTimeZone", .type = kInt16},
    {.name = "ClientEventTime", .type = kInt64},
    {.name = "SilverlightVersion1", .type = kInt16},
    {.name = "SilverlightVersion2", .type = kInt16},
    {.name = "SilverlightVersion3", .type = kInt32},
    {.name = "SilverlightVersion4", .type = kInt16},
    {.name = "PageCharset", .type = kBinary},
    {.name = "CodeVersion", .type = kInt32},
    {.name = "IsLink", .type = kInt16},
    {.name = "IsDownload", .type = kInt16},
    {.name = "IsNotBounce", .type = kInt16},
    {.name = "FUniqID", .type = kInt64},
    {.name = "OriginalURL", .type = kBinary},
    {.name = "HID", .type = kInt32},
    {.name = "IsOldCounter", .type = kInt16},
    {.name = "IsEvent", .type = kInt16},
    {.name = "IsParameter", .type = kInt16},
    {.name = "DontCountHits", .type = kInt16},
    {.name = "WithHash", .type = kInt16},
    {.name = "HitColor", .type = kBinary},
    {.name = "LocalEventTime", .type = kInt64},
    {.name = "Age", .type = kInt16},
    {.name = "Sex", .type = kInt16},
    {.name = "Income", .type = kInt16},
    {.name = "Interests", .type = kInt16},
    {.name = "Robotness", .type = kInt16},
    {.name = "RemoteIP", .type = kInt32},
    {.name = "WindowName", .type = kInt32},
    {.name = "OpenerName", .type = kInt32},
    {.name = "HistoryLength", .type = kInt16},
    {.name = "BrowserLanguage", .type = kBinary},
    {.name = "BrowserCountry", .type = kBinary},
    {.name = "SocialNetwork", .type = kBinary},
    {.name = "SocialAction", .type = kBinary},
    {.name = "HTTPError", .type = kInt16},
    {.name = "SendTiming", .type = kInt32},
    {.name = "DNSTiming", .type = kInt32},
    {.name = "ConnectTiming", .type = kInt32},
    {.name = "ResponseStartTiming", .type = kInt32},
    {.name = "ResponseEndTiming", .type = kInt32},
    {.name = "FetchTiming", .type = kInt32},
    {.name = "SocialSourceNetworkID", .type = kInt16},
    {.name = "SocialSourcePage", .type = kBinary},
    {.name = "ParamPrice", .type = kInt64},
    {.name = "ParamOrderID", .type = kBinary},
    {.name = "ParamCurrency", .type = kBinary},
    {.name = "ParamCurrencyID", .type = kInt16},
    {.name = "OpenstatServiceName", .type = kBinary},
    {.name = "OpenstatCampaignID", .type = kBinary},
    {.name = "OpenstatAdID", .type = kBinary},
    {.name = "OpenstatSourceID", .type = kBinary},
    {.name = "UTMSource", .type = kBinary},
    {.name = "UTMMedium", .type = kBinary},
    {.name = "UTMCampaign", .type = kBinary},
    {.name = "UTMContent", .type = kBinary},
    {.name = "UTMTerm", .type = kBinary},
    {.name = "FromTag", .type = kBinary},
    {.name = "HasGCLID", .type = kInt16},
    {.name = "RefererHash", .type = kInt64},
    {.name = "URLHash", .type = kInt64},
    {.name = "CLID", .type = kInt32},
}};

constexpr std::size_t CountOf(HitsType type) {
  std::size_t n = 0;
  for (const auto& c : kColumns) {
    n += c.type == type ? 1 : 0;
  }
  return n;
}

static_assert(CountOf(kInt64) == 9 && CountOf(kInt32) == 19 && CountOf(kUInt16) == 1 &&
                  CountOf(kBinary) == 28 && CountOf(kInt16) == 48,
              "the hits-like column type counts are fixed");

std::shared_ptr<arrow::DataType> ArrowType(HitsType type, HitsVariant variant) {
  switch (type) {
    case kInt64:
      return arrow::int64();
    case kInt32:
      return arrow::int32();
    case kInt16:
      return arrow::int16();
    case kUInt16:
      return arrow::uint16();
    case kBinary:
      return variant == HitsVariant::kSingleFile ? arrow::utf8() : arrow::binary();
  }
  std::unreachable();
}

}  // namespace

std::span<const HitsColumn> HitsColumns() { return kColumns; }

std::shared_ptr<arrow::Schema> HitsArrowSchema(HitsVariant variant) {
  arrow::FieldVector fields;
  fields.reserve(kColumns.size());
  const bool nullable = variant == HitsVariant::kPartitioned;
  for (const auto& c : kColumns) {
    fields.push_back(arrow::field(std::string(c.name), ArrowType(c.type, variant), nullable));
  }
  return arrow::schema(std::move(fields));
}

}  // namespace antb1::fixturegen
