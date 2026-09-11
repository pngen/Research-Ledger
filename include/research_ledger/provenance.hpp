#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

namespace research_ledger {

// How a piece of evidence came to exist. Provenance travels with every
// material value and is exposed by snapshots, queries, explanations and the
// CLI. Synthetic evidence is never reported as measured, reconstructed
// evidence is never reported as an original observation, and an estimated cost
// is never reported as a measured cost.
enum class Provenance : std::uint8_t {
    Unknown = 0,
    Measured = 1,
    Reported = 2,
    Derived = 3,
    Estimated = 4,
    Synthetic = 5,
    Reconstructed = 6,
};

std::string_view provenance_name(Provenance provenance) noexcept;
std::optional<Provenance> parse_provenance(std::string_view text) noexcept;

}  // namespace research_ledger
