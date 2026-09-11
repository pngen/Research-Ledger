#include "test_framework.hpp"
#include "test_support.hpp"

#include "research_ledger/cluster.hpp"
#include "research_ledger/codec.hpp"
#include "research_ledger/net.hpp"
#include "research_ledger/persistence.hpp"
#include "research_ledger/protocol.hpp"
#include "research_ledger/record_codec.hpp"
#include "research_ledger/replay.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace research_ledger {
namespace {

using namespace research_ledger::test;

// Snapshot layout offsets, as documented in research_ledger/persistence.hpp.
inline constexpr std::size_t kSnapshotFormatOffset = 8;
inline constexpr std::size_t kSnapshotHeaderBytes = 76;

// Frame header offsets, as documented in research_ledger/protocol.hpp.
inline constexpr std::size_t kFrameMagicOffset = 0;
inline constexpr std::size_t kFramePayloadLengthOffset = 10;

#define RL_REQUIRE_STATUS(EXPRESSION)                                                        \
    do {                                                                                     \
        const ::research_ledger::Status rl_status_value = (EXPRESSION);                       \
        ::research_ledger::test::check_message(                                              \
            rl_status_value.ok(),                                                            \
            std::string(#EXPRESSION " failed: ") +                                           \
                std::string(::research_ledger::error_code_name(rl_status_value.code)) +      \
                " (" + rl_status_value.message + ")",                                        \
            __FILE__, __LINE__);                                                             \
    } while (false)

// A Result<T> whose T is move-only cannot be passed through RL_CHECK_OK, which
// copies the result to inspect it. This reports the same way without copying.
#define RL_REQUIRE_RESULT(EXPRESSION)                                                            do {                                                                                             auto& rl_result_ref = (EXPRESSION);                                                           ::research_ledger::test::check_message(                                                          rl_result_ref.ok(),                                                                          std::string(#EXPRESSION " failed: ") +                                                           std::string(::research_ledger::error_code_name(rl_result_ref.code())) +                      " (" + rl_result_ref.message() + ")",                                                    __FILE__, __LINE__);                                                                 } while (false)

// Removes its directory when the test ends, however it ends.
class TempDirectory {
public:
    explicit TempDirectory(const char* name) {
        std::error_code error;
        const std::filesystem::path base = std::filesystem::temp_directory_path(error);
        path_ = error ? std::filesystem::path(name) : base / name;
        std::filesystem::remove_all(path_, error);
        std::filesystem::create_directories(path_, error);
        created_ = std::filesystem::exists(path_);
    }

    ~TempDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    TempDirectory(const TempDirectory&) = delete;
    TempDirectory& operator=(const TempDirectory&) = delete;

    [[nodiscard]] bool created() const noexcept { return created_; }
    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }
    [[nodiscard]] std::string file(const std::string& name) const {
        return (path_ / name).string();
    }

private:
    std::filesystem::path path_{};
    bool created_ = false;
};

// Stops and joins the coordinator's serve thread when the test ends, however it
// ends. A failed check unwinds through this guard instead of terminating the
// process on a joinable thread.
class ServerGuard {
public:
    ServerGuard(Coordinator* coordinator, std::thread* thread) noexcept
        : coordinator_(coordinator), thread_(thread) {}

    ~ServerGuard() {
        if (coordinator_ != nullptr) {
            static_cast<void>(coordinator_->request_shutdown("test scope ended"));
        }
        if (thread_ != nullptr && thread_->joinable()) {
            thread_->join();
        }
    }

    ServerGuard(const ServerGuard&) = delete;
    ServerGuard& operator=(const ServerGuard&) = delete;

private:
    Coordinator* coordinator_ = nullptr;
    std::thread* thread_ = nullptr;
};

void put_u32(std::vector<std::byte>& bytes, std::size_t offset, std::uint32_t value) {
    for (std::size_t index = 0; index < 4; ++index) {
        bytes[offset + index] =
            static_cast<std::byte>((value >> (8u * static_cast<unsigned>(index))) & 0xffu);
    }
}

std::vector<RecordDraft> research_baseline(std::uint64_t session, std::uint64_t hypothesis,
                                           std::uint64_t branch, std::uint64_t experiment,
                                           std::uint64_t attempt) {
    return std::vector<RecordDraft>{
        session_opened(session), hypothesis_declared(hypothesis, session, "h"),
        branch_declared(branch, session),
        experiment_declared(experiment, session, hypothesis, branch),
        attempt_started(attempt, experiment, branch)};
}

Result<std::vector<Record>> all_records(const std::shared_ptr<Ledger>& ledger) {
    const RecordSequence last = ledger->last_sequence();
    if (!last.valid()) {
        return std::vector<Record>{};
    }
    return ledger->snapshot().records(RecordSequence::first(), last);
}

Result<std::vector<std::byte>> image_of(const std::shared_ptr<Ledger>& ledger,
                                        const Limits& limits) {
    auto records = all_records(ledger);
    if (!records.ok()) {
        return records.status();
    }
    return serialize_snapshot(records.value(), ledger->generation(), ledger->epoch(), limits);
}

WorkerConfig worker_config_for(std::uint16_t port, std::uint64_t worker_id) {
    WorkerConfig config;
    config.host = "127.0.0.1";
    config.port = port;
    config.worker = WorkerId::from_value(worker_id);
    config.authority = "adversarial-worker";
    return config;
}

// --- duplicate identities, replays and duplicate commits --------------------

RL_TEST(adversarial_duplicate_identities_and_replays_are_typed) {
    auto created = Ledger::create(local_config());
    RL_CHECK_OK(created);
    const std::shared_ptr<Ledger>& ledger = created.value();

    // The same caller supplied record identity twice inside one batch.
    RecordDraft first = session_opened(1);
    first.record_id = LedgerRecordId::from_value(5001);
    RecordDraft same_identity = session_opened(2);
    same_identity.record_id = LedgerRecordId::from_value(5001);
    RL_CHECK_CODE(ledger->append_batch(std::vector<RecordDraft>{first, same_identity}),
                  ErrorCode::DuplicateRecord);
    RL_CHECK_EQ(ledger->last_sequence().value(), 0u);

    RL_CHECK_OK(ledger->append(first));
    RL_CHECK_EQ(ledger->last_sequence().value(), 1u);

    // A replay of a committed identity is a duplicate, not a second record.
    RL_CHECK_CODE(ledger->append(same_identity), ErrorCode::DuplicateRecord);
    RL_CHECK_EQ(ledger->last_sequence().value(), 1u);

    // The same replay, marked idempotent, is recognized instead.
    same_identity.idempotent = true;
    auto replay = ledger->append(same_identity);
    RL_CHECK_OK(replay);
    RL_CHECK(replay.value().duplicate);
    RL_CHECK(replay.value().state == CommitState::Duplicate);
    RL_CHECK_EQ(replay.value().sequence.value(), 1u);
    RL_CHECK_EQ(ledger->last_sequence().value(), 1u);
    auto recorded = ledger->snapshot().record_at(RecordSequence::from_value(1));
    RL_CHECK_OK(recorded);

    // An idempotent replay inside one batch is recognized as well, and it points
    // at the sequence the first copy received.
    RecordDraft queued = session_opened(3);
    queued.record_id = LedgerRecordId::from_value(5002);
    RecordDraft queued_replay = queued;
    queued_replay.idempotent = true;
    auto batch = ledger->append_batch(std::vector<RecordDraft>{queued, queued_replay});
    RL_CHECK_OK(batch);
    RL_CHECK_EQ(batch.value().size(), 2u);
    RL_CHECK(batch.value()[0].state == CommitState::Committed);
    RL_CHECK(batch.value()[1].duplicate);
    RL_CHECK_EQ(batch.value()[1].sequence.value(), batch.value()[0].sequence.value());
    RL_CHECK_EQ(ledger->last_sequence().value(), 2u);

    // A repeated declaration of an existing entity is a duplicate record even
    // though it carries a fresh record identity.
    RL_CHECK_CODE(ledger->append(session_opened(1)), ErrorCode::DuplicateRecord);
    RL_CHECK_CODE(ledger->append(session_opened(3)), ErrorCode::DuplicateRecord);

    // Two terminal records for the same attempt.
    RL_CHECK_OK(ledger->append_batch(research_baseline(9, 9, 9, 9, 9)));
    RL_CHECK_OK(ledger->append(attempt_completed(9, "first outcome")));
    RL_CHECK_CODE(ledger->append(attempt_completed(9, "second outcome")),
                  ErrorCode::DuplicateCompletion);
    RL_CHECK_CODE(ledger->append(attempt_failed(9, 1)), ErrorCode::AlreadyTerminal);
    RL_CHECK_CODE(ledger->append(attempt_cancelled(9)), ErrorCode::AlreadyTerminal);
    RL_CHECK_CODE(ledger->append(attempt_started(9, 9, 9)), ErrorCode::DuplicateRecord);

    // Duplicate evidence inside one decision.
    auto snapshot = ledger->snapshot();
    auto hypothesis_sequence = snapshot.subject_sequence(SubjectId::of(HypothesisId::from_value(9)));
    RL_CHECK_OK(hypothesis_sequence);
    std::vector<EvidenceRef> repeated{EvidenceRef{SubjectId::of(HypothesisId::from_value(9)),
                                                  hypothesis_sequence.value()},
                                      EvidenceRef{SubjectId::of(HypothesisId::from_value(9)),
                                                  hypothesis_sequence.value()}};
    RL_CHECK_CODE(ledger->append(decision_recorded(1, 9, SubjectId::of(AttemptId::from_value(9)),
                                                   DecisionType::Audit, DecisionOutcome::Recorded,
                                                   repeated)),
                  ErrorCode::InvalidArgument);

    auto integrity = ledger->snapshot().verify();
    RL_CHECK_OK(integrity);
    RL_CHECK(integrity.value().ok);
    RL_CHECK(integrity.value().issues.empty());
}

// --- ordering ---------------------------------------------------------------

RL_TEST(adversarial_out_of_order_references_are_rejected_atomically) {
    auto created = Ledger::create(local_config());
    RL_CHECK_OK(created);
    const std::shared_ptr<Ledger>& ledger = created.value();

    // The first draft of the batch depends on the two declared after it.
    RL_CHECK_CODE(ledger->append_batch(std::vector<RecordDraft>{experiment_declared(1, 1, 1, 1),
                                                                hypothesis_declared(1, 1, "h"),
                                                                branch_declared(1, 1)}),
                  ErrorCode::MissingDependency);
    RL_CHECK_EQ(ledger->last_sequence().value(), 0u);
    auto empty = ledger->snapshot();
    RL_CHECK_EQ(empty.watermark().value(), 0u);
    RL_CHECK_CODE(empty.session(ResearchSessionId::from_value(1)), ErrorCode::NotFound);

    // The same drafts in dependency order commit completely.
    RL_CHECK_OK(ledger->append_batch(
        std::vector<RecordDraft>{session_opened(1), hypothesis_declared(1, 1, "h"),
                                 branch_declared(1, 1), experiment_declared(1, 1, 1, 1)}));
    RL_CHECK_EQ(ledger->last_sequence().value(), 4u);

    // A result that names an artifact declared later in the same batch.
    RL_CHECK_CODE(
        ledger->append_batch(std::vector<RecordDraft>{
            result_declared(1, 1, 1, 1, {ArtifactId::from_value(1)}),
            artifact_referenced(1, 1, SubjectId::of(ExperimentId::from_value(1)),
                                ArtifactRole::ResultArtifact, "result-payload")}),
        ErrorCode::MissingDependency);
    RL_CHECK_EQ(ledger->last_sequence().value(), 4u);

    // Inside one batch a draft may resolve an entity staged earlier in that
    // same batch. That is a dependency order, not an out-of-order arrival.
    RL_CHECK_OK(ledger->append_batch(std::vector<RecordDraft>{
        attempt_started(1, 1, 1),
        artifact_referenced(1, 1, SubjectId::of(AttemptId::from_value(1)),
                            ArtifactRole::Intermediate, "staged-producer")}));
    RL_CHECK_EQ(ledger->last_sequence().value(), 6u);

    // The same shape of batch with the references pointing forward is rejected
    // whole, and it leaves nothing behind.
    RL_CHECK_CODE(
        ledger->append_batch(std::vector<RecordDraft>{
            artifact_referenced(2, 1, SubjectId::of(AttemptId::from_value(1)),
                                ArtifactRole::Intermediate, "forward-parent",
                                {ArtifactId::from_value(3)}),
            artifact_referenced(3, 1, SubjectId::of(AttemptId::from_value(1)),
                                ArtifactRole::Intermediate, "parent")}),
        ErrorCode::MissingDependency);
    RL_CHECK_EQ(ledger->last_sequence().value(), 6u);
    RL_CHECK_CODE(ledger->snapshot().artifact(ArtifactId::from_value(3)), ErrorCode::NotFound);

    // Commit the completion and the result, then attack the decision window:
    // evidence must already be committed.
    RL_CHECK_OK(ledger->append_batch(std::vector<RecordDraft>{
        attempt_completed(1, "outcome"), result_declared(1, 1, 1, 1, {ArtifactId::from_value(1)})}));
    const std::uint64_t last = ledger->last_sequence().value();
    RL_CHECK_EQ(last, 8u);

    auto future = decision_recorded(
        1, 1, SubjectId::of(ResultId::from_value(1)), DecisionType::Audit,
        DecisionOutcome::Recorded,
        {EvidenceRef{SubjectId::of(ResultId::from_value(1)),
                     RecordSequence::from_value(static_cast<std::uint32_t>(last + 4u))}});
    RL_CHECK_CODE(ledger->append(future), ErrorCode::InvalidTransition);

    // Evidence at or after the decision's own sequence is not evidence yet.
    auto at_self = decision_recorded(
        1, 1, SubjectId::of(ResultId::from_value(1)), DecisionType::Audit,
        DecisionOutcome::Recorded,
        {EvidenceRef{SubjectId::of(ResultId::from_value(1)),
                     RecordSequence::from_value(static_cast<std::uint32_t>(last + 1u))}});
    RL_CHECK_CODE(ledger->append(at_self), ErrorCode::InvalidTransition);

    // Evidence with an invalid sequence identity.
    auto zero_sequence = decision_recorded(1, 1, SubjectId::of(ResultId::from_value(1)),
                                           DecisionType::Audit, DecisionOutcome::Recorded,
                                           {EvidenceRef{SubjectId::of(ResultId::from_value(1)),
                                                        RecordSequence{}}});
    RL_CHECK_CODE(ledger->append(zero_sequence), ErrorCode::InvalidIdentity);

    RL_CHECK_EQ(ledger->last_sequence().value(), last);
    auto integrity = ledger->snapshot().verify();
    RL_CHECK_OK(integrity);
    RL_CHECK(integrity.value().ok);
}

// --- authority --------------------------------------------------------------

RL_TEST(adversarial_stale_authority_is_fenced) {
    LedgerConfig config;
    config.require_worker_admission = true;
    auto created = Ledger::create(config);
    RL_CHECK_OK(created);
    const std::shared_ptr<Ledger>& ledger = created.value();

    const WorkerId worker = WorkerId::from_value(9);
    std::uint32_t counter = 1;
    auto first = ledger->admit_worker_incarnation(worker, WorkerBootId{}, counter++);
    RL_CHECK_OK(first);
    RL_CHECK_OK(ledger->append_batch_with_authority(std::vector<RecordDraft>{session_opened(1)},
                                                    first.value()));
    RL_CHECK_EQ(ledger->last_sequence().value(), 1u);

    // A second incarnation of the same worker fences the first.
    auto second = ledger->admit_worker_incarnation(worker, WorkerBootId{}, counter++);
    RL_CHECK_OK(second);
    RL_CHECK(second.value().worker_boot != first.value().worker_boot);
    RL_CHECK(!ledger->worker_is_live(worker, first.value().worker_boot));
    RL_CHECK(ledger->worker_is_live(worker, second.value().worker_boot));
    RL_CHECK_CODE(ledger->append_batch_with_authority(std::vector<RecordDraft>{session_opened(2)},
                                                      first.value()),
                  ErrorCode::StaleWorker);
    RL_CHECK_EQ(ledger->last_sequence().value(), 1u);

    // A forged boot identity for an admitted worker.
    AuthorityEnvelope forged_boot = second.value();
    forged_boot.worker_boot = WorkerBootId::from_value(second.value().worker_boot.value() + 1u);
    RL_CHECK_CODE(ledger->append_batch_with_authority(std::vector<RecordDraft>{session_opened(2)},
                                                      forged_boot),
                  ErrorCode::StaleWorker);

    // A worker that was never admitted.
    AuthorityEnvelope stranger = second.value();
    stranger.worker = WorkerId::from_value(4242);
    RL_CHECK_CODE(ledger->append_batch_with_authority(std::vector<RecordDraft>{session_opened(2)},
                                                      stranger),
                  ErrorCode::Unauthorized);

    // An incomplete authority envelope is not authority at all.
    AuthorityEnvelope incomplete = second.value();
    incomplete.worker_boot = WorkerBootId{};
    RL_CHECK_CODE(ledger->append_batch_with_authority(std::vector<RecordDraft>{session_opened(2)},
                                                      incomplete),
                  ErrorCode::InvalidIdentity);
    AuthorityEnvelope no_worker = second.value();
    no_worker.worker = WorkerId{};
    RL_CHECK_CODE(ledger->append_batch_with_authority(std::vector<RecordDraft>{session_opened(2)},
                                                      no_worker),
                  ErrorCode::InvalidIdentity);

    // Another ledger generation is a different history, not an older one.
    AuthorityEnvelope other_generation = second.value();
    other_generation.ledger = LedgerGeneration::from_value(2);
    RL_CHECK_CODE(ledger->append_batch_with_authority(std::vector<RecordDraft>{session_opened(2)},
                                                      other_generation),
                  ErrorCode::StaleGeneration);

    // A new coordinator epoch fences every live incarnation.
    auto advanced = ledger->advance_epoch();
    RL_CHECK_OK(advanced);
    RL_CHECK(advanced.value().value() > second.value().epoch.value());
    RL_CHECK(!ledger->worker_is_live(worker, second.value().worker_boot));
    RL_CHECK_CODE(ledger->append_batch_with_authority(std::vector<RecordDraft>{session_opened(2)},
                                                      second.value()),
                  ErrorCode::StaleEpoch);

    // After the epoch change the prior incarnation is not live, so a reconnect
    // is admitted as a fresh incarnation at the new epoch, never as the old one.
    auto refreshed = ledger->admit_worker_incarnation(worker, second.value().worker_boot, counter++);
    RL_CHECK_OK(refreshed);
    RL_CHECK(refreshed.value().worker_boot != second.value().worker_boot);
    RL_CHECK(refreshed.value().epoch == advanced.value());
    RL_CHECK_OK(ledger->append_batch_with_authority(std::vector<RecordDraft>{session_opened(2)},
                                                    refreshed.value()));
    RL_CHECK_EQ(ledger->last_sequence().value(), 2u);

    auto integrity = ledger->snapshot().verify();
    RL_CHECK_OK(integrity);
    RL_CHECK(integrity.value().ok);
    RL_CHECK_OK(ledger->snapshot().session(ResearchSessionId::from_value(1)));
    RL_CHECK_OK(ledger->snapshot().session(ResearchSessionId::from_value(2)));
}

// --- attempt lifecycle ------------------------------------------------------

RL_TEST(adversarial_attempt_lifecycle_attacks) {
    auto created = Ledger::create(local_config());
    RL_CHECK_OK(created);
    const std::shared_ptr<Ledger>& ledger = created.value();
    RL_CHECK_OK(ledger->append_batch(research_baseline(1, 1, 1, 1, 1)));
    RL_CHECK_OK(ledger->append(attempt_started(2, 1, 1)));
    RL_CHECK_OK(ledger->append(attempt_started(3, 1, 1)));
    RL_CHECK_OK(ledger->append(attempt_started(4, 1, 1)));
    const std::uint64_t committed_setup = ledger->last_sequence().value();

    // Attempt 1 reaches a terminal state once.
    RL_CHECK_OK(ledger->append(attempt_completed(1, "outcome")));
    RL_CHECK_CODE(ledger->append(attempt_cancelled(1, "too late")), ErrorCode::AlreadyTerminal);
    RL_CHECK_CODE(ledger->append(attempt_failed(1, 1)), ErrorCode::AlreadyTerminal);

    // A stale or absent attempt generation never terminates a live attempt.
    RL_CHECK_CODE(ledger->append(attempt_failed(2, 1)), ErrorCode::MissingDependency);
    AttemptCompleted future_generation = std::get<AttemptCompleted>(attempt_completed(2).body);
    future_generation.generation = AttemptGeneration::from_value(2);
    RL_CHECK_CODE(ledger->append(draft_of(future_generation)), ErrorCode::StaleAttempt);
    AttemptCompleted zero_generation = std::get<AttemptCompleted>(attempt_completed(2).body);
    zero_generation.generation = AttemptGeneration{};
    // Generation zero is not a stale generation, it is not an identity at all:
    // identity validity is checked before the committed state is consulted.
    RL_CHECK_CODE(ledger->append(draft_of(zero_generation)), ErrorCode::InvalidIdentity);
    auto still_running = ledger->snapshot().attempt(AttemptId::from_value(2));
    RL_CHECK_OK(still_running);
    RL_CHECK(still_running.value().state == AttemptState::Running);

    // A cancelled attempt can never become successful, and it acquires no new
    // authoritative activity.
    RL_CHECK_OK(ledger->append(attempt_cancelled(2, "operator cancelled")));
    RL_CHECK_CODE(ledger->append(attempt_completed(2, "late success")), ErrorCode::Cancelled);
    RL_CHECK_CODE(ledger->append(attempt_failed(2, 1)), ErrorCode::Cancelled);
    RL_CHECK_CODE(ledger->append(model_call_recorded(1, 2)), ErrorCode::Cancelled);
    RL_CHECK_CODE(ledger->append(tool_call_recorded(1, 2)), ErrorCode::Cancelled);
    RL_CHECK_CODE(ledger->append(observation_recorded(1, 2, "accuracy",
                                                      MetricValue::ratio(0.5).value())),
                  ErrorCode::Cancelled);

    // A late failure after the attempt was superseded by a newer generation of
    // the same attempt is refused.
    RL_CHECK_OK(ledger->append(failure_recorded(1, SubjectId::of(AttemptId::from_value(3)),
                                                FailureCategory::Execution, "worker died")));
    RL_CHECK_OK(ledger->append(attempt_failed(3, 1)));
    RL_CHECK_CODE(ledger->append(attempt_completed(3, "late success")), ErrorCode::AlreadyTerminal);
    RL_CHECK_CODE(ledger->append(attempt_cancelled(3, "late cancellation")),
                  ErrorCode::AlreadyTerminal);

    // Two competing terminal events inside one batch: the batch is rejected
    // whole, so the attempt is left running rather than half terminated.
    const std::uint64_t before_competing = ledger->last_sequence().value();
    RL_CHECK_CODE(ledger->append_batch(std::vector<RecordDraft>{attempt_completed(4, "done"),
                                                                attempt_cancelled(4, "cancelled")}),
                  ErrorCode::AlreadyTerminal);
    RL_CHECK_EQ(ledger->last_sequence().value(), before_competing);
    auto attempt = ledger->snapshot().attempt(AttemptId::from_value(4));
    RL_CHECK_OK(attempt);
    RL_CHECK(attempt.value().state == AttemptState::Running);

    // The same batch in the reverse order is equally rejected: a cancellation
    // staged earlier in the batch cannot be overwritten by a later completion.
    RL_CHECK_CODE(ledger->append_batch(std::vector<RecordDraft>{attempt_cancelled(4, "cancelled"),
                                                                attempt_completed(4, "done")}),
                  ErrorCode::Cancelled);
    RL_CHECK_EQ(ledger->last_sequence().value(), before_competing);
    RL_CHECK(ledger->last_sequence().value() > committed_setup);

    // A failure that concerns another attempt cannot terminate this one.
    RL_CHECK_OK(ledger->append(failure_recorded(2, SubjectId::of(AttemptId::from_value(4)),
                                                FailureCategory::Execution, "worker died")));
    RL_CHECK_CODE(ledger->append(attempt_failed(4, 1)), ErrorCode::InvalidTransition);
    RL_CHECK_OK(ledger->append(attempt_failed(4, 2)));
    auto failed = ledger->snapshot().attempt(AttemptId::from_value(4));
    RL_CHECK_OK(failed);
    RL_CHECK(failed.value().state == AttemptState::Failed);
    RL_CHECK(failed.value().failure.has_value());

    auto integrity = ledger->snapshot().verify();
    RL_CHECK_OK(integrity);
    RL_CHECK(integrity.value().ok);
}

// --- results and decisions --------------------------------------------------

RL_TEST(adversarial_result_and_decision_attacks) {
    auto created = Ledger::create(local_config());
    RL_CHECK_OK(created);
    const std::shared_ptr<Ledger>& ledger = created.value();
    RL_CHECK_OK(ledger->append_batch(research_baseline(1, 1, 1, 1, 1)));
    RL_CHECK_OK(ledger->append_batch(std::vector<RecordDraft>{
        artifact_referenced(1, 1, SubjectId::of(ExperimentId::from_value(1)),
                            ArtifactRole::ResultArtifact, "first-result"),
        artifact_referenced(2, 1, SubjectId::of(ExperimentId::from_value(1)),
                            ArtifactRole::ResultArtifact, "second-result"),
        attempt_completed(1, "outcome"),
        result_declared(1, 1, 1, 1, {ArtifactId::from_value(1)}),
        result_declared(2, 1, 1, 1, {ArtifactId::from_value(2)})}));

    // A status change requires a committed decision that concerns the result.
    RL_CHECK_CODE(ledger->append(result_status_changed(1, 2, ResultStatus::Candidate,
                                                       ResultStatus::Accepted, 99)),
                  ErrorCode::MissingDependency);
    RL_CHECK_CODE(ledger->append(result_status_changed(1, 5, ResultStatus::Candidate,
                                                       ResultStatus::Accepted, 1)),
                  ErrorCode::StaleGeneration);
    RL_CHECK_CODE(ledger->append(result_status_changed(1, 2, ResultStatus::Rejected,
                                                       ResultStatus::Accepted, 1)),
                  ErrorCode::InvalidTransition);

    auto snapshot = ledger->snapshot();
    auto experiment_sequence = snapshot.subject_sequence(SubjectId::of(ExperimentId::from_value(1)));
    RL_CHECK_OK(experiment_sequence);

    // Contradictory decision type and outcome pair.
    RL_CHECK_CODE(ledger->append(decision_recorded(1, 1, SubjectId::of(ResultId::from_value(1)),
                                                   DecisionType::Accept,
                                                   DecisionOutcome::Rejected)),
                  ErrorCode::InvalidArgument);

    // Missing supporting evidence: evidence that is not committed, that concerns
    // another subject, or that belongs to another session.
    RL_CHECK_CODE(
        ledger->append(decision_recorded(
            1, 1, SubjectId::of(ResultId::from_value(1)), DecisionType::Audit,
            DecisionOutcome::Recorded,
            {EvidenceRef{SubjectId::of(ExperimentId::from_value(77)), experiment_sequence.value()}})),
        ErrorCode::BrokenLineage);
    RL_CHECK_CODE(
        ledger->append(decision_recorded(
            1, 1, SubjectId::of(ResultId::from_value(1)), DecisionType::Audit,
            DecisionOutcome::Recorded,
            {EvidenceRef{SubjectId::of(ExperimentId::from_value(9)), experiment_sequence.value()}})),
        ErrorCode::BrokenLineage);
    RL_CHECK_CODE(
        ledger->append(decision_recorded(
            1, 1, SubjectId::of(ResultId::from_value(1)), DecisionType::Audit,
            DecisionOutcome::Recorded,
            {EvidenceRef{SubjectId::of(AttemptId::from_value(1)), experiment_sequence.value()}})),
        ErrorCode::BrokenLineage);
    RL_CHECK_CODE(
        ledger->append(decision_recorded(
            1, 1, SubjectId::of(ResultId::from_value(1)), DecisionType::Audit,
            DecisionOutcome::Recorded,
            {EvidenceRef{SubjectId::of(ExperimentId::from_value(1)),
                         RecordSequence::from_value(0)}})),
        ErrorCode::InvalidIdentity);

    // A committed acceptance, with evidence.
    RL_CHECK_OK(ledger->append(decision_recorded(
        1, 1, SubjectId::of(ResultId::from_value(1)), DecisionType::Accept,
        DecisionOutcome::Accepted,
        {EvidenceRef{SubjectId::of(ExperimentId::from_value(1)), experiment_sequence.value()}})));
    RL_CHECK_OK(ledger->append(result_status_changed(1, 2, ResultStatus::Candidate,
                                                     ResultStatus::Accepted, 1)));
    auto accepted = ledger->snapshot().result(ResultId::from_value(1));
    RL_CHECK_OK(accepted);
    RL_CHECK(accepted.value().status == ResultStatus::Accepted);
    RL_CHECK(accepted.value().acceptance_decision.has_value());

    // A contradictory rejection after acceptance needs a decision the model
    // permits, and a direct acceptance to rejection transition is not permitted.
    RL_CHECK_OK(ledger->append(decision_recorded(2, 1, SubjectId::of(ResultId::from_value(1)),
                                                 DecisionType::Reject,
                                                 DecisionOutcome::Rejected)));
    RL_CHECK_CODE(ledger->append(result_status_changed(1, 3, ResultStatus::Accepted,
                                                       ResultStatus::Rejected, 2)),
                  ErrorCode::InvalidTransition);
    auto unchanged = ledger->snapshot().result(ResultId::from_value(1));
    RL_CHECK_OK(unchanged);
    RL_CHECK(unchanged.value().status == ResultStatus::Accepted);

    // An audit decision never authorizes a result status: it observes, it does
    // not rule.
    RL_CHECK_OK(ledger->append(decision_recorded(3, 1, SubjectId::of(ResultId::from_value(2)),
                                                 DecisionType::Audit,
                                                 DecisionOutcome::Recorded)));
    RL_CHECK_CODE(ledger->append(result_status_changed(2, 2, ResultStatus::Candidate,
                                                       ResultStatus::Accepted, 3)),
                  ErrorCode::InvalidTransition);
    auto still_candidate = ledger->snapshot().result(ResultId::from_value(2));
    RL_CHECK_OK(still_candidate);
    RL_CHECK(still_candidate.value().status == ResultStatus::Candidate);

    // An acceptance that cites no evidence is history, not reconstruction: the
    // ledger records the decision and never invents the evidence for it.
    RL_CHECK_OK(ledger->append(decision_recorded(4, 1, SubjectId::of(ResultId::from_value(2)),
                                                 DecisionType::Accept,
                                                 DecisionOutcome::Accepted)));
    RL_CHECK_OK(ledger->append(result_status_changed(2, 2, ResultStatus::Candidate,
                                                     ResultStatus::Accepted, 4)));
    auto bare = ledger->snapshot().supporting_evidence(ResultId::from_value(2));
    RL_CHECK_OK(bare);
    RL_CHECK(bare.value().accepted());
    RL_CHECK(bare.value().cited_evidence.empty());
    RL_CHECK(!bare.value().decisions.empty());
    RL_CHECK(bare.value().reconstructable);

    // A decision that concerns another session is not a decision about this one.
    RL_CHECK_OK(ledger->append_batch(research_baseline(2, 2, 2, 2, 2)));
    auto other_sequence = ledger->snapshot().subject_sequence(SubjectId::of(HypothesisId::from_value(2)));
    RL_CHECK_OK(other_sequence);
    RL_CHECK_CODE(ledger->append(decision_recorded(
                      5, 1, SubjectId::of(ResultId::from_value(1)), DecisionType::Audit,
                      DecisionOutcome::Recorded,
                      {EvidenceRef{SubjectId::of(HypothesisId::from_value(2)),
                                   other_sequence.value()}})),
                  ErrorCode::CrossSessionReference);
    RL_CHECK_CODE(ledger->append(decision_recorded(6, 1, SubjectId::of(AttemptId::from_value(2)),
                                                   DecisionType::Audit,
                                                   DecisionOutcome::Recorded)),
                  ErrorCode::CrossSessionReference);

    auto integrity = ledger->snapshot().verify();
    RL_CHECK_OK(integrity);
    RL_CHECK(integrity.value().ok);
    RL_CHECK(integrity.value().issues.empty());
}

// --- lineage ----------------------------------------------------------------

RL_TEST(adversarial_lineage_and_artifact_attacks) {
    auto created = Ledger::create(local_config());
    RL_CHECK_OK(created);
    const std::shared_ptr<Ledger>& ledger = created.value();
    RL_CHECK_OK(ledger->append_batch(research_baseline(1, 1, 1, 1, 1)));
    RL_CHECK_OK(ledger->append_batch(research_baseline(2, 2, 2, 2, 2)));
    const SubjectId producer = SubjectId::of(AttemptId::from_value(1));
    RL_CHECK_OK(ledger->append(
        artifact_referenced(1, 1, producer, ArtifactRole::Dataset, "dataset")));
    RL_CHECK_OK(ledger->append(artifact_referenced(2, 1, producer, ArtifactRole::Intermediate,
                                                   "derived", {ArtifactId::from_value(1)})));
    RL_CHECK_OK(ledger->append(artifact_referenced(9, 2,
                                                   SubjectId::of(ExperimentId::from_value(2)),
                                                   ArtifactRole::Dataset, "other-session")));
    const std::uint64_t committed = ledger->last_sequence().value();

    // An artifact cannot be its own parent, nor repeat a parent.
    RL_CHECK_CODE(ledger->append(artifact_referenced(3, 1, producer, ArtifactRole::Other, "cycle",
                                                     {ArtifactId::from_value(3)})),
                  ErrorCode::LineageCycle);
    RL_CHECK_CODE(ledger->append(artifact_referenced(
                      3, 1, producer, ArtifactRole::Other, "repeated",
                      {ArtifactId::from_value(1), ArtifactId::from_value(1)})),
                  ErrorCode::InvalidArgument);
    // A broken ancestral edge is a missing dependency, not a silent gap.
    RL_CHECK_CODE(ledger->append(artifact_referenced(3, 1, producer, ArtifactRole::Other, "orphan",
                                                     {ArtifactId::from_value(99)})),
                  ErrorCode::MissingDependency);
    // A producer that is not committed, or not a producer kind at all.
    RL_CHECK_CODE(ledger->append(artifact_referenced(3, 1, SubjectId::of(AttemptId::from_value(99)),
                                                     ArtifactRole::Other, "unknown producer")),
                  ErrorCode::MissingDependency);
    RL_CHECK_CODE(ledger->append(artifact_referenced(3, 1,
                                                     SubjectId::of(HypothesisId::from_value(1)),
                                                     ArtifactRole::Other, "bad producer kind")),
                  ErrorCode::InvalidArgument);
    RL_CHECK_CODE(ledger->append(artifact_referenced(3, 1, producer, ArtifactRole::Invalid,
                                                     "no role")),
                  ErrorCode::InvalidArgument);
    ArtifactReferenced no_digest = std::get<ArtifactReferenced>(
        artifact_referenced(3, 1, producer, ArtifactRole::Other, "no digest").body);
    no_digest.content_digest = Digest{};
    RL_CHECK_CODE(ledger->append(draft_of(no_digest, Provenance::Reported)),
                  ErrorCode::InvalidArgument);
    // Cross session lineage: an artifact cannot borrow another session's parent.
    RL_CHECK_CODE(ledger->append(artifact_referenced(3, 1, producer, ArtifactRole::Other, "mixed",
                                                     {ArtifactId::from_value(9)})),
                  ErrorCode::CrossSessionReference);
    RL_CHECK_CODE(ledger->append(artifact_referenced(3, 1,
                                                     SubjectId::of(AttemptId::from_value(2)),
                                                     ArtifactRole::Other, "foreign producer")),
                  ErrorCode::CrossSessionReference);
    // A revision of an existing artifact must be the next generation.
    ArtifactReferenced skipped = std::get<ArtifactReferenced>(
        artifact_referenced(1, 1, producer, ArtifactRole::Other, "skipped generation").body);
    skipped.generation = ArtifactGeneration::from_value(5);
    RL_CHECK_CODE(ledger->append(draft_of(skipped, Provenance::Reported)),
                  ErrorCode::StaleGeneration);
    ArtifactReferenced zero = std::get<ArtifactReferenced>(
        artifact_referenced(3, 1, producer, ArtifactRole::Other, "zero generation").body);
    zero.generation = ArtifactGeneration{};
    RL_CHECK_CODE(ledger->append(draft_of(zero, Provenance::Reported)),
                  ErrorCode::InvalidIdentity);

    // Hypothesis lineage: missing parent, stale parent generation, cross session.
    HypothesisDeclared orphan = std::get<HypothesisDeclared>(
        hypothesis_declared(3, 1, "orphan").body);
    orphan.parent = HypothesisId::from_value(77);
    orphan.parent_generation = HypothesisGeneration::first();
    RL_CHECK_CODE(ledger->append(draft_of(orphan, Provenance::Reported)),
                  ErrorCode::MissingDependency);
    HypothesisDeclared stale_parent = std::get<HypothesisDeclared>(
        hypothesis_declared(3, 1, "stale parent").body);
    stale_parent.parent = HypothesisId::from_value(1);
    stale_parent.parent_generation = HypothesisGeneration::from_value(4);
    RL_CHECK_CODE(ledger->append(draft_of(stale_parent, Provenance::Reported)),
                  ErrorCode::StaleGeneration);
    HypothesisDeclared foreign_parent = std::get<HypothesisDeclared>(
        hypothesis_declared(3, 1, "foreign parent").body);
    foreign_parent.parent = HypothesisId::from_value(2);
    foreign_parent.parent_generation = HypothesisGeneration::first();
    RL_CHECK_CODE(ledger->append(draft_of(foreign_parent, Provenance::Reported)),
                  ErrorCode::CrossSessionReference);
    // A hypothesis declaration is always its first generation.
    HypothesisDeclared revision = std::get<HypothesisDeclared>(
        hypothesis_declared(3, 1, "revision").body);
    revision.generation = HypothesisGeneration::from_value(2);
    RL_CHECK_CODE(ledger->append(draft_of(revision, Provenance::Reported)),
                  ErrorCode::StaleGeneration);

    // Branch lineage.
    RL_CHECK_CODE(ledger->append(branch_declared(3, 1, BranchKind::Retry, 3)),
                  ErrorCode::LineageCycle);
    RL_CHECK_CODE(ledger->append(branch_declared(3, 1, BranchKind::Retry, 77)),
                  ErrorCode::MissingDependency);
    RL_CHECK_CODE(ledger->append(branch_declared(3, 1, BranchKind::Retry, 2)),
                  ErrorCode::CrossSessionReference);
    RL_CHECK_CODE(ledger->append(branch_declared(3, 1, BranchKind::Root, 1)),
                  ErrorCode::InvalidArgument);
    RL_CHECK_CODE(ledger->append(branch_declared(3, 1, BranchKind::Retry)),
                  ErrorCode::InvalidArgument);

    // Experiment lineage: a hypothesis from another session.
    RL_CHECK_CODE(ledger->append(experiment_declared(3, 1, 2, 1)), ErrorCode::CrossSessionReference);
    RL_CHECK_CODE(ledger->append(experiment_declared(3, 1, 1, 2)), ErrorCode::CrossSessionReference);
    RL_CHECK_CODE(ledger->append(experiment_declared(3, 1, 1, 1, 2)),
                  ErrorCode::CrossSessionReference);

    // The history is exactly what it was before the attacks.
    RL_CHECK_EQ(ledger->last_sequence().value(), committed);
    auto integrity = ledger->snapshot().verify();
    RL_CHECK_OK(integrity);
    RL_CHECK(integrity.value().ok);
    RL_CHECK(integrity.value().issues.empty());
}

// --- provenance, units and extreme values -----------------------------------

RL_TEST(adversarial_provenance_units_and_values) {
    auto created = Ledger::create(local_config());
    RL_CHECK_OK(created);
    const std::shared_ptr<Ledger>& ledger = created.value();
    RL_CHECK_OK(ledger->append_batch(research_baseline(1, 1, 1, 1, 1)));

    // Provenance is stated, never assumed.
    RecordDraft unknown = session_opened(2);
    unknown.provenance = Provenance::Unknown;
    RL_CHECK_CODE(ledger->append(unknown), ErrorCode::InvalidArgument);
    Limits limits;
    // The canonical encoding will not even carry an unstated provenance: a
    // draft that cannot be committed is rejected at the codec boundary too.
    auto encoded_unknown = encode_draft(unknown, limits);
    RL_CHECK_OK(encoded_unknown);
    RL_CHECK_CODE(decode_draft(encoded_unknown.value(), limits), ErrorCode::ProtocolError);
    unknown.provenance = Provenance::Measured;
    auto encoded_stated = encode_draft(unknown, limits);
    RL_CHECK_OK(encoded_stated);
    auto decoded_stated = decode_draft(encoded_stated.value(), limits);
    RL_CHECK_OK(decoded_stated);
    RL_CHECK(decoded_stated.value().provenance == Provenance::Measured);
    RL_CHECK_EQ(ledger->last_sequence().value(), 5u);

    // A declared unit accepts only compatible value kinds.
    RL_CHECK_CODE(ledger->append(metric_declared(1, 1, "latency", UnitKind::Bytes,
                                                 MetricValueKind::Ratio)),
                  ErrorCode::InvalidArgument);
    RL_CHECK_CODE(ledger->append(metric_declared(1, 1, "mystery", UnitKind::None,
                                                 MetricValueKind::Count)),
                  ErrorCode::InvalidArgument);
    RL_CHECK_CODE(ledger->append(metric_declared(1, 1, "mystery", UnitKind::Count,
                                                 MetricValueKind::Unknown)),
                  ErrorCode::InvalidArgument);
    RL_CHECK_OK(ledger->append(
        metric_declared(1, 1, "accuracy", UnitKind::Ratio, MetricValueKind::Ratio)));
    RL_CHECK_OK(ledger->append(
        metric_declared(2, 1, "samples", UnitKind::Count, MetricValueKind::Boolean)));

    // An observation under a unit that does not accept its value.
    RL_CHECK_CODE(ledger->append(observation_recorded(1, 1, "latency",
                                                      MetricValue::bytes(4096).value(),
                                                      UnitKind::Nanoseconds)),
                  ErrorCode::InvalidArgument);
    // The observation value kind must match the declared metric kind exactly.
    ObservationRecorded mismatch = std::get<ObservationRecorded>(
        observation_recorded(1, 1, "samples", MetricValue::count(3).value(), UnitKind::Count).body);
    mismatch.metric = MetricId::from_value(2);
    RL_CHECK_CODE(ledger->append(draft_of(mismatch, Provenance::Measured)),
                  ErrorCode::InvalidArgument);
    // ... and the value must be compatible with that metric's unit.
    ObservationRecorded compatible = std::get<ObservationRecorded>(
        observation_recorded(1, 1, "accuracy", MetricValue::ratio(0.91).value(),
                             UnitKind::Ratio)
            .body);
    compatible.metric = MetricId::from_value(1);
    RL_CHECK_OK(ledger->append(draft_of(compatible, Provenance::Measured)));
    // A metric from another session is not this observation's metric.
    ObservationRecorded foreign = compatible;
    foreign.observation = ObservationId::from_value(2);
    foreign.metric = MetricId::from_value(3);
    RL_CHECK_CODE(ledger->append(draft_of(foreign, Provenance::Measured)),
                  ErrorCode::MissingDependency);
    // An observation with neither a metric nor a key states nothing.
    ObservationRecorded anonymous = std::get<ObservationRecorded>(
        observation_recorded(3, 1, "", MetricValue::ratio(0.5).value(), UnitKind::Ratio).body);
    RL_CHECK_CODE(ledger->append(draft_of(anonymous, Provenance::Measured)),
                  ErrorCode::InvalidArgument);

    // Zero identities and generations are never valid.
    HypothesisDeclared zero_generation =
        std::get<HypothesisDeclared>(hypothesis_declared(3, 1, "zero generation").body);
    zero_generation.generation = HypothesisGeneration{};
    RL_CHECK_CODE(ledger->append(draft_of(zero_generation, Provenance::Reported)),
                  ErrorCode::InvalidIdentity);
    ResultDeclared zero_result =
        std::get<ResultDeclared>(result_declared(1, 1, 1, 1, {}).body);
    zero_result.generation = ResultGeneration{};
    RL_CHECK_CODE(ledger->append(draft_of(zero_result, Provenance::Derived)),
                  ErrorCode::InvalidIdentity);
    RecordDraft zero_identity = session_opened(0);
    RL_CHECK_CODE(ledger->append(zero_identity), ErrorCode::InvalidIdentity);

    // A committed record with sequence zero is not a record.
    auto snapshot = ledger->snapshot();
    auto record = snapshot.record_at(RecordSequence::from_value(1));
    RL_CHECK_OK(record);
    auto encoded_record = encode_record(record.value(), limits);
    RL_CHECK_OK(encoded_record);
    {
        std::vector<std::byte> broken = encoded_record.value();
        put_u32(broken, 1u, 0u);
        RL_CHECK_CODE(decode_record(broken, limits), ErrorCode::PersistenceCorrupt);
    }
    {
        // A zero ledger generation in a committed record is rejected as well.
        std::vector<std::byte> broken = encoded_record.value();
        put_u32(broken, 17u, 0u);
        RL_CHECK_CODE(decode_record(broken, limits), ErrorCode::PersistenceCorrupt);
    }
    {
        std::vector<std::byte> broken = encoded_record.value();
        put_u32(broken, 22u, 0u);
        RL_CHECK_CODE(decode_record(broken, limits), ErrorCode::PersistenceCorrupt);
    }
    RL_CHECK_OK(decode_record(encoded_record.value(), limits));

    // An absurd element count inside a decoded payload is refused before the
    // count can be used to reserve anything.
    {
        ByteWriter writer;
        writer.u16(static_cast<std::uint16_t>(RecordType::SessionOpened));
        write_identity(writer, ResearchSessionId::from_value(1));
        writer.string("label", limits.max_label_length);
        writer.string("question", limits.max_string_length);
        writer.u32(0xffffffffu);
        auto payload = writer.take();
        RL_CHECK_CODE(decode_record_body(payload, limits), ErrorCode::LimitExceeded);
    }
    // A declared string length beyond the limit is refused before the read.
    {
        ByteWriter writer;
        writer.u32(0xffffffffu);
        auto payload = writer.take();
        ByteReader reader(payload, limits, ErrorCode::ProtocolError);
        RL_CHECK_CODE(reader.string(limits.max_string_length), ErrorCode::LimitExceeded);
    }

    auto integrity = ledger->snapshot().verify();
    RL_CHECK_OK(integrity);
    RL_CHECK(integrity.value().ok);
}

// --- accounting -------------------------------------------------------------

RL_TEST(adversarial_accounting_overflow_and_extremes) {
    const std::uint64_t maximum = std::numeric_limits<std::uint64_t>::max();
    const std::int64_t maximum_money = std::numeric_limits<std::int64_t>::max();
    const std::int64_t minimum_money = std::numeric_limits<std::int64_t>::min();

    AccountingVector total;
    total.model_input_tokens = InputTokens::known(maximum, Provenance::Measured);
    AccountingVector addition;
    addition.model_input_tokens = InputTokens::known(1, Provenance::Measured);

    RL_CHECK_CODE(accumulate(total, addition), ErrorCode::AccountingOverflow);
    RL_CHECK_CODE(accumulate(addition, total), ErrorCode::AccountingOverflow);

    AccountingVector zero_addition;
    zero_addition.model_input_tokens = InputTokens::known(0, Provenance::Measured);
    auto saturated = accumulate(total, zero_addition);
    RL_CHECK_OK(saturated);
    RL_CHECK(saturated.value().model_input_tokens.is_known());
    RL_CHECK_EQ(saturated.value().model_input_tokens.units(), maximum);

    // Monetary accumulation is exact and checked in both directions.
    AccountingVector money;
    money.monetary = MonetaryMeasure::known_amount(maximum_money, "USD", Provenance::Reported);
    AccountingVector more_money;
    more_money.monetary = MonetaryMeasure::known_amount(1, "USD", Provenance::Reported);
    RL_CHECK_CODE(accumulate(money, more_money), ErrorCode::AccountingOverflow);

    AccountingVector negative_money;
    negative_money.monetary =
        MonetaryMeasure::known_amount(minimum_money, "USD", Provenance::Reported);
    AccountingVector less_money;
    less_money.monetary = MonetaryMeasure::known_amount(-1, "USD", Provenance::Reported);
    RL_CHECK_CODE(accumulate(negative_money, less_money), ErrorCode::AccountingOverflow);

    // Different currencies never silently become one number.
    AccountingVector dollars;
    dollars.monetary = MonetaryMeasure::known_amount(100, "USD", Provenance::Reported);
    AccountingVector euros;
    euros.monetary = MonetaryMeasure::known_amount(100, "EUR", Provenance::Reported);
    RL_CHECK_CODE(accumulate(dollars, euros), ErrorCode::Unsupported);

    // Unknown never decays into zero, and a known zero is a stated fact.
    AccountingVector unknown_total;
    unknown_total.model_input_tokens = InputTokens::unknown(Provenance::Measured);
    auto mixed = accumulate(unknown_total, zero_addition);
    RL_CHECK_OK(mixed);
    RL_CHECK(!mixed.value().model_input_tokens.is_known());
    RL_CHECK(!mixed.value().model_input_tokens.is_zero());
    RL_CHECK(zero_addition.model_input_tokens.is_known());
    RL_CHECK(zero_addition.model_input_tokens.is_zero());

    const AccountingVector contributions[1] = {zero_addition};
    auto aggregate = aggregate_accounting(contributions, 0);
    RL_CHECK_OK(aggregate);
    RL_CHECK(aggregate.value().empty);
    RL_CHECK_EQ(aggregate.value().contributions, 0u);

    // Ledger side: an accounting record must state something, and money must
    // name its currency.
    auto created = Ledger::create(local_config());
    RL_CHECK_OK(created);
    const std::shared_ptr<Ledger>& ledger = created.value();
    RL_CHECK_OK(ledger->append_batch(research_baseline(1, 1, 1, 1, 1)));
    RL_CHECK_CODE(ledger->append(accounting_recorded(SubjectId::of(AttemptId::from_value(1)),
                                                     AccountingVector{})),
                  ErrorCode::InvalidArgument);
    AccountingVector nameless_money;
    nameless_money.monetary = MonetaryMeasure::known_amount(10, "", Provenance::Reported);
    RL_CHECK_CODE(ledger->append(accounting_recorded(SubjectId::of(AttemptId::from_value(1)),
                                                     nameless_money)),
                  ErrorCode::InvalidArgument);
    RL_CHECK_OK(ledger->append(accounting_recorded(SubjectId::of(AttemptId::from_value(1)),
                                                   zero_addition)));

    // Extreme values survive the canonical encoding exactly.
    Limits limits;
    AccountingRecorded extreme;
    extreme.scope = SubjectId::of(AttemptId::from_value(1));
    extreme.accounting.model_input_tokens = InputTokens::known(maximum, Provenance::Measured);
    extreme.accounting.monetary =
        MonetaryMeasure::known_amount(minimum_money, "USD", Provenance::Reported);
    extreme.source = "worker";
    extreme.authority = "worker:1";
    auto encoded = encode_record_body(extreme, limits);
    RL_CHECK_OK(encoded);
    auto decoded = decode_record_body(encoded.value(), limits);
    RL_CHECK_OK(decoded);
    const AccountingRecorded* round = std::get_if<AccountingRecorded>(&decoded.value());
    RL_CHECK(round != nullptr);
    if (round != nullptr) {
        RL_CHECK_EQ(round->accounting.model_input_tokens.units(), maximum);
        RL_CHECK(round->accounting.monetary.micro_units == minimum_money);
        RL_CHECK_EQ(round->accounting.monetary.currency, std::string("USD"));
    }
    auto integrity = ledger->snapshot().verify();
    RL_CHECK_OK(integrity);
    RL_CHECK(integrity.value().ok);
}

// --- corrupt snapshots ------------------------------------------------------

RL_TEST(adversarial_corrupt_and_truncated_snapshots) {
    TempDirectory directory("research-ledger-tests-adversarial-9");
    RL_CHECK(directory.created());
    const std::string path = directory.file("ledger.rls");

    auto created = Ledger::create(local_config());
    RL_CHECK_OK(created);
    const std::shared_ptr<Ledger>& ledger = created.value();
    RL_CHECK_OK(ledger->append_batch(research_baseline(1, 1, 1, 1, 1)));
    RL_REQUIRE_STATUS(ledger->save(path));

    Limits limits;
    auto image = image_of(ledger, limits);
    RL_CHECK_OK(image);
    const std::size_t size = image.value().size();
    RL_CHECK(size > kSnapshotHeaderBytes);

    // Truncation at every offset of a real image.
    for (std::size_t length = 0; length < size; ++length) {
        const std::span<const std::byte> prefix(image.value().data(), length);
        auto decoded = deserialize_snapshot(prefix, limits);
        RL_CHECK_MESSAGE(!decoded.ok(),
                         "truncation to " + std::to_string(length) + " bytes was accepted");
    }
    // Unsupported format versions.
    for (const std::uint32_t version : {0u, 2u, 0xffffffffu}) {
        std::vector<std::byte> broken = image.value();
        put_u32(broken, kSnapshotFormatOffset, version);
        RL_CHECK_CODE(deserialize_snapshot(broken, limits),
                      ErrorCode::PersistenceUnsupportedVersion);
    }
    // A corrupt image on disk does not load, and the in-memory ledger and the
    // last good file are unaffected until the next save.
    {
        std::vector<std::byte> broken = image.value();
        broken[kSnapshotHeaderBytes + 2] ^= std::byte{0x40};
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        RL_CHECK(output.good());
        output.write(reinterpret_cast<const char*>(broken.data()),
                     static_cast<std::streamsize>(broken.size()));
        output.close();
        RL_CHECK(output.good());
    }
    auto corrupted = Ledger::load(path, local_config());
    RL_CHECK(!corrupted.ok());
    RL_CHECK(corrupted.code() == ErrorCode::PersistenceCorrupt);
    RL_CHECK_EQ(ledger->last_sequence().value(), 5u);
    auto integrity = ledger->snapshot().verify();
    RL_CHECK_OK(integrity);
    RL_CHECK(integrity.value().ok);

    // The good image still decodes, and re-saving restores a loadable file.
    RL_CHECK_OK(deserialize_snapshot(image.value(), limits));
    RL_REQUIRE_STATUS(ledger->save(path));
    auto reloaded = Ledger::load(path, local_config());
    RL_CHECK_OK(reloaded);
    RL_CHECK_EQ(reloaded.value()->last_sequence().value(), 5u);
    RL_CHECK(reloaded.value()->logical_digest() == ledger->logical_digest());
}

// --- transport adversaries --------------------------------------------------

RL_TEST(adversarial_malformed_frames_do_not_disturb_a_running_coordinator) {
    NetworkRuntime network;
    CoordinatorConfig config;
    config.port = 0;
    config.identity = "adversarial-test-coordinator";
    auto started = Coordinator::start(config);
    RL_REQUIRE_RESULT(started);
    std::unique_ptr<Coordinator> coordinator = started.take();
    RL_CHECK(coordinator->port() != 0);

    Status serve_status{};
    std::thread server([&coordinator, &serve_status]() { serve_status = coordinator->serve(); });
    ServerGuard guard(coordinator.get(), &server);

    const std::uint16_t port = coordinator->port();
    Limits limits;
    const AuthorityEnvelope authority = coordinator->ledger()->local_authority();

    // A raw connection that sends 46 bytes of garbage.
    {
        auto socket = connect_loopback(port, "127.0.0.1");
        RL_REQUIRE_RESULT(socket);
        const std::vector<std::byte> garbage(static_cast<std::size_t>(kFrameHeaderSize),
                                             std::byte{0xab});
        RL_CHECK(send_all(socket.value(), garbage).ok());
        socket.value().close();
    }
    // A frame whose magic is wrong.
    {
        auto frame = encode_frame(MessageType::Hello, 1, authority, std::span<const std::byte>{},
                                  limits);
        RL_CHECK_OK(frame);
        std::vector<std::byte> broken = frame.value();
        put_u32(broken, kFrameMagicOffset, 0x00000000u);
        auto socket = connect_loopback(port, "127.0.0.1");
        RL_REQUIRE_RESULT(socket);
        RL_CHECK(send_all(socket.value(), broken).ok());
        socket.value().close();
    }
    // An absurd declared payload length: the coordinator must not allocate it.
    {
        auto frame = encode_frame(MessageType::Hello, 2, authority, std::span<const std::byte>{},
                                  limits);
        RL_CHECK_OK(frame);
        std::vector<std::byte> broken = frame.value();
        put_u32(broken, kFramePayloadLengthOffset, 0x3ffffff0u);
        auto socket = connect_loopback(port, "127.0.0.1");
        RL_REQUIRE_RESULT(socket);
        RL_CHECK(send_all(socket.value(), broken).ok());
        socket.value().close();
    }
    // Half a frame, then an abrupt close.
    {
        auto frame = encode_frame(MessageType::Hello, 3, authority, std::span<const std::byte>{},
                                  limits);
        RL_CHECK_OK(frame);
        auto socket = connect_loopback(port, "127.0.0.1");
        RL_REQUIRE_RESULT(socket);
        RL_CHECK(send_all(socket.value(),
                          std::span<const std::byte>(frame.value().data(), kFrameHeaderSize / 2u))
                    .ok());
        socket.value().close();
    }
    // A well formed header followed by a payload that is not any message.
    {
        const std::vector<std::byte> nonsense(64, std::byte{0x5a});
        auto frame = encode_frame(MessageType::Hello, 4, authority, nonsense, limits);
        RL_CHECK_OK(frame);
        auto socket = connect_loopback(port, "127.0.0.1");
        RL_REQUIRE_RESULT(socket);
        RL_CHECK(send_all(socket.value(), frame.value()).ok());
        socket.value().close();
    }

    // A valid worker still completes a handshake, appends and reads status.
    auto client = WorkerClient::connect(worker_config_for(port, 1));
    RL_REQUIRE_RESULT(client);
    auto acknowledgement = client.value()->handshake();
    RL_CHECK_OK(acknowledgement);
    RL_CHECK(acknowledgement.value().epoch == coordinator->epoch());
    RL_CHECK_EQ(acknowledgement.value().coordinator, config.identity);
    auto outcomes = client.value()->append(
        std::vector<RecordDraft>{session_opened(1, "session", "question"),
                                 hypothesis_declared(1, 1, "h")});
    RL_CHECK_OK(outcomes);
    RL_CHECK_EQ(outcomes.value().size(), 2u);
    RL_CHECK(outcomes.value()[0].state == CommitState::Committed);
    auto status = client.value()->status();
    RL_CHECK_OK(status);
    RL_CHECK_EQ(status.value().last_sequence.value(), 2u);
    RL_CHECK_EQ(status.value().sessions, 1u);
    RL_CHECK_EQ(status.value().live_workers, 1u);
    client.value()->close();

    RL_CHECK(coordinator->request_shutdown("malformed frame test complete").ok());
    server.join();
    RL_CHECK(serve_status.ok());

    auto integrity = coordinator->ledger()->snapshot().verify();
    RL_CHECK_OK(integrity);
    RL_CHECK(integrity.value().ok);
    RL_CHECK_EQ(coordinator->ledger()->last_sequence().value(), 2u);
}

RL_TEST(adversarial_worker_death_during_append_is_survivable) {
    NetworkRuntime network;
    CoordinatorConfig config;
    config.port = 0;
    config.identity = "adversarial-death-coordinator";
    auto started = Coordinator::start(config);
    RL_REQUIRE_RESULT(started);
    std::unique_ptr<Coordinator> coordinator = started.take();

    Status serve_status{};
    std::thread server([&coordinator, &serve_status]() { serve_status = coordinator->serve(); });
    ServerGuard guard(coordinator.get(), &server);

    const std::uint16_t port = coordinator->port();
    Limits limits;

    // A worker sends an append that can never be committed and dies before it
    // can read the coordinator's answer. However the abandoned request is
    // handled, history must not change.
    {
        auto doomed = WorkerClient::connect(worker_config_for(port, 21));
        RL_REQUIRE_RESULT(doomed);
        RecordDraft no_provenance = session_opened(1, "abandoned", "question");
        no_provenance.provenance = Provenance::Unknown;
        AppendRequestMessage request;
        request.drafts = std::vector<RecordDraft>{no_provenance};
        auto payload = encode_append_request(request, limits);
        RL_CHECK_OK(payload);
        RL_CHECK(doomed.value()
                     ->send_raw_frame(MessageType::AppendRequest, 1, doomed.value()->authority(),
                                      payload.value())
                     .ok());
        doomed.value()->close();
    }

    // A second worker sends a valid append and dies before the answer reaches
    // it. Its batch is committed whole or not at all, never in part.
    {
        auto doomed = WorkerClient::connect(worker_config_for(port, 23));
        RL_REQUIRE_RESULT(doomed);
        AppendRequestMessage request;
        request.drafts = std::vector<RecordDraft>{session_opened(1, "abandoned", "question")};
        auto payload = encode_append_request(request, limits);
        RL_CHECK_OK(payload);
        RL_CHECK(doomed.value()
                     ->send_raw_frame(MessageType::AppendRequest, 1, doomed.value()->authority(),
                                      payload.value())
                     .ok());
        doomed.value()->close();
    }

    // A third worker is unaffected: its records do not depend on either
    // abandoned request, so its outcome does not depend on their timing.
    auto survivor = WorkerClient::connect(worker_config_for(port, 22));
    RL_REQUIRE_RESULT(survivor);
    auto outcomes = survivor.value()->append(
        std::vector<RecordDraft>{session_opened(2, "survivor", "question"),
                                 hypothesis_declared(1, 2, "h")});
    RL_CHECK_OK(outcomes);
    RL_CHECK_EQ(outcomes.value().size(), 2u);
    RL_CHECK(outcomes.value()[0].state == CommitState::Committed);
    RL_CHECK(outcomes.value()[1].state == CommitState::Committed);

    // Everything below is read from one snapshot, so it describes one state.
    auto snapshot = coordinator->ledger()->snapshot();
    auto stats = snapshot.stats();
    RL_CHECK_OK(stats);
    const std::uint64_t sequence = stats.value().last_sequence.value();
    RL_CHECK(sequence == 2u || sequence == 3u);
    RL_CHECK_EQ(stats.value().sessions, sequence == 3u ? 2u : 1u);
    RL_CHECK_EQ(stats.value().hypotheses, 1u);
    auto integrity = snapshot.verify();
    RL_CHECK_OK(integrity);
    RL_CHECK(integrity.value().ok);
    RL_CHECK(integrity.value().issues.empty());
    RL_CHECK_OK(snapshot.session(ResearchSessionId::from_value(2)));
    RL_CHECK_OK(snapshot.hypothesis(HypothesisId::from_value(1)));

    // The dead workers may come back; a reconnect is admitted as a fresh
    // incarnation and disturbs nothing.
    auto revived = WorkerClient::connect(worker_config_for(port, 21));
    RL_REQUIRE_RESULT(revived);
    auto revived_status = revived.value()->status();
    RL_CHECK_OK(revived_status);
    RL_CHECK(revived_status.value().last_sequence.value() >= sequence);
    survivor.value()->close();
    revived.value()->close();

    RL_CHECK(coordinator->request_shutdown("worker death test complete").ok());
    server.join();
    RL_CHECK(serve_status.ok());
    auto final_stats = coordinator->ledger()->snapshot().stats();
    RL_CHECK_OK(final_stats);
    RL_CHECK(final_stats.value().last_sequence.value() >= sequence);
}

// --- scale ------------------------------------------------------------------

RL_TEST(adversarial_high_fan_out_and_deep_lineage) {
    auto created = Ledger::create(local_config());
    RL_CHECK_OK(created);
    const std::shared_ptr<Ledger>& ledger = created.value();
    RL_CHECK_OK(ledger->append_batch(research_baseline(1, 1, 1, 1, 1)));
    const SubjectId producer = SubjectId::of(AttemptId::from_value(1));

    // One artifact with many children.
    const std::uint64_t fan_out = 64;
    RL_CHECK_OK(ledger->append(
        artifact_referenced(1, 1, producer, ArtifactRole::Dataset, "root")));
    std::vector<RecordDraft> children;
    for (std::uint64_t index = 0; index < fan_out; ++index) {
        children.push_back(artifact_referenced(2u + index, 1, producer, ArtifactRole::Intermediate,
                                               "child", {ArtifactId::from_value(1)}));
    }
    RL_CHECK_OK(ledger->append_batch(children));
    auto descendants = ledger->snapshot().artifact_descendants(ArtifactId::from_value(1));
    RL_CHECK_OK(descendants);
    RL_CHECK_EQ(descendants.value().size(), fan_out);
    auto child_ancestry = ledger->snapshot().artifact_ancestry(ArtifactId::from_value(2));
    RL_CHECK_OK(child_ancestry);
    RL_CHECK_EQ(child_ancestry.value().size(), 1u);

    // A deep chain: each artifact is the child of the one before it.
    const std::uint64_t chain_length = 100;
    const std::uint64_t chain_base = 1000;
    std::vector<RecordDraft> chain;
    for (std::uint64_t index = 0; index < chain_length; ++index) {
        std::vector<ArtifactId> parents;
        if (index > 0) {
            parents.push_back(ArtifactId::from_value(chain_base + index - 1u));
        }
        chain.push_back(artifact_referenced(chain_base + index, 1, producer,
                                            ArtifactRole::Intermediate, "chain", parents));
    }
    RL_CHECK_OK(ledger->append_batch(chain));
    auto deep_ancestry =
        ledger->snapshot().artifact_ancestry(ArtifactId::from_value(chain_base + chain_length - 1u));
    RL_CHECK_OK(deep_ancestry);
    RL_CHECK_EQ(deep_ancestry.value().size(), chain_length - 1u);
    auto deep_descendants =
        ledger->snapshot().artifact_descendants(ArtifactId::from_value(chain_base));
    RL_CHECK_OK(deep_descendants);
    RL_CHECK_EQ(deep_descendants.value().size(), chain_length - 1u);

    // One artifact with as many parents as the model allows, and one more.
    std::vector<ArtifactId> many_parents;
    for (std::uint64_t index = 0; index < ledger->limits().max_parents_per_artifact; ++index) {
        many_parents.push_back(ArtifactId::from_value(chain_base + index));
    }
    std::vector<ArtifactId> too_many_parents = many_parents;
    too_many_parents.push_back(ArtifactId::from_value(chain_base + 100u));
    RL_CHECK_CODE(ledger->append(artifact_referenced(2000, 1, producer, ArtifactRole::Report,
                                                     "merged", too_many_parents)),
                  ErrorCode::LimitExceeded);
    RL_CHECK_OK(ledger->append(artifact_referenced(2000, 1, producer, ArtifactRole::Report,
                                                   "merged", many_parents)));
    auto merged = ledger->snapshot().artifact(ArtifactId::from_value(2000));
    RL_CHECK_OK(merged);
    RL_CHECK_EQ(merged.value().parents.size(),
                static_cast<std::size_t>(ledger->limits().max_parents_per_artifact));

    // A repeated parent is a malformed edge even when the list is short.
    RL_CHECK_CODE(ledger->append(artifact_referenced(2001, 1, producer, ArtifactRole::Report,
                                                     "repeated",
                                                     {ArtifactId::from_value(1000),
                                                      ArtifactId::from_value(1000)})),
                  ErrorCode::InvalidArgument);

    auto integrity = ledger->snapshot().verify();
    RL_CHECK_OK(integrity);
    RL_CHECK(integrity.value().ok);
    RL_CHECK(integrity.value().issues.empty());
    auto stats = ledger->snapshot().stats();
    RL_CHECK_OK(stats);
    RL_CHECK_EQ(stats.value().artifacts, 1u + fan_out + chain_length + 1u);
}

RL_TEST(adversarial_high_volume_append_and_reconstruction) {
    auto created = Ledger::create(local_config());
    RL_CHECK_OK(created);
    const std::shared_ptr<Ledger>& ledger = created.value();
    RL_CHECK_OK(ledger->append_batch(research_baseline(1, 1, 1, 1, 1)));

    // Attempt 1 was declared by the baseline, so the volume run declares the
    // attempts after it.
    const std::uint64_t attempts = 1500;
    const std::uint64_t first_attempt = 2;
    const std::uint64_t last_attempt = first_attempt + attempts - 1u;
    const std::uint64_t batch_attempts = 64;
    for (std::uint64_t first = first_attempt; first <= last_attempt; first += batch_attempts) {
        const std::uint64_t last =
            first + batch_attempts - 1u > last_attempt ? last_attempt : first + batch_attempts - 1u;
        std::vector<RecordDraft> batch;
        for (std::uint64_t attempt = first; attempt <= last; ++attempt) {
            batch.push_back(attempt_started(attempt, 1, 1, "worker:1"));
            batch.push_back(attempt_completed(attempt, "outcome"));
        }
        auto outcomes = ledger->append_batch(batch);
        RL_CHECK_OK(outcomes);
    }

    const std::uint64_t expected = 5u + (attempts * 2u);
    RL_CHECK_EQ(ledger->last_sequence().value(), expected);
    auto stats = ledger->snapshot().stats();
    RL_CHECK_OK(stats);
    RL_CHECK_EQ(stats.value().attempts, attempts + 1u);
    RL_CHECK_EQ(stats.value().sessions, 1u);
    RL_CHECK_EQ(stats.value().count_of(RecordType::AttemptCompleted), attempts);
    auto integrity = ledger->snapshot().verify();
    RL_CHECK_OK(integrity);
    RL_CHECK(integrity.value().ok);
    RL_CHECK(integrity.value().issues.empty());
    RL_CHECK_EQ(integrity.value().checked_records, expected);

    // Reconstruction of the whole history is deterministic and matches the live
    // ledger exactly.
    auto records = all_records(ledger);
    RL_CHECK_OK(records);
    RL_CHECK_EQ(records.value().size(), static_cast<std::size_t>(expected));
    auto replayed = replay_committed(records.value(), local_config());
    RL_CHECK_OK(replayed);
    RL_CHECK_EQ(replayed.value().records, expected);
    RL_CHECK(replayed.value().chain_digest == stats.value().chain_digest);
    RL_CHECK(replayed.value().logical_digest == ledger->logical_digest());
    RL_CHECK(replayed.value().integrity.ok);
    RL_CHECK_EQ(replayed.value().integrity.checked_records, expected);

    // The same reconstruction from a permuted record vector agrees.
    std::vector<Record> permuted = records.value();
    std::reverse(permuted.begin(), permuted.end());
    auto permuted_replay = replay_committed(permuted, local_config());
    RL_CHECK_OK(permuted_replay);
    RL_CHECK(permuted_replay.value().logical_digest == replayed.value().logical_digest);
    RL_CHECK(permuted_replay.value().chain_digest == replayed.value().chain_digest);
}

}  // namespace
}  // namespace research_ledger
