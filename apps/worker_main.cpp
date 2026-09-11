// apps/worker_main.cpp
//
// Reference worker process for the multiprocess proof.
//
// A worker owns no authoritative state. Every record it produces is submitted
// to the coordinator, which validates it against committed history and either
// commits the whole batch or rejects it without touching committed state. The
// worker therefore knows exactly two things: the identities it names itself,
// and the append outcomes the coordinator returned.
//
//   research-ledger-worker --port N [--host H] [--worker ID] [--previous-boot V]
//                          [--authority TEXT]
//
// Command stream: one command per line on standard input, exactly one response
// line per command on standard output, flushed. A failed command prints
// "ERR <ERROR_CODE_NAME> <detail>" and the worker keeps running. Standard input
// and standard output are the pipes research_ledger::ChildProcess creates, so a
// proof that starts this program as a child process drives it with
// ChildProcess::write_line and reads its answers with ChildProcess::read_line.
//
//   create-session <session> [label...]   -> OK session=<id>
//   scenario-main <session> <base>        -> OK main hypothesis=.. branch=..
//                                            experiment=.. attempt=.. model_call=..
//                                            tool_call=.. artifact=.. observation=..
//                                            result=..
//   scenario-failure <session> <base>     -> OK failure hypothesis=.. branch=..
//                                            experiment=.. attempt=.. failure=..
//   scenario-retry <session> <base>       -> OK retry branch=.. experiment=..
//                                            attempt=.. artifact=.. result=..
//   accept <result> <decision>            -> OK accepted result=.. decision=..
//   status                                -> OK last=<seq> chain=<hex> epoch=<n> boot=<v>
//   exit                                  -> OK bye, then exit 0
//   raw-status <epoch> <boot>             -> adversarial frame with an explicit
//                                            authority claim; the coordinator's
//                                            rejection is reported as ERR
//
// Documented identity scheme of the proof history (the same scheme is used by
// apps/multiprocess_proof_main.cpp, which names every identity it asks for):
//
//   create-session 1                      Session 1
//   scenario-main    1 1                  Hypothesis 1, Branch 1 (Root),
//                                         Experiment 1, Attempt 1, ModelCall 1,
//                                         ToolCall 1, Artifact 1 (Intermediate),
//                                         Metric 1, Observation 1, Artifact 2
//                                         (ResultArtifact, parent 1), Result 1
//   scenario-failure 1 1                  Hypothesis 2, Branch 2 (Fork of 1),
//                                         Experiment 2, Attempt 2, Failure 1
//   scenario-retry   1 1                  Branch 3 (Retry of 2), Experiment 3,
//                                         Attempt 3, ModelCall 2, Artifact 3
//                                         (ResultArtifact), Result 2
//   accept 2 1                            Decision 1 over Result 2
//
// Identity domains never share a representation, so the same numeric value is
// used by different domains (Hypothesis 1 and Attempt 1 are different
// entities) and no domain repeats a value.

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "research_ledger/cluster.hpp"
#include "research_ledger/digest.hpp"
#include "research_ledger/error.hpp"
#include "research_ledger/net.hpp"
#include "research_ledger/protocol.hpp"
// net.hpp is required here for NetworkRuntime: the worker opens the client side
// of the ingestion boundary itself, and Winsock must be initialized first.

namespace rl = research_ledger;

namespace {

// --- text helpers -----------------------------------------------------------

bool parse_unsigned(std::string_view text, std::uint64_t& out) {
    if (text.empty()) {
        return false;
    }
    std::uint64_t value = 0;
    const char* const first = text.data();
    const char* const last = text.data() + text.size();
    const std::from_chars_result parsed = std::from_chars(first, last, value);
    if (parsed.ec != std::errc{} || parsed.ptr != last) {
        return false;
    }
    out = value;
    return true;
}

std::vector<std::string> split_tokens(const std::string& line) {
    std::vector<std::string> tokens;
    std::size_t index = 0;
    while (index < line.size()) {
        while (index < line.size() && (line[index] == ' ' || line[index] == '\t')) {
            ++index;
        }
        if (index >= line.size()) {
            break;
        }
        const std::size_t start = index;
        while (index < line.size() && line[index] != ' ' && line[index] != '\t') {
            ++index;
        }
        tokens.push_back(line.substr(start, index - start));
    }
    return tokens;
}

std::string join_from(const std::vector<std::string>& tokens, std::size_t first) {
    std::string text;
    for (std::size_t index = first; index < tokens.size(); ++index) {
        if (!text.empty()) {
            text.push_back(' ');
        }
        text += tokens[index];
    }
    return text;
}

std::string strip_carriage_return(std::string line) {
    if (!line.empty() && line.back() == '\r') {
        line.pop_back();
    }
    return line;
}

std::string worker_authority(std::uint64_t worker) {
    return "worker:" + std::to_string(worker);
}

rl::Digest digest_of(std::string_view text) { return rl::sha256(text); }

rl::RecordDraft draft_of(rl::RecordBody body, rl::Provenance provenance) {
    rl::RecordDraft draft;
    draft.body = std::move(body);
    draft.provenance = provenance;
    return draft;
}

// --- committed layout of the proof history ----------------------------------
//
// Every step of the proof is deterministic, so the committed prefix is
// deterministic too. The table below is the documented layout: the record
// sequence and the primary subject of each committed record, exactly as
// LedgerSnapshot::subject_sequence reports a subject's position.
//
// The reviewing worker cannot ask the coordinator where a subject was
// committed: the ledger belongs to the coordinator process and the worker
// protocol exposes no subject -> sequence query. A citation is an exact
// (subject, sequence) pair, so the worker resolves the pair from the
// documented prefix and refuses to cite anything at all unless the coordinator
// reports exactly that prefix (see kReviewablePrefix). The proof then verifies
// the citation against a real LedgerSnapshot::subject_sequence over the
// reloaded ledger, so a drifted layout fails the proof instead of silently
// citing the wrong record.

struct LayoutEntry {
    std::uint64_t sequence;
    rl::SubjectKind kind;
    std::uint64_t value;
};

constexpr LayoutEntry kProofLayout[] = {
    {1, rl::SubjectKind::Session, 1},       // SessionOpened
    {2, rl::SubjectKind::Hypothesis, 1},    // HypothesisDeclared (main)
    {3, rl::SubjectKind::Branch, 1},        // BranchDeclared (Root)
    {4, rl::SubjectKind::Experiment, 1},    // ExperimentDeclared (main)
    {5, rl::SubjectKind::Attempt, 1},       // AttemptStarted (main)
    {6, rl::SubjectKind::ModelCall, 1},     // ModelCallRecorded (main)
    {7, rl::SubjectKind::ToolCall, 1},      // ToolCallRecorded (main)
    {8, rl::SubjectKind::Artifact, 1},      // ArtifactReferenced (Intermediate)
    {9, rl::SubjectKind::Session, 1},       // MetricDeclared concerns its session
    {10, rl::SubjectKind::Observation, 1},  // ObservationRecorded (main)
    {11, rl::SubjectKind::Artifact, 2},     // ArtifactReferenced (ResultArtifact)
    {12, rl::SubjectKind::Attempt, 1},      // AttemptCompleted (main)
    {13, rl::SubjectKind::Attempt, 1},      // AccountingRecorded (main attempt)
    {14, rl::SubjectKind::Result, 1},       // ResultDeclared (main)
    {15, rl::SubjectKind::Hypothesis, 2},   // HypothesisDeclared (failure)
    {16, rl::SubjectKind::Branch, 2},       // BranchDeclared (Fork)
    {17, rl::SubjectKind::Experiment, 2},   // ExperimentDeclared (failure)
    {18, rl::SubjectKind::Attempt, 2},      // AttemptStarted (failure)
    {19, rl::SubjectKind::Failure, 1},      // FailureRecorded (scope attempt 2)
    {20, rl::SubjectKind::Attempt, 2},      // AttemptFailed (failure)
    {21, rl::SubjectKind::Branch, 3},       // BranchDeclared (Retry)
    {22, rl::SubjectKind::Experiment, 3},   // ExperimentDeclared (retry)
    {23, rl::SubjectKind::Attempt, 3},      // AttemptStarted (retry)
    {24, rl::SubjectKind::ModelCall, 2},    // ModelCallRecorded (retry)
    {25, rl::SubjectKind::Artifact, 3},     // ArtifactReferenced (retry result)
    {26, rl::SubjectKind::Attempt, 3},      // AttemptCompleted (retry)
    {27, rl::SubjectKind::Result, 2},       // ResultDeclared (retry)
};

inline constexpr std::uint64_t kProofHistoryLength = 27;
inline constexpr std::uint64_t kProofSession = 1;

// The committed prefix the acceptance decision is allowed to cite into. The
// reviewing worker refuses to cite a sequence unless the coordinator reports
// this exact prefix and these exact entity counts.
struct PrefixExpectation {
    std::uint64_t last_sequence;
    std::uint64_t sessions;
    std::uint64_t hypotheses;
    std::uint64_t experiments;
    std::uint64_t attempts;
    std::uint64_t results;
    std::uint64_t decisions;
};

constexpr PrefixExpectation kReviewablePrefix{27, 1, 2, 3, 3, 2, 0};

static_assert(kReviewablePrefix.last_sequence == kProofHistoryLength,
              "the reviewable prefix must be exactly the documented proof history");

std::optional<std::uint64_t> latest_sequence(rl::SubjectKind kind, std::uint64_t value) {
    std::optional<std::uint64_t> found;
    for (const LayoutEntry& entry : kProofLayout) {
        if (entry.kind == kind && entry.value == value) {
            found = entry.sequence;
        }
    }
    return found;
}

// --- outcome checking -------------------------------------------------------

rl::Status require_committed(const std::vector<rl::AppendOutcome>& outcomes, std::size_t expected,
                             std::string_view what) {
    if (outcomes.size() != expected) {
        return rl::Status(rl::ErrorCode::InternalError,
                          std::string(what) + ": the coordinator committed " +
                              std::to_string(outcomes.size()) + " of " + std::to_string(expected) +
                              " records");
    }
    for (std::size_t index = 0; index < outcomes.size(); ++index) {
        if (outcomes[index].state != rl::CommitState::Committed) {
            return rl::Status(rl::ErrorCode::InternalError,
                              std::string(what) + ": record " + std::to_string(index) +
                                  " was not committed");
        }
        if (index > 0 &&
            !(outcomes[index].sequence.value() == outcomes[index - 1].sequence.value() + 1)) {
            return rl::Status(rl::ErrorCode::InternalError,
                              std::string(what) + ": committed sequences are not contiguous");
        }
    }
    return rl::Status{};
}

rl::Result<std::string> ok_line(std::string text) { return rl::make_ok(std::move(text)); }

// --- scenarios --------------------------------------------------------------

rl::Result<std::string> create_session(rl::WorkerClient& client, std::uint64_t session_value,
                                       const std::string& label, const std::string& authority) {
    if (session_value == 0) {
        return rl::make_error<std::string>(rl::ErrorCode::InvalidIdentity, "session identity zero is never valid");
    }
    rl::SessionOpened body;
    body.session = rl::ResearchSessionId::from_value(session_value);
    body.label = label.empty() ? std::string("worker session") : label;
    body.question = "does committed history survive worker and coordinator death?";
    body.authority = authority;

    auto outcomes = client.append({draft_of(body, rl::Provenance::Reported)});
    if (!outcomes.ok()) {
        return outcomes.status();
    }
    const rl::Status committed = require_committed(outcomes.value(), 1, "create-session");
    if (!committed.ok()) {
        return committed;
    }
    return ok_line("OK session=" + std::to_string(session_value));
}

rl::Result<std::string> scenario_main(rl::WorkerClient& client, std::uint64_t session_value,
                                      std::uint64_t base, const std::string& authority) {
    if (session_value == 0 || base == 0) {
        return rl::make_error<std::string>(rl::ErrorCode::InvalidIdentity, "session and base identities must be non-zero");
    }
    const rl::ResearchSessionId session = rl::ResearchSessionId::from_value(session_value);
    const rl::HypothesisId hypothesis = rl::HypothesisId::from_value(base);
    const rl::BranchId branch = rl::BranchId::from_value(base);
    const rl::ExperimentId experiment = rl::ExperimentId::from_value(base);
    const rl::AttemptId attempt = rl::AttemptId::from_value(base);
    const rl::ModelCallId model_call = rl::ModelCallId::from_value(base);
    const rl::ToolCallId tool_call = rl::ToolCallId::from_value(base);
    const rl::ArtifactId intermediate = rl::ArtifactId::from_value(base);
    const rl::ArtifactId final_artifact = rl::ArtifactId::from_value(base + 1);
    const rl::MetricId metric = rl::MetricId::from_value(base);
    const rl::ObservationId observation = rl::ObservationId::from_value(base);
    const rl::ResultId result = rl::ResultId::from_value(base);

    auto ratio = rl::MetricValue::ratio(0.91);
    if (!ratio.ok()) {
        return ratio.status();
    }

    std::vector<rl::RecordDraft> setup;

    rl::HypothesisDeclared hypothesis_body;
    hypothesis_body.hypothesis = hypothesis;
    hypothesis_body.generation = rl::HypothesisGeneration::first();
    hypothesis_body.session = session;
    hypothesis_body.claim = "the committed ledger survives worker and coordinator death";
    hypothesis_body.authority = authority;
    setup.push_back(draft_of(hypothesis_body, rl::Provenance::Reported));

    rl::BranchDeclared branch_body;
    branch_body.branch = branch;
    branch_body.session = session;
    branch_body.kind = rl::BranchKind::Root;
    branch_body.label = "root";
    branch_body.authority = authority;
    setup.push_back(draft_of(branch_body, rl::Provenance::Reported));

    rl::ExperimentDeclared experiment_body;
    experiment_body.experiment = experiment;
    experiment_body.generation = rl::ExperimentGeneration::first();
    experiment_body.session = session;
    experiment_body.hypotheses.push_back(hypothesis);
    experiment_body.branch = branch;
    experiment_body.environment_reference = "env:multiprocess-proof";
    experiment_body.authority = authority;
    setup.push_back(draft_of(experiment_body, rl::Provenance::Reported));

    rl::AttemptStarted attempt_body;
    attempt_body.attempt = attempt;
    attempt_body.generation = rl::AttemptGeneration::first();
    attempt_body.experiment = experiment;
    attempt_body.branch = branch;
    attempt_body.worker_authority = authority;
    setup.push_back(draft_of(attempt_body, rl::Provenance::Reported));

    rl::ModelCallRecorded model_body;
    model_body.call = model_call;
    model_body.attempt = attempt;
    model_body.model_identity = "reference-model";
    model_body.model_revision = "r1";
    model_body.provider = "reference-provider";
    model_body.configuration_digest = "cfg-main";
    model_body.input_reference = digest_of("main-input");
    model_body.output_reference = digest_of("main-output");
    model_body.input_tokens = rl::InputTokens::known(1200, rl::Provenance::Measured);
    model_body.output_tokens = rl::OutputTokens::known(340, rl::Provenance::Measured);
    model_body.latency = rl::WallNanos::known(1500000, rl::Provenance::Measured);
    model_body.cost = rl::MonetaryMeasure::known_amount(1200, "USD", rl::Provenance::Reported);
    model_body.outcome = rl::ModelCallOutcome::Succeeded;
    model_body.authority = authority;
    setup.push_back(draft_of(model_body, rl::Provenance::Reported));

    rl::ToolCallRecorded tool_body;
    tool_body.call = tool_call;
    tool_body.attempt = attempt;
    tool_body.tool_identity = "normalizer";
    tool_body.tool_version = "1.2";
    tool_body.request_reference = digest_of("main-tool-request");
    tool_body.output_reference = digest_of("main-tool-output");
    tool_body.state = rl::ToolCallState::Completed;
    tool_body.accounting.tool_calls = rl::ToolCalls::known(1, rl::Provenance::Measured);
    tool_body.accounting.cpu_nanos = rl::CpuNanos::known(5000, rl::Provenance::Measured);
    tool_body.authority = authority;
    setup.push_back(draft_of(tool_body, rl::Provenance::Reported));

    rl::ArtifactReferenced intermediate_body;
    intermediate_body.artifact = intermediate;
    intermediate_body.generation = rl::ArtifactGeneration::first();
    intermediate_body.session = session;
    intermediate_body.content_digest = digest_of("main-intermediate-artifact");
    intermediate_body.role = rl::ArtifactRole::Intermediate;
    intermediate_body.media_type = "application/octet-stream";
    intermediate_body.producer = rl::SubjectId::of(attempt);
    intermediate_body.location = "artifact-fabric://main-intermediate";
    intermediate_body.validation = rl::ValidationState::Validated;
    intermediate_body.authority = authority;
    setup.push_back(draft_of(intermediate_body, rl::Provenance::Reported));

    rl::MetricDeclared metric_body;
    metric_body.metric = metric;
    metric_body.session = session;
    metric_body.canonical_key = "acceptance_ratio";
    metric_body.unit = rl::UnitKind::Ratio;
    metric_body.value_kind = rl::MetricValueKind::Ratio;
    metric_body.description = "fraction of the committed plan that was executed";
    metric_body.authority = authority;
    setup.push_back(draft_of(metric_body, rl::Provenance::Reported));

    rl::ObservationRecorded observation_body;
    observation_body.observation = observation;
    observation_body.attempt = attempt;
    observation_body.metric = metric;
    observation_body.key = "acceptance_ratio";
    observation_body.unit = rl::UnitKind::Ratio;
    observation_body.value = ratio.value();
    observation_body.measured_at = rl::now_timestamp();
    observation_body.source = "worker-suite";
    observation_body.authority = authority;
    setup.push_back(draft_of(observation_body, rl::Provenance::Measured));

    auto setup_outcomes = client.append(setup);
    if (!setup_outcomes.ok()) {
        return setup_outcomes.status();
    }
    const rl::Status setup_committed =
        require_committed(setup_outcomes.value(), setup.size(), "scenario-main setup");
    if (!setup_committed.ok()) {
        return setup_committed;
    }

    std::vector<rl::RecordDraft> completion;

    rl::ArtifactReferenced final_body;
    final_body.artifact = final_artifact;
    final_body.generation = rl::ArtifactGeneration::first();
    final_body.session = session;
    final_body.content_digest = digest_of("main-result-artifact");
    final_body.role = rl::ArtifactRole::ResultArtifact;
    final_body.media_type = "application/octet-stream";
    final_body.producer = rl::SubjectId::of(attempt);
    final_body.location = "artifact-fabric://main-result";
    final_body.parents.push_back(intermediate);
    final_body.validation = rl::ValidationState::Validated;
    final_body.authority = authority;
    completion.push_back(draft_of(final_body, rl::Provenance::Reported));

    rl::AttemptCompleted completed_body;
    completed_body.attempt = attempt;
    completed_body.generation = rl::AttemptGeneration::first();
    completed_body.outcome_reference = "main-outcome";
    completion.push_back(draft_of(completed_body, rl::Provenance::Reported));

    rl::AccountingVector accounting;
    accounting.model_input_tokens = rl::InputTokens::known(1200, rl::Provenance::Measured);
    accounting.model_output_tokens = rl::OutputTokens::known(340, rl::Provenance::Measured);
    accounting.attempts = rl::AttemptCount::known(1, rl::Provenance::Measured);
    accounting.retries = rl::RetryCount::known(0, rl::Provenance::Measured);
    rl::AccountingRecorded accounting_body;
    accounting_body.scope = rl::SubjectId::of(attempt);
    accounting_body.accounting = accounting;
    accounting_body.source = "worker-suite";
    accounting_body.authority = authority;
    completion.push_back(draft_of(accounting_body, rl::Provenance::Measured));

    rl::ResultDeclared result_body;
    result_body.result = result;
    result_body.generation = rl::ResultGeneration::first();
    result_body.session = session;
    result_body.summary = "the main attempt produced a measured result";
    result_body.hypotheses.push_back(hypothesis);
    result_body.experiments.push_back(experiment);
    result_body.artifacts.push_back(final_artifact);
    result_body.observations.push_back(observation);
    result_body.content_digest = digest_of("main-result");
    result_body.authority = authority;
    completion.push_back(draft_of(result_body, rl::Provenance::Derived));

    auto completion_outcomes = client.append(completion);
    if (!completion_outcomes.ok()) {
        return completion_outcomes.status();
    }
    const rl::Status completion_committed =
        require_committed(completion_outcomes.value(), completion.size(), "scenario-main completion");
    if (!completion_committed.ok()) {
        return completion_committed;
    }

    return ok_line("OK main hypothesis=" + std::to_string(base) + " branch=" + std::to_string(base) +
                   " experiment=" + std::to_string(base) + " attempt=" + std::to_string(base) +
                   " model_call=" + std::to_string(base) + " tool_call=" + std::to_string(base) +
                   " artifact=" + std::to_string(base + 1) +
                   " observation=" + std::to_string(base) + " result=" + std::to_string(base));
}

rl::Result<std::string> scenario_failure(rl::WorkerClient& client, std::uint64_t session_value,
                                         std::uint64_t base, const std::string& authority) {
    if (session_value == 0 || base == 0) {
        return rl::make_error<std::string>(rl::ErrorCode::InvalidIdentity, "session and base identities must be non-zero");
    }
    const rl::ResearchSessionId session = rl::ResearchSessionId::from_value(session_value);
    const rl::HypothesisId hypothesis = rl::HypothesisId::from_value(base + 1);
    const rl::BranchId fork = rl::BranchId::from_value(base + 1);
    const rl::ExperimentId experiment = rl::ExperimentId::from_value(base + 1);
    const rl::AttemptId attempt = rl::AttemptId::from_value(base + 1);
    const rl::FailureId failure = rl::FailureId::from_value(base);

    std::vector<rl::RecordDraft> batch;

    rl::HypothesisDeclared hypothesis_body;
    hypothesis_body.hypothesis = hypothesis;
    hypothesis_body.generation = rl::HypothesisGeneration::first();
    hypothesis_body.session = session;
    hypothesis_body.claim = "the failed attempt stays in history";
    hypothesis_body.authority = authority;
    batch.push_back(draft_of(hypothesis_body, rl::Provenance::Reported));

    rl::BranchDeclared branch_body;
    branch_body.branch = fork;
    branch_body.session = session;
    branch_body.kind = rl::BranchKind::Fork;
    branch_body.parent_branch = rl::BranchId::from_value(base);
    branch_body.label = "failure-fork";
    branch_body.authority = authority;
    batch.push_back(draft_of(branch_body, rl::Provenance::Reported));

    rl::ExperimentDeclared experiment_body;
    experiment_body.experiment = experiment;
    experiment_body.generation = rl::ExperimentGeneration::first();
    experiment_body.session = session;
    experiment_body.hypotheses.push_back(hypothesis);
    experiment_body.branch = fork;
    experiment_body.environment_reference = "env:multiprocess-proof-failure";
    experiment_body.authority = authority;
    batch.push_back(draft_of(experiment_body, rl::Provenance::Reported));

    rl::AttemptStarted attempt_body;
    attempt_body.attempt = attempt;
    attempt_body.generation = rl::AttemptGeneration::first();
    attempt_body.experiment = experiment;
    attempt_body.branch = fork;
    attempt_body.worker_authority = authority;
    batch.push_back(draft_of(attempt_body, rl::Provenance::Reported));

    rl::FailureRecorded failure_body;
    failure_body.failure = failure;
    failure_body.scope = rl::SubjectId::of(attempt);
    failure_body.category = rl::FailureCategory::Execution;
    failure_body.message = "worker died before completion";
    failure_body.retriable = true;
    failure_body.terminal = true;
    failure_body.authority = authority;
    batch.push_back(draft_of(failure_body, rl::Provenance::Measured));

    rl::AttemptFailed failed_body;
    failed_body.attempt = attempt;
    failed_body.generation = rl::AttemptGeneration::first();
    failed_body.failure = failure;
    batch.push_back(draft_of(failed_body, rl::Provenance::Measured));

    auto outcomes = client.append(batch);
    if (!outcomes.ok()) {
        return outcomes.status();
    }
    const rl::Status committed = require_committed(outcomes.value(), batch.size(), "scenario-failure");
    if (!committed.ok()) {
        return committed;
    }
    return ok_line("OK failure hypothesis=" + std::to_string(base + 1) +
                   " branch=" + std::to_string(base + 1) +
                   " experiment=" + std::to_string(base + 1) +
                   " attempt=" + std::to_string(base + 1) +
                   " failure=" + std::to_string(base));
}

rl::Result<std::string> scenario_retry(rl::WorkerClient& client, std::uint64_t session_value,
                                       std::uint64_t base, const std::string& authority) {
    if (session_value == 0 || base == 0) {
        return rl::make_error<std::string>(rl::ErrorCode::InvalidIdentity, "session and base identities must be non-zero");
    }
    const rl::ResearchSessionId session = rl::ResearchSessionId::from_value(session_value);
    const rl::BranchId retry = rl::BranchId::from_value(base + 2);
    const rl::ExperimentId experiment = rl::ExperimentId::from_value(base + 2);
    const rl::AttemptId attempt = rl::AttemptId::from_value(base + 2);
    const rl::ModelCallId model_call = rl::ModelCallId::from_value(base + 1);
    const rl::ArtifactId artifact = rl::ArtifactId::from_value(base + 2);
    const rl::ResultId result = rl::ResultId::from_value(base + 1);

    std::vector<rl::RecordDraft> batch;

    rl::BranchDeclared branch_body;
    branch_body.branch = retry;
    branch_body.session = session;
    branch_body.kind = rl::BranchKind::Retry;
    branch_body.parent_branch = rl::BranchId::from_value(base + 1);
    branch_body.label = "retry";
    branch_body.authority = authority;
    batch.push_back(draft_of(branch_body, rl::Provenance::Reported));

    rl::ExperimentDeclared experiment_body;
    experiment_body.experiment = experiment;
    experiment_body.generation = rl::ExperimentGeneration::first();
    experiment_body.session = session;
    experiment_body.hypotheses.push_back(rl::HypothesisId::from_value(base + 1));
    experiment_body.branch = retry;
    experiment_body.parent_experiment = rl::ExperimentId::from_value(base + 1);
    experiment_body.environment_reference = "env:multiprocess-proof-retry";
    experiment_body.authority = authority;
    batch.push_back(draft_of(experiment_body, rl::Provenance::Reported));

    rl::AttemptStarted attempt_body;
    attempt_body.attempt = attempt;
    attempt_body.generation = rl::AttemptGeneration::first();
    attempt_body.experiment = experiment;
    attempt_body.branch = retry;
    attempt_body.worker_authority = authority;
    batch.push_back(draft_of(attempt_body, rl::Provenance::Reported));

    rl::ModelCallRecorded model_body;
    model_body.call = model_call;
    model_body.attempt = attempt;
    model_body.model_identity = "reference-model";
    model_body.model_revision = "r1";
    model_body.provider = "reference-provider";
    model_body.configuration_digest = "cfg-retry";
    model_body.input_reference = digest_of("retry-input");
    model_body.output_reference = digest_of("retry-output");
    model_body.input_tokens = rl::InputTokens::known(900, rl::Provenance::Measured);
    model_body.output_tokens = rl::OutputTokens::known(210, rl::Provenance::Measured);
    model_body.latency = rl::WallNanos::known(1100000, rl::Provenance::Measured);
    model_body.cost = rl::MonetaryMeasure::known_amount(900, "USD", rl::Provenance::Reported);
    model_body.outcome = rl::ModelCallOutcome::Succeeded;
    model_body.authority = authority;
    batch.push_back(draft_of(model_body, rl::Provenance::Reported));

    rl::ArtifactReferenced artifact_body;
    artifact_body.artifact = artifact;
    artifact_body.generation = rl::ArtifactGeneration::first();
    artifact_body.session = session;
    artifact_body.content_digest = digest_of("retry-result-artifact");
    artifact_body.role = rl::ArtifactRole::ResultArtifact;
    artifact_body.media_type = "application/octet-stream";
    artifact_body.producer = rl::SubjectId::of(attempt);
    artifact_body.location = "artifact-fabric://retry-result";
    artifact_body.validation = rl::ValidationState::Validated;
    artifact_body.authority = authority;
    batch.push_back(draft_of(artifact_body, rl::Provenance::Reported));

    rl::AttemptCompleted completed_body;
    completed_body.attempt = attempt;
    completed_body.generation = rl::AttemptGeneration::first();
    completed_body.outcome_reference = "retry-outcome";
    batch.push_back(draft_of(completed_body, rl::Provenance::Reported));

    rl::ResultDeclared result_body;
    result_body.result = result;
    result_body.generation = rl::ResultGeneration::first();
    result_body.session = session;
    result_body.summary = "the retry attempt reproduced the measurement";
    result_body.hypotheses.push_back(rl::HypothesisId::from_value(base + 1));
    result_body.experiments.push_back(experiment);
    result_body.artifacts.push_back(artifact);
    result_body.content_digest = digest_of("retry-result");
    result_body.authority = authority;
    batch.push_back(draft_of(result_body, rl::Provenance::Derived));

    auto outcomes = client.append(batch);
    if (!outcomes.ok()) {
        return outcomes.status();
    }
    const rl::Status committed = require_committed(outcomes.value(), batch.size(), "scenario-retry");
    if (!committed.ok()) {
        return committed;
    }
    return ok_line("OK retry branch=" + std::to_string(base + 2) +
                   " experiment=" + std::to_string(base + 2) +
                   " attempt=" + std::to_string(base + 2) +
                   " artifact=" + std::to_string(base + 2) +
                   " result=" + std::to_string(base + 1));
}

// Acceptance is one atomic batch: the decision and the status change it
// authorizes commit together or not at all.
rl::Result<std::string> accept_result(rl::WorkerClient& client, std::uint64_t result_value,
                                      std::uint64_t decision_value, const std::string& authority) {
    if (result_value == 0 || decision_value == 0) {
        return rl::make_error<std::string>(rl::ErrorCode::InvalidIdentity,
                              "result and decision identities must be non-zero");
    }
    const rl::ResultId result = rl::ResultId::from_value(result_value);
    // Documented scheme: scenario-retry commits Result (base + 1) naming
    // Experiment (base + 2) and Artifact (base + 2).
    const rl::ExperimentId experiment = rl::ExperimentId::from_value(result_value + 1);
    const rl::ArtifactId artifact = rl::ArtifactId::from_value(result_value + 1);

    auto observed = client.status();
    if (!observed.ok()) {
        return observed.status();
    }
    const rl::StatusReplyMessage& prefix = observed.value();
    if (prefix.last_sequence.value() != kReviewablePrefix.last_sequence ||
        prefix.sessions != kReviewablePrefix.sessions ||
        prefix.hypotheses != kReviewablePrefix.hypotheses ||
        prefix.experiments != kReviewablePrefix.experiments ||
        prefix.attempts != kReviewablePrefix.attempts || prefix.results != kReviewablePrefix.results ||
        prefix.decisions != kReviewablePrefix.decisions) {
        return rl::make_error<std::string>(rl::ErrorCode::NotReady,
                              "the committed prefix differs from the documented proof history: last=" +
                                  std::to_string(prefix.last_sequence.value()) + " sessions=" +
                                  std::to_string(prefix.sessions) + " hypotheses=" +
                                  std::to_string(prefix.hypotheses) + " experiments=" +
                                  std::to_string(prefix.experiments) + " attempts=" +
                                  std::to_string(prefix.attempts) + " results=" +
                                  std::to_string(prefix.results) + " decisions=" +
                                  std::to_string(prefix.decisions));
    }

    const std::optional<std::uint64_t> experiment_sequence =
        latest_sequence(rl::SubjectKind::Experiment, experiment.value());
    const std::optional<std::uint64_t> artifact_sequence =
        latest_sequence(rl::SubjectKind::Artifact, artifact.value());
    if (!experiment_sequence.has_value() || !artifact_sequence.has_value()) {
        return rl::make_error<std::string>(rl::ErrorCode::NotFound,
                              "the documented proof history does not contain the evidence for result " +
                                  std::to_string(result_value));
    }

    rl::DecisionRecorded decision_body;
    decision_body.decision = rl::DecisionId::from_value(decision_value);
    decision_body.session = rl::ResearchSessionId::from_value(kProofSession);
    decision_body.subject = rl::SubjectId::of(result);
    decision_body.type = rl::DecisionType::Accept;
    decision_body.outcome = rl::DecisionOutcome::Accepted;
    decision_body.policy_identity = "policy:multiprocess-acceptance";
    decision_body.policy_generation = rl::PolicyGeneration::first();
    rl::EvidenceRef experiment_evidence;
    experiment_evidence.subject = rl::SubjectId::of(experiment);
    experiment_evidence.sequence =
        rl::RecordSequence::from_value(static_cast<std::uint32_t>(experiment_sequence.value()));
    decision_body.evidence.push_back(experiment_evidence);
    rl::EvidenceRef artifact_evidence;
    artifact_evidence.subject = rl::SubjectId::of(artifact);
    artifact_evidence.sequence =
        rl::RecordSequence::from_value(static_cast<std::uint32_t>(artifact_sequence.value()));
    decision_body.evidence.push_back(artifact_evidence);
    decision_body.explanation =
        "the retry reproduced the measurement and the failed attempt remains in history";
    decision_body.authority = authority;

    rl::ResultStatusChanged status_body;
    status_body.result = result;
    status_body.generation = rl::ResultGeneration::from_value(2);
    status_body.from = rl::ResultStatus::Candidate;
    status_body.to = rl::ResultStatus::Accepted;
    status_body.decision = rl::DecisionId::from_value(decision_value);

    std::vector<rl::RecordDraft> batch{draft_of(decision_body, rl::Provenance::Reported),
                                       draft_of(status_body, rl::Provenance::Reported)};
    auto outcomes = client.append(batch);
    if (!outcomes.ok()) {
        return outcomes.status();
    }
    const rl::Status committed = require_committed(outcomes.value(), batch.size(), "accept");
    if (!committed.ok()) {
        return committed;
    }
    if (outcomes.value()[0].sequence.value() != kReviewablePrefix.last_sequence + 1 ||
        outcomes.value()[1].sequence.value() != kReviewablePrefix.last_sequence + 2) {
        return rl::make_error<std::string>(rl::ErrorCode::InternalError,
                              "the acceptance batch did not land at the documented positions");
    }
    return ok_line("OK accepted result=" + std::to_string(result_value) +
                   " decision=" + std::to_string(decision_value));
}

rl::Result<std::string> report_status(rl::WorkerClient& client) {
    auto reply = client.status();
    if (!reply.ok()) {
        return reply.status();
    }
    const rl::StatusReplyMessage& message = reply.value();
    return ok_line("OK last=" + std::to_string(message.last_sequence.value()) + " chain=" +
                   rl::to_hex(message.chain_digest) + " epoch=" + std::to_string(message.epoch.value()) +
                   " boot=" + std::to_string(client.authority().worker_boot.value()));
}

// Adversarial support for the proof: a frame whose authority claim is set
// explicitly instead of being the connection's own admitted incarnation. The
// coordinator is expected to reject it, and the rejection code is what the
// proof checks.
rl::Result<std::string> send_stale_frame(rl::WorkerClient& client, std::uint64_t epoch_value,
                                         std::uint64_t boot_value) {
    if (epoch_value == 0 || epoch_value > 0xffffffffull || boot_value == 0) {
        return rl::make_error<std::string>(rl::ErrorCode::InvalidIdentity, "epoch and boot claims must be valid");
    }
    rl::AuthorityEnvelope claim = client.authority();
    claim.epoch = rl::CoordinatorEpoch::from_value(static_cast<std::uint32_t>(epoch_value));
    claim.worker_boot = rl::WorkerBootId::from_value(boot_value);

    const std::span<const std::byte> no_payload{};
    const rl::Status sent =
        client.send_raw_frame(rl::MessageType::StatusRequest, 1, claim, no_payload);
    if (!sent.ok()) {
        return sent;
    }
    auto frame_bytes = client.receive_raw_frame();
    if (!frame_bytes.ok()) {
        return frame_bytes.status();
    }
    const std::vector<std::byte> storage = frame_bytes.take();
    const rl::Limits limits{};
    auto frame = rl::decode_frame(storage, limits);
    if (!frame.ok()) {
        return frame.status();
    }
    if (frame.value().type == rl::MessageType::ErrorReply) {
        auto message = rl::decode_error(frame.value().payload, limits);
        if (!message.ok()) {
            return message.status();
        }
        return rl::make_error<std::string>(message.value().code, message.value().detail);
    }
    return ok_line(std::string("OK raw=") +
                   std::string(rl::message_type_name(frame.value().type)));
}

// --- options ----------------------------------------------------------------

struct Options {
    std::uint64_t port = 0;
    std::string host = "127.0.0.1";
    std::uint64_t worker = 0;
    std::uint64_t previous_boot = 0;
    std::string authority{};
};

void print_usage() {
    std::cout << "usage: research-ledger-worker --port N [--host H] [--worker ID] "
                 "[--previous-boot V] [--authority TEXT]"
              << std::endl;
}

bool parse_options(int argc, char** argv, Options& options) {
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument(argv[index]);
        if (index + 1 >= argc) {
            return false;
        }
        const std::string_view value(argv[index + 1]);
        if (argument == "--port") {
            std::uint64_t parsed = 0;
            if (!parse_unsigned(value, parsed) || parsed == 0 || parsed > 65535) {
                return false;
            }
            options.port = parsed;
        } else if (argument == "--host") {
            options.host = std::string(value);
        } else if (argument == "--worker") {
            std::uint64_t parsed = 0;
            if (!parse_unsigned(value, parsed) || parsed == 0) {
                return false;
            }
            options.worker = parsed;
        } else if (argument == "--previous-boot") {
            std::uint64_t parsed = 0;
            if (!parse_unsigned(value, parsed)) {
                return false;
            }
            options.previous_boot = parsed;
        } else if (argument == "--authority") {
            options.authority = std::string(value);
        } else {
            return false;
        }
        ++index;
    }
    return options.port != 0 && options.worker != 0;
}

}  // namespace

int main(int argc, char** argv) {
    Options options;
    if (!parse_options(argc, argv, options)) {
        print_usage();
        return 2;
    }

    // Winsock must be initialized before any socket call: the worker opens the
    // client side of the ingestion boundary itself.
    const rl::NetworkRuntime network;

    rl::WorkerConfig config;
    config.host = options.host;
    config.port = static_cast<std::uint16_t>(options.port);
    config.worker = rl::WorkerId::from_value(options.worker);
    config.worker_boot = rl::WorkerBootId::from_value(options.previous_boot);
    config.authority =
        options.authority.empty() ? worker_authority(options.worker) : options.authority;

    auto connected = rl::WorkerClient::connect(config);
    if (!connected.ok()) {
        std::cout << "ERROR " << rl::error_code_name(connected.code()) << " " << connected.message()
                  << std::endl;
        return 1;
    }
    std::unique_ptr<rl::WorkerClient> client = connected.take();

    auto acknowledgement = client->handshake();
    if (!acknowledgement.ok()) {
        std::cout << "ERROR " << rl::error_code_name(acknowledgement.code()) << " "
                  << acknowledgement.message() << std::endl;
        return 1;
    }
    const rl::HelloAckMessage& admitted = acknowledgement.value();
    // The coordinator mints the boot identity of this incarnation; the worker
    // reports what it was admitted as, never what it asked for.
    std::cout << "WORKER READY worker=" << options.worker << " boot=" << admitted.worker_boot.value()
              << " epoch=" << admitted.epoch.value()
              << " generation=" << admitted.generation.value() << std::endl;

    const std::string authority =
        options.authority.empty() ? worker_authority(options.worker) : options.authority;

    std::string line;
    while (std::getline(std::cin, line)) {
        line = strip_carriage_return(std::move(line));
        const std::vector<std::string> tokens = split_tokens(line);
        if (tokens.empty()) {
            continue;
        }
        const std::string& command = tokens[0];
        if (command == "exit") {
            std::cout << "OK bye" << std::endl;
            return 0;
        }

        rl::Result<std::string> response =
            rl::make_error<std::string>(rl::ErrorCode::InvalidArgument, "unknown command " + command);
        std::uint64_t first = 0;
        std::uint64_t second = 0;

        if (command == "create-session") {
            if (tokens.size() < 2 || !parse_unsigned(tokens[1], first)) {
                response = rl::make_error<std::string>(rl::ErrorCode::InvalidArgument,
                                          "create-session requires a session identity");
            } else {
                response = create_session(*client, first, join_from(tokens, 2), authority);
            }
        } else if (command == "scenario-main") {
            if (tokens.size() != 3 || !parse_unsigned(tokens[1], first) ||
                !parse_unsigned(tokens[2], second)) {
                response = rl::make_error<std::string>(rl::ErrorCode::InvalidArgument,
                                          "scenario-main requires <session> <base>");
            } else {
                response = scenario_main(*client, first, second, authority);
            }
        } else if (command == "scenario-failure") {
            if (tokens.size() != 3 || !parse_unsigned(tokens[1], first) ||
                !parse_unsigned(tokens[2], second)) {
                response = rl::make_error<std::string>(rl::ErrorCode::InvalidArgument,
                                          "scenario-failure requires <session> <base>");
            } else {
                response = scenario_failure(*client, first, second, authority);
            }
        } else if (command == "scenario-retry") {
            if (tokens.size() != 3 || !parse_unsigned(tokens[1], first) ||
                !parse_unsigned(tokens[2], second)) {
                response = rl::make_error<std::string>(rl::ErrorCode::InvalidArgument,
                                          "scenario-retry requires <session> <base>");
            } else {
                response = scenario_retry(*client, first, second, authority);
            }
        } else if (command == "accept") {
            if (tokens.size() != 3 || !parse_unsigned(tokens[1], first) ||
                !parse_unsigned(tokens[2], second)) {
                response = rl::make_error<std::string>(rl::ErrorCode::InvalidArgument,
                                          "accept requires <result> <decision>");
            } else {
                response = accept_result(*client, first, second, authority);
            }
        } else if (command == "status") {
            if (tokens.size() != 1) {
                response = rl::make_error<std::string>(rl::ErrorCode::InvalidArgument, "status takes no arguments");
            } else {
                response = report_status(*client);
            }
        } else if (command == "raw-status") {
            if (tokens.size() != 3 || !parse_unsigned(tokens[1], first) ||
                !parse_unsigned(tokens[2], second)) {
                response = rl::make_error<std::string>(rl::ErrorCode::InvalidArgument,
                                          "raw-status requires <epoch> <boot>");
            } else {
                response = send_stale_frame(*client, first, second);
            }
        }

        if (response.ok()) {
            std::cout << response.value() << std::endl;
        } else {
            std::cout << "ERR " << rl::error_code_name(response.code()) << " " << response.message()
                      << std::endl;
        }
    }
    return 0;
}
