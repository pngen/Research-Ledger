#include "research_ledger/ledger.hpp"

#include <algorithm>
#include <memory>
#include <utility>
#include <variant>

#include "ledger_commit.hpp"
#include "ledger_state.hpp"
#include "research_ledger/record_codec.hpp"

namespace research_ledger {

LedgerRecordId LedgerState::derive_record_id(RecordSequence sequence,
                                             const Digest& payload_digest) const {
    DigestBuilder builder;
    builder.update_u32(sequence.value());
    builder.update_bytes(payload_digest);
    const Digest digest = builder.finish();
    std::uint64_t value = 0;
    for (std::size_t index = 0; index < 8; ++index) {
        value = (value << 8) | static_cast<std::uint64_t>(digest.bytes()[index]);
    }
    if (value == 0) {
        value = 1;
    }
    return LedgerRecordId::from_value(value);
}

void LedgerState::note_subject(const SubjectId& subject, ResearchSessionId session,
                               RecordSequence sequence) {
    subject_index[SubjectKey{subject.kind(), subject.value()}] = SubjectLocation{session, sequence};
}

Result<SubjectLocation> LedgerState::locate(const SubjectId& subject) const {
    const auto iterator = subject_index.find(SubjectKey{subject.kind(), subject.value()});
    if (iterator == subject_index.end()) {
        return Status(ErrorCode::NotFound, "subject " + subject.to_string() + " is not committed");
    }
    return iterator->second;
}

Ledger::Ledger(std::shared_ptr<LedgerState> state) : state_(std::move(state)) {}

Ledger::~Ledger() = default;

Result<std::shared_ptr<Ledger>> Ledger::create(const LedgerConfig& config) {
    if (!config.generation.valid()) {
        return Status(ErrorCode::InvalidIdentity, "ledger generation zero is never valid");
    }
    if (!config.epoch.valid()) {
        return Status(ErrorCode::InvalidIdentity, "coordinator epoch zero is never valid");
    }
    if (!config.local_worker.valid() || !config.local_boot.valid()) {
        return Status(ErrorCode::InvalidIdentity, "local authority identity is not valid");
    }
    auto state = std::make_shared<LedgerState>();
    state->limits = config.limits;
    state->generation = config.generation;
    state->epoch = config.epoch;
    state->require_admission = config.require_worker_admission;
    state->record_counts.assign(static_cast<std::size_t>(kRecordTypeCount) + 1, 0);
    state->local_authority = AuthorityEnvelope{config.generation, config.epoch, config.local_worker,
                                               config.local_boot};
    state->workers[config.local_worker] = WorkerAuthorityView{config.local_worker, config.local_boot,
                                                              config.epoch, true, RecordSequence{}, 0};
    state->local_authority_admitted = true;
    return std::make_shared<Ledger>(std::move(state));
}

AuthorityEnvelope Ledger::local_authority() const {
    std::shared_lock lock(state_->mutex);
    return state_->local_authority;
}

CoordinatorEpoch Ledger::epoch() const {
    std::shared_lock lock(state_->mutex);
    return state_->epoch;
}

LedgerGeneration Ledger::generation() const {
    std::shared_lock lock(state_->mutex);
    return state_->generation;
}

Limits Ledger::limits() const {
    std::shared_lock lock(state_->mutex);
    return state_->limits;
}

RecordSequence Ledger::last_sequence() const {
    std::shared_lock lock(state_->mutex);
    return state_->last_sequence;
}

LedgerSnapshot Ledger::snapshot() const {
    std::shared_lock lock(state_->mutex);
    return LedgerSnapshot(state_, state_->last_sequence, state_->limits);
}

Result<AuthorityEnvelope> Ledger::admit_worker(WorkerId worker, WorkerBootId boot) {
    if (!worker.valid() || !boot.valid()) {
        return Status(ErrorCode::InvalidIdentity, "worker authority identity is not valid");
    }
    std::unique_lock lock(state_->mutex);
    const auto iterator = state_->workers.find(worker);
    if (iterator != state_->workers.end() && iterator->second.boot == boot && iterator->second.live &&
        iterator->second.epoch == state_->epoch) {
        return AuthorityEnvelope{state_->generation, state_->epoch, worker, boot};
    }
    std::uint64_t committed = 0;
    if (iterator != state_->workers.end()) {
        committed = iterator->second.committed_records;
    }
    WorkerAuthorityView view{};
    view.worker = worker;
    view.boot = boot;
    view.epoch = state_->epoch;
    view.live = true;
    view.admitted_at = state_->last_sequence;
    view.committed_records = committed;
    state_->workers[worker] = view;
    if (worker == state_->local_authority.worker) {
        state_->local_authority = AuthorityEnvelope{state_->generation, state_->epoch, worker, boot};
        state_->local_authority_admitted = true;
    }
    return AuthorityEnvelope{state_->generation, state_->epoch, worker, boot};
}

Result<AuthorityEnvelope> Ledger::admit_worker_incarnation(WorkerId worker,
                                                                WorkerBootId previous_boot,
                                                                std::uint32_t boot_counter) {
    if (!worker.valid()) {
        return Status(ErrorCode::InvalidIdentity, "worker identity is not valid");
    }
    std::unique_lock lock(state_->mutex);
    const auto iterator = state_->workers.find(worker);
    if (iterator != state_->workers.end() && previous_boot.valid() &&
        iterator->second.boot == previous_boot && iterator->second.live &&
        iterator->second.epoch == state_->epoch) {
        return AuthorityEnvelope{state_->generation, state_->epoch, worker, iterator->second.boot};
    }
    if (boot_counter == 0) {
        return Status(ErrorCode::InvalidIdentity,
                      "a worker incarnation needs a non-zero boot counter");
    }
    // The boot identity is (coordinator epoch, boot counter): a restart advances
    // the epoch, so an identity can never be reused across coordinator
    // generations, and the counter keeps incarnations distinct within one.
    std::uint64_t boot_value =
        (static_cast<std::uint64_t>(state_->epoch.value()) << 32) |
        static_cast<std::uint64_t>(boot_counter);
    std::uint64_t committed = 0;
    if (iterator != state_->workers.end()) {
        committed = iterator->second.committed_records;
    }
    WorkerAuthorityView view{};
    view.worker = worker;
    view.boot = WorkerBootId::from_value(boot_value);
    view.epoch = state_->epoch;
    view.live = true;
    view.admitted_at = state_->last_sequence;
    view.committed_records = committed;
    state_->workers[worker] = view;
    return AuthorityEnvelope{state_->generation, state_->epoch, worker, view.boot};
}

Status Ledger::fence_worker(WorkerId worker) {
    std::unique_lock lock(state_->mutex);
    const auto iterator = state_->workers.find(worker);
    if (iterator == state_->workers.end()) {
        return Status(ErrorCode::NotFound, "worker is not known to this ledger");
    }
    iterator->second.live = false;
    return Status{};
}

Status Ledger::fence_worker_boot(WorkerId worker, WorkerBootId boot) {
    std::unique_lock lock(state_->mutex);
    const auto iterator = state_->workers.find(worker);
    if (iterator == state_->workers.end()) {
        return Status(ErrorCode::NotFound, "worker is not known to this ledger");
    }
    if (iterator->second.boot == boot) {
        iterator->second.live = false;
    }
    return Status{};
}

Status Ledger::fence_all_workers() {
    std::unique_lock lock(state_->mutex);
    for (auto& entry : state_->workers) {
        entry.second.live = false;
    }
    return Status{};
}

Result<CoordinatorEpoch> Ledger::advance_epoch() {
    std::unique_lock lock(state_->mutex);
    const CoordinatorEpoch next = state_->epoch.next();
    if (!next.valid()) {
        return Status(ErrorCode::LimitExceeded, "coordinator epoch exhausted");
    }
    std::uint64_t local_committed = 0;
    if (const auto local = state_->workers.find(state_->local_authority.worker);
        local != state_->workers.end()) {
        local_committed = local->second.committed_records;
    }
    state_->epoch = next;
    for (auto& entry : state_->workers) {
        entry.second.live = false;
    }
    // The local authority belongs to the coordinator process that has just
    // started. Every other incarnation must re-handshake before it may append.
    state_->local_authority.epoch = next;
    state_->workers[state_->local_authority.worker] =
        WorkerAuthorityView{state_->local_authority.worker, state_->local_authority.worker_boot, next,
                            true, state_->last_sequence, local_committed};
    return next;
}

bool Ledger::worker_is_live(WorkerId worker, WorkerBootId boot) const {
    std::shared_lock lock(state_->mutex);
    const auto iterator = state_->workers.find(worker);
    if (iterator == state_->workers.end()) {
        return false;
    }
    return iterator->second.live && iterator->second.boot == boot &&
           iterator->second.epoch == state_->epoch;
}

Result<std::vector<WorkerAuthorityView>> Ledger::workers() const {
    std::shared_lock lock(state_->mutex);
    std::vector<WorkerAuthorityView> result;
    result.reserve(state_->workers.size());
    for (const auto& entry : state_->workers) {
        result.push_back(entry.second);
    }
    std::sort(result.begin(), result.end(),
              [](const WorkerAuthorityView& lhs, const WorkerAuthorityView& rhs) {
                  return lhs.worker < rhs.worker;
              });
    return result;
}

Result<std::vector<AppendOutcome>> Ledger::append_batch_with_authority(
    std::span<const RecordDraft> drafts, const AuthorityEnvelope& authority) {
    if (drafts.empty()) {
        return std::vector<AppendOutcome>{};
    }
    std::unique_lock lock(state_->mutex);
    if (drafts.size() > state_->limits.max_records_per_batch) {
        return Status(ErrorCode::LimitExceeded, "batch exceeds the configured maximum record count");
    }
    if (!authority.ledger.valid() || !authority.epoch.valid() || !authority.worker.valid() ||
        !authority.worker_boot.valid()) {
        return Status(ErrorCode::InvalidIdentity, "append authority is not complete");
    }
    if (!(authority.ledger == state_->generation)) {
        return Status(ErrorCode::StaleGeneration, "append targets a different ledger generation");
    }
    if (!(authority.epoch == state_->epoch)) {
        return Status(ErrorCode::StaleEpoch, "append carries a superseded coordinator epoch");
    }
    if (state_->require_admission) {
        const auto worker = state_->workers.find(authority.worker);
        if (worker == state_->workers.end()) {
            return Status(ErrorCode::Unauthorized, "worker incarnation was never admitted");
        }
        if (!(worker->second.boot == authority.worker_boot)) {
            return Status(ErrorCode::StaleWorker, "append carries a superseded worker incarnation");
        }
        if (!worker->second.live) {
            return Status(ErrorCode::StaleWorker, "worker incarnation is fenced");
        }
        if (!(worker->second.epoch == state_->epoch)) {
            return Status(ErrorCode::StaleEpoch, "worker incarnation belongs to a superseded epoch");
        }
    }

    const std::uint64_t projected = state_->last_sequence.value() + drafts.size();
    if (projected > state_->limits.max_records) {
        return Status(ErrorCode::LimitExceeded, "ledger would exceed the configured record maximum");
    }

    BatchStaging staging;
    staging.staged.reserve(drafts.size());
    staging.next_sequence = state_->last_sequence;
    std::vector<AppendOutcome> outcomes;
    outcomes.reserve(drafts.size());

    for (const RecordDraft& draft : drafts) {
        if (draft.provenance == Provenance::Unknown) {
            return Status(ErrorCode::InvalidArgument,
                          "a record cannot be committed without stated provenance");
        }
        if (draft.record_id.valid()) {
            const auto existing = state_->record_index.find(draft.record_id);
            if (existing != state_->record_index.end()) {
                if (!draft.idempotent) {
                    return Status(ErrorCode::DuplicateRecord, "record identity was already committed");
                }
                outcomes.push_back(AppendOutcome{existing->second, CommitState::Duplicate, true});
                continue;
            }
            bool staged_duplicate = false;
            RecordSequence staged_sequence{};
            for (std::size_t index = 0; index < staging.record_ids.size(); ++index) {
                if (staging.record_ids[index] == draft.record_id) {
                    staged_duplicate = true;
                    staged_sequence = staging.sequences[index];
                    break;
                }
            }
            if (staged_duplicate) {
                if (!draft.idempotent) {
                    return Status(ErrorCode::DuplicateRecord, "record identity repeats inside the batch");
                }
                outcomes.push_back(AppendOutcome{staged_sequence, CommitState::Duplicate, true});
                continue;
            }
        }

        const RecordSequence sequence = staging.next_sequence.next();
        if (!sequence.valid()) {
            return Status(ErrorCode::LimitExceeded, "record sequence exhausted");
        }
        auto payload_digest = record_body_digest(draft.body, state_->limits);
        if (!payload_digest.ok()) {
            return payload_digest.status();
        }
        auto staged = build_staged_entity(*state_, staging, draft, sequence);
        if (!staged.ok()) {
            return staged.status();
        }
        const LedgerRecordId record_id = draft.record_id.valid()
                                             ? draft.record_id
                                             : state_->derive_record_id(sequence, payload_digest.value());
        staging.stage(staged.take(), record_id, payload_digest.value(), draft.provenance,
                      draft.body);
        staging.next_sequence = sequence;
        outcomes.push_back(AppendOutcome{sequence, CommitState::Validated, false});
    }

    // Every draft validated: commit the batch. Nothing below can fail, so
    // committed history is either extended completely or not at all.
    const TimestampNs committed_at = now_timestamp();
    Digest chain = state_->chain_digest;
    std::size_t index = 0;
    for (const StagedEntity& staged : staging.staged) {
        Record record;
        record.header.sequence = staged.sequence;
        record.header.type = staged.type;
        record.header.record_id = staging.record_ids[index];
        record.header.authority = authority;
        record.header.committed_at = committed_at;
        record.header.provenance = staging.provenances[index];
        record.header.payload_digest = staging.payload_digests[index];
        record.header.chain_digest = next_chain_digest(chain, record.header);
        chain = record.header.chain_digest;
        record.body = staging.bodies[index];
        apply_staged_entity(*state_, record, staged);
        ++index;
    }
    state_->chain_digest = chain;
    for (AppendOutcome& outcome : outcomes) {
        if (outcome.state == CommitState::Validated) {
            outcome.state = CommitState::Committed;
        }
    }
    // Committed-record accounting is per incarnation, whether or not admission
    // is enforced: the ledger records who produced what.
    const auto producer = state_->workers.find(authority.worker);
    if (producer != state_->workers.end()) {
        producer->second.committed_records += staging.staged.size();
    }
    return outcomes;
}

Result<AppendOutcome> Ledger::append_with_authority(const RecordDraft& draft,
                                                    const AuthorityEnvelope& authority) {
    const std::span<const RecordDraft> single(&draft, 1);
    auto outcomes = append_batch_with_authority(single, authority);
    if (!outcomes.ok()) {
        return outcomes.status();
    }
    if (outcomes.value().empty()) {
        return Status(ErrorCode::InternalError, "append produced no outcome");
    }
    return outcomes.value().front();
}

Result<std::vector<AppendOutcome>> Ledger::append_batch(std::span<const RecordDraft> drafts) {
    return append_batch_with_authority(drafts, local_authority());
}

Result<AppendOutcome> Ledger::append(const RecordDraft& draft) {
    return append_with_authority(draft, local_authority());
}

}  // namespace research_ledger
