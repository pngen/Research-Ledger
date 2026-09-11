// Local synthetic benchmark for the Research Ledger runtime.
//
// This is not a hardware-counter measurement and it is not a distributed
// measurement: one process, one machine, no network. Every measured operation
// runs through the real public API on real committed state, every operation in
// a measured loop is checked for success, and each benchmark warms up before
// its timed loop. Wall-clock numbers from a shared machine are indicative, not
// portable claims.

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <span>
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

// --- harness ----------------------------------------------------------------

int g_failures = 0;

bool bench_fail(const std::string& what) {
    ++g_failures;
    std::printf("BENCH FAILURE: %s\n", what.c_str());
    return false;
}

struct Measurement {
    std::string name{};
    std::string scale{};
    std::uint64_t warmup_operations = 0;
    std::uint64_t operations = 0;
    double seconds = 0.0;
    std::uint64_t checksum = 0;
    std::string note{};
};

void report(const Measurement& measurement) {
    const double per_operation_us =
        measurement.operations == 0
            ? 0.0
            : (measurement.seconds / static_cast<double>(measurement.operations)) * 1000000.0;
    const double per_second = measurement.seconds > 0.0
                                  ? static_cast<double>(measurement.operations) / measurement.seconds
                                  : 0.0;
    std::printf("%-30s %-42s warmup=%-6llu ops=%-8llu total=%9.6f s  per_op=%11.3f us  "
                "%13.1f %s",
                measurement.name.c_str(), measurement.scale.c_str(),
                static_cast<unsigned long long>(measurement.warmup_operations),
                static_cast<unsigned long long>(measurement.operations), measurement.seconds,
                per_operation_us, per_second, measurement.note.c_str());
    std::printf("\n");
    std::fflush(stdout);
}

class Stopwatch {
public:
    void start() noexcept { start_ = std::chrono::steady_clock::now(); }

    [[nodiscard]] double seconds() const noexcept {
        const std::chrono::duration<double> elapsed = std::chrono::steady_clock::now() - start_;
        return elapsed.count();
    }

private:
    std::chrono::steady_clock::time_point start_{};
};

std::uint64_t digest_checksum(const Digest& digest) noexcept {
    std::uint64_t value = 0;
    for (std::size_t index = 0; index < 8; ++index) {
        value = (value << 8) | static_cast<std::uint64_t>(digest.bytes()[index]);
    }
    return value;
}

std::string id_text(std::uint64_t value) { return std::to_string(value); }

std::string status_text(const Status& status) {
    return std::string(error_code_name(status.code)) + ": " + status.message;
}

// --- record builders --------------------------------------------------------

RecordDraft draft_of(RecordBody body, Provenance provenance) {
    RecordDraft draft;
    draft.body = std::move(body);
    draft.provenance = provenance;
    return draft;
}

RecordDraft session_draft(std::uint64_t id) {
    SessionOpened body;
    body.session = ResearchSessionId::from_value(id);
    body.label = "benchmark session";
    body.question = "does the ledger hold under measurement?";
    body.authority = "benchmark";
    return draft_of(body, Provenance::Reported);
}

RecordDraft hypothesis_draft(std::uint64_t id, std::uint64_t session, std::uint64_t parent) {
    HypothesisDeclared body;
    body.hypothesis = HypothesisId::from_value(id);
    body.generation = HypothesisGeneration::first();
    body.session = ResearchSessionId::from_value(session);
    body.claim = "benchmark hypothesis";
    if (parent != 0) {
        body.parent = HypothesisId::from_value(parent);
        body.parent_generation = HypothesisGeneration::first();
    }
    body.authority = "benchmark";
    return draft_of(body, Provenance::Reported);
}

RecordDraft branch_draft(std::uint64_t id, std::uint64_t session, BranchKind kind,
                         std::uint64_t parent) {
    BranchDeclared body;
    body.branch = BranchId::from_value(id);
    body.session = ResearchSessionId::from_value(session);
    body.kind = kind;
    if (parent != 0) {
        body.parent_branch = BranchId::from_value(parent);
    }
    body.label = "benchmark branch";
    body.authority = "benchmark";
    return draft_of(body, Provenance::Reported);
}

RecordDraft experiment_draft(std::uint64_t id, std::uint64_t session, std::uint64_t hypothesis,
                             std::uint64_t branch) {
    ExperimentDeclared body;
    body.experiment = ExperimentId::from_value(id);
    body.generation = ExperimentGeneration::first();
    body.session = ResearchSessionId::from_value(session);
    body.hypotheses.push_back(HypothesisId::from_value(hypothesis));
    body.branch = BranchId::from_value(branch);
    body.environment_reference = "benchmark";
    body.authority = "benchmark";
    return draft_of(body, Provenance::Reported);
}

RecordDraft attempt_draft(std::uint64_t id, std::uint64_t experiment, std::uint64_t branch) {
    AttemptStarted body;
    body.attempt = AttemptId::from_value(id);
    body.generation = AttemptGeneration::first();
    body.experiment = ExperimentId::from_value(experiment);
    body.branch = BranchId::from_value(branch);
    body.worker_authority = "benchmark";
    return draft_of(body, Provenance::Reported);
}

RecordDraft attempt_completed_draft(std::uint64_t id) {
    AttemptCompleted body;
    body.attempt = AttemptId::from_value(id);
    body.generation = AttemptGeneration::first();
    body.outcome_reference = "benchmark outcome";
    return draft_of(body, Provenance::Reported);
}

RecordDraft failure_draft(std::uint64_t id, SubjectId scope, std::string message) {
    FailureRecorded body;
    body.failure = FailureId::from_value(id);
    body.scope = scope;
    body.category = FailureCategory::Execution;
    body.message = std::move(message);
    body.retriable = true;
    body.terminal = true;
    body.authority = "benchmark";
    return draft_of(body, Provenance::Measured);
}

RecordDraft attempt_failed_draft(std::uint64_t id, std::uint64_t failure) {
    AttemptFailed body;
    body.attempt = AttemptId::from_value(id);
    body.generation = AttemptGeneration::first();
    body.failure = FailureId::from_value(failure);
    return draft_of(body, Provenance::Measured);
}

RecordDraft artifact_draft(std::uint64_t id, std::uint64_t session, SubjectId producer,
                           ArtifactRole role, std::string content,
                           std::vector<ArtifactId> parents) {
    ArtifactReferenced body;
    body.artifact = ArtifactId::from_value(id);
    body.generation = ArtifactGeneration::first();
    body.session = ResearchSessionId::from_value(session);
    body.content_digest = sha256(content);
    body.role = role;
    body.media_type = "application/octet-stream";
    body.producer = producer;
    body.location = "artifact-fabric://benchmark";
    body.parents = std::move(parents);
    body.validation = ValidationState::Validated;
    body.authority = "benchmark";
    return draft_of(body, Provenance::Reported);
}

RecordDraft model_call_draft(std::uint64_t id, std::uint64_t attempt) {
    ModelCallRecorded body;
    body.call = ModelCallId::from_value(id);
    body.attempt = AttemptId::from_value(attempt);
    body.model_identity = "benchmark-model";
    body.model_revision = "1";
    body.provider = "benchmark-provider";
    body.configuration_digest = "benchmark-config";
    body.input_reference = sha256("benchmark-input");
    body.output_reference = sha256("benchmark-output");
    body.input_tokens = InputTokens::known(1000, Provenance::Measured);
    body.output_tokens = OutputTokens::known(200, Provenance::Measured);
    body.latency = WallNanos::known(1000000, Provenance::Measured);
    body.cost = MonetaryMeasure::known_amount(100, "USD", Provenance::Reported);
    body.outcome = ModelCallOutcome::Succeeded;
    body.authority = "benchmark";
    return draft_of(body, Provenance::Reported);
}

RecordDraft tool_call_draft(std::uint64_t id, std::uint64_t attempt) {
    ToolCallRecorded body;
    body.call = ToolCallId::from_value(id);
    body.attempt = AttemptId::from_value(attempt);
    body.tool_identity = "benchmark-tool";
    body.tool_version = "1";
    body.request_reference = sha256("benchmark-request");
    body.output_reference = sha256("benchmark-tool-output");
    body.state = ToolCallState::Completed;
    body.accounting.cpu_nanos = CpuNanos::known(1000, Provenance::Measured);
    body.accounting.tool_calls = ToolCalls::known(1, Provenance::Measured);
    body.authority = "benchmark";
    return draft_of(body, Provenance::Reported);
}

RecordDraft observation_draft(std::uint64_t id, std::uint64_t attempt) {
    ObservationRecorded body;
    body.observation = ObservationId::from_value(id);
    body.attempt = AttemptId::from_value(attempt);
    body.key = "benchmark_accuracy";
    body.unit = UnitKind::Ratio;
    body.value = MetricValue::ratio(0.9).value();
    body.measured_at = now_timestamp();
    body.source = "benchmark";
    body.authority = "benchmark";
    return draft_of(body, Provenance::Measured);
}

RecordDraft result_draft(std::uint64_t id, std::uint64_t session, std::uint64_t hypothesis,
                         std::uint64_t experiment, std::vector<ArtifactId> artifacts,
                         std::vector<ObservationId> observations,
                         std::vector<ModelCallId> model_calls,
                         std::vector<ToolCallId> tool_calls) {
    ResultDeclared body;
    body.result = ResultId::from_value(id);
    body.generation = ResultGeneration::first();
    body.session = ResearchSessionId::from_value(session);
    body.summary = "benchmark result";
    body.hypotheses.push_back(HypothesisId::from_value(hypothesis));
    body.experiments.push_back(ExperimentId::from_value(experiment));
    body.artifacts = std::move(artifacts);
    body.observations = std::move(observations);
    body.model_calls = std::move(model_calls);
    body.tool_calls = std::move(tool_calls);
    body.content_digest = sha256("benchmark-result-payload");
    body.authority = "benchmark";
    return draft_of(body, Provenance::Derived);
}

RecordDraft decision_draft(std::uint64_t id, std::uint64_t session, SubjectId subject,
                           DecisionType type, DecisionOutcome outcome,
                           std::vector<EvidenceRef> evidence) {
    DecisionRecorded body;
    body.decision = DecisionId::from_value(id);
    body.session = ResearchSessionId::from_value(session);
    body.subject = subject;
    body.type = type;
    body.outcome = outcome;
    body.policy_identity = "benchmark-policy";
    body.policy_generation = PolicyGeneration::first();
    body.evidence = std::move(evidence);
    body.explanation = "benchmark acceptance";
    body.authority = "benchmark";
    return draft_of(body, Provenance::Reported);
}

RecordDraft status_change_draft(std::uint64_t result, std::uint32_t generation, ResultStatus from,
                                ResultStatus to, std::uint64_t decision) {
    ResultStatusChanged body;
    body.result = ResultId::from_value(result);
    body.generation = ResultGeneration::from_value(generation);
    body.from = from;
    body.to = to;
    body.decision = DecisionId::from_value(decision);
    return draft_of(body, Provenance::Reported);
}

RecordDraft accounting_draft(SubjectId scope, AccountingVector accounting) {
    AccountingRecorded body;
    body.scope = scope;
    body.accounting = std::move(accounting);
    body.source = "benchmark";
    body.authority = "benchmark";
    return draft_of(body, Provenance::Measured);
}

// --- fixture helpers --------------------------------------------------------

bool append_all(const std::shared_ptr<Ledger>& ledger, const std::vector<RecordDraft>& drafts,
                std::size_t batch_size, const std::string& what) {
    const std::span<const RecordDraft> all(drafts);
    for (std::size_t offset = 0; offset < drafts.size(); offset += batch_size) {
        const std::size_t count = std::min(batch_size, drafts.size() - offset);
        auto outcomes = ledger->append_batch(all.subspan(offset, count));
        if (!outcomes.ok()) {
            return bench_fail("appending " + what + " -> " + status_text(outcomes.status()));
        }
    }
    return true;
}

std::shared_ptr<Ledger> new_ledger() {
    auto created = Ledger::create(LedgerConfig{});
    if (!created.ok()) {
        bench_fail("Ledger::create -> " + status_text(created.status()));
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

// A ledger holding tens of thousands of indexed records: one session per
// session record and one hypothesis per session.
std::shared_ptr<Ledger> build_index_fixture(std::uint64_t session_count,
                                            std::uint64_t hypothesis_count) {
    std::shared_ptr<Ledger> ledger = new_ledger();
    if (ledger == nullptr) {
        return nullptr;
    }
    std::vector<RecordDraft> drafts;
    drafts.reserve(static_cast<std::size_t>(session_count + hypothesis_count));
    for (std::uint64_t index = 1; index <= session_count; ++index) {
        drafts.push_back(session_draft(index));
    }
    for (std::uint64_t index = 1; index <= hypothesis_count; ++index) {
        drafts.push_back(hypothesis_draft(index, index, 0));
    }
    if (!append_all(ledger, drafts, 256, "the index fixture")) {
        return nullptr;
    }
    return ledger;
}

// --- append throughput ------------------------------------------------------

bool bench_append_single() {
    const std::uint64_t record_count = 10000;
    std::vector<RecordDraft> drafts;
    drafts.reserve(record_count);
    for (std::uint64_t index = 1; index <= record_count; ++index) {
        drafts.push_back(session_draft(index));
    }

    std::uint64_t warmup = 0;
    {
        std::shared_ptr<Ledger> warm = new_ledger();
        if (warm == nullptr) {
            return false;
        }
        for (std::uint64_t index = 1; index <= 256; ++index) {
            auto outcome = warm->append(session_draft(index));
            if (!outcome.ok()) {
                return bench_fail("warm-up append -> " + status_text(outcome.status()));
            }
            ++warmup;
        }
    }

    std::shared_ptr<Ledger> ledger = new_ledger();
    if (ledger == nullptr) {
        return false;
    }
    std::uint64_t checksum = 0;
    Stopwatch watch;
    watch.start();
    for (const RecordDraft& draft : drafts) {
        auto outcome = ledger->append(draft);
        if (!outcome.ok()) {
            return bench_fail("single append -> " + status_text(outcome.status()));
        }
        checksum += outcome.value().sequence.value();
    }
    const double seconds = watch.seconds();
    if (ledger->last_sequence().value() != record_count) {
        return bench_fail("the single-append ledger did not reach " + id_text(record_count) +
                          " records");
    }
    report(Measurement{"append_single", id_text(record_count) + " records, one append() call each",
                       warmup, record_count, seconds, checksum, "records/s"});
    return true;
}

bool bench_append_batch() {
    const std::uint64_t record_count = 10000;
    const std::size_t batch_size = 256;
    std::vector<RecordDraft> drafts;
    drafts.reserve(record_count);
    for (std::uint64_t index = 1; index <= record_count; ++index) {
        drafts.push_back(session_draft(index));
    }

    std::uint64_t warmup = 0;
    {
        std::shared_ptr<Ledger> warm = new_ledger();
        if (warm == nullptr) {
            return false;
        }
        std::vector<RecordDraft> warm_drafts;
        for (std::uint64_t index = 1; index <= batch_size; ++index) {
            warm_drafts.push_back(session_draft(index));
        }
        auto outcome = warm->append_batch(warm_drafts);
        if (!outcome.ok()) {
            return bench_fail("warm-up batch -> " + status_text(outcome.status()));
        }
        warmup = batch_size;
    }

    std::shared_ptr<Ledger> ledger = new_ledger();
    if (ledger == nullptr) {
        return false;
    }
    const std::span<const RecordDraft> all(drafts);
    std::uint64_t checksum = 0;
    std::uint64_t batches = 0;
    Stopwatch watch;
    watch.start();
    for (std::size_t offset = 0; offset < drafts.size(); offset += batch_size) {
        const std::size_t count = std::min(batch_size, drafts.size() - offset);
        auto outcomes = ledger->append_batch(all.subspan(offset, count));
        if (!outcomes.ok()) {
            return bench_fail("batched append -> " + status_text(outcomes.status()));
        }
        for (const AppendOutcome& outcome : outcomes.value()) {
            checksum += outcome.sequence.value();
        }
        ++batches;
    }
    const double seconds = watch.seconds();
    if (ledger->last_sequence().value() != record_count) {
        return bench_fail("the batched ledger did not reach " + id_text(record_count) + " records");
    }
    report(Measurement{"append_batch",
                       id_text(record_count) + " records in batches of " + id_text(batch_size) +
                           " (" + id_text(batches) + " batches)",
                       warmup, record_count, seconds, checksum, "records/s"});
    return true;
}

// --- indexed point lookup ---------------------------------------------------

bool bench_point_lookup(const std::shared_ptr<Ledger>& ledger, std::uint64_t lookup_count) {
    const LedgerSnapshot snapshot = ledger->snapshot();
    std::uint64_t checksum = 0;
    std::uint64_t warmup = 0;
    for (std::uint64_t index = 1; index <= 128; ++index) {
        auto hypothesis = snapshot.hypothesis(HypothesisId::from_value(index));
        if (!hypothesis.ok()) {
            return bench_fail("warm-up hypothesis lookup -> " + status_text(hypothesis.status()));
        }
        ++warmup;
    }
    Stopwatch watch;
    watch.start();
    for (std::uint64_t index = 1; index <= lookup_count; ++index) {
        auto hypothesis = snapshot.hypothesis(HypothesisId::from_value(index));
        if (!hypothesis.ok() || hypothesis.value().hypothesis.value() != index) {
            return bench_fail("hypothesis lookup missed " + id_text(index));
        }
        auto session = snapshot.session(ResearchSessionId::from_value(index));
        if (!session.ok() || session.value().session.value() != index) {
            return bench_fail("session lookup missed " + id_text(index));
        }
        checksum += hypothesis.value().hypothesis.value() + session.value().session.value();
    }
    const double seconds = watch.seconds();
    report(Measurement{"point_lookup",
                       "2 lookups each over a " +
                           id_text(ledger->last_sequence().value()) + "-record ledger",
                       warmup, lookup_count * 2, seconds, checksum, "lookups/s"});
    return true;
}

// --- ancestry reconstruction ------------------------------------------------

bool bench_hypothesis_ancestry(std::uint64_t depth, std::uint64_t iterations) {
    std::shared_ptr<Ledger> ledger = new_ledger();
    if (ledger == nullptr) {
        return false;
    }
    std::vector<RecordDraft> drafts;
    drafts.reserve(static_cast<std::size_t>(depth) + 2);
    drafts.push_back(session_draft(1));
    for (std::uint64_t index = 1; index <= depth; ++index) {
        drafts.push_back(hypothesis_draft(index, 1, index == 1 ? 0 : index - 1));
    }
    if (!append_all(ledger, drafts, 256, "the hypothesis chain")) {
        return false;
    }
    const LedgerSnapshot snapshot = ledger->snapshot();
    auto warm = snapshot.hypothesis_ancestry(HypothesisId::from_value(depth));
    if (!warm.ok() || warm.value().size() != depth) {
        const std::uint64_t observed =
            warm.ok() ? static_cast<std::uint64_t>(warm.value().size()) : 0;
        return bench_fail("warm-up hypothesis ancestry returned " + id_text(observed) +
                          " nodes, expected " + id_text(depth));
    }
    std::uint64_t checksum = 0;
    Stopwatch watch;
    watch.start();
    for (std::uint64_t iteration = 0; iteration < iterations; ++iteration) {
        auto ancestry = snapshot.hypothesis_ancestry(HypothesisId::from_value(depth));
        if (!ancestry.ok() || ancestry.value().size() != depth) {
            return bench_fail("hypothesis ancestry did not reconstruct the chain");
        }
        checksum += static_cast<std::uint64_t>(ancestry.value().size());
    }
    const double seconds = watch.seconds();
    report(Measurement{"hypothesis_ancestry", "chain depth " + id_text(depth), 1, iterations, seconds,
                       checksum, "reconstructions/s"});
    return true;
}

bool bench_branch_ancestry(std::uint64_t depth, std::uint64_t iterations) {
    std::shared_ptr<Ledger> ledger = new_ledger();
    if (ledger == nullptr) {
        return false;
    }
    std::vector<RecordDraft> drafts;
    drafts.reserve(static_cast<std::size_t>(depth) + 2);
    drafts.push_back(session_draft(1));
    for (std::uint64_t index = 1; index <= depth; ++index) {
        drafts.push_back(branch_draft(index, 1, index == 1 ? BranchKind::Root : BranchKind::Fork,
                                      index == 1 ? 0 : index - 1));
    }
    if (!append_all(ledger, drafts, 256, "the branch chain")) {
        return false;
    }
    const LedgerSnapshot snapshot = ledger->snapshot();
    auto warm = snapshot.branch_ancestry(BranchId::from_value(depth));
    if (!warm.ok() || warm.value().size() != depth) {
        const std::uint64_t observed =
            warm.ok() ? static_cast<std::uint64_t>(warm.value().size()) : 0;
        return bench_fail("warm-up branch ancestry returned " + id_text(observed) +
                          " nodes, expected " + id_text(depth));
    }
    std::uint64_t checksum = 0;
    Stopwatch watch;
    watch.start();
    for (std::uint64_t iteration = 0; iteration < iterations; ++iteration) {
        auto ancestry = snapshot.branch_ancestry(BranchId::from_value(depth));
        if (!ancestry.ok() || ancestry.value().size() != depth) {
            return bench_fail("branch ancestry did not reconstruct the chain");
        }
        checksum += static_cast<std::uint64_t>(ancestry.value().size());
    }
    const double seconds = watch.seconds();
    report(Measurement{"branch_ancestry", "chain depth " + id_text(depth), 1, iterations, seconds,
                       checksum, "reconstructions/s"});
    return true;
}

// --- artifact lineage -------------------------------------------------------

bool bench_artifact_deep_chain(std::uint64_t depth, std::uint64_t iterations) {
    std::shared_ptr<Ledger> ledger = new_ledger();
    if (ledger == nullptr) {
        return false;
    }
    std::vector<RecordDraft> setup{session_draft(1), hypothesis_draft(1, 1, 0),
                                   branch_draft(1, 1, BranchKind::Root, 0),
                                   experiment_draft(1, 1, 1, 1), attempt_draft(1, 1, 1)};
    if (!append_all(ledger, setup, 256, "the artifact chain setup")) {
        return false;
    }
    const SubjectId producer = SubjectId::of(AttemptId::from_value(1));
    std::vector<RecordDraft> chain;
    chain.reserve(static_cast<std::size_t>(depth));
    for (std::uint64_t index = 1; index <= depth; ++index) {
        std::vector<ArtifactId> parents;
        if (index > 1) {
            parents.push_back(ArtifactId::from_value(index - 1));
        }
        chain.push_back(artifact_draft(index, 1, producer,
                                       index == depth ? ArtifactRole::Model
                                                      : ArtifactRole::Intermediate,
                                       "chain-artifact-" + id_text(index), std::move(parents)));
    }
    if (!append_all(ledger, chain, 256, "the artifact chain")) {
        return false;
    }
    const LedgerSnapshot snapshot = ledger->snapshot();
    const std::uint64_t expected = depth - 1;
    auto warm = snapshot.artifact_ancestry(ArtifactId::from_value(depth));
    if (!warm.ok() || warm.value().size() != expected) {
        const std::uint64_t observed =
            warm.ok() ? static_cast<std::uint64_t>(warm.value().size()) : 0;
        return bench_fail("warm-up artifact ancestry returned " + id_text(observed) +
                          " ancestors, expected " + id_text(expected));
    }
    std::uint64_t checksum = 0;
    Stopwatch watch;
    watch.start();
    for (std::uint64_t iteration = 0; iteration < iterations; ++iteration) {
        auto ancestry = snapshot.artifact_ancestry(ArtifactId::from_value(depth));
        if (!ancestry.ok() || ancestry.value().size() != expected) {
            return bench_fail("artifact ancestry did not reconstruct the chain");
        }
        checksum += static_cast<std::uint64_t>(ancestry.value().size());
    }
    const double seconds = watch.seconds();
    report(Measurement{"artifact_ancestry_deep",
                       "chain depth " + id_text(depth) + " (" + id_text(depth - 1) + " ancestors)",
                       1, iterations, seconds, checksum, "traversals/s"});
    return true;
}

bool bench_artifact_wide_fanout(std::uint64_t fanout, std::uint64_t iterations) {
    std::shared_ptr<Ledger> ledger = new_ledger();
    if (ledger == nullptr) {
        return false;
    }
    std::vector<RecordDraft> setup{session_draft(1), hypothesis_draft(1, 1, 0),
                                   branch_draft(1, 1, BranchKind::Root, 0),
                                   experiment_draft(1, 1, 1, 1), attempt_draft(1, 1, 1),
                                   artifact_draft(1, 1, SubjectId::of(AttemptId::from_value(1)),
                                                  ArtifactRole::Dataset, "fanout-root", {})};
    if (!append_all(ledger, setup, 256, "the fan-out setup")) {
        return false;
    }
    const SubjectId producer = SubjectId::of(AttemptId::from_value(1));
    std::vector<RecordDraft> children;
    children.reserve(static_cast<std::size_t>(fanout));
    for (std::uint64_t index = 0; index < fanout; ++index) {
        children.push_back(artifact_draft(2 + index, 1, producer, ArtifactRole::Intermediate,
                                          "fanout-child-" + id_text(index),
                                          {ArtifactId::from_value(1)}));
    }
    if (!append_all(ledger, children, 256, "the fan-out children")) {
        return false;
    }
    const LedgerSnapshot snapshot = ledger->snapshot();
    auto warm = snapshot.artifact_descendants(ArtifactId::from_value(1));
    if (!warm.ok() || warm.value().size() != fanout) {
        const std::uint64_t observed =
            warm.ok() ? static_cast<std::uint64_t>(warm.value().size()) : 0;
        return bench_fail("warm-up artifact descendants returned " + id_text(observed) +
                          " children, expected " + id_text(fanout));
    }
    std::uint64_t checksum = 0;
    Stopwatch watch;
    watch.start();
    for (std::uint64_t iteration = 0; iteration < iterations; ++iteration) {
        auto descendants = snapshot.artifact_descendants(ArtifactId::from_value(1));
        if (!descendants.ok() || descendants.value().size() != fanout) {
            return bench_fail("artifact descendants did not reconstruct the fan-out");
        }
        checksum += static_cast<std::uint64_t>(descendants.value().size());
    }
    const double seconds = watch.seconds();
    report(Measurement{"artifact_descendants_wide", "fan-out " + id_text(fanout), 1, iterations,
                       seconds, checksum, "traversals/s"});
    return true;
}

// --- accepted-result reconstruction -----------------------------------------

// A research history with a failed attempt, a completed retry, a 20-artifact
// chain, model calls, tool calls and an observation, ending in an accepted
// result with an explicit decision.
std::shared_ptr<Ledger> build_result_fixture() {
    std::shared_ptr<Ledger> ledger = new_ledger();
    if (ledger == nullptr) {
        return nullptr;
    }
    std::vector<RecordDraft> setup{
        session_draft(1),
        hypothesis_draft(1, 1, 0),
        branch_draft(1, 1, BranchKind::Root, 0),
        branch_draft(2, 1, BranchKind::Retry, 1),
        experiment_draft(1, 1, 1, 1),
        attempt_draft(1, 1, 1),
        failure_draft(1, SubjectId::of(AttemptId::from_value(1)), "benchmark failure"),
        attempt_failed_draft(1, 1),
        experiment_draft(2, 1, 1, 2),
        attempt_draft(2, 2, 2),
        attempt_completed_draft(2),
    };
    if (!append_all(ledger, setup, 256, "the result fixture setup")) {
        return nullptr;
    }
    const SubjectId producer = SubjectId::of(AttemptId::from_value(2));
    std::vector<RecordDraft> outputs;
    for (std::uint64_t index = 1; index <= 4; ++index) {
        outputs.push_back(model_call_draft(index, 2));
    }
    for (std::uint64_t index = 1; index <= 3; ++index) {
        outputs.push_back(tool_call_draft(index, 2));
    }
    outputs.push_back(observation_draft(1, 2));
    const std::uint64_t artifact_count = 20;
    for (std::uint64_t index = 1; index <= artifact_count; ++index) {
        std::vector<ArtifactId> parents;
        if (index > 1) {
            parents.push_back(ArtifactId::from_value(index - 1));
        }
        outputs.push_back(artifact_draft(index, 1, producer,
                                         index == artifact_count ? ArtifactRole::ResultArtifact
                                                                 : ArtifactRole::Intermediate,
                                         "result-artifact-" + id_text(index), std::move(parents)));
    }
    std::vector<ArtifactId> artifacts{ArtifactId::from_value(artifact_count)};
    std::vector<ObservationId> observations{ObservationId::from_value(1)};
    std::vector<ModelCallId> model_calls{ModelCallId::from_value(1), ModelCallId::from_value(2),
                                         ModelCallId::from_value(3), ModelCallId::from_value(4)};
    std::vector<ToolCallId> tool_calls{ToolCallId::from_value(1), ToolCallId::from_value(2),
                                       ToolCallId::from_value(3)};
    outputs.push_back(result_draft(1, 1, 1, 2, artifacts, observations, model_calls, tool_calls));
    if (!append_all(ledger, outputs, 256, "the result fixture outputs")) {
        return nullptr;
    }
    const LedgerSnapshot snapshot = ledger->snapshot();
    auto experiment_sequence = snapshot.subject_sequence(SubjectId::of(ExperimentId::from_value(2)));
    auto artifact_sequence =
        snapshot.subject_sequence(SubjectId::of(ArtifactId::from_value(artifact_count)));
    if (!experiment_sequence.ok() || !artifact_sequence.ok()) {
        bench_fail("the result fixture could not resolve its evidence sequences");
        return nullptr;
    }
    std::vector<RecordDraft> acceptance{
        decision_draft(1, 1, SubjectId::of(ResultId::from_value(1)), DecisionType::Accept,
                       DecisionOutcome::Accepted,
                       {EvidenceRef{SubjectId::of(ExperimentId::from_value(2)),
                                    experiment_sequence.value()},
                        EvidenceRef{SubjectId::of(ArtifactId::from_value(artifact_count)),
                                    artifact_sequence.value()}}),
        status_change_draft(1, 2, ResultStatus::Candidate, ResultStatus::Accepted, 1),
    };
    if (!append_all(ledger, acceptance, 256, "the result fixture acceptance")) {
        return nullptr;
    }
    return ledger;
}

bool bench_supporting_evidence(const std::shared_ptr<Ledger>& ledger, std::uint64_t iterations) {
    const LedgerSnapshot snapshot = ledger->snapshot();
    auto warm = snapshot.supporting_evidence(ResultId::from_value(1));
    if (!warm.ok() || !warm.value().accepted() || !warm.value().complete ||
        !warm.value().reconstructable) {
        return bench_fail("warm-up supporting_evidence did not reconstruct an accepted result");
    }
    const std::uint64_t nodes = warm.value().nodes_visited;
    std::uint64_t checksum = 0;
    Stopwatch watch;
    watch.start();
    for (std::uint64_t iteration = 0; iteration < iterations; ++iteration) {
        auto bundle = snapshot.supporting_evidence(ResultId::from_value(1));
        if (!bundle.ok() || !bundle.value().accepted() || !bundle.value().complete) {
            return bench_fail("supporting_evidence did not reconstruct the accepted result");
        }
        checksum += bundle.value().nodes_visited;
    }
    const double seconds = watch.seconds();
    report(Measurement{"supporting_evidence",
                       id_text(ledger->last_sequence().value()) + "-record history, closure nodes=" +
                           id_text(nodes),
                       1, iterations, seconds, checksum, "reconstructions/s"});
    return true;
}

bool bench_explain_result(const std::shared_ptr<Ledger>& ledger, std::uint64_t iterations) {
    const LedgerSnapshot snapshot = ledger->snapshot();
    auto warm = snapshot.explain_result(ResultId::from_value(1));
    if (!warm.ok() || warm.value().lines.size() < 10) {
        return bench_fail("warm-up explain_result produced too few lines");
    }
    const std::uint64_t lines = warm.value().lines.size();
    std::uint64_t checksum = 0;
    Stopwatch watch;
    watch.start();
    for (std::uint64_t iteration = 0; iteration < iterations; ++iteration) {
        auto explanation = snapshot.explain_result(ResultId::from_value(1));
        if (!explanation.ok() || explanation.value().lines.size() != lines) {
            return bench_fail("explain_result did not reproduce its line count");
        }
        checksum += explanation.value().lines.size();
    }
    const double seconds = watch.seconds();
    report(Measurement{"explain_result",
                       id_text(ledger->last_sequence().value()) + "-record history, " +
                           id_text(lines) + " explanation lines",
                       1, iterations, seconds, checksum, "explanations/s"});
    return true;
}

// --- accounting aggregation -------------------------------------------------

std::shared_ptr<Ledger> build_accounting_fixture(std::uint64_t experiments,
                                                 std::uint64_t attempts_per_experiment) {
    std::shared_ptr<Ledger> ledger = new_ledger();
    if (ledger == nullptr) {
        return nullptr;
    }
    std::vector<RecordDraft> structure{session_draft(1), hypothesis_draft(1, 1, 0),
                                       branch_draft(1, 1, BranchKind::Root, 0)};
    std::vector<RecordDraft> contributions;
    std::uint64_t attempt_id = 0;
    for (std::uint64_t experiment = 1; experiment <= experiments; ++experiment) {
        structure.push_back(experiment_draft(experiment, 1, 1, 1));
        for (std::uint64_t attempt = 0; attempt < attempts_per_experiment; ++attempt) {
            ++attempt_id;
            structure.push_back(attempt_draft(attempt_id, experiment, 1));
        }
    }
    if (!append_all(ledger, structure, 256, "the accounting fixture structure")) {
        return nullptr;
    }
    attempt_id = 0;
    for (std::uint64_t experiment = 1; experiment <= experiments; ++experiment) {
        for (std::uint64_t attempt = 0; attempt < attempts_per_experiment; ++attempt) {
            ++attempt_id;
            AccountingVector accounting;
            accounting.model_input_tokens = InputTokens::known(100, Provenance::Measured);
            accounting.model_output_tokens = OutputTokens::known(20, Provenance::Measured);
            accounting.attempts = AttemptCount::known(1, Provenance::Measured);
            contributions.push_back(accounting_draft(SubjectId::of(AttemptId::from_value(attempt_id)),
                                                     std::move(accounting)));
        }
        AccountingVector experiment_accounting;
        experiment_accounting.wall_nanos = WallNanos::known(1000, Provenance::Measured);
        contributions.push_back(accounting_draft(SubjectId::of(ExperimentId::from_value(experiment)),
                                                 std::move(experiment_accounting)));
    }
    if (!append_all(ledger, contributions, 256, "the accounting fixture contributions")) {
        return nullptr;
    }
    return ledger;
}

bool bench_accounting(const std::shared_ptr<Ledger>& ledger, std::uint64_t expected_contributions,
                      std::uint64_t iterations) {
    const LedgerSnapshot snapshot = ledger->snapshot();
    const SubjectId scope = SubjectId::of(ResearchSessionId::from_value(1));
    auto warm = snapshot.accounting(scope);
    if (!warm.ok() || warm.value().contributions != expected_contributions) {
        const std::uint64_t observed = warm.ok() ? warm.value().contributions : 0;
        return bench_fail("warm-up accounting aggregation returned " + id_text(observed) +
                          " contributions");
    }
    std::uint64_t checksum = 0;
    Stopwatch watch;
    watch.start();
    for (std::uint64_t iteration = 0; iteration < iterations; ++iteration) {
        auto aggregate = snapshot.accounting(scope);
        if (!aggregate.ok() || aggregate.value().contributions != expected_contributions) {
            return bench_fail("accounting aggregation lost contributions");
        }
        checksum += aggregate.value().total.model_input_tokens.units();
    }
    const double seconds = watch.seconds();
    report(Measurement{"accounting_aggregate",
                       "session scope over " + id_text(expected_contributions) + " contributions", 1,
                       iterations, seconds, checksum, "aggregations/s"});
    return true;
}

// --- persistence, replay and digests ----------------------------------------

bool bench_snapshot_save(const std::shared_ptr<Ledger>& ledger, const std::filesystem::path& path,
                         std::uint64_t iterations) {
    std::error_code error;
    if (std::filesystem::remove(path, error)) {
        std::printf("[persistence] removed a snapshot left by a previous run\n");
    }
    error.clear();
    const Status warm = ledger->save(path.string());
    if (!warm.ok()) {
        return bench_fail("warm-up save -> " + status_text(warm));
    }
    Stopwatch watch;
    watch.start();
    for (std::uint64_t iteration = 0; iteration < iterations; ++iteration) {
        const Status saved = ledger->save(path.string());
        if (!saved.ok()) {
            return bench_fail("save -> " + status_text(saved));
        }
    }
    const double seconds = watch.seconds();
    const std::uintmax_t bytes = std::filesystem::file_size(path, error);
    if (error) {
        return bench_fail("the saved snapshot is missing");
    }
    report(Measurement{"snapshot_save",
                       id_text(ledger->last_sequence().value()) + "-record snapshot, " +
                           id_text(static_cast<std::uint64_t>(bytes)) + " bytes",
                       1, iterations, seconds, static_cast<std::uint64_t>(bytes), "saves/s"});
    return true;
}

bool bench_snapshot_load(const std::shared_ptr<Ledger>& ledger, const std::filesystem::path& path,
                         std::uint64_t iterations) {
    auto warm = Ledger::load(path.string(), LedgerConfig{});
    if (!warm.ok()) {
        return bench_fail("warm-up load -> " + status_text(warm.status()));
    }
    std::uint64_t checksum = 0;
    Stopwatch watch;
    watch.start();
    for (std::uint64_t iteration = 0; iteration < iterations; ++iteration) {
        auto loaded = Ledger::load(path.string(), LedgerConfig{});
        if (!loaded.ok()) {
            return bench_fail("load -> " + status_text(loaded.status()));
        }
        if (loaded.value()->last_sequence() != ledger->last_sequence()) {
            return bench_fail("the reloaded ledger has a different last sequence");
        }
        checksum += loaded.value()->last_sequence().value();
        if (!(loaded.value()->logical_digest() == ledger->logical_digest())) {
            return bench_fail("a reloaded ledger has a different logical digest");
        }
    }
    const double seconds = watch.seconds();
    std::error_code size_error;
    const std::uintmax_t bytes = std::filesystem::file_size(path, size_error);
    report(Measurement{"snapshot_load",
                       id_text(ledger->last_sequence().value()) + "-record snapshot, " +
                           id_text(size_error ? 0u : static_cast<std::uint64_t>(bytes)) + " bytes",
                       1, iterations, seconds, checksum, "loads/s"});
    return true;
}

bool bench_replay(const std::shared_ptr<Ledger>& ledger) {
    const LedgerSnapshot snapshot = ledger->snapshot();
    auto records = snapshot.records(RecordSequence::from_value(1), ledger->last_sequence());
    if (!records.ok()) {
        return bench_fail("snapshot.records -> " + status_text(records.status()));
    }
    LedgerConfig config;
    config.generation = ledger->generation();
    config.epoch = ledger->epoch();
    auto warm = replay_committed(records.value(), config);
    if (!warm.ok()) {
        return bench_fail("warm-up replay -> " + status_text(warm.status()));
    }
    if (!(warm.value().logical_digest == ledger->logical_digest())) {
        return bench_fail("warm-up replay did not reproduce the live logical digest");
    }
    Stopwatch watch;
    watch.start();
    auto replay = replay_committed(records.value(), config);
    const double seconds = watch.seconds();
    if (!replay.ok()) {
        return bench_fail("replay -> " + status_text(replay.status()));
    }
    if (!(replay.value().logical_digest == warm.value().logical_digest)) {
        return bench_fail("replaying the same records produced a different logical digest");
    }
    if (!replay.value().integrity.ok) {
        return bench_fail("replay reported an integrity failure");
    }
    report(Measurement{"replay_digest",
                       id_text(replay.value().records) + " records replayed, integrity re-verified",
                       1, replay.value().records, seconds,
                       digest_checksum(replay.value().logical_digest), "records/s"});
    return true;
}

bool bench_logical_digest(const std::shared_ptr<Ledger>& ledger, std::uint64_t iterations) {
    const Digest warm = ledger->logical_digest();
    if (warm.is_zero()) {
        return bench_fail("warm-up logical digest is zero");
    }
    std::uint64_t checksum = 0;
    Stopwatch watch;
    watch.start();
    for (std::uint64_t iteration = 0; iteration < iterations; ++iteration) {
        const Digest digest = ledger->logical_digest();
        if (!(digest == warm)) {
            return bench_fail("the logical digest is not stable across identical calls");
        }
        checksum += digest_checksum(digest);
    }
    const double seconds = watch.seconds();
    report(Measurement{"logical_digest",
                       id_text(ledger->last_sequence().value()) + "-record ledger", 1, iterations,
                       seconds, checksum, "digests/s"});
    return true;
}

}  // namespace
}  // namespace research_ledger

int main() {
    using research_ledger::bench_accounting;
    using research_ledger::bench_append_batch;
    using research_ledger::bench_append_single;
    using research_ledger::bench_artifact_deep_chain;
    using research_ledger::bench_artifact_wide_fanout;
    using research_ledger::bench_branch_ancestry;
    using research_ledger::bench_explain_result;
    using research_ledger::bench_hypothesis_ancestry;
    using research_ledger::bench_logical_digest;
    using research_ledger::bench_point_lookup;
    using research_ledger::bench_replay;
    using research_ledger::bench_snapshot_load;
    using research_ledger::bench_snapshot_save;
    using research_ledger::bench_supporting_evidence;
    using research_ledger::build_accounting_fixture;
    using research_ledger::build_index_fixture;
    using research_ledger::build_result_fixture;
    using research_ledger::g_failures;
    using research_ledger::scratch_path;

    std::printf("research-ledger-bench: local synthetic benchmark\n");
    std::printf("One process, one machine, no network, no hardware counters. Every measured "
                "operation\nruns through the public API on committed state and is checked for "
                "success; each\nbenchmark warms up before its timed loop. Timings come from "
                "std::chrono::steady_clock.\n\n");

    int executed = 0;
    bool all_ran = true;

    all_ran = bench_append_single() && all_ran;
    ++executed;
    all_ran = bench_append_batch() && all_ran;
    ++executed;

    const std::shared_ptr<research_ledger::Ledger> index_ledger = build_index_fixture(10000, 10000);
    if (index_ledger == nullptr) {
        std::printf("\nsummary: local synthetic benchmark aborted, the index fixture is not "
                    "available\n");
        return 1;
    }
    std::printf("\n[index fixture] %llu committed records (10000 sessions, 10000 hypotheses)\n\n",
                static_cast<unsigned long long>(index_ledger->last_sequence().value()));

    all_ran = bench_point_lookup(index_ledger, 10000) && all_ran;
    ++executed;
    all_ran = bench_hypothesis_ancestry(500, 200) && all_ran;
    ++executed;
    all_ran = bench_branch_ancestry(500, 200) && all_ran;
    ++executed;
    all_ran = bench_artifact_deep_chain(1000, 20) && all_ran;
    ++executed;
    all_ran = bench_artifact_wide_fanout(1000, 20) && all_ran;
    ++executed;

    const std::shared_ptr<research_ledger::Ledger> result_ledger = build_result_fixture();
    if (result_ledger == nullptr) {
        std::printf("\nsummary: local synthetic benchmark aborted, the result fixture is not "
                    "available\n");
        return 1;
    }
    std::printf("\n[result fixture] %llu committed records ending in an accepted result\n\n",
                static_cast<unsigned long long>(result_ledger->last_sequence().value()));
    all_ran = bench_supporting_evidence(result_ledger, 200) && all_ran;
    ++executed;
    all_ran = bench_explain_result(result_ledger, 200) && all_ran;
    ++executed;

    const std::shared_ptr<research_ledger::Ledger> accounting_ledger =
        build_accounting_fixture(20, 10);
    if (accounting_ledger == nullptr) {
        std::printf("\nsummary: local synthetic benchmark aborted, the accounting fixture is not "
                    "available\n");
        return 1;
    }
    std::printf("\n[accounting fixture] %llu committed records, 220 accounting records\n\n",
                static_cast<unsigned long long>(accounting_ledger->last_sequence().value()));
    all_ran = bench_accounting(accounting_ledger, 220, 200) && all_ran;
    ++executed;

    const std::filesystem::path snapshot_path =
        scratch_path("research-ledger-bench-snapshot.rls");
    std::printf("\n[persistence] snapshot file %s\n\n", snapshot_path.string().c_str());
    const bool saved = bench_snapshot_save(index_ledger, snapshot_path, 5);
    all_ran = saved && all_ran;
    ++executed;
    if (saved) {
        all_ran = bench_snapshot_load(index_ledger, snapshot_path, 5) && all_ran;
        ++executed;
    } else {
        std::printf("%-30s skipped: the snapshot could not be written\n", "snapshot_load");
    }
    std::error_code remove_error;
    if (std::filesystem::remove(snapshot_path, remove_error)) {
        std::printf("[persistence] snapshot file removed\n");
    }

    std::printf("\n[replay and digests] over the index fixture\n\n");
    all_ran = bench_replay(index_ledger) && all_ran;
    ++executed;
    all_ran = bench_logical_digest(index_ledger, 10) && all_ran;
    ++executed;

    std::printf("\nsummary: local synthetic benchmark; %d measurement(s); %d failure(s); "
                "all measured operations completed=%s; append_fixture_records=10000; "
                "index_fixture_records=%llu\n",
                executed, g_failures, all_ran && g_failures == 0 ? "true" : "false",
                static_cast<unsigned long long>(index_ledger->last_sequence().value()));
    return (all_ran && g_failures == 0) ? 0 : 1;
}
