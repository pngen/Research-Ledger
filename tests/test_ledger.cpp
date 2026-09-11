#include "test_framework.hpp"
#include "test_support.hpp"

#include "research_ledger/record_codec.hpp"

#include <string>
#include <vector>

namespace research_ledger {
namespace {

using namespace research_ledger::test;

RL_TEST(append_assigns_monotonic_sequences) {
    auto ledger = Ledger::create(local_config());
    RL_CHECK_OK(ledger);
    auto outcomes = ledger.value()->append_batch(std::vector<RecordDraft>{session_opened(1),
                                                                         hypothesis_declared(1, 1, "h")});
    RL_CHECK_OK(outcomes);
    RL_CHECK_EQ(outcomes.value().size(), 2u);
    RL_CHECK_EQ(outcomes.value()[0].sequence.value(), 1u);
    RL_CHECK_EQ(outcomes.value()[0].state, CommitState::Committed);
    RL_CHECK_EQ(outcomes.value()[1].sequence.value(), 2u);
    RL_CHECK_EQ(ledger.value()->last_sequence().value(), 2u);

    auto snapshot = ledger.value()->snapshot();
    auto record = snapshot.record_at(RecordSequence::from_value(2));
    RL_CHECK_OK(record);
    RL_CHECK(record.value().header.type == RecordType::HypothesisDeclared);
    RL_CHECK(!record.value().header.payload_digest.is_zero());
    RL_CHECK(!record.value().header.chain_digest.is_zero());
    RL_CHECK_EQ(record.value().header.authority.worker.value(), 1u);
    RL_CHECK_EQ(record.value().header.sequence.value(), 2u);
}

RL_TEST(duplicate_record_identity_rejects_or_deduplicates) {
    auto ledger = Ledger::create(local_config());
    RL_CHECK_OK(ledger);

    RecordDraft first = session_opened(1);
    first.record_id = LedgerRecordId::from_value(9001);
    RL_CHECK_OK(ledger.value()->append(first));

    RecordDraft replay = session_opened(1);
    replay.record_id = LedgerRecordId::from_value(9001);
    auto rejected = ledger.value()->append(replay);
    RL_CHECK_CODE(rejected, ErrorCode::DuplicateRecord);
    RL_CHECK_EQ(ledger.value()->last_sequence().value(), 1u);

    replay.idempotent = true;
    auto recognized = ledger.value()->append(replay);
    RL_CHECK_OK(recognized);
    RL_CHECK(recognized.value().duplicate);
    RL_CHECK_EQ(recognized.value().state, CommitState::Duplicate);
    RL_CHECK_EQ(recognized.value().sequence.value(), 1u);
    RL_CHECK_EQ(ledger.value()->last_sequence().value(), 1u);
}

RL_TEST(failed_batch_leaves_committed_history_untouched) {
    auto ledger = Ledger::create(local_config());
    RL_CHECK_OK(ledger);
    RL_CHECK_OK(ledger.value()->append(session_opened(1)));

    std::vector<RecordDraft> batch{hypothesis_declared(1, 1, "valid"),
                                   hypothesis_declared(2, 99, "session does not exist")};
    auto outcomes = ledger.value()->append_batch(batch);
    RL_CHECK_CODE(outcomes, ErrorCode::MissingDependency);
    RL_CHECK_EQ(ledger.value()->last_sequence().value(), 1u);
    auto snapshot = ledger.value()->snapshot();
    RL_CHECK(!snapshot.hypothesis(HypothesisId::from_value(1)).ok());

    RL_CHECK_OK(ledger.value()->append(hypothesis_declared(1, 1, "valid")));
    // The older snapshot still observes only what was committed when it was
    // taken; a new snapshot observes the appended record.
    RL_CHECK_CODE(snapshot.hypothesis(HypothesisId::from_value(1)), ErrorCode::NotFound);
    RL_CHECK_OK(ledger.value()->snapshot().hypothesis(HypothesisId::from_value(1)));
}

RL_TEST(provenance_must_be_stated) {
    auto ledger = Ledger::create(local_config());
    RL_CHECK_OK(ledger);
    RecordDraft draft = session_opened(1);
    draft.provenance = Provenance::Unknown;
    RL_CHECK_CODE(ledger.value()->append(draft), ErrorCode::InvalidArgument);
    RL_CHECK_EQ(ledger.value()->last_sequence().value(), 0u);
}

RL_TEST(authority_is_fenced_across_incarnations_and_epochs) {
    LedgerConfig config;
    config.require_worker_admission = true;
    auto ledger = Ledger::create(config);
    RL_CHECK_OK(ledger);
    auto ledger_ptr = ledger.value();

    const WorkerId worker = WorkerId::from_value(7);
    std::uint32_t counter = 1;
    auto first = ledger_ptr->admit_worker_incarnation(worker, WorkerBootId{}, counter++);
    RL_CHECK_OK(first);
    RL_CHECK(first.value().worker_boot.valid());

    RL_CHECK_OK(ledger_ptr->append_batch_with_authority(
        std::vector<RecordDraft>{session_opened(1)}, first.value()));

    // An unknown incarnation is not authorized.
    AuthorityEnvelope forged = first.value();
    forged.worker_boot = WorkerBootId::from_value(999999);
    auto unauthorized = ledger_ptr->append_batch_with_authority(
        std::vector<RecordDraft>{session_opened(2)}, forged);
    RL_CHECK_CODE(unauthorized, ErrorCode::StaleWorker);

    // An unknown worker is not authorized at all.
    AuthorityEnvelope stranger = first.value();
    stranger.worker = WorkerId::from_value(8);
    stranger.worker_boot = WorkerBootId::from_value(5);
    auto stranger_status = ledger_ptr->append_batch_with_authority(
        std::vector<RecordDraft>{session_opened(3)}, stranger);
    RL_CHECK_CODE(stranger_status, ErrorCode::Unauthorized);

    // A reconnect that presents the boot it already holds keeps it.
    auto reconnect_attempt =
        ledger_ptr->admit_worker_incarnation(worker, first.value().worker_boot, counter++);
    RL_CHECK_OK(reconnect_attempt);
    RL_CHECK(reconnect_attempt.value().worker_boot == first.value().worker_boot);

    // A fresh incarnation is admitted with a fresh boot identity, and the
    // previous incarnation is fenced.
    auto second = ledger_ptr->admit_worker_incarnation(worker, WorkerBootId{}, counter++);
    RL_CHECK_OK(second);
    RL_CHECK(second.value().worker_boot != first.value().worker_boot);
    RL_CHECK(!ledger_ptr->worker_is_live(worker, first.value().worker_boot));
    RL_CHECK(ledger_ptr->worker_is_live(worker, second.value().worker_boot));

    auto stale_boot = ledger_ptr->append_batch_with_authority(
        std::vector<RecordDraft>{session_opened(4)}, first.value());
    RL_CHECK_CODE(stale_boot, ErrorCode::StaleWorker);

    // A new coordinator epoch fences every live incarnation and rejects
    // prior-epoch traffic.
    auto epoch = ledger_ptr->advance_epoch();
    RL_CHECK_OK(epoch);
    RL_CHECK(epoch.value().value() > first.value().epoch.value());
    RL_CHECK(!ledger_ptr->worker_is_live(worker, second.value().worker_boot));
    auto stale_epoch = ledger_ptr->append_batch_with_authority(
        std::vector<RecordDraft>{session_opened(5)}, second.value());
    RL_CHECK_CODE(stale_epoch, ErrorCode::StaleEpoch);

    auto refreshed = ledger_ptr->admit_worker_incarnation(worker, second.value().worker_boot, counter++);
    RL_CHECK_OK(refreshed);
    RL_CHECK(refreshed.value().epoch == epoch.value());
    RL_CHECK_OK(ledger_ptr->append_batch_with_authority(
        std::vector<RecordDraft>{session_opened(5)}, refreshed.value()));
}

RL_TEST(session_closure_only_accepts_annotations) {
    auto ledger = Ledger::create(local_config());
    RL_CHECK_OK(ledger);
    RL_CHECK_OK(ledger.value()->append(session_opened(1)));

    SessionClosed closed;
    closed.session = ResearchSessionId::from_value(1);
    closed.note = "closed after review";
    RL_CHECK_OK(ledger.value()->append(draft_of(closed, Provenance::Reported)));
    RL_CHECK_CODE(ledger.value()->append(draft_of(closed, Provenance::Reported)),
                  ErrorCode::AlreadyTerminal);

    // A closed session accepts a bounded post-hoc annotation, not new research.
    RL_CHECK_CODE(ledger.value()->append(hypothesis_declared(1, 1, "too late")),
                  ErrorCode::SessionClosed);
    SessionAnnotation annotation;
    annotation.session = ResearchSessionId::from_value(1);
    annotation.note = "audit note";
    RL_CHECK_OK(ledger.value()->append(draft_of(annotation, Provenance::Reconstructed)));

    auto snapshot = ledger.value()->snapshot();
    auto session = snapshot.session(ResearchSessionId::from_value(1));
    RL_CHECK_OK(session);
    RL_CHECK(session.value().state == SessionState::Closed);
    RL_CHECK_EQ(session.value().label, std::string("session"));
}

RL_TEST(hypothesis_revisions_form_acyclic_lineage) {
    auto ledger = Ledger::create(local_config());
    RL_CHECK_OK(ledger);
    RL_CHECK_OK(ledger.value()->append(session_opened(1)));
    RL_CHECK_OK(ledger.value()->append(hypothesis_declared(1, 1, "base hypothesis")));

    HypothesisDeclared revised;
    revised.hypothesis = HypothesisId::from_value(2);
    revised.generation = HypothesisGeneration::first();
    revised.session = ResearchSessionId::from_value(1);
    revised.claim = "revised hypothesis";
    revised.parent = HypothesisId::from_value(1);
    revised.parent_generation = HypothesisGeneration::first();
    revised.authority = "researcher";
    RL_CHECK_OK(ledger.value()->append(draft_of(revised, Provenance::Reported)));

    HypothesisDeclared stale;
    stale.hypothesis = HypothesisId::from_value(3);
    stale.generation = HypothesisGeneration::first();
    stale.session = ResearchSessionId::from_value(1);
    stale.claim = "stale parent generation";
    stale.parent = HypothesisId::from_value(1);
    stale.parent_generation = HypothesisGeneration::from_value(9);
    stale.authority = "researcher";
    RL_CHECK_CODE(ledger.value()->append(draft_of(stale, Provenance::Reported)),
                  ErrorCode::StaleGeneration);

    HypothesisDeclared missing;
    missing.hypothesis = HypothesisId::from_value(4);
    missing.generation = HypothesisGeneration::first();
    missing.session = ResearchSessionId::from_value(1);
    missing.claim = "missing parent";
    missing.parent = HypothesisId::from_value(77);
    missing.parent_generation = HypothesisGeneration::first();
    missing.authority = "researcher";
    RL_CHECK_CODE(ledger.value()->append(draft_of(missing, Provenance::Reported)),
                  ErrorCode::MissingDependency);

    auto snapshot = ledger.value()->snapshot();
    auto ancestry = snapshot.hypothesis_ancestry(HypothesisId::from_value(2));
    RL_CHECK_OK(ancestry);
    RL_CHECK_EQ(ancestry.value().size(), 2u);
    RL_CHECK_EQ(ancestry.value()[0].value(), 2u);
    RL_CHECK_EQ(ancestry.value()[1].value(), 1u);
    auto descendants = snapshot.hypothesis_descendants(HypothesisId::from_value(1));
    RL_CHECK_OK(descendants);
    RL_CHECK_EQ(descendants.value().size(), 1u);
}

RL_TEST(hypothesis_status_is_generation_checked) {
    auto ledger = Ledger::create(local_config());
    RL_CHECK_OK(ledger);
    RL_CHECK_OK(ledger.value()->append(session_opened(1)));
    RL_CHECK_OK(ledger.value()->append(hypothesis_declared(1, 1, "h")));

    HypothesisStatusChanged change;
    change.hypothesis = HypothesisId::from_value(1);
    change.generation = HypothesisGeneration::from_value(2);
    change.from = HypothesisStatus::Proposed;
    change.to = HypothesisStatus::Active;
    change.authority = "researcher";
    RL_CHECK_OK(ledger.value()->append(draft_of(change, Provenance::Reported)));

    HypothesisStatusChanged repeated = change;
    repeated.generation = HypothesisGeneration::from_value(3);
    RL_CHECK_CODE(ledger.value()->append(draft_of(repeated, Provenance::Reported)),
                  ErrorCode::InvalidTransition);

    HypothesisStatusChanged wrong_generation = change;
    wrong_generation.generation = HypothesisGeneration::from_value(9);
    wrong_generation.from = HypothesisStatus::Active;
    wrong_generation.to = HypothesisStatus::Supported;
    RL_CHECK_CODE(ledger.value()->append(draft_of(wrong_generation, Provenance::Reported)),
                  ErrorCode::StaleGeneration);

    auto snapshot = ledger.value()->snapshot();
    auto hypothesis = snapshot.hypothesis(HypothesisId::from_value(1));
    RL_CHECK_OK(hypothesis);
    RL_CHECK(hypothesis.value().status == HypothesisStatus::Active);
    RL_CHECK_EQ(hypothesis.value().generation.value(), 2u);
}

RL_TEST(attempt_terminal_states_are_exclusive) {
    auto ledger = Ledger::create(local_config());
    RL_CHECK_OK(ledger);
    std::vector<RecordDraft> setup{session_opened(1), hypothesis_declared(1, 1, "h"),
                                   branch_declared(1, 1), experiment_declared(1, 1, 1, 1),
                                   attempt_started(1, 1, 1)};
    RL_CHECK_OK(ledger.value()->append_batch(setup));

    // A second start for the same attempt is a duplicate identity.
    RL_CHECK_CODE(ledger.value()->append(attempt_started(1, 1, 1)), ErrorCode::DuplicateRecord);

    FailureRecorded failure;
    failure.failure = FailureId::from_value(1);
    failure.scope = SubjectId::of(AttemptId::from_value(1));
    failure.category = FailureCategory::Execution;
    failure.message = "worker died";
    failure.authority = "worker:1";
    RL_CHECK_OK(ledger.value()->append(draft_of(failure, Provenance::Measured)));

    // Failure evidence alone does not terminate the attempt: the attempt's own
    // terminal record does.
    RL_CHECK_OK(ledger.value()->append(attempt_failed(1, 1)));
    RL_CHECK_CODE(ledger.value()->append(attempt_completed(1)), ErrorCode::AlreadyTerminal);
    RL_CHECK_CODE(ledger.value()->append(attempt_failed(1, 1)), ErrorCode::DuplicateCompletion);
    RL_CHECK_CODE(ledger.value()->append(attempt_cancelled(1)), ErrorCode::AlreadyTerminal);

    auto snapshot = ledger.value()->snapshot();
    auto attempt = snapshot.attempt(AttemptId::from_value(1));
    RL_CHECK_OK(attempt);
    RL_CHECK(attempt.value().state == AttemptState::Failed);

    // A cancelled attempt can never become successful through a delayed
    // completion, and it accepts no further authoritative activity.
    std::vector<RecordDraft> second{attempt_started(2, 1, 1)};
    RL_CHECK_OK(ledger.value()->append_batch(second));
    RL_CHECK_OK(ledger.value()->append(attempt_cancelled(2, "operator cancelled")));
    RL_CHECK_CODE(ledger.value()->append(attempt_completed(2)), ErrorCode::Cancelled);
    RL_CHECK_CODE(ledger.value()->append(model_call_recorded(1, 2)), ErrorCode::Cancelled);
    RL_CHECK_CODE(ledger.value()->append(attempt_failed(2, 1)), ErrorCode::Cancelled);
}

RL_TEST(artifact_lineage_is_explicit_and_acyclic) {
    auto ledger = Ledger::create(local_config());
    RL_CHECK_OK(ledger);
    std::vector<RecordDraft> setup{session_opened(1), hypothesis_declared(1, 1, "h"),
                                   branch_declared(1, 1), experiment_declared(1, 1, 1, 1),
                                   attempt_started(1, 1, 1)};
    RL_CHECK_OK(ledger.value()->append_batch(setup));

    const SubjectId producer = SubjectId::of(AttemptId::from_value(1));
    RL_CHECK_OK(ledger.value()->append(
        artifact_referenced(1, 1, producer, ArtifactRole::Dataset, "dataset")));
    RL_CHECK_OK(ledger.value()->append(artifact_referenced(
        2, 1, producer, ArtifactRole::Intermediate, "normalized", {ArtifactId::from_value(1)})));
    RL_CHECK_OK(ledger.value()->append(artifact_referenced(
        3, 1, producer, ArtifactRole::ResultArtifact, "final", {ArtifactId::from_value(2)})));

    // An artifact cannot be its own parent.
    RL_CHECK_CODE(ledger.value()->append(artifact_referenced(
                      4, 1, producer, ArtifactRole::Other, "cycle", {ArtifactId::from_value(4)})),
                  ErrorCode::LineageCycle);
    // A missing parent is a broken reference, not a silent gap.
    RL_CHECK_CODE(ledger.value()->append(artifact_referenced(
                      5, 1, producer, ArtifactRole::Other, "orphan", {ArtifactId::from_value(99)})),
                  ErrorCode::MissingDependency);
    // A new revision of an existing artifact must be the next generation.
    ArtifactReferenced wrong_generation;
    wrong_generation.artifact = ArtifactId::from_value(3);
    wrong_generation.generation = ArtifactGeneration::from_value(5);
    wrong_generation.session = ResearchSessionId::from_value(1);
    wrong_generation.content_digest = digest_of("other");
    wrong_generation.role = ArtifactRole::Other;
    wrong_generation.producer = producer;
    wrong_generation.validation = ValidationState::Validated;
    wrong_generation.authority = "worker:1";
    RL_CHECK_CODE(ledger.value()->append(draft_of(wrong_generation, Provenance::Reported)),
                  ErrorCode::StaleGeneration);
    // A content digest must be stated.
    ArtifactReferenced no_digest;
    no_digest.artifact = ArtifactId::from_value(6);
    no_digest.generation = ArtifactGeneration::first();
    no_digest.session = ResearchSessionId::from_value(1);
    no_digest.role = ArtifactRole::Other;
    no_digest.producer = producer;
    no_digest.validation = ValidationState::Validated;
    no_digest.authority = "worker:1";
    RL_CHECK_CODE(ledger.value()->append(draft_of(no_digest, Provenance::Reported)),
                  ErrorCode::InvalidArgument);

    auto snapshot = ledger.value()->snapshot();
    auto ancestry = snapshot.artifact_ancestry(ArtifactId::from_value(3));
    RL_CHECK_OK(ancestry);
    RL_CHECK_EQ(ancestry.value().size(), 2u);
    RL_CHECK_EQ(ancestry.value()[0].artifact.value(), 1u);
    RL_CHECK_EQ(ancestry.value()[1].artifact.value(), 2u);
    auto descendants = snapshot.artifact_descendants(ArtifactId::from_value(1));
    RL_CHECK_OK(descendants);
    RL_CHECK_EQ(descendants.value().size(), 2u);
}

RL_TEST(cross_session_references_are_rejected) {
    auto ledger = Ledger::create(local_config());
    RL_CHECK_OK(ledger);
    std::vector<RecordDraft> setup{session_opened(1), session_opened(2),
                                   hypothesis_declared(1, 1, "h1"), branch_declared(1, 1)};
    RL_CHECK_OK(ledger.value()->append_batch(setup));

    // An experiment cannot borrow a branch from another session.
    RL_CHECK_CODE(ledger.value()->append(experiment_declared(1, 2, 1, 1)),
                  ErrorCode::CrossSessionReference);
    // A decision cannot be recorded in a session it does not belong to.
    auto decision = decision_recorded(1, 2, SubjectId::of(HypothesisId::from_value(1)),
                                      DecisionType::HypothesisTransition, DecisionOutcome::Recorded);
    RL_CHECK_CODE(ledger.value()->append(decision), ErrorCode::CrossSessionReference);
}

RL_TEST(evidence_must_precede_and_concern_the_decision) {
    auto ledger = Ledger::create(local_config());
    RL_CHECK_OK(ledger);
    auto history = build_standard_history(local_config());
    RL_CHECK_OK(history);

    auto snapshot = history.value().ledger->snapshot();
    // Evidence that is not committed yet cannot be cited.
    auto not_yet = snapshot.subject_sequence(SubjectId::of(ResultId::from_value(1)));
    RL_CHECK_OK(not_yet);
    const std::uint64_t future = history.value().ledger->last_sequence().value() + 5;
    auto decision = decision_recorded(
        2, 1, SubjectId::of(ResultId::from_value(1)), DecisionType::Audit,
        DecisionOutcome::Recorded,
        {EvidenceRef{SubjectId::of(ResultId::from_value(1)),
                     RecordSequence::from_value(static_cast<std::uint32_t>(future))}});
    RL_CHECK_CODE(history.value().ledger->append(decision), ErrorCode::InvalidTransition);

    // Evidence that concerns another subject is rejected.
    const RecordSequence experiment_sequence =
        snapshot.subject_sequence(SubjectId::of(ExperimentId::from_value(1))).value();
    auto mismatched = decision_recorded(
        3, 1, SubjectId::of(ResultId::from_value(1)), DecisionType::Audit,
        DecisionOutcome::Recorded,
        {EvidenceRef{SubjectId::of(HypothesisId::from_value(1)), experiment_sequence}});
    RL_CHECK_CODE(history.value().ledger->append(mismatched), ErrorCode::BrokenLineage);

    // A decision that cites the same evidence twice is rejected.
    auto duplicate_evidence = decision_recorded(
        4, 1, SubjectId::of(ResultId::from_value(1)), DecisionType::Audit,
        DecisionOutcome::Recorded,
        {EvidenceRef{SubjectId::of(ExperimentId::from_value(1)), experiment_sequence},
         EvidenceRef{SubjectId::of(ExperimentId::from_value(1)), experiment_sequence}});
    RL_CHECK_CODE(history.value().ledger->append(duplicate_evidence), ErrorCode::InvalidArgument);
}

RL_TEST(standard_history_reconstructs_the_accepted_result) {
    auto history = build_standard_history(local_config());
    RL_CHECK_OK(history);
    auto snapshot = history.value().ledger->snapshot();

    auto result = snapshot.result(history.value().result);
    RL_CHECK_OK(result);
    RL_CHECK(result.value().status == ResultStatus::Accepted);
    RL_CHECK(result.value().acceptance_decision.has_value());
    RL_CHECK_EQ(result.value().acceptance_decision->value(), 1u);

    auto bundle = snapshot.supporting_evidence(history.value().result);
    RL_CHECK_OK(bundle);
    RL_CHECK(bundle.value().accepted());
    RL_CHECK(bundle.value().complete);
    RL_CHECK(bundle.value().reconstructable);
    RL_CHECK(!bundle.value().lineage_digest.is_zero());
    RL_CHECK_EQ(bundle.value().hypotheses.size(), 1u);
    RL_CHECK_EQ(bundle.value().experiments.size(), 1u);
    RL_CHECK_EQ(bundle.value().attempts.size(), 1u);
    RL_CHECK_EQ(bundle.value().model_calls.size(), 1u);
    RL_CHECK_EQ(bundle.value().tool_calls.size(), 1u);
    RL_CHECK_EQ(bundle.value().artifacts.size(), 2u);
    RL_CHECK_EQ(bundle.value().observations.size(), 1u);
    RL_CHECK_EQ(bundle.value().decisions.size(), 1u);
    RL_CHECK(!bundle.value().cited_evidence.empty());
    // Artifact 1 is the ancestor of artifact 2 and is part of the closure.
    RL_CHECK_EQ(bundle.value().artifacts[0].artifact.value(), 1u);
    RL_CHECK_EQ(bundle.value().artifacts[1].artifact.value(), 2u);

    auto explanation = snapshot.explain_result(history.value().result);
    RL_CHECK_OK(explanation);
    RL_CHECK(explanation.value().lines.size() > 5u);
    const std::string text = explanation.value().to_text();
    RL_CHECK(text.find("RESULT result:1") != std::string::npos);
    RL_CHECK(text.find("ACCEPTED") != std::string::npos);
    RL_CHECK(text.find("EVIDENCE") != std::string::npos);
    RL_CHECK(text.find("MODEL_CALL") != std::string::npos);
    RL_CHECK(text.find("OBSERVATION") != std::string::npos);

    auto integrity = snapshot.verify();
    RL_CHECK_OK(integrity);
    RL_CHECK(integrity.value().ok);
    RL_CHECK_EQ(integrity.value().checked_records, history.value().ledger->last_sequence().value());
    RL_CHECK(integrity.value().issues.empty());

    auto stats = snapshot.stats();
    RL_CHECK_OK(stats);
    RL_CHECK_EQ(stats.value().sessions, 1u);
    RL_CHECK_EQ(stats.value().hypotheses, 1u);
    RL_CHECK_EQ(stats.value().experiments, 1u);
    RL_CHECK_EQ(stats.value().attempts, 1u);
    RL_CHECK_EQ(stats.value().results, 1u);
    RL_CHECK_EQ(stats.value().decisions, 1u);
    RL_CHECK(stats.value().count_of(RecordType::ResultDeclared) == 1u);
    RL_CHECK(stats.value().count_of(RecordType::ModelCallRecorded) == 1u);
}

RL_TEST(a_rejected_result_stays_in_history) {
    auto ledger = Ledger::create(local_config());
    RL_CHECK_OK(ledger);
    std::vector<RecordDraft> setup{session_opened(1), hypothesis_declared(1, 1, "h"),
                                   branch_declared(1, 1), experiment_declared(1, 1, 1, 1),
                                   attempt_started(1, 1, 1), attempt_completed(1), failure_recorded(1, SubjectId::of(AttemptId::from_value(1)), FailureCategory::Validation, "invalid measurement"),
                                   result_declared(1, 1, 1, 1)};
    RL_CHECK_OK(ledger.value()->append_batch(setup));

    auto pending = ledger.value()->snapshot().result(ResultId::from_value(1));
    RL_CHECK_OK(pending);
    RL_CHECK(pending.value().status == ResultStatus::Candidate);

    auto snapshot = ledger.value()->snapshot();
    const RecordSequence attempt_sequence =
        snapshot.subject_sequence(SubjectId::of(AttemptId::from_value(1))).value();
    std::vector<RecordDraft> rejection{
        decision_recorded(1, 1, SubjectId::of(ResultId::from_value(1)), DecisionType::Reject,
                          DecisionOutcome::Rejected,
                          {EvidenceRef{SubjectId::of(AttemptId::from_value(1)), attempt_sequence}}),
        result_status_changed(1, 2, ResultStatus::Candidate, ResultStatus::Rejected, 1)};
    RL_CHECK_OK(ledger.value()->append_batch(rejection));

    // A rejected result cannot become accepted directly; a reclassification
    // decision is required, and the rejection remains historical.
    auto direct_accept = decision_recorded(2, 1, SubjectId::of(ResultId::from_value(1)),
                                           DecisionType::Accept, DecisionOutcome::Accepted);
    RL_CHECK_OK(ledger.value()->append(direct_accept));
    RL_CHECK_CODE(ledger.value()->append(result_status_changed(
                      1, 3, ResultStatus::Rejected, ResultStatus::Accepted, 2)),
                  ErrorCode::InvalidTransition);

    std::vector<RecordDraft> reclassify{
        decision_recorded(3, 1, SubjectId::of(ResultId::from_value(1)), DecisionType::Reclassify,
                          DecisionOutcome::Reclassified),
        result_status_changed(1, 3, ResultStatus::Rejected, ResultStatus::Candidate, 3)};
    RL_CHECK_OK(ledger.value()->append_batch(reclassify));
    std::vector<RecordDraft> accept{
        decision_recorded(4, 1, SubjectId::of(ResultId::from_value(1)), DecisionType::Accept,
                          DecisionOutcome::Accepted,
                          {EvidenceRef{SubjectId::of(AttemptId::from_value(1)), attempt_sequence}}),
        result_status_changed(1, 4, ResultStatus::Candidate, ResultStatus::Accepted, 4)};
    RL_CHECK_OK(ledger.value()->append_batch(accept));

    auto final_result = ledger.value()->snapshot().result(ResultId::from_value(1));
    RL_CHECK_OK(final_result);
    RL_CHECK(final_result.value().status == ResultStatus::Accepted);
    RL_CHECK_EQ(final_result.value().acceptance_decision->value(), 4u);

    // Every step remains queryable: the original rejection decision is still
    // committed evidence.
    auto decisions = ledger.value()->snapshot().decisions_of(SubjectId::of(ResultId::from_value(1)));
    RL_CHECK_OK(decisions);
    RL_CHECK_EQ(decisions.value().size(), 4u);
    RL_CHECK(decisions.value()[0].outcome == DecisionOutcome::Rejected);
    RL_CHECK(decisions.value()[3].outcome == DecisionOutcome::Accepted);
}

RL_TEST(snapshot_watermark_hides_later_records) {
    auto ledger = Ledger::create(local_config());
    RL_CHECK_OK(ledger);
    RL_CHECK_OK(ledger.value()->append(session_opened(1)));
    LedgerSnapshot snapshot = ledger.value()->snapshot();
    RL_CHECK_EQ(snapshot.watermark().value(), 1u);

    RL_CHECK_OK(ledger.value()->append(hypothesis_declared(1, 1, "later")));
    RL_CHECK_CODE(snapshot.hypothesis(HypothesisId::from_value(1)), ErrorCode::NotFound);
    RL_CHECK(!snapshot.record_at(RecordSequence::from_value(2)).ok());
    RL_CHECK_OK(ledger.value()->snapshot().hypothesis(HypothesisId::from_value(1)));
}

RL_TEST(bounds_are_enforced_before_allocation) {
    Limits limits;
    limits.max_records_per_batch = 2;
    LedgerConfig config = local_config();
    config.limits = limits;
    auto ledger = Ledger::create(config);
    RL_CHECK_OK(ledger);
    std::vector<RecordDraft> too_many{session_opened(1), session_opened(2), session_opened(3)};
    RL_CHECK_CODE(ledger.value()->append_batch(too_many), ErrorCode::LimitExceeded);

    // A declared string length beyond the limit is rejected before it is used.
    std::vector<std::byte> image;
    ByteWriter writer;
    writer.u32(0xffffffffu);
    image = writer.take();
    ByteReader reader(image, limits);
    RL_CHECK_CODE(reader.string(64), ErrorCode::LimitExceeded);

    // An oversized metadata value is rejected.
    RecordDraft draft = session_opened(1);
    MetadataEntry entry;
    entry.key = "k";
    entry.value = std::string(limits.max_metadata_value_length + 1, 'x');
    std::get<SessionOpened>(draft.body).metadata.push_back(entry);
    RL_CHECK_CODE(ledger.value()->append(draft), ErrorCode::LimitExceeded);

    // A record without any known accounting quantity is not evidence.
    RL_CHECK_OK(ledger.value()->append(session_opened(9)));
    RL_CHECK_CODE(
        ledger.value()->append(accounting_recorded(SubjectId::of(ResearchSessionId::from_value(9)),
                                                   AccountingVector{})),
        ErrorCode::InvalidArgument);
}

RL_TEST(record_codec_round_trips_every_record_type) {
    Limits limits;
    const std::vector<RecordBody> bodies{
        session_opened(1).body,
        draft_of(SessionClosed{ResearchSessionId::from_value(1), "closed"}, Provenance::Reported).body,
        draft_of(SessionAnnotation{ResearchSessionId::from_value(1), "note"}, Provenance::Reported).body,
        hypothesis_declared(1, 1, "claim").body,
        draft_of(HypothesisStatusChanged{HypothesisId::from_value(1),
                                         HypothesisGeneration::from_value(2),
                                         HypothesisStatus::Proposed, HypothesisStatus::Active,
                                         std::nullopt, "researcher"},
                 Provenance::Reported)
            .body,
        branch_declared(1, 1).body,
        experiment_declared(1, 1, 1, 1).body,
        attempt_started(1, 1, 1).body,
        attempt_completed(1, "outcome").body,
        attempt_cancelled(1, "cancelled").body,
        attempt_failed(1, 1).body,
        model_call_recorded(1, 1).body,
        tool_call_recorded(1, 1).body,
        artifact_referenced(1, 1, SubjectId::of(AttemptId::from_value(1)), ArtifactRole::Dataset,
                            "dataset", {})
            .body,
        draft_of(ArtifactInvalidated{ArtifactId::from_value(1), ArtifactGeneration::first(),
                                     FailureId::from_value(1), DecisionId::from_value(1), "stale"},
                 Provenance::Reconstructed)
            .body,
        metric_declared(1, 1, "accuracy").body,
        observation_recorded(1, 1, "accuracy", MetricValue::ratio(0.5).value(), UnitKind::Ratio).body,
        failure_recorded(1, SubjectId::of(AttemptId::from_value(1)), FailureCategory::Model,
                         "model refused")
            .body,
        decision_recorded(1, 1, SubjectId::of(ResultId::from_value(1)), DecisionType::Accept,
                          DecisionOutcome::Accepted,
                          {EvidenceRef{SubjectId::of(ResultId::from_value(1)),
                                       RecordSequence::from_value(1)}})
            .body,
        result_declared(1, 1, 1, 1, {ArtifactId::from_value(1)}, {ObservationId::from_value(1)}).body,
        result_status_changed(1, 2, ResultStatus::Candidate, ResultStatus::Accepted, 1).body,
        accounting_recorded(SubjectId::of(AttemptId::from_value(1)),
                            []() {
                                AccountingVector accounting;
                                accounting.model_input_tokens =
                                    InputTokens::known(10, Provenance::Measured);
                                accounting.monetary = MonetaryMeasure::known_amount(-5, "USD",
                                                                                    Provenance::Reported);
                                return accounting;
                            }())
            .body,
    };
    RL_CHECK_EQ(bodies.size(), static_cast<std::size_t>(kRecordTypeCount));

    for (const RecordBody& body : bodies) {
        auto encoded = encode_record_body(body, limits);
        RL_CHECK_OK(encoded);
        auto decoded = decode_record_body(encoded.value(), limits);
        RL_CHECK_OK(decoded);
        RL_CHECK(record_type_of(decoded.value()) == record_type_of(body));
        // A payload that does not match its declared type is rejected.
        std::vector<std::byte> corrupted = encoded.value();
        corrupted[1] = std::byte(0x7f);
        auto wrong_type = decode_record_body(corrupted, limits);
        RL_CHECK(!wrong_type.ok());
    }

    // Trailing bytes after a payload are rejected.
    auto encoded = encode_record_body(bodies[0], limits);
    RL_CHECK_OK(encoded);
    std::vector<std::byte> extended = encoded.value();
    extended.push_back(std::byte{0});
    RL_CHECK(!decode_record_body(extended, limits).ok());

    // Truncation at any offset is rejected rather than silently accepted.
    for (std::size_t length = 0; length < encoded.value().size(); ++length) {
        const std::span<const std::byte> prefix(encoded.value().data(), length);
        RL_CHECK(!decode_record_body(prefix, limits).ok());
    }
}

RL_TEST(a_draft_survives_its_round_trip) {
    Limits limits;
    RecordDraft draft = session_opened(4, "label", "question");
    draft.record_id = LedgerRecordId::from_value(1234);
    draft.idempotent = true;
    auto encoded = encode_draft(draft, limits);
    RL_CHECK_OK(encoded);
    auto decoded = decode_draft(encoded.value(), limits);
    RL_CHECK_OK(decoded);
    RL_CHECK_EQ(decoded.value().record_id.value(), 1234u);
    RL_CHECK(decoded.value().idempotent);
    RL_CHECK_EQ(std::get<SessionOpened>(decoded.value().body).label, std::string("label"));
}

}  // namespace
}  // namespace research_ledger
