#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <arrow/result.h>
#include <arrow/type_fwd.h>

// The star schema of the join and subquery tests (docs/testing.md#fixtures): a ride-hailing fact
// table and its dimensions, written to star/<name>.parquet and registered by tests/slt/tables.txt.
// Values are integer formulas of the row number i, with no random numbers and no libm: DOUBLE
// values are multiples of 1/8 (exact, never NaN or -0.0), DATE values are days, and DECIMAL
// formulas give the unscaled value. Row counts all differ, and every non-empty table has several
// row groups, the last one shorter.
//
// Every table's first column is <prefix>row (INTEGER, not nullable, 0 to rows - 1); every other
// column is nullable. Every column name carries its table's prefix but one deliberate collision:
// `label` in zones and cities. "-> t.c" marks the references that tables.txt declares (ref=).
//
//   trips (tr_)    3000 rows, groups of 1200: the fact table
//     tr_rider     BIGINT -> riders.rd_id: NULL if i%23==4; else 900+i%7 if i%101==9 (beyond the
//                  riders' range); else 7i%236+1 (riders 237..240 have no trip)
//     tr_driver    INTEGER -> drivers.dv_id: NULL if i%29==6; else 1000+37k with k=11i%100, plus
//                  10 if 90<=k<=95 (drivers 90..95 have no trip; k 50 and 96..105 match no driver)
//     tr_pickup    INTEGER -> zones.zn_id, never NULL: i%43+1 (41..43 match no zone)
//     tr_dropoff   INTEGER -> zones.zn_id (zones in a second role): NULL if i%17==11; else 13i%40+1
//     tr_day       DATE: NULL if i%37==20; else 2026-03-01 plus i%31 days (no shift on March 31)
//     (tr_driver, tr_day) -> shifts.(sh_driver, sh_day)
//     tr_tariff    VARCHAR -> tariffs.tf_code: NULL if i%19==3; else STD NIGHT AIRPORT Std ''
//                  'XL ' Ж-1 POOL std XL by 3i%10 (std and XL match no tariff)
//     tr_rate      DECIMAL(4,2) -> tariffs.tf_rate, of another type: NULL if i%13==6; else unscaled
//                  100 125 150 175 200 225 250 88 by i/7%8 (2.25 and 0.88 match no tariff)
//     tr_promo     INTEGER -> promos.pm_id: i/50%4+1 if i%50==0, else NULL (every value dangles)
//     tr_fare      DECIMAL(9,2): NULL if i%31==15; else unscaled 350+25*(29i%97), 3.50 to 27.50
//     tr_distance  DOUBLE: NULL if i%41==30; else (5i%160+1)/8, some equal to a zn_radius
//   riders (rd_)   240 rows, groups of 64
//     rd_id        BIGINT, dense 1..240: i+1, but rows 7, 47, 87, 127, 167, 207 (i%40==7) and the
//                  first rows of groups 1 and 3 (64, 192) repeat the key of the row before: 8 keys
//                  repeat (2 of them across a row group boundary) and 8 are missing
//     rd_city      SMALLINT -> cities.ct_id: NULL if i%29==13; else i%3+1
//     rd_tier      VARCHAR: basic plus gold by i/2%3
//     rd_joined    DATE: 2025-01-01 plus 13i%450 days
//   drivers (dv_)  97 rows, groups of 32 (the last group has 1 row)
//     dv_id        BIGINT, sparse: NULL if i==50; 2^32+1000 if i==96 (beyond INTEGER: an INTEGER
//                  key that wraps 32 bits matches the 29 trips of driver 1000); else 1000+37i. The
//                  range spans 2^32, so a join domain estimated from it is about 4.3e9 keys.
//     dv_city      SMALLINT -> cities.ct_id: NULL if i%11==10; else i%4+1
//     dv_since     DATE: 2025-01-01 plus 11i%365 days
//   zones (zn_)    40 rows, groups of 16
//     zn_id        INTEGER: i+1, dense and unique
//     zn_city      SMALLINT -> cities.ct_id: NULL if i==37; 9 if i==38 (no such city); else i%5+1
//     zn_parent    INTEGER -> zones.zn_id (a self-reference): NULL if i<5; 77 if i==39 (no such
//                  zone); else i%5+1
//     label        VARCHAR: NULL if i%8==7; else center north south airport docks park market
//     zn_radius    DOUBLE: NULL if i%13==12; else (i%12+2)/4
//   cities (ct_)   6 rows, groups of 4
//     ct_id        SMALLINT: 1..6 (city 5 is referenced only by zones, city 6 by nothing)
//     label        VARCHAR: center harbor hills airport 'old town' outskirts
//     ct_pop       INTEGER: 120000 45000 30000 NULL 8000 2500
//   shifts (sh_)   500 rows, groups of 128: the two-column key (sh_driver, sh_day). Rows 498 and
//                  499 repeat the keys of rows 3 and 4 (src = i>=498 ? i-495 : i), the only 2
//                  repeated pairs.
//     sh_driver    INTEGER -> drivers.dv_id: NULL if src%89==5; else 1000+37*(src%90)
//     sh_day       DATE: NULL if src%97==13; else 2026-03-01 plus (7*(src%90)+11*(src/90))%30 days
//     sh_hours     DECIMAL(4,1): NULL if i%23==7; else unscaled 40+5i%81, 4.0 to 12.0
//   tariffs (tf_)  9 rows, groups of 4
//     tf_code      VARCHAR: STD NIGHT AIRPORT Std '' 'XL ' Ж-1 NULL POOL (case, the empty string,
//                  a trailing space, UTF-8, NULL)
//     tf_rate      DECIMAL(5,3): 1.000 1.500 2.000 1.250 2.125 2.500 1.750 NULL 0.875 (2.125 and
//                  0.875 have no 2-digit twin; 0.875 rounds to 0.88)
//     tf_label     VARCHAR: standard night airport standard center xl center unknown pool
//   promos (pm_)   0 rows (one empty row group): pm_id INTEGER, pm_code VARCHAR,
//                  pm_discount DECIMAL(4,2)
//
// Shapes for the join tests: keys of two types (INTEGER to BIGINT, DECIMAL(4,2) to DECIMAL(5,3)),
// a two-column key (shifts), a join cycle (trips, zones, cities, drivers), a many-to-many shortcut
// (zn_city = dv_city), a role-playing dimension (zones) and DOUBLE columns in two tables.

namespace antb1::fixturegen {

struct StarTable {
  std::string name;    // the tables.txt name; written to star/<name>.parquet
  std::string prefix;  // of every column name but `label`, e.g. "tr_"
  std::shared_ptr<arrow::Table> table;
  int64_t row_group_rows = 0;  // fewer than the rows of every non-empty table
};

// trips, riders, drivers, zones, cities, shifts, tariffs, promos: the order of tables.txt.
arrow::Result<std::vector<StarTable>> MakeStarTables();

}  // namespace antb1::fixturegen
