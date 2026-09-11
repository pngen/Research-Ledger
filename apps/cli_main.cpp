#include "cli_support.hpp"

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "research_ledger/evidence.hpp"
#include "research_ledger/ledger.hpp"
#include "research_ledger/persistence.hpp"
#include "research_ledger/replay.hpp"

namespace research_ledger {
namespace cli {
namespace {

// --- shared helpers ---------------------------------------------------------

int report_error(ErrorCode code, const std::string& detail) {
    std::fprintf(stderr, "ERROR %s: %s\n", std::string(error_code_name(code)).c_str(),
                 detail.c_str());
    return kExitFailure;
}

int report_usage_error(const std::string& problem) {
    std::fprintf(stderr, "%s\n\n%s", problem.c_str(), usage_text());
    return kExitUsage;
}

std::string number_text(std::uint64_t value) { return std::to_string(value); }

std::string name_text(std::string_view text) { return std::string(text); }

RecordDraft draft_of(RecordBody body, Provenance provenance) {
    RecordDraft draft;
    draft.body = std::move(body);
    draft.provenance = provenance;
    draft.record_id = LedgerRecordId{};
    draft.idempotent = false;
    return draft;
}

// A draft together with the subject text the command already knows, so the
// report of a committed record never has to re-derive what it refers to.
struct PendingRecord {
    RecordDraft draft{};
    std::string subject{};
};

std::optional<std::uint64_t> identity_argument(const std::vector<std::string>& operands,
                                               std::size_t index, std::string_view domain) {
    const std::optional<std::uint64_t> parsed = parse_identity_argument(operands[index], domain);
    if (!parsed.has_value()) {
        report_error(ErrorCode::InvalidIdentity, "argument " + std::to_string(index + 1) +
                                                     " is not a " + std::string(domain) +
                                                     " identity: " + operands[index]);
    }
    return parsed;
}

bool open_state(const std::string& state_path, std::shared_ptr<Ledger>& ledger) {
    // The default configuration is used deliberately: Ledger::load restores the
    // committed ledger generation and coordinator epoch from the snapshot, so a
    // reloaded ledger appends exactly like a freshly created one.
    LedgerConfig config;
    auto loaded = Ledger::load(state_path, config);
    if (!loaded.ok()) {
        report_error(loaded.code(), loaded.message());
        return false;
    }
    ledger = loaded.take();
    return true;
}

int commit_records(const std::shared_ptr<Ledger>& ledger, const std::string& state_path,
                   const std::vector<PendingRecord>& pending) {
    std::vector<RecordDraft> drafts;
    drafts.reserve(pending.size());
    for (const PendingRecord& record : pending) {
        drafts.push_back(record.draft);
    }
    auto outcomes = ledger->append_batch(drafts);
    if (!outcomes.ok()) {
        return report_error(outcomes.code(), outcomes.message());
    }
    if (outcomes.value().size() != pending.size()) {
        return report_error(ErrorCode::InternalError,
                            "the ledger reported a different number of outcomes than drafts");
    }
    for (std::size_t index = 0; index < pending.size(); ++index) {
        const AppendOutcome& outcome = outcomes.value()[index];
        std::printf("OK sequence=%s type=%s subject=%s state=%s provenance=%s\n",
                    number_text(outcome.sequence.value()).c_str(),
                    name_text(record_type_name(record_type_of(pending[index].draft.body))).c_str(),
                    pending[index].subject.c_str(), name_text(commit_state_name(outcome.state)).c_str(),
                    name_text(provenance_name(pending[index].draft.provenance)).c_str());
    }
    const Status saved = ledger->save(state_path);
    if (!saved.ok()) {
        return report_error(saved.code, saved.message);
    }
    std::printf("SAVED state=%s records=%s\n", state_path.c_str(),
                number_text(ledger->last_sequence().value()).c_str());
    return kExitSuccess;
}

// --- scope kinds shared by decisions and accounting -------------------------

std::string_view scope_domain(std::string_view kind) noexcept {
    return kind == "session" ? std::string_view("research-session") : kind;
}

bool known_scope_kind(std::string_view kind) noexcept {
    return kind == "session" || kind == "hypothesis" || kind == "experiment" || kind == "branch" ||
           kind == "attempt" || kind == "model-call" || kind == "tool-call" || kind == "artifact" ||
           kind == "observation" || kind == "failure" || kind == "decision" || kind == "result";
}

std::optional<SubjectId> scope_subject(std::string_view kind, std::uint64_t value) {
    if (kind == "session") {
        return SubjectId::of(ResearchSessionId::from_value(value));
    }
    if (kind == "hypothesis") {
        return SubjectId::of(HypothesisId::from_value(value));
    }
    if (kind == "experiment") {
        return SubjectId::of(ExperimentId::from_value(value));
    }
    if (kind == "branch") {
        return SubjectId::of(BranchId::from_value(value));
    }
    if (kind == "attempt") {
        return SubjectId::of(AttemptId::from_value(value));
    }
    if (kind == "model-call") {
        return SubjectId::of(ModelCallId::from_value(value));
    }
    if (kind == "tool-call") {
        return SubjectId::of(ToolCallId::from_value(value));
    }
    if (kind == "artifact") {
        return SubjectId::of(ArtifactId::from_value(value));
    }
    if (kind == "observation") {
        return SubjectId::of(ObservationId::from_value(value));
    }
    if (kind == "failure") {
        return SubjectId::of(FailureId::from_value(value));
    }
    if (kind == "decision") {
        return SubjectId::of(DecisionId::from_value(value));
    }
    if (kind == "result") {
        return SubjectId::of(ResultId::from_value(value));
    }
    return std::nullopt;
}

// Resolves the "<type> <id>" pair shared by the decisions and accounting
// commands.
std::optional<SubjectId> scope_argument(const std::vector<std::string>& operands,
                                        std::string& problem) {
    if (!known_scope_kind(operands[0])) {
        problem = "unknown subject type: " + operands[0] +
                  " (session, hypothesis, experiment, branch, attempt, model-call, tool-call, "
                  "artifact, observation, failure, decision, result)";
        return std::nullopt;
    }
    const std::optional<std::uint64_t> value =
        identity_argument(operands, 1, scope_domain(operands[0]));
    if (!value.has_value()) {
        return std::nullopt;
    }
    return scope_subject(operands[0], *value);
}

// --- branch kinds -----------------------------------------------------------

std::optional<BranchKind> parse_branch_kind_token(std::string_view token) {
    if (token == "root") {
        return BranchKind::Root;
    }
    if (token == "fork") {
        return BranchKind::Fork;
    }
    if (token == "retry") {
        return BranchKind::Retry;
    }
    if (token == "alternate-method") {
        return BranchKind::AlternateMethod;
    }
    if (token == "control") {
        return BranchKind::Control;
    }
    if (token == "ablation") {
        return BranchKind::Ablation;
    }
    return std::nullopt;
}

std::string_view branch_kind_token(BranchKind kind) noexcept {
    switch (kind) {
        case BranchKind::Root:
            return "root";
        case BranchKind::Fork:
            return "fork";
        case BranchKind::Continuation:
            return "continuation";
        case BranchKind::Retry:
            return "retry";
        case BranchKind::AlternateMethod:
            return "alternate-method";
        case BranchKind::Control:
            return "control";
        case BranchKind::Ablation:
            return "ablation";
        case BranchKind::CompetingHypothesis:
            return "competing-hypothesis";
        case BranchKind::MergedEvidence:
            return "merged-evidence";
        case BranchKind::Invalid:
            return "invalid";
    }
    return "invalid";
}

// --- mutating commands ------------------------------------------------------

int command_init(const std::string& state_path, const std::vector<std::string>& operands) {
    std::string problem;
    if (!require_operands("init", "no arguments", operands, 0, 0, problem)) {
        return report_usage_error(problem);
    }
    std::error_code error;
    const std::uintmax_t size = std::filesystem::file_size(state_path, error);
    if (!error && size > 0) {
        return report_error(ErrorCode::InvalidArgument,
                            "state file already exists and is not empty: " + state_path +
                                " (remove it to start a new ledger)");
    }
    LedgerConfig config;
    auto created = Ledger::create(config);
    if (!created.ok()) {
        return report_error(created.code(), created.message());
    }
    const Status saved = created.value()->save(state_path);
    if (!saved.ok()) {
        return report_error(saved.code, saved.message);
    }
    std::printf("OK state=%s records=%s generation=%s epoch=%s\n", state_path.c_str(),
                number_text(created.value()->last_sequence().value()).c_str(),
                number_text(created.value()->generation().value()).c_str(),
                number_text(created.value()->epoch().value()).c_str());
    return kExitSuccess;
}

int command_create_session(const std::string& state_path, const std::vector<std::string>& operands) {
    std::string problem;
    if (!require_operands("create-session", "<id> [label] [question]", operands, 1, 3, problem)) {
        return report_usage_error(problem);
    }
    const auto session_value = identity_argument(operands, 0, "research-session");
    if (!session_value.has_value()) {
        return kExitFailure;
    }
    std::shared_ptr<Ledger> ledger;
    if (!open_state(state_path, ledger)) {
        return kExitFailure;
    }
    SessionOpened body;
    body.session = ResearchSessionId::from_value(*session_value);
    if (operands.size() > 1) {
        body.label = operands[1];
    }
    if (operands.size() > 2) {
        body.question = operands[2];
    }
    body.authority = "cli:local-operator";

    std::vector<PendingRecord> pending;
    pending.push_back(PendingRecord{draft_of(body, Provenance::Reported), to_string(body.session)});
    return commit_records(ledger, state_path, pending);
}

int command_create_hypothesis(const std::string& state_path,
                              const std::vector<std::string>& operands) {
    std::string problem;
    if (!require_operands("create-hypothesis", "<session> <id> <claim...>", operands, 3, 64,
                          problem)) {
        return report_usage_error(problem);
    }
    const auto session_value = identity_argument(operands, 0, "research-session");
    if (!session_value.has_value()) {
        return kExitFailure;
    }
    const auto hypothesis_value = identity_argument(operands, 1, "hypothesis");
    if (!hypothesis_value.has_value()) {
        return kExitFailure;
    }
    std::shared_ptr<Ledger> ledger;
    if (!open_state(state_path, ledger)) {
        return kExitFailure;
    }
    HypothesisDeclared body;
    body.hypothesis = HypothesisId::from_value(*hypothesis_value);
    body.generation = HypothesisGeneration::first();
    body.session = ResearchSessionId::from_value(*session_value);
    body.claim = join_operands(operands, 2);
    body.authority = "cli:local-operator";

    std::vector<PendingRecord> pending;
    pending.push_back(
        PendingRecord{draft_of(body, Provenance::Reported), to_string(body.hypothesis)});
    return commit_records(ledger, state_path, pending);
}

int command_create_branch(const std::string& state_path, const std::vector<std::string>& operands) {
    std::string problem;
    if (!require_operands("create-branch", "<session> <id> [kind] [parent]", operands, 2, 4,
                          problem)) {
        return report_usage_error(problem);
    }
    const auto session_value = identity_argument(operands, 0, "research-session");
    if (!session_value.has_value()) {
        return kExitFailure;
    }
    const auto branch_value = identity_argument(operands, 1, "branch");
    if (!branch_value.has_value()) {
        return kExitFailure;
    }
    BranchKind kind = BranchKind::Root;
    if (operands.size() > 2) {
        const std::optional<BranchKind> parsed = parse_branch_kind_token(operands[2]);
        if (!parsed.has_value()) {
            return report_error(ErrorCode::InvalidArgument,
                                "branch kind must be root, fork, retry, alternate-method, control "
                                "or ablation: " +
                                    operands[2]);
        }
        kind = *parsed;
    }
    std::shared_ptr<Ledger> ledger;
    if (!open_state(state_path, ledger)) {
        return kExitFailure;
    }
    BranchDeclared body;
    body.branch = BranchId::from_value(*branch_value);
    body.session = ResearchSessionId::from_value(*session_value);
    body.kind = kind;
    body.label = std::string(branch_kind_token(kind));
    body.authority = "cli:local-operator";
    if (operands.size() > 3) {
        const auto parent_value = identity_argument(operands, 3, "branch");
        if (!parent_value.has_value()) {
            return kExitFailure;
        }
        body.parent_branch = BranchId::from_value(*parent_value);
    }

    std::vector<PendingRecord> pending;
    pending.push_back(PendingRecord{draft_of(body, Provenance::Reported), to_string(body.branch)});
    return commit_records(ledger, state_path, pending);
}

int command_create_experiment(const std::string& state_path,
                              const std::vector<std::string>& operands) {
    std::string problem;
    if (!require_operands("create-experiment", "<session> <id> <hypothesis> <branch>", operands, 4,
                          4, problem)) {
        return report_usage_error(problem);
    }
    const auto session_value = identity_argument(operands, 0, "research-session");
    if (!session_value.has_value()) {
        return kExitFailure;
    }
    const auto experiment_value = identity_argument(operands, 1, "experiment");
    if (!experiment_value.has_value()) {
        return kExitFailure;
    }
    const auto hypothesis_value = identity_argument(operands, 2, "hypothesis");
    if (!hypothesis_value.has_value()) {
        return kExitFailure;
    }
    const auto branch_value = identity_argument(operands, 3, "branch");
    if (!branch_value.has_value()) {
        return kExitFailure;
    }
    std::shared_ptr<Ledger> ledger;
    if (!open_state(state_path, ledger)) {
        return kExitFailure;
    }
    ExperimentDeclared body;
    body.experiment = ExperimentId::from_value(*experiment_value);
    body.generation = ExperimentGeneration::first();
    body.session = ResearchSessionId::from_value(*session_value);
    body.hypotheses.push_back(HypothesisId::from_value(*hypothesis_value));
    body.branch = BranchId::from_value(*branch_value);
    body.environment_reference = "unspecified";
    body.authority = "cli:local-operator";

    std::vector<PendingRecord> pending;
    pending.push_back(
        PendingRecord{draft_of(body, Provenance::Reported), to_string(body.experiment)});
    return commit_records(ledger, state_path, pending);
}

int command_create_attempt(const std::string& state_path, const std::vector<std::string>& operands) {
    std::string problem;
    if (!require_operands("create-attempt", "<experiment> <id> <branch>", operands, 3, 3, problem)) {
        return report_usage_error(problem);
    }
    const auto experiment_value = identity_argument(operands, 0, "experiment");
    if (!experiment_value.has_value()) {
        return kExitFailure;
    }
    const auto attempt_value = identity_argument(operands, 1, "attempt");
    if (!attempt_value.has_value()) {
        return kExitFailure;
    }
    const auto branch_value = identity_argument(operands, 2, "branch");
    if (!branch_value.has_value()) {
        return kExitFailure;
    }
    std::shared_ptr<Ledger> ledger;
    if (!open_state(state_path, ledger)) {
        return kExitFailure;
    }
    AttemptStarted body;
    body.attempt = AttemptId::from_value(*attempt_value);
    body.generation = AttemptGeneration::first();
    body.experiment = ExperimentId::from_value(*experiment_value);
    body.branch = BranchId::from_value(*branch_value);
    body.worker_authority = "cli:local-operator";

    std::vector<PendingRecord> pending;
    pending.push_back(PendingRecord{draft_of(body, Provenance::Reported), to_string(body.attempt)});
    return commit_records(ledger, state_path, pending);
}

int command_complete_attempt(const std::string& state_path,
                             const std::vector<std::string>& operands) {
    std::string problem;
    if (!require_operands("complete-attempt", "<attempt> [reference]", operands, 1, 2, problem)) {
        return report_usage_error(problem);
    }
    const auto attempt_value = identity_argument(operands, 0, "attempt");
    if (!attempt_value.has_value()) {
        return kExitFailure;
    }
    std::shared_ptr<Ledger> ledger;
    if (!open_state(state_path, ledger)) {
        return kExitFailure;
    }
    LedgerSnapshot snapshot = ledger->snapshot();
    auto attempt = snapshot.attempt(AttemptId::from_value(*attempt_value));
    if (!attempt.ok()) {
        return report_error(attempt.code(), attempt.message());
    }
    AttemptCompleted body;
    body.attempt = attempt.value().attempt;
    body.generation = attempt.value().generation;
    if (operands.size() > 1) {
        body.outcome_reference = operands[1];
    }

    std::vector<PendingRecord> pending;
    pending.push_back(PendingRecord{draft_of(body, Provenance::Reported), to_string(body.attempt)});
    return commit_records(ledger, state_path, pending);
}

int command_fail_attempt(const std::string& state_path, const std::vector<std::string>& operands) {
    std::string problem;
    if (!require_operands("fail-attempt", "<attempt> <failure-id> <message...>", operands, 3, 64,
                          problem)) {
        return report_usage_error(problem);
    }
    const auto attempt_value = identity_argument(operands, 0, "attempt");
    if (!attempt_value.has_value()) {
        return kExitFailure;
    }
    const auto failure_value = identity_argument(operands, 1, "failure");
    if (!failure_value.has_value()) {
        return kExitFailure;
    }
    std::shared_ptr<Ledger> ledger;
    if (!open_state(state_path, ledger)) {
        return kExitFailure;
    }
    LedgerSnapshot snapshot = ledger->snapshot();
    auto attempt = snapshot.attempt(AttemptId::from_value(*attempt_value));
    if (!attempt.ok()) {
        return report_error(attempt.code(), attempt.message());
    }

    // Failure evidence and the attempt's own terminal record commit together:
    // failure evidence alone never terminates an attempt.
    FailureRecorded failure;
    failure.failure = FailureId::from_value(*failure_value);
    failure.scope = SubjectId::of(attempt.value().attempt);
    failure.category = FailureCategory::Execution;
    failure.message = join_operands(operands, 2);
    failure.retriable = true;
    failure.terminal = true;
    failure.authority = "cli:local-operator";

    AttemptFailed terminal;
    terminal.attempt = attempt.value().attempt;
    terminal.generation = attempt.value().generation;
    terminal.failure = failure.failure;

    std::vector<PendingRecord> pending;
    pending.push_back(PendingRecord{draft_of(failure, Provenance::Reported), to_string(failure.failure)});
    pending.push_back(PendingRecord{draft_of(terminal, Provenance::Reported), to_string(terminal.attempt)});
    return commit_records(ledger, state_path, pending);
}

int command_create_result(const std::string& state_path, const std::vector<std::string>& operands) {
    std::string problem;
    if (!require_operands("create-result", "<session> <id> <hypothesis> <experiment> [artifact-id]...",
                          operands, 4, 68, problem)) {
        return report_usage_error(problem);
    }
    const auto session_value = identity_argument(operands, 0, "research-session");
    if (!session_value.has_value()) {
        return kExitFailure;
    }
    const auto result_value = identity_argument(operands, 1, "result");
    if (!result_value.has_value()) {
        return kExitFailure;
    }
    const auto hypothesis_value = identity_argument(operands, 2, "hypothesis");
    if (!hypothesis_value.has_value()) {
        return kExitFailure;
    }
    const auto experiment_value = identity_argument(operands, 3, "experiment");
    if (!experiment_value.has_value()) {
        return kExitFailure;
    }
    std::vector<ArtifactId> artifacts;
    for (std::size_t index = 4; index < operands.size(); ++index) {
        const auto artifact_value = identity_argument(operands, index, "artifact");
        if (!artifact_value.has_value()) {
            return kExitFailure;
        }
        artifacts.push_back(ArtifactId::from_value(*artifact_value));
    }
    std::shared_ptr<Ledger> ledger;
    if (!open_state(state_path, ledger)) {
        return kExitFailure;
    }
    ResultDeclared body;
    body.result = ResultId::from_value(*result_value);
    body.generation = ResultGeneration::first();
    body.session = ResearchSessionId::from_value(*session_value);
    body.summary = "result " + number_text(body.result.value()) + " over experiment " +
                   number_text(*experiment_value);
    body.hypotheses.push_back(HypothesisId::from_value(*hypothesis_value));
    body.experiments.push_back(ExperimentId::from_value(*experiment_value));
    body.artifacts = std::move(artifacts);
    // The CLI states a content digest for what it declares, and says so: the
    // digest is derived from the declaration, not measured from a payload.
    body.content_digest = sha256("result " + number_text(body.result.value()) + " session " +
                                 number_text(*session_value) + " experiment " +
                                 number_text(*experiment_value));
    body.authority = "cli:local-operator";

    std::vector<PendingRecord> pending;
    pending.push_back(PendingRecord{draft_of(body, Provenance::Derived), to_string(body.result)});
    return commit_records(ledger, state_path, pending);
}

int command_decide(const std::string& state_path, const std::vector<std::string>& operands,
                   bool accept) {
    std::string problem;
    const std::string_view command = accept ? std::string_view("accept") : std::string_view("reject");
    if (!require_operands(command, "<result> <decision-id>", operands, 2, 2, problem)) {
        return report_usage_error(problem);
    }
    const auto result_value = identity_argument(operands, 0, "result");
    if (!result_value.has_value()) {
        return kExitFailure;
    }
    const auto decision_value = identity_argument(operands, 1, "decision");
    if (!decision_value.has_value()) {
        return kExitFailure;
    }
    std::shared_ptr<Ledger> ledger;
    if (!open_state(state_path, ledger)) {
        return kExitFailure;
    }
    LedgerSnapshot snapshot = ledger->snapshot();
    auto result = snapshot.result(ResultId::from_value(*result_value));
    if (!result.ok()) {
        return report_error(result.code(), result.message());
    }
    const ResultView& view = result.value();

    // Evidence is cited by (subject, sequence) for records that are already
    // committed, so a decision can never cite something it did not precede.
    std::vector<EvidenceRef> evidence;
    const auto cite = [&snapshot, &evidence](const SubjectId& subject) -> Status {
        auto sequence = snapshot.subject_sequence(subject);
        if (!sequence.ok()) {
            return sequence.status();
        }
        evidence.push_back(EvidenceRef{subject, sequence.value()});
        return Status{};
    };
    for (const ExperimentId experiment : view.experiments) {
        const Status status = cite(SubjectId::of(experiment));
        if (!status.ok()) {
            return report_error(status.code, status.message);
        }
    }
    for (const ArtifactId artifact : view.artifacts) {
        const Status status = cite(SubjectId::of(artifact));
        if (!status.ok()) {
            return report_error(status.code, status.message);
        }
    }
    for (const HypothesisId hypothesis : view.hypotheses) {
        const Status status = cite(SubjectId::of(hypothesis));
        if (!status.ok()) {
            return report_error(status.code, status.message);
        }
    }
    if (evidence.size() > ledger->limits().max_evidence_refs_per_decision) {
        return report_error(ErrorCode::LimitExceeded,
                            "the result names more evidence than a decision may cite");
    }

    DecisionRecorded decision;
    decision.decision = DecisionId::from_value(*decision_value);
    decision.session = view.session;
    decision.subject = SubjectId::of(view.result);
    decision.type = accept ? DecisionType::Accept : DecisionType::Reject;
    decision.outcome = accept ? DecisionOutcome::Accepted : DecisionOutcome::Rejected;
    decision.policy_generation = PolicyGeneration::first();
    decision.evidence = evidence;
    decision.explanation = std::string(accept ? "accept" : "reject") +
                           " decision recorded by research-ledger-cli citing " +
                           number_text(evidence.size()) + " committed evidence record(s)";
    decision.authority = "cli:local-reviewer";

    ResultStatusChanged change;
    change.result = view.result;
    change.generation = view.generation.next();
    change.from = view.status;
    change.to = accept ? ResultStatus::Accepted : ResultStatus::Rejected;
    change.decision = decision.decision;

    // The decision and the status change it authorizes commit as one batch, so
    // an accepted result always has the decision that accepted it.
    std::vector<PendingRecord> pending;
    pending.push_back(PendingRecord{draft_of(decision, Provenance::Reported), to_string(decision.decision)});
    pending.push_back(PendingRecord{draft_of(change, Provenance::Reported), to_string(change.result)});
    return commit_records(ledger, state_path, pending);
}

// --- inspection commands ----------------------------------------------------

int command_inspect_session(const std::string& state_path, const std::vector<std::string>& operands) {
    std::string problem;
    if (!require_operands("inspect-session", "<id>", operands, 1, 1, problem)) {
        return report_usage_error(problem);
    }
    const auto session_value = identity_argument(operands, 0, "research-session");
    if (!session_value.has_value()) {
        return kExitFailure;
    }
    std::shared_ptr<Ledger> ledger;
    if (!open_state(state_path, ledger)) {
        return kExitFailure;
    }
    auto view = ledger->snapshot().session(ResearchSessionId::from_value(*session_value));
    if (!view.ok()) {
        return report_error(view.code(), view.message());
    }
    const SessionView& session = view.value();
    std::printf("SESSION %s state=%s label=%s question=%s\n", to_string(session.session).c_str(),
                name_text(session_state_name(session.state)).c_str(), unstated(session.label).c_str(),
                unstated(session.question).c_str());
    std::printf("  hypotheses=%s experiments=%s results=%s opened_at=%s closed_at=%s provenance=%s authority=%s\n",
                number_text(session.hypothesis_count).c_str(),
                number_text(session.experiment_count).c_str(),
                number_text(session.result_count).c_str(),
                number_text(session.opened_at.value()).c_str(),
                number_text(session.closed_at.value()).c_str(),
                name_text(provenance_name(session.provenance)).c_str(),
                unstated(session.authority).c_str());
    for (const MetadataEntry& entry : session.metadata) {
        std::printf("  metadata %s=%s\n", entry.key.c_str(), entry.value.c_str());
    }
    return kExitSuccess;
}

int command_inspect_hypothesis(const std::string& state_path,
                               const std::vector<std::string>& operands) {
    std::string problem;
    if (!require_operands("inspect-hypothesis", "<id>", operands, 1, 1, problem)) {
        return report_usage_error(problem);
    }
    const auto hypothesis_value = identity_argument(operands, 0, "hypothesis");
    if (!hypothesis_value.has_value()) {
        return kExitFailure;
    }
    std::shared_ptr<Ledger> ledger;
    if (!open_state(state_path, ledger)) {
        return kExitFailure;
    }
    auto view = ledger->snapshot().hypothesis(HypothesisId::from_value(*hypothesis_value));
    if (!view.ok()) {
        return report_error(view.code(), view.message());
    }
    const HypothesisView& hypothesis = view.value();
    std::printf("HYPOTHESIS %s generation=%s status=%s session=%s\n",
                to_string(hypothesis.hypothesis).c_str(),
                number_text(hypothesis.generation.value()).c_str(),
                name_text(hypothesis_status_name(hypothesis.status)).c_str(),
                to_string(hypothesis.session).c_str());
    std::printf("  claim=%s parent=%s experiments=%s declared_at=%s last_changed_at=%s provenance=%s authority=%s\n",
                unstated(hypothesis.claim).c_str(),
                optional_identity_text(hypothesis.parent).c_str(),
                identity_list_text(hypothesis.experiments).c_str(),
                number_text(hypothesis.declared_at.value()).c_str(),
                number_text(hypothesis.last_changed_at.value()).c_str(),
                name_text(provenance_name(hypothesis.provenance)).c_str(),
                unstated(hypothesis.authority).c_str());
    return kExitSuccess;
}

int command_inspect_experiment(const std::string& state_path,
                               const std::vector<std::string>& operands) {
    std::string problem;
    if (!require_operands("inspect-experiment", "<id>", operands, 1, 1, problem)) {
        return report_usage_error(problem);
    }
    const auto experiment_value = identity_argument(operands, 0, "experiment");
    if (!experiment_value.has_value()) {
        return kExitFailure;
    }
    std::shared_ptr<Ledger> ledger;
    if (!open_state(state_path, ledger)) {
        return kExitFailure;
    }
    auto view = ledger->snapshot().experiment(ExperimentId::from_value(*experiment_value));
    if (!view.ok()) {
        return report_error(view.code(), view.message());
    }
    const ExperimentView& experiment = view.value();
    std::printf("EXPERIMENT %s generation=%s session=%s branch=%s\n",
                to_string(experiment.experiment).c_str(),
                number_text(experiment.generation.value()).c_str(),
                to_string(experiment.session).c_str(), to_string(experiment.branch).c_str());
    std::printf("  hypotheses=%s attempts=%s parent=%s environment=%s declared_at=%s provenance=%s authority=%s\n",
                identity_list_text(experiment.hypotheses).c_str(),
                identity_list_text(experiment.attempts).c_str(),
                optional_identity_text(experiment.parent_experiment).c_str(),
                unstated(experiment.environment_reference).c_str(),
                number_text(experiment.declared_at.value()).c_str(),
                name_text(provenance_name(experiment.provenance)).c_str(),
                unstated(experiment.authority).c_str());
    return kExitSuccess;
}

int command_inspect_attempt(const std::string& state_path, const std::vector<std::string>& operands) {
    std::string problem;
    if (!require_operands("inspect-attempt", "<id>", operands, 1, 1, problem)) {
        return report_usage_error(problem);
    }
    const auto attempt_value = identity_argument(operands, 0, "attempt");
    if (!attempt_value.has_value()) {
        return kExitFailure;
    }
    std::shared_ptr<Ledger> ledger;
    if (!open_state(state_path, ledger)) {
        return kExitFailure;
    }
    auto view = ledger->snapshot().attempt(AttemptId::from_value(*attempt_value));
    if (!view.ok()) {
        return report_error(view.code(), view.message());
    }
    const AttemptView& attempt = view.value();
    std::printf("ATTEMPT %s generation=%s state=%s experiment=%s branch=%s\n",
                to_string(attempt.attempt).c_str(), number_text(attempt.generation.value()).c_str(),
                name_text(attempt_state_name(attempt.state)).c_str(),
                to_string(attempt.experiment).c_str(), to_string(attempt.branch).c_str());
    std::printf("  failure=%s model_calls=%s tool_calls=%s observations=%s started_at=%s terminated_at=%s provenance=%s worker_authority=%s\n",
                optional_identity_text(attempt.failure).c_str(),
                number_text(attempt.model_call_count).c_str(),
                number_text(attempt.tool_call_count).c_str(),
                number_text(attempt.observation_count).c_str(),
                number_text(attempt.started_at.value()).c_str(),
                number_text(attempt.terminated_at.value()).c_str(),
                name_text(provenance_name(attempt.provenance)).c_str(),
                unstated(attempt.worker_authority).c_str());
    return kExitSuccess;
}

int command_inspect_result(const std::string& state_path, const std::vector<std::string>& operands) {
    std::string problem;
    if (!require_operands("inspect-result", "<id>", operands, 1, 1, problem)) {
        return report_usage_error(problem);
    }
    const auto result_value = identity_argument(operands, 0, "result");
    if (!result_value.has_value()) {
        return kExitFailure;
    }
    std::shared_ptr<Ledger> ledger;
    if (!open_state(state_path, ledger)) {
        return kExitFailure;
    }
    auto view = ledger->snapshot().result(ResultId::from_value(*result_value));
    if (!view.ok()) {
        return report_error(view.code(), view.message());
    }
    const ResultView& result = view.value();
    std::printf("RESULT %s generation=%s status=%s session=%s\n", to_string(result.result).c_str(),
                number_text(result.generation.value()).c_str(),
                name_text(result_status_name(result.status)).c_str(),
                to_string(result.session).c_str());
    std::printf("  hypotheses=%s experiments=%s artifacts=%s observations=%s model_calls=%s tool_calls=%s\n",
                identity_list_text(result.hypotheses).c_str(),
                identity_list_text(result.experiments).c_str(),
                identity_list_text(result.artifacts).c_str(),
                identity_list_text(result.observations).c_str(),
                identity_list_text(result.model_calls).c_str(),
                identity_list_text(result.tool_calls).c_str());
    std::printf("  acceptance_decision=%s last_decision=%s declared_at=%s last_changed_at=%s digest=%s provenance=%s authority=%s\n",
                optional_identity_text(result.acceptance_decision).c_str(),
                optional_identity_text(result.last_decision).c_str(),
                number_text(result.declared_at.value()).c_str(),
                number_text(result.last_changed_at.value()).c_str(),
                result.content_digest.has_value() ? to_hex(*result.content_digest).c_str() : "(none)",
                name_text(provenance_name(result.provenance)).c_str(),
                unstated(result.authority).c_str());
    std::printf("  summary=%s\n", unstated(result.summary).c_str());
    return kExitSuccess;
}

int command_lineage(const std::string& state_path, const std::vector<std::string>& operands) {
    std::string problem;
    if (!require_operands("lineage", "<hypothesis|branch|artifact> <id>", operands, 2, 2, problem)) {
        return report_usage_error(problem);
    }
    const std::string& kind = operands[0];
    if (kind != "hypothesis" && kind != "branch" && kind != "artifact") {
        return report_error(ErrorCode::InvalidArgument,
                            "lineage type must be hypothesis, branch or artifact: " + kind);
    }
    const auto value = identity_argument(operands, 1, kind);
    if (!value.has_value()) {
        return kExitFailure;
    }
    std::shared_ptr<Ledger> ledger;
    if (!open_state(state_path, ledger)) {
        return kExitFailure;
    }
    LedgerSnapshot snapshot = ledger->snapshot();

    if (kind == "hypothesis") {
        auto ancestry = snapshot.hypothesis_ancestry(HypothesisId::from_value(*value));
        if (!ancestry.ok()) {
            return report_error(ancestry.code(), ancestry.message());
        }
        std::printf("LINEAGE hypothesis %s nodes=%s\n",
                    to_string(HypothesisId::from_value(*value)).c_str(),
                    number_text(ancestry.value().size()).c_str());
        for (std::size_t index = 0; index < ancestry.value().size(); ++index) {
            std::printf("  %s %s\n", number_text(index + 1).c_str(),
                        to_string(ancestry.value()[index]).c_str());
        }
        return kExitSuccess;
    }
    if (kind == "branch") {
        auto ancestry = snapshot.branch_ancestry(BranchId::from_value(*value));
        if (!ancestry.ok()) {
            return report_error(ancestry.code(), ancestry.message());
        }
        std::printf("LINEAGE branch %s nodes=%s\n", to_string(BranchId::from_value(*value)).c_str(),
                    number_text(ancestry.value().size()).c_str());
        for (std::size_t index = 0; index < ancestry.value().size(); ++index) {
            std::printf("  %s %s\n", number_text(index + 1).c_str(),
                        to_string(ancestry.value()[index]).c_str());
        }
        return kExitSuccess;
    }
    auto ancestry = snapshot.artifact_ancestry(ArtifactId::from_value(*value));
    if (!ancestry.ok()) {
        return report_error(ancestry.code(), ancestry.message());
    }
    // Artifact ancestry deliberately excludes the artifact itself.
    std::printf("LINEAGE artifact %s ancestors=%s\n",
                to_string(ArtifactId::from_value(*value)).c_str(),
                number_text(ancestry.value().size()).c_str());
    for (std::size_t index = 0; index < ancestry.value().size(); ++index) {
        const ArtifactView& artifact = ancestry.value()[index];
        std::printf("  %s %s role=%s validation=%s digest=%s producer=%s\n",
                    number_text(index + 1).c_str(), to_string(artifact.artifact).c_str(),
                    name_text(artifact_role_name(artifact.role)).c_str(),
                    name_text(validation_state_name(artifact.validation)).c_str(),
                    to_hex(artifact.content_digest).c_str(),
                    name_text(artifact.producer.to_string()).c_str());
    }
    return kExitSuccess;
}

int command_evidence(const std::string& state_path, const std::vector<std::string>& operands) {
    std::string problem;
    if (!require_operands("evidence", "<result>", operands, 1, 1, problem)) {
        return report_usage_error(problem);
    }
    const auto result_value = identity_argument(operands, 0, "result");
    if (!result_value.has_value()) {
        return kExitFailure;
    }
    std::shared_ptr<Ledger> ledger;
    if (!open_state(state_path, ledger)) {
        return kExitFailure;
    }
    auto bundle = ledger->snapshot().supporting_evidence(ResultId::from_value(*result_value));
    if (!bundle.ok()) {
        return report_error(bundle.code(), bundle.message());
    }
    const EvidenceBundle& evidence = bundle.value();
    std::printf("EVIDENCE %s status=%s accepted=%s\n", to_string(evidence.result.result).c_str(),
                name_text(result_status_name(evidence.result.status)).c_str(),
                boolean_text(evidence.accepted()));
    std::printf("  complete=%s reconstructable=%s truncated=%s nodes_visited=%s highest_sequence=%s\n",
                boolean_text(evidence.complete), boolean_text(evidence.reconstructable),
                boolean_text(evidence.truncated), number_text(evidence.nodes_visited).c_str(),
                number_text(evidence.highest_sequence.value()).c_str());
    std::printf("  lineage_digest=%s\n", to_hex(evidence.lineage_digest).c_str());
    std::printf("  hypotheses=%s experiments=%s attempts=%s model_calls=%s tool_calls=%s artifacts=%s observations=%s failures=%s decisions=%s cited=%s\n",
                number_text(evidence.hypotheses.size()).c_str(),
                number_text(evidence.experiments.size()).c_str(),
                number_text(evidence.attempts.size()).c_str(),
                number_text(evidence.model_calls.size()).c_str(),
                number_text(evidence.tool_calls.size()).c_str(),
                number_text(evidence.artifacts.size()).c_str(),
                number_text(evidence.observations.size()).c_str(),
                number_text(evidence.failures.size()).c_str(),
                number_text(evidence.decisions.size()).c_str(),
                number_text(evidence.cited_evidence.size()).c_str());
    for (const EvidenceRef& reference : evidence.cited_evidence) {
        std::printf("  cited %s at sequence %s\n", reference.subject.to_string().c_str(),
                    number_text(reference.sequence.value()).c_str());
    }
    return kExitSuccess;
}

int command_explain(const std::string& state_path, const std::vector<std::string>& operands) {
    std::string problem;
    if (!require_operands("explain", "<result>", operands, 1, 1, problem)) {
        return report_usage_error(problem);
    }
    const auto result_value = identity_argument(operands, 0, "result");
    if (!result_value.has_value()) {
        return kExitFailure;
    }
    std::shared_ptr<Ledger> ledger;
    if (!open_state(state_path, ledger)) {
        return kExitFailure;
    }
    auto explanation = ledger->snapshot().explain_result(ResultId::from_value(*result_value));
    if (!explanation.ok()) {
        return report_error(explanation.code(), explanation.message());
    }
    const std::string text = explanation.value().to_text();
    std::fputs(text.c_str(), stdout);
    return kExitSuccess;
}

int command_failures(const std::string& state_path, const std::vector<std::string>& operands) {
    std::string problem;
    if (!require_operands("failures", "no arguments", operands, 0, 0, problem)) {
        return report_usage_error(problem);
    }
    std::shared_ptr<Ledger> ledger;
    if (!open_state(state_path, ledger)) {
        return kExitFailure;
    }
    auto failures = ledger->snapshot().failures();
    if (!failures.ok()) {
        return report_error(failures.code(), failures.message());
    }
    std::printf("FAILURES count=%s\n", number_text(failures.value().size()).c_str());
    for (const FailureView& failure : failures.value()) {
        std::printf("  %s scope=%s category=%s retriable=%s terminal=%s recorded_at=%s provenance=%s message=%s\n",
                    to_string(failure.failure).c_str(), failure.scope.to_string().c_str(),
                    name_text(failure_category_name(failure.category)).c_str(),
                    boolean_text(failure.retriable), boolean_text(failure.terminal),
                    number_text(failure.recorded_at.value()).c_str(),
                    name_text(provenance_name(failure.provenance)).c_str(),
                    unstated(failure.message).c_str());
    }
    return kExitSuccess;
}

int command_decisions(const std::string& state_path, const std::vector<std::string>& operands) {
    std::string problem;
    if (!require_operands("decisions", "<type> <id>", operands, 2, 2, problem)) {
        return report_usage_error(problem);
    }
    const std::optional<SubjectId> subject = scope_argument(operands, problem);
    if (!subject.has_value()) {
        if (!problem.empty()) {
            return report_usage_error(problem);
        }
        return kExitFailure;
    }
    std::shared_ptr<Ledger> ledger;
    if (!open_state(state_path, ledger)) {
        return kExitFailure;
    }
    auto decisions = ledger->snapshot().decisions_of(*subject);
    if (!decisions.ok()) {
        return report_error(decisions.code(), decisions.message());
    }
    std::printf("DECISIONS subject=%s count=%s\n", subject->to_string().c_str(),
                number_text(decisions.value().size()).c_str());
    for (const DecisionView& decision : decisions.value()) {
        std::printf("  %s type=%s outcome=%s recorded_at=%s evidence=%s policy=%s provenance=%s authority=%s\n",
                    to_string(decision.decision).c_str(),
                    name_text(decision_type_name(decision.type)).c_str(),
                    name_text(decision_outcome_name(decision.outcome)).c_str(),
                    number_text(decision.recorded_at.value()).c_str(),
                    number_text(decision.evidence.size()).c_str(),
                    unstated(decision.policy_identity).c_str(),
                    name_text(provenance_name(decision.provenance)).c_str(),
                    unstated(decision.authority).c_str());
        std::printf("    explanation=%s\n", unstated(decision.explanation).c_str());
        for (const EvidenceRef& reference : decision.evidence) {
            std::printf("    EVIDENCE %s at sequence %s\n", reference.subject.to_string().c_str(),
                        number_text(reference.sequence.value()).c_str());
        }
    }
    return kExitSuccess;
}

int command_accounting(const std::string& state_path, const std::vector<std::string>& operands) {
    std::string problem;
    if (!require_operands("accounting", "<type> <id>", operands, 2, 2, problem)) {
        return report_usage_error(problem);
    }
    const std::optional<SubjectId> subject = scope_argument(operands, problem);
    if (!subject.has_value()) {
        if (!problem.empty()) {
            return report_usage_error(problem);
        }
        return kExitFailure;
    }
    std::shared_ptr<Ledger> ledger;
    if (!open_state(state_path, ledger)) {
        return kExitFailure;
    }
    auto aggregate = ledger->snapshot().accounting(*subject);
    if (!aggregate.ok()) {
        return report_error(aggregate.code(), aggregate.message());
    }
    const AccountingAggregate& accounting = aggregate.value();
    std::printf("ACCOUNTING scope=%s contributions=%s contributions_with_unknown_fields=%s complete=%s empty=%s\n",
                subject->to_string().c_str(), number_text(accounting.contributions).c_str(),
                number_text(accounting.contributions_with_unknown_fields).c_str(),
                boolean_text(accounting.complete), boolean_text(accounting.empty));
    std::printf("  model_input_tokens=%s\n", measure_text(accounting.total.model_input_tokens).c_str());
    std::printf("  model_output_tokens=%s\n", measure_text(accounting.total.model_output_tokens).c_str());
    std::printf("  model_calls=%s\n", measure_text(accounting.total.model_calls).c_str());
    std::printf("  tool_calls=%s\n", measure_text(accounting.total.tool_calls).c_str());
    std::printf("  accelerator_nanos=%s\n", measure_text(accounting.total.accelerator_nanos).c_str());
    std::printf("  cpu_nanos=%s\n", measure_text(accounting.total.cpu_nanos).c_str());
    std::printf("  wall_nanos=%s\n", measure_text(accounting.total.wall_nanos).c_str());
    std::printf("  storage_bytes=%s\n", measure_text(accounting.total.storage_bytes).c_str());
    std::printf("  transfer_bytes=%s\n", measure_text(accounting.total.transfer_bytes).c_str());
    std::printf("  energy_micro_joules=%s\n", measure_text(accounting.total.energy_micro_joules).c_str());
    std::printf("  attempts=%s\n", measure_text(accounting.total.attempts).c_str());
    std::printf("  retries=%s\n", measure_text(accounting.total.retries).c_str());
    std::printf("  failure_overhead_nanos=%s\n", measure_text(accounting.total.failure_overhead_nanos).c_str());
    std::printf("  monetary=%s\n", monetary_text(accounting.total.monetary).c_str());
    return kExitSuccess;
}

int command_verify(const std::string& state_path, const std::vector<std::string>& operands) {
    std::string problem;
    if (!require_operands("verify", "no arguments", operands, 0, 0, problem)) {
        return report_usage_error(problem);
    }
    std::shared_ptr<Ledger> ledger;
    if (!open_state(state_path, ledger)) {
        return kExitFailure;
    }
    auto report = ledger->snapshot().verify();
    if (!report.ok()) {
        return report_error(report.code(), report.message());
    }
    const std::string text = report.value().to_text();
    std::fputs(text.c_str(), stdout);
    std::fputc('\n', stdout);
    if (!report.value().ok) {
        return report_error(ErrorCode::IntegrityFailure, "committed history failed verification");
    }
    return kExitSuccess;
}

int command_replay(const std::string& state_path, const std::vector<std::string>& operands) {
    std::string problem;
    if (!require_operands("replay", "no arguments", operands, 0, 0, problem)) {
        return report_usage_error(problem);
    }
    Limits limits;
    auto snapshot = load_snapshot_file(state_path, limits);
    if (!snapshot.ok()) {
        return report_error(snapshot.code(), snapshot.message());
    }
    LedgerConfig config;
    config.generation = snapshot.value().generation;
    config.epoch = snapshot.value().epoch;
    const std::span<const Record> records(snapshot.value().records);

    auto first = replay_committed(records, config);
    if (!first.ok()) {
        return report_error(first.code(), first.message());
    }
    auto second = replay_committed(records, config);
    if (!second.ok()) {
        return report_error(second.code(), second.message());
    }
    std::shared_ptr<Ledger> ledger;
    if (!open_state(state_path, ledger)) {
        return kExitFailure;
    }
    const Digest live = ledger->logical_digest();
    const bool deterministic = first.value().logical_digest == second.value().logical_digest;
    const bool matches_live = first.value().logical_digest == live;

    std::printf("REPLAY records=%s last_sequence=%s integrity=%s checked=%s\n",
                number_text(first.value().records).c_str(),
                number_text(first.value().last_sequence.value()).c_str(),
                boolean_text(first.value().integrity.ok),
                number_text(first.value().integrity.checked_records).c_str());
    std::printf("  chain_digest=%s\n", to_hex(first.value().chain_digest).c_str());
    std::printf("  logical_digest=%s\n", to_hex(first.value().logical_digest).c_str());
    std::printf("  replay_repeat_digest=%s deterministic=%s\n",
                to_hex(second.value().logical_digest).c_str(), boolean_text(deterministic));
    std::printf("  live_digest=%s live_matches_replay=%s\n", to_hex(live).c_str(),
                boolean_text(matches_live));
    if (!deterministic || !matches_live || !first.value().integrity.ok) {
        return report_error(ErrorCode::IntegrityFailure,
                            "replay did not reproduce the committed logical state");
    }
    return kExitSuccess;
}

int command_digest(const std::string& state_path, const std::vector<std::string>& operands) {
    std::string problem;
    if (!require_operands("digest", "no arguments", operands, 0, 0, problem)) {
        return report_usage_error(problem);
    }
    std::shared_ptr<Ledger> ledger;
    if (!open_state(state_path, ledger)) {
        return kExitFailure;
    }
    auto stats = ledger->snapshot().stats();
    if (!stats.ok()) {
        return report_error(stats.code(), stats.message());
    }
    const Digest logical = ledger->logical_digest();
    std::printf("DIGEST logical=%s chain=%s records=%s\n", to_hex(logical).c_str(),
                to_hex(stats.value().chain_digest).c_str(),
                number_text(stats.value().last_sequence.value()).c_str());
    return kExitSuccess;
}

int command_stats(const std::string& state_path, const std::vector<std::string>& operands) {
    std::string problem;
    if (!require_operands("stats", "no arguments", operands, 0, 0, problem)) {
        return report_usage_error(problem);
    }
    std::shared_ptr<Ledger> ledger;
    if (!open_state(state_path, ledger)) {
        return kExitFailure;
    }
    auto stats = ledger->snapshot().stats();
    if (!stats.ok()) {
        return report_error(stats.code(), stats.message());
    }
    const LedgerStats& value = stats.value();
    std::printf("STATS records=%s last_sequence=%s generation=%s epoch=%s chain_digest=%s\n",
                number_text(value.last_sequence.value()).c_str(),
                number_text(value.last_sequence.value()).c_str(),
                number_text(value.generation.value()).c_str(),
                number_text(value.epoch.value()).c_str(), to_hex(value.chain_digest).c_str());
    std::printf("  sessions=%s hypotheses=%s experiments=%s branches=%s attempts=%s model_calls=%s tool_calls=%s artifacts=%s observations=%s failures=%s decisions=%s results=%s accounting_records=%s live_workers=%s\n",
                number_text(value.sessions).c_str(), number_text(value.hypotheses).c_str(),
                number_text(value.experiments).c_str(), number_text(value.branches).c_str(),
                number_text(value.attempts).c_str(), number_text(value.model_calls).c_str(),
                number_text(value.tool_calls).c_str(), number_text(value.artifacts).c_str(),
                number_text(value.observations).c_str(), number_text(value.failures).c_str(),
                number_text(value.decisions).c_str(), number_text(value.results).c_str(),
                number_text(value.accounting_records).c_str(),
                number_text(value.live_workers).c_str());
    for (std::uint16_t raw = 1; raw <= kRecordTypeCount; ++raw) {
        const RecordType type = static_cast<RecordType>(raw);
        std::printf("  %s=%s\n", name_text(record_type_name(type)).c_str(),
                    number_text(value.count_of(type)).c_str());
    }
    return kExitSuccess;
}

// --- dispatch ---------------------------------------------------------------

int run_command(const Invocation& invocation) {
    const std::string& state_path = invocation.state_path;
    const std::string& command = invocation.command;
    const std::vector<std::string>& operands = invocation.operands;

    if (command == "init") {
        return command_init(state_path, operands);
    }
    if (command == "create-session") {
        return command_create_session(state_path, operands);
    }
    if (command == "create-hypothesis") {
        return command_create_hypothesis(state_path, operands);
    }
    if (command == "create-branch") {
        return command_create_branch(state_path, operands);
    }
    if (command == "create-experiment") {
        return command_create_experiment(state_path, operands);
    }
    if (command == "create-attempt") {
        return command_create_attempt(state_path, operands);
    }
    if (command == "complete-attempt") {
        return command_complete_attempt(state_path, operands);
    }
    if (command == "fail-attempt") {
        return command_fail_attempt(state_path, operands);
    }
    if (command == "create-result") {
        return command_create_result(state_path, operands);
    }
    if (command == "accept") {
        return command_decide(state_path, operands, true);
    }
    if (command == "reject") {
        return command_decide(state_path, operands, false);
    }
    if (command == "inspect-session") {
        return command_inspect_session(state_path, operands);
    }
    if (command == "inspect-hypothesis") {
        return command_inspect_hypothesis(state_path, operands);
    }
    if (command == "inspect-experiment") {
        return command_inspect_experiment(state_path, operands);
    }
    if (command == "inspect-attempt") {
        return command_inspect_attempt(state_path, operands);
    }
    if (command == "inspect-result") {
        return command_inspect_result(state_path, operands);
    }
    if (command == "lineage") {
        return command_lineage(state_path, operands);
    }
    if (command == "evidence") {
        return command_evidence(state_path, operands);
    }
    if (command == "explain") {
        return command_explain(state_path, operands);
    }
    if (command == "failures") {
        return command_failures(state_path, operands);
    }
    if (command == "decisions") {
        return command_decisions(state_path, operands);
    }
    if (command == "accounting") {
        return command_accounting(state_path, operands);
    }
    if (command == "verify") {
        return command_verify(state_path, operands);
    }
    if (command == "replay") {
        return command_replay(state_path, operands);
    }
    if (command == "digest") {
        return command_digest(state_path, operands);
    }
    if (command == "stats") {
        return command_stats(state_path, operands);
    }
    return report_usage_error("unknown command: " + command);
}

}  // namespace
}  // namespace cli
}  // namespace research_ledger

int main(int argc, char** argv) {
    research_ledger::cli::Invocation invocation;
    std::string problem;
    if (!research_ledger::cli::parse_invocation(argc, argv, invocation, problem)) {
        if (argc <= 1) {
            problem = "no arguments were given";
        }
        return research_ledger::cli::report_usage_error(problem);
    }
    if (invocation.help) {
        std::fputs(research_ledger::cli::usage_text(), stdout);
        return research_ledger::cli::kExitSuccess;
    }
    return research_ledger::cli::run_command(invocation);
}
