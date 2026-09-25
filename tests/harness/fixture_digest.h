#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include <arrow/result.h>

// The logical digest of Parquet files (harness.fixtures.digest, tests/fixtures/fixtures.digest).
//
// One line per file:
//   <relative path> rows=<n> row_groups=<rows of each group> schema=<sha256> data=<sha256>
//   schema  hashes the Parquet schema: per column its path, physical, logical and converted type,
//           repetition and definition/repetition levels
//   data    hashes every value of every row group, read with the Parquet library, in a canonical
//           encoding: integers in decimal, floating point as IEEE bits, byte arrays
//           length-prefixed, NULL marked
// Compression, encodings, page layout and the writer's version string do not enter it, so every
// platform must produce the same digest for the same generator: the committed digest pins what
// tools/fixturegen writes on Linux and macOS alike.

namespace antb1::harness {

struct FileDigest {
  std::string path;  // relative to the digested directory, '/' separated
  int64_t rows = 0;
  std::vector<int64_t> row_groups;  // rows of each row group
  std::string schema_sha256;
  std::string data_sha256;

  [[nodiscard]] std::string Line() const;
};

arrow::Result<FileDigest> DigestFile(const std::filesystem::path& file, std::string relative_path);

// Every *.parquet file under dir (recursively), sorted by relative path.
arrow::Result<std::vector<FileDigest>> DigestDirectory(const std::filesystem::path& dir);

// The digest file text: a comment header, then one line per file.
std::string DigestText(const std::vector<FileDigest>& digests);

// Differences between an expected digest text and the actual digests (comment and blank lines
// ignored); empty if they agree.
std::vector<std::string> CompareDigests(const std::string& expected_text,
                                        const std::vector<FileDigest>& actual);

}  // namespace antb1::harness
