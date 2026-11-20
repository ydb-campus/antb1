#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "antb1/common/int128.h"

namespace antb1::exec {

// The hash of a FixedKeyTable: the mixing scheme of foldhash 0.2.0's fast tier. fold(x, y) is the
// low xor the high half of the 128-bit product (a folded multiply, as wyhash and others use), and a
// word mixes in as fold(v ^ prev ^ A3, A6), prev being the seed for the first word. The three
// constants are hexadecimal digits of pi, the ones foldhash picks for the same roles. The seed is
// fixed, so a table's ids are a function of its input. The table takes a slot from the hash's low
// bits, which the high half of the product makes depend on every bit of the key, and a tag from its
// top seven.
struct FoldHash {
  static constexpr std::uint64_t kFoldA3 = 0x082efa98ec4e6c89ULL;
  static constexpr std::uint64_t kFoldA6 = 0xc0ac29b7c97c50ddULL;
  static constexpr std::uint64_t kSeed = 0x243f6a8885a308d3ULL;

  static std::uint64_t Mix(std::uint64_t value, std::uint64_t prev) {
    const UInt128 full = static_cast<UInt128>(value ^ prev ^ kFoldA3) * kFoldA6;
    return static_cast<std::uint64_t>(full) ^ static_cast<std::uint64_t>(full >> 64U);
  }
  // A key's words, mixed in one after another.
  template <std::size_t kWords>
  static std::uint64_t Hash(const std::array<std::uint64_t, kWords>& words) {
    std::uint64_t hash = kSeed;
    for (const std::uint64_t word : words) {
      hash = Mix(word, hash);
    }
    return hash;
  }
};

}  // namespace antb1::exec
