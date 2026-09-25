// Corrupt, missing and mismatched Parquet inputs (label integration): every one is an IOError that
// names the file, both through Session::RegisterParquet and through FROM '<path>', and the session
// stays usable afterwards. The CLI maps IOError to exit code 3 (tests/cli).

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

#include <arrow/api.h>
#include <gtest/gtest.h>

#include "antb1/engine/session.h"

#include "integration_util.h"

namespace antb1::integration {
namespace {

namespace fs = std::filesystem;

std::string ReadBytes(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(in), {}};
}

class ParquetErrors : public ::testing::Test {
 protected:
  void SetUp() override {
    dir_ = fs::path(::testing::TempDir()) / "antb1_integration" /
           ::testing::UnitTest::GetInstance()->current_test_info()->name();
    std::error_code ec;
    fs::remove_all(dir_, ec);
    fs::create_directories(dir_);
    session_ = NewSession();
    ASSERT_NE(session_, nullptr);
    ASSERT_TRUE(session_->RegisterParquet("edge", {Fixture("edge.parquet")}).ok());
  }
  void TearDown() override {
    std::error_code ec;
    fs::remove_all(dir_, ec);
  }

  // Writes `bytes` to <dir>/<name> and returns the path.
  std::string Write(const std::string& name, const std::string& bytes) {
    const fs::path path = dir_ / name;
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << bytes;
    return path.string();
  }

  // A copy of the edge fixture with `edit` applied to its bytes.
  std::string EditedEdge(const std::string& name, const std::function<void(std::string&)>& edit) {
    std::string bytes = ReadBytes(Fixture("edge.parquet"));
    EXPECT_GT(bytes.size(), 16U);
    edit(bytes);
    return Write(name, bytes);
  }

  // Both ways of reading `paths` fail with an IOError that names `mention`.
  void ExpectIoError(const std::vector<std::string>& paths, const std::string& mention) {
    const auto registered = session_->RegisterParquet("t", paths);
    EXPECT_TRUE(registered.IsIOError()) << registered.ToString();
    EXPECT_NE(registered.message().find(mention), std::string::npos) << registered.ToString();
    if (paths.size() == 1) {
      const auto queried = session_->Execute(FromPath(paths[0])).status();
      EXPECT_TRUE(queried.IsIOError()) << queried.ToString();
      EXPECT_NE(queried.message().find(mention), std::string::npos) << queried.ToString();
    }
    auto count = Count(*session_, "SELECT COUNT(*) FROM edge");  // still usable
    ASSERT_TRUE(count.ok()) << count.status().ToString();
    EXPECT_EQ(*count, 12);
  }

  fs::path dir_;
  std::unique_ptr<engine::Session> session_;
};

TEST_F(ParquetErrors, MissingFile) {
  const std::string path = (dir_ / "missing.parquet").string();
  ExpectIoError({path}, path);
}

TEST_F(ParquetErrors, GlobWithoutMatches) {
  const std::string pattern = (dir_ / "none-*.parquet").string();
  ExpectIoError({pattern}, "no files match");
}

TEST_F(ParquetErrors, EmptyFile) {
  const std::string path = Write("empty.parquet", "");
  ExpectIoError({path}, path);
}

TEST_F(ParquetErrors, NotParquet) {
  const std::string path = Write("text.parquet", "SELECT COUNT(*) FROM t\n");
  ExpectIoError({path}, path);
}

TEST_F(ParquetErrors, OnlyMagicBytes) {
  const std::string path = Write("magic.parquet", "PAR1PAR1");
  ExpectIoError({path}, path);
}

TEST_F(ParquetErrors, TruncatedFile) {
  const std::string path =
      EditedEdge("truncated.parquet", [](std::string& b) { b.resize(b.size() / 2); });
  ExpectIoError({path}, path);
}

TEST_F(ParquetErrors, BadTrailingMagic) {
  const std::string path =
      EditedEdge("magic_end.parquet", [](std::string& b) { b.replace(b.size() - 4, 4, "XXXX"); });
  ExpectIoError({path}, path);
}

TEST_F(ParquetErrors, FooterLengthBeyondTheFile) {
  // The 4 bytes before the trailing magic are the little-endian footer length.
  const std::string path = EditedEdge("footer_length.parquet", [](std::string& b) {
    b.replace(b.size() - 8, 4, std::string("\xff\xff\xff\x7f", 4));
  });
  ExpectIoError({path}, path);
}

TEST_F(ParquetErrors, CorruptFooterMetadata) {
  // Overwrite the thrift-encoded footer (it precedes the length and the magic) with garbage.
  const std::string path = EditedEdge("footer.parquet", [](std::string& b) {
    const auto length =
        static_cast<std::size_t>(static_cast<unsigned char>(b[b.size() - 8])) |
        (static_cast<std::size_t>(static_cast<unsigned char>(b[b.size() - 7])) << 8U);
    const std::size_t start = b.size() - 8 - length;
    for (std::size_t i = start; i < b.size() - 8; ++i) {
      b[i] = static_cast<char>(0xEE);
    }
  });
  ExpectIoError({path}, path);
}

TEST_F(ParquetErrors, DirectoryInsteadOfFile) {
  const fs::path sub = dir_ / "dir.parquet";
  fs::create_directories(sub);
  ExpectIoError({sub.string()}, sub.string());
}

TEST_F(ParquetErrors, MismatchedSchemas) {
  ExpectIoError({Fixture("hits_like.parquet"), Fixture("edge.parquet")}, "differs");
}

TEST_F(ParquetErrors, MismatchedRepetitionAndStringAnnotation) {
  // hits_like_required has the same column names but REQUIRED columns and UTF8 strings.
  ExpectIoError({Fixture("hits_like.parquet"), Fixture("hits_like_required.parquet")}, "differs");
}

TEST_F(ParquetErrors, GlobThatIncludesACorruptFile) {
  fs::copy_file(Fixture("edge.parquet"), dir_ / "part-0.parquet");
  const std::string corrupt = Write("part-1.parquet", "not parquet");
  ExpectIoError({(dir_ / "part-*.parquet").string()}, corrupt);
}

TEST_F(ParquetErrors, GlobThatMixesSchemas) {
  fs::copy_file(Fixture("edge.parquet"), dir_ / "part-0.parquet");
  fs::copy_file(Fixture("empty.parquet"), dir_ / "part-1.parquet");
  ExpectIoError({(dir_ / "part-*.parquet").string()}, "differs");
}

}  // namespace
}  // namespace antb1::integration
