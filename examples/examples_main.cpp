// Runnable examples over the public Research Ledger API.
//
// Every example builds a real ledger, commits real records and prints what it
// observed. Nothing here reaches inside the runtime: the examples use exactly
// the API a downstream consumer uses, so an example that stops working becomes
// a failing example rather than an aspirational comment.

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "research_ledger/evidence.hpp"
#include "research_ledger/ledger.hpp"
#include "research_ledger/persistence.hpp"
#include "research_ledger/replay.hpp"

namespace research_ledger {
namespace {

// --- reporting --------------------------------------------------------------

int g_failures = 0;

void fail(const std::string& what) {
    ++g_failures;
    std::printf("  FAILED: %s\n", what.c_str());
}

bool expect(bool condition, const std::string& what) {
    if (!condition) {
        fail(what);
    }
    return condition;
}

template <class T>
bool ok(const Result<T>& result, const std::string& what) {
    if (!result.ok()) {
        fail(what + " -> " + std::string(error_code_name(result.code())) + ": " + result.message());
        return false;
    }
    return true;
}

bool ok(const Status& status, const std::string& what) {
    if (!status.ok()) {
        fail(what + " -> " + std::string(error_code_name(status.code)) + ": " + status.message);
        return false;
    }
    return true;
}

template <class Integer>
std::string number_text(Integer value) {
    return std::to_string(value);
}

std::string measure_text(const AccountingVector& accounting) {
    std::string text = "input_tokens=";
    text += accounting.model_input_tokens.is_known()
                ? number_text(accounting.model_input_tokens.units())
                : std::string("UNKNOWN");
    text += " output_tokens=";
    text += accounting.model_output_tokens.is_known()
                ? number_text(accounting.model_output_tokens.units())
                : std::string("UNKNOWN");
    text += " attempts=";
    text += accounting.attempts.is_known() ? number_text(accounting.attempts.units())
                                           : std::string("UNKNOWN");
    return text;
}

// --- record builders --------------------------------------------------------

RecordDraft draft_of(RecordBody body, Provenance provenance) {
    RecordDraft draft;
    draft.body = std::move(body);
    draft.provenance = provenance;
    return draft;
}

RecordDraft session_opened(std::uint64_t id, std::string label, std::string question) {
    SessionOpened body;
    body.session = ResearchSessionId::from_value(id);
    body.label = std::move(label);
    body.question = std::move(question);
    body.authority = "researcher:alice";
    return draft_of(body, Provenance::Reported);
}

RecordDraft hypothesis_declared(std::uint64_t id, std::uint64_t session, std::string claim) {
    HypothesisDeclared body;
    body.hypothesis = HypothesisId::from_value(id);
    body.generation = HypothesisGeneration::first();
    body.session = ResearchSessionId::from_value(session);
    body.claim = std::move(claim);
    body.authority = "researcher:alice";
    return draft_of(body, Provenance::Reported);
}

RecordDraft branch_declared(std::uint64_t id, std::uint64_t session, BranchKind kind,
                            std::uint64_t parent) {
    BranchDeclared body;
    body.branch = BranchId::from_value(id);
    body.session = ResearchSessionId::from_value(session);
    body.kind = kind;
    if (parent != 0) {
        body.parent_branch = BranchId::from_value(parent);
    }
    body.label = "branch";
    body.authority = "researcher:alice";
    return draft_of(body, Provenance::Reported);
}

RecordDraft experiment_declared(std::uint64_t id, std::uint64_t session, std::uint64_t hypothesis,
                                std::uint64_t branch) {
    ExperimentDeclared body;
    body.experiment = ExperimentId::from_value(id);
    body.generation = ExperimentGeneration::first();
    body.session = ResearchSessionId::from_value(session);
    body.hypotheses.push_back(HypothesisId::from_value(hypothesis));
    body.branch = BranchId::from_value(branch);
    body.environment_reference = "container:python-3.12";
    body.authority = "researcher:alice";
    return draft_of(body, Provenance::Reported);
}

RecordDraft attempt_started(std::uint64_t id, std::uint64_t experiment, std::uint64_t branch) {
    AttemptStarted body;
    body.attempt = AttemptId::from_value(id);
    body.generation = AttemptGeneration::first();
    body.experiment = ExperimentId::from_value(experiment);
    body.branch = BranchId::from_value(branch);
    body.worker_authority = "worker:7";
    return draft_of(body, Provenance::Reported);
}

RecordDraft attempt_completed(std::uint64_t id, std::string reference = "outcome") {
    AttemptCompleted body;
    body.attempt = AttemptId::from_value(id);
    body.generation = AttemptGeneration::first();
    body.outcome_reference = std::move(reference);
    return draft_of(body, Provenance::Reported);
}

RecordDraft failure_recorded(std::uint64_t id, SubjectId scope, FailureCategory category,
                             std::string message) {
    FailureRecorded body;
    body.failure = FailureId::from_value(id);
    body.scope = scope;
    body.category = category;
    body.message = std::move(message);
    body.retriable = true;
    body.terminal = true;
    body.authority = "worker:7";
    return draft_of(body, Provenance::Measured);
}

RecordDraft attempt_failed(std::uint64_t id, std::uint64_t failure) {
    AttemptFailed body;
    body.attempt = AttemptId::from_value(id);
    body.generation = AttemptGeneration::first();
    body.failure = FailureId::from_value(failure);
    return draft_of(body, Provenance::Measured);
}

RecordDraft tool_call_recorded(std::uint64_t id, std::uint64_t attempt, std::string tool) {
    ToolCallRecorded body;
    body.call = ToolCallId::from_value(id);
    body.attempt = AttemptId::from_value(attempt);
    body.tool_identity = std::move(tool);
    body.tool_version = "2.4.1";
    body.request_reference = sha256("tool-request");
    body.output_reference = sha256("tool-output");
    body.state = ToolCallState::Completed;
    body.accounting.cpu_nanos = CpuNanos::known(4000000, Provenance::Measured);
    body.accounting.tool_calls = ToolCalls::known(1, Provenance::Measured);
    body.authority = "worker:7";
    return draft_of(body, Provenance::Reported);
}

RecordDraft artifact_referenced(std::uint64_t id, std::uint64_t session, SubjectId producer,
                                ArtifactRole role, std::string content,
                                std::vector<ArtifactId> parents = {}) {
    ArtifactReferenced body;
    body.artifact = ArtifactId::from_value(id);
    body.generation = ArtifactGeneration::first();
    body.session = ResearchSessionId::from_value(session);
    body.content_digest = sha256(content);
    body.role = role;
    body.media_type = "application/octet-stream";
    body.producer = producer;
    body.location = "artifact-fabric://research-ledger-examples/" + content;
    body.parents = std::move(parents);
    body.validation = ValidationState::Validated;
    body.authority = "worker:7";
    return draft_of(body, Provenance::Reported);
}

RecordDraft metric_declared(std::uint64_t id, std::uint64_t session, std::string key) {
    MetricDeclared body;
    body.metric = MetricId::from_value(id);
    body.session = ResearchSessionId::from_value(session);
    body.canonical_key = std::move(key);
    body.unit = UnitKind::Ratio;
    body.value_kind = MetricValueKind::Ratio;
    body.description = "held-out accuracy";
    body.authority = "researcher:alice";
    return draft_of(body, Provenance::Reported);
}

RecordDraft observation_recorded(std::uint64_t id, std::uint64_t attempt, std::uint64_t metric,
                                 std::string key, double value) {
    ObservationRecorded body;
    body.observation = ObservationId::from_value(id);
    body.attempt = AttemptId::from_value(attempt);
    body.metric = MetricId::from_value(metric);
    body.key = std::move(key);
    body.unit = UnitKind::Ratio;
    body.value = MetricValue::ratio(value).value();
    body.measured_at = now_timestamp();
    body.source = "held-out evaluation";
    body.authority = "worker:7";
    return draft_of(body, Provenance::Measured);
}

RecordDraft result_declared(std::uint64_t id, std::uint64_t session, std::uint64_t hypothesis,
                            std::uint64_t experiment, std::vector<ArtifactId> artifacts = {},
                            std::vector<ObservationId> observations = {},
                            std::vector<ModelCallId> model_calls = {}) {
    ResultDeclared body;
    body.result = ResultId::from_value(id);
    body.generation = ResultGeneration::first();
    body.session = ResearchSessionId::from_value(session);
    body.summary = "held-out accuracy improved over the control";
    body.hypotheses.push_back(HypothesisId::from_value(hypothesis));
    body.experiments.push_back(ExperimentId::from_value(experiment));
    body.artifacts = std::move(artifacts);
    body.observations = std::move(observations);
    body.model_calls = std::move(model_calls);
    body.content_digest = sha256("result-payload");
    body.authority = "researcher:alice";
    return draft_of(body, Provenance::Derived);
}

RecordDraft decision_recorded(std::uint64_t id, std::uint64_t session, SubjectId subject,
                              DecisionType type, DecisionOutcome outcome,
                              std::vector<EvidenceRef> evidence) {
    DecisionRecorded body;
    body.decision = DecisionId::from_value(id);
    body.session = ResearchSessionId::from_value(session);
    body.subject = subject;
    body.type = type;
    body.outcome = outcome;
    body.policy_identity = "policy:acceptance-v3";
    body.policy_generation = PolicyGeneration::first();
    body.evidence = std::move(evidence);
    body.explanation = "evidence reviewed by the acceptance policy";
    body.authority = "reviewer:bob";
    return draft_of(body, Provenance::Reported);
}

RecordDraft result_status_changed(std::uint64_t result, std::uint32_t generation, ResultStatus from,
                                  ResultStatus to, std::uint64_t decision) {
    ResultStatusChanged body;
    body.result = ResultId::from_value(result);
    body.generation = ResultGeneration::from_value(generation);
    body.from = from;
    body.to = to;
    body.decision = DecisionId::from_value(decision);
    return draft_of(body, Provenance::Reported);
}

RecordDraft accounting_recorded(SubjectId scope, AccountingVector accounting, std::string source) {
    AccountingRecorded body;
    body.scope = scope;
    body.accounting = std::move(accounting);
    body.source = std::move(source);
    body.authority = "worker:7";
    return draft_of(body, Provenance::Measured);
}

RecordDraft model_call_recorded(std::uint64_t id, std::uint64_t attempt, std::string model,
                                InputTokens input_tokens, OutputTokens output_tokens) {
    ModelCallRecorded body;
    body.call = ModelCallId::from_value(id);
    body.attempt = AttemptId::from_value(attempt);
    body.model_identity = std::move(model);
    body.model_revision = "2026-01-14";
    body.provider = "reference-provider";
    body.configuration_digest = to_hex(sha256("temperature=0;max_tokens=2048"));
    body.input_reference = sha256("prompt-bytes");
    body.output_reference = sha256("completion-bytes");
    body.input_tokens = input_tokens;
    body.output_tokens = output_tokens;
    body.latency = WallNanos::known(1500000, Provenance::Measured);
    body.cost = MonetaryMeasure::known_amount(1200, "USD", Provenance::Reported);
    body.outcome = ModelCallOutcome::Succeeded;
    body.authority = "worker:7";
    return draft_of(body, Provenance::Reported);
}

std::shared_ptr<Ledger> open_ledger() {
    auto created = Ledger::create(LedgerConfig{});
    if (!ok(created, "Ledger::create")) {
        return nullptr;
    }
    return created.take();
}

std::filesystem::path scratch_path(const std::string& name) {
    std::error_code error;
    const std::filesystem::path directory = std::filesystem::temp_directory_path(error);
    if (error) {
        return std::filesystem::path(name);
    }
    return directory / name;
}

std::string decision_text(const std::optional<DecisionId>& decision) {
    return decision.has_value() ? to_string(*decision) : std::string("none");
}

// --- 1: basic research session ----------------------------------------------

bool example_01_basic_session() {
    std::printf("\n[1] basic research session\n");
    std::shared_ptr<Ledger> ledger = open_ledger();
    if (ledger == nullptr) {
        return false;
    }
    auto appended = ledger->append(session_opened(
        1, "protein folding sweep", "does the ledger reconstruct the accepted result?"));
    if (!ok(appended, "append(SessionOpened)")) {
        return false;
    }
    LedgerSnapshot snapshot = ledger->snapshot();
    auto session = snapshot.session(ResearchSessionId::from_value(1));
    if (!ok(session, "snapshot.session")) {
        return false;
    }
    std::printf("  observed: %s state=%s label=%s records=%s\n",
                to_string(session.value().session).c_str(),
                std::string(session_state_name(session.value().state)).c_str(),
                session.value().label.c_str(),
                number_text(ledger->last_sequence().value()).c_str());
    bool passed = true;
    passed = expect(session.value().state == SessionState::Open, "a new session is OPEN") && passed;
    passed = expect(ledger->last_sequence().value() == 1, "exactly one record is committed") && passed;
    return passed;
}

// --- 2: hypothesis -> experiment -> result ----------------------------------

bool example_02_result_lineage() {
    std::printf("\n[2] hypothesis to experiment to result lineage\n");
    std::shared_ptr<Ledger> ledger = open_ledger();
    if (ledger == nullptr) {
        return false;
    }
    std::vector<RecordDraft> setup{
        session_opened(1, "lineage walk", "does the result carry its lineage?"),
        hypothesis_declared(1, 1, "quantization preserves held-out accuracy"),
        branch_declared(1, 1, BranchKind::Root, 0),
        experiment_declared(1, 1, 1, 1),
        attempt_started(1, 1, 1),
        attempt_completed(1),
        result_declared(1, 1, 1, 1),
    };
    if (!ok(ledger->append_batch(setup), "append_batch(lineage)")) {
        return false;
    }
    LedgerSnapshot snapshot = ledger->snapshot();
    auto result = snapshot.result(ResultId::from_value(1));
    auto ancestry = snapshot.hypothesis_ancestry(HypothesisId::from_value(1));
    auto bundle = snapshot.supporting_evidence(ResultId::from_value(1));
    if (!ok(result, "snapshot.result") || !ok(ancestry, "hypothesis_ancestry") ||
        !ok(bundle, "supporting_evidence")) {
        return false;
    }
    std::printf("  observed: %s status=%s experiments=experiment:%s hypothesis_ancestry=%s "
                "evidence(experiments=%s attempts=%s complete=%s)\n",
                to_string(result.value().result).c_str(),
                std::string(result_status_name(result.value().status)).c_str(),
                number_text(result.value().experiments.front().value()).c_str(),
                number_text(ancestry.value().size()).c_str(),
                number_text(bundle.value().experiments.size()).c_str(),
                number_text(bundle.value().attempts.size()).c_str(),
                bundle.value().complete ? "true" : "false");
    bool passed = true;
    passed = expect(result.value().status == ResultStatus::Candidate,
                    "a declared result starts as CANDIDATE") && passed;
    passed = expect(bundle.value().complete, "the evidence closure is complete") && passed;
    return passed;
}

// --- 3: failed branch and successful branch both survive ---------------------

bool example_03_two_branches() {
    std::printf("\n[3] failed branch and successful branch both kept in history\n");
    std::shared_ptr<Ledger> ledger = open_ledger();
    if (ledger == nullptr) {
        return false;
    }
    std::vector<RecordDraft> root{
        session_opened(1, "retry study", "does a failed branch stay in history?"),
        hypothesis_declared(1, 1, "wider batches converge faster"),
        branch_declared(1, 1, BranchKind::Root, 0),
        experiment_declared(1, 1, 1, 1),
        attempt_started(1, 1, 1),
    };
    if (!ok(ledger->append_batch(root), "append_batch(root branch)")) {
        return false;
    }
    std::vector<RecordDraft> failed{
        failure_recorded(1, SubjectId::of(AttemptId::from_value(1)), FailureCategory::Resource,
                         "accelerator ran out of memory"),
        attempt_failed(1, 1),
    };
    if (!ok(ledger->append_batch(failed), "append_batch(failure)")) {
        return false;
    }
    std::vector<RecordDraft> retry{
        branch_declared(2, 1, BranchKind::Retry, 1),
        experiment_declared(2, 1, 1, 2),
        attempt_started(2, 2, 2),
        attempt_completed(2, "narrower batches converged"),
        result_declared(1, 1, 1, 2),
    };
    if (!ok(ledger->append_batch(retry), "append_batch(retry branch)")) {
        return false;
    }
    LedgerSnapshot snapshot = ledger->snapshot();
    auto experiment_sequence = snapshot.subject_sequence(SubjectId::of(ExperimentId::from_value(2)));
    if (!ok(experiment_sequence, "subject_sequence(experiment)")) {
        return false;
    }
    std::vector<RecordDraft> acceptance{
        decision_recorded(1, 1, SubjectId::of(ResultId::from_value(1)), DecisionType::Accept,
                          DecisionOutcome::Accepted,
                          {EvidenceRef{SubjectId::of(ExperimentId::from_value(2)),
                                       experiment_sequence.value()}}),
        result_status_changed(1, 2, ResultStatus::Candidate, ResultStatus::Accepted, 1),
    };
    if (!ok(ledger->append_batch(acceptance), "append_batch(acceptance)")) {
        return false;
    }
    snapshot = ledger->snapshot();
    auto failed_attempt = snapshot.attempt(AttemptId::from_value(1));
    auto retry_attempt = snapshot.attempt(AttemptId::from_value(2));
    auto failures = snapshot.failures();
    auto unresolved = snapshot.unresolved_branches(ResearchSessionId::from_value(1));
    auto stats = snapshot.stats();
    auto result = snapshot.result(ResultId::from_value(1));
    if (!ok(failed_attempt, "snapshot.attempt(1)") || !ok(retry_attempt, "snapshot.attempt(2)") ||
        !ok(failures, "snapshot.failures") || !ok(unresolved, "unresolved_branches") ||
        !ok(stats, "snapshot.stats") || !ok(result, "snapshot.result")) {
        return false;
    }
    std::printf("  observed: attempt:1=%s attempt:2=%s failures=%s branches=%s result=%s "
                "unresolved_branches=%s\n",
                std::string(attempt_state_name(failed_attempt.value().state)).c_str(),
                std::string(attempt_state_name(retry_attempt.value().state)).c_str(),
                number_text(failures.value().size()).c_str(),
                number_text(stats.value().branches).c_str(),
                std::string(result_status_name(result.value().status)).c_str(),
                number_text(unresolved.value().size()).c_str());
    bool passed = true;
    passed = expect(failures.value().size() == 1, "the failure of branch 1 is still committed") && passed;
    passed = expect(unresolved.value().size() == 1 && unresolved.value().front().value() == 1,
                    "branch 1 stays explicitly unresolved") && passed;
    passed = expect(result.value().status == ResultStatus::Accepted,
                    "the accepted result of branch 2 stays accepted") && passed;
    return passed;
}

// --- 4: model and tool provenance, unknown stays unknown --------------------

bool example_04_provenance() {
    std::printf("\n[4] model and tool call provenance, including an unknown token count\n");
    std::shared_ptr<Ledger> ledger = open_ledger();
    if (ledger == nullptr) {
        return false;
    }
    std::vector<RecordDraft> setup{
        session_opened(1, "provenance study", "is an unknown token count preserved?"),
        hypothesis_declared(1, 1, "the provider reports output tokens"),
        branch_declared(1, 1, BranchKind::Root, 0),
        experiment_declared(1, 1, 1, 1),
        attempt_started(1, 1, 1),
    };
    if (!ok(ledger->append_batch(setup), "append_batch(setup)")) {
        return false;
    }
    // The provider did not report output tokens: the count stays explicitly
    // unknown instead of decaying into zero.
    AccountingVector attempt_accounting;
    attempt_accounting.model_input_tokens = InputTokens::known(1200, Provenance::Measured);
    attempt_accounting.model_output_tokens = OutputTokens::unknown(Provenance::Unknown);
    attempt_accounting.attempts = AttemptCount::known(1, Provenance::Measured);

    std::vector<RecordDraft> calls{
        model_call_recorded(1, 1, "reasoning-model-3",
                            InputTokens::known(1200, Provenance::Measured),
                            OutputTokens::unknown(Provenance::Unknown)),
        tool_call_recorded(1, 1, "normalizer"),
        accounting_recorded(SubjectId::of(AttemptId::from_value(1)), attempt_accounting, "worker:7"),
    };
    if (!ok(ledger->append_batch(calls), "append_batch(model call, tool call, accounting)")) {
        return false;
    }
    LedgerSnapshot snapshot = ledger->snapshot();
    auto model_calls = snapshot.model_calls(AttemptId::from_value(1));
    auto tool_calls = snapshot.tool_calls(AttemptId::from_value(1));
    auto aggregate = snapshot.accounting(SubjectId::of(AttemptId::from_value(1)));
    if (!ok(model_calls, "snapshot.model_calls") || !ok(tool_calls, "snapshot.tool_calls") ||
        !ok(aggregate, "snapshot.accounting")) {
        return false;
    }
    const ModelCallView& call = model_calls.value().front();
    std::printf("  observed: model=%s input_digest=%s output_digest=%s input_tokens=%s "
                "output_tokens=%s tool=%s\n",
                call.model_identity.c_str(), to_hex(*call.input_reference).c_str(),
                to_hex(*call.output_reference).c_str(), number_text(call.input_tokens.units()).c_str(),
                call.output_tokens.is_known() ? "KNOWN" : "UNKNOWN",
                tool_calls.value().front().tool_identity.c_str());
    std::printf("  observed: accounting %s complete=%s contributors_with_unknown_fields=%s\n",
                measure_text(aggregate.value().total).c_str(),
                aggregate.value().complete ? "true" : "false",
                number_text(aggregate.value().contributions_with_unknown_fields).c_str());
    bool passed = true;
    passed = expect(!aggregate.value().total.model_output_tokens.is_known(),
                    "an unknown output token count stays unknown in the aggregate") && passed;
    passed = expect(!aggregate.value().complete,
                    "an aggregate with an unknown field is not complete") && passed;
    return passed;
}

// --- 5: artifact ancestry ---------------------------------------------------

bool example_05_artifacts() {
    std::printf("\n[5] artifact ancestry: dataset to normalized to model\n");
    std::shared_ptr<Ledger> ledger = open_ledger();
    if (ledger == nullptr) {
        return false;
    }
    std::vector<RecordDraft> setup{
        session_opened(1, "artifact lineage", "is the artifact chain reconstructable?"),
        hypothesis_declared(1, 1, "normalization improves accuracy"),
        branch_declared(1, 1, BranchKind::Root, 0),
        experiment_declared(1, 1, 1, 1),
        attempt_started(1, 1, 1),
    };
    if (!ok(ledger->append_batch(setup), "append_batch(setup)")) {
        return false;
    }
    const SubjectId producer = SubjectId::of(AttemptId::from_value(1));
    std::vector<RecordDraft> artifacts{
        artifact_referenced(1, 1, producer, ArtifactRole::Dataset, "dataset-v1"),
        artifact_referenced(2, 1, producer, ArtifactRole::Intermediate, "normalized-v1",
                            {ArtifactId::from_value(1)}),
        artifact_referenced(3, 1, producer, ArtifactRole::Model, "model-v1",
                            {ArtifactId::from_value(2)}),
    };
    if (!ok(ledger->append_batch(artifacts), "append_batch(artifacts)")) {
        return false;
    }
    LedgerSnapshot snapshot = ledger->snapshot();
    auto ancestry = snapshot.artifact_ancestry(ArtifactId::from_value(3));
    auto descendants = snapshot.artifact_descendants(ArtifactId::from_value(1));
    if (!ok(ancestry, "artifact_ancestry") || !ok(descendants, "artifact_descendants")) {
        return false;
    }
    bool passed = true;
    passed = expect(ancestry.value().size() == 2, "the model artifact has two ancestors") && passed;
    passed = expect(descendants.value().size() == 2, "the dataset has two descendants") && passed;
    std::printf("  observed: model ancestors=%s dataset descendants=%s normalized_digest=%s\n",
                number_text(ancestry.value().size()).c_str(),
                number_text(descendants.value().size()).c_str(),
                ancestry.value().size() == 2 ? to_hex(ancestry.value()[0].content_digest).c_str()
                                             : "(unavailable)");
    if (ancestry.value().size() == 2) {
        std::printf("  observed: chain=%s -> %s -> %s\n",
                    to_string(ancestry.value()[0].artifact).c_str(),
                    to_string(ancestry.value()[1].artifact).c_str(),
                    to_string(ArtifactId::from_value(3)).c_str());
    }
    return passed;
}

// --- 6: acceptance decision with cited evidence -----------------------------

bool example_06_acceptance() {
    std::printf("\n[6] acceptance decision with cited evidence\n");
    std::shared_ptr<Ledger> ledger = open_ledger();
    if (ledger == nullptr) {
        return false;
    }
    std::vector<RecordDraft> setup{
        session_opened(1, "acceptance study", "is the acceptance reconstructable?"),
        hypothesis_declared(1, 1, "the method improves held-out accuracy"),
        branch_declared(1, 1, BranchKind::Root, 0),
        experiment_declared(1, 1, 1, 1),
        attempt_started(1, 1, 1),
        metric_declared(1, 1, "held_out_accuracy"),
        observation_recorded(1, 1, 1, "held_out_accuracy", 0.91),
        artifact_referenced(1, 1, SubjectId::of(AttemptId::from_value(1)), ArtifactRole::Report,
                            "report-v1"),
        attempt_completed(1, "evaluation finished"),
        result_declared(1, 1, 1, 1, {ArtifactId::from_value(1)}, {ObservationId::from_value(1)}),
    };
    if (!ok(ledger->append_batch(setup), "append_batch(setup)")) {
        return false;
    }
    LedgerSnapshot snapshot = ledger->snapshot();
    auto experiment_sequence = snapshot.subject_sequence(SubjectId::of(ExperimentId::from_value(1)));
    auto artifact_sequence = snapshot.subject_sequence(SubjectId::of(ArtifactId::from_value(1)));
    if (!ok(experiment_sequence, "subject_sequence(experiment)") ||
        !ok(artifact_sequence, "subject_sequence(artifact)")) {
        return false;
    }
    std::vector<RecordDraft> acceptance{
        decision_recorded(1, 1, SubjectId::of(ResultId::from_value(1)), DecisionType::Accept,
                          DecisionOutcome::Accepted,
                          {EvidenceRef{SubjectId::of(ExperimentId::from_value(1)),
                                       experiment_sequence.value()},
                           EvidenceRef{SubjectId::of(ArtifactId::from_value(1)),
                                       artifact_sequence.value()}}),
        result_status_changed(1, 2, ResultStatus::Candidate, ResultStatus::Accepted, 1),
    };
    if (!ok(ledger->append_batch(acceptance), "append_batch(decision and status change)")) {
        return false;
    }
    snapshot = ledger->snapshot();
    auto bundle = snapshot.supporting_evidence(ResultId::from_value(1));
    if (!ok(bundle, "supporting_evidence")) {
        return false;
    }
    std::printf("  observed: status=%s acceptance_decision=%s cited=%s reconstructable=%s "
                "observations=%s decisions=%s\n",
                std::string(result_status_name(bundle.value().result.status)).c_str(),
                decision_text(bundle.value().result.acceptance_decision).c_str(),
                number_text(bundle.value().cited_evidence.size()).c_str(),
                bundle.value().reconstructable ? "true" : "false",
                number_text(bundle.value().observations.size()).c_str(),
                number_text(bundle.value().decisions.size()).c_str());
    for (const EvidenceRef& reference : bundle.value().cited_evidence) {
        std::printf("  observed: cited %s at sequence %s\n", reference.subject.to_string().c_str(),
                    number_text(reference.sequence.value()).c_str());
    }

    // Evidence is cited as (subject, sequence): a record that is not committed
    // yet cannot be cited, and the append is rejected as a whole.
    const std::uint64_t future = ledger->last_sequence().value() + 5;
    std::vector<RecordDraft> premature{
        decision_recorded(2, 1, SubjectId::of(ResultId::from_value(1)), DecisionType::Audit,
                          DecisionOutcome::Recorded,
                          {EvidenceRef{SubjectId::of(ResultId::from_value(1)),
                                       RecordSequence::from_value(
                                           static_cast<std::uint32_t>(future))}}),
    };
    auto rejected = ledger->append_batch(premature);
    std::printf("  observed: citing an uncommitted sequence -> %s\n",
                std::string(error_code_name(rejected.code())).c_str());
    bool passed = true;
    passed = expect(bundle.value().accepted(), "the result is ACCEPTED") && passed;
    passed = expect(rejected.code() == ErrorCode::InvalidTransition,
                    "evidence that is not committed yet is rejected") && passed;
    return passed;
}

// --- 7: accounting aggregation counts shared ancestry once ------------------

bool example_07_accounting() {
    std::printf("\n[7] accounting aggregation counts shared ancestry once\n");
    std::shared_ptr<Ledger> ledger = open_ledger();
    if (ledger == nullptr) {
        return false;
    }
    std::vector<RecordDraft> setup{
        session_opened(1, "accounting study", "is consumption counted once?"),
        hypothesis_declared(1, 1, "the retry costs less than the original attempt"),
        branch_declared(1, 1, BranchKind::Root, 0),
        experiment_declared(1, 1, 1, 1),
        attempt_started(1, 1, 1),
        attempt_started(2, 1, 1),
    };
    if (!ok(ledger->append_batch(setup), "append_batch(setup)")) {
        return false;
    }
    AccountingVector attempt_one;
    attempt_one.model_input_tokens = InputTokens::known(100, Provenance::Measured);
    attempt_one.model_output_tokens = OutputTokens::known(10, Provenance::Measured);
    attempt_one.attempts = AttemptCount::known(1, Provenance::Measured);

    AccountingVector attempt_two;
    attempt_two.model_input_tokens = InputTokens::known(50, Provenance::Measured);
    attempt_two.model_output_tokens = OutputTokens::unknown(Provenance::Unknown);
    attempt_two.attempts = AttemptCount::known(1, Provenance::Measured);

    AccountingVector call_one;
    call_one.model_input_tokens = InputTokens::known(25, Provenance::Measured);

    std::vector<RecordDraft> accounting{
        model_call_recorded(1, 2, "reasoning-model-3", InputTokens::known(25, Provenance::Measured),
                            OutputTokens::known(5, Provenance::Measured)),
        accounting_recorded(SubjectId::of(AttemptId::from_value(1)), attempt_one, "worker:7"),
        accounting_recorded(SubjectId::of(AttemptId::from_value(2)), attempt_two, "worker:7"),
        accounting_recorded(SubjectId::of(ModelCallId::from_value(1)), call_one, "worker:7"),
        // The result names experiment 1 and model call 1, so the model call is
        // reachable both through the experiment's attempt and directly.
        result_declared(1, 1, 1, 1, {}, {}, {ModelCallId::from_value(1)}),
    };
    if (!ok(ledger->append_batch(accounting), "append_batch(accounting)")) {
        return false;
    }
    LedgerSnapshot snapshot = ledger->snapshot();
    auto by_session = snapshot.accounting(SubjectId::of(ResearchSessionId::from_value(1)));
    auto by_experiment = snapshot.accounting(SubjectId::of(ExperimentId::from_value(1)));
    auto by_attempt = snapshot.accounting(SubjectId::of(AttemptId::from_value(2)));
    auto by_result = snapshot.accounting(SubjectId::of(ResultId::from_value(1)));
    if (!ok(by_session, "accounting(session)") || !ok(by_experiment, "accounting(experiment)") ||
        !ok(by_attempt, "accounting(attempt)") || !ok(by_result, "accounting(result)")) {
        return false;
    }
    std::printf("  observed: session contributions=%s input=%s\n",
                number_text(by_session.value().contributions).c_str(),
                measure_text(by_session.value().total).c_str());
    std::printf("  observed: experiment contributions=%s input=%s\n",
                number_text(by_experiment.value().contributions).c_str(),
                measure_text(by_experiment.value().total).c_str());
    std::printf("  observed: result contributions=%s input=%s (model call reached twice, "
                "counted once)\n",
                number_text(by_result.value().contributions).c_str(),
                measure_text(by_result.value().total).c_str());
    std::printf("  observed: attempt:2 contributions=%s input=%s complete=%s "
                "contributors_with_unknown_fields=%s\n",
                number_text(by_attempt.value().contributions).c_str(),
                measure_text(by_attempt.value().total).c_str(),
                by_session.value().complete ? "true" : "false",
                number_text(by_session.value().contributions_with_unknown_fields).c_str());
    bool passed = true;
    passed = expect(by_session.value().contributions == 3,
                    "three accounting records are counted once each") && passed;
    passed = expect(by_result.value().contributions == 3,
                    "a record reachable by two paths is counted once") && passed;
    passed = expect(by_session.value().total.model_input_tokens.units() == 175,
                    "the shared model call is not added twice") && passed;
    passed = expect(by_attempt.value().total.model_input_tokens.units() == 75,
                    "the attempt scope includes its own model call") && passed;
    passed = expect(!by_session.value().complete,
                    "one unknown contribution keeps the aggregate incomplete") && passed;
    return passed;
}

// --- 8: persistence round trip ----------------------------------------------

bool example_08_persistence() {
    std::printf("\n[8] persistence: save, reload and append again\n");
    std::shared_ptr<Ledger> ledger = open_ledger();
    if (ledger == nullptr) {
        return false;
    }
    std::vector<RecordDraft> setup{
        session_opened(1, "persistence study", "does a reloaded ledger accept appends?"),
        hypothesis_declared(1, 1, "committed history survives a reload"),
        branch_declared(1, 1, BranchKind::Root, 0),
        experiment_declared(1, 1, 1, 1),
    };
    if (!ok(ledger->append_batch(setup), "append_batch(setup)")) {
        return false;
    }
    const std::filesystem::path path = scratch_path("research-ledger-examples-roundtrip.rls");
    if (!ok(ledger->save(path.string()), "Ledger::save")) {
        return false;
    }
    std::error_code error;
    const std::uintmax_t bytes = std::filesystem::file_size(path, error);
    bool passed = true;
    passed = expect(!error, "the snapshot file exists after save") && passed;

    auto reloaded = Ledger::load(path.string(), LedgerConfig{});
    if (!ok(reloaded, "Ledger::load")) {
        return false;
    }
    const std::shared_ptr<Ledger>& second = reloaded.value();
    const bool same_digest = second->logical_digest() == ledger->logical_digest();
    const bool same_sequence = second->last_sequence() == ledger->last_sequence();

    // A reloaded ledger restores the committed epoch, so it accepts appends.
    auto appended = second->append(hypothesis_declared(2, 1, "history loaded from the snapshot file"));
    if (!ok(appended, "append after reload")) {
        return false;
    }
    std::printf("  observed: bytes=%s records_after_reload=%s same_digest=%s "
                "append_after_reload=sequence:%s\n",
                number_text(bytes).c_str(), number_text(second->last_sequence().value()).c_str(),
                same_digest ? "true" : "false",
                number_text(appended.value().sequence.value()).c_str());

    const bool removed = std::filesystem::remove(path);
    passed = expect(same_digest, "the reloaded ledger has the same logical digest") && passed;
    passed = expect(same_sequence, "the reloaded ledger has the same last sequence") && passed;
    passed = expect(appended.value().sequence.value() == 5,
                    "the reloaded ledger accepts a new append at the next sequence") && passed;
    passed = expect(removed, "the example removed its own snapshot file") && passed;
    return passed;
}

// --- 9: deterministic replay ------------------------------------------------

bool example_09_replay() {
    std::printf("\n[9] deterministic replay of committed records\n");
    std::shared_ptr<Ledger> ledger = open_ledger();
    if (ledger == nullptr) {
        return false;
    }
    std::vector<RecordDraft> setup{
        session_opened(1, "replay study", "does replay reproduce the logical state?"),
        hypothesis_declared(1, 1, "replay is deterministic"),
        branch_declared(1, 1, BranchKind::Root, 0),
        experiment_declared(1, 1, 1, 1),
        attempt_started(1, 1, 1),
        attempt_completed(1),
        result_declared(1, 1, 1, 1),
    };
    if (!ok(ledger->append_batch(setup), "append_batch(setup)")) {
        return false;
    }
    LedgerSnapshot snapshot = ledger->snapshot();
    auto records = snapshot.records(RecordSequence::from_value(1), ledger->last_sequence());
    if (!ok(records, "snapshot.records")) {
        return false;
    }
    LedgerConfig config;
    auto first = replay_committed(records.value(), config);
    auto second = replay_committed(records.value(), config);
    if (!ok(first, "replay_committed (first)") || !ok(second, "replay_committed (second)")) {
        return false;
    }
    const Digest live = ledger->logical_digest();
    std::printf("  observed: records=%s logical_digest=%s\n",
                number_text(first.value().records).c_str(),
                to_hex(first.value().logical_digest).c_str());
    std::printf("  observed: replay_repeat=%s live=%s chain_digest=%s\n",
                to_hex(second.value().logical_digest).c_str(), to_hex(live).c_str(),
                to_hex(first.value().chain_digest).c_str());
    bool passed = true;
    passed = expect(first.value().logical_digest == second.value().logical_digest,
                    "replaying the same records twice produces the same logical digest") && passed;
    passed = expect(first.value().logical_digest == live,
                    "replay reproduces the logical digest of the live ledger") && passed;
    return passed;
}

// --- 10: snapshot isolation -------------------------------------------------

bool example_10_snapshot_isolation() {
    std::printf("\n[10] snapshot isolation\n");
    std::shared_ptr<Ledger> ledger = open_ledger();
    if (ledger == nullptr) {
        return false;
    }
    if (!ok(ledger->append(session_opened(1, "isolation study", "does a snapshot freeze?")),
            "append(SessionOpened)")) {
        return false;
    }
    const LedgerSnapshot old_snapshot = ledger->snapshot();
    if (!ok(ledger->append(hypothesis_declared(1, 1, "committed after the snapshot was taken")),
            "append(HypothesisDeclared)")) {
        return false;
    }
    auto hidden = old_snapshot.hypothesis(HypothesisId::from_value(1));
    auto visible = ledger->snapshot().hypothesis(HypothesisId::from_value(1));
    auto session = old_snapshot.session(ResearchSessionId::from_value(1));
    if (!ok(visible, "snapshot.hypothesis") || !ok(session, "snapshot.session")) {
        return false;
    }
    std::printf("  observed: old_watermark=%s old_hypothesis=%s new_hypothesis=%s\n",
                number_text(old_snapshot.watermark().value()).c_str(),
                std::string(error_code_name(hidden.code())).c_str(),
                std::string(hypothesis_status_name(visible.value().status)).c_str());
    bool passed = true;
    passed = expect(!hidden.ok() && hidden.code() == ErrorCode::NotFound,
                    "the old snapshot does not observe the later record") && passed;
    passed = expect(session.ok(), "the old snapshot still observes what it was taken over") && passed;
    return passed;
}

}  // namespace
}  // namespace research_ledger

int main() {
    using Example = bool (*)();
    struct Entry {
        const char* name;
        Example function;
    };
    const Entry examples[] = {
        {"basic research session", research_ledger::example_01_basic_session},
        {"hypothesis to experiment to result lineage", research_ledger::example_02_result_lineage},
        {"failed branch and successful branch", research_ledger::example_03_two_branches},
        {"model and tool call provenance", research_ledger::example_04_provenance},
        {"artifact ancestry", research_ledger::example_05_artifacts},
        {"acceptance decision with cited evidence", research_ledger::example_06_acceptance},
        {"accounting aggregation", research_ledger::example_07_accounting},
        {"persistence round trip", research_ledger::example_08_persistence},
        {"deterministic replay", research_ledger::example_09_replay},
        {"snapshot isolation", research_ledger::example_10_snapshot_isolation},
    };
    const int example_count = static_cast<int>(sizeof(examples) / sizeof(examples[0]));
    std::printf("research-ledger-examples: %d runnable examples over the public API\n", example_count);
    int executed = 0;
    int failed = 0;
    for (const Entry& entry : examples) {
        std::printf("\n--- %s\n", entry.name);
        if (!entry.function()) {
            ++failed;
        }
        ++executed;
    }
    std::printf("\n%d example(s) executed, %d failure(s)\n", executed, failed);
    return failed == 0 ? 0 : 1;
}
