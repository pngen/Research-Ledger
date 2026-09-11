#include "test_framework.hpp"
#include "test_support.hpp"

#include "research_ledger/evidence.hpp"
#include "research_ledger/ledger.hpp"

#include <cstdint>
#include <memory>
#include <string>
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

// --- fixtures ---------------------------------------------------------------

// A committed baseline that stops before any decision: session 1, hypothesis 1,
// branch 1, experiment 1, a running attempt 1, artifact 1 and result 1 still in
// the candidate state.
inline Result<std::shared_ptr<Ledger>> ledger_with_candidate(const LedgerConfig& config) {
    auto created = Ledger::create(config);
    if (!created.ok()) {
        return created.status();
    }
    std::shared_ptr<Ledger> ledger = created.take();
    const std::vector<RecordDraft> batch{
        session_opened(1),
        hypothesis_declared(1, 1, "the ledger reconstructs accepted results"),
        branch_declared(1, 1, BranchKind::Root),
        experiment_declared(1, 1, 1, 1),
        attempt_started(1, 1, 1),
        artifact_referenced(1, 1, SubjectId::of(AttemptId::from_value(1)), ArtifactRole::Dataset,
                            "dataset"),
        result_declared(1, 1, 1, 1, {ArtifactId::from_value(1)})};
    auto appended = ledger->append_batch(batch);
    if (!appended.ok()) {
        return appended.status();
    }
    return ledger;
}

inline Result<AppendOutcome> declare_result(const std::shared_ptr<Ledger>& ledger,
                                            std::uint64_t result, std::uint64_t experiment) {
    return ledger->append(result_declared(result, 1, 1, experiment, {ArtifactId::from_value(1)}));
}

inline Result<AppendOutcome> record_decision(const std::shared_ptr<Ledger>& ledger,
                                             std::uint64_t decision, SubjectId subject,
                                             DecisionType type, DecisionOutcome outcome) {
    return ledger->append(decision_recorded(decision, 1, subject, type, outcome));
}

inline Result<AppendOutcome> move_result(const std::shared_ptr<Ledger>& ledger,
                                         std::uint64_t result, std::uint32_t generation,
                                         ResultStatus from, ResultStatus to,
                                         std::uint64_t decision) {
    return ledger->append(result_status_changed(result, generation, from, to, decision));
}

// Accepts a candidate result through its own explicit decision.
inline Result<AppendOutcome> accept_result(const std::shared_ptr<Ledger>& ledger,
                                           std::uint64_t result, std::uint64_t decision) {
    auto recorded = record_decision(ledger, decision, SubjectId::of(ResultId::from_value(result)),
                                    DecisionType::Accept, DecisionOutcome::Accepted);
    if (!recorded.ok()) {
        return recorded.status();
    }
    return move_result(ledger, result, 2, ResultStatus::Candidate, ResultStatus::Accepted, decision);
}

inline RecordSequence sequence_of(std::uint64_t value) {
    return RecordSequence::from_value(static_cast<std::uint32_t>(value));
}

inline RecordDraft hypothesis_change(std::uint64_t hypothesis, std::uint32_t generation,
                                     HypothesisStatus from, HypothesisStatus to) {
    HypothesisStatusChanged body;
    body.hypothesis = HypothesisId::from_value(hypothesis);
    body.generation = HypothesisGeneration::from_value(generation);
    body.from = from;
    body.to = to;
    body.authority = "researcher";
    return draft_of(body, Provenance::Reported);
}

// --- acceptance is an explicit decision -------------------------------------

RL_TEST(lifecycle_acceptance_requires_an_explicit_decision) {
    auto created = ledger_with_candidate(local_config());
    RL_CHECK_OK(created);
    const std::shared_ptr<Ledger>& book = created.value();
    const SubjectId result_subject = SubjectId::of(ResultId::from_value(1));

    // A status change that names no decision at all is refused.
    RL_CHECK_CODE(move_result(book, 1, 2, ResultStatus::Candidate, ResultStatus::Accepted, 0),
                  ErrorCode::MissingDependency);

    // A decision committed after the change, in the same batch, did not
    // authorize it: the change is validated against history, not intent.
    RL_CHECK_CODE(book->append_batch(std::vector<RecordDraft>{
                      result_status_changed(1, 2, ResultStatus::Candidate, ResultStatus::Accepted,
                                            9),
                      decision_recorded(9, 1, result_subject, DecisionType::Accept,
                                        DecisionOutcome::Accepted)}),
                  ErrorCode::MissingDependency);

    // A decision about another subject does not authorize this result.
    RL_CHECK_OK(record_decision(book, 1, SubjectId::of(HypothesisId::from_value(1)),
                                DecisionType::HypothesisTransition, DecisionOutcome::Recorded));
    RL_CHECK_CODE(move_result(book, 1, 2, ResultStatus::Candidate, ResultStatus::Accepted, 1),
                  ErrorCode::InvalidTransition);

    // A decision whose outcome does not authorize acceptance is refused even
    // when it concerns this result.
    RL_CHECK_OK(record_decision(book, 2, result_subject, DecisionType::Reject,
                                DecisionOutcome::Rejected));
    RL_CHECK_CODE(move_result(book, 1, 2, ResultStatus::Candidate, ResultStatus::Accepted, 2),
                  ErrorCode::InvalidTransition);

    // The right decision with the wrong generation is a stale generation.
    RL_CHECK_OK(record_decision(book, 3, result_subject, DecisionType::Accept,
                                DecisionOutcome::Accepted));
    RL_CHECK_CODE(move_result(book, 1, 1, ResultStatus::Candidate, ResultStatus::Accepted, 3),
                  ErrorCode::StaleGeneration);
    RL_CHECK_CODE(move_result(book, 1, 7, ResultStatus::Candidate, ResultStatus::Accepted, 3),
                  ErrorCode::StaleGeneration);

    // The change must also start from the status the result actually holds.
    RL_CHECK_CODE(move_result(book, 1, 2, ResultStatus::Rejected, ResultStatus::Accepted, 3),
                  ErrorCode::InvalidTransition);

    // No refusal committed anything, and no refusal was idempotent.
    {
        auto snapshot = book->snapshot();
        const ResultView view = expect(snapshot.result(ResultId::from_value(1)), "result 1");
        RL_CHECK(view.status == ResultStatus::Candidate);
        RL_CHECK(!view.acceptance_decision.has_value());
        RL_CHECK(!view.last_decision.has_value());
        const std::vector<DecisionView> decisions = expect(snapshot.decisions_of(result_subject),
                                                           "decisions of result 1");
        RL_CHECK_EQ(decisions.size(), 2u);
        RL_CHECK_EQ(book->last_sequence().value(), 10u);
    }

    // The explicit decision plus the transition it authorizes is accepted.
    RL_CHECK_OK(move_result(book, 1, 2, ResultStatus::Candidate, ResultStatus::Accepted, 3));
    {
        auto snapshot = book->snapshot();
        const ResultView view = expect(snapshot.result(ResultId::from_value(1)), "result 1");
        RL_CHECK(view.status == ResultStatus::Accepted);
        RL_CHECK(view.acceptance_decision.has_value());
        RL_CHECK_EQ(view.acceptance_decision->value(), 3u);
        RL_CHECK(view.last_decision.has_value());
        RL_CHECK_EQ(view.last_decision->value(), 3u);
        RL_CHECK_EQ(view.generation.value(), 2u);
    }
}

// --- forbidden transitions --------------------------------------------------

RL_TEST(lifecycle_forbidden_result_transitions_are_rejected) {
    auto created = ledger_with_candidate(local_config());
    RL_CHECK_OK(created);
    const std::shared_ptr<Ledger>& book = created.value();
    const auto subject = [](std::uint64_t result) {
        return SubjectId::of(ResultId::from_value(result));
    };

    // Result 1: candidate -> accepted -> superseded.
    RL_CHECK_OK(accept_result(book, 1, 1));
    RL_CHECK_OK(record_decision(book, 2, subject(1), DecisionType::Supersede,
                                DecisionOutcome::Superseded));
    RL_CHECK_OK(move_result(book, 1, 3, ResultStatus::Accepted, ResultStatus::Superseded, 2));

    // Result 2: candidate -> rejected.
    RL_CHECK_OK(declare_result(book, 2, 1));
    RL_CHECK_OK(record_decision(book, 3, subject(2), DecisionType::Reject,
                                DecisionOutcome::Rejected));
    RL_CHECK_OK(move_result(book, 2, 2, ResultStatus::Candidate, ResultStatus::Rejected, 3));

    // Result 3: candidate -> accepted -> retracted.
    RL_CHECK_OK(declare_result(book, 3, 1));
    RL_CHECK_OK(accept_result(book, 3, 4));
    RL_CHECK_OK(record_decision(book, 5, subject(3), DecisionType::Retract,
                                DecisionOutcome::Retracted));
    RL_CHECK_OK(move_result(book, 3, 3, ResultStatus::Accepted, ResultStatus::Retracted, 5));

    // A rejected result never becomes accepted directly: reclassification to
    // candidate comes first, and acceptance then needs its own decision.
    RL_CHECK_OK(record_decision(book, 6, subject(2), DecisionType::Accept,
                                DecisionOutcome::Accepted));
    RL_CHECK_CODE(move_result(book, 2, 3, ResultStatus::Rejected, ResultStatus::Accepted, 6),
                  ErrorCode::InvalidTransition);

    // A superseded result never becomes accepted again.
    RL_CHECK_CODE(move_result(book, 1, 4, ResultStatus::Superseded, ResultStatus::Accepted, 6),
                  ErrorCode::InvalidTransition);

    // A retracted result acquires nothing.
    RL_CHECK_OK(record_decision(book, 7, subject(3), DecisionType::Supersede,
                                DecisionOutcome::Superseded));
    RL_CHECK_CODE(move_result(book, 3, 4, ResultStatus::Retracted, ResultStatus::Superseded, 7),
                  ErrorCode::InvalidTransition);
    RL_CHECK_OK(record_decision(book, 8, subject(3), DecisionType::Invalidate,
                                DecisionOutcome::Invalidated));
    RL_CHECK_CODE(move_result(book, 3, 4, ResultStatus::Retracted, ResultStatus::Invalidated, 8),
                  ErrorCode::InvalidTransition);
    RL_CHECK_CODE(move_result(book, 3, 4, ResultStatus::Retracted, ResultStatus::Accepted, 6),
                  ErrorCode::InvalidTransition);
    RL_CHECK_CODE(move_result(book, 3, 4, ResultStatus::Retracted, ResultStatus::Retracted, 8),
                  ErrorCode::InvalidTransition);

    // The same state is not a transition.
    RL_CHECK_CODE(move_result(book, 1, 4, ResultStatus::Superseded, ResultStatus::Superseded, 0),
                  ErrorCode::InvalidTransition);
    RL_CHECK_CODE(move_result(book, 2, 3, ResultStatus::Rejected, ResultStatus::Rejected, 0),
                  ErrorCode::InvalidTransition);

    // Generations are consecutive: a skip and a repeat are both stale.
    RL_CHECK_CODE(move_result(book, 1, 6, ResultStatus::Superseded, ResultStatus::Invalidated, 8),
                  ErrorCode::StaleGeneration);
    RL_CHECK_CODE(move_result(book, 1, 3, ResultStatus::Superseded, ResultStatus::Invalidated, 8),
                  ErrorCode::StaleGeneration);

    {
        auto snapshot = book->snapshot();
        const ResultView first = expect(snapshot.result(ResultId::from_value(1)), "result 1");
        const ResultView second = expect(snapshot.result(ResultId::from_value(2)), "result 2");
        const ResultView third = expect(snapshot.result(ResultId::from_value(3)), "result 3");
        RL_CHECK(first.status == ResultStatus::Superseded);
        RL_CHECK(second.status == ResultStatus::Rejected);
        RL_CHECK(third.status == ResultStatus::Retracted);
        RL_CHECK_EQ(first.generation.value(), 3u);
        RL_CHECK_EQ(second.generation.value(), 2u);
        RL_CHECK_EQ(third.generation.value(), 3u);
    }
}

// --- supersession keeps the decision lineage --------------------------------

RL_TEST(lifecycle_supersession_preserves_the_superseded_result) {
    auto created = ledger_with_candidate(local_config());
    RL_CHECK_OK(created);
    const std::shared_ptr<Ledger>& book = created.value();
    const SubjectId result_subject = SubjectId::of(ResultId::from_value(1));

    RL_CHECK_OK(accept_result(book, 1, 1));
    RL_CHECK_OK(record_decision(book, 2, result_subject, DecisionType::Supersede,
                                DecisionOutcome::Superseded));
    RL_CHECK_OK(move_result(book, 1, 3, ResultStatus::Accepted, ResultStatus::Superseded, 2));

    auto snapshot = book->snapshot();
    const ResultView view = expect(snapshot.result(ResultId::from_value(1)), "result 1");
    // The superseded result keeps its own acceptance lineage: supersession is a
    // later decision, it does not erase the earlier one.
    RL_CHECK(view.status == ResultStatus::Superseded);
    RL_CHECK(view.acceptance_decision.has_value());
    RL_CHECK_EQ(view.acceptance_decision->value(), 1u);
    RL_CHECK(view.last_decision.has_value());
    RL_CHECK_EQ(view.last_decision->value(), 2u);
    RL_CHECK_EQ(view.declared_at.value(), 7u);

    const std::vector<DecisionView> decisions = expect(snapshot.decisions_of(result_subject),
                                                       "decisions of result 1");
    RL_CHECK_EQ(decisions.size(), 2u);
    RL_CHECK(decisions[0].outcome == DecisionOutcome::Accepted);
    RL_CHECK(decisions[1].outcome == DecisionOutcome::Superseded);
    RL_CHECK(decisions[0].recorded_at < decisions[1].recorded_at);

    // Both decisions are still readable on their own, and the acceptance
    // decision still reads as an acceptance.
    const DecisionView acceptance = expect(snapshot.decision(DecisionId::from_value(1)),
                                           "decision 1");
    RL_CHECK(acceptance.type == DecisionType::Accept);
    RL_CHECK(acceptance.outcome == DecisionOutcome::Accepted);
    RL_CHECK(acceptance.subject == result_subject);

    // The acceptance decision record is itself still in committed history.
    const RecordSequence sequence =
        expect(snapshot.subject_sequence(SubjectId::of(DecisionId::from_value(1))),
               "decision 1 sequence");
    const Record record = expect(snapshot.record_at(sequence), "decision 1 record");
    RL_CHECK(record.header.type == RecordType::DecisionRecorded);
    const DecisionRecorded* body = std::get_if<DecisionRecorded>(&record.body);
    RL_CHECK(body != nullptr);
    if (body != nullptr) {
        RL_CHECK(body->outcome == DecisionOutcome::Accepted);
        RL_CHECK(body->decision == DecisionId::from_value(1));
    }

    // The evidence closure still contains both decisions.
    const EvidenceBundle bundle = expect(snapshot.supporting_evidence(ResultId::from_value(1)),
                                         "supporting evidence");
    RL_CHECK_EQ(bundle.decisions.size(), 2u);
    RL_CHECK(!bundle.accepted());
}

RL_TEST(lifecycle_invalidation_and_retraction_keep_the_original_event) {
    auto created = ledger_with_candidate(local_config());
    RL_CHECK_OK(created);
    const std::shared_ptr<Ledger>& book = created.value();
    const SubjectId result_subject = SubjectId::of(ResultId::from_value(1));

    RL_CHECK_OK(accept_result(book, 1, 1));
    const RecordSequence declared_at =
        expect(book->snapshot().result(ResultId::from_value(1)), "result 1").declared_at;

    RL_CHECK_OK(record_decision(book, 2, result_subject, DecisionType::Invalidate,
                                DecisionOutcome::Invalidated));
    RL_CHECK_OK(move_result(book, 1, 3, ResultStatus::Accepted, ResultStatus::Invalidated, 2));
    {
        auto snapshot = book->snapshot();
        const ResultView view = expect(snapshot.result(ResultId::from_value(1)), "result 1");
        RL_CHECK(view.status == ResultStatus::Invalidated);
        RL_CHECK(view.acceptance_decision.has_value());
        RL_CHECK_EQ(view.acceptance_decision->value(), 1u);
        RL_CHECK(view.last_decision.has_value());
        RL_CHECK_EQ(view.last_decision->value(), 2u);
        RL_CHECK(view.declared_at == declared_at);
    }

    RL_CHECK_OK(record_decision(book, 3, result_subject, DecisionType::Retract,
                                DecisionOutcome::Retracted));
    RL_CHECK_OK(move_result(book, 1, 4, ResultStatus::Invalidated, ResultStatus::Retracted, 3));
    {
        auto snapshot = book->snapshot();
        const ResultView view = expect(snapshot.result(ResultId::from_value(1)), "result 1");
        RL_CHECK(view.status == ResultStatus::Retracted);
        // The original acceptance survives the whole later history.
        RL_CHECK(view.acceptance_decision.has_value());
        RL_CHECK_EQ(view.acceptance_decision->value(), 1u);
        RL_CHECK(view.last_decision.has_value());
        RL_CHECK_EQ(view.last_decision->value(), 3u);
        RL_CHECK(view.declared_at == declared_at);

        const std::vector<DecisionView> decisions = expect(snapshot.decisions_of(result_subject),
                                                           "decisions of result 1");
        RL_CHECK_EQ(decisions.size(), 3u);
        RL_CHECK(decisions[0].outcome == DecisionOutcome::Accepted);
        RL_CHECK(decisions[1].outcome == DecisionOutcome::Invalidated);
        RL_CHECK(decisions[2].outcome == DecisionOutcome::Retracted);

        // Every decision record is still committed, in order, each one an
        // append to history rather than a rewrite of it.
        std::uint64_t previous = 0;
        for (const DecisionView& decision : decisions) {
            const Record record = expect(snapshot.record_at(decision.recorded_at), "decision record");
            RL_CHECK(record.header.type == RecordType::DecisionRecorded);
            RL_CHECK(decision.recorded_at.value() > previous);
            previous = decision.recorded_at.value();
        }
    }
}

// --- hypothesis lifecycle ---------------------------------------------------

RL_TEST(lifecycle_hypothesis_progress_is_generation_checked) {
    auto created = ledger_with_candidate(local_config());
    RL_CHECK_OK(created);
    const std::shared_ptr<Ledger>& book = created.value();

    const auto change = [&book](std::uint64_t hypothesis, std::uint32_t generation,
                                HypothesisStatus from, HypothesisStatus to) {
        HypothesisStatusChanged body;
        body.hypothesis = HypothesisId::from_value(hypothesis);
        body.generation = HypothesisGeneration::from_value(generation);
        body.from = from;
        body.to = to;
        body.authority = "researcher";
        return book->append(draft_of(body, Provenance::Reported));
    };

    // proposed -> active -> supported.
    RL_CHECK_OK(change(1, 2, HypothesisStatus::Proposed, HypothesisStatus::Active));
    RL_CHECK_OK(change(1, 3, HypothesisStatus::Active, HypothesisStatus::Supported));
    {
        auto snapshot = book->snapshot();
        const HypothesisView view = expect(snapshot.hypothesis(HypothesisId::from_value(1)),
                                           "hypothesis 1");
        RL_CHECK(view.status == HypothesisStatus::Supported);
        RL_CHECK_EQ(view.generation.value(), 3u);
        RL_CHECK_EQ(view.claim, std::string("the ledger reconstructs accepted results"));
    }

    // Transitions require the next generation and must start from the status
    // the hypothesis actually holds.
    RL_CHECK_CODE(change(1, 3, HypothesisStatus::Supported, HypothesisStatus::Superseded),
                  ErrorCode::StaleGeneration);
    RL_CHECK_CODE(change(1, 5, HypothesisStatus::Supported, HypothesisStatus::Superseded),
                  ErrorCode::StaleGeneration);
    RL_CHECK_CODE(change(1, 4, HypothesisStatus::Active, HypothesisStatus::Superseded),
                  ErrorCode::InvalidTransition);

    // Supersession keeps the declaration the hypothesis was made with.
    RL_CHECK_OK(change(1, 4, HypothesisStatus::Supported, HypothesisStatus::Superseded));
    {
        auto snapshot = book->snapshot();
        const HypothesisView view = expect(snapshot.hypothesis(HypothesisId::from_value(1)),
                                           "hypothesis 1");
        RL_CHECK(view.status == HypothesisStatus::Superseded);
        RL_CHECK_EQ(view.declared_at.value(), 2u);
        RL_CHECK(view.last_changed_at > view.declared_at);
        const std::vector<HypothesisId> ancestry =
            expect(snapshot.hypothesis_ancestry(HypothesisId::from_value(1)), "ancestry");
        RL_CHECK_EQ(ancestry.size(), 1u);
        RL_CHECK(ancestry[0] == HypothesisId::from_value(1));
    }
    // Superseded is terminal for a hypothesis: a superseded claim is not
    // silently revived, and the decision does not change that.
    RL_CHECK_OK(record_decision(book, 1, SubjectId::of(HypothesisId::from_value(1)),
                                DecisionType::HypothesisTransition, DecisionOutcome::Recorded));
    RL_CHECK_CODE(change(1, 5, HypothesisStatus::Superseded, HypothesisStatus::Active),
                  ErrorCode::InvalidTransition);
    RL_CHECK_CODE(change(1, 5, HypothesisStatus::Superseded, HypothesisStatus::Rejected),
                  ErrorCode::InvalidTransition);

    // not_supported is evidence against a claim, not a rejection of it.
    RL_CHECK_OK(book->append(hypothesis_declared(2, 1, "a second claim")));
    RL_CHECK_OK(change(2, 2, HypothesisStatus::Proposed, HypothesisStatus::Active));
    RL_CHECK_OK(change(2, 3, HypothesisStatus::Active, HypothesisStatus::NotSupported));
    {
        auto snapshot = book->snapshot();
        const HypothesisView view = expect(snapshot.hypothesis(HypothesisId::from_value(2)),
                                           "hypothesis 2");
        RL_CHECK(view.status == HypothesisStatus::NotSupported);
    }
    RL_CHECK_OK(change(2, 4, HypothesisStatus::NotSupported, HypothesisStatus::Supported));

    // A rejected hypothesis is terminal except for supersession.
    RL_CHECK_OK(book->append(hypothesis_declared(3, 1, "a rejected claim")));
    RL_CHECK_OK(change(3, 2, HypothesisStatus::Proposed, HypothesisStatus::Rejected));
    RL_CHECK_CODE(change(3, 3, HypothesisStatus::Rejected, HypothesisStatus::Supported),
                  ErrorCode::InvalidTransition);
    RL_CHECK_CODE(change(3, 3, HypothesisStatus::Rejected, HypothesisStatus::Active),
                  ErrorCode::InvalidTransition);
    RL_CHECK_OK(change(3, 3, HypothesisStatus::Rejected, HypothesisStatus::Superseded));

    {
        auto snapshot = book->snapshot();
        const HypothesisView second = expect(snapshot.hypothesis(HypothesisId::from_value(2)),
                                             "hypothesis 2");
        const HypothesisView third = expect(snapshot.hypothesis(HypothesisId::from_value(3)),
                                            "hypothesis 3");
        RL_CHECK(second.status == HypothesisStatus::Supported);
        RL_CHECK(third.status == HypothesisStatus::Superseded);
        RL_CHECK_EQ(second.generation.value(), 4u);
        RL_CHECK_EQ(third.generation.value(), 3u);
    }
}

RL_TEST(lifecycle_an_experiment_failure_does_not_reject_the_hypothesis) {
    auto created = ledger_with_candidate(local_config());
    RL_CHECK_OK(created);
    const std::shared_ptr<Ledger>& book = created.value();

    // Hypothesis 1 is active while its experiment runs.
    HypothesisStatusChanged active;
    active.hypothesis = HypothesisId::from_value(1);
    active.generation = HypothesisGeneration::from_value(2);
    active.from = HypothesisStatus::Proposed;
    active.to = HypothesisStatus::Active;
    active.authority = "researcher";
    RL_CHECK_OK(book->append(draft_of(active, Provenance::Reported)));

    // The attempt fails and the failure is recorded against the attempt.
    RL_CHECK_OK(book->append(failure_recorded(1, SubjectId::of(AttemptId::from_value(1)),
                                              FailureCategory::Execution, "worker died")));
    RL_CHECK_OK(book->append(attempt_failed(1, 1)));
    {
        auto snapshot = book->snapshot();
        const AttemptView attempt = expect(snapshot.attempt(AttemptId::from_value(1)), "attempt 1");
        RL_CHECK(attempt.state == AttemptState::Failed);
        RL_CHECK(attempt.failure.has_value());
        RL_CHECK_EQ(attempt.failure->value(), 1u);

        // The hypothesis is untouched: an experiment outcome is evidence, not a
        // decision about the claim.
        const HypothesisView hypothesis = expect(snapshot.hypothesis(HypothesisId::from_value(1)),
                                                 "hypothesis 1");
        RL_CHECK(hypothesis.status == HypothesisStatus::Active);
        RL_CHECK_EQ(hypothesis.generation.value(), 2u);
        const std::vector<DecisionView> decisions =
            expect(snapshot.decisions_of(SubjectId::of(HypothesisId::from_value(1))),
                   "decisions of hypothesis 1");
        RL_CHECK_EQ(decisions.size(), 0u);
        const std::vector<FailureView> failures =
            expect(snapshot.failures_of(SubjectId::of(AttemptId::from_value(1))),
                   "failures of attempt 1");
        RL_CHECK_EQ(failures.size(), 1u);
    }
}

// --- branch lifecycle -------------------------------------------------------

RL_TEST(lifecycle_branches_stay_queryable_and_unresolved_is_explicit) {
    auto created = ledger_with_candidate(local_config());
    RL_CHECK_OK(created);
    const std::shared_ptr<Ledger>& book = created.value();

    // Branch 1 carries an attempt with no completion: it is unresolved.
    {
        auto snapshot = book->snapshot();
        const std::vector<BranchId> branches =
            expect(snapshot.unresolved_branches(ResearchSessionId::from_value(1)), "unresolved");
        RL_CHECK_EQ(branches.size(), 1u);
        RL_CHECK(branches[0] == BranchId::from_value(1));
        const BranchView view = expect(snapshot.branch(BranchId::from_value(1)), "branch 1");
        RL_CHECK(view.unresolved);
        RL_CHECK_EQ(view.attempt_count, 1u);
    }

    // A branch is abandoned by a decision. The branch, its experiment and its
    // attempt stay queryable, and abandonment is not completion.
    RL_CHECK_OK(record_decision(book, 1, SubjectId::of(BranchId::from_value(1)),
                                DecisionType::BranchAbandonment, DecisionOutcome::Recorded));
    {
        auto snapshot = book->snapshot();
        const BranchView view = expect(snapshot.branch(BranchId::from_value(1)), "branch 1");
        RL_CHECK(view.kind == BranchKind::Root);
        RL_CHECK_EQ(view.label, std::string("branch"));
        RL_CHECK_EQ(view.experiments.size(), 1u);
        const std::vector<AttemptView> attempts =
            expect(snapshot.branch_attempts(BranchId::from_value(1)), "branch 1 attempts");
        RL_CHECK_EQ(attempts.size(), 1u);
        RL_CHECK(attempts[0].state == AttemptState::Running);
        const std::vector<BranchId> branches =
            expect(snapshot.unresolved_branches(ResearchSessionId::from_value(1)), "unresolved");
        RL_CHECK_EQ(branches.size(), 1u);
        const DecisionView abandonment = expect(snapshot.decision(DecisionId::from_value(1)),
                                                "abandonment decision");
        RL_CHECK(abandonment.type == DecisionType::BranchAbandonment);
        RL_CHECK(abandonment.outcome == DecisionOutcome::Recorded);
    }

    // Merged evidence is a branch of its own that names the branch it merges.
    RL_CHECK_OK(book->append(branch_declared(2, 1, BranchKind::MergedEvidence, 1)));
    RL_CHECK_OK(book->append(experiment_declared(2, 1, 1, 2)));
    RL_CHECK_OK(book->append(attempt_started(2, 2, 2)));
    {
        auto snapshot = book->snapshot();
        const BranchView merged = expect(snapshot.branch(BranchId::from_value(2)), "branch 2");
        RL_CHECK(merged.kind == BranchKind::MergedEvidence);
        RL_CHECK(merged.parent_branch.has_value());
        RL_CHECK_EQ(merged.parent_branch->value(), 1u);
        RL_CHECK(merged.unresolved);
        const std::vector<BranchId> ancestry =
            expect(snapshot.branch_ancestry(BranchId::from_value(2)), "branch 2 ancestry");
        RL_CHECK_EQ(ancestry.size(), 2u);
        RL_CHECK(ancestry[0] == BranchId::from_value(2));
        RL_CHECK(ancestry[1] == BranchId::from_value(1));
        const std::vector<BranchId> branches =
            expect(snapshot.unresolved_branches(ResearchSessionId::from_value(1)), "unresolved");
        RL_CHECK_EQ(branches.size(), 2u);
    }

    // Completing the merged branch and accepting a result of its experiment
    // resolves that branch only.
    RL_CHECK_OK(book->append(attempt_completed(2, "merged outcome")));
    RL_CHECK_OK(declare_result(book, 2, 2));
    RL_CHECK_OK(accept_result(book, 2, 2));
    {
        auto snapshot = book->snapshot();
        const std::vector<BranchId> branches =
            expect(snapshot.unresolved_branches(ResearchSessionId::from_value(1)), "unresolved");
        RL_CHECK_EQ(branches.size(), 1u);
        RL_CHECK(branches[0] == BranchId::from_value(1));
        const BranchView merged = expect(snapshot.branch(BranchId::from_value(2)), "branch 2");
        RL_CHECK(!merged.unresolved);
        RL_CHECK_EQ(merged.attempt_count, 1u);
        const BranchView root = expect(snapshot.branch(BranchId::from_value(1)), "branch 1");
        RL_CHECK(root.unresolved);
    }
}

// --- decisions are temporal -------------------------------------------------

RL_TEST(lifecycle_decisions_cite_only_committed_earlier_evidence) {
    auto created = ledger_with_candidate(local_config());
    RL_CHECK_OK(created);
    const std::shared_ptr<Ledger>& book = created.value();

    const SubjectId result_subject = SubjectId::of(ResultId::from_value(1));
    const SubjectId artifact_subject = SubjectId::of(ArtifactId::from_value(1));
    auto snapshot = book->snapshot();
    const RecordSequence result_sequence =
        expect(snapshot.subject_sequence(result_subject), "result sequence");
    const RecordSequence artifact_sequence =
        expect(snapshot.subject_sequence(artifact_subject), "artifact sequence");
    const std::uint64_t last = book->last_sequence().value();

    const auto decide_about_result = [&book, result_subject](std::uint64_t decision,
                                                             std::vector<EvidenceRef> evidence) {
        return book->append(decision_recorded(decision, 1, result_subject, DecisionType::Audit,
                                              DecisionOutcome::Recorded, std::move(evidence)));
    };

    // Evidence that is not committed yet cannot be cited.
    RL_CHECK_CODE(decide_about_result(1, {EvidenceRef{result_subject, sequence_of(last + 1)}}),
                  ErrorCode::InvalidTransition);
    // Neither can a sequence that does not concern the cited subject.
    RL_CHECK_CODE(decide_about_result(1, {EvidenceRef{SubjectId::of(HypothesisId::from_value(1)), artifact_sequence}}),
                  ErrorCode::BrokenLineage);
    // Evidence from another research session cannot be cited.
    RL_CHECK_OK(book->append(session_opened(2, "other session")));
    RL_CHECK_OK(book->append(hypothesis_declared(2, 2, "other claim")));
    const RecordSequence other_sequence =
        expect(book->snapshot().subject_sequence(SubjectId::of(HypothesisId::from_value(2))),
               "other hypothesis sequence");
    RL_CHECK_CODE(decide_about_result(1, {EvidenceRef{SubjectId::of(HypothesisId::from_value(2)), other_sequence}}),
                  ErrorCode::CrossSessionReference);

    // A decision made now cites the result as it stood when the decision was made.
    RL_CHECK_OK(decide_about_result(1, {EvidenceRef{result_subject, result_sequence}}));

    // Evidence committed after that decision exists, but it is not evidence the
    // earlier decision considered.
    RL_CHECK_OK(book->append(artifact_referenced(2, 1, SubjectId::of(AttemptId::from_value(1)),
                                                 ArtifactRole::Intermediate, "later artifact",
                                                 {ArtifactId::from_value(1)})));
    const RecordSequence later_sequence =
        expect(book->snapshot().subject_sequence(SubjectId::of(ArtifactId::from_value(2))),
               "later artifact sequence");
    RL_CHECK(later_sequence.value() > result_sequence.value());
    {
        const DecisionView view = expect(book->snapshot().decision(DecisionId::from_value(1)),
                                         "decision 1");
        RL_CHECK_EQ(view.evidence.size(), 1u);
        RL_CHECK(view.evidence[0].sequence == result_sequence);
        RL_CHECK(view.recorded_at < later_sequence);
    }

    // A later decision may cite the later evidence.
    RL_CHECK_OK(book->append(decision_recorded(
        2, 1, SubjectId::of(ArtifactId::from_value(2)), DecisionType::Audit,
        DecisionOutcome::Recorded,
        {EvidenceRef{SubjectId::of(ArtifactId::from_value(2)), later_sequence}})));
    {
        auto later_snapshot = book->snapshot();
        const DecisionView later = expect(later_snapshot.decision(DecisionId::from_value(2)),
                                          "decision 2");
        RL_CHECK_EQ(later.evidence.size(), 1u);
        RL_CHECK(later.evidence[0].sequence == later_sequence);
        const DecisionView earlier = expect(later_snapshot.decision(DecisionId::from_value(1)),
                                            "decision 1");
        RL_CHECK_EQ(earlier.evidence.size(), 1u);
        RL_CHECK(earlier.evidence[0].sequence == result_sequence);
    }
}

// --- attempt lifecycle ------------------------------------------------------

RL_TEST(lifecycle_attempts_terminate_once_and_retry_explicitly) {
    auto created = ledger_with_candidate(local_config());
    RL_CHECK_OK(created);
    const std::shared_ptr<Ledger>& book = created.value();

    // Attempt 1 completes; a second terminal record is a duplicate completion.
    RL_CHECK_OK(book->append(attempt_completed(1)));
    RL_CHECK_CODE(book->append(attempt_completed(1)), ErrorCode::DuplicateCompletion);
    RL_CHECK_CODE(book->append(attempt_failed(1, 1)), ErrorCode::AlreadyTerminal);
    RL_CHECK_CODE(book->append(attempt_cancelled(1)), ErrorCode::AlreadyTerminal);

    // A terminal record carrying a generation that is not current is stale.
    AttemptCompleted stale;
    stale.attempt = AttemptId::from_value(1);
    stale.generation = AttemptGeneration::from_value(2);
    stale.outcome_reference = "stale";
    RL_CHECK_CODE(book->append(draft_of(stale, Provenance::Reported)), ErrorCode::StaleAttempt);

    // A failed attempt stays failed, and the work it produced stays recorded.
    RL_CHECK_OK(book->append(attempt_started(2, 1, 1)));
    RL_CHECK_OK(book->append(model_call_recorded(1, 2)));
    RL_CHECK_OK(book->append(failure_recorded(1, SubjectId::of(AttemptId::from_value(2)),
                                              FailureCategory::Infrastructure, "node lost")));
    RL_CHECK_OK(book->append(attempt_failed(2, 1)));
    RL_CHECK_CODE(book->append(attempt_failed(2, 1)), ErrorCode::DuplicateCompletion);
    RL_CHECK_CODE(book->append(attempt_completed(2)), ErrorCode::AlreadyTerminal);

    // A failure recorded against a different attempt does not terminate this one.
    RL_CHECK_OK(book->append(attempt_started(3, 1, 1)));
    RL_CHECK_OK(book->append(failure_recorded(2, SubjectId::of(AttemptId::from_value(1)),
                                              FailureCategory::Execution,
                                              "someone else's failure")));
    RL_CHECK_CODE(book->append(attempt_failed(3, 2)), ErrorCode::InvalidTransition);
    RL_CHECK_CODE(book->append(attempt_failed(3, 99)), ErrorCode::MissingDependency);

    // A cancelled attempt acquires no new authoritative activity and never
    // becomes a different terminal state.
    RL_CHECK_OK(book->append(attempt_cancelled(3, "operator cancelled")));
    RL_CHECK_CODE(book->append(attempt_completed(3)), ErrorCode::Cancelled);
    RL_CHECK_CODE(book->append(attempt_cancelled(3)), ErrorCode::DuplicateCompletion);
    RL_CHECK_CODE(book->append(model_call_recorded(2, 3)), ErrorCode::Cancelled);
    RL_CHECK_CODE(book->append(tool_call_recorded(2, 3)), ErrorCode::Cancelled);
    MetricValue ratio = expect(MetricValue::ratio(0.5), "ratio value");
    RL_CHECK_CODE(book->append(observation_recorded(1, 3, "accuracy", ratio, UnitKind::Ratio)),
                  ErrorCode::Cancelled);

    auto snapshot = book->snapshot();
    const std::vector<AttemptView> attempts =
        expect(snapshot.experiment_attempts(ExperimentId::from_value(1)), "experiment attempts");
    RL_CHECK_EQ(attempts.size(), 3u);
    RL_CHECK(attempts[0].state == AttemptState::Completed);
    RL_CHECK(attempts[1].state == AttemptState::Failed);
    RL_CHECK(attempts[2].state == AttemptState::Cancelled);
    for (const AttemptView& attempt : attempts) {
        RL_CHECK(attempt.terminated_at.valid());
        RL_CHECK_EQ(attempt.generation.value(), 1u);
        RL_CHECK_EQ(attempt.model_call_count, attempt.attempt == AttemptId::from_value(2) ? 1u : 0u);
    }

    // A retry is a new attempt, not a resurrection of the failed one.
    RL_CHECK_OK(book->append(attempt_started(4, 1, 1)));
    {
        const AttemptView retried = expect(book->snapshot().attempt(AttemptId::from_value(4)),
                                           "attempt 4");
        RL_CHECK(retried.state == AttemptState::Running);
        RL_CHECK(!retried.terminated_at.valid());
        const AttemptView failed = expect(book->snapshot().attempt(AttemptId::from_value(2)),
                                          "attempt 2");
        RL_CHECK(failed.state == AttemptState::Failed);
        RL_CHECK(failed.terminated_at.valid());
    }
}

// --- snapshot consistency ---------------------------------------------------
// The two tests below assert the documented snapshot contract: everything above
// a snapshot watermark is not committed as far as that snapshot is concerned,
// so no query on it may return an entity whose committed record the same
// snapshot reports as not committed.

RL_TEST(lifecycle_snapshot_evidence_stays_within_its_watermark) {
    auto created = ledger_with_candidate(local_config());
    RL_CHECK_OK(created);
    const std::shared_ptr<Ledger>& book = created.value();
    const SubjectId result_subject = SubjectId::of(ResultId::from_value(1));

    auto old = book->snapshot();
    const RecordSequence watermark = old.watermark();
    RL_CHECK_OK(accept_result(book, 1, 1));
    const RecordSequence decision_sequence =
        expect(book->snapshot().subject_sequence(SubjectId::of(DecisionId::from_value(1))),
               "decision sequence");
    RL_CHECK(decision_sequence.value() > watermark.value());

    // The snapshot reports the decision record as beyond its watermark.
    RL_CHECK_CODE(old.record_at(decision_sequence), ErrorCode::NotFound);
    RL_CHECK_CODE(old.decision(DecisionId::from_value(1)), ErrorCode::NotFound);
    const std::vector<DecisionView> visible =
        expect(old.decisions_of(result_subject), "decisions of result 1");
    RL_CHECK_EQ(visible.size(), 0u);

    // ... so its evidence closure must not report that decision either.
    const EvidenceBundle bundle =
        expect(old.supporting_evidence(ResultId::from_value(1)), "supporting evidence");
    RL_CHECK_EQ(bundle.decisions.size(), visible.size());
    for (const DecisionView& decision : bundle.decisions) {
        RL_CHECK(old.decision(decision.decision).ok());
        RL_CHECK(old.record_at(decision.recorded_at).ok());
    }
}

RL_TEST(lifecycle_unresolved_branches_agree_with_the_branch_view) {
    auto created = ledger_with_candidate(local_config());
    RL_CHECK_OK(created);
    const std::shared_ptr<Ledger>& book = created.value();

    RL_CHECK_OK(book->append(branch_declared(2, 1, BranchKind::MergedEvidence, 1)));
    RL_CHECK_OK(book->append(experiment_declared(2, 1, 1, 2)));

    auto old = book->snapshot();
    RL_CHECK_OK(book->append(attempt_started(2, 2, 2)));

    // At this snapshot branch 2 has no attempt at all.
    const BranchView view = expect(old.branch(BranchId::from_value(2)), "branch 2");
    RL_CHECK_EQ(view.attempt_count, 0u);
    RL_CHECK(!view.unresolved);

    // ... so the same snapshot does not report it as unresolved either.
    const std::vector<BranchId> unresolved =
        expect(old.unresolved_branches(ResearchSessionId::from_value(1)), "unresolved branches");
    bool lists_branch_two = false;
    for (const BranchId branch : unresolved) {
        if (branch == BranchId::from_value(2)) {
            lists_branch_two = true;
        }
    }
    RL_CHECK(!lists_branch_two);
}

// --- one batch, one revision per record -------------------------------------
// A batch is validated draft by draft against committed state plus the drafts
// that precede it in the same batch. A draft therefore has to see the current
// revision of a subject, not the first one the batch staged.

RL_TEST(lifecycle_a_batch_cannot_terminate_an_attempt_twice) {
    auto created = Ledger::create(local_config());
    RL_CHECK_OK(created);
    const std::shared_ptr<Ledger>& book = created.value();

    // The attempt is started and terminated inside one batch that also carries
    // a second, contradictory completion. The attempt is terminal after the
    // second draft, so the third draft is a duplicate completion.
    const auto outcomes = book->append_batch(std::vector<RecordDraft>{
        session_opened(1), hypothesis_declared(1, 1, "one terminal record"),
        branch_declared(1, 1, BranchKind::Root), experiment_declared(1, 1, 1, 1),
        attempt_started(1, 1, 1), attempt_completed(1, "first outcome"),
        attempt_completed(1, "second outcome")});
    if (outcomes.ok()) {
        const LedgerStats stats = expect(book->snapshot().stats(), "stats");
        RL_CHECK_MESSAGE(stats.count_of(RecordType::AttemptCompleted) <= 1u,
                         "one attempt acquired " +
                             std::to_string(stats.count_of(RecordType::AttemptCompleted)) +
                             " terminal records inside a single batch");
    }
    RL_CHECK_CODE(outcomes, ErrorCode::DuplicateCompletion);
    // A refused batch commits nothing at all.
    RL_CHECK_EQ(book->last_sequence().value(), 0u);

    // The same records split across appends commit the first completion and
    // refuse the second, which is what the batch above must guarantee too.
    RL_CHECK_OK(book->append_batch(std::vector<RecordDraft>{
        session_opened(1), hypothesis_declared(1, 1, "one terminal record"),
        branch_declared(1, 1, BranchKind::Root), experiment_declared(1, 1, 1, 1),
        attempt_started(1, 1, 1), attempt_completed(1, "first outcome")}));
    RL_CHECK_CODE(book->append(attempt_completed(1, "second outcome")),
                  ErrorCode::DuplicateCompletion);
    const LedgerStats stats = expect(book->snapshot().stats(), "stats");
    RL_CHECK_EQ(stats.count_of(RecordType::AttemptCompleted), 1u);
    RL_CHECK(book->snapshot().attempt(AttemptId::from_value(1)).value().state ==
             AttemptState::Completed);
}

RL_TEST(lifecycle_a_batch_accepts_successive_revisions_of_one_subject) {
    // A batch that declares a hypothesis and then moves it proposed -> active ->
    // supported is a legal history: the same three records commit one append at
    // a time, so they must also commit as one batch.
    auto created = Ledger::create(local_config());
    RL_CHECK_OK(created);
    const std::shared_ptr<Ledger>& book = created.value();
    RL_CHECK_OK(book->append_batch(std::vector<RecordDraft>{
        session_opened(1), hypothesis_declared(1, 1, "the batch is internally consistent"),
        hypothesis_change(1, 2, HypothesisStatus::Proposed, HypothesisStatus::Active),
        hypothesis_change(1, 3, HypothesisStatus::Active, HypothesisStatus::Supported)}));
    {
        const HypothesisView view = expect(book->snapshot().hypothesis(HypothesisId::from_value(1)),
                                           "hypothesis 1");
        RL_CHECK(view.status == HypothesisStatus::Supported);
        RL_CHECK_EQ(view.generation.value(), 3u);
    }

    // The same records, one append each, are accepted.
    auto split = Ledger::create(local_config());
    RL_CHECK_OK(split);
    const std::shared_ptr<Ledger>& other = split.value();
    RL_CHECK_OK(other->append_batch(std::vector<RecordDraft>{
        session_opened(1), hypothesis_declared(1, 1, "the batch is internally consistent")}));
    RL_CHECK_OK(other->append(
        hypothesis_change(1, 2, HypothesisStatus::Proposed, HypothesisStatus::Active)));
    RL_CHECK_OK(other->append(
        hypothesis_change(1, 3, HypothesisStatus::Active, HypothesisStatus::Supported)));
    const HypothesisView split_view =
        expect(other->snapshot().hypothesis(HypothesisId::from_value(1)), "hypothesis 1");
    RL_CHECK(split_view.status == HypothesisStatus::Supported);
    RL_CHECK_EQ(split_view.generation.value(), 3u);
    RL_CHECK(book->logical_digest() == other->logical_digest());
}

RL_TEST(lifecycle_a_snapshot_reports_the_result_view_it_observed) {
    auto created = ledger_with_candidate(local_config());
    RL_CHECK_OK(created);
    const std::shared_ptr<Ledger>& book = created.value();

    auto old = book->snapshot();
    const RecordSequence watermark = old.watermark();
    const ResultView observed = expect(old.result(ResultId::from_value(1)), "result 1");
    RL_CHECK(observed.status == ResultStatus::Candidate);

    // A hypothesis whose status changes after the snapshot is reported by the
    // snapshot as it stood at the watermark.
    RL_CHECK_OK(book->append(
        hypothesis_change(1, 2, HypothesisStatus::Proposed, HypothesisStatus::Active)));
    const HypothesisView projected =
        expect(old.hypothesis(HypothesisId::from_value(1)), "hypothesis 1");
    RL_CHECK(projected.status == HypothesisStatus::Proposed);
    RL_CHECK_EQ(projected.generation.value(), 1u);

    // The result follows the same rule: a status change committed after the
    // snapshot is not part of what the snapshot reports.
    RL_CHECK_OK(accept_result(book, 1, 1));
    const RecordSequence change_sequence =
        expect(book->snapshot().subject_sequence(SubjectId::of(ResultId::from_value(1))),
               "result sequence");
    RL_CHECK(change_sequence.value() > watermark.value());
    RL_CHECK_CODE(old.record_at(change_sequence), ErrorCode::NotFound);
    RL_CHECK_CODE(old.decision(DecisionId::from_value(1)), ErrorCode::NotFound);
    const ResultView view = expect(old.result(ResultId::from_value(1)), "result 1");
    RL_CHECK_MESSAGE(view.status == observed.status,
                     std::string("a snapshot at watermark ") + std::to_string(watermark.value()) +
                         " reports result status " +
                         std::string(result_status_name(view.status)) +
                         " although the transition that set it is not committed at that "
                         "watermark");
    RL_CHECK(view.generation == observed.generation);
    RL_CHECK(view.acceptance_decision == observed.acceptance_decision);
}

}  // namespace
}  // namespace research_ledger
