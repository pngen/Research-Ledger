#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "research_ledger/error.hpp"
#include "research_ledger/evidence.hpp"
#include "research_ledger/identity.hpp"
#include "research_ledger/limits.hpp"
#include "research_ledger/record.hpp"

namespace research_ledger {

// Implementation state of a ledger. Opaque: no mutable internal container is
// reachable from the public API, and a snapshot keeps the state it observes
// alive even if the Ledger object itself is destroyed.
class LedgerState;

struct LedgerConfig {
    // Ledger generation distinguishes independent authoritative ledger
    // histories. Records from another generation are never accepted.
    LedgerGeneration generation = LedgerGeneration::first();

    // Local authority used by append() without an explicit envelope.
    WorkerId local_worker = WorkerId::from_value(1);
    WorkerBootId local_boot = WorkerBootId::from_value(1);
    CoordinatorEpoch epoch = CoordinatorEpoch::first();

    // When true, only an admitted (worker, boot) incarnation may append. The
    // coordinator enables this; the single process reference deployment does
    // not need it.
    bool require_worker_admission = false;

    Limits limits{};
};

// Live process authority, as opposed to durable history. A worker that produced
// records yesterday is historical truth; that does not authorize its old
// incarnation to append today.
struct WorkerAuthorityView {
    WorkerId worker{};
    WorkerBootId boot{};
    CoordinatorEpoch epoch{};
    bool live = false;
    RecordSequence admitted_at{};
    std::uint64_t committed_records = 0;
};

class LedgerSnapshot {
public:
    LedgerSnapshot() = default;

    [[nodiscard]] bool valid() const noexcept { return state_ != nullptr; }
    // Everything at or below this sequence was committed before the snapshot
    // was taken. A snapshot never observes a partially committed record.
    [[nodiscard]] RecordSequence watermark() const noexcept { return watermark_; }
    [[nodiscard]] Limits limits() const noexcept { return limits_; }

    Result<SessionView> session(ResearchSessionId session) const;
    Result<std::vector<SessionView>> sessions() const;
    Result<HypothesisView> hypothesis(HypothesisId hypothesis) const;
    Result<std::vector<HypothesisId>> hypothesis_ancestry(HypothesisId hypothesis) const;
    Result<std::vector<HypothesisId>> hypothesis_descendants(HypothesisId hypothesis) const;
    Result<BranchView> branch(BranchId branch) const;
    Result<std::vector<BranchId>> branch_ancestry(BranchId branch) const;
    Result<ExperimentView> experiment(ExperimentId experiment) const;
    Result<AttemptView> attempt(AttemptId attempt) const;
    Result<std::vector<AttemptView>> experiment_attempts(ExperimentId experiment) const;
    Result<std::vector<AttemptView>> branch_attempts(BranchId branch) const;
    Result<std::vector<ModelCallView>> model_calls(AttemptId attempt) const;
    Result<std::vector<ToolCallView>> tool_calls(AttemptId attempt) const;
    Result<std::vector<ObservationView>> observations(AttemptId attempt) const;
    Result<ArtifactView> artifact(ArtifactId artifact) const;
    Result<std::vector<ArtifactView>> artifact_ancestry(ArtifactId artifact) const;
    Result<std::vector<ArtifactView>> artifact_descendants(ArtifactId artifact) const;
    Result<FailureView> failure(FailureId failure) const;
    Result<std::vector<FailureView>> failures() const;
    Result<std::vector<FailureView>> failures_of(SubjectId scope) const;
    Result<DecisionView> decision(DecisionId decision) const;
    Result<std::vector<DecisionView>> decisions_of(SubjectId subject) const;
    Result<ResultView> result(ResultId result) const;
    Result<std::vector<ResultView>> results_in_session(ResearchSessionId session) const;
    Result<EvidenceBundle> supporting_evidence(ResultId result) const;
    Result<AccountingAggregate> accounting(SubjectId scope) const;
    Result<Explanation> explain_result(ResultId result) const;
    Result<std::vector<BranchId>> unresolved_branches(ResearchSessionId session) const;
    // Latest committed record sequence concerning a subject. A decision cites
    // evidence as (subject, sequence) pairs, so this is how a caller obtains
    // the sequence of the record it wants to cite.
    Result<RecordSequence> subject_sequence(SubjectId subject) const;
    Result<Record> record_at(RecordSequence sequence) const;
    Result<std::vector<Record>> records(RecordSequence from, RecordSequence to) const;
    Result<LedgerStats> stats() const;
    Result<IntegrityReport> verify() const;

private:
    friend class Ledger;
    LedgerSnapshot(std::shared_ptr<LedgerState> state, RecordSequence watermark, Limits limits)
        : state_(std::move(state)), watermark_(watermark), limits_(limits) {}

    std::shared_ptr<LedgerState> state_{};
    RecordSequence watermark_{};
    Limits limits_{};
};

// The append-only research ledger.
//
// Thread safety: the public API is internally synchronized. Appends take an
// exclusive lock only for validation and mutation of committed state; no lock
// is ever held across network I/O, filesystem I/O, callbacks or user code.
class Ledger {
public:
    static Result<std::shared_ptr<Ledger>> create(const LedgerConfig& config);

    explicit Ledger(std::shared_ptr<LedgerState> state);
    ~Ledger();
    Ledger(const Ledger&) = delete;
    Ledger& operator=(const Ledger&) = delete;
    Ledger(Ledger&&) = delete;
    Ledger& operator=(Ledger&&) = delete;

    // --- commit -----------------------------------------------------------------
    Result<AppendOutcome> append(const RecordDraft& draft);
    Result<std::vector<AppendOutcome>> append_batch(std::span<const RecordDraft> drafts);
    Result<AppendOutcome> append_with_authority(const RecordDraft& draft,
                                                const AuthorityEnvelope& authority);
    Result<std::vector<AppendOutcome>> append_batch_with_authority(
        std::span<const RecordDraft> drafts, const AuthorityEnvelope& authority);

    // --- process authority ------------------------------------------------------
    [[nodiscard]] AuthorityEnvelope local_authority() const;
    [[nodiscard]] CoordinatorEpoch epoch() const;
    [[nodiscard]] LedgerGeneration generation() const;
    [[nodiscard]] Limits limits() const;
    [[nodiscard]] RecordSequence last_sequence() const;

    // Admits a worker incarnation. A second boot for the same worker fences the
    // previous incarnation rather than allowing two live authorities.
    Result<AuthorityEnvelope> admit_worker(WorkerId worker, WorkerBootId boot);
    // Mints the boot identity of a worker incarnation. A worker never invents
    // its own boot identity: the coordinator mints one per admitted
    // incarnation, which makes an older incarnation's traffic stale by
    // construction. A reconnect that presents the current boot keeps it. The
    // caller supplies a fresh, monotonically increasing boot counter; the ledger
    // never owns that counter, so nothing is shared between connections.
    Result<AuthorityEnvelope> admit_worker_incarnation(WorkerId worker, WorkerBootId previous_boot,
                                                       std::uint32_t boot_counter);
    Status fence_worker(WorkerId worker);
    Status fence_worker_boot(WorkerId worker, WorkerBootId boot);
    Status fence_all_workers();
    // Advances the coordinator epoch and fences every live incarnation. Called
    // when a coordinator process starts or restarts.
    Result<CoordinatorEpoch> advance_epoch();
    [[nodiscard]] bool worker_is_live(WorkerId worker, WorkerBootId boot) const;
    Result<std::vector<WorkerAuthorityView>> workers() const;

    // --- reading ----------------------------------------------------------------
    [[nodiscard]] LedgerSnapshot snapshot() const;

    // --- persistence and replay -------------------------------------------------
    Status save(const std::string& path) const;
    static Result<std::shared_ptr<Ledger>> load(const std::string& path, const LedgerConfig& config);
    // Rebuilds a ledger from committed records. Historical authority metadata is
    // preserved as history; it never becomes current process authority.
    static Result<std::shared_ptr<Ledger>> restore(std::span<const Record> records,
                                                   const LedgerConfig& config);
    [[nodiscard]] Digest logical_digest() const;

private:
    std::shared_ptr<LedgerState> state_{};
};

}  // namespace research_ledger
