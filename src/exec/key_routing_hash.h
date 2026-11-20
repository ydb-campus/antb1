#pragma once

#include <cstdint>

namespace antb1::exec {

// The GroupTable lane hash is shared by routed input rows and the stored keys of finished parts.
inline constexpr std::uint64_t kNullGroupKeyHash = 0x5BD1E995ULL;
inline constexpr std::uint64_t kGroupKeySeed = 0x9E3779B97F4A7C15ULL;

inline std::uint64_t MixGroupKeyHash(std::uint64_t hash, std::uint64_t value) {
  return (hash ^ value) * 0x100000001B3ULL;
}

inline std::uint64_t FinishGroupKeyHash(std::uint64_t hash) { return hash ^ (hash >> 33U); }

}  // namespace antb1::exec
