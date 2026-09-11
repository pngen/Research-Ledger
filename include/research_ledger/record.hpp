#pragma once

#include <cstdint>
#include <optional>
#include <string_view>
#include <variant>
#include <vector>

#include "research_ledger/digest.hpp"
#include "research_ledger/error.hpp"
#include "research_ledger/identity.hpp"
#include "research_ledger/model.hpp"
#include "research_ledger/provenance.hpp"

namespace research_ledger {

// One committed record type per payload alternative. The record type is derived
// from the active payload alternative, so a header can never disagree with the
// body it carries.
enum class RecordType : std::uint16_t {
    SessionOpened = 1,
    SessionClosed = 2,
    SessionAnnotation = 3,
    HypothesisDeclared = 4,
    HypothesisStatusChanged = 5,
    ExperimentDeclared = 6,
    BranchDeclared = 7,
    AttemptStarted = 8,
    AttemptCompleted = 9,
    AttemptFailed = 10,
    AttemptCancelled = 11,
    ModelCallRecorded = 12,
    ToolCallRecorded = 13,
    ArtifactReferenced = 14,
    ArtifactInvalidated = 15,
    MetricDeclared = 16,
    ObservationRecorded = 17,
    FailureRecorded = 18,
    DecisionRecorded = 19,
    ResultDeclared = 20,
    ResultStatusChanged = 21,
    AccountingRecorded = 22,
};

inline constexpr std::uint16_t kRecordTypeCount = 22;

std::string_view record_type_name(RecordType type) noexcept;
std::optional<RecordType> parse_record_type(std::string_view text) noexcept;

using RecordBody =
    std::variant<SessionOpened, SessionClosed, SessionAnnotation, HypothesisDeclared,
                 HypothesisStatusChanged, ExperimentDeclared, BranchDeclared, AttemptStarted,
                 AttemptCompleted, AttemptFailed, AttemptCancelled, ModelCallRecorded,
                 ToolCallRecorded, ArtifactReferenced, ArtifactInvalidated, MetricDeclared,
                 ObservationRecorded, FailureRecorded, DecisionRecorded, ResultDeclared,
                 ResultStatusChanged, AccountingRecorded>;

static_assert(std::variant_size_v<RecordBody> == kRecordTypeCount,
              "record type enumeration must stay in step with the record body");

RecordType record_type_of(const RecordBody& body) noexcept;

// A draft is a proposed record: what happened, reported with a provenance, by
// an authority. It becomes historical truth only when the ledger commits it.
struct RecordDraft {
    // Optional caller supplied record identity. When it is invalid the ledger
    // derives a deterministic identity from the committed position. When it is
    // valid, a second append of the same identity is either rejected as a
    // duplicate or recognized idempotently, depending on idempotent.
    LedgerRecordId record_id{};
    Provenance provenance = Provenance::Unknown;
    bool idempotent = false;
    RecordBody body{};
};

struct RecordHeader {
    RecordSequence sequence{};
    RecordType type = RecordType::SessionOpened;
    LedgerRecordId record_id{};
    AuthorityEnvelope authority{};
    TimestampNs committed_at{};
    Provenance provenance = Provenance::Unknown;
    Digest payload_digest{};
    Digest chain_digest{};
};

struct Record {
    RecordHeader header{};
    RecordBody body{};
};

// Result of a single append inside a batch.
struct AppendOutcome {
    RecordSequence sequence{};
    CommitState state = CommitState::Invalid;
    bool duplicate = false;
};

// The identity of an entity that is being declared: a value plus, where the
// domain has revisions, the generation the record claims.
struct EntityClaim {
    SubjectId subject{};
    std::uint32_t generation = 0;  // zero when the domain has no generation
};

}  // namespace research_ledger
