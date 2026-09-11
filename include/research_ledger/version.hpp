#pragma once

#include <cstdint>
#include <string_view>

namespace research_ledger {

// Version of the Research Ledger runtime. The version is reported by the CLI,
// embedded in the CMake package and used by the persistence layer to reject
// snapshots written by an incompatible runtime.
inline constexpr std::uint32_t kVersionMajor = 1;
inline constexpr std::uint32_t kVersionMinor = 0;
inline constexpr std::uint32_t kVersionPatch = 0;

inline constexpr std::string_view kVersionString = "1.0.0";

// Persistence and protocol format revisions. A change that makes an older
// payload ambiguous must bump the corresponding revision; unsupported
// revisions are rejected before any payload is interpreted.
inline constexpr std::uint32_t kPersistenceFormatVersion = 1;
inline constexpr std::uint16_t kProtocolVersion = 1;

}  // namespace research_ledger
