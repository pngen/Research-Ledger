#include "ledger_commit.hpp"

#include <string>
#include <utility>

namespace research_ledger {
namespace {

Status require_text(const std::string& text, std::uint32_t maximum, const char* what) {
    if (text.size() > maximum) {
        return Status(ErrorCode::LimitExceeded, std::string(what) + " exceeds the configured maximum");
    }
    return Status{};
}

Status require_present(const std::string& text, const char* what) {
    if (text.empty()) {
        return Status(ErrorCode::InvalidArgument, std::string(what) + " must not be empty");
    }
    return Status{};
}

SubjectKey key_of(const SubjectId& subject) { return SubjectKey{subject.kind(), subject.value()}; }

template <class View, class Id>
const View* find_staged(const BatchStaging& staging, Id id) {
    return staging.find_as<View>(SubjectId::of(id));
}

const SessionView* find_session(const LedgerState& state, const BatchStaging& staging,
                                ResearchSessionId id) {
    if (const SessionView* staged = find_staged<SessionView>(staging, id)) {
        return staged;
    }
    const auto iterator = state.sessions.find(id);
    return iterator == state.sessions.end() ? nullptr : &iterator->second;
}

const HypothesisView* find_hypothesis(const LedgerState& state, const BatchStaging& staging,
                                      HypothesisId id) {
    if (const HypothesisView* staged = find_staged<HypothesisView>(staging, id)) {
        return staged;
    }
    const auto iterator = state.hypotheses.find(id);
    return iterator == state.hypotheses.end() ? nullptr : &iterator->second;
}

const ExperimentView* find_experiment(const LedgerState& state, const BatchStaging& staging,
                                      ExperimentId id) {
    if (const ExperimentView* staged = find_staged<ExperimentView>(staging, id)) {
        return staged;
    }
    const auto iterator = state.experiments.find(id);
    return iterator == state.experiments.end() ? nullptr : &iterator->second;
}

const BranchView* find_branch(const LedgerState& state, const BatchStaging& staging, BranchId id) {
    if (const BranchView* staged = find_staged<BranchView>(staging, id)) {
        return staged;
    }
    const auto iterator = state.branches.find(id);
    return iterator == state.branches.end() ? nullptr : &iterator->second;
}

const AttemptView* find_attempt(const LedgerState& state, const BatchStaging& staging, AttemptId id) {
    if (const AttemptView* staged = find_staged<AttemptView>(staging, id)) {
        return staged;
    }
    const auto iterator = state.attempts.find(id);
    return iterator == state.attempts.end() ? nullptr : &iterator->second;
}

const ModelCallView* find_model_call(const LedgerState& state, const BatchStaging& staging,
                                     ModelCallId id) {
    if (const ModelCallView* staged = find_staged<ModelCallView>(staging, id)) {
        return staged;
    }
    const auto iterator = state.model_calls.find(id);
    return iterator == state.model_calls.end() ? nullptr : &iterator->second;
}

const ToolCallView* find_tool_call(const LedgerState& state, const BatchStaging& staging,
                                   ToolCallId id) {
    if (const ToolCallView* staged = find_staged<ToolCallView>(staging, id)) {
        return staged;
    }
    const auto iterator = state.tool_calls.find(id);
    return iterator == state.tool_calls.end() ? nullptr : &iterator->second;
}

const ArtifactView* find_artifact(const LedgerState& state, const BatchStaging& staging,
                                  ArtifactId id) {
    if (const ArtifactView* staged = find_staged<ArtifactView>(staging, id)) {
        return staged;
    }
    const auto iterator = state.artifacts.find(id);
    return iterator == state.artifacts.end() ? nullptr : &iterator->second.view;
}

const ObservationView* find_observation(const LedgerState& state, const BatchStaging& staging,
                                        ObservationId id) {
    if (const ObservationView* staged = find_staged<ObservationView>(staging, id)) {
        return staged;
    }
    const auto iterator = state.observations.find(id);
    return iterator == state.observations.end() ? nullptr : &iterator->second;
}

// A metric is not a subject of its own: it is looked up in the batch overlay by
// identity rather than through the subject index.
const MetricState* find_metric(const LedgerState& state, const BatchStaging& staging, MetricId id) {
    for (const StagedEntity& staged : staging.staged) {
        if (const MetricState* metric = std::get_if<MetricState>(&staged.entity)) {
            if (metric->metric == id) {
                return metric;
            }
        }
    }
    const auto iterator = state.metrics.find(id);
    return iterator == state.metrics.end() ? nullptr : &iterator->second;
}

const FailureView* find_failure(const LedgerState& state, const BatchStaging& staging, FailureId id) {
    if (const FailureView* staged = find_staged<FailureView>(staging, id)) {
        return staged;
    }
    const auto iterator = state.failures.find(id);
    return iterator == state.failures.end() ? nullptr : &iterator->second;
}

const DecisionView* find_decision(const LedgerState& state, const BatchStaging& staging,
                                  DecisionId id) {
    if (const DecisionView* staged = find_staged<DecisionView>(staging, id)) {
        return staged;
    }
    const auto iterator = state.decisions.find(id);
    return iterator == state.decisions.end() ? nullptr : &iterator->second;
}

const ResultView* find_result(const LedgerState& state, const BatchStaging& staging, ResultId id) {
    if (const ResultView* staged = find_staged<ResultView>(staging, id)) {
        return staged;
    }
    const auto iterator = state.results.find(id);
    return iterator == state.results.end() ? nullptr : &iterator->second;
}

bool subject_exists(const LedgerState& state, const BatchStaging& staging, const SubjectId& subject) {
    if (!subject.valid()) {
        return false;
    }
    if (staging.find(subject) != nullptr) {
        return true;
    }
    return state.subject_index.find(key_of(subject)) != state.subject_index.end();
}

Result<SubjectLocation> locate_subject(const LedgerState& state, const BatchStaging& staging,
                                       const SubjectId& subject) {
    if (!subject.valid()) {
        return Status(ErrorCode::InvalidIdentity, "subject identity is not valid");
    }
    if (const StagedEntity* staged = staging.find(subject)) {
        return SubjectLocation{staged->session, staged->sequence};
    }
    const auto iterator = state.subject_index.find(key_of(subject));
    if (iterator == state.subject_index.end()) {
        return Status(ErrorCode::MissingDependency,
                      "referenced subject " + subject.to_string() + " is not committed");
    }
    return iterator->second;
}

// A decision may only cite a record that is already committed, that concerns
// the subject it names, and that precedes the decision. Evidence added later
// cannot become evidence an earlier decision considered.
Status check_evidence(const LedgerState& state, const EvidenceRef& evidence,
                      ResearchSessionId session, RecordSequence decision_sequence) {
    if (!evidence.subject.valid()) {
        return Status(ErrorCode::InvalidIdentity, "evidence subject is not valid");
    }
    if (!evidence.sequence.valid()) {
        return Status(ErrorCode::InvalidIdentity, "evidence sequence is not valid");
    }
    if (evidence.sequence.value() > state.last_sequence.value()) {
        return Status(ErrorCode::InvalidTransition,
                      "a decision cannot cite evidence that is not committed yet");
    }
    if (decision_sequence.valid() && evidence.sequence.value() >= decision_sequence.value()) {
        return Status(ErrorCode::InvalidTransition,
                      "a decision cannot cite evidence at or after its own sequence");
    }
    const std::size_t index = static_cast<std::size_t>(evidence.sequence.value()) - 1;
    if (index >= state.records.size()) {
        return Status(ErrorCode::MissingDependency,
                      "evidence sequence is outside the committed history");
    }
    if (!(state.record_subjects[index] == evidence.subject)) {
        return Status(ErrorCode::BrokenLineage, "evidence sequence does not concern the cited subject");
    }
    const auto location = state.subject_index.find(key_of(evidence.subject));
    if (location == state.subject_index.end()) {
        return Status(ErrorCode::MissingDependency, "cited evidence subject is not committed");
    }
    if (!(location->second.session == session)) {
        return Status(ErrorCode::CrossSessionReference,
                      "evidence belongs to a different research session");
    }
    return Status{};
}

Status same_session(ResearchSessionId lhs, ResearchSessionId rhs, const char* what) {
    if (!(lhs == rhs)) {
        return Status(ErrorCode::CrossSessionReference,
                      std::string(what) + " belongs to a different research session");
    }
    return Status{};
}

SubjectId primary_subject(const RecordBody& body) {
    return std::visit(
        [](const auto& payload) -> SubjectId {
            using Payload = std::decay_t<decltype(payload)>;
            if constexpr (std::is_same_v<Payload, SessionOpened> ||
                          std::is_same_v<Payload, SessionClosed> ||
                          std::is_same_v<Payload, SessionAnnotation>) {
                return SubjectId::of(payload.session);
            } else if constexpr (std::is_same_v<Payload, HypothesisDeclared> ||
                                 std::is_same_v<Payload, HypothesisStatusChanged>) {
                return SubjectId::of(payload.hypothesis);
            } else if constexpr (std::is_same_v<Payload, ExperimentDeclared>) {
                return SubjectId::of(payload.experiment);
            } else if constexpr (std::is_same_v<Payload, BranchDeclared>) {
                return SubjectId::of(payload.branch);
            } else if constexpr (std::is_same_v<Payload, AttemptStarted> ||
                                 std::is_same_v<Payload, AttemptCompleted> ||
                                 std::is_same_v<Payload, AttemptFailed> ||
                                 std::is_same_v<Payload, AttemptCancelled>) {
                return SubjectId::of(payload.attempt);
            } else if constexpr (std::is_same_v<Payload, ModelCallRecorded>) {
                return SubjectId::of(payload.call);
            } else if constexpr (std::is_same_v<Payload, ToolCallRecorded>) {
                return SubjectId::of(payload.call);
            } else if constexpr (std::is_same_v<Payload, ArtifactReferenced> ||
                                 std::is_same_v<Payload, ArtifactInvalidated>) {
                return SubjectId::of(payload.artifact);
            } else if constexpr (std::is_same_v<Payload, MetricDeclared>) {
                // A metric declaration concerns its research session.
                return SubjectId::of(payload.session);
            } else if constexpr (std::is_same_v<Payload, ObservationRecorded>) {
                return SubjectId::of(payload.observation);
            } else if constexpr (std::is_same_v<Payload, FailureRecorded>) {
                return SubjectId::of(payload.failure);
            } else if constexpr (std::is_same_v<Payload, DecisionRecorded>) {
                return SubjectId::of(payload.decision);
            } else if constexpr (std::is_same_v<Payload, ResultDeclared> ||
                                 std::is_same_v<Payload, ResultStatusChanged>) {
                return SubjectId::of(payload.result);
            } else {
                return payload.scope;
            }
        },
        body);
}

Status attempt_not_cancelled(const AttemptView& attempt) {
    if (attempt.state == AttemptState::Cancelled) {
        return Status(ErrorCode::Cancelled,
                      "a cancelled attempt cannot acquire new authoritative activity");
    }
    return Status{};
}

}  // namespace

Result<ResearchSessionId> committed_session_of(const LedgerState& state,
                                               const BatchStaging& staging,
                                               const SubjectId& subject) {
    auto location = locate_subject(state, staging, subject);
    if (!location.ok()) {
        return location.status();
    }
    return location.value().session;
}

namespace {

Result<StagedEntity> build_staged_entity_unchecked(const LedgerState& state,
                                                   const BatchStaging& staging,
                                                   const RecordDraft& draft,
                                                   RecordSequence sequence) {
    StagedEntity staged;
    staged.sequence = sequence;
    staged.type = record_type_of(draft.body);
    staged.subject = primary_subject(draft.body);
    if (!staged.subject.valid()) {
        return Status(ErrorCode::InvalidIdentity, "record subject identity is not valid");
    }

    const Limits& limits = state.limits;

    switch (staged.type) {
        case RecordType::SessionOpened: {
            const auto& body = std::get<SessionOpened>(draft.body);
            if (!body.session.valid()) {
                return Status(ErrorCode::InvalidIdentity, "research session identity is not valid");
            }
            if (find_session(state, staging, body.session) != nullptr) {
                return Status(ErrorCode::DuplicateRecord, "research session is already open");
            }
            Status status = require_text(body.label, limits.max_label_length, "session label");
            if (!status.ok()) return status;
            status = require_text(body.question, limits.max_string_length, "research question");
            if (!status.ok()) return status;
            status = require_text(body.authority, limits.max_string_length, "authority");
            if (!status.ok()) return status;
            if (body.metadata.size() > limits.max_metadata_entries) {
                return Status(ErrorCode::LimitExceeded, "metadata exceeds the configured maximum");
            }
            for (const MetadataEntry& entry : body.metadata) {
                status = require_text(entry.key, limits.max_metadata_key_length, "metadata key");
                if (!status.ok()) return status;
                status = require_text(entry.value, limits.max_metadata_value_length, "metadata value");
                if (!status.ok()) return status;
            }
            SessionView view;
            view.session = body.session;
            view.label = body.label;
            view.question = body.question;
            view.metadata = body.metadata;
            view.state = SessionState::Open;
            view.opened_at = sequence;
            view.provenance = draft.provenance;
            view.authority = body.authority;
            staged.session = body.session;
            staged.entity = std::move(view);
            return staged;
        }
        case RecordType::SessionClosed:
        case RecordType::SessionAnnotation: {
            const ResearchSessionId session = std::holds_alternative<SessionClosed>(draft.body)
                                                  ? std::get<SessionClosed>(draft.body).session
                                                  : std::get<SessionAnnotation>(draft.body).session;
            const std::string note = std::holds_alternative<SessionClosed>(draft.body)
                                         ? std::get<SessionClosed>(draft.body).note
                                         : std::get<SessionAnnotation>(draft.body).note;
            const SessionView* existing = find_session(state, staging, session);
            if (existing == nullptr) {
                return Status(ErrorCode::MissingDependency, "research session does not exist");
            }
            Status status = require_text(note, limits.max_string_length, "session note");
            if (!status.ok()) return status;
            SessionView view = *existing;
            staged.session = session;
            if (staged.type == RecordType::SessionClosed) {
                if (view.state == SessionState::Closed) {
                    return Status(ErrorCode::AlreadyTerminal, "research session is already closed");
                }
                view.state = SessionState::Closed;
                view.closed_at = sequence;
            }
            staged.entity = std::move(view);
            return staged;
        }
        case RecordType::HypothesisDeclared: {
            const auto& body = std::get<HypothesisDeclared>(draft.body);
            if (!body.hypothesis.valid() || !body.generation.valid()) {
                return Status(ErrorCode::InvalidIdentity, "hypothesis identity is not valid");
            }
            if (find_hypothesis(state, staging, body.hypothesis) != nullptr) {
                return Status(ErrorCode::DuplicateRecord, "hypothesis already exists");
            }
            if (!(body.generation == HypothesisGeneration::first())) {
                return Status(ErrorCode::StaleGeneration,
                              "a hypothesis declaration must be its first generation");
            }
            if (find_session(state, staging, body.session) == nullptr) {
                return Status(ErrorCode::MissingDependency, "research session does not exist");
            }
            Status status = require_present(body.claim, "hypothesis claim");
            if (!status.ok()) return status;
            status = require_text(body.claim, limits.max_string_length, "hypothesis claim");
            if (!status.ok()) return status;
            status = require_text(body.authority, limits.max_string_length, "authority");
            if (!status.ok()) return status;

            HypothesisView view;
            view.hypothesis = body.hypothesis;
            view.generation = body.generation;
            view.session = body.session;
            view.claim = body.claim;
            view.status = HypothesisStatus::Proposed;
            view.declared_at = sequence;
            view.last_changed_at = sequence;
            view.provenance = draft.provenance;
            view.authority = body.authority;
            if (body.parent.has_value()) {
                const HypothesisView* parent = find_hypothesis(state, staging, *body.parent);
                if (parent == nullptr) {
                    return Status(ErrorCode::MissingDependency, "parent hypothesis does not exist");
                }
                status = same_session(parent->session, body.session, "parent hypothesis");
                if (!status.ok()) return status;
                if (!(parent->generation == body.parent_generation)) {
                    return Status(ErrorCode::StaleGeneration,
                                  "parent hypothesis generation is not current");
                }
                view.parent = *body.parent;
                view.parent_generation = body.parent_generation;
            }
            staged.session = body.session;
            staged.entity = std::move(view);
            return staged;
        }
        case RecordType::HypothesisStatusChanged: {
            const auto& body = std::get<HypothesisStatusChanged>(draft.body);
            const HypothesisView* existing = find_hypothesis(state, staging, body.hypothesis);
            if (existing == nullptr) {
                return Status(ErrorCode::MissingDependency, "hypothesis does not exist");
            }
            if (!(body.generation == existing->generation.next())) {
                return Status(ErrorCode::StaleGeneration, "hypothesis generation is not the next one");
            }
            if (!(body.from == existing->status)) {
                return Status(ErrorCode::InvalidTransition,
                              "hypothesis transition does not start from the current status");
            }
            if (!hypothesis_transition_allowed(body.from, body.to)) {
                return Status(ErrorCode::InvalidTransition, "hypothesis transition is not permitted");
            }
            Status status = require_text(body.authority, limits.max_string_length, "authority");
            if (!status.ok()) return status;
            if (body.decision.has_value()) {
                const DecisionView* decision = find_decision(state, staging, *body.decision);
                if (decision == nullptr) {
                    return Status(ErrorCode::MissingDependency, "referenced decision does not exist");
                }
                if (!(decision->subject == SubjectId::of(body.hypothesis))) {
                    return Status(ErrorCode::InvalidTransition,
                                  "decision does not concern this hypothesis");
                }
            }
            HypothesisView view = *existing;
            view.generation = body.generation;
            view.status = body.to;
            view.last_changed_at = sequence;
            view.provenance = draft.provenance;
            view.authority = body.authority;
            staged.session = view.session;
            staged.entity = std::move(view);
            return staged;
        }
        case RecordType::ExperimentDeclared: {
            const auto& body = std::get<ExperimentDeclared>(draft.body);
            if (!body.experiment.valid() || !body.generation.valid()) {
                return Status(ErrorCode::InvalidIdentity, "experiment identity is not valid");
            }
            if (find_experiment(state, staging, body.experiment) != nullptr) {
                return Status(ErrorCode::DuplicateRecord, "experiment already exists");
            }
            if (!(body.generation == ExperimentGeneration::first())) {
                return Status(ErrorCode::StaleGeneration,
                              "an experiment declaration must be its first generation");
            }
            if (find_session(state, staging, body.session) == nullptr) {
                return Status(ErrorCode::MissingDependency, "research session does not exist");
            }
            if (body.hypotheses.empty()) {
                return Status(ErrorCode::InvalidArgument,
                              "an experiment must name at least one hypothesis");
            }
            if (body.hypotheses.size() > limits.max_hypotheses_per_experiment) {
                return Status(ErrorCode::LimitExceeded, "hypothesis list exceeds the configured maximum");
            }
            for (std::size_t index = 0; index < body.hypotheses.size(); ++index) {
                const HypothesisId hypothesis = body.hypotheses[index];
                if (!hypothesis.valid()) {
                    return Status(ErrorCode::InvalidIdentity, "hypothesis identity is not valid");
                }
                for (std::size_t earlier = 0; earlier < index; ++earlier) {
                    if (body.hypotheses[earlier] == hypothesis) {
                        return Status(ErrorCode::InvalidArgument, "hypothesis list repeats an entry");
                    }
                }
                const HypothesisView* view = find_hypothesis(state, staging, hypothesis);
                if (view == nullptr) {
                    return Status(ErrorCode::MissingDependency, "hypothesis does not exist");
                }
                Status status = same_session(view->session, body.session, "hypothesis");
                if (!status.ok()) return status;
            }
            const BranchView* branch = find_branch(state, staging, body.branch);
            if (branch == nullptr) {
                return Status(ErrorCode::MissingDependency, "branch does not exist");
            }
            Status status = same_session(branch->session, body.session, "branch");
            if (!status.ok()) return status;
            if (body.parent_experiment.has_value()) {
                const ExperimentView* parent = find_experiment(state, staging, *body.parent_experiment);
                if (parent == nullptr) {
                    return Status(ErrorCode::MissingDependency, "parent experiment does not exist");
                }
                status = same_session(parent->session, body.session, "parent experiment");
                if (!status.ok()) return status;
            }
            status = require_text(body.environment_reference, limits.max_string_length,
                                  "environment reference");
            if (!status.ok()) return status;
            status = require_text(body.authority, limits.max_string_length, "authority");
            if (!status.ok()) return status;
            if (body.inputs.size() > limits.max_inputs) {
                return Status(ErrorCode::LimitExceeded, "input list exceeds the configured maximum");
            }
            for (const ExperimentInput& input : body.inputs) {
                if (!input.input.valid()) {
                    return Status(ErrorCode::InvalidIdentity, "input identity is not valid");
                }
                status = require_text(input.reference, limits.max_string_length, "input reference");
                if (!status.ok()) return status;
            }
            if (body.expected_outputs.size() > limits.max_expected_outputs) {
                return Status(ErrorCode::LimitExceeded,
                              "expected output list exceeds the configured maximum");
            }
            for (const std::string& output : body.expected_outputs) {
                status = require_text(output, limits.max_string_length, "expected output");
                if (!status.ok()) return status;
            }

            ExperimentView view;
            view.experiment = body.experiment;
            view.generation = body.generation;
            view.session = body.session;
            view.hypotheses = body.hypotheses;
            view.branch = body.branch;
            view.parent_experiment = body.parent_experiment;
            view.environment_reference = body.environment_reference;
            view.inputs = body.inputs;
            view.expected_outputs = body.expected_outputs;
            view.declared_at = sequence;
            view.provenance = draft.provenance;
            view.authority = body.authority;
            staged.session = body.session;
            staged.entity = std::move(view);
            return staged;
        }
        case RecordType::BranchDeclared: {
            const auto& body = std::get<BranchDeclared>(draft.body);
            if (!body.branch.valid()) {
                return Status(ErrorCode::InvalidIdentity, "branch identity is not valid");
            }
            if (find_branch(state, staging, body.branch) != nullptr) {
                return Status(ErrorCode::DuplicateRecord, "branch already exists");
            }
            if (find_session(state, staging, body.session) == nullptr) {
                return Status(ErrorCode::MissingDependency, "research session does not exist");
            }
            const bool root = body.kind == BranchKind::Root;
            if (root && body.parent_branch.has_value()) {
                return Status(ErrorCode::InvalidArgument, "a root branch cannot have a parent");
            }
            if (!root && !body.parent_branch.has_value()) {
                return Status(ErrorCode::InvalidArgument, "a derived branch must name its parent");
            }
            Status status = require_text(body.label, limits.max_label_length, "branch label");
            if (!status.ok()) return status;
            status = require_text(body.authority, limits.max_string_length, "authority");
            if (!status.ok()) return status;
            if (body.parent_branch.has_value()) {
                if (*body.parent_branch == body.branch) {
                    return Status(ErrorCode::LineageCycle, "a branch cannot be its own parent");
                }
                const BranchView* parent = find_branch(state, staging, *body.parent_branch);
                if (parent == nullptr) {
                    return Status(ErrorCode::MissingDependency, "parent branch does not exist");
                }
                status = same_session(parent->session, body.session, "parent branch");
                if (!status.ok()) return status;
            }
            BranchView view;
            view.branch = body.branch;
            view.session = body.session;
            view.kind = body.kind;
            view.parent_branch = body.parent_branch;
            view.label = body.label;
            view.declared_at = sequence;
            view.provenance = draft.provenance;
            view.authority = body.authority;
            staged.session = body.session;
            staged.entity = std::move(view);
            return staged;
        }
        case RecordType::AttemptStarted: {
            const auto& body = std::get<AttemptStarted>(draft.body);
            if (!body.attempt.valid() || !body.generation.valid()) {
                return Status(ErrorCode::InvalidIdentity, "attempt identity is not valid");
            }
            if (find_attempt(state, staging, body.attempt) != nullptr) {
                return Status(ErrorCode::DuplicateRecord, "attempt already exists");
            }
            if (!(body.generation == AttemptGeneration::first())) {
                return Status(ErrorCode::StaleGeneration, "an attempt start must be its first generation");
            }
            const ExperimentView* experiment = find_experiment(state, staging, body.experiment);
            if (experiment == nullptr) {
                return Status(ErrorCode::MissingDependency, "experiment does not exist");
            }
            const BranchView* branch = find_branch(state, staging, body.branch);
            if (branch == nullptr) {
                return Status(ErrorCode::MissingDependency, "branch does not exist");
            }
            if (!(experiment->branch == body.branch)) {
                return Status(ErrorCode::InvalidArgument, "attempt branch is not the experiment branch");
            }
            Status status = same_session(branch->session, experiment->session, "attempt branch");
            if (!status.ok()) return status;
            status = require_text(body.worker_authority, limits.max_string_length, "worker authority");
            if (!status.ok()) return status;

            AttemptView view;
            view.attempt = body.attempt;
            view.generation = body.generation;
            view.experiment = body.experiment;
            view.branch = body.branch;
            view.state = AttemptState::Running;
            view.worker_authority = body.worker_authority;
            view.started_at = sequence;
            view.provenance = draft.provenance;
            staged.session = experiment->session;
            staged.entity = std::move(view);
            return staged;
        }
        case RecordType::AttemptCompleted:
        case RecordType::AttemptFailed:
        case RecordType::AttemptCancelled: {
            AttemptId identity{};
            AttemptGeneration generation{};
            AttemptState target = AttemptState::Completed;
            if (staged.type == RecordType::AttemptCompleted) {
                identity = std::get<AttemptCompleted>(draft.body).attempt;
                generation = std::get<AttemptCompleted>(draft.body).generation;
            } else if (staged.type == RecordType::AttemptFailed) {
                identity = std::get<AttemptFailed>(draft.body).attempt;
                generation = std::get<AttemptFailed>(draft.body).generation;
                target = AttemptState::Failed;
            } else {
                identity = std::get<AttemptCancelled>(draft.body).attempt;
                generation = std::get<AttemptCancelled>(draft.body).generation;
                target = AttemptState::Cancelled;
            }
            if (!generation.valid()) {
                return Status(ErrorCode::InvalidIdentity, "attempt generation zero is never valid");
            }
            const AttemptView* existing = find_attempt(state, staging, identity);
            if (existing == nullptr) {
                return Status(ErrorCode::MissingDependency, "attempt does not exist");
            }
            if (!(existing->generation == generation)) {
                return Status(ErrorCode::StaleAttempt, "attempt generation is not current");
            }
            if (existing->state != AttemptState::Running) {
                if (existing->state == target) {
                    return Status(ErrorCode::DuplicateCompletion, "attempt already reached this state");
                }
                if (existing->state == AttemptState::Cancelled) {
                    return Status(ErrorCode::Cancelled,
                                  "a cancelled attempt cannot later become a different terminal state");
                }
                return Status(ErrorCode::AlreadyTerminal, "attempt already reached a terminal state");
            }
            AttemptView view = *existing;
            view.generation = generation;
            view.state = target;
            view.terminated_at = sequence;
            view.provenance = draft.provenance;
            if (staged.type == RecordType::AttemptFailed) {
                const auto& body = std::get<AttemptFailed>(draft.body);
                const FailureView* failure = find_failure(state, staging, body.failure);
                if (failure == nullptr) {
                    return Status(ErrorCode::MissingDependency, "failure does not exist");
                }
                if (!(failure->scope == SubjectId::of(identity))) {
                    return Status(ErrorCode::InvalidTransition,
                                  "failure does not concern this attempt");
                }
                view.failure = body.failure;
            } else if (staged.type == RecordType::AttemptCancelled) {
                Status status = require_text(std::get<AttemptCancelled>(draft.body).reason,
                                             limits.max_string_length, "cancellation reason");
                if (!status.ok()) return status;
            } else {
                Status status = require_text(std::get<AttemptCompleted>(draft.body).outcome_reference,
                                             limits.max_string_length, "outcome reference");
                if (!status.ok()) return status;
            }
            const ExperimentView* experiment = find_experiment(state, staging, view.experiment);
            if (experiment == nullptr) {
                return Status(ErrorCode::MissingDependency, "attempt experiment does not exist");
            }
            staged.session = experiment->session;
            staged.entity = std::move(view);
            return staged;
        }
        case RecordType::ModelCallRecorded: {
            const auto& body = std::get<ModelCallRecorded>(draft.body);
            if (!body.call.valid()) {
                return Status(ErrorCode::InvalidIdentity, "model call identity is not valid");
            }
            if (find_model_call(state, staging, body.call) != nullptr) {
                return Status(ErrorCode::DuplicateRecord, "model call already exists");
            }
            const AttemptView* attempt = find_attempt(state, staging, body.attempt);
            if (attempt == nullptr) {
                return Status(ErrorCode::MissingDependency, "attempt does not exist");
            }
            Status status = attempt_not_cancelled(*attempt);
            if (!status.ok()) return status;
            const ExperimentView* experiment = find_experiment(state, staging, attempt->experiment);
            if (experiment == nullptr) {
                return Status(ErrorCode::MissingDependency, "attempt experiment does not exist");
            }
            status = require_present(body.model_identity, "model identity");
            if (!status.ok()) return status;
            status = require_text(body.model_identity, limits.max_string_length, "model identity");
            if (!status.ok()) return status;
            status = require_text(body.model_revision, limits.max_string_length, "model revision");
            if (!status.ok()) return status;
            status = require_text(body.provider, limits.max_string_length, "provider identity");
            if (!status.ok()) return status;
            status = require_text(body.configuration_digest, limits.max_string_length,
                                  "configuration digest");
            if (!status.ok()) return status;
            status = require_text(body.authority, limits.max_string_length, "authority");
            if (!status.ok()) return status;
            if (body.outcome == ModelCallOutcome::Invalid) {
                return Status(ErrorCode::InvalidArgument, "model call outcome must be stated");
            }
            if (body.parent_calls.size() > limits.max_calls_per_attempt) {
                return Status(ErrorCode::LimitExceeded, "parent call list exceeds the configured maximum");
            }
            for (const ModelCallId parent : body.parent_calls) {
                const ModelCallView* parent_view = find_model_call(state, staging, parent);
                if (parent_view == nullptr) {
                    return Status(ErrorCode::MissingDependency, "parent model call does not exist");
                }
                const AttemptView* parent_attempt =
                    find_attempt(state, staging, parent_view->attempt);
                if (parent_attempt == nullptr) {
                    return Status(ErrorCode::MissingDependency, "parent call attempt does not exist");
                }
                const ExperimentView* parent_experiment =
                    find_experiment(state, staging, parent_attempt->experiment);
                if (parent_experiment == nullptr) {
                    return Status(ErrorCode::MissingDependency,
                                  "parent call experiment does not exist");
                }
                status = same_session(parent_experiment->session, experiment->session,
                                      "parent model call");
                if (!status.ok()) return status;
                if (parent == body.call) {
                    return Status(ErrorCode::LineageCycle, "a model call cannot be its own parent");
                }
            }

            ModelCallView view;
            view.call = body.call;
            view.attempt = body.attempt;
            view.model_identity = body.model_identity;
            view.model_revision = body.model_revision;
            view.provider = body.provider;
            view.configuration_digest = body.configuration_digest;
            view.input_reference = body.input_reference;
            view.output_reference = body.output_reference;
            view.input_tokens = body.input_tokens;
            view.output_tokens = body.output_tokens;
            view.latency = body.latency;
            view.cost = body.cost;
            view.outcome = body.outcome;
            view.parent_calls = body.parent_calls;
            view.recorded_at = sequence;
            view.provenance = draft.provenance;
            view.authority = body.authority;
            staged.session = experiment->session;
            staged.entity = std::move(view);
            return staged;
        }
        case RecordType::ToolCallRecorded: {
            const auto& body = std::get<ToolCallRecorded>(draft.body);
            if (!body.call.valid()) {
                return Status(ErrorCode::InvalidIdentity, "tool call identity is not valid");
            }
            if (find_tool_call(state, staging, body.call) != nullptr) {
                return Status(ErrorCode::DuplicateRecord, "tool call already exists");
            }
            const AttemptView* attempt = find_attempt(state, staging, body.attempt);
            if (attempt == nullptr) {
                return Status(ErrorCode::MissingDependency, "attempt does not exist");
            }
            Status status = attempt_not_cancelled(*attempt);
            if (!status.ok()) return status;
            const ExperimentView* experiment = find_experiment(state, staging, attempt->experiment);
            if (experiment == nullptr) {
                return Status(ErrorCode::MissingDependency, "attempt experiment does not exist");
            }
            status = require_present(body.tool_identity, "tool identity");
            if (!status.ok()) return status;
            status = require_text(body.tool_identity, limits.max_string_length, "tool identity");
            if (!status.ok()) return status;
            status = require_text(body.tool_version, limits.max_string_length, "tool version");
            if (!status.ok()) return status;
            status = require_text(body.authority, limits.max_string_length, "authority");
            if (!status.ok()) return status;
            if (body.state == ToolCallState::Invalid) {
                return Status(ErrorCode::InvalidArgument, "tool call state must be stated");
            }

            ToolCallView view;
            view.call = body.call;
            view.attempt = body.attempt;
            view.tool_identity = body.tool_identity;
            view.tool_version = body.tool_version;
            view.request_reference = body.request_reference;
            view.output_reference = body.output_reference;
            view.state = body.state;
            view.accounting = body.accounting;
            view.recorded_at = sequence;
            view.provenance = draft.provenance;
            view.authority = body.authority;
            staged.session = experiment->session;
            staged.entity = std::move(view);
            return staged;
        }
        case RecordType::ArtifactReferenced: {
            const auto& body = std::get<ArtifactReferenced>(draft.body);
            if (!body.artifact.valid() || !body.generation.valid()) {
                return Status(ErrorCode::InvalidIdentity, "artifact identity is not valid");
            }
            if (find_session(state, staging, body.session) == nullptr) {
                return Status(ErrorCode::MissingDependency, "research session does not exist");
            }
            const ArtifactView* existing = find_artifact(state, staging, body.artifact);
            if (existing == nullptr) {
                if (!(body.generation == ArtifactGeneration::first())) {
                    return Status(ErrorCode::StaleGeneration,
                                  "a first artifact reference must be its first generation");
                }
            } else if (!(body.generation == existing->generation.next())) {
                return Status(ErrorCode::StaleGeneration, "artifact generation is not the next one");
            }
            if (body.content_digest.is_zero()) {
                return Status(ErrorCode::InvalidArgument, "artifact content digest must be stated");
            }
            if (body.role == ArtifactRole::Invalid) {
                return Status(ErrorCode::InvalidArgument, "artifact role must be stated");
            }
            if (body.validation == ValidationState::Invalid) {
                return Status(ErrorCode::InvalidArgument, "artifact validation state must be stated");
            }
            if (!body.producer.valid()) {
                return Status(ErrorCode::InvalidIdentity, "artifact producer is not valid");
            }
            switch (body.producer.kind()) {
                case SubjectKind::Session:
                case SubjectKind::Experiment:
                case SubjectKind::Attempt:
                case SubjectKind::ModelCall:
                case SubjectKind::ToolCall:
                case SubjectKind::Artifact:
                    break;
                default:
                    return Status(ErrorCode::InvalidArgument,
                                  "artifact producer must be a session, experiment, attempt, call or artifact");
            }
            Status status = require_text(body.media_type, limits.max_string_length, "media type");
            if (!status.ok()) return status;
            status = require_text(body.location, limits.max_location_length, "artifact location");
            if (!status.ok()) return status;
            status = require_text(body.authority, limits.max_string_length, "authority");
            if (!status.ok()) return status;
            if (body.parents.size() > limits.max_parents_per_artifact) {
                return Status(ErrorCode::LimitExceeded, "artifact parent list exceeds the configured maximum");
            }
            auto producer_location = locate_subject(state, staging, body.producer);
            if (!producer_location.ok()) {
                return producer_location.status();
            }
            status = same_session(producer_location.value().session, body.session, "artifact producer");
            if (!status.ok()) return status;
            for (std::size_t index = 0; index < body.parents.size(); ++index) {
                const ArtifactId parent = body.parents[index];
                if (!parent.valid()) {
                    return Status(ErrorCode::InvalidIdentity, "artifact parent identity is not valid");
                }
                if (parent == body.artifact) {
                    return Status(ErrorCode::LineageCycle, "an artifact cannot be its own parent");
                }
                for (std::size_t earlier = 0; earlier < index; ++earlier) {
                    if (body.parents[earlier] == parent) {
                        return Status(ErrorCode::InvalidArgument, "artifact parent list repeats an entry");
                    }
                }
                const ArtifactView* parent_view = find_artifact(state, staging, parent);
                if (parent_view == nullptr) {
                    return Status(ErrorCode::MissingDependency, "artifact parent does not exist");
                }
                status = same_session(parent_view->session, body.session, "artifact parent");
                if (!status.ok()) return status;
            }

            ArtifactView view;
            view.artifact = body.artifact;
            view.generation = body.generation;
            view.session = body.session;
            view.content_digest = body.content_digest;
            view.role = body.role;
            view.media_type = body.media_type;
            view.producer = body.producer;
            view.location = body.location;
            view.parents = body.parents;
            view.validation = body.validation;
            view.recorded_at = sequence;
            view.provenance = draft.provenance;
            view.authority = body.authority;
            staged.session = body.session;
            staged.entity = std::move(view);
            return staged;
        }
        case RecordType::ArtifactInvalidated: {
            const auto& body = std::get<ArtifactInvalidated>(draft.body);
            const ArtifactView* existing = find_artifact(state, staging, body.artifact);
            if (existing == nullptr) {
                return Status(ErrorCode::MissingDependency, "artifact does not exist");
            }
            if (!(existing->generation == body.generation)) {
                return Status(ErrorCode::StaleGeneration, "artifact generation is not current");
            }
            if (existing->validation == ValidationState::Invalidated) {
                return Status(ErrorCode::AlreadyTerminal, "artifact is already invalidated");
            }
            Status status = require_text(body.reason, limits.max_string_length, "invalidation reason");
            if (!status.ok()) return status;
            if (body.failure.has_value() && find_failure(state, staging, *body.failure) == nullptr) {
                return Status(ErrorCode::MissingDependency, "referenced failure does not exist");
            }
            if (body.decision.has_value()) {
                const DecisionView* decision = find_decision(state, staging, *body.decision);
                if (decision == nullptr) {
                    return Status(ErrorCode::MissingDependency, "referenced decision does not exist");
                }
                if (!(decision->subject == SubjectId::of(body.artifact))) {
                    return Status(ErrorCode::InvalidTransition,
                                  "decision does not concern this artifact");
                }
            }
            ArtifactView view = *existing;
            view.validation = ValidationState::Invalidated;
            staged.session = view.session;
            staged.entity = std::move(view);
            return staged;
        }
        case RecordType::MetricDeclared: {
            const auto& body = std::get<MetricDeclared>(draft.body);
            if (!body.metric.valid()) {
                return Status(ErrorCode::InvalidIdentity, "metric identity is not valid");
            }
            if (find_metric(state, staging, body.metric) != nullptr) {
                return Status(ErrorCode::DuplicateRecord, "metric already exists");
            }
            if (find_session(state, staging, body.session) == nullptr) {
                return Status(ErrorCode::MissingDependency, "research session does not exist");
            }
            Status status = require_present(body.canonical_key, "metric key");
            if (!status.ok()) return status;
            status = require_text(body.canonical_key, limits.max_string_length, "metric key");
            if (!status.ok()) return status;
            status = require_text(body.description, limits.max_string_length, "metric description");
            if (!status.ok()) return status;
            status = require_text(body.authority, limits.max_string_length, "authority");
            if (!status.ok()) return status;
            if (body.unit == UnitKind::None) {
                return Status(ErrorCode::InvalidArgument, "metric unit must be stated");
            }
            if (body.value_kind == MetricValueKind::Unknown) {
                return Status(ErrorCode::InvalidArgument, "metric value kind must be stated");
            }
            if (!unit_accepts_value(body.unit, body.value_kind)) {
                return Status(ErrorCode::InvalidArgument,
                              "metric unit does not accept the declared value kind");
            }
            const auto keys = state.session_metric_keys.find(body.session);
            if (keys != state.session_metric_keys.end() &&
                keys->second.find(body.canonical_key) != keys->second.end()) {
                return Status(ErrorCode::DuplicateRecord, "metric key already declared in this session");
            }
            for (const StagedEntity& other : staging.staged) {
                const MetricState* staged_metric = std::get_if<MetricState>(&other.entity);
                if (staged_metric != nullptr && staged_metric->session == body.session &&
                    staged_metric->canonical_key == body.canonical_key) {
                    return Status(ErrorCode::DuplicateRecord,
                                  "metric key already declared in this batch");
                }
            }
            MetricState metric;
            metric.metric = body.metric;
            metric.session = body.session;
            metric.canonical_key = body.canonical_key;
            metric.unit = body.unit;
            metric.value_kind = body.value_kind;
            metric.declared_at = sequence;
            staged.session = body.session;
            staged.entity = std::move(metric);
            return staged;
        }
        case RecordType::ObservationRecorded: {
            const auto& body = std::get<ObservationRecorded>(draft.body);
            if (!body.observation.valid()) {
                return Status(ErrorCode::InvalidIdentity, "observation identity is not valid");
            }
            if (find_observation(state, staging, body.observation) != nullptr) {
                return Status(ErrorCode::DuplicateRecord, "observation already exists");
            }
            const AttemptView* attempt = find_attempt(state, staging, body.attempt);
            if (attempt == nullptr) {
                return Status(ErrorCode::MissingDependency, "attempt does not exist");
            }
            Status status = attempt_not_cancelled(*attempt);
            if (!status.ok()) return status;
            const ExperimentView* experiment = find_experiment(state, staging, attempt->experiment);
            if (experiment == nullptr) {
                return Status(ErrorCode::MissingDependency, "attempt experiment does not exist");
            }
            if (body.metric.has_value()) {
                const MetricState* metric = find_metric(state, staging, *body.metric);
                if (metric == nullptr) {
                    return Status(ErrorCode::MissingDependency, "metric does not exist");
                }
                status = same_session(metric->session, experiment->session, "metric");
                if (!status.ok()) return status;
                if (!unit_accepts_value(metric->unit, body.value.kind())) {
                    return Status(ErrorCode::InvalidArgument,
                                  "observation value is not compatible with the declared metric unit");
                }
                if (!(metric->value_kind == body.value.kind())) {
                    return Status(ErrorCode::InvalidArgument,
                                  "observation value kind differs from the declared metric kind");
                }
            } else {
                status = require_present(body.key, "observation key");
                if (!status.ok()) return status;
            }
            status = require_text(body.key, limits.max_string_length, "observation key");
            if (!status.ok()) return status;
            status = require_text(body.source, limits.max_string_length, "observation source");
            if (!status.ok()) return status;
            status = require_text(body.authority, limits.max_string_length, "authority");
            if (!status.ok()) return status;
            if (!unit_accepts_value(body.unit, body.value.kind())) {
                return Status(ErrorCode::InvalidArgument,
                              "observation value is not compatible with the stated unit");
            }

            ObservationView view;
            view.observation = body.observation;
            view.attempt = body.attempt;
            view.experiment = attempt->experiment;
            view.session = experiment->session;
            view.metric = body.metric;
            view.key = body.key;
            view.unit = body.unit;
            view.value = body.value;
            view.measured_at = body.measured_at;
            view.provenance = draft.provenance;
            view.source = body.source;
            view.authority = body.authority;
            view.recorded_at = sequence;
            staged.session = experiment->session;
            staged.entity = std::move(view);
            return staged;
        }
        case RecordType::FailureRecorded: {
            const auto& body = std::get<FailureRecorded>(draft.body);
            if (!body.failure.valid()) {
                return Status(ErrorCode::InvalidIdentity, "failure identity is not valid");
            }
            if (find_failure(state, staging, body.failure) != nullptr) {
                return Status(ErrorCode::DuplicateRecord, "failure already exists");
            }
            if (body.category == FailureCategory::Invalid) {
                return Status(ErrorCode::InvalidArgument, "failure category must be stated");
            }
            Status status = require_text(body.message, limits.max_failure_message_length,
                                         "failure message");
            if (!status.ok()) return status;
            status = require_text(body.authority, limits.max_string_length, "authority");
            if (!status.ok()) return status;
            auto scope = locate_subject(state, staging, body.scope);
            if (!scope.ok()) {
                return scope.status();
            }
            if (body.predecessor.has_value()) {
                if (*body.predecessor == body.failure) {
                    return Status(ErrorCode::LineageCycle, "a failure cannot be its own predecessor");
                }
                if (find_failure(state, staging, *body.predecessor) == nullptr) {
                    return Status(ErrorCode::MissingDependency, "predecessor failure does not exist");
                }
            }
            if (body.recovery_attempt.has_value()) {
                const AttemptView* recovery = find_attempt(state, staging, *body.recovery_attempt);
                if (recovery == nullptr) {
                    return Status(ErrorCode::MissingDependency, "recovery attempt does not exist");
                }
                const ExperimentView* recovery_experiment =
                    find_experiment(state, staging, recovery->experiment);
                if (recovery_experiment == nullptr) {
                    return Status(ErrorCode::MissingDependency,
                                  "recovery attempt experiment does not exist");
                }
                status = same_session(recovery_experiment->session, scope.value().session,
                                      "recovery attempt");
                if (!status.ok()) return status;
            }

            FailureView view;
            view.failure = body.failure;
            view.session = scope.value().session;
            view.scope = body.scope;
            view.category = body.category;
            view.message = body.message;
            view.retriable = body.retriable;
            view.terminal = body.terminal;
            view.predecessor = body.predecessor;
            view.recovery_attempt = body.recovery_attempt;
            view.recorded_at = sequence;
            view.provenance = draft.provenance;
            view.authority = body.authority;
            staged.session = scope.value().session;
            staged.entity = std::move(view);
            return staged;
        }
        case RecordType::DecisionRecorded: {
            const auto& body = std::get<DecisionRecorded>(draft.body);
            if (!body.decision.valid()) {
                return Status(ErrorCode::InvalidIdentity, "decision identity is not valid");
            }
            if (find_decision(state, staging, body.decision) != nullptr) {
                return Status(ErrorCode::DuplicateRecord, "decision already exists");
            }
            if (find_session(state, staging, body.session) == nullptr) {
                return Status(ErrorCode::MissingDependency, "research session does not exist");
            }
            auto subject = locate_subject(state, staging, body.subject);
            if (!subject.ok()) {
                return subject.status();
            }
            Status status = same_session(subject.value().session, body.session, "decision subject");
            if (!status.ok()) return status;
            if (body.type == DecisionType::Invalid || body.outcome == DecisionOutcome::Invalid) {
                return Status(ErrorCode::InvalidArgument, "decision type and outcome must be stated");
            }
            if (!decision_outcome_matches_type(body.type, body.outcome)) {
                return Status(ErrorCode::InvalidArgument,
                              "decision outcome is not consistent with the decision type");
            }
            status = require_text(body.policy_identity, limits.max_string_length, "policy identity");
            if (!status.ok()) return status;
            status = require_text(body.explanation, limits.max_explanation_length,
                                  "decision explanation");
            if (!status.ok()) return status;
            status = require_text(body.authority, limits.max_string_length, "authority");
            if (!status.ok()) return status;
            if (body.evidence.size() > limits.max_evidence_refs_per_decision) {
                return Status(ErrorCode::LimitExceeded,
                              "evidence list exceeds the configured maximum");
            }
            for (std::size_t index = 0; index < body.evidence.size(); ++index) {
                for (std::size_t earlier = 0; earlier < index; ++earlier) {
                    if (body.evidence[earlier].subject == body.evidence[index].subject &&
                        body.evidence[earlier].sequence == body.evidence[index].sequence) {
                        return Status(ErrorCode::InvalidArgument, "evidence list repeats an entry");
                    }
                }
                status = check_evidence(state, body.evidence[index], body.session, sequence);
                if (!status.ok()) return status;
            }

            DecisionView view;
            view.decision = body.decision;
            view.session = body.session;
            view.subject = body.subject;
            view.type = body.type;
            view.outcome = body.outcome;
            view.policy_identity = body.policy_identity;
            view.policy_generation = body.policy_generation;
            view.evidence = body.evidence;
            view.explanation = body.explanation;
            view.recorded_at = sequence;
            view.provenance = draft.provenance;
            view.authority = body.authority;
            staged.session = body.session;
            staged.entity = std::move(view);
            return staged;
        }
        case RecordType::ResultDeclared: {
            const auto& body = std::get<ResultDeclared>(draft.body);
            if (!body.result.valid() || !body.generation.valid()) {
                return Status(ErrorCode::InvalidIdentity, "result identity is not valid");
            }
            if (find_result(state, staging, body.result) != nullptr) {
                return Status(ErrorCode::DuplicateRecord, "result already exists");
            }
            if (!(body.generation == ResultGeneration::first())) {
                return Status(ErrorCode::StaleGeneration,
                              "a result declaration must be its first generation");
            }
            if (find_session(state, staging, body.session) == nullptr) {
                return Status(ErrorCode::MissingDependency, "research session does not exist");
            }
            Status status = require_text(body.summary, limits.max_string_length, "result summary");
            if (!status.ok()) return status;
            status = require_text(body.authority, limits.max_string_length, "authority");
            if (!status.ok()) return status;
            if (body.hypotheses.size() > limits.max_hypotheses_per_experiment ||
                body.experiments.size() > limits.max_experiments_per_result ||
                body.artifacts.size() > limits.max_artifacts_per_result ||
                body.observations.size() > limits.max_observations_per_result ||
                body.model_calls.size() > limits.max_calls_per_attempt ||
                body.tool_calls.size() > limits.max_calls_per_attempt) {
                return Status(ErrorCode::LimitExceeded,
                              "result evidence list exceeds the configured maximum");
            }
            for (const HypothesisId hypothesis : body.hypotheses) {
                const HypothesisView* view = find_hypothesis(state, staging, hypothesis);
                if (view == nullptr) {
                    return Status(ErrorCode::MissingDependency, "result hypothesis does not exist");
                }
                status = same_session(view->session, body.session, "result hypothesis");
                if (!status.ok()) return status;
            }
            for (const ExperimentId experiment : body.experiments) {
                const ExperimentView* view = find_experiment(state, staging, experiment);
                if (view == nullptr) {
                    return Status(ErrorCode::MissingDependency, "result experiment does not exist");
                }
                status = same_session(view->session, body.session, "result experiment");
                if (!status.ok()) return status;
            }
            for (const ArtifactId artifact : body.artifacts) {
                const ArtifactView* view = find_artifact(state, staging, artifact);
                if (view == nullptr) {
                    return Status(ErrorCode::MissingDependency, "result artifact does not exist");
                }
                status = same_session(view->session, body.session, "result artifact");
                if (!status.ok()) return status;
            }
            for (const ObservationId observation : body.observations) {
                const ObservationView* view = find_observation(state, staging, observation);
                if (view == nullptr) {
                    return Status(ErrorCode::MissingDependency, "result observation does not exist");
                }
                status = same_session(view->session, body.session, "result observation");
                if (!status.ok()) return status;
            }
            for (const ModelCallId call : body.model_calls) {
                const ModelCallView* view = find_model_call(state, staging, call);
                if (view == nullptr) {
                    return Status(ErrorCode::MissingDependency, "result model call does not exist");
                }
                const AttemptView* attempt = find_attempt(state, staging, view->attempt);
                if (attempt == nullptr) {
                    return Status(ErrorCode::MissingDependency,
                                  "result model call attempt does not exist");
                }
                const ExperimentView* experiment =
                    find_experiment(state, staging, attempt->experiment);
                if (experiment == nullptr) {
                    return Status(ErrorCode::MissingDependency,
                                  "result model call experiment does not exist");
                }
                status = same_session(experiment->session, body.session, "result model call");
                if (!status.ok()) return status;
            }
            for (const ToolCallId call : body.tool_calls) {
                const ToolCallView* view = find_tool_call(state, staging, call);
                if (view == nullptr) {
                    return Status(ErrorCode::MissingDependency, "result tool call does not exist");
                }
                const AttemptView* attempt = find_attempt(state, staging, view->attempt);
                if (attempt == nullptr) {
                    return Status(ErrorCode::MissingDependency,
                                  "result tool call attempt does not exist");
                }
                const ExperimentView* experiment =
                    find_experiment(state, staging, attempt->experiment);
                if (experiment == nullptr) {
                    return Status(ErrorCode::MissingDependency,
                                  "result tool call experiment does not exist");
                }
                status = same_session(experiment->session, body.session, "result tool call");
                if (!status.ok()) return status;
            }
            if (body.experiments.empty() && body.artifacts.empty()) {
                return Status(ErrorCode::InvalidArgument,
                              "a result must name at least one experiment or artifact");
            }
            if (body.content_digest.has_value() && body.content_digest->is_zero()) {
                return Status(ErrorCode::InvalidArgument, "result content digest must be stated");
            }

            ResultView view;
            view.result = body.result;
            view.generation = body.generation;
            view.session = body.session;
            view.summary = body.summary;
            view.status = ResultStatus::Candidate;
            view.hypotheses = body.hypotheses;
            view.experiments = body.experiments;
            view.artifacts = body.artifacts;
            view.observations = body.observations;
            view.model_calls = body.model_calls;
            view.tool_calls = body.tool_calls;
            view.content_digest = body.content_digest;
            view.declared_at = sequence;
            view.last_changed_at = sequence;
            view.provenance = draft.provenance;
            view.authority = body.authority;
            staged.session = body.session;
            staged.entity = std::move(view);
            return staged;
        }
        case RecordType::ResultStatusChanged: {
            const auto& body = std::get<ResultStatusChanged>(draft.body);
            const ResultView* existing = find_result(state, staging, body.result);
            if (existing == nullptr) {
                return Status(ErrorCode::MissingDependency, "result does not exist");
            }
            if (!(body.generation == existing->generation.next())) {
                return Status(ErrorCode::StaleGeneration, "result generation is not the next one");
            }
            if (!(body.from == existing->status)) {
                return Status(ErrorCode::InvalidTransition,
                              "result transition does not start from the current status");
            }
            if (!result_transition_allowed(body.from, body.to)) {
                return Status(ErrorCode::InvalidTransition, "result transition is not permitted");
            }
            const DecisionView* decision = find_decision(state, staging, body.decision);
            if (decision == nullptr) {
                return Status(ErrorCode::MissingDependency,
                              "a result status change requires an explicit committed decision");
            }
            if (!(decision->subject == SubjectId::of(body.result))) {
                return Status(ErrorCode::InvalidTransition, "decision does not concern this result");
            }
            if (!decision_outcome_matches_type(decision->type, decision->outcome)) {
                return Status(ErrorCode::InvalidArgument, "decision is not internally consistent");
            }
            if (!result_status_matches_decision(body.to, decision->outcome)) {
                return Status(ErrorCode::InvalidTransition,
                              "decision outcome does not authorize this result status");
            }
            ResultView view = *existing;
            view.generation = body.generation;
            view.status = body.to;
            view.last_changed_at = sequence;
            view.last_decision = body.decision;
            if (body.to == ResultStatus::Accepted) {
                view.acceptance_decision = body.decision;
            }
            staged.session = view.session;
            staged.entity = std::move(view);
            return staged;
        }
        case RecordType::AccountingRecorded: {
            const auto& body = std::get<AccountingRecorded>(draft.body);
            auto scope = locate_subject(state, staging, body.scope);
            if (!scope.ok()) {
                return scope.status();
            }
            if (body.accounting.is_empty()) {
                return Status(ErrorCode::InvalidArgument,
                              "an accounting record must state at least one known quantity");
            }
            if (body.accounting.monetary.known && body.accounting.monetary.currency.empty()) {
                return Status(ErrorCode::InvalidArgument,
                              "a monetary accounting value must name its currency");
            }
            Status status = require_text(body.source, limits.max_string_length, "accounting source");
            if (!status.ok()) return status;
            status = require_text(body.authority, limits.max_string_length, "authority");
            if (!status.ok()) return status;
            staged.session = scope.value().session;
            staged.has_accounting = true;
            staged.accounting.scope = body.scope;
            staged.accounting.accounting = body.accounting;
            staged.accounting.sequence = sequence;
            return staged;
        }
    }

    return Status(ErrorCode::InternalError, "unhandled record type");
}

}  // namespace

Result<StagedEntity> build_staged_entity(const LedgerState& state, const BatchStaging& staging,
                                         const RecordDraft& draft, RecordSequence sequence) {
    auto staged = build_staged_entity_unchecked(state, staging, draft, sequence);
    if (!staged.ok()) {
        return staged.status();
    }
    // Closure is a property of the session, not of each record type: once a
    // session is closed it accepts only bounded post-hoc annotations. It never
    // silently accepts new research, and it never reopens.
    const SessionView* session = find_session(state, staging, staged.value().session);
    if (session != nullptr && session->state == SessionState::Closed &&
        staged.value().type != RecordType::SessionAnnotation) {
        return Status(ErrorCode::SessionClosed,
                      "a closed research session accepts only post-hoc annotations");
    }
    return staged;
}

}  // namespace research_ledger

