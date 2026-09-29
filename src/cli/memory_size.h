#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

// Private to the cli module: `--memory-limit` sizes.

namespace antb1::cli {

// A --memory-limit value in bytes: "123", "500KB", "1.5GB" (1000-based units KB MB GB TB),
// "2GiB" (1024-based KiB MiB GiB TiB), or "80%" of `physical` bytes; units are case-insensitive.
// std::nullopt for anything else, for a percentage without `physical`, and for less than 1 byte.
std::optional<int64_t> ParseMemorySize(std::string_view text, std::optional<int64_t> physical);

// The host's physical memory in bytes, if the OS reports it.
std::optional<int64_t> PhysicalMemory();

}  // namespace antb1::cli
