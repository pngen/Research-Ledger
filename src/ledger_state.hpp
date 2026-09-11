#pragma once

// Internal ledger state. This header is not installed: it is the private
// representation behind the opaque LedgerState handle.

#include <cstdint>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "research_ledger/evidence.hpp"
#include "research_ledger/identity.hpp"
#include "research_ledger/ledger.hpp"
#include "research_ledger/limits.hpp"
#include "research_ledger/record.hpp"

namespace research_ledger {

struct SubjectKey {
    SubjectKind kind = SubjectKind::None;
    std::uint64_t value = 0;

    friend bool operator==(const SubjectKey& lhs, const SubjectKey& rhs) noexcept {
        return lhs.kind == rhs.kind && lhs.value == rhs.value;
    }
};

struct SubjectKeyHash {
    std::size_t operator()(const SubjectKey& key) const noexcept {
        return std::hash<std::uint64_t>{}(key.value * 1099511628211ull +
                                          static_cast<std::uint64_t>(key.kind));
    }
};

struct SubjectLocation {
    ResearchSessionId session{};
    RecordSequence sequence{};
};

struct MetricState {
    MetricId metric{};
    ResearchSessionId session{};
    std::string canonical_key{};
    UnitKind unit = UnitKind::None;
    MetricValueKind value_kind = MetricValueKind::Unknown;
    RecordSequence declared_at{};
};

struct AccountingState {
    SubjectId scope{};
    AccountingVector accounting{};
    RecordSequence sequence{};
};

struct ArtifactState {
    ArtifactView view{};
    std::vector<ArtifactId> children{};
    // Sequence of the record that invalidated this artifact, if any, so a
    // snapshot can report the state it actually observed.
    RecordSequence invalidated_at{};
};

// The mutable, synchronized ledger representation. One instance is shared by
// the Ledger facade and by every snapshot taken from it.
class LedgerState {
public:
    mutable std::shared_mutex mutex{};

    Limits limits{};
    LedgerGeneration generation = LedgerGeneration::first();
    CoordinatorEpoch epoch = CoordinatorEpoch::first();
    bool require_admission = false;
    AuthorityEnvelope local_authority{};
    bool local_authority_admitted = false;

    std::vector<Record> records{};
    // Primary subject of each committed record, parallel to records. Used to
    // check that a decision only cites records that concern the subject it
    // decided about.
    std::vector<SubjectId> record_subjects{};
    std::unordered_map<LedgerRecordId, RecordSequence> record_index{};
    std::unordered_map<RecordSequence, std::size_t> sequence_index{};

    std::unordered_map<ResearchSessionId, SessionView> sessions{};
    std::unordered_map<HypothesisId, HypothesisView> hypotheses{};
    std::unordered_map<ExperimentId, ExperimentView> experiments{};
    std::unordered_map<BranchId, BranchView> branches{};
    std::unordered_map<AttemptId, AttemptView> attempts{};
    std::unordered_map<ModelCallId, ModelCallView> model_calls{};
    std::unordered_map<ToolCallId, ToolCallView> tool_calls{};
    std::unordered_map<ArtifactId, ArtifactState> artifacts{};
    std::unordered_map<ObservationId, ObservationView> observations{};
    std::unordered_map<MetricId, MetricState> metrics{};
    std::unordered_map<FailureId, FailureView> failures{};
    std::unordered_map<DecisionId, DecisionView> decisions{};
    std::unordered_map<ResultId, ResultView> results{};

    // Session membership and subject resolution.
    std::unordered_map<ResearchSessionId, std::vector<HypothesisId>> session_hypotheses{};
    std::unordered_map<ResearchSessionId, std::vector<ExperimentId>> session_experiments{};
    std::unordered_map<ResearchSessionId, std::vector<ResultId>> session_results{};
    std::unordered_map<ResearchSessionId, std::vector<BranchId>> session_branches{};
    std::unordered_map<ResearchSessionId, std::vector<FailureId>> session_failures{};
    std::unordered_map<ResearchSessionId, std::unordered_map<std::string, MetricId>> session_metric_keys{};
    std::unordered_map<HypothesisId, std::vector<HypothesisId>> hypothesis_children{};
    std::unordered_map<SubjectKey, SubjectLocation, SubjectKeyHash> subject_index{};
    std::unordered_map<SubjectKey, std::vector<std::size_t>, SubjectKeyHash> accounting_by_scope{};
    std::vector<AccountingState> accounting{};

    // Process authority, distinct from durable history.
    std::unordered_map<WorkerId, WorkerAuthorityView> workers{};
    std::uint64_t historical_authority_records = 0;

    Digest chain_digest{};
    RecordSequence last_sequence{};
    std::vector<std::uint64_t> record_counts{};

    // Deterministic identity for a record that a caller did not identify.
    LedgerRecordId derive_record_id(RecordSequence sequence, const Digest& payload_digest) const;

    // Subject bookkeeping helpers shared by validation, apply and queries.
    void note_subject(const SubjectId& subject, ResearchSessionId session, RecordSequence sequence);
    Result<SubjectLocation> locate(const SubjectId& subject) const;
};

// Deterministic digest of reconstructed logical state at or below a
// watermark. The rendering is independent of insertion order: two ledgers that
// committed the same records in a different arrival order produce the same
// digest.
Digest logical_state_digest_of(const LedgerState& state, RecordSequence watermark);

// Chain digest of a header given the chain of the record before it. The chain
// covers the previous chain, the sequence, the record type, the record
// identity, the commit timestamp, the stated provenance and the payload digest,
// so any edit to committed history breaks the chain.
Digest next_chain_digest(const Digest& previous, const RecordHeader& header);

// Experiments of a session that an accepted result rests on, at or below a
// watermark. Branch resolution and unresolved-branch listing share this set, so
// the two queries cannot disagree.
std::unordered_set<ExperimentId> accepted_experiments_of(const LedgerState& state,
                                                         ResearchSessionId session,
                                                         RecordSequence watermark);

// Fills a branch view's attempt count and its unresolved flag from committed
// state. A branch is unresolved when it has attempts, none of them completed,
// and no accepted result rests on one of its experiments.
void resolve_branch_state(const LedgerState& state, BranchView& view, RecordSequence watermark,
                          const std::unordered_set<ExperimentId>& accepted_experiments);

// Rebuilds an aggregate scope set for an accounting query.
void collect_accounting_scope(const LedgerState& state, const SubjectId& scope,
                              std::unordered_set<SubjectKey, SubjectKeyHash>& out);

}  // namespace research_ledger
