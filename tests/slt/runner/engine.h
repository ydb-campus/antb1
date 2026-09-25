#pragma once

#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// The engines antb1-slt runs records on (antb1 through engine::Session, DuckDB through its C API).
// Both return values as canonical text (canonical.h), so one formatter and one comparator serve
// both.

namespace antb1::slt {

// sqllogictest column classes: I integer, R real (compared with a tolerance), T text (dates too).
enum class ColumnClass : std::uint8_t { kInteger, kReal, kText };

char ClassLetter(ColumnClass c);

struct ResultSet {
  std::vector<ColumnClass> classes;
  std::vector<std::string> type_names;  // engine type names, for messages (e.g. BIGINT, HUGEINT)
  // Canonical text of every value (canonical.h); std::nullopt is SQL NULL.
  std::vector<std::vector<std::optional<std::string>>> rows;
};

struct EngineError {
  std::string kind;  // antb1: parse|bind|unsupported|io|execution|internal; DuckDB: its error type
  std::string message;       // "<kind>: <message>"; `statement error <regex>` searches it
  bool unsupported = false;  // antb1 SqlErrorDetail{kUnsupported}: never an acceptable outcome
  bool internal = false;     // an engine or harness bug, never an acceptable `statement error`
};

using ExecResult = std::expected<ResultSet, EngineError>;

class Engine {
 public:
  Engine() = default;
  Engine(const Engine&) = delete;
  Engine& operator=(const Engine&) = delete;
  Engine(Engine&&) = delete;
  Engine& operator=(Engine&&) = delete;
  virtual ~Engine() = default;

  // "antb1" or "duckdb": the names used by skipif/onlyif.
  [[nodiscard]] virtual std::string_view name() const = 0;
  virtual ExecResult Execute(const std::string& sql) = 0;
};

}  // namespace antb1::slt
