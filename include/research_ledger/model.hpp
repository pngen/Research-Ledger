#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "research_ledger/digest.hpp"
#include "research_ledger/error.hpp"
#include "research_ledger/identity.hpp"
#include "research_ledger/provenance.hpp"
#include "research_ledger/quantity.hpp"

namespace research_ledger {

// Externally meaningful timestamp: nanoseconds since the Unix epoch, UTC. It is
// evidence, not authority: committed order comes from RecordSequence.
struct TimestampNs {
    std::int64_t unix_nanos = 0;

    friend bool operator==(const TimestampNs& lhs, const TimestampNs& rhs) noexcept {
        return lhs.unix_nanos == rhs.unix_nanos;
    }
    friend bool operator<(const TimestampNs& lhs, const TimestampNs& rhs) noexcept {
        return lhs.unix_nanos < rhs.unix_nanos;
    }
};

// Locally measured elapsed time from a monotonic clock. Never mixed with
// TimestampNs.
struct MonotonicNs {
    std::uint64_t nanos = 0;
};

TimestampNs now_timestamp() noexcept;
MonotonicNs monotonic_now() noexcept;

// --- enumerations -----------------------------------------------------------
enum class SessionState : std::uint8_t { Invalid = 0, Open = 1, Closed = 2 };
enum class HypothesisStatus : std::uint8_t {
    Invalid = 0,
    Proposed = 1,
    Active = 2,
    Supported = 3,
    NotSupported = 4,
    Rejected = 5,
    Superseded = 6,
    Inconclusive = 7,
};
enum class ExperimentState : std::uint8_t { Invalid = 0, Declared = 1 };
enum class BranchKind : std::uint8_t {
    Invalid = 0,
    Root = 1,
    Fork = 2,
    Continuation = 3,
    Retry = 4,
    AlternateMethod = 5,
    Control = 6,
    Ablation = 7,
    CompetingHypothesis = 8,
    MergedEvidence = 9,
};
enum class AttemptState : std::uint8_t {
    Invalid = 0,
    Running = 1,
    Completed = 2,
    Failed = 3,
    Cancelled = 4,
};
enum class ModelCallOutcome : std::uint8_t {
    Invalid = 0,
    Succeeded = 1,
    Failed = 2,
    Cancelled = 3,
    OutcomeUnknown = 4,
};
enum class ToolCallState : std::uint8_t {
    Invalid = 0,
    Submitted = 1,
    Acknowledged = 2,
    Completed = 3,
    Failed = 4,
    Cancelled = 5,
    OutcomeUnknown = 6,
};
enum class ArtifactRole : std::uint8_t {
    Invalid = 0,
    Dataset = 1,
    Input = 2,
    Intermediate = 3,
    Checkpoint = 4,
    Model = 5,
    Code = 6,
    Configuration = 7,
    Report = 8,
    Figure = 9,
    ResultArtifact = 10,
    Other = 11,
};
enum class ValidationState : std::uint8_t { Invalid = 0, Unvalidated = 1, Validated = 2, Invalidated = 3 };
enum class FailureCategory : std::uint8_t {
    Invalid = 0,
    Execution = 1,
    Model = 2,
    Tool = 3,
    Validation = 4,
    Input = 5,
    Resource = 6,
    Infrastructure = 7,
    Cancelled = 8,
    OutcomeUnknown = 9,
    Integrity = 10,
    Authority = 11,
    Unknown = 12,
};
enum class DecisionType : std::uint8_t {
    Invalid = 0,
    Accept = 1,
    Reject = 2,
    Supersede = 3,
    Invalidate = 4,
    Retract = 5,
    Reclassify = 6,
    HypothesisTransition = 7,
    BranchAbandonment = 8,
    Audit = 9,
};
enum class DecisionOutcome : std::uint8_t {
    Invalid = 0,
    Accepted = 1,
    Rejected = 2,
    Superseded = 3,
    Invalidated = 4,
    Retracted = 5,
    Reclassified = 6,
    Recorded = 7,
};
enum class ResultStatus : std::uint8_t {
    Invalid = 0,
    Candidate = 1,
    Accepted = 2,
    Rejected = 3,
    Superseded = 4,
    Invalidated = 5,
    Retracted = 6,
};
enum class SubjectKind : std::uint8_t {
    None = 0,
    Session = 1,
    Hypothesis = 2,
    Experiment = 3,
    Branch = 4,
    Attempt = 5,
    ModelCall = 6,
    ToolCall = 7,
    Artifact = 8,
    Observation = 9,
    Failure = 10,
    Decision = 11,
    Result = 12,
};

std::string_view session_state_name(SessionState value) noexcept;
std::string_view hypothesis_status_name(HypothesisStatus value) noexcept;
std::string_view experiment_state_name(ExperimentState value) noexcept;
std::string_view branch_kind_name(BranchKind value) noexcept;
std::string_view attempt_state_name(AttemptState value) noexcept;
std::string_view model_call_outcome_name(ModelCallOutcome value) noexcept;
std::string_view tool_call_state_name(ToolCallState value) noexcept;
std::string_view artifact_role_name(ArtifactRole value) noexcept;
std::string_view validation_state_name(ValidationState value) noexcept;
std::string_view failure_category_name(FailureCategory value) noexcept;
std::string_view decision_type_name(DecisionType value) noexcept;
std::string_view decision_outcome_name(DecisionOutcome value) noexcept;
std::string_view result_status_name(ResultStatus value) noexcept;
std::string_view subject_kind_name(SubjectKind value) noexcept;

std::optional<HypothesisStatus> parse_hypothesis_status(std::string_view text) noexcept;
std::optional<BranchKind> parse_branch_kind(std::string_view text) noexcept;
std::optional<AttemptState> parse_attempt_state(std::string_view text) noexcept;
std::optional<ArtifactRole> parse_artifact_role(std::string_view text) noexcept;
std::optional<FailureCategory> parse_failure_category(std::string_view text) noexcept;
std::optional<ResultStatus> parse_result_status(std::string_view text) noexcept;
std::optional<DecisionType> parse_decision_type(std::string_view text) noexcept;
std::optional<DecisionOutcome> parse_decision_outcome(std::string_view text) noexcept;
std::optional<ModelCallOutcome> parse_model_call_outcome(std::string_view text) noexcept;
std::optional<ToolCallState> parse_tool_call_state(std::string_view text) noexcept;
std::optional<UnitKind> parse_unit_kind_checked(std::string_view text) noexcept;
std::optional<Provenance> parse_provenance_checked(std::string_view text) noexcept;

// --- generic references -----------------------------------------------------
// A subject is a reference to any ledger entity. The alternative that is active
// determines the domain: a subject never reinterprets bytes as another domain.
class SubjectId {
public:
    using Payload = std::variant<std::monostate, ResearchSessionId, HypothesisId, ExperimentId, BranchId,
                                 AttemptId, ModelCallId, ToolCallId, ArtifactId, ObservationId, FailureId,
                                 DecisionId, ResultId>;

    SubjectId() noexcept = default;

    static SubjectId of(ResearchSessionId value) noexcept;
    static SubjectId of(HypothesisId value) noexcept;
    static SubjectId of(ExperimentId value) noexcept;
    static SubjectId of(BranchId value) noexcept;
    static SubjectId of(AttemptId value) noexcept;
    static SubjectId of(ModelCallId value) noexcept;
    static SubjectId of(ToolCallId value) noexcept;
    static SubjectId of(ArtifactId value) noexcept;
    static SubjectId of(ObservationId value) noexcept;
    static SubjectId of(FailureId value) noexcept;
    static SubjectId of(DecisionId value) noexcept;
    static SubjectId of(ResultId value) noexcept;

    [[nodiscard]] SubjectKind kind() const noexcept;
    [[nodiscard]] bool valid() const noexcept { return kind() != SubjectKind::None; }
    [[nodiscard]] std::uint64_t value() const noexcept;
    [[nodiscard]] IdentityDomain domain() const noexcept;
    [[nodiscard]] const Payload& payload() const noexcept { return payload_; }

    template <class Strong>
    [[nodiscard]] std::optional<Strong> as() const noexcept {
        if (const Strong* typed = std::get_if<Strong>(&payload_)) {
            return *typed;
        }
        return std::nullopt;
    }

    [[nodiscard]] std::string to_string() const;

    friend bool operator==(const SubjectId& lhs, const SubjectId& rhs) noexcept;
    friend bool operator<(const SubjectId& lhs, const SubjectId& rhs) noexcept;

private:
    Payload payload_{};
};

// A decision cites evidence by subject and by the committed sequence at which
// that subject's record was committed. Evidence added later cannot retroactively
// appear as evidence that an earlier decision considered.
struct EvidenceRef {
    SubjectId subject{};
    RecordSequence sequence{};
};

struct MetadataEntry {
    std::string key{};
    std::string value{};
};

struct ExperimentInput {
    InputId input{};
    std::string reference{};
};

// --- record payloads --------------------------------------------------------
struct SessionOpened {
    ResearchSessionId session{};
    std::string label{};
    std::string question{};
    std::vector<MetadataEntry> metadata{};
    std::string authority{};
};

struct SessionClosed {
    ResearchSessionId session{};
    std::string note{};
};

// The only record a closed session accepts: a bounded post-hoc annotation. It
// never rewrites the past, it adds to it.
struct SessionAnnotation {
    ResearchSessionId session{};
    std::string note{};
};

struct HypothesisDeclared {
    HypothesisId hypothesis{};
    HypothesisGeneration generation{};
    ResearchSessionId session{};
    std::string claim{};
    std::optional<HypothesisId> parent{};
    HypothesisGeneration parent_generation{};
    std::string authority{};
};

struct HypothesisStatusChanged {
    HypothesisId hypothesis{};
    HypothesisGeneration generation{};
    HypothesisStatus from = HypothesisStatus::Invalid;
    HypothesisStatus to = HypothesisStatus::Invalid;
    std::optional<DecisionId> decision{};
    std::string authority{};
};

struct ExperimentDeclared {
    ExperimentId experiment{};
    ExperimentGeneration generation{};
    ResearchSessionId session{};
    std::vector<HypothesisId> hypotheses{};
    BranchId branch{};
    std::optional<ExperimentId> parent_experiment{};
    std::string environment_reference{};
    std::vector<ExperimentInput> inputs{};
    std::vector<std::string> expected_outputs{};
    std::string authority{};
};

struct BranchDeclared {
    BranchId branch{};
    ResearchSessionId session{};
    BranchKind kind = BranchKind::Root;
    std::optional<BranchId> parent_branch{};
    std::string label{};
    std::string authority{};
};

struct AttemptStarted {
    AttemptId attempt{};
    AttemptGeneration generation{};
    ExperimentId experiment{};
    BranchId branch{};
    std::string worker_authority{};
};

struct AttemptCompleted {
    AttemptId attempt{};
    AttemptGeneration generation{};
    std::string outcome_reference{};
};

struct AttemptFailed {
    AttemptId attempt{};
    AttemptGeneration generation{};
    FailureId failure{};
};

struct AttemptCancelled {
    AttemptId attempt{};
    AttemptGeneration generation{};
    std::string reason{};
};

struct ModelCallRecorded {
    ModelCallId call{};
    AttemptId attempt{};
    std::string model_identity{};
    std::string model_revision{};
    std::string provider{};
    std::string configuration_digest{};
    std::optional<Digest> input_reference{};
    std::optional<Digest> output_reference{};
    InputTokens input_tokens{};
    OutputTokens output_tokens{};
    WallNanos latency{};
    MonetaryMeasure cost{};
    ModelCallOutcome outcome = ModelCallOutcome::OutcomeUnknown;
    std::vector<ModelCallId> parent_calls{};
    std::string authority{};
};

struct ToolCallRecorded {
    ToolCallId call{};
    AttemptId attempt{};
    std::string tool_identity{};
    std::string tool_version{};
    std::optional<Digest> request_reference{};
    std::optional<Digest> output_reference{};
    ToolCallState state = ToolCallState::OutcomeUnknown;
    AccountingVector accounting{};
    std::string authority{};
};

struct ArtifactReferenced {
    ArtifactId artifact{};
    ArtifactGeneration generation{};
    ResearchSessionId session{};
    Digest content_digest{};
    ArtifactRole role = ArtifactRole::Other;
    std::string media_type{};
    SubjectId producer{};
    std::string location{};
    std::vector<ArtifactId> parents{};
    ValidationState validation = ValidationState::Unvalidated;
    std::string authority{};
};

struct ArtifactInvalidated {
    ArtifactId artifact{};
    ArtifactGeneration generation{};
    std::optional<FailureId> failure{};
    std::optional<DecisionId> decision{};
    std::string reason{};
};

struct MetricDeclared {
    MetricId metric{};
    ResearchSessionId session{};
    std::string canonical_key{};
    UnitKind unit = UnitKind::None;
    MetricValueKind value_kind = MetricValueKind::Unknown;
    std::string description{};
    std::string authority{};
};

struct ObservationRecorded {
    ObservationId observation{};
    AttemptId attempt{};
    std::optional<MetricId> metric{};
    std::string key{};
    UnitKind unit = UnitKind::None;
    MetricValue value{};
    TimestampNs measured_at{};
    std::string source{};
    std::string authority{};
};

struct FailureRecorded {
    FailureId failure{};
    SubjectId scope{};
    FailureCategory category = FailureCategory::Unknown;
    std::string message{};
    bool retriable = false;
    bool terminal = true;
    std::optional<FailureId> predecessor{};
    std::optional<AttemptId> recovery_attempt{};
    std::string authority{};
};

struct DecisionRecorded {
    DecisionId decision{};
    ResearchSessionId session{};
    SubjectId subject{};
    DecisionType type = DecisionType::Invalid;
    DecisionOutcome outcome = DecisionOutcome::Invalid;
    std::string policy_identity{};
    PolicyGeneration policy_generation{};
    std::vector<EvidenceRef> evidence{};
    std::string explanation{};
    std::string authority{};
};

struct ResultDeclared {
    ResultId result{};
    ResultGeneration generation{};
    ResearchSessionId session{};
    std::string summary{};
    std::vector<HypothesisId> hypotheses{};
    std::vector<ExperimentId> experiments{};
    std::vector<ArtifactId> artifacts{};
    std::vector<ObservationId> observations{};
    std::vector<ModelCallId> model_calls{};
    std::vector<ToolCallId> tool_calls{};
    std::optional<Digest> content_digest{};
    std::string authority{};
};

struct ResultStatusChanged {
    ResultId result{};
    ResultGeneration generation{};
    ResultStatus from = ResultStatus::Invalid;
    ResultStatus to = ResultStatus::Invalid;
    DecisionId decision{};
};

struct AccountingRecorded {
    SubjectId scope{};
    AccountingVector accounting{};
    std::string source{};
    std::string authority{};
};

// --- views ------------------------------------------------------------------
// Views are plain values copied out of a snapshot. No view exposes a container
// owned by the ledger.
struct SessionView {
    ResearchSessionId session{};
    std::string label{};
    std::string question{};
    std::vector<MetadataEntry> metadata{};
    SessionState state = SessionState::Invalid;
    RecordSequence opened_at{};
    RecordSequence closed_at{};
    std::uint64_t hypothesis_count = 0;
    std::uint64_t experiment_count = 0;
    std::uint64_t result_count = 0;
    Provenance provenance = Provenance::Unknown;
    std::string authority{};
};

struct HypothesisView {
    HypothesisId hypothesis{};
    HypothesisGeneration generation{};
    ResearchSessionId session{};
    std::string claim{};
    HypothesisStatus status = HypothesisStatus::Invalid;
    std::optional<HypothesisId> parent{};
    HypothesisGeneration parent_generation{};
    std::vector<ExperimentId> experiments{};
    RecordSequence declared_at{};
    RecordSequence last_changed_at{};
    Provenance provenance = Provenance::Unknown;
    std::string authority{};
};

struct BranchView {
    BranchId branch{};
    ResearchSessionId session{};
    BranchKind kind = BranchKind::Invalid;
    std::optional<BranchId> parent_branch{};
    std::string label{};
    std::vector<ExperimentId> experiments{};
    std::uint64_t attempt_count = 0;
    bool unresolved = false;
    RecordSequence declared_at{};
    Provenance provenance = Provenance::Unknown;
    std::string authority{};
};

struct ExperimentView {
    ExperimentId experiment{};
    ExperimentGeneration generation{};
    ResearchSessionId session{};
    std::vector<HypothesisId> hypotheses{};
    BranchId branch{};
    std::optional<ExperimentId> parent_experiment{};
    std::string environment_reference{};
    std::vector<ExperimentInput> inputs{};
    std::vector<std::string> expected_outputs{};
    std::vector<AttemptId> attempts{};
    RecordSequence declared_at{};
    Provenance provenance = Provenance::Unknown;
    std::string authority{};
};

struct AttemptView {
    AttemptId attempt{};
    AttemptGeneration generation{};
    ExperimentId experiment{};
    BranchId branch{};
    AttemptState state = AttemptState::Invalid;
    std::string worker_authority{};
    std::optional<FailureId> failure{};
    std::uint64_t model_call_count = 0;
    std::uint64_t tool_call_count = 0;
    std::uint64_t observation_count = 0;
    RecordSequence started_at{};
    RecordSequence terminated_at{};
    Provenance provenance = Provenance::Unknown;
};

struct ModelCallView {
    ModelCallId call{};
    AttemptId attempt{};
    std::string model_identity{};
    std::string model_revision{};
    std::string provider{};
    std::string configuration_digest{};
    std::optional<Digest> input_reference{};
    std::optional<Digest> output_reference{};
    InputTokens input_tokens{};
    OutputTokens output_tokens{};
    WallNanos latency{};
    MonetaryMeasure cost{};
    ModelCallOutcome outcome = ModelCallOutcome::Invalid;
    std::vector<ModelCallId> parent_calls{};
    RecordSequence recorded_at{};
    Provenance provenance = Provenance::Unknown;
    std::string authority{};
};

struct ToolCallView {
    ToolCallId call{};
    AttemptId attempt{};
    std::string tool_identity{};
    std::string tool_version{};
    std::optional<Digest> request_reference{};
    std::optional<Digest> output_reference{};
    ToolCallState state = ToolCallState::Invalid;
    AccountingVector accounting{};
    RecordSequence recorded_at{};
    Provenance provenance = Provenance::Unknown;
    std::string authority{};
};

struct ArtifactView {
    ArtifactId artifact{};
    ArtifactGeneration generation{};
    ResearchSessionId session{};
    Digest content_digest{};
    ArtifactRole role = ArtifactRole::Invalid;
    std::string media_type{};
    SubjectId producer{};
    std::string location{};
    std::vector<ArtifactId> parents{};
    ValidationState validation = ValidationState::Invalid;
    RecordSequence recorded_at{};
    Provenance provenance = Provenance::Unknown;
    std::string authority{};
};

struct ObservationView {
    ObservationId observation{};
    AttemptId attempt{};
    ExperimentId experiment{};
    ResearchSessionId session{};
    std::optional<MetricId> metric{};
    std::string key{};
    UnitKind unit = UnitKind::None;
    MetricValue value{};
    TimestampNs measured_at{};
    Provenance provenance = Provenance::Unknown;
    std::string source{};
    std::string authority{};
    RecordSequence recorded_at{};
};

struct FailureView {
    FailureId failure{};
    ResearchSessionId session{};
    SubjectId scope{};
    FailureCategory category = FailureCategory::Invalid;
    std::string message{};
    bool retriable = false;
    bool terminal = true;
    std::optional<FailureId> predecessor{};
    std::optional<AttemptId> recovery_attempt{};
    TimestampNs occurred_at{};
    RecordSequence recorded_at{};
    Provenance provenance = Provenance::Unknown;
    std::string authority{};
};

struct DecisionView {
    DecisionId decision{};
    ResearchSessionId session{};
    SubjectId subject{};
    DecisionType type = DecisionType::Invalid;
    DecisionOutcome outcome = DecisionOutcome::Invalid;
    std::string policy_identity{};
    PolicyGeneration policy_generation{};
    std::vector<EvidenceRef> evidence{};
    std::string explanation{};
    TimestampNs decided_at{};
    RecordSequence recorded_at{};
    Provenance provenance = Provenance::Unknown;
    std::string authority{};
};

struct ResultView {
    ResultId result{};
    ResultGeneration generation{};
    ResearchSessionId session{};
    std::string summary{};
    ResultStatus status = ResultStatus::Invalid;
    std::vector<HypothesisId> hypotheses{};
    std::vector<ExperimentId> experiments{};
    std::vector<ArtifactId> artifacts{};
    std::vector<ObservationId> observations{};
    std::vector<ModelCallId> model_calls{};
    std::vector<ToolCallId> tool_calls{};
    std::optional<Digest> content_digest{};
    std::optional<DecisionId> acceptance_decision{};
    std::optional<DecisionId> last_decision{};
    RecordSequence declared_at{};
    RecordSequence last_changed_at{};
    Provenance provenance = Provenance::Unknown;
    std::string authority{};
};

// Transition rules are part of the model, not of the call site: the ledger
// rejects a transition the model forbids instead of trusting a caller.
bool result_transition_allowed(ResultStatus from, ResultStatus to) noexcept;
bool hypothesis_transition_allowed(HypothesisStatus from, HypothesisStatus to) noexcept;
bool result_status_matches_decision(ResultStatus status, DecisionOutcome outcome) noexcept;
bool decision_outcome_matches_type(DecisionType type, DecisionOutcome outcome) noexcept;

}  // namespace research_ledger
