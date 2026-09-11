#include "research_ledger/model.hpp"

#include <chrono>
#include <type_traits>
#include <utility>

namespace research_ledger {

TimestampNs now_timestamp() noexcept {
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    const auto nanos = std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();
    return TimestampNs{static_cast<std::int64_t>(nanos)};
}

MonotonicNs monotonic_now() noexcept {
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    const auto nanos = std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();
    return MonotonicNs{static_cast<std::uint64_t>(nanos)};
}

std::string_view session_state_name(SessionState value) noexcept {
    switch (value) {
        case SessionState::Invalid:
            return "INVALID";
        case SessionState::Open:
            return "OPEN";
        case SessionState::Closed:
            return "CLOSED";
    }
    return "INVALID";
}

std::string_view hypothesis_status_name(HypothesisStatus value) noexcept {
    switch (value) {
        case HypothesisStatus::Invalid:
            return "INVALID";
        case HypothesisStatus::Proposed:
            return "PROPOSED";
        case HypothesisStatus::Active:
            return "ACTIVE";
        case HypothesisStatus::Supported:
            return "SUPPORTED";
        case HypothesisStatus::NotSupported:
            return "NOT_SUPPORTED";
        case HypothesisStatus::Rejected:
            return "REJECTED";
        case HypothesisStatus::Superseded:
            return "SUPERSEDED";
        case HypothesisStatus::Inconclusive:
            return "INCONCLUSIVE";
    }
    return "INVALID";
}

std::string_view experiment_state_name(ExperimentState value) noexcept {
    switch (value) {
        case ExperimentState::Invalid:
            return "INVALID";
        case ExperimentState::Declared:
            return "DECLARED";
    }
    return "INVALID";
}

std::string_view branch_kind_name(BranchKind value) noexcept {
    switch (value) {
        case BranchKind::Invalid:
            return "INVALID";
        case BranchKind::Root:
            return "ROOT";
        case BranchKind::Fork:
            return "FORK";
        case BranchKind::Continuation:
            return "CONTINUATION";
        case BranchKind::Retry:
            return "RETRY";
        case BranchKind::AlternateMethod:
            return "ALTERNATE_METHOD";
        case BranchKind::Control:
            return "CONTROL";
        case BranchKind::Ablation:
            return "ABLATION";
        case BranchKind::CompetingHypothesis:
            return "COMPETING_HYPOTHESIS";
        case BranchKind::MergedEvidence:
            return "MERGED_EVIDENCE";
    }
    return "INVALID";
}

std::string_view attempt_state_name(AttemptState value) noexcept {
    switch (value) {
        case AttemptState::Invalid:
            return "INVALID";
        case AttemptState::Running:
            return "RUNNING";
        case AttemptState::Completed:
            return "COMPLETED";
        case AttemptState::Failed:
            return "FAILED";
        case AttemptState::Cancelled:
            return "CANCELLED";
    }
    return "INVALID";
}

std::string_view model_call_outcome_name(ModelCallOutcome value) noexcept {
    switch (value) {
        case ModelCallOutcome::Invalid:
            return "INVALID";
        case ModelCallOutcome::Succeeded:
            return "SUCCEEDED";
        case ModelCallOutcome::Failed:
            return "FAILED";
        case ModelCallOutcome::Cancelled:
            return "CANCELLED";
        case ModelCallOutcome::OutcomeUnknown:
            return "OUTCOME_UNKNOWN";
    }
    return "INVALID";
}

std::string_view tool_call_state_name(ToolCallState value) noexcept {
    switch (value) {
        case ToolCallState::Invalid:
            return "INVALID";
        case ToolCallState::Submitted:
            return "SUBMITTED";
        case ToolCallState::Acknowledged:
            return "ACKNOWLEDGED";
        case ToolCallState::Completed:
            return "COMPLETED";
        case ToolCallState::Failed:
            return "FAILED";
        case ToolCallState::Cancelled:
            return "CANCELLED";
        case ToolCallState::OutcomeUnknown:
            return "OUTCOME_UNKNOWN";
    }
    return "INVALID";
}

std::string_view artifact_role_name(ArtifactRole value) noexcept {
    switch (value) {
        case ArtifactRole::Invalid:
            return "INVALID";
        case ArtifactRole::Dataset:
            return "DATASET";
        case ArtifactRole::Input:
            return "INPUT";
        case ArtifactRole::Intermediate:
            return "INTERMEDIATE";
        case ArtifactRole::Checkpoint:
            return "CHECKPOINT";
        case ArtifactRole::Model:
            return "MODEL";
        case ArtifactRole::Code:
            return "CODE";
        case ArtifactRole::Configuration:
            return "CONFIGURATION";
        case ArtifactRole::Report:
            return "REPORT";
        case ArtifactRole::Figure:
            return "FIGURE";
        case ArtifactRole::ResultArtifact:
            return "RESULT_ARTIFACT";
        case ArtifactRole::Other:
            return "OTHER";
    }
    return "INVALID";
}

std::string_view validation_state_name(ValidationState value) noexcept {
    switch (value) {
        case ValidationState::Invalid:
            return "INVALID";
        case ValidationState::Unvalidated:
            return "UNVALIDATED";
        case ValidationState::Validated:
            return "VALIDATED";
        case ValidationState::Invalidated:
            return "INVALIDATED";
    }
    return "INVALID";
}

std::string_view failure_category_name(FailureCategory value) noexcept {
    switch (value) {
        case FailureCategory::Invalid:
            return "INVALID";
        case FailureCategory::Execution:
            return "EXECUTION_FAILURE";
        case FailureCategory::Model:
            return "MODEL_FAILURE";
        case FailureCategory::Tool:
            return "TOOL_FAILURE";
        case FailureCategory::Validation:
            return "VALIDATION_FAILURE";
        case FailureCategory::Input:
            return "INPUT_FAILURE";
        case FailureCategory::Resource:
            return "RESOURCE_FAILURE";
        case FailureCategory::Infrastructure:
            return "INFRASTRUCTURE_FAILURE";
        case FailureCategory::Cancelled:
            return "CANCELLED";
        case FailureCategory::OutcomeUnknown:
            return "OUTCOME_UNKNOWN";
        case FailureCategory::Integrity:
            return "INTEGRITY_FAILURE";
        case FailureCategory::Authority:
            return "AUTHORITY_FAILURE";
        case FailureCategory::Unknown:
            return "UNKNOWN";
    }
    return "INVALID";
}

std::string_view decision_type_name(DecisionType value) noexcept {
    switch (value) {
        case DecisionType::Invalid:
            return "INVALID";
        case DecisionType::Accept:
            return "ACCEPT";
        case DecisionType::Reject:
            return "REJECT";
        case DecisionType::Supersede:
            return "SUPERSEDE";
        case DecisionType::Invalidate:
            return "INVALIDATE";
        case DecisionType::Retract:
            return "RETRACT";
        case DecisionType::Reclassify:
            return "RECLASSIFY";
        case DecisionType::HypothesisTransition:
            return "HYPOTHESIS_TRANSITION";
        case DecisionType::BranchAbandonment:
            return "BRANCH_ABANDONMENT";
        case DecisionType::Audit:
            return "AUDIT";
    }
    return "INVALID";
}

std::string_view decision_outcome_name(DecisionOutcome value) noexcept {
    switch (value) {
        case DecisionOutcome::Invalid:
            return "INVALID";
        case DecisionOutcome::Accepted:
            return "ACCEPTED";
        case DecisionOutcome::Rejected:
            return "REJECTED";
        case DecisionOutcome::Superseded:
            return "SUPERSEDED";
        case DecisionOutcome::Invalidated:
            return "INVALIDATED";
        case DecisionOutcome::Retracted:
            return "RETRACTED";
        case DecisionOutcome::Reclassified:
            return "RECLASSIFIED";
        case DecisionOutcome::Recorded:
            return "RECORDED";
    }
    return "INVALID";
}

std::string_view result_status_name(ResultStatus value) noexcept {
    switch (value) {
        case ResultStatus::Invalid:
            return "INVALID";
        case ResultStatus::Candidate:
            return "CANDIDATE";
        case ResultStatus::Accepted:
            return "ACCEPTED";
        case ResultStatus::Rejected:
            return "REJECTED";
        case ResultStatus::Superseded:
            return "SUPERSEDED";
        case ResultStatus::Invalidated:
            return "INVALIDATED";
        case ResultStatus::Retracted:
            return "RETRACTED";
    }
    return "INVALID";
}

std::string_view subject_kind_name(SubjectKind value) noexcept {
    switch (value) {
        case SubjectKind::None:
            return "NONE";
        case SubjectKind::Session:
            return "SESSION";
        case SubjectKind::Hypothesis:
            return "HYPOTHESIS";
        case SubjectKind::Experiment:
            return "EXPERIMENT";
        case SubjectKind::Branch:
            return "BRANCH";
        case SubjectKind::Attempt:
            return "ATTEMPT";
        case SubjectKind::ModelCall:
            return "MODEL_CALL";
        case SubjectKind::ToolCall:
            return "TOOL_CALL";
        case SubjectKind::Artifact:
            return "ARTIFACT";
        case SubjectKind::Observation:
            return "OBSERVATION";
        case SubjectKind::Failure:
            return "FAILURE";
        case SubjectKind::Decision:
            return "DECISION";
        case SubjectKind::Result:
            return "RESULT";
    }
    return "NONE";
}

namespace {

template <class Enum>
std::optional<Enum> parse_named(std::string_view text, std::string_view (*name)(Enum) noexcept,
                                Enum last) {
    for (std::uint16_t raw = 1; raw <= static_cast<std::uint16_t>(last); ++raw) {
        const auto value = static_cast<Enum>(raw);
        if (name(value) == text) {
            return value;
        }
    }
    return std::nullopt;
}

}  // namespace

std::optional<HypothesisStatus> parse_hypothesis_status(std::string_view text) noexcept {
    return parse_named<HypothesisStatus>(text, hypothesis_status_name, HypothesisStatus::Inconclusive);
}

std::optional<BranchKind> parse_branch_kind(std::string_view text) noexcept {
    return parse_named<BranchKind>(text, branch_kind_name, BranchKind::MergedEvidence);
}

std::optional<AttemptState> parse_attempt_state(std::string_view text) noexcept {
    return parse_named<AttemptState>(text, attempt_state_name, AttemptState::Cancelled);
}

std::optional<ArtifactRole> parse_artifact_role(std::string_view text) noexcept {
    return parse_named<ArtifactRole>(text, artifact_role_name, ArtifactRole::Other);
}

std::optional<FailureCategory> parse_failure_category(std::string_view text) noexcept {
    return parse_named<FailureCategory>(text, failure_category_name, FailureCategory::Unknown);
}

std::optional<ResultStatus> parse_result_status(std::string_view text) noexcept {
    return parse_named<ResultStatus>(text, result_status_name, ResultStatus::Retracted);
}

std::optional<DecisionType> parse_decision_type(std::string_view text) noexcept {
    return parse_named<DecisionType>(text, decision_type_name, DecisionType::Audit);
}

std::optional<DecisionOutcome> parse_decision_outcome(std::string_view text) noexcept {
    return parse_named<DecisionOutcome>(text, decision_outcome_name, DecisionOutcome::Recorded);
}

std::optional<ModelCallOutcome> parse_model_call_outcome(std::string_view text) noexcept {
    return parse_named<ModelCallOutcome>(text, model_call_outcome_name, ModelCallOutcome::OutcomeUnknown);
}

std::optional<ToolCallState> parse_tool_call_state(std::string_view text) noexcept {
    return parse_named<ToolCallState>(text, tool_call_state_name, ToolCallState::OutcomeUnknown);
}

std::optional<UnitKind> parse_unit_kind_checked(std::string_view text) noexcept {
    return parse_unit_kind(text);
}

std::optional<Provenance> parse_provenance_checked(std::string_view text) noexcept {
    return parse_provenance(text);
}

// --- SubjectId --------------------------------------------------------------
#define RESEARCH_LEDGER_SUBJECT_FACTORY(KIND, TYPE)                     \
    SubjectId SubjectId::of(TYPE value) noexcept {                      \
        SubjectId subject;                                             \
        subject.payload_ = value;                                      \
        return subject;                                                \
    }

RESEARCH_LEDGER_SUBJECT_FACTORY(Session, ResearchSessionId)
RESEARCH_LEDGER_SUBJECT_FACTORY(Hypothesis, HypothesisId)
RESEARCH_LEDGER_SUBJECT_FACTORY(Experiment, ExperimentId)
RESEARCH_LEDGER_SUBJECT_FACTORY(Branch, BranchId)
RESEARCH_LEDGER_SUBJECT_FACTORY(Attempt, AttemptId)
RESEARCH_LEDGER_SUBJECT_FACTORY(ModelCall, ModelCallId)
RESEARCH_LEDGER_SUBJECT_FACTORY(ToolCall, ToolCallId)
RESEARCH_LEDGER_SUBJECT_FACTORY(Artifact, ArtifactId)
RESEARCH_LEDGER_SUBJECT_FACTORY(Observation, ObservationId)
RESEARCH_LEDGER_SUBJECT_FACTORY(Failure, FailureId)
RESEARCH_LEDGER_SUBJECT_FACTORY(Decision, DecisionId)
RESEARCH_LEDGER_SUBJECT_FACTORY(Result, ResultId)

#undef RESEARCH_LEDGER_SUBJECT_FACTORY

SubjectKind SubjectId::kind() const noexcept {
    switch (payload_.index()) {
        case 0:
            return SubjectKind::None;
        case 1:
            return SubjectKind::Session;
        case 2:
            return SubjectKind::Hypothesis;
        case 3:
            return SubjectKind::Experiment;
        case 4:
            return SubjectKind::Branch;
        case 5:
            return SubjectKind::Attempt;
        case 6:
            return SubjectKind::ModelCall;
        case 7:
            return SubjectKind::ToolCall;
        case 8:
            return SubjectKind::Artifact;
        case 9:
            return SubjectKind::Observation;
        case 10:
            return SubjectKind::Failure;
        case 11:
            return SubjectKind::Decision;
        case 12:
            return SubjectKind::Result;
        default:
            return SubjectKind::None;
    }
}

std::uint64_t SubjectId::value() const noexcept {
    return std::visit(
        [](const auto& alternative) -> std::uint64_t {
            using Alternative = std::decay_t<decltype(alternative)>;
            if constexpr (std::is_same_v<Alternative, std::monostate>) {
                return 0;
            } else {
                return alternative.value();
            }
        },
        payload_);
}

IdentityDomain SubjectId::domain() const noexcept {
    switch (kind()) {
        case SubjectKind::None:
            return IdentityDomain::None;
        case SubjectKind::Session:
            return IdentityDomain::ResearchSession;
        case SubjectKind::Hypothesis:
            return IdentityDomain::Hypothesis;
        case SubjectKind::Experiment:
            return IdentityDomain::Experiment;
        case SubjectKind::Branch:
            return IdentityDomain::Branch;
        case SubjectKind::Attempt:
            return IdentityDomain::Attempt;
        case SubjectKind::ModelCall:
            return IdentityDomain::ModelCall;
        case SubjectKind::ToolCall:
            return IdentityDomain::ToolCall;
        case SubjectKind::Artifact:
            return IdentityDomain::Artifact;
        case SubjectKind::Observation:
            return IdentityDomain::Observation;
        case SubjectKind::Failure:
            return IdentityDomain::Failure;
        case SubjectKind::Decision:
            return IdentityDomain::Decision;
        case SubjectKind::Result:
            return IdentityDomain::Result;
    }
    return IdentityDomain::None;
}

std::string SubjectId::to_string() const {
    if (!valid()) {
        return "none";
    }
    std::string text(identity_domain_name(domain()));
    text += ':';
    text += std::to_string(value());
    return text;
}

bool operator==(const SubjectId& lhs, const SubjectId& rhs) noexcept {
    return lhs.payload_ == rhs.payload_;
}

bool operator<(const SubjectId& lhs, const SubjectId& rhs) noexcept {
    if (lhs.kind() != rhs.kind()) {
        return static_cast<std::uint8_t>(lhs.kind()) < static_cast<std::uint8_t>(rhs.kind());
    }
    return lhs.value() < rhs.value();
}

// --- transition rules -------------------------------------------------------
bool result_transition_allowed(ResultStatus from, ResultStatus to) noexcept {
    if (from == to) {
        return false;
    }
    switch (from) {
        case ResultStatus::Candidate:
            return to == ResultStatus::Accepted || to == ResultStatus::Rejected;
        case ResultStatus::Accepted:
            return to == ResultStatus::Superseded || to == ResultStatus::Invalidated ||
                   to == ResultStatus::Retracted;
        case ResultStatus::Rejected:
            // A rejected result never becomes accepted directly. Reclassification
            // by an explicit decision returns it to candidate, and acceptance
            // then requires its own decision.
            return to == ResultStatus::Superseded || to == ResultStatus::Invalidated ||
                   to == ResultStatus::Candidate;
        case ResultStatus::Superseded:
            return to == ResultStatus::Invalidated || to == ResultStatus::Retracted;
        case ResultStatus::Invalidated:
            return to == ResultStatus::Retracted;
        case ResultStatus::Retracted:
            return false;
        case ResultStatus::Invalid:
            return false;
    }
    return false;
}

bool hypothesis_transition_allowed(HypothesisStatus from, HypothesisStatus to) noexcept {
    if (from == to) {
        return false;
    }
    switch (from) {
        case HypothesisStatus::Proposed:
            return to == HypothesisStatus::Active || to == HypothesisStatus::Rejected ||
                   to == HypothesisStatus::Superseded || to == HypothesisStatus::Inconclusive;
        case HypothesisStatus::Active:
            return to == HypothesisStatus::Supported || to == HypothesisStatus::NotSupported ||
                   to == HypothesisStatus::Rejected || to == HypothesisStatus::Superseded ||
                   to == HypothesisStatus::Inconclusive;
        case HypothesisStatus::Supported:
            return to == HypothesisStatus::Superseded || to == HypothesisStatus::Inconclusive;
        case HypothesisStatus::NotSupported:
            return to == HypothesisStatus::Superseded || to == HypothesisStatus::Inconclusive ||
                   to == HypothesisStatus::Supported;
        case HypothesisStatus::Inconclusive:
            return to == HypothesisStatus::Active || to == HypothesisStatus::Supported ||
                   to == HypothesisStatus::NotSupported || to == HypothesisStatus::Rejected ||
                   to == HypothesisStatus::Superseded;
        case HypothesisStatus::Rejected:
            return to == HypothesisStatus::Superseded;
        case HypothesisStatus::Superseded:
            return false;
        case HypothesisStatus::Invalid:
            return false;
    }
    return false;
}

bool result_status_matches_decision(ResultStatus status, DecisionOutcome outcome) noexcept {
    switch (outcome) {
        case DecisionOutcome::Accepted:
            return status == ResultStatus::Accepted;
        case DecisionOutcome::Rejected:
            return status == ResultStatus::Rejected;
        case DecisionOutcome::Superseded:
            return status == ResultStatus::Superseded;
        case DecisionOutcome::Invalidated:
            return status == ResultStatus::Invalidated;
        case DecisionOutcome::Retracted:
            return status == ResultStatus::Retracted;
        case DecisionOutcome::Reclassified:
            return status == ResultStatus::Candidate || status == ResultStatus::Rejected;
        case DecisionOutcome::Recorded:
        case DecisionOutcome::Invalid:
            return false;
    }
    return false;
}

bool decision_outcome_matches_type(DecisionType type, DecisionOutcome outcome) noexcept {
    switch (type) {
        case DecisionType::Accept:
            return outcome == DecisionOutcome::Accepted;
        case DecisionType::Reject:
            return outcome == DecisionOutcome::Rejected;
        case DecisionType::Supersede:
            return outcome == DecisionOutcome::Superseded;
        case DecisionType::Invalidate:
            return outcome == DecisionOutcome::Invalidated;
        case DecisionType::Retract:
            return outcome == DecisionOutcome::Retracted;
        case DecisionType::Reclassify:
            return outcome == DecisionOutcome::Reclassified;
        case DecisionType::HypothesisTransition:
        case DecisionType::BranchAbandonment:
        case DecisionType::Audit:
            return outcome == DecisionOutcome::Recorded;
        case DecisionType::Invalid:
            return false;
    }
    return false;
}

}  // namespace research_ledger
