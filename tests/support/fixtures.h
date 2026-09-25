#pragma once

#include <filesystem>

// Paths for gtest suites that read the Parquet fixtures (tools/fixturegen). Their tests are
// registered with antb1_add_fixture_gtest() (cmake/Antb1Testing.cmake), which requires the ctest
// fixture antb1_fixtures (the fixtures.generate test writes the files first) and compiles in:
//   ANTB1_TEST_FIXTURES_DIR  the fixtures directory of this build tree (build/<preset>/fixtures)
//   ANTB1_TEST_SOURCE_DIR    the source tree

namespace antb1::testing {

inline std::filesystem::path FixturesDir() { return ANTB1_TEST_FIXTURES_DIR; }

inline std::filesystem::path SourceDir() { return ANTB1_TEST_SOURCE_DIR; }

// tests/slt/tables.txt: the fixture tables (name, files, options) shared by every suite.
inline std::filesystem::path SltTablesFile() {
  return SourceDir() / "tests" / "slt" / "tables.txt";
}

}  // namespace antb1::testing
