#include "fixture_digest.h"

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <format>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>
#include <vector>

#include <arrow/api.h>
#include <arrow/io/file.h>
#include <parquet/arrow/reader.h>
#include <parquet/exception.h>
#include <parquet/file_reader.h>
#include <parquet/metadata.h>
#include <parquet/schema.h>
#include <parquet/types.h>

#include "sha256.h"

namespace antb1::harness {
namespace {

namespace fs = std::filesystem;

// Buffers small pieces before they reach the SHA-256 (one Update per megabyte).
class Hasher {
 public:
  void Add(std::string_view piece) {
    buffer_ += piece;
    if (buffer_.size() >= kFlushBytes) {
      Flush();
    }
  }
  std::string Finish() {
    Flush();
    return sha_.HexDigest();
  }

 private:
  static constexpr std::size_t kFlushBytes = std::size_t{1} << 20U;

  void Flush() {
    sha_.Update(buffer_);
    buffer_.clear();
  }

  slt::Sha256 sha_;
  std::string buffer_;
};

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

// Appends the decimal (or, for base 16, hex) text of an integer and a terminator.
template <class T>
void AddNumber(Hasher& h, T value, char prefix, char terminator, int base = 10) {
  std::array<char, 32> buf{};
  buf[0] = prefix;
  const auto [end, ec] = std::to_chars(buf.data() + 1, buf.data() + buf.size() - 1, value, base);
  *end = terminator;
  h.Add(std::string_view(buf.data(), static_cast<std::size_t>(end + 1 - buf.data())));
}

template <class ArrayType>
void HashValues(const arrow::Array& array, Hasher& h) {
  const auto& typed = static_cast<const ArrayType&>(array);
  for (int64_t i = 0; i < typed.length(); ++i) {
    if (typed.IsNull(i)) {
      h.Add("N;");
      continue;
    }
    if constexpr (std::is_same_v<ArrayType, arrow::DoubleArray>) {
      AddNumber(h, std::bit_cast<uint64_t>(typed.Value(i)), 'd', ';', 16);
    } else if constexpr (std::is_same_v<ArrayType, arrow::FloatArray>) {
      AddNumber(h, std::bit_cast<uint32_t>(typed.Value(i)), 'f', ';', 16);
    } else if constexpr (std::is_base_of_v<arrow::BinaryArray, ArrayType>) {
      const std::string_view v = typed.GetView(i);
      AddNumber(h, v.size(), 'b', ':');
      h.Add(v);
      h.Add(";");
    } else {
      AddNumber(h, typed.Value(i), 'i', ';');
    }
  }
}

arrow::Status HashArray(const arrow::Array& array, Hasher& h) {
  switch (array.type_id()) {
    case arrow::Type::INT8:
      HashValues<arrow::Int8Array>(array, h);
      break;
    case arrow::Type::UINT8:
      HashValues<arrow::UInt8Array>(array, h);
      break;
    case arrow::Type::INT16:
      HashValues<arrow::Int16Array>(array, h);
      break;
    case arrow::Type::UINT16:
      HashValues<arrow::UInt16Array>(array, h);
      break;
    case arrow::Type::INT32:
      HashValues<arrow::Int32Array>(array, h);
      break;
    case arrow::Type::UINT32:
      HashValues<arrow::UInt32Array>(array, h);
      break;
    case arrow::Type::INT64:
      HashValues<arrow::Int64Array>(array, h);
      break;
    case arrow::Type::UINT64:
      HashValues<arrow::UInt64Array>(array, h);
      break;
    case arrow::Type::DATE32:
      HashValues<arrow::Date32Array>(array, h);
      break;
    case arrow::Type::FLOAT:
      HashValues<arrow::FloatArray>(array, h);
      break;
    case arrow::Type::DOUBLE:
      HashValues<arrow::DoubleArray>(array, h);
      break;
    case arrow::Type::BINARY:
      HashValues<arrow::BinaryArray>(array, h);
      break;
    case arrow::Type::STRING:
      HashValues<arrow::StringArray>(array, h);
      break;
    default:
      return arrow::Status::NotImplemented("fixture digest: no canonical encoding for ",
                                           array.type()->ToString());
  }
  return arrow::Status::OK();
}

std::string SchemaSha256(const parquet::SchemaDescriptor& schema) {
  Hasher h;
  for (int i = 0; i < schema.num_columns(); ++i) {
    const parquet::ColumnDescriptor* c = schema.Column(i);
    h.Add(std::format("{}|{}|{}|{}|{}|{}|{}\n", c->path()->ToDotString(),
                      parquet::TypeToString(c->physical_type()), c->logical_type()->ToString(),
                      parquet::ConvertedTypeToString(c->converted_type()),
                      RepetitionName(c->schema_node()->repetition()), c->max_definition_level(),
                      c->max_repetition_level()));
  }
  return h.Finish();
}

arrow::Result<FileDigest> DigestOpenFile(const fs::path& file, std::string relative_path) {
  ARROW_ASSIGN_OR_RAISE(auto input, arrow::io::ReadableFile::Open(file.string()));
  ARROW_ASSIGN_OR_RAISE(auto reader, parquet::arrow::OpenFile(input, arrow::default_memory_pool()));
  const std::shared_ptr<parquet::FileMetaData> metadata = reader->parquet_reader()->metadata();
  FileDigest d;
  d.path = std::move(relative_path);
  d.rows = metadata->num_rows();
  d.schema_sha256 = SchemaSha256(*metadata->schema());
  Hasher data;
  for (int g = 0; g < metadata->num_row_groups(); ++g) {
    d.row_groups.push_back(metadata->RowGroup(g)->num_rows());
    ARROW_ASSIGN_OR_RAISE(auto table, reader->ReadRowGroup(g));
    data.Add(std::format("rg {} {}\n", g, table->num_rows()));
    for (int c = 0; c < table->num_columns(); ++c) {
      data.Add(std::format("col {}\n", c));
      for (const auto& chunk : table->column(c)->chunks()) {
        ARROW_RETURN_NOT_OK(HashArray(*chunk, data));
      }
    }
  }
  d.data_sha256 = data.Finish();
  return d;
}

std::map<std::string, std::string, std::less<>> LinesByPath(const std::string& text) {
  std::map<std::string, std::string, std::less<>> lines;
  std::istringstream in(text);
  for (std::string line; std::getline(in, line);) {
    if (line.empty() || line.starts_with('#')) {
      continue;
    }
    lines.emplace(line.substr(0, line.find(' ')), line);
  }
  return lines;
}

}  // namespace

std::string FileDigest::Line() const {
  std::string groups;
  for (const int64_t g : row_groups) {
    groups += std::format("{}{}", groups.empty() ? "" : ",", g);
  }
  return std::format("{} rows={} row_groups={} schema={} data={}", path, rows, groups,
                     schema_sha256, data_sha256);
}

arrow::Result<FileDigest> DigestFile(const fs::path& file, std::string relative_path) {
  try {
    return DigestOpenFile(file, std::move(relative_path));
  } catch (const std::exception& e) {
    return arrow::Status::IOError("cannot read Parquet file '", file.string(), "': ", e.what());
  }
}

arrow::Result<std::vector<FileDigest>> DigestDirectory(const fs::path& dir) {
  std::vector<std::pair<std::string, fs::path>> files;
  std::error_code ec;
  for (fs::recursive_directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
    if (it->is_regular_file(ec) && it->path().extension() == ".parquet") {
      files.emplace_back(it->path().lexically_relative(dir).generic_string(), it->path());
    }
  }
  if (ec) {
    return arrow::Status::IOError("cannot list '", dir.string(), "': ", ec.message());
  }
  if (files.empty()) {
    return arrow::Status::IOError("no Parquet files in '", dir.string(),
                                  "' (the fixtures.generate test writes them)");
  }
  std::ranges::sort(files);
  std::vector<FileDigest> digests;
  for (const auto& [relative, path] : files) {
    ARROW_ASSIGN_OR_RAISE(auto d, DigestFile(path, relative));
    digests.push_back(std::move(d));
  }
  return digests;
}

std::string DigestText(const std::vector<FileDigest>& digests) {
  std::string text =
      "# Logical digest of the Parquet fixtures of tools/fixturegen (harness.fixtures.digest; see\n"
      "# tests/harness/fixture_digest.h). Every platform must generate exactly this. After an\n"
      "# intended generator change, rewrite it and commit it with the change:\n"
      "#   build/dev/bin/antb1-fixture-digest --write tests/fixtures/fixtures.digest "
      "build/dev/fixtures\n";
  for (const auto& d : digests) {
    text += d.Line() + "\n";
  }
  return text;
}

std::vector<std::string> CompareDigests(const std::string& expected_text,
                                        const std::vector<FileDigest>& actual) {
  const auto expected = LinesByPath(expected_text);
  std::map<std::string, std::string, std::less<>> got;
  for (const auto& d : actual) {
    got.emplace(d.path, d.Line());
  }
  std::vector<std::string> diffs;
  for (const auto& [path, line] : expected) {
    const auto it = got.find(path);
    if (it == got.end()) {
      diffs.push_back(std::format("missing fixture {}", path));
    } else if (it->second != line) {
      diffs.push_back(
          std::format("{} differs:\n    expected {}\n    actual   {}", path, line, it->second));
    }
  }
  for (const auto& [path, line] : got) {
    if (!expected.contains(path)) {
      diffs.push_back(std::format("unexpected fixture {}", line));
    }
  }
  return diffs;
}

}  // namespace antb1::harness
