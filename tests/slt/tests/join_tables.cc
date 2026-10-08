#include "join_tables.h"

#include <cstdint>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include "query_gen.h"

namespace antb1::slt {
namespace {

GenColumn IntegerColumn(std::string name, int64_t min, int64_t max) {
  return GenColumn{.name = std::move(name),
                   .kind = ValueKind::kInteger,
                   .min = min,
                   .max = max,
                   .samples = {"1", "7"}};
}

GenRef Ref(std::vector<std::string> columns, std::string table,
           std::vector<std::string> ref_columns, GenKeyStats stats, GenKeyStats ref_stats) {
  return GenRef{.columns = std::move(columns),
                .table = std::move(table),
                .ref_columns = std::move(ref_columns),
                .stats = stats,
                .ref_stats = ref_stats};
}

GenKeyStats Stats(int64_t non_null, int64_t distinct, int64_t max_multiplicity) {
  return GenKeyStats{
      .non_null = non_null, .distinct = distinct, .max_multiplicity = max_multiplicity};
}

}  // namespace

std::vector<GenTable> JoinTables() {
  constexpr int64_t kInt32Min = std::numeric_limits<int32_t>::min();
  constexpr int64_t kInt32Max = std::numeric_limits<int32_t>::max();
  constexpr int64_t kInt64Min = std::numeric_limits<int64_t>::min();
  constexpr int64_t kInt64Max = std::numeric_limits<int64_t>::max();
  const auto decimal = [](std::string name, int precision, int scale, std::string sample) {
    return GenColumn{.name = std::move(name),
                     .kind = ValueKind::kDecimal,
                     .samples = {std::move(sample)},
                     .precision = precision,
                     .scale = scale};
  };
  GenTable facts{.name = "facts", .path = "/data/star/facts.parquet", .rows = 3000, .columns = {}};
  facts.columns = {
      IntegerColumn("f_id", kInt32Min, kInt32Max),
      IntegerColumn("f_dim", kInt64Min, kInt64Max),
      IntegerColumn("f_alt", kInt32Min, kInt32Max),
      {.name = "f_code", .kind = ValueKind::kVarchar, .samples = {"STD", "xl "}},
      {.name = "f_day", .kind = ValueKind::kDate, .samples = {"2026-03-01"}},
      decimal("f_amount", 9, 2, "3.50"),
      decimal("f_wide", 38, 0, "5"),
      {.name = "f_ratio", .kind = ValueKind::kDouble, .samples = {"0.5"}},
      {.name = "label", .kind = ValueKind::kVarchar, .samples = {"north"}},
      {.name = "EventDate",
       .kind = ValueKind::kDate,
       .via_override = true,
       .samples = {"2013-07-02"}},
  };
  const GenKeyStats dim_key = Stats(40, 40, 1);
  facts.refs = {
      Ref({"f_dim"}, "dims", {"d_id"}, Stats(3000, 40, 75), dim_key),
      Ref({"f_alt"}, "dims", {"d_id"}, Stats(2800, 40, 70), dim_key),
      Ref({"f_code"}, "codes", {"c_code"}, Stats(2700, 12, 400), Stats(0, 0, 0)),
      Ref({"f_dim", "f_day"}, "slots", {"s_dim", "s_day"}, Stats(2900, 400, 21),
          Stats(490, 480, 2)),
      Ref({"f_amount"}, "dims", {"d_amount"}, Stats(2950, 30, 300), Stats(38, 30, 2)),
      Ref({"f_ratio"}, "dims", {"d_ratio"}, Stats(3000, 100, 30), Stats(40, 40, 1)),
      Ref({"f_id"}, "ghosts", {"g_id"}, Stats(3000, 3000, 1), Stats(10, 10, 1)),
      Ref({"f_float"}, "dims", {"d_id"}, Stats(3000, 40, 75), dim_key),
      Ref({"f_wide"}, "dims", {"d_fine"}, Stats(3000, 40, 75), dim_key),
      Ref({"f_id"}, "codes", {"c_code"}, Stats(3000, 3000, 1), Stats(0, 0, 0)),
  };
  GenTable dims{.name = "dims", .path = "/data/dims/part-*.parquet", .rows = 40, .columns = {}};
  dims.columns = {
      IntegerColumn("d_id", kInt64Min, kInt64Max),
      IntegerColumn("d_parent", kInt32Min, kInt32Max),
      {.name = "label", .kind = ValueKind::kVarchar, .samples = {"south"}},
      {.name = "d_ratio", .kind = ValueKind::kDouble, .samples = {"0.25"}},
      decimal("d_amount", 9, 2, "3.50"),
      decimal("d_fine", 38, 10, "1.0000000000"),
  };
  dims.refs = {Ref({"d_parent"}, "dims", {"d_id"}, Stats(35, 6, 7), dim_key)};
  GenTable slots{.name = "slots",
                 .path = "/data/star/slot_table.parquet",
                 .rows = 500,
                 .columns = {},
                 .other_columns = true};
  slots.columns = {
      IntegerColumn("s_dim", kInt32Min, kInt32Max),
      {.name = "s_day", .kind = ValueKind::kDate, .samples = {"2026-03-02"}},
      decimal("s_hours", 4, 1, "8.5"),
  };
  GenTable codes{.name = "codes", .path = "/data/.codes.v1.parquet", .rows = 0, .columns = {}};
  codes.columns = {
      {.name = "c_code", .kind = ValueKind::kVarchar, .samples = {}},
      IntegerColumn("c_rank", kInt32Min, kInt32Max),
  };
  return {facts, dims, slots, codes};
}

}  // namespace antb1::slt
