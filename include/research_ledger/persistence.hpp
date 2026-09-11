#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "research_ledger/digest.hpp"
#include "research_ledger/error.hpp"
#include "research_ledger/identity.hpp"
#include "research_ledger/limits.hpp"
#include "research_ledger/record.hpp"

namespace research_ledger {

// Persistence layout (all integers little-endian, written explicitly):
//
//   magic           8 bytes  "RLEDGER1"
//   format_version  u32      must equal kPersistenceFormatVersion
//   runtime major   u32
//   runtime minor   u32
//   runtime patch   u32
//   ledger_gen      u32      generation of the persisted ledger
//   epoch           u32      coordinator epoch at the time of the snapshot
//   record_count    u32      bounded by Limits::max_persistence_records
//   body_length     u64      bounded by Limits::max_persistence_bytes
//   header_digest   32 bytes SHA-256 over every preceding header byte
//   body            record_count records, each length prefixed
//   body_digest     32 bytes SHA-256 over the body
//
// A snapshot is rejected before any payload is interpreted when the magic, the
// format version, the declared counts, the header digest or the body digest do
// not match, when the file is truncated, or when trailing bytes remain.
inline constexpr char kPersistenceMagic[8] = {'R', 'L', 'E', 'D', 'G', 'E', 'R', '1'};

struct LoadedSnapshot {
    std::vector<Record> records{};
    LedgerGeneration generation{};
    CoordinatorEpoch epoch{};
    Digest chain_digest{};
    Digest body_digest{};
    std::uint64_t bytes = 0;
};

// Serializes a committed record list into an in-memory snapshot image.
Result<std::vector<std::byte>> serialize_snapshot(std::span<const Record> records,
                                                  LedgerGeneration generation, CoordinatorEpoch epoch,
                                                  const Limits& limits);

// Decodes a snapshot image. Never allocates from an unbounded declared count.
Result<LoadedSnapshot> deserialize_snapshot(std::span<const std::byte> image, const Limits& limits);

// Reads a snapshot file with a bounded read.
Result<LoadedSnapshot> load_snapshot_file(const std::string& path, const Limits& limits);

// Transactional write: serialize, validate, write to a temporary file in the
// same directory, flush and close it, replace the target atomically, then
// reopen and verify that the committed image decodes to the same records.
Status save_snapshot_file(const std::string& path, std::span<const Record> records,
                          LedgerGeneration generation, CoordinatorEpoch epoch, const Limits& limits);

}  // namespace research_ledger
