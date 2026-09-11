#pragma once

// Commit path: staging, validation and application of record drafts.
//
// Validation never mutates committed state. Every draft in a batch produces a
// staged entity that later drafts in the same batch can resolve; the batch is
// applied only after all of its drafts validated. A rejected batch leaves the
// previously committed history exactly as it was.

#include <cstddef>
#include <unordered_map>
#include <variant>
#include <vector>

#include "ledger_state.hpp"
#include "research_ledger/ledger.hpp"
#include "research_ledger/record.hpp"

namespace research_ledger {

struct StagedEntity {
    SubjectId subject{};
    ResearchSessionId session{};
    RecordSequence sequence{};
    RecordType type = RecordType::SessionOpened;
    std::variant<std::monostate, SessionView, HypothesisView, ExperimentView, BranchView, AttemptView,
                 ModelCallView, ToolCallView, ArtifactView, ObservationView, FailureView, DecisionView,
                 ResultView, MetricState>
        entity{};
    bool has_accounting = false;
    AccountingState accounting{};
};

struct BatchStaging {
    // Several staged records can concern the same subject (for example a
    // session declaration and a metric declared in the same session), so each
    // key resolves to a list and lookups select by entity type.
    std::unordered_map<SubjectKey, std::vector<std::size_t>, SubjectKeyHash> staged_by_subject{};
    std::vector<StagedEntity> staged{};
    std::vector<RecordSequence> sequences{};
    std::vector<LedgerRecordId> record_ids{};
    std::vector<Digest> payload_digests{};
    std::vector<Provenance> provenances{};
    std::vector<RecordBody> bodies{};
    RecordSequence next_sequence{};

    [[nodiscard]] const StagedEntity* find(const SubjectId& subject) const {
        const auto iterator = staged_by_subject.find(SubjectKey{subject.kind(), subject.value()});
        if (iterator == staged_by_subject.end() || iterator->second.empty()) {
            return nullptr;
        }
        return &staged[iterator->second.front()];
    }

    // Returns the staged entity of exactly this type for a subject, or null.
    // When a batch carries several revisions of one entity, the latest staged
    // revision is the one a later draft in the same batch must see.
    template <class View>
    [[nodiscard]] const View* find_as(const SubjectId& subject) const {
        const auto iterator = staged_by_subject.find(SubjectKey{subject.kind(), subject.value()});
        if (iterator == staged_by_subject.end()) {
            return nullptr;
        }
        for (auto entry = iterator->second.rbegin(); entry != iterator->second.rend(); ++entry) {
            if (const View* view = std::get_if<View>(&staged[*entry].entity)) {
                return view;
            }
        }
        return nullptr;
    }

    [[nodiscard]] bool has_record_id(LedgerRecordId id) const {
        for (const LedgerRecordId candidate : record_ids) {
            if (candidate == id) {
                return true;
            }
        }
        return false;
    }

    void stage(StagedEntity entity, LedgerRecordId record_id, Digest payload_digest,
               Provenance provenance, RecordBody body) {
        staged_by_subject[SubjectKey{entity.subject.kind(), entity.subject.value()}].push_back(
            staged.size());
        sequences.push_back(entity.sequence);
        record_ids.push_back(record_id);
        payload_digests.push_back(payload_digest);
        provenances.push_back(provenance);
        bodies.push_back(std::move(body));
        staged.push_back(std::move(entity));
    }
};

// Validates a draft against committed state plus the entities already staged in
// this batch and returns the entity it would declare.
Result<StagedEntity> build_staged_entity(const LedgerState& state, const BatchStaging& staging,
                                         const RecordDraft& draft, RecordSequence sequence);

// Applies a staged entity to committed state. This function cannot fail: every
// condition it depends on was checked by build_staged_entity.
void apply_staged_entity(LedgerState& state, const Record& record, const StagedEntity& staged);

// Truth for records derived from committed state.
Result<ResearchSessionId> committed_session_of(const LedgerState& state,
                                               const BatchStaging& staging,
                                               const SubjectId& subject);

}  // namespace research_ledger
