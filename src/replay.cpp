#include "research_ledger/replay.hpp"

#include <algorithm>
#include <memory>
#include <utility>

#include "ledger_commit.hpp"
#include "ledger_state.hpp"
#include "research_ledger/persistence.hpp"
#include "research_ledger/record_codec.hpp"

namespace research_ledger {

Result<std::shared_ptr<Ledger>> Ledger::restore(std::span<const Record> records,
                                                const LedgerConfig& config) {
    auto created = Ledger::create(config);
    if (!created.ok()) {
        return created.status();
    }
    std::shared_ptr<Ledger> ledger = created.take();
    std::shared_ptr<LedgerState> state = ledger->state_;

    std::vector<Record> ordered(records.begin(), records.end());
    std::sort(ordered.begin(), ordered.end(), [](const Record& lhs, const Record& rhs) {
        return lhs.header.sequence < rhs.header.sequence;
    });

    std::unique_lock lock(state->mutex);
    Digest chain{};
    for (std::size_t index = 0; index < ordered.size(); ++index) {
        const Record& record = ordered[index];
        if (record.header.sequence.value() != index + 1) {
            return Status(ErrorCode::IntegrityFailure,
                          "restored records are not a contiguous committed history");
        }
        if (!(record.header.authority.ledger == state->generation)) {
            return Status(ErrorCode::StaleGeneration,
                          "restored record belongs to a different ledger generation");
        }
        if (state->record_index.find(record.header.record_id) != state->record_index.end()) {
            return Status(ErrorCode::DuplicateRecord, "restored history repeats a record identity");
        }
        RecordDraft draft;
        draft.record_id = record.header.record_id;
        draft.provenance = record.header.provenance;
        draft.body = record.body;
        BatchStaging staging;
        staging.next_sequence = state->last_sequence;
        auto staged = build_staged_entity(*state, staging, draft, record.header.sequence);
        if (!staged.ok()) {
            return staged.status();
        }
        auto payload_digest = record_body_digest(record.body, state->limits);
        if (!payload_digest.ok()) {
            return payload_digest.status();
        }
        if (!(payload_digest.value() == record.header.payload_digest)) {
            return Status(ErrorCode::IntegrityFailure,
                          "restored record payload digest does not match its payload");
        }
        const Digest expected_chain = next_chain_digest(chain, record.header);
        if (!(expected_chain == record.header.chain_digest)) {
            return Status(ErrorCode::IntegrityFailure,
                          "restored record chain digest does not match the committed chain");
        }
        chain = expected_chain;
        apply_staged_entity(*state, record, staged.value());
        state->chain_digest = chain;

        // Historical authority is preserved as history. It is never admitted as
        // current process authority: a worker that appended yesterday is not
        // authorized to append today just because its records were reloaded.
        const AuthorityEnvelope& authority = record.header.authority;
        auto worker = state->workers.find(authority.worker);
        if (worker == state->workers.end()) {
            WorkerAuthorityView view{};
            view.worker = authority.worker;
            view.boot = authority.worker_boot;
            view.epoch = authority.epoch;
            view.live = false;
            view.admitted_at = record.header.sequence;
            view.committed_records = 1;
            state->workers[authority.worker] = view;
        } else {
            worker->second.boot = authority.worker_boot;
            worker->second.epoch = authority.epoch;
            worker->second.committed_records += 1;
            worker->second.live = false;
        }
        state->historical_authority_records += 1;
    }
    // Restored history carries no live authority at all. The local authority is
    // re-established only for a ledger that does not require admission, because
    // that configuration appends on its own behalf; an admission-enforcing
    // ledger (the coordinator) must see every incarnation handshake again.
    if (!state->require_admission) {
        WorkerAuthorityView local{};
        local.worker = state->local_authority.worker;
        local.boot = state->local_authority.worker_boot;
        local.epoch = state->epoch;
        local.live = true;
        local.admitted_at = state->last_sequence;
        local.committed_records = 0;
        state->workers[state->local_authority.worker] = local;
    }
    return ledger;
}

Status Ledger::save(const std::string& path) const {
    std::vector<Record> copy;
    Limits limits{};
    LedgerGeneration generation{};
    CoordinatorEpoch epoch{};
    {
        std::shared_lock lock(state_->mutex);
        copy.assign(state_->records.begin(), state_->records.end());
        limits = state_->limits;
        generation = state_->generation;
        epoch = state_->epoch;
    }
    // No lock is held across the filesystem work.
    return save_snapshot_file(path, copy, generation, epoch, limits);
}

Result<std::shared_ptr<Ledger>> Ledger::load(const std::string& path, const LedgerConfig& config) {
    auto snapshot = load_snapshot_file(path, config.limits);
    if (!snapshot.ok()) {
        return snapshot.status();
    }
    LedgerConfig effective = config;
    effective.generation = snapshot.value().generation;
    effective.epoch = snapshot.value().epoch;
    return Ledger::restore(snapshot.value().records, effective);
}

Result<ReplayReport> replay_committed(std::span<const Record> records, const LedgerConfig& config) {
    auto restored = Ledger::restore(records, config);
    if (!restored.ok()) {
        return restored.status();
    }
    const std::shared_ptr<Ledger>& ledger = restored.value();
    auto snapshot = ledger->snapshot();
    ReplayReport report;
    report.records = records.size();
    report.last_sequence = ledger->last_sequence();
    auto stats = snapshot.stats();
    if (!stats.ok()) {
        return stats.status();
    }
    report.stats = stats.value();
    report.chain_digest = stats.value().chain_digest;
    report.logical_digest = ledger->logical_digest();
    auto integrity = snapshot.verify();
    if (!integrity.ok()) {
        return integrity.status();
    }
    report.integrity = integrity.value();
    return report;
}

}  // namespace research_ledger
