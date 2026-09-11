#include "test_framework.hpp"
#include "test_support.hpp"

#include "research_ledger/ledger.hpp"
#include "research_ledger/persistence.hpp"
#include "research_ledger/replay.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace research_ledger {
namespace {

using namespace research_ledger::test;

// Every query is read through this helper so that a failed query is reported
// through the framework instead of being dereferenced.
template <class T>
inline T expect(Result<T> result, const char* what) {
    RL_CHECK_MESSAGE(result.ok(), std::string(what) + ": " +
                                      std::string(error_code_name(result.code())) + " (" +
                                      result.message() + ")");
    return result.take();
}

// A Status is not a Result: it is reported through the same controlled path.
inline void require_ok(const Status& status, const char* what) {
    RL_CHECK_MESSAGE(status.ok(), std::string(what) + ": " +
                                      std::string(error_code_name(status.code)) + " (" +
                                      status.message + ")");
}

inline std::string temp_path(const char* name) {
    std::error_code error;
    std::filesystem::path root = std::filesystem::temp_directory_path(error);
    if (error) {
        root = std::filesystem::path(".");
    }
    root /= "rlagent";
    std::filesystem::create_directories(root, error);
    root /= name;
    return root.string();
}

inline std::vector<std::byte> read_bytes(const std::string& path) {
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    if (!stream) {
        return {};
    }
    const std::streamoff size = stream.tellg();
    if (size <= 0) {
        return {};
    }
    std::vector<std::byte> bytes(static_cast<std::size_t>(size));
    stream.seekg(0, std::ios::beg);
    stream.read(reinterpret_cast<char*>(bytes.data()), size);
    if (!stream) {
        return {};
    }
    return bytes;
}

inline bool write_bytes(const std::string& path, const std::vector<std::byte>& bytes) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream) {
        return false;
    }
    stream.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    stream.flush();
    return stream.good();
}

// --- a deterministic generator of valid research histories ------------------

// Records are committed in chunks of this size. A subject that a batch
// declares and then revises more than once is not a case this generator
// produces: see the notes on repeated staged revisions in the report.
constexpr std::size_t kBatchChunk = 32u;

struct GeneratedHistory {
    std::vector<RecordDraft> drafts{};

    std::vector<std::uint64_t> hypothesis_ids{};
    std::vector<std::uint32_t> hypothesis_generation{};
    std::vector<HypothesisStatus> hypothesis_status{};
    std::vector<std::uint64_t> hypothesis_parent{};
    std::vector<std::size_t> hypothesis_revision_at{};

    std::vector<std::uint64_t> branch_ids{};
    std::vector<std::uint64_t> branch_parent{};

    std::vector<std::uint64_t> experiment_ids{};
    std::vector<std::uint64_t> experiment_branch{};
    std::vector<std::uint64_t> experiment_hypothesis{};

    std::vector<std::uint64_t> attempt_ids{};
    std::vector<std::uint64_t> attempt_experiment{};
    std::vector<AttemptState> attempt_state{};

    std::vector<std::uint64_t> artifact_ids{};
    std::vector<std::vector<std::uint64_t>> artifact_parents{};

    std::vector<std::uint64_t> failure_ids{};
    std::vector<std::uint64_t> result_ids{};
    std::vector<std::uint32_t> result_generation{};
    std::vector<ResultStatus> result_status{};
    std::vector<std::size_t> result_revision_at{};
    std::vector<std::uint64_t> decision_ids{};

    std::uint64_t next_record_id = 1;
};

// The decision that authorizes a permitted result transition.
inline bool decision_for_transition(ResultStatus from, ResultStatus to, DecisionType& type,
                                    DecisionOutcome& outcome) {
    if (from == ResultStatus::Candidate && to == ResultStatus::Accepted) {
        type = DecisionType::Accept;
        outcome = DecisionOutcome::Accepted;
    } else if (from == ResultStatus::Candidate && to == ResultStatus::Rejected) {
        type = DecisionType::Reject;
        outcome = DecisionOutcome::Rejected;
    } else if (to == ResultStatus::Superseded) {
        type = DecisionType::Supersede;
        outcome = DecisionOutcome::Superseded;
    } else if (to == ResultStatus::Invalidated) {
        type = DecisionType::Invalidate;
        outcome = DecisionOutcome::Invalidated;
    } else if (to == ResultStatus::Retracted) {
        type = DecisionType::Retract;
        outcome = DecisionOutcome::Retracted;
    } else if (to == ResultStatus::Candidate) {
        type = DecisionType::Reclassify;
        outcome = DecisionOutcome::Reclassified;
    } else {
        return false;
    }
    return result_transition_allowed(from, to) && result_status_matches_decision(to, outcome) &&
           decision_outcome_matches_type(type, outcome);
}

inline MetricValue random_measurement(Rng& rng) {
    switch (rng.u32(7u)) {
        case 0u:
            return MetricValue::ratio(0.25 + static_cast<double>(rng.u32(1000u)) / 4000.0).take();
        case 1u:
            return MetricValue::count(rng.range(1000000u)).take();
        case 2u:
            return MetricValue::bytes(rng.range(1u << 20)).take();
        case 3u:
            return MetricValue::duration_nanos(rng.range(1u << 24)).take();
        case 4u:
            return MetricValue::boolean(rng.chance(1u, 2u)).take();
        case 5u:
            return MetricValue::signed_integer(-static_cast<std::int64_t>(rng.range(1000u))).take();
        default:
            return MetricValue::unknown();
    }
}

inline UnitKind unit_for(const MetricValue& value) {
    switch (value.kind()) {
        case MetricValueKind::Ratio:
        case MetricValueKind::Rate:
            return UnitKind::Ratio;
        case MetricValueKind::Count:
        case MetricValueKind::Boolean:
        case MetricValueKind::SignedInteger:
        case MetricValueKind::UnsignedInteger:
            return UnitKind::Count;
        case MetricValueKind::Bytes:
            return UnitKind::Bytes;
        case MetricValueKind::DurationNanos:
            return UnitKind::Nanoseconds;
        case MetricValueKind::Decimal:
            return UnitKind::Seconds;
        case MetricValueKind::Category:
            return UnitKind::Category;
        case MetricValueKind::Digest:
            return UnitKind::Custom;
        case MetricValueKind::Unknown:
            return UnitKind::Ratio;
    }
    return UnitKind::Ratio;
}

// Builds a fixed number of valid record drafts against one research session.
// Every step chooses an operation whose prerequisites are already committed, so
// the generated history is a legal one whatever the random choices are.
inline void generate_history(Rng& rng, std::uint32_t operations, GeneratedHistory& history) {
    const auto push = [&history](RecordDraft draft) {
        draft.record_id = LedgerRecordId::from_value(history.next_record_id++);
        history.drafts.push_back(std::move(draft));
    };

    push(session_opened(1, "generated session", "can a generated history be committed?"));

    std::uint64_t next_hypothesis = 1;
    std::uint64_t next_branch = 1;
    std::uint64_t next_experiment = 1;
    std::uint64_t next_attempt = 1;
    std::uint64_t next_model_call = 1;
    std::uint64_t next_tool_call = 1;
    std::uint64_t next_observation = 1;
    std::uint64_t next_artifact = 1;
    std::uint64_t next_failure = 1;
    std::uint64_t next_result = 1;
    std::uint64_t next_decision = 1;

    for (std::uint32_t step = 0; step < operations; ++step) {
        bool produced = false;
        for (unsigned attempt = 0; attempt < 8 && !produced; ++attempt) {
            switch (rng.u32(14u)) {
                case 0u: {
                    HypothesisDeclared body;
                    body.hypothesis = HypothesisId::from_value(next_hypothesis);
                    body.generation = HypothesisGeneration::first();
                    body.session = ResearchSessionId::from_value(1);
                    body.claim = "generated claim " + std::to_string(next_hypothesis);
                    std::uint64_t parent = 0;
                    if (!history.hypothesis_ids.empty() && rng.chance(1u, 3u)) {
                        // Only a parent whose current revision is already
                        // committed is named, so the generation it is cited
                        // with is the generation the ledger holds.
                        std::vector<std::size_t> settled;
                        for (std::size_t index = 0; index < history.hypothesis_ids.size();
                             ++index) {
                            if (history.drafts.size() - history.hypothesis_revision_at[index] >=
                                kBatchChunk) {
                                settled.push_back(index);
                            }
                        }
                        if (!settled.empty()) {
                            const std::size_t index =
                                settled[static_cast<std::size_t>(rng.range(settled.size()))];
                            parent = history.hypothesis_ids[index];
                            body.parent = HypothesisId::from_value(parent);
                            body.parent_generation = HypothesisGeneration::from_value(
                                history.hypothesis_generation[index]);
                        }
                    }
                    body.authority = "generator";
                    push(draft_of(body, Provenance::Reported));
                    history.hypothesis_ids.push_back(next_hypothesis);
                    history.hypothesis_generation.push_back(1);
                    history.hypothesis_status.push_back(HypothesisStatus::Proposed);
                    history.hypothesis_parent.push_back(parent);
                    history.hypothesis_revision_at.push_back(history.drafts.size());
                    next_hypothesis += 1;
                    produced = true;
                    break;
                }
                case 1u: {
                    if (history.hypothesis_ids.empty()) {
                        break;
                    }
                    std::vector<std::size_t> revisable;
                    for (std::size_t candidate = 0; candidate < history.hypothesis_ids.size();
                         ++candidate) {
                        if (history.drafts.size() - history.hypothesis_revision_at[candidate] <
                            kBatchChunk) {
                            continue;
                        }
                        const HypothesisStatus status = history.hypothesis_status[candidate];
                        for (std::uint8_t raw = 1;
                             raw <= static_cast<std::uint8_t>(HypothesisStatus::Inconclusive);
                             ++raw) {
                            if (hypothesis_transition_allowed(
                                    status, static_cast<HypothesisStatus>(raw))) {
                                revisable.push_back(candidate);
                                break;
                            }
                        }
                    }
                    if (revisable.empty()) {
                        break;
                    }
                    const std::size_t index =
                        revisable[static_cast<std::size_t>(rng.range(revisable.size()))];
                    const HypothesisStatus from = history.hypothesis_status[index];
                    std::vector<HypothesisStatus> allowed;
                    for (std::uint8_t raw = 1;
                         raw <= static_cast<std::uint8_t>(HypothesisStatus::Inconclusive); ++raw) {
                        const auto candidate = static_cast<HypothesisStatus>(raw);
                        if (hypothesis_transition_allowed(from, candidate)) {
                            allowed.push_back(candidate);
                        }
                    }
                    if (allowed.empty()) {
                        break;
                    }
                    const HypothesisStatus to =
                        allowed[static_cast<std::size_t>(rng.range(allowed.size()))];
                    HypothesisStatusChanged body;
                    body.hypothesis = HypothesisId::from_value(history.hypothesis_ids[index]);
                    body.generation = HypothesisGeneration::from_value(
                        history.hypothesis_generation[index] + 1u);
                    body.from = from;
                    body.to = to;
                    body.authority = "generator";
                    push(draft_of(body, Provenance::Reported));
                    history.hypothesis_status[index] = to;
                    history.hypothesis_generation[index] += 1u;
                    history.hypothesis_revision_at[index] = history.drafts.size();
                    produced = true;
                    break;
                }
                case 2u: {
                    const bool root = history.branch_ids.empty() || rng.chance(1u, 4u);
                    const std::uint64_t parent =
                        root ? 0
                             : history.branch_ids[static_cast<std::size_t>(
                                   rng.range(history.branch_ids.size()))];
                    BranchDeclared body;
                    body.branch = BranchId::from_value(next_branch);
                    body.session = ResearchSessionId::from_value(1);
                    body.kind =
                        root ? BranchKind::Root : static_cast<BranchKind>(2u + rng.u32(7u));
                    if (parent != 0) {
                        body.parent_branch = BranchId::from_value(parent);
                    }
                    body.label = "branch " + std::to_string(next_branch);
                    body.authority = "generator";
                    push(draft_of(body, Provenance::Reported));
                    history.branch_ids.push_back(next_branch);
                    history.branch_parent.push_back(parent);
                    next_branch += 1;
                    produced = true;
                    break;
                }
                case 3u: {
                    if (history.hypothesis_ids.empty() || history.branch_ids.empty()) {
                        break;
                    }
                    const std::size_t hypothesis =
                        static_cast<std::size_t>(rng.range(history.hypothesis_ids.size()));
                    const std::size_t branch =
                        static_cast<std::size_t>(rng.range(history.branch_ids.size()));
                    ExperimentDeclared body;
                    body.experiment = ExperimentId::from_value(next_experiment);
                    body.generation = ExperimentGeneration::first();
                    body.session = ResearchSessionId::from_value(1);
                    body.hypotheses.push_back(
                        HypothesisId::from_value(history.hypothesis_ids[hypothesis]));
                    body.branch = BranchId::from_value(history.branch_ids[branch]);
                    body.environment_reference = "env:generated";
                    body.authority = "generator";
                    push(draft_of(body, Provenance::Reported));
                    history.experiment_ids.push_back(next_experiment);
                    history.experiment_branch.push_back(history.branch_ids[branch]);
                    history.experiment_hypothesis.push_back(history.hypothesis_ids[hypothesis]);
                    next_experiment += 1;
                    produced = true;
                    break;
                }
                case 4u: {
                    if (history.experiment_ids.empty()) {
                        break;
                    }
                    const std::size_t index =
                        static_cast<std::size_t>(rng.range(history.experiment_ids.size()));
                    AttemptStarted body;
                    body.attempt = AttemptId::from_value(next_attempt);
                    body.generation = AttemptGeneration::first();
                    body.experiment = ExperimentId::from_value(history.experiment_ids[index]);
                    body.branch = BranchId::from_value(history.experiment_branch[index]);
                    body.worker_authority = "worker:1";
                    push(draft_of(body, Provenance::Reported));
                    history.attempt_ids.push_back(next_attempt);
                    history.attempt_experiment.push_back(history.experiment_ids[index]);
                    history.attempt_state.push_back(AttemptState::Running);
                    next_attempt += 1;
                    produced = true;
                    break;
                }
                case 5u: {
                    std::vector<std::size_t> running;
                    for (std::size_t index = 0; index < history.attempt_state.size(); ++index) {
                        if (history.attempt_state[index] == AttemptState::Running) {
                            running.push_back(index);
                        }
                    }
                    if (running.empty()) {
                        break;
                    }
                    const std::size_t index =
                        running[static_cast<std::size_t>(rng.range(running.size()))];
                    const std::uint64_t identity = history.attempt_ids[index];
                    switch (rng.u32(3u)) {
                        case 0u:
                            push(attempt_completed(identity, "generated outcome"));
                            history.attempt_state[index] = AttemptState::Completed;
                            break;
                        case 1u:
                            push(failure_recorded(
                                next_failure,
                                SubjectId::of(AttemptId::from_value(identity)),
                                FailureCategory::Execution, "generated failure"));
                            push(attempt_failed(identity, next_failure));
                            history.failure_ids.push_back(next_failure);
                            next_failure += 1;
                            history.attempt_state[index] = AttemptState::Failed;
                            break;
                        default:
                            push(attempt_cancelled(identity, "generated cancellation"));
                            history.attempt_state[index] = AttemptState::Cancelled;
                            break;
                    }
                    produced = true;
                    break;
                }
                case 6u:
                case 7u: {
                    std::vector<std::size_t> running;
                    for (std::size_t index = 0; index < history.attempt_state.size(); ++index) {
                        if (history.attempt_state[index] == AttemptState::Running) {
                            running.push_back(index);
                        }
                    }
                    if (running.empty()) {
                        break;
                    }
                    const std::uint64_t identity = history.attempt_ids[running[static_cast<
                        std::size_t>(rng.range(running.size()))]];
                    if (rng.chance(1u, 2u)) {
                        push(model_call_recorded(next_model_call, identity,
                                                 "model-" + std::to_string(1u + rng.u32(3u)),
                                                 ModelCallOutcome::Succeeded, rng.range(1000u),
                                                 rng.range(1000u)));
                        next_model_call += 1;
                    } else {
                        push(tool_call_recorded(next_tool_call, identity,
                                                "tool-" + std::to_string(1u + rng.u32(3u)),
                                                ToolCallState::Completed));
                        next_tool_call += 1;
                    }
                    produced = true;
                    break;
                }
                case 8u: {
                    std::vector<std::size_t> running;
                    for (std::size_t index = 0; index < history.attempt_state.size(); ++index) {
                        if (history.attempt_state[index] == AttemptState::Running) {
                            running.push_back(index);
                        }
                    }
                    if (running.empty()) {
                        break;
                    }
                    const std::uint64_t identity = history.attempt_ids[running[static_cast<
                        std::size_t>(rng.range(running.size()))]];
                    const MetricValue value = random_measurement(rng);
                    push(observation_recorded(next_observation, identity,
                                              "metric-" + std::to_string(1u + rng.u32(5u)), value,
                                              unit_for(value)));
                    next_observation += 1;
                    produced = true;
                    break;
                }
                case 9u: {
                    if (history.attempt_ids.empty()) {
                        break;
                    }
                    const std::uint64_t producer =
                        history.attempt_ids[static_cast<std::size_t>(
                            rng.range(history.attempt_ids.size()))];
                    std::vector<ArtifactId> parents;
                    if (!history.artifact_ids.empty() && rng.chance(2u, 3u)) {
                        const std::size_t count = 1u + static_cast<std::size_t>(rng.u32(2u));
                        for (std::size_t parent = 0; parent < count; ++parent) {
                            const ArtifactId candidate = ArtifactId::from_value(
                                history.artifact_ids[static_cast<std::size_t>(
                                    rng.range(history.artifact_ids.size()))]);
                            if (std::find(parents.begin(), parents.end(), candidate) ==
                                parents.end()) {
                                parents.push_back(candidate);
                            }
                        }
                    }
                    push(artifact_referenced(
                        next_artifact, 1, SubjectId::of(AttemptId::from_value(producer)),
                        static_cast<ArtifactRole>(1u + rng.u32(11u)),
                        "artifact-" + std::to_string(next_artifact), parents));
                    history.artifact_ids.push_back(next_artifact);
                    std::vector<std::uint64_t> recorded;
                    for (const ArtifactId parent : parents) {
                        recorded.push_back(parent.value());
                    }
                    history.artifact_parents.push_back(recorded);
                    next_artifact += 1;
                    produced = true;
                    break;
                }
                case 10u: {
                    if (history.attempt_ids.empty()) {
                        break;
                    }
                    const std::uint64_t identity =
                        history.attempt_ids[static_cast<std::size_t>(
                            rng.range(history.attempt_ids.size()))];
                    push(failure_recorded(next_failure,
                                          SubjectId::of(AttemptId::from_value(identity)),
                                          FailureCategory::Tool,
                                          "generated failure " + std::to_string(next_failure)));
                    history.failure_ids.push_back(next_failure);
                    next_failure += 1;
                    produced = true;
                    break;
                }
                case 11u: {
                    if (history.attempt_ids.empty()) {
                        break;
                    }
                    const std::uint64_t identity =
                        history.attempt_ids[static_cast<std::size_t>(
                            rng.range(history.attempt_ids.size()))];
                    AccountingVector accounting;
                    accounting.model_input_tokens =
                        InputTokens::known(rng.range(10000u), Provenance::Measured);
                    accounting.model_output_tokens =
                        OutputTokens::known(rng.range(10000u), Provenance::Measured);
                    if (rng.chance(1u, 2u)) {
                        accounting.attempts = AttemptCount::known(1, Provenance::Measured);
                    }
                    if (rng.chance(1u, 2u)) {
                        accounting.monetary = MonetaryMeasure::known_amount(
                            static_cast<std::int64_t>(rng.range(100000u)), "USD",
                            Provenance::Measured);
                    }
                    push(accounting_recorded(SubjectId::of(AttemptId::from_value(identity)),
                                             accounting, "generator"));
                    produced = true;
                    break;
                }
                case 12u: {
                    if (history.experiment_ids.empty()) {
                        break;
                    }
                    const std::size_t index =
                        static_cast<std::size_t>(rng.range(history.experiment_ids.size()));
                    std::vector<ArtifactId> artifacts;
                    if (!history.artifact_ids.empty() && rng.chance(1u, 2u)) {
                        artifacts.push_back(ArtifactId::from_value(
                            history.artifact_ids[static_cast<std::size_t>(
                                rng.range(history.artifact_ids.size()))]));
                    }
                    push(result_declared(next_result, 1, history.experiment_hypothesis[index],
                                         history.experiment_ids[index], artifacts));
                    history.result_ids.push_back(next_result);
                    history.result_generation.push_back(1);
                    history.result_status.push_back(ResultStatus::Candidate);
                    history.result_revision_at.push_back(history.drafts.size());
                    next_result += 1;
                    produced = true;
                    break;
                }
                default: {
                    std::vector<std::size_t> movable;
                    for (std::size_t index = 0; index < history.result_status.size(); ++index) {
                        if (history.drafts.size() - history.result_revision_at[index] <
                            kBatchChunk) {
                            continue;
                        }
                        for (std::uint8_t raw = 1;
                             raw <= static_cast<std::uint8_t>(ResultStatus::Retracted); ++raw) {
                            DecisionType type = DecisionType::Invalid;
                            DecisionOutcome outcome = DecisionOutcome::Invalid;
                            if (decision_for_transition(history.result_status[index],
                                                        static_cast<ResultStatus>(raw), type,
                                                        outcome)) {
                                movable.push_back(index);
                                break;
                            }
                        }
                    }
                    if (movable.empty()) {
                        break;
                    }
                    const std::size_t index =
                        movable[static_cast<std::size_t>(rng.range(movable.size()))];
                    const ResultStatus from = history.result_status[index];
                    std::vector<ResultStatus> allowed;
                    for (std::uint8_t raw = 1;
                         raw <= static_cast<std::uint8_t>(ResultStatus::Retracted); ++raw) {
                        DecisionType type = DecisionType::Invalid;
                        DecisionOutcome outcome = DecisionOutcome::Invalid;
                        if (decision_for_transition(from, static_cast<ResultStatus>(raw), type,
                                                    outcome)) {
                            allowed.push_back(static_cast<ResultStatus>(raw));
                        }
                    }
                    const ResultStatus to =
                        allowed[static_cast<std::size_t>(rng.range(allowed.size()))];
                    DecisionType type = DecisionType::Invalid;
                    DecisionOutcome outcome = DecisionOutcome::Invalid;
                    if (!decision_for_transition(from, to, type, outcome)) {
                        break;
                    }
                    push(decision_recorded(
                        next_decision, 1,
                        SubjectId::of(ResultId::from_value(history.result_ids[index])), type,
                        outcome));
                    history.decision_ids.push_back(next_decision);
                    push(result_status_changed(history.result_ids[index],
                                               history.result_generation[index] + 1u, from, to,
                                               next_decision));
                    history.result_status[index] = to;
                    history.result_generation[index] += 1u;
                    history.result_revision_at[index] = history.drafts.size();
                    next_decision += 1;
                    produced = true;
                    break;
                }
            }
        }
    }
}

inline Result<std::vector<AppendOutcome>> append_all(const std::shared_ptr<Ledger>& book,
                                                     const std::vector<RecordDraft>& drafts) {
    std::vector<AppendOutcome> outcomes;
    outcomes.reserve(drafts.size());
    std::size_t offset = 0;
    while (offset < drafts.size()) {
        const std::size_t count = std::min<std::size_t>(kBatchChunk, drafts.size() - offset);
        const std::vector<RecordDraft> chunk(
            drafts.begin() + static_cast<std::ptrdiff_t>(offset),
            drafts.begin() + static_cast<std::ptrdiff_t>(offset + count));
        auto appended = book->append_batch(chunk);
        if (!appended.ok()) {
            return appended.status();
        }
        outcomes.insert(outcomes.end(), appended.value().begin(), appended.value().end());
        offset += count;
    }
    return outcomes;
}

// --- state fingerprints -----------------------------------------------------

struct Fingerprint {
    std::uint64_t last_sequence = 0;
    Digest chain{};
    Digest logical{};
    std::uint64_t accounting_records = 0;
    std::vector<std::uint64_t> record_counts{};
};

inline Fingerprint fingerprint(const std::shared_ptr<Ledger>& book) {
    Fingerprint value;
    value.last_sequence = book->last_sequence().value();
    value.logical = book->logical_digest();
    const LedgerStats stats = expect(book->snapshot().stats(), "stats");
    value.chain = stats.chain_digest;
    value.accounting_records = stats.accounting_records;
    value.record_counts = stats.record_counts;
    return value;
}

inline bool identical(const Fingerprint& lhs, const Fingerprint& rhs) {
    return lhs.last_sequence == rhs.last_sequence && lhs.chain == rhs.chain &&
           lhs.logical == rhs.logical && lhs.accounting_records == rhs.accounting_records &&
           lhs.record_counts == rhs.record_counts;
}

// --- independent accounting recomputation -----------------------------------

#define RL_ACCOUNTING_FIELDS(X) \
    X(model_input_tokens)       \
    X(model_output_tokens)      \
    X(model_calls)              \
    X(tool_calls)               \
    X(accelerator_nanos)        \
    X(cpu_nanos)                \
    X(wall_nanos)               \
    X(storage_bytes)            \
    X(transfer_bytes)           \
    X(energy_micro_joules)      \
    X(attempts)                 \
    X(retries)                  \
    X(failure_overhead_nanos)

struct FieldTotal {
    bool known = false;
    std::uint64_t units = 0;
};

struct VectorTotal {
#define RL_DECLARE_TOTAL(NAME) FieldTotal NAME;
    RL_ACCOUNTING_FIELDS(RL_DECLARE_TOTAL)
#undef RL_DECLARE_TOTAL
    bool monetary_known = false;
    std::int64_t monetary_micro = 0;
    std::string monetary_currency{};
};

enum class SumStatus { Ok, Overflow, CurrencyMismatch, Other };

// A second, independent implementation of the accumulation rule. It is written
// from the model rather than from the ledger, so agreement is evidence.
inline void expected_add(VectorTotal& total, const AccountingVector& addition, SumStatus& status) {
    if (status != SumStatus::Ok) {
        return;
    }
#define RL_ADD_FIELD(NAME)                                                              \
    if (!total.NAME.known || !addition.NAME.is_known()) {                               \
        total.NAME.known = false;                                                       \
        total.NAME.units = 0;                                                           \
    } else if (total.NAME.units >                                                       \
               std::numeric_limits<std::uint64_t>::max() - addition.NAME.units()) {     \
        status = SumStatus::Overflow;                                                   \
        return;                                                                         \
    } else {                                                                            \
        total.NAME.units += addition.NAME.units();                                      \
    }
    RL_ACCOUNTING_FIELDS(RL_ADD_FIELD)
#undef RL_ADD_FIELD
    if (!total.monetary_known || !addition.monetary.known) {
        total.monetary_known = false;
        total.monetary_micro = 0;
        if (total.monetary_currency.empty()) {
            total.monetary_currency = addition.monetary.currency;
        }
        return;
    }
    if (total.monetary_currency != addition.monetary.currency) {
        status = SumStatus::CurrencyMismatch;
        return;
    }
    const std::int64_t left = total.monetary_micro;
    const std::int64_t right = addition.monetary.micro_units;
    if ((right > 0 && left > std::numeric_limits<std::int64_t>::max() - right) ||
        (right < 0 && left < std::numeric_limits<std::int64_t>::min() - right)) {
        status = SumStatus::Overflow;
        return;
    }
    total.monetary_micro = left + right;
}

inline VectorTotal expected_total(const std::vector<AccountingVector>& contributions,
                                  SumStatus& status) {
    VectorTotal total;
    status = SumStatus::Ok;
    if (contributions.empty()) {
        return total;
    }
    const AccountingVector& first = contributions[0];
#define RL_SEED_FIELD(NAME)                     \
    total.NAME.known = first.NAME.is_known();   \
    total.NAME.units = first.NAME.is_known() ? first.NAME.units() : 0u;
    RL_ACCOUNTING_FIELDS(RL_SEED_FIELD)
#undef RL_SEED_FIELD
    total.monetary_known = first.monetary.known;
    total.monetary_micro = first.monetary.micro_units;
    total.monetary_currency = first.monetary.currency;
    for (std::size_t index = 1; index < contributions.size(); ++index) {
        expected_add(total, contributions[index], status);
        if (status != SumStatus::Ok) {
            break;
        }
    }
    return total;
}

inline bool matches(const AccountingVector& actual, const VectorTotal& expected) {
#define RL_MATCH_FIELD(NAME)                                                            \
    if (actual.NAME.is_known() != expected.NAME.known ||                                \
        (expected.NAME.known && actual.NAME.units() != expected.NAME.units)) {          \
        return false;                                                                   \
    }
    RL_ACCOUNTING_FIELDS(RL_MATCH_FIELD)
#undef RL_MATCH_FIELD
    if (actual.monetary.known != expected.monetary_known) {
        return false;
    }
    if (expected.monetary_known && actual.monetary.micro_units != expected.monetary_micro) {
        return false;
    }
    return true;
}

inline SumStatus classify(const Status& status) {
    if (status.code == ErrorCode::AccountingOverflow) {
        return SumStatus::Overflow;
    }
    if (status.code == ErrorCode::Unsupported) {
        return SumStatus::CurrencyMismatch;
    }
    return SumStatus::Other;
}

inline AccountingVector random_accounting(Rng& rng, bool extreme) {
    static const std::uint64_t small_pool[] = {0u, 1u, 2u, 7u, 1000u, 100000u};
    static const std::uint64_t extreme_pool[] = {
        0u, 1u, 2u, std::numeric_limits<std::uint64_t>::max() - 1u,
        std::numeric_limits<std::uint64_t>::max() / 2u,
        std::numeric_limits<std::uint64_t>::max()};
    static const Provenance provenance_pool[] = {Provenance::Measured, Provenance::Reported,
                                                 Provenance::Estimated, Provenance::Synthetic};
    static const std::int64_t money_pool[] = {
        0, 1, -1, 1000000, std::numeric_limits<std::int64_t>::max(),
        std::numeric_limits<std::int64_t>::min()};
    static const char* currency_pool[] = {"USD", "EUR"};

    const std::uint64_t* pool = extreme ? extreme_pool : small_pool;
    const std::size_t pool_size = 6u;
    AccountingVector vector;
#define RL_RANDOM_FIELD(NAME)                                                          \
    if (rng.chance(7u, 10u)) {                                                         \
        vector.NAME = decltype(vector.NAME)::known(                                    \
            pool[static_cast<std::size_t>(rng.range(pool_size))],                      \
            provenance_pool[static_cast<std::size_t>(rng.range(4u))]);                 \
    } else {                                                                           \
        vector.NAME = decltype(vector.NAME)::unknown(                                  \
            provenance_pool[static_cast<std::size_t>(rng.range(4u))]);                 \
    }
    RL_ACCOUNTING_FIELDS(RL_RANDOM_FIELD)
#undef RL_RANDOM_FIELD
    if (rng.chance(7u, 10u)) {
        vector.monetary = MonetaryMeasure::known_amount(
            money_pool[static_cast<std::size_t>(rng.range(6u))],
            currency_pool[static_cast<std::size_t>(rng.range(2u))],
            provenance_pool[static_cast<std::size_t>(rng.range(4u))]);
    } else {
        vector.monetary = MonetaryMeasure::unknown(
            provenance_pool[static_cast<std::size_t>(rng.range(4u))]);
    }
    if (vector.is_empty()) {
        vector.model_input_tokens = InputTokens::known(
            pool[static_cast<std::size_t>(rng.range(pool_size))], Provenance::Measured);
    }
    return vector;
}

// --- seeds ------------------------------------------------------------------

constexpr std::uint32_t kSeeds = 25;

inline RecordSequence sequence_of(std::uint64_t value) {
    return RecordSequence::from_value(static_cast<std::uint32_t>(value));
}

inline Result<std::shared_ptr<Ledger>> ledger_with_attempt(const LedgerConfig& config) {
    auto created = Ledger::create(config);
    if (!created.ok()) {
        return created.status();
    }
    std::shared_ptr<Ledger> ledger = created.take();
    const std::vector<RecordDraft> batch{session_opened(1),
                                          hypothesis_declared(1, 1, "the property holds"),
                                          branch_declared(1, 1, BranchKind::Root),
                                          experiment_declared(1, 1, 1, 1), attempt_started(1, 1, 1)};
    auto appended = ledger->append_batch(batch);
    if (!appended.ok()) {
        return appended.status();
    }
    return ledger;
}

// --- record sequence --------------------------------------------------------

RL_TEST(property_record_sequence_never_regresses_or_repeats) {
    for (std::uint32_t seed = 1; seed <= kSeeds; ++seed) {
        Rng rng(seed);
        const std::uint32_t operations = 40u + rng.u32(60u);
        std::printf("PROPERTY record-sequence seed=%u operations=%u\n", seed, operations);
        GeneratedHistory history;
        generate_history(rng, operations, history);
        // A step whose prerequisities are not committed yet produces nothing, so
        // the draft count is bounded below rather than fixed by the step count.
        RL_CHECK(history.drafts.size() >= operations / 2u);

        auto created = Ledger::create(local_config());
        RL_CHECK_OK(created);
        const std::shared_ptr<Ledger>& book = created.value();
        const std::vector<AppendOutcome> outcomes =
            expect(append_all(book, history.drafts), "generated history");
        RL_CHECK_EQ(outcomes.size(), history.drafts.size());

        std::uint64_t expected = 1;
        for (const AppendOutcome& outcome : outcomes) {
            RL_CHECK(outcome.state == CommitState::Committed);
            RL_CHECK(!outcome.duplicate);
            RL_CHECK_EQ(outcome.sequence.value(), expected);
            expected += 1;
        }
        RL_CHECK_EQ(book->last_sequence().value(), history.drafts.size());
        const IntegrityReport integrity = expect(book->snapshot().verify(), "verify");
        RL_CHECK(integrity.ok);
        RL_CHECK_EQ(integrity.issues.size(), 0u);
    }
}

// --- committed records never mutate -----------------------------------------

RL_TEST(property_a_snapshot_reports_what_it_reported_before) {
    for (std::uint32_t seed = 1; seed <= kSeeds; ++seed) {
        Rng rng(seed);
        const std::uint32_t operations = 40u + rng.u32(40u);
        std::printf("PROPERTY snapshot-stability seed=%u operations=%u\n", seed, operations);
        GeneratedHistory history;
        generate_history(rng, operations, history);

        const std::size_t half = history.drafts.size() / 2u;
        const std::vector<RecordDraft> first(history.drafts.begin(),
                                             history.drafts.begin() + static_cast<std::ptrdiff_t>(half));
        const std::vector<RecordDraft> second(history.drafts.begin() + static_cast<std::ptrdiff_t>(half),
                                              history.drafts.end());

        auto created = Ledger::create(local_config());
        RL_CHECK_OK(created);
        const std::shared_ptr<Ledger>& book = created.value();
        expect(append_all(book, first), "first half");

        auto old = book->snapshot();
        const RecordSequence watermark = old.watermark();
        RL_CHECK(watermark.valid());
        const std::vector<Record> before =
            expect(old.records(RecordSequence::from_value(1), watermark), "records");
        RL_CHECK_EQ(before.size(), half);
        const SessionView session_before =
            expect(old.session(ResearchSessionId::from_value(1)), "session");

        expect(append_all(book, second), "second half");
        auto now = book->snapshot();
        RL_CHECK(now.watermark() > watermark);

        // The records the snapshot exposes are exactly the records it exposed.
        const std::vector<Record> after =
            expect(old.records(RecordSequence::from_value(1), watermark), "records");
        RL_CHECK_EQ(after.size(), before.size());
        for (std::size_t index = 0; index < before.size() && index < after.size(); ++index) {
            const Record& lhs = before[index];
            const Record& rhs = after[index];
            RL_CHECK(lhs.header.sequence == rhs.header.sequence);
            RL_CHECK(lhs.header.type == rhs.header.type);
            RL_CHECK(lhs.header.record_id == rhs.header.record_id);
            RL_CHECK(lhs.header.payload_digest == rhs.header.payload_digest);
            RL_CHECK(lhs.header.chain_digest == rhs.header.chain_digest);
            RL_CHECK(lhs.header.committed_at == rhs.header.committed_at);
            RL_CHECK(lhs.header.authority == rhs.header.authority);
            RL_CHECK(lhs.header.provenance == rhs.header.provenance);
            const Digest lhs_body = expect(record_body_digest(lhs.body, old.limits()), "digest");
            const Digest rhs_body = expect(record_body_digest(rhs.body, old.limits()), "digest");
            RL_CHECK(lhs_body == rhs_body);
        }
        RL_CHECK(old.watermark() == watermark);
        RL_CHECK_CODE(old.record_at(sequence_of(watermark.value() + 1)), ErrorCode::NotFound);
        RL_CHECK_CODE(old.record_at(sequence_of(now.watermark().value())), ErrorCode::NotFound);

        // Entities that did not exist at the watermark are still not visible.
        std::uint64_t checked = 0;
        for (const std::uint64_t identity : history.hypothesis_ids) {
            const HypothesisId id = HypothesisId::from_value(identity);
            if (now.hypothesis(id).ok() && !old.hypothesis(id).ok()) {
                RL_CHECK_CODE(old.hypothesis(id), ErrorCode::NotFound);
                checked += 1;
            }
        }
        for (const std::uint64_t identity : history.attempt_ids) {
            const AttemptId id = AttemptId::from_value(identity);
            if (now.attempt(id).ok() && !old.attempt(id).ok()) {
                RL_CHECK_CODE(old.attempt(id), ErrorCode::NotFound);
                checked += 1;
            }
        }
        for (const std::uint64_t identity : history.artifact_ids) {
            const ArtifactId id = ArtifactId::from_value(identity);
            if (now.artifact(id).ok() && !old.artifact(id).ok()) {
                RL_CHECK_CODE(old.artifact(id), ErrorCode::NotFound);
                checked += 1;
            }
        }
        for (const std::uint64_t identity : history.result_ids) {
            const ResultId id = ResultId::from_value(identity);
            if (now.result(id).ok() && !old.result(id).ok()) {
                RL_CHECK_CODE(old.result(id), ErrorCode::NotFound);
                checked += 1;
            }
        }
        for (const std::uint64_t identity : history.decision_ids) {
            const DecisionId id = DecisionId::from_value(identity);
            if (now.decision(id).ok() && !old.decision(id).ok()) {
                RL_CHECK_CODE(old.decision(id), ErrorCode::NotFound);
                checked += 1;
            }
        }
        RL_CHECK(checked > 0u);

        // An entity the snapshot already reported reports the same declaration.
        const SessionView session_after =
            expect(old.session(ResearchSessionId::from_value(1)), "session");
        RL_CHECK_EQ(session_after.label, session_before.label);
        RL_CHECK_EQ(session_after.question, session_before.question);
        RL_CHECK(session_after.state == session_before.state);
        RL_CHECK(session_after.opened_at == session_before.opened_at);
        RL_CHECK_EQ(session_after.authority, session_before.authority);
    }
}

// --- identity domains -------------------------------------------------------

RL_TEST(property_identity_domains_never_collide) {
    for (std::uint32_t seed = 1; seed <= kSeeds; ++seed) {
        Rng rng(seed);
        std::printf("PROPERTY identity-domains seed=%u iterations=%u\n", seed, 40u);
        for (unsigned iteration = 0; iteration < 40u; ++iteration) {
            const std::uint64_t value = 1u + rng.range(1u << 30);
            const std::vector<SubjectId> subjects{
                SubjectId::of(ResearchSessionId::from_value(value)),
                SubjectId::of(HypothesisId::from_value(value)),
                SubjectId::of(ExperimentId::from_value(value)),
                SubjectId::of(BranchId::from_value(value)),
                SubjectId::of(AttemptId::from_value(value)),
                SubjectId::of(ModelCallId::from_value(value)),
                SubjectId::of(ToolCallId::from_value(value)),
                SubjectId::of(ArtifactId::from_value(value)),
                SubjectId::of(ObservationId::from_value(value)),
                SubjectId::of(FailureId::from_value(value)),
                SubjectId::of(DecisionId::from_value(value)),
                SubjectId::of(ResultId::from_value(value))};
            for (std::size_t lhs = 0; lhs < subjects.size(); ++lhs) {
                RL_CHECK(subjects[lhs].valid());
                RL_CHECK_EQ(subjects[lhs].value(), value);
                for (std::size_t rhs = 0; rhs < lhs; ++rhs) {
                    RL_CHECK(!(subjects[lhs] == subjects[rhs]));
                    RL_CHECK(subjects[lhs].to_string() != subjects[rhs].to_string());
                }
            }
            RL_CHECK(subjects[0].kind() == SubjectKind::Session);
            RL_CHECK(subjects[1].kind() == SubjectKind::Hypothesis);
            RL_CHECK(subjects[11].kind() == SubjectKind::Result);
            RL_CHECK(subjects[0].as<ResearchSessionId>().has_value());
            RL_CHECK(!subjects[0].as<ResultId>().has_value());
            RL_CHECK(subjects[11].as<ResultId>().has_value());
            RL_CHECK(!subjects[11].as<ResearchSessionId>().has_value());
            RL_CHECK(subjects[0].domain() == IdentityDomain::ResearchSession);
            RL_CHECK(subjects[11].domain() == IdentityDomain::Result);
            // Zero is not a valid identity in any domain, and a generation
            // never wraps into one that was already issued.
            RL_CHECK(!ResultId::from_value(0).valid());
            RL_CHECK(!ResearchSessionId::from_value(0).valid());
            RL_CHECK(!HypothesisGeneration::from_value(0).valid());
            RL_CHECK(HypothesisGeneration::from_value(0).next().valid());
            RL_CHECK(!HypothesisGeneration::from_value(0xffffffffu).next().valid());
        }
    }
}

// --- lineage ----------------------------------------------------------------

RL_TEST(property_lineage_stays_acyclic_under_randomized_construction) {
    for (std::uint32_t seed = 1; seed <= kSeeds; ++seed) {
        Rng rng(seed);
        const std::uint32_t operations = 60u + rng.u32(40u);
        std::printf("PROPERTY lineage seed=%u operations=%u\n", seed, operations);
        GeneratedHistory history;
        generate_history(rng, operations, history);

        auto created = Ledger::create(local_config());
        RL_CHECK_OK(created);
        const std::shared_ptr<Ledger>& book = created.value();
        expect(append_all(book, history.drafts), "generated history");
        auto snapshot = book->snapshot();

        // Hypotheses: the ancestry is exactly the recorded parent chain, ending
        // at a hypothesis that has no parent.
        for (const std::uint64_t identity : history.hypothesis_ids) {
            const HypothesisId id = HypothesisId::from_value(identity);
            const std::vector<HypothesisId> ancestry =
                expect(snapshot.hypothesis_ancestry(id), "hypothesis ancestry");
            std::vector<std::uint64_t> expected_chain;
            std::uint64_t cursor = identity;
            while (cursor != 0) {
                expected_chain.push_back(cursor);
                cursor = history.hypothesis_parent[static_cast<std::size_t>(cursor - 1u)];
                RL_CHECK(expected_chain.size() <= history.hypothesis_ids.size());
            }
            RL_CHECK_EQ(ancestry.size(), expected_chain.size());
            for (std::size_t index = 0; index < ancestry.size(); ++index) {
                RL_CHECK_EQ(ancestry[index].value(), expected_chain[index]);
            }
        }

        // Branches: the same property through parent_branch.
        for (const std::uint64_t identity : history.branch_ids) {
            const BranchId id = BranchId::from_value(identity);
            const std::vector<BranchId> ancestry =
                expect(snapshot.branch_ancestry(id), "branch ancestry");
            std::vector<std::uint64_t> expected_chain;
            std::uint64_t cursor = identity;
            while (cursor != 0) {
                expected_chain.push_back(cursor);
                RL_CHECK(expected_chain.size() <= history.branch_ids.size());
                cursor = history.branch_parent[static_cast<std::size_t>(cursor - 1u)];
            }
            RL_CHECK_EQ(ancestry.size(), expected_chain.size());
            for (std::size_t index = 0; index < ancestry.size(); ++index) {
                RL_CHECK_EQ(ancestry[index].value(), expected_chain[index]);
            }
        }

        // Artifacts: the ancestry is the transitive closure of the recorded
        // parents, without the artifact itself and without a repeat.
        for (const std::uint64_t identity : history.artifact_ids) {
            const std::vector<ArtifactView> ancestry =
                expect(snapshot.artifact_ancestry(ArtifactId::from_value(identity)),
                       "artifact ancestry");
            std::vector<std::uint64_t> expected_closure;
            std::vector<std::uint64_t> pending =
                history.artifact_parents[static_cast<std::size_t>(identity - 1u)];
            while (!pending.empty()) {
                const std::uint64_t parent = pending.back();
                pending.pop_back();
                if (std::find(expected_closure.begin(), expected_closure.end(), parent) !=
                    expected_closure.end()) {
                    continue;
                }
                RL_CHECK(parent != identity);
                expected_closure.push_back(parent);
                const std::vector<std::uint64_t>& parents =
                    history.artifact_parents[static_cast<std::size_t>(parent - 1u)];
                pending.insert(pending.end(), parents.begin(), parents.end());
                RL_CHECK(expected_closure.size() <= history.artifact_ids.size());
            }
            RL_CHECK_EQ(ancestry.size(), expected_closure.size());
            for (const ArtifactView& view : ancestry) {
                RL_CHECK(std::find(expected_closure.begin(), expected_closure.end(),
                                   view.artifact.value()) != expected_closure.end());
            }
            for (const std::uint64_t parent : expected_closure) {
                bool found = false;
                for (const ArtifactView& view : ancestry) {
                    if (view.artifact.value() == parent) {
                        found = true;
                    }
                }
                RL_CHECK(found);
            }
        }

        // The ledger's own lineage checks agree.
        const IntegrityReport integrity = expect(snapshot.verify(), "verify");
        RL_CHECK(integrity.ok);
    }
}

// --- terminal attempts ------------------------------------------------------

RL_TEST(property_a_terminal_attempt_never_acquires_a_contradictory_state) {
    for (std::uint32_t seed = 1; seed <= kSeeds; ++seed) {
        Rng rng(seed);
        const std::uint32_t operations = 50u + rng.u32(40u);
        std::printf("PROPERTY terminal-attempts seed=%u operations=%u\n", seed, operations);
        GeneratedHistory history;
        generate_history(rng, operations, history);

        auto created = Ledger::create(local_config());
        RL_CHECK_OK(created);
        const std::shared_ptr<Ledger>& book = created.value();
        expect(append_all(book, history.drafts), "generated history");
        auto snapshot = book->snapshot();

        for (const std::uint64_t identity : history.attempt_ids) {
            const AttemptId id = AttemptId::from_value(identity);
            const AttemptView view = expect(snapshot.attempt(id), "attempt");
            RL_CHECK(view.generation.value() == 1u);
            if (view.state == AttemptState::Running) {
                RL_CHECK(!view.terminated_at.valid());
                continue;
            }
            RL_CHECK(view.terminated_at.valid());
            const ErrorCode completed = book->append(attempt_completed(identity)).code();
            const ErrorCode cancelled =
                book->append(attempt_cancelled(identity, "contradiction")).code();
            if (view.state == AttemptState::Completed) {
                RL_CHECK(completed == ErrorCode::DuplicateCompletion);
                RL_CHECK(cancelled == ErrorCode::AlreadyTerminal);
            } else if (view.state == AttemptState::Failed) {
                RL_CHECK(completed == ErrorCode::AlreadyTerminal);
                RL_CHECK(cancelled == ErrorCode::AlreadyTerminal);
            } else {
                RL_CHECK(view.state == AttemptState::Cancelled);
                RL_CHECK(completed == ErrorCode::Cancelled);
                RL_CHECK(cancelled == ErrorCode::DuplicateCompletion);
            }
            // The refused records changed nothing.
            const AttemptView after = expect(book->snapshot().attempt(id), "attempt");
            RL_CHECK(after.state == view.state);
            RL_CHECK(after.terminated_at == view.terminated_at);
            RL_CHECK_EQ(after.generation.value(), view.generation.value());
        }
    }
}

// --- accounting -------------------------------------------------------------

RL_TEST(property_accounting_never_overflows_and_totals_recompute) {
    std::uint64_t overflows = 0;
    std::uint64_t mismatches = 0;
    for (std::uint32_t seed = 1; seed <= kSeeds; ++seed) {
        Rng rng(seed);
        const std::uint32_t count = 4u + rng.u32(6u);
        std::printf("PROPERTY accounting seed=%u contributions=%u\n", seed, count);
        std::vector<AccountingVector> contributions;
        contributions.reserve(count);
        for (std::uint32_t index = 0; index < count; ++index) {
            contributions.push_back(random_accounting(rng, rng.chance(1u, 2u)));
        }

        SumStatus expected_status = SumStatus::Ok;
        const VectorTotal expected = expected_total(contributions, expected_status);
        if (expected_status == SumStatus::Overflow) {
            overflows += 1;
        }
        if (expected_status == SumStatus::CurrencyMismatch) {
            mismatches += 1;
        }

        // A second implementation of the same rule.
        AccountingVector running = contributions[0];
        SumStatus actual_status = SumStatus::Ok;
        for (std::size_t index = 1; index < contributions.size(); ++index) {
            auto sum = accumulate(running, contributions[index]);
            if (!sum.ok()) {
                actual_status = classify(sum.status());
                break;
            }
            running = sum.take();
        }
        RL_CHECK(actual_status == expected_status);
        if (expected_status == SumStatus::Ok) {
            RL_CHECK(matches(running, expected));
        }

        // The aggregate over the same contributions agrees with both.
        auto aggregate = aggregate_accounting(contributions.data(), contributions.size());
        if (expected_status == SumStatus::Ok) {
            RL_CHECK_OK(aggregate);
            RL_CHECK_EQ(aggregate.value().contributions, count);
            RL_CHECK(aggregate.value().complete ==
                     std::all_of(contributions.begin(), contributions.end(),
                                 [](const AccountingVector& vector) {
                                     return vector.all_known();
                                 }));
            RL_CHECK(matches(aggregate.value().total, expected));
        } else {
            RL_CHECK(!aggregate.ok());
            RL_CHECK(classify(aggregate.status()) == expected_status);
        }

        // The same contributions recorded in the ledger produce the same total.
        auto ledger = ledger_with_attempt(local_config());
        RL_CHECK_OK(ledger);
        const std::shared_ptr<Ledger>& book = ledger.value();
        for (const AccountingVector& vector : contributions) {
            RL_CHECK_OK(book->append(accounting_recorded(SubjectId::of(AttemptId::from_value(1)),
                                                         vector, "property")));
        }
        const auto scope = SubjectId::of(AttemptId::from_value(1));
        auto ledger_total = book->snapshot().accounting(scope);
        if (expected_status == SumStatus::Ok) {
            RL_CHECK_OK(ledger_total);
            RL_CHECK_EQ(ledger_total.value().contributions, count);
            RL_CHECK(matches(ledger_total.value().total, expected));
        } else {
            RL_CHECK(!ledger_total.ok());
            RL_CHECK(classify(ledger_total.status()) == expected_status);
        }
    }
    std::printf("PROPERTY accounting summary: seeds=%u overflow-seeds=%llu mismatch-seeds=%llu\n",
                kSeeds, static_cast<unsigned long long>(overflows),
                static_cast<unsigned long long>(mismatches));
    RL_CHECK(overflows + mismatches > 0u);
}

// --- replay and idempotency -------------------------------------------------

RL_TEST(property_replayed_records_never_double_count) {
    for (std::uint32_t seed = 1; seed <= kSeeds; ++seed) {
        Rng rng(seed);
        const std::uint32_t operations = 40u + rng.u32(40u);
        std::printf("PROPERTY replay seed=%u operations=%u\n", seed, operations);
        GeneratedHistory history;
        generate_history(rng, operations, history);

        auto created = Ledger::create(local_config());
        RL_CHECK_OK(created);
        const std::shared_ptr<Ledger>& book = created.value();
        expect(append_all(book, history.drafts), "generated history");
        const Fingerprint before = fingerprint(book);

        for (unsigned round = 0; round < 5u; ++round) {
            const std::size_t index =
                static_cast<std::size_t>(rng.range(history.drafts.size()));
            RecordDraft replay = history.drafts[index];
            replay.idempotent = true;
            const AppendOutcome outcome = expect(book->append(replay), "idempotent replay");
            RL_CHECK(outcome.duplicate);
            RL_CHECK(outcome.state == CommitState::Duplicate);
            // The replay is recognized as the record that was already committed
            // at this position rather than being committed again.
            RL_CHECK_EQ(outcome.sequence.value(), index + 1u);

            RecordDraft strict = history.drafts[index];
            RL_CHECK_CODE(book->append(strict), ErrorCode::DuplicateRecord);
        }
        RL_CHECK(identical(before, fingerprint(book)));
    }
}

RL_TEST(property_the_same_ledger_reconstructs_the_same_digest) {
    const std::string first_path = temp_path("property-replay-a.bin");
    const std::string second_path = temp_path("property-replay-b.bin");
    std::remove(first_path.c_str());
    std::remove(second_path.c_str());

    for (std::uint32_t seed = 1; seed <= kSeeds; ++seed) {
        Rng rng(seed);
        const std::uint32_t operations = 30u + rng.u32(40u);
        std::printf("PROPERTY logical-digest seed=%u operations=%u\n", seed, operations);
        GeneratedHistory history;
        generate_history(rng, operations, history);

        auto created = Ledger::create(local_config());
        RL_CHECK_OK(created);
        const std::shared_ptr<Ledger>& book = created.value();
        expect(append_all(book, history.drafts), "generated history");
        const Digest live = book->logical_digest();

        require_ok(book->save(first_path), "save");
        auto loaded = Ledger::load(first_path, local_config());
        RL_CHECK_OK(loaded);
        RL_CHECK(loaded.value()->logical_digest() == live);
        RL_CHECK_EQ(loaded.value()->last_sequence().value(), book->last_sequence().value());
        require_ok(loaded.value()->save(second_path), "re-save");
        RL_CHECK(read_bytes(first_path) == read_bytes(second_path));

        const std::vector<Record> records =
            expect(book->snapshot().records(RecordSequence::from_value(1), book->last_sequence()),
                   "records");
        auto replay = replay_committed(records, local_config());
        RL_CHECK_OK(replay);
        RL_CHECK(replay.value().logical_digest == live);
        RL_CHECK_EQ(replay.value().records, records.size());
        RL_CHECK(replay.value().integrity.ok);

        auto restored = Ledger::restore(records, local_config());
        RL_CHECK_OK(restored);
        RL_CHECK(restored.value()->logical_digest() == live);
        // Loading the same image twice reconstructs the same state.
        auto again = Ledger::load(first_path, local_config());
        RL_CHECK_OK(again);
        RL_CHECK(again.value()->logical_digest() == live);
    }
    std::remove(first_path.c_str());
    std::remove(second_path.c_str());
}

// --- corrupt persistence ----------------------------------------------------

RL_TEST(property_a_corrupt_image_never_becomes_valid_state) {
    const std::string good_path = temp_path("property-corrupt-good.bin");
    const std::string bad_path = temp_path("property-corrupt-bad.bin");
    std::remove(good_path.c_str());
    std::remove(bad_path.c_str());

    for (std::uint32_t seed = 1; seed <= kSeeds; ++seed) {
        Rng rng(seed);
        const std::uint32_t operations = 20u + rng.u32(25u);
        std::printf("PROPERTY corrupt-image seed=%u operations=%u\n", seed, operations);
        GeneratedHistory history;
        generate_history(rng, operations, history);

        auto created = Ledger::create(local_config());
        RL_CHECK_OK(created);
        const std::shared_ptr<Ledger>& book = created.value();
        expect(append_all(book, history.drafts), "generated history");
        require_ok(book->save(good_path), "save");

        const std::vector<std::byte> image = read_bytes(good_path);
        RL_CHECK(!image.empty());
        const std::size_t offset = static_cast<std::size_t>(rng.range(image.size()));
        const unsigned bit = 1u << rng.u32(8u);
        std::vector<std::byte> corrupt = image;
        corrupt[offset] = static_cast<std::byte>(std::to_integer<unsigned>(corrupt[offset]) ^ bit);
        RL_CHECK(!(corrupt == image));
        RL_CHECK(write_bytes(bad_path, corrupt));

        // A damaged image is never accepted as state.
        auto decoded = deserialize_snapshot(corrupt, Limits{});
        RL_CHECK(!decoded.ok());
        auto loaded = Ledger::load(bad_path, local_config());
        RL_CHECK(!loaded.ok());
        RL_CHECK(loaded.code() != ErrorCode::Ok);
        auto snapshot_file = load_snapshot_file(bad_path, Limits{});
        RL_CHECK(!snapshot_file.ok());

        // The undamaged image is still exactly what it was.
        auto still_good = Ledger::load(good_path, local_config());
        RL_CHECK_OK(still_good);
        RL_CHECK(still_good.value()->logical_digest() == book->logical_digest());
    }
    std::remove(good_path.c_str());
    std::remove(bad_path.c_str());
}

// --- failed batches ---------------------------------------------------------

RL_TEST(property_a_failed_append_never_partially_mutates_the_ledger) {
    for (std::uint32_t seed = 1; seed <= kSeeds; ++seed) {
        Rng rng(seed);
        const std::uint32_t operations = 40u + rng.u32(40u);
        std::printf("PROPERTY failed-batch seed=%u operations=%u\n", seed, operations);
        GeneratedHistory history;
        generate_history(rng, operations, history);

        auto created = Ledger::create(local_config());
        RL_CHECK_OK(created);
        const std::shared_ptr<Ledger>& book = created.value();
        expect(append_all(book, history.drafts), "generated history");

        for (unsigned round = 0; round < 3u; ++round) {
            const Fingerprint before = fingerprint(book);
            std::vector<RecordDraft> batch;
            const std::size_t valid_count = 1u + static_cast<std::size_t>(rng.u32(3u));
            for (std::size_t index = 0; index < valid_count; ++index) {
                batch.push_back(history.drafts[static_cast<std::size_t>(
                    rng.range(history.drafts.size()))]);
            }
            // One draft in the batch is not committable.
            RecordDraft injected;
            switch (rng.u32(4u)) {
                case 0u:
                    injected = session_opened(7, "no provenance");
                    injected.provenance = Provenance::Unknown;
                    break;
                case 1u:
                    injected = hypothesis_declared(5000, 4242, "session does not exist");
                    break;
                case 2u:
                    injected = hypothesis_declared(history.hypothesis_ids.empty()
                                                       ? 5001
                                                       : history.hypothesis_ids[0],
                                                   1, "already declared");
                    break;
                default:
                    injected = attempt_started(5002, 4242, 1);
                    break;
            }
            const std::size_t position =
                static_cast<std::size_t>(rng.range(batch.size() + 1u));
            batch.insert(batch.begin() + static_cast<std::ptrdiff_t>(position), injected);

            auto outcomes = book->append_batch(batch);
            RL_CHECK(!outcomes.ok());
            RL_CHECK(outcomes.code() != ErrorCode::Ok);
            // Nothing from the failed batch is visible, and nothing else moved.
            RL_CHECK(identical(before, fingerprint(book)));
            RL_CHECK_EQ(book->last_sequence().value(), before.last_sequence);
            const IntegrityReport integrity = expect(book->snapshot().verify(), "verify");
            RL_CHECK(integrity.ok);
        }
    }
}

}  // namespace
}  // namespace research_ledger
