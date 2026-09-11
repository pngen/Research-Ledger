#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "research_ledger/digest.hpp"
#include "research_ledger/evidence.hpp"
#include "research_ledger/ledger.hpp"
#include "research_ledger/record.hpp"

namespace research_ledger {

// Outcome of a deterministic replay of committed ledger bytes.
struct ReplayReport {
    std::uint64_t records = 0;
    RecordSequence last_sequence{};
    Digest chain_digest{};      // digest of the committed record stream
    Digest logical_digest{};    // digest of the reconstructed logical state
    IntegrityReport integrity{};
    LedgerStats stats{};
};

// Reconstructs logical state from committed records and reports the digest of
// that state. Replaying the same bytes twice produces the same logical digest.
Result<ReplayReport> replay_committed(std::span<const Record> records, const LedgerConfig& config);

// Digest of the reconstructed logical state of a live ledger.
Result<Digest> logical_state_digest(const Ledger& ledger);

}  // namespace research_ledger
