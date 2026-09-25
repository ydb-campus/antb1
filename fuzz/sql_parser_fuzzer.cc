// antb1-sql-parser-fuzzer: libFuzzer target for the SQL round-trip property
// (sql_parser_property.h). Built only with ANTB1_BUILD_FUZZERS=ON (Clang; preset `fuzz`).
//   pixi run fuzz-smoke  short deterministic run (ctest fuzz.sql_parser.smoke)
//   pixi run fuzz        long run
// Crash inputs land in build/fuzz/artifacts/ (fuzz/regressions/README.md).

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "sql_parser_property.h"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  // libFuzzer passes raw bytes; the parser reads them as (possibly invalid) UTF-8 text.
  antb1::fuzz::CheckSqlParserProperty(std::string_view(reinterpret_cast<const char*>(data), size));
  return 0;
}
