// Corrupt, missing and mismatched Parquet inputs (label integration): every one is an IOError that
// names the file, both through Session::RegisterParquet and through FROM '<path>', and the session
// stays usable afterwards. The CLI maps IOError to exit code 3 (tests/cli).

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <initializer_list>
#include <iterator>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <arrow/api.h>
#include <arrow/io/file.h>
#include <gtest/gtest.h>
#include <parquet/arrow/writer.h>
#include <parquet/properties.h>

#include "antb1/engine/session.h"

#include "integration_util.h"

namespace antb1::integration {
namespace {

namespace fs = std::filesystem;

// The bytes `codes`, each a number below 256 or a character.
std::string Bytes(std::initializer_list<unsigned char> codes) {
  return {codes.begin(), codes.end()};
}

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

  // A Parquet file of one column `c` holding `values`, uncompressed and without an ARROW:schema, so
  // that the Parquet schema alone gives its type.
  std::string WriteColumn(const std::string& name, const std::shared_ptr<arrow::Array>& values) {
    const auto table =
        arrow::Table::Make(arrow::schema({arrow::field("c", values->type())}), {values});
    const fs::path path = dir_ / name;
    auto out = arrow::io::FileOutputStream::Open(path.string());
    EXPECT_TRUE(out.ok()) << out.status().ToString();
    const auto properties = parquet::WriterProperties::Builder()
                                .compression(parquet::Compression::UNCOMPRESSED)
                                ->build();
    const arrow::Status written =
        parquet::arrow::WriteTable(*table, arrow::default_memory_pool(), *out, 1024, properties);
    EXPECT_TRUE(written.ok()) << written.ToString();
    EXPECT_TRUE((*out)->Close().ok());
    return path.string();
  }

  // An array of `values` of the builder's type.
  template <typename Builder>
  static std::shared_ptr<arrow::Array> ArrayOf(
      const std::vector<typename Builder::value_type>& values) {
    Builder builder;
    EXPECT_TRUE(builder.AppendValues(values).ok());
    return builder.Finish().ValueOrDie();
  }

  // Replaces the one occurrence of `from` in the file at `path` by `to`, of the same length.
  static void Patch(const std::string& path, std::string_view from, std::string_view to) {
    ASSERT_EQ(from.size(), to.size());
    std::string bytes = ReadBytes(path);
    const std::size_t at = bytes.find(from);
    ASSERT_NE(at, std::string::npos);
    ASSERT_EQ(bytes.find(from, at + 1), std::string::npos) << "more than one occurrence";
    bytes.replace(at, to.size(), to);
    std::ofstream(path, std::ios::binary | std::ios::trunc) << bytes;
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

// A Parquet schema nested deeper than Arrow's limit of 100 levels (apache/arrow GH-51238, Arrow 26)
// is an IOError when the file is registered, where a shallow one registers with its nested column
// unsupported.
TEST_F(ParquetErrors, SchemaNestedTooDeep) {
  const auto nested = [](int levels) {
    std::shared_ptr<arrow::Array> column = ArrayOf<arrow::Int32Builder>({1, 2});
    for (int level = 0; level < levels; ++level) {
      column = arrow::StructArray::Make({column}, {arrow::field("s", column->type())}).ValueOrDie();
    }
    return column;
  };
  ASSERT_TRUE(
      session_->RegisterParquet("shallow", {WriteColumn("shallow.parquet", nested(3))}).ok());
  const std::string deep = WriteColumn("deep.parquet", nested(120));
  ExpectIoError({deep}, deep);
  const auto registered = session_->RegisterParquet("deep", {deep});
  EXPECT_NE(registered.message().find("too deeply nested"), std::string::npos)
      << registered.ToString();
}

// A column chunk whose compression codec Parquet does not define is an IOError when the scan reads
// it (apache/arrow GH-53518, Arrow 26), where Arrow 25 read the chunk as if it were uncompressed.
// The footer is Thrift's compact encoding: ColumnMetaData's path_in_schema (field 3, a list of one
// string, "c") is followed by its codec (field 4, an i32): 0x15, then the zigzag varint of the
// codec, 0x00 for UNCOMPRESSED, 0x10 for 8, one past LZ4_RAW.
TEST_F(ParquetErrors, UnknownCompressionCodec) {
  const std::string path = WriteColumn("codec.parquet", ArrayOf<arrow::Int32Builder>({1, 2, 3}));
  ASSERT_NO_FATAL_FAILURE(Patch(path, Bytes({0x19, 0x18, 0x01, 'c', 0x15, 0x00}),
                                Bytes({0x19, 0x18, 0x01, 'c', 0x15, 0x10})));
  ASSERT_TRUE(session_->RegisterParquet("t", {path}).ok());
  const auto read = Count(*session_, "SELECT COUNT(*) FROM t WHERE c = 2");
  EXPECT_TRUE(read.status().IsIOError()) << read.status().ToString();
  EXPECT_NE(read.status().message().find(path), std::string::npos) << read.status().ToString();
}

// A logical type that does not fit its physical type is dropped (apache/arrow GH-50910, Arrow 26),
// where Arrow 25 refused the file: the column reads by its converted type. The column is a DATE:
// SchemaElement's name (field 4, "c") is followed by its converted_type (field 6: 0x25, then 0x0c
// for DATE) and its logicalType (field 10: 0x4c, then the union's member, 0x6c for field 6, DATE).
// The patch writes INT_32 (17, zigzag 0x22) and a STRING logical type (0x1c, field 1), which no
// INT32 column can have.
TEST_F(ParquetErrors, LogicalTypeThatDoesNotFitItsColumn) {
  const std::string path = WriteColumn("logical.parquet", ArrayOf<arrow::Date32Builder>({1, 2, 3}));
  ASSERT_NO_FATAL_FAILURE(Patch(path, Bytes({0x18, 0x01, 'c', 0x25, 0x0c, 0x4c, 0x6c}),
                                Bytes({0x18, 0x01, 'c', 0x25, 0x22, 0x4c, 0x1c})));
  ASSERT_TRUE(session_->RegisterParquet("t", {path}).ok());
  const auto read = Count(*session_, "SELECT COUNT(*) FROM t WHERE c = 2");
  ASSERT_TRUE(read.ok()) << read.status().ToString();
  EXPECT_EQ(*read, 1);
}

}  // namespace
}  // namespace antb1::integration
