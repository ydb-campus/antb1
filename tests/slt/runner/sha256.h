#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace antb1::slt {

// Incremental SHA-256 (FIPS 180-4), for inputs too large to hold in one string (fixture digests).
class Sha256 {
 public:
  Sha256() = default;

  void Update(std::string_view data);
  // Lowercase hex digest of everything passed to Update(). Finalizes: call it once.
  std::string HexDigest();

 private:
  void Push(unsigned char byte);

  std::array<uint32_t, 8> state_ = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                    0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
  std::array<unsigned char, 64> block_{};
  std::size_t used_ = 0;
  uint64_t length_ = 0;  // bytes
};

// Lowercase hex SHA-256 of data. Used for hashed query results and for redacted failure reports,
// which must identify results without printing their values.
std::string Sha256Hex(std::string_view data);

}  // namespace antb1::slt
