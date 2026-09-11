#include "ledger_commit.hpp"

#include <utility>

namespace research_ledger {
namespace {

void touch_session_collections(LedgerState& state, ResearchSessionId session) {
    state.session_hypotheses[session];
    state.session_experiments[session];
    state.session_results[session];
    state.session_branches[session];
    state.session_failures[session];
    state.session_metric_keys[session];
}

}  // namespace

void apply_staged_entity(LedgerState& state, const Record& record, const StagedEntity& staged) {
    const RecordSequence sequence = staged.sequence;
    state.records.push_back(record);
    state.record_subjects.push_back(staged.subject);
    state.record_index[record.header.record_id] = sequence;
    state.sequence_index[sequence] = state.records.size() - 1;
    const std::size_t type_index = static_cast<std::size_t>(staged.type);
    if (type_index < state.record_counts.size()) {
        state.record_counts[type_index] += 1;
    }
    state.last_sequence = sequence;
    if (staged.subject.valid()) {
        state.note_subject(staged.subject, staged.session, sequence);
    }

    switch (staged.type) {
        case RecordType::SessionOpened: {
            const SessionView* view = std::get_if<SessionView>(&staged.entity);
            if (view == nullptr) {
                return;
            }
            state.sessions[view->session] = *view;
            touch_session_collections(state, view->session);
            return;
        }
        case RecordType::SessionClosed:
        case RecordType::SessionAnnotation: {
            const SessionView* view = std::get_if<SessionView>(&staged.entity);
            if (view == nullptr) {
                return;
            }
            state.sessions[view->session] = *view;
            return;
        }
        case RecordType::HypothesisDeclared: {
            const HypothesisView* view = std::get_if<HypothesisView>(&staged.entity);
            if (view == nullptr) {
                return;
            }
            state.hypotheses[view->hypothesis] = *view;
            state.session_hypotheses[view->session].push_back(view->hypothesis);
            if (const auto session = state.sessions.find(view->session); session != state.sessions.end()) {
                session->second.hypothesis_count += 1;
            }
            if (view->parent.has_value()) {
                state.hypothesis_children[*view->parent].push_back(view->hypothesis);
            }
            return;
        }
        case RecordType::HypothesisStatusChanged: {
            const HypothesisView* view = std::get_if<HypothesisView>(&staged.entity);
            if (view == nullptr) {
                return;
            }
            state.hypotheses[view->hypothesis] = *view;
            return;
        }
        case RecordType::ExperimentDeclared: {
            const ExperimentView* view = std::get_if<ExperimentView>(&staged.entity);
            if (view == nullptr) {
                return;
            }
            state.experiments[view->experiment] = *view;
            state.session_experiments[view->session].push_back(view->experiment);
            if (const auto session = state.sessions.find(view->session); session != state.sessions.end()) {
                session->second.experiment_count += 1;
            }
            if (const auto branch = state.branches.find(view->branch); branch != state.branches.end()) {
                branch->second.experiments.push_back(view->experiment);
            }
            for (const HypothesisId hypothesis : view->hypotheses) {
                if (const auto entry = state.hypotheses.find(hypothesis);
                    entry != state.hypotheses.end()) {
                    entry->second.experiments.push_back(view->experiment);
                }
            }
            return;
        }
        case RecordType::BranchDeclared: {
            const BranchView* view = std::get_if<BranchView>(&staged.entity);
            if (view == nullptr) {
                return;
            }
            state.branches[view->branch] = *view;
            state.session_branches[view->session].push_back(view->branch);
            return;
        }
        case RecordType::AttemptStarted: {
            const AttemptView* view = std::get_if<AttemptView>(&staged.entity);
            if (view == nullptr) {
                return;
            }
            state.attempts[view->attempt] = *view;
            if (const auto experiment = state.experiments.find(view->experiment);
                experiment != state.experiments.end()) {
                experiment->second.attempts.push_back(view->attempt);
            }
            if (const auto branch = state.branches.find(view->branch); branch != state.branches.end()) {
                branch->second.attempt_count += 1;
            }
            return;
        }
        case RecordType::AttemptCompleted:
        case RecordType::AttemptFailed:
        case RecordType::AttemptCancelled: {
            const AttemptView* view = std::get_if<AttemptView>(&staged.entity);
            if (view == nullptr) {
                return;
            }
            state.attempts[view->attempt] = *view;
            return;
        }
        case RecordType::ModelCallRecorded: {
            const ModelCallView* view = std::get_if<ModelCallView>(&staged.entity);
            if (view == nullptr) {
                return;
            }
            state.model_calls[view->call] = *view;
            if (const auto attempt = state.attempts.find(view->attempt); attempt != state.attempts.end()) {
                attempt->second.model_call_count += 1;
            }
            return;
        }
        case RecordType::ToolCallRecorded: {
            const ToolCallView* view = std::get_if<ToolCallView>(&staged.entity);
            if (view == nullptr) {
                return;
            }
            state.tool_calls[view->call] = *view;
            if (const auto attempt = state.attempts.find(view->attempt); attempt != state.attempts.end()) {
                attempt->second.tool_call_count += 1;
            }
            return;
        }
        case RecordType::ArtifactReferenced: {
            const ArtifactView* view = std::get_if<ArtifactView>(&staged.entity);
            if (view == nullptr) {
                return;
            }
            ArtifactState& entry = state.artifacts[view->artifact];
            entry.view = *view;
            for (const ArtifactId parent : view->parents) {
                if (const auto parent_entry = state.artifacts.find(parent);
                    parent_entry != state.artifacts.end()) {
                    parent_entry->second.children.push_back(view->artifact);
                }
            }
            return;
        }
        case RecordType::ArtifactInvalidated: {
            const ArtifactView* view = std::get_if<ArtifactView>(&staged.entity);
            if (view == nullptr) {
                return;
            }
            if (const auto entry = state.artifacts.find(view->artifact); entry != state.artifacts.end()) {
                entry->second.view = *view;
                entry->second.invalidated_at = sequence;
            }
            return;
        }
        case RecordType::MetricDeclared: {
            const MetricState* metric = std::get_if<MetricState>(&staged.entity);
            if (metric == nullptr) {
                return;
            }
            state.metrics[metric->metric] = *metric;
            state.session_metric_keys[metric->session][metric->canonical_key] = metric->metric;
            return;
        }
        case RecordType::ObservationRecorded: {
            const ObservationView* view = std::get_if<ObservationView>(&staged.entity);
            if (view == nullptr) {
                return;
            }
            state.observations[view->observation] = *view;
            if (const auto attempt = state.attempts.find(view->attempt); attempt != state.attempts.end()) {
                attempt->second.observation_count += 1;
            }
            return;
        }
        case RecordType::FailureRecorded: {
            const FailureView* view = std::get_if<FailureView>(&staged.entity);
            if (view == nullptr) {
                return;
            }
            state.failures[view->failure] = *view;
            state.session_failures[view->session].push_back(view->failure);
            return;
        }
        case RecordType::DecisionRecorded: {
            const DecisionView* view = std::get_if<DecisionView>(&staged.entity);
            if (view == nullptr) {
                return;
            }
            state.decisions[view->decision] = *view;
            return;
        }
        case RecordType::ResultDeclared: {
            const ResultView* view = std::get_if<ResultView>(&staged.entity);
            if (view == nullptr) {
                return;
            }
            state.results[view->result] = *view;
            state.session_results[view->session].push_back(view->result);
            if (const auto session = state.sessions.find(view->session); session != state.sessions.end()) {
                session->second.result_count += 1;
            }
            return;
        }
        case RecordType::ResultStatusChanged: {
            const ResultView* view = std::get_if<ResultView>(&staged.entity);
            if (view == nullptr) {
                return;
            }
            state.results[view->result] = *view;
            return;
        }
        case RecordType::AccountingRecorded: {
            if (!staged.has_accounting) {
                return;
            }
            const std::size_t index = state.accounting.size();
            state.accounting.push_back(staged.accounting);
            state.accounting_by_scope[SubjectKey{staged.accounting.scope.kind(),
                                                 staged.accounting.scope.value()}]
                .push_back(index);
            return;
        }
    }
}

}  // namespace research_ledger
