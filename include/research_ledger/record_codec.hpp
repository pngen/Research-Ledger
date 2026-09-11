#pragma once

#include <cstddef>
#include <span>
#include <vector>

#include "research_ledger/digest.hpp"
#include "research_ledger/error.hpp"
#include "research_ledger/limits.hpp"
#include "research_ledger/record.hpp"

namespace research_ledger {

// Canonical binary encoding shared by the protocol and the persistence layer.
// Integers are explicit little-endian, identities carry their domain tag,
// generations are explicit, and every declared length is checked against the
// configured limit before it is used.
Result<std::vector<std::byte>> encode_record_body(const RecordBody& body, const Limits& limits);
Result<RecordBody> decode_record_body(std::span<const std::byte> payload, const Limits& limits);

Result<std::vector<std::byte>> encode_draft(const RecordDraft& draft, const Limits& limits);
Result<RecordDraft> decode_draft(std::span<const std::byte> payload, const Limits& limits);

Result<std::vector<std::byte>> encode_record(const Record& record, const Limits& limits);
Result<Record> decode_record(std::span<const std::byte> payload, const Limits& limits);

// Digest of the canonical encoding of a body: what the ledger commits to.
Result<Digest> record_body_digest(const RecordBody& body, const Limits& limits);

}  // namespace research_ledger
