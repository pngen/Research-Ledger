#pragma once

// Shared helpers for the test suite. They build real drafts through the public
// API: a test never reaches inside the ledger.

#include <cstdint>
#include <initializer_list>
#include <string>
#include <vector>

#include "research_ledger/cluster.hpp"
#include "research_ledger/codec.hpp"
#include "research_ledger/ledger.hpp"
#include "research_ledger/persistence.hpp"
#include "research_ledger/replay.hpp"

namespace research_ledger {
namespace test {

inline Digest digest_of(std::string_view text) { return sha256(text); }

inline RecordDraft draft_of(RecordBody body, Provenance provenance = Provenance::Measured,
                            LedgerRecordId record_id = LedgerRecordId{}, bool idempotent = false) {
    RecordDraft draft;
    draft.body = std::move(body);
    draft.provenance = provenance;
    draft.record_id = record_id;
    draft.idempotent = idempotent;
    return draft;
}

inline RecordDraft session_opened(std::uint64_t id, std::string label = "session",
                                  std::string question = "does the ledger hold?") {
    SessionOpened body;
    body.session = ResearchSessionId::from_value(id);
    body.label = std::move(label);
    body.question = std::move(question);
    body.authority = "researcher";
    return draft_of(body, Provenance::Reported);
}

inline RecordDraft hypothesis_declared(std::uint64_t id, std::uint64_t session, std::string claim) {
    HypothesisDeclared body;
    body.hypothesis = HypothesisId::from_value(id);
    body.generation = HypothesisGeneration::first();
    body.session = ResearchSessionId::from_value(session);
    body.claim = std::move(claim);
    body.authority = "researcher";
    return draft_of(body, Provenance::Reported);
}

inline RecordDraft branch_declared(std::uint64_t id, std::uint64_t session,
                                   BranchKind kind = BranchKind::Root,
                                   std::uint64_t parent = 0) {
    BranchDeclared body;
    body.branch = BranchId::from_value(id);
    body.session = ResearchSessionId::from_value(session);
    body.kind = kind;
    if (parent != 0) {
        body.parent_branch = BranchId::from_value(parent);
    }
    body.label = "branch";
    body.authority = "researcher";
    return draft_of(body, Provenance::Reported);
}

inline RecordDraft experiment_declared(std::uint64_t id, std::uint64_t session,
                                       std::uint64_t hypothesis, std::uint64_t branch,
                                       std::uint64_t parent_experiment = 0) {
    ExperimentDeclared body;
    body.experiment = ExperimentId::from_value(id);
    body.generation = ExperimentGeneration::first();
    body.session = ResearchSessionId::from_value(session);
    body.hypotheses.push_back(HypothesisId::from_value(hypothesis));
    body.branch = BranchId::from_value(branch);
    if (parent_experiment != 0) {
        body.parent_experiment = ExperimentId::from_value(parent_experiment);
    }
    body.environment_reference = "env:test";
    body.authority = "researcher";
    return draft_of(body, Provenance::Reported);
}

inline RecordDraft attempt_started(std::uint64_t id, std::uint64_t experiment, std::uint64_t branch,
                                   std::string authority = "worker:1") {
    AttemptStarted body;
    body.attempt = AttemptId::from_value(id);
    body.generation = AttemptGeneration::first();
    body.experiment = ExperimentId::from_value(experiment);
    body.branch = BranchId::from_value(branch);
    body.worker_authority = std::move(authority);
    return draft_of(body, Provenance::Reported);
}

inline RecordDraft attempt_completed(std::uint64_t id, std::string reference = "outcome") {
    AttemptCompleted body;
    body.attempt = AttemptId::from_value(id);
    body.generation = AttemptGeneration::first();
    body.outcome_reference = std::move(reference);
    return draft_of(body, Provenance::Reported);
}

inline RecordDraft attempt_cancelled(std::uint64_t id, std::string reason = "cancelled") {
    AttemptCancelled body;
    body.attempt = AttemptId::from_value(id);
    body.generation = AttemptGeneration::first();
    body.reason = std::move(reason);
    return draft_of(body, Provenance::Reported);
}

inline RecordDraft attempt_failed(std::uint64_t id, std::uint64_t failure) {
    AttemptFailed body;
    body.attempt = AttemptId::from_value(id);
    body.generation = AttemptGeneration::first();
    body.failure = FailureId::from_value(failure);
    return draft_of(body, Provenance::Measured);
}

inline RecordDraft failure_recorded(std::uint64_t id, SubjectId scope,
                                    FailureCategory category = FailureCategory::Execution,
                                    std::string message = "failed") {
    FailureRecorded body;
    body.failure = FailureId::from_value(id);
    body.scope = scope;
    body.category = category;
    body.message = std::move(message);
    body.retriable = true;
    body.terminal = true;
    body.authority = "worker:1";
    return draft_of(body, Provenance::Measured);
}

inline RecordDraft model_call_recorded(std::uint64_t id, std::uint64_t attempt,
                                       std::string model = "model-x",
                                       ModelCallOutcome outcome = ModelCallOutcome::Succeeded,
                                       std::uint64_t input_tokens = 100,
                                       std::uint64_t output_tokens = 20) {
    ModelCallRecorded body;
    body.call = ModelCallId::from_value(id);
    body.attempt = AttemptId::from_value(attempt);
    body.model_identity = std::move(model);
    body.model_revision = "r1";
    body.provider = "reference-provider";
    body.configuration_digest = "cfg";
    body.input_reference = digest_of("input-payload");
    body.output_reference = digest_of("output-payload");
    body.input_tokens = InputTokens::known(input_tokens, Provenance::Measured);
    body.output_tokens = OutputTokens::known(output_tokens, Provenance::Measured);
    body.latency = WallNanos::known(1000, Provenance::Measured);
    body.cost = MonetaryMeasure::known_amount(1500, "USD", Provenance::Reported);
    body.outcome = outcome;
    body.authority = "worker:1";
    return draft_of(body, Provenance::Reported);
}

inline RecordDraft tool_call_recorded(std::uint64_t id, std::uint64_t attempt,
                                      std::string tool = "normalizer",
                                      ToolCallState state = ToolCallState::Completed) {
    ToolCallRecorded body;
    body.call = ToolCallId::from_value(id);
    body.attempt = AttemptId::from_value(attempt);
    body.tool_identity = std::move(tool);
    body.tool_version = "1.2";
    body.request_reference = digest_of("tool-request");
    body.output_reference = digest_of("tool-output");
    body.state = state;
    body.accounting.cpu_nanos = CpuNanos::known(5000, Provenance::Measured);
    body.accounting.tool_calls = ToolCalls::known(1, Provenance::Measured);
    body.authority = "worker:1";
    return draft_of(body, Provenance::Reported);
}

inline RecordDraft artifact_referenced(std::uint64_t id, std::uint64_t session, SubjectId producer,
                                       ArtifactRole role = ArtifactRole::Intermediate,
                                       std::string content = "artifact",
                                       std::vector<ArtifactId> parents = {}) {
    ArtifactReferenced body;
    body.artifact = ArtifactId::from_value(id);
    body.generation = ArtifactGeneration::first();
    body.session = ResearchSessionId::from_value(session);
    body.content_digest = digest_of(content);
    body.role = role;
    body.media_type = "application/octet-stream";
    body.producer = producer;
    body.location = "artifact-fabric://stored";
    body.parents = std::move(parents);
    body.validation = ValidationState::Validated;
    body.authority = "worker:1";
    return draft_of(body, Provenance::Reported);
}

inline RecordDraft metric_declared(std::uint64_t id, std::uint64_t session, std::string key,
                                   UnitKind unit = UnitKind::Ratio,
                                   MetricValueKind kind = MetricValueKind::Ratio) {
    MetricDeclared body;
    body.metric = MetricId::from_value(id);
    body.session = ResearchSessionId::from_value(session);
    body.canonical_key = std::move(key);
    body.unit = unit;
    body.value_kind = kind;
    body.description = "measured research metric";
    body.authority = "researcher";
    return draft_of(body, Provenance::Reported);
}

inline RecordDraft observation_recorded(std::uint64_t id, std::uint64_t attempt,
                                        std::string key, MetricValue value,
                                        UnitKind unit = UnitKind::Ratio) {
    ObservationRecorded body;
    body.observation = ObservationId::from_value(id);
    body.attempt = AttemptId::from_value(attempt);
    body.key = std::move(key);
    body.unit = unit;
    body.value = std::move(value);
    body.measured_at = now_timestamp();
    body.source = "benchmark";
    body.authority = "worker:1";
    return draft_of(body, Provenance::Measured);
}

inline RecordDraft result_declared(std::uint64_t id, std::uint64_t session, std::uint64_t hypothesis,
                                   std::uint64_t experiment, std::vector<ArtifactId> artifacts = {},
                                   std::vector<ObservationId> observations = {}) {
    ResultDeclared body;
    body.result = ResultId::from_value(id);
    body.generation = ResultGeneration::first();
    body.session = ResearchSessionId::from_value(session);
    body.summary = "accepted research result";
    body.hypotheses.push_back(HypothesisId::from_value(hypothesis));
    body.experiments.push_back(ExperimentId::from_value(experiment));
    body.artifacts = std::move(artifacts);
    body.observations = std::move(observations);
    body.content_digest = digest_of("result-payload");
    body.authority = "researcher";
    return draft_of(body, Provenance::Derived);
}

inline RecordDraft decision_recorded(std::uint64_t id, std::uint64_t session, SubjectId subject,
                                     DecisionType type, DecisionOutcome outcome,
                                     std::vector<EvidenceRef> evidence = {},
                                     std::string policy = "policy:acceptance") {
    DecisionRecorded body;
    body.decision = DecisionId::from_value(id);
    body.session = ResearchSessionId::from_value(session);
    body.subject = subject;
    body.type = type;
    body.outcome = outcome;
    body.policy_identity = std::move(policy);
    body.policy_generation = PolicyGeneration::first();
    body.evidence = std::move(evidence);
    body.explanation = "evidence reviewed";
    body.authority = "reviewer";
    return draft_of(body, Provenance::Reported);
}

inline RecordDraft result_status_changed(std::uint64_t result, std::uint32_t generation,
                                         ResultStatus from, ResultStatus to,
                                         std::uint64_t decision) {
    ResultStatusChanged body;
    body.result = ResultId::from_value(result);
    body.generation = ResultGeneration::from_value(generation);
    body.from = from;
    body.to = to;
    body.decision = DecisionId::from_value(decision);
    return draft_of(body, Provenance::Reported);
}

inline RecordDraft accounting_recorded(SubjectId scope, AccountingVector accounting,
                                       std::string source = "worker") {
    AccountingRecorded body;
    body.scope = scope;
    body.accounting = std::move(accounting);
    body.source = std::move(source);
    body.authority = "worker:1";
    return draft_of(body, Provenance::Measured);
}

inline LedgerConfig local_config() {
    LedgerConfig config;
    config.require_worker_admission = false;
    return config;
}

// A fully populated research history: session, hypothesis, control branch,
// failed attempt on a retry branch, successful retry, artifact chain and an
// accepted result with an explicit decision.
struct StandardHistory {
    std::shared_ptr<Ledger> ledger{};
    ResearchSessionId session = ResearchSessionId::from_value(1);
    HypothesisId hypothesis = HypothesisId::from_value(1);
    ExperimentId experiment = ExperimentId::from_value(1);
    BranchId branch = BranchId::from_value(1);
    AttemptId attempt = AttemptId::from_value(1);
    ArtifactId artifact = ArtifactId::from_value(1);
    ResultId result = ResultId::from_value(1);
    DecisionId decision = DecisionId::from_value(1);
};

inline Result<StandardHistory> build_standard_history(const LedgerConfig& config) {
    StandardHistory history;
    auto created = Ledger::create(config);
    if (!created.ok()) {
        return created.status();
    }
    history.ledger = created.value();

    MetricValue ratio = MetricValue::unknown();
    auto ratio_value = MetricValue::ratio(0.91);
    if (!ratio_value.ok()) {
        return ratio_value.status();
    }
    ratio = ratio_value.take();

    const std::vector<RecordDraft> batches[] = {
        {session_opened(1, "autonomous research session", "can the ledger reconstruct the result?"),
         hypothesis_declared(1, 1, "a bounded ledger reconstructs accepted results"),
         branch_declared(1, 1, BranchKind::Root),
         experiment_declared(1, 1, 1, 1),
         attempt_started(1, 1, 1),
         model_call_recorded(1, 1),
         tool_call_recorded(1, 1),
         artifact_referenced(1, 1, SubjectId::of(AttemptId::from_value(1)), ArtifactRole::Intermediate,
                             "intermediate"),
         metric_declared(1, 1, "accuracy", UnitKind::Ratio, MetricValueKind::Ratio),
         observation_recorded(1, 1, "accuracy", ratio, UnitKind::Ratio)},
        {artifact_referenced(2, 1, SubjectId::of(AttemptId::from_value(1)), ArtifactRole::ResultArtifact,
                             "final", {ArtifactId::from_value(1)}),
         attempt_completed(1, "outcome-reference"),
         accounting_recorded(SubjectId::of(AttemptId::from_value(1)),
                             []() {
                                 AccountingVector accounting;
                                 accounting.model_input_tokens =
                                     InputTokens::known(100, Provenance::Measured);
                                 accounting.model_output_tokens =
                                     OutputTokens::known(20, Provenance::Measured);
                                 accounting.attempts = AttemptCount::known(1, Provenance::Measured);
                                 return accounting;
                             }()),
         result_declared(1, 1, 1, 1, {ArtifactId::from_value(2)},
                         {ObservationId::from_value(1)})},
    };

    // Evidence is cited by the sequence at which it was committed, so the
    // sequences are read after the records they name exist.
    for (const std::vector<RecordDraft>& batch : batches) {
        auto outcomes = history.ledger->append_batch(batch);
        if (!outcomes.ok()) {
            return outcomes.status();
        }
    }
    auto snapshot = history.ledger->snapshot();
    auto experiment_sequence = snapshot.subject_sequence(SubjectId::of(ExperimentId::from_value(1)));
    if (!experiment_sequence.ok()) {
        return experiment_sequence.status();
    }
    auto artifact_sequence = snapshot.subject_sequence(SubjectId::of(ArtifactId::from_value(2)));
    if (!artifact_sequence.ok()) {
        return artifact_sequence.status();
    }

    const std::vector<RecordDraft> decisions[] = {
        {decision_recorded(1, 1, SubjectId::of(ResultId::from_value(1)), DecisionType::Accept,
                           DecisionOutcome::Accepted,
                           {EvidenceRef{SubjectId::of(ExperimentId::from_value(1)),
                                        experiment_sequence.value()},
                            EvidenceRef{SubjectId::of(ArtifactId::from_value(2)),
                                        artifact_sequence.value()}}),
         result_status_changed(1, 2, ResultStatus::Candidate, ResultStatus::Accepted, 1)},
    };

    for (const std::vector<RecordDraft>& batch : decisions) {
        auto outcomes = history.ledger->append_batch(batch);
        if (!outcomes.ok()) {
            return outcomes.status();
        }
    }
    return history;
}

}  // namespace test
}  // namespace research_ledger
