#include "duckdb_engine.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <duckdb.h>

#include "antb1/common/int128.h"

#include "canonical.h"
#include "engine.h"
#include "tables.h"

namespace antb1::slt {

struct DuckDbEngine::Handles {
  Handles() = default;
  Handles(const Handles&) = delete;
  Handles& operator=(const Handles&) = delete;
  Handles(Handles&&) = delete;
  Handles& operator=(Handles&&) = delete;
  ~Handles() {
    if (conn != nullptr) {
      duckdb_disconnect(&conn);
    }
    if (db != nullptr) {
      duckdb_close(&db);
    }
  }

  duckdb_database db = nullptr;
  duckdb_connection conn = nullptr;
};

namespace {

namespace fs = std::filesystem;

// duckdb_destroy_result must run for every duckdb_query, also a failed one.
class QueryResult {
 public:
  QueryResult() = default;
  QueryResult(const QueryResult&) = delete;
  QueryResult& operator=(const QueryResult&) = delete;
  QueryResult(QueryResult&&) = delete;
  QueryResult& operator=(QueryResult&&) = delete;
  ~QueryResult() { duckdb_destroy_result(&result_); }

  duckdb_result* get() { return &result_; }

 private:
  duckdb_result result_{};
};

class DataChunk {
 public:
  explicit DataChunk(duckdb_data_chunk chunk) : chunk_(chunk) {}
  DataChunk(const DataChunk&) = delete;
  DataChunk& operator=(const DataChunk&) = delete;
  DataChunk(DataChunk&&) = delete;
  DataChunk& operator=(DataChunk&&) = delete;
  ~DataChunk() {
    if (chunk_ != nullptr) {
      duckdb_destroy_data_chunk(&chunk_);
    }
  }

  [[nodiscard]] duckdb_data_chunk get() const { return chunk_; }

 private:
  duckdb_data_chunk chunk_;
};

struct ColumnReader {
  duckdb_type type = DUCKDB_TYPE_INVALID;
  duckdb_type storage = DUCKDB_TYPE_INVALID;  // DECIMAL: the physical integer type
  uint8_t scale = 0;                          // DECIMAL
  ColumnClass cls = ColumnClass::kText;
  std::string type_name;
};

std::string_view TypeName(duckdb_type type) {
  switch (type) {
    case DUCKDB_TYPE_BOOLEAN:
      return "BOOLEAN";
    case DUCKDB_TYPE_TINYINT:
      return "TINYINT";
    case DUCKDB_TYPE_SMALLINT:
      return "SMALLINT";
    case DUCKDB_TYPE_INTEGER:
      return "INTEGER";
    case DUCKDB_TYPE_BIGINT:
      return "BIGINT";
    case DUCKDB_TYPE_UTINYINT:
      return "UTINYINT";
    case DUCKDB_TYPE_USMALLINT:
      return "USMALLINT";
    case DUCKDB_TYPE_UINTEGER:
      return "UINTEGER";
    case DUCKDB_TYPE_UBIGINT:
      return "UBIGINT";
    case DUCKDB_TYPE_HUGEINT:
      return "HUGEINT";
    case DUCKDB_TYPE_UHUGEINT:
      return "UHUGEINT";
    case DUCKDB_TYPE_FLOAT:
      return "FLOAT";
    case DUCKDB_TYPE_DOUBLE:
      return "DOUBLE";
    case DUCKDB_TYPE_DECIMAL:
      return "DECIMAL";
    case DUCKDB_TYPE_DATE:
      return "DATE";
    case DUCKDB_TYPE_VARCHAR:
      return "VARCHAR";
    case DUCKDB_TYPE_BLOB:
      return "BLOB";
    default:
      return "";
  }
}

std::optional<ColumnReader> MakeReader(duckdb_result* result, idx_t column) {
  duckdb_logical_type logical = duckdb_column_logical_type(result, column);
  ColumnReader reader;
  reader.type = duckdb_get_type_id(logical);
  if (reader.type == DUCKDB_TYPE_DECIMAL) {
    reader.scale = duckdb_decimal_scale(logical);
    reader.storage = duckdb_decimal_internal_type(logical);
  }
  duckdb_destroy_logical_type(&logical);
  reader.type_name = std::string(TypeName(reader.type));
  if (reader.type_name.empty()) {
    return std::nullopt;
  }
  switch (reader.type) {
    case DUCKDB_TYPE_FLOAT:
    case DUCKDB_TYPE_DOUBLE:
      reader.cls = ColumnClass::kReal;
      break;
    case DUCKDB_TYPE_DECIMAL:
      reader.cls = reader.scale > 0 ? ColumnClass::kReal : ColumnClass::kInteger;
      break;
    case DUCKDB_TYPE_BOOLEAN:
    case DUCKDB_TYPE_DATE:
    case DUCKDB_TYPE_VARCHAR:
    case DUCKDB_TYPE_BLOB:
      reader.cls = ColumnClass::kText;
      break;
    default:
      reader.cls = ColumnClass::kInteger;
      break;
  }
  return reader;
}

std::string UInt128Text(UInt128 value) {
  if (value == 0) {
    return "0";
  }
  std::string digits;
  while (value > 0) {
    digits += static_cast<char>('0' + static_cast<int>(value % 10U));
    value /= 10U;
  }
  std::ranges::reverse(digits);
  return digits;
}

Int128 FromHugeint(duckdb_hugeint v) {
  return static_cast<Int128>((static_cast<UInt128>(static_cast<uint64_t>(v.upper)) << 64U) |
                             v.lower);
}

// Exact decimal text of value / 10^scale, then the canonical double (R columns compare
// numerically).
std::string DecimalText(Int128 value, uint8_t scale) {
  if (scale == 0) {
    return Int128ToString(value);
  }
  std::string digits = Int128ToString(value);
  const bool negative = digits.starts_with('-');
  if (negative) {
    digits.erase(0, 1);
  }
  if (digits.size() <= scale) {
    digits.insert(0, scale + 1 - digits.size(), '0');
  }
  digits.insert(digits.size() - scale, ".");
  const std::string text = (negative ? "-" : "") + digits;
  double parsed = 0;
  std::from_chars(text.data(), text.data() + text.size(), parsed);
  return CanonicalDouble(parsed);
}

template <class T>
T At(void* data, idx_t row) {
  return static_cast<const T*>(data)[row];
}

std::string ReadValue(const ColumnReader& reader, void* data, idx_t row) {
  switch (reader.type) {
    case DUCKDB_TYPE_BOOLEAN:
      return At<bool>(data, row) ? "true" : "false";
    case DUCKDB_TYPE_TINYINT:
      return std::to_string(At<int8_t>(data, row));
    case DUCKDB_TYPE_SMALLINT:
      return std::to_string(At<int16_t>(data, row));
    case DUCKDB_TYPE_INTEGER:
      return std::to_string(At<int32_t>(data, row));
    case DUCKDB_TYPE_BIGINT:
      return std::to_string(At<int64_t>(data, row));
    case DUCKDB_TYPE_UTINYINT:
      return std::to_string(At<uint8_t>(data, row));
    case DUCKDB_TYPE_USMALLINT:
      return std::to_string(At<uint16_t>(data, row));
    case DUCKDB_TYPE_UINTEGER:
      return std::to_string(At<uint32_t>(data, row));
    case DUCKDB_TYPE_UBIGINT:
      return std::to_string(At<uint64_t>(data, row));
    case DUCKDB_TYPE_HUGEINT:
      return Int128ToString(FromHugeint(At<duckdb_hugeint>(data, row)));
    case DUCKDB_TYPE_UHUGEINT: {
      const auto v = At<duckdb_uhugeint>(data, row);
      return UInt128Text((static_cast<UInt128>(v.upper) << 64U) | v.lower);
    }
    case DUCKDB_TYPE_FLOAT:
      return CanonicalDouble(static_cast<double>(At<float>(data, row)));
    case DUCKDB_TYPE_DOUBLE:
      return CanonicalDouble(At<double>(data, row));
    case DUCKDB_TYPE_DATE:
      return CanonicalDate(At<duckdb_date>(data, row).days);
    case DUCKDB_TYPE_VARCHAR:
    case DUCKDB_TYPE_BLOB: {
      auto* strings = static_cast<duckdb_string_t*>(data);
      return {duckdb_string_t_data(&strings[row]), duckdb_string_t_length(strings[row])};
    }
    case DUCKDB_TYPE_DECIMAL:
      switch (reader.storage) {
        case DUCKDB_TYPE_SMALLINT:
          return DecimalText(At<int16_t>(data, row), reader.scale);
        case DUCKDB_TYPE_INTEGER:
          return DecimalText(At<int32_t>(data, row), reader.scale);
        case DUCKDB_TYPE_BIGINT:
          return DecimalText(At<int64_t>(data, row), reader.scale);
        default:
          return DecimalText(FromHugeint(At<duckdb_hugeint>(data, row)), reader.scale);
      }
    default:
      return "?";
  }
}

std::string SqlString(std::string_view s) {
  std::string out = "'";
  for (const char c : s) {
    out += c;
    if (c == '\'') {
      out += '\'';
    }
  }
  return out + "'";
}

// An EngineError from DuckDB's error text. The kind is printed in redacted reports: only a
// "<Type> Error" prefix, never message text.
EngineError MakeError(std::string message, bool internal) {
  EngineError error;
  error.message = std::move(message);
  const std::size_t colon = error.message.find(':');
  const std::string prefix = error.message.substr(0, colon);
  const bool is_type = colon != std::string::npos && prefix.size() <= 40 &&
                       prefix.ends_with(" Error") && std::ranges::all_of(prefix, [](char c) {
                         return c == ' ' || std::isalpha(static_cast<unsigned char>(c)) != 0;
                       });
  error.kind = is_type ? prefix : "Error";
  error.internal = internal;
  return error;
}

EngineError ErrorOf(duckdb_result* result) {
  const char* text = duckdb_result_error(result);
  const duckdb_error_type type = duckdb_result_error_type(result);
  return MakeError(text != nullptr ? text : "unknown DuckDB error",
                   type == DUCKDB_ERROR_INTERNAL || type == DUCKDB_ERROR_FATAL);
}

// A failed prepare only has a message; its type is the "<Type> Error" prefix.
EngineError PrepareErrorOf(duckdb_prepared_statement prepared) {
  const char* text = duckdb_prepare_error(prepared);
  std::string message = text != nullptr ? text : "unknown DuckDB error";
  const bool internal = message.starts_with("INTERNAL Error") || message.starts_with("FATAL Error");
  return MakeError(std::move(message), internal);
}

// duckdb_destroy_extracted must run after every duckdb_extract_statements, also a failed one.
class ExtractedStatements {
 public:
  ExtractedStatements() = default;
  ExtractedStatements(const ExtractedStatements&) = delete;
  ExtractedStatements& operator=(const ExtractedStatements&) = delete;
  ExtractedStatements(ExtractedStatements&&) = delete;
  ExtractedStatements& operator=(ExtractedStatements&&) = delete;
  ~ExtractedStatements() {
    if (statements_ != nullptr) {
      duckdb_destroy_extracted(&statements_);
    }
  }

  duckdb_extracted_statements* out() { return &statements_; }
  [[nodiscard]] duckdb_extracted_statements get() const { return statements_; }

 private:
  duckdb_extracted_statements statements_ = nullptr;
};

// duckdb_destroy_prepare must run after every prepare, also a failed one.
class PreparedStatement {
 public:
  PreparedStatement() = default;
  PreparedStatement(const PreparedStatement&) = delete;
  PreparedStatement& operator=(const PreparedStatement&) = delete;
  PreparedStatement(PreparedStatement&&) = delete;
  PreparedStatement& operator=(PreparedStatement&&) = delete;
  ~PreparedStatement() {
    if (prepared_ != nullptr) {
      duckdb_destroy_prepare(&prepared_);
    }
  }

  duckdb_prepared_statement* out() { return &prepared_; }
  [[nodiscard]] duckdb_prepared_statement get() const { return prepared_; }

 private:
  duckdb_prepared_statement prepared_ = nullptr;
};

// The statement types the oracle runs for a record. Every other type is refused before it runs:
// COPY, ATTACH and EXPORT DATABASE could write files inside the allowed directories (next to the
// shared fixtures), and DDL or DML would change the oracle's state for the later records of a file.
// SET and LOAD (INSTALL, LOAD) stay allowed, so selftest/lockdown.slt sees DuckDB's own refusal:
// the configuration is locked and external access is off.
bool AllowedStatement(duckdb_statement_type type) {
  switch (type) {
    case DUCKDB_STATEMENT_TYPE_SELECT:
    case DUCKDB_STATEMENT_TYPE_EXPLAIN:
    case DUCKDB_STATEMENT_TYPE_SET:
    case DUCKDB_STATEMENT_TYPE_LOAD:
      return true;
    default:
      return false;
  }
}

std::string_view StatementTypeName(duckdb_statement_type type) {
  switch (type) {
    case DUCKDB_STATEMENT_TYPE_COPY:
    case DUCKDB_STATEMENT_TYPE_COPY_DATABASE:
      return "COPY";
    case DUCKDB_STATEMENT_TYPE_ATTACH:
      return "ATTACH";
    case DUCKDB_STATEMENT_TYPE_DETACH:
      return "DETACH";
    case DUCKDB_STATEMENT_TYPE_EXPORT:
      return "EXPORT";
    case DUCKDB_STATEMENT_TYPE_CREATE:
    case DUCKDB_STATEMENT_TYPE_CREATE_FUNC:
      return "CREATE";
    case DUCKDB_STATEMENT_TYPE_INSERT:
      return "INSERT";
    case DUCKDB_STATEMENT_TYPE_UPDATE:
      return "UPDATE";
    case DUCKDB_STATEMENT_TYPE_DELETE:
      return "DELETE";
    case DUCKDB_STATEMENT_TYPE_DROP:
      return "DROP";
    case DUCKDB_STATEMENT_TYPE_ALTER:
      return "ALTER";
    case DUCKDB_STATEMENT_TYPE_PRAGMA:
      return "PRAGMA";
    case DUCKDB_STATEMENT_TYPE_CALL:
      return "CALL";
    default:
      return "such";
  }
}

EngineError Refused(std::string_view what) {
  return MakeError(
      std::format(
          "Permission Error: the antb1-slt oracle does not run {} (only one SELECT, EXPLAIN, "
          "SET or LOAD statement per record: tests/slt/runner/duckdb_engine.cc)",
          what),
      /*internal=*/false);
}

ExecResult ReadResult(duckdb_result* result) {
  ResultSet out;
  std::vector<ColumnReader> readers;
  const idx_t columns = duckdb_column_count(result);
  for (idx_t c = 0; c < columns; ++c) {
    auto reader = MakeReader(result, c);
    if (!reader.has_value()) {
      return std::unexpected(EngineError{
          .kind = "harness",
          .message = std::format(
              "harness: column {} has a DuckDB type the canonical formatter does not support", c),
          .unsupported = false,
          .internal = true});
    }
    out.classes.push_back(reader->cls);
    out.type_names.push_back(reader->type_name);
    readers.push_back(std::move(*reader));
  }
  while (true) {
    const DataChunk chunk(duckdb_fetch_chunk(*result));
    if (chunk.get() == nullptr) {
      break;
    }
    const idx_t rows = duckdb_data_chunk_get_size(chunk.get());
    const std::size_t first = out.rows.size();
    out.rows.resize(first + rows);
    for (idx_t c = 0; c < columns; ++c) {
      duckdb_vector vector = duckdb_data_chunk_get_vector(chunk.get(), c);
      void* data = duckdb_vector_get_data(vector);
      uint64_t* validity = duckdb_vector_get_validity(vector);
      for (idx_t r = 0; r < rows; ++r) {
        auto& row = out.rows[first + r];
        if (validity != nullptr && !duckdb_validity_row_is_valid(validity, r)) {
          row.emplace_back(std::nullopt);
        } else {
          row.emplace_back(ReadValue(readers[c], data, r));
        }
      }
    }
  }
  return out;
}

// Runs `sql` without the statement check: the setup of the oracle, and SQL that does not parse.
ExecResult Query(duckdb_connection conn, const std::string& sql) {
  QueryResult result;
  if (duckdb_query(conn, sql.c_str(), result.get()) == DuckDBError) {
    return std::unexpected(ErrorOf(result.get()));
  }
  return ReadResult(result.get());
}

}  // namespace

DuckDbEngine::DuckDbEngine(std::unique_ptr<Handles> handles) : handles_(std::move(handles)) {}

DuckDbEngine::~DuckDbEngine() = default;

std::string DuckDbEngine::Version() { return duckdb_library_version(); }

std::expected<std::unique_ptr<DuckDbEngine>, std::string> DuckDbEngine::Make(
    const std::vector<TableDef>& tables, const fs::path& fixtures_dir, const fs::path& temp_dir) {
  std::error_code ec;
  fs::create_directories(temp_dir, ec);
  const std::string fixtures = fs::absolute(fixtures_dir, ec).lexically_normal().string();
  const std::string temp = fs::absolute(temp_dir, ec).lexically_normal().string();

  auto handles = std::make_unique<Handles>();
  duckdb_config config = nullptr;
  if (duckdb_create_config(&config) == DuckDBError) {
    return std::unexpected("duckdb: cannot create a config");
  }
  const std::array<std::pair<std::string, std::string>, 4> options = {{
      {"threads", "1"},
      {"autoinstall_known_extensions", "false"},
      {"autoload_known_extensions", "false"},
      {"temp_directory", temp},
  }};
  for (const auto& [key, value] : options) {
    if (duckdb_set_config(config, key.c_str(), value.c_str()) == DuckDBError) {
      duckdb_destroy_config(&config);
      return std::unexpected(std::format("duckdb: cannot set {}={}", key, value));
    }
  }
  char* open_error = nullptr;
  const duckdb_state opened = duckdb_open_ext(nullptr, &handles->db, config, &open_error);
  duckdb_destroy_config(&config);
  if (opened == DuckDBError) {
    std::string message = open_error != nullptr ? open_error : "unknown error";
    duckdb_free(open_error);
    return std::unexpected("duckdb: cannot open an in-memory database: " + message);
  }
  if (duckdb_connect(handles->db, &handles->conn) == DuckDBError) {
    return std::unexpected("duckdb: cannot connect");
  }
  std::vector<std::string> setup = {
      "SET binary_as_string = true",
      std::format("SET allowed_directories = [{}, {}]", SqlString(fixtures + "/"),
                  SqlString(temp + "/")),
      "SET enable_external_access = false",
  };
  for (const auto& t : tables) {
    std::string files;
    for (const auto& f : t.files) {
      files += (files.empty() ? "" : ", ") + SqlString(f);
    }
    setup.push_back(std::format(
        "CREATE VIEW \"{}\" AS SELECT * {}FROM read_parquet([{}], binary_as_string = true)", t.name,
        t.clickbench ? "REPLACE (make_date(EventDate) AS EventDate) " : "", files));
  }
  setup.emplace_back("SET lock_configuration = true");
  for (const auto& sql : setup) {
    if (auto result = Query(handles->conn, sql); !result) {
      return std::unexpected(
          std::format("duckdb setup failed: {}\n  {}", result.error().message, sql));
    }
  }
  return std::unique_ptr<DuckDbEngine>(new DuckDbEngine(std::move(handles)));
}

ExecResult DuckDbEngine::Execute(const std::string& sql) {
  ExtractedStatements statements;
  const idx_t count = duckdb_extract_statements(handles_->conn, sql.c_str(), statements.out());
  if (count == 0) {
    // A syntax error (or no statement): nothing can run, and the plain query reports the error
    // exactly as DuckDB does.
    return Query(handles_->conn, sql);
  }
  if (count > 1) {
    return std::unexpected(Refused(std::format("{} statements in one record", count)));
  }
  PreparedStatement prepared;
  if (duckdb_prepare_extracted_statement(handles_->conn, statements.get(), 0, prepared.out()) ==
      DuckDBError) {
    return std::unexpected(PrepareErrorOf(prepared.get()));
  }
  const duckdb_statement_type type = duckdb_prepared_statement_type(prepared.get());
  if (!AllowedStatement(type)) {
    return std::unexpected(Refused(std::format("{} statements", StatementTypeName(type))));
  }
  QueryResult result;
  if (duckdb_execute_prepared(prepared.get(), result.get()) == DuckDBError) {
    return std::unexpected(ErrorOf(result.get()));
  }
  return ReadResult(result.get());
}

}  // namespace antb1::slt
