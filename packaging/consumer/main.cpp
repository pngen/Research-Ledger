// Independent downstream consumer of the installed Research Ledger package.
//
// It builds a small research history through the installed public API, commits
// it, checks integrity, reconstructs the accepted result and reports what it
// found. Nothing here reaches into the source tree.

#include <cstdio>
#include <string>
#include <vector>

#include <research_ledger/ledger.hpp>
#include <research_ledger/replay.hpp>
#include <research_ledger/version.hpp>

using namespace research_ledger;

int main() {
    std::printf("Research Ledger %s consumer\n", std::string(kVersionString).c_str());

    LedgerConfig config;
    auto created = Ledger::create(config);
    if (!created.ok()) {
        std::printf("create failed: %s\n", created.status().to_string().c_str());
        return 1;
    }
    std::shared_ptr<Ledger> ledger = created.value();

    SessionOpened session;
    session.session = ResearchSessionId::from_value(1);
    session.label = "consumer session";
    session.question = "does the installed package answer the core question?";
    session.authority = "consumer";

    HypothesisDeclared hypothesis;
    hypothesis.hypothesis = HypothesisId::from_value(1);
    hypothesis.generation = HypothesisGeneration::first();
    hypothesis.session = ResearchSessionId::from_value(1);
    hypothesis.claim = "the installed package reconstructs an accepted result";
    hypothesis.authority = "consumer";

    BranchDeclared branch;
    branch.branch = BranchId::from_value(1);
    branch.session = ResearchSessionId::from_value(1);
    branch.kind = BranchKind::Root;
    branch.label = "root";
    branch.authority = "consumer";

    ExperimentDeclared experiment;
    experiment.experiment = ExperimentId::from_value(1);
    experiment.generation = ExperimentGeneration::first();
    experiment.session = ResearchSessionId::from_value(1);
    experiment.hypotheses.push_back(HypothesisId::from_value(1));
    experiment.branch = BranchId::from_value(1);
    experiment.environment_reference = "consumer:environment";
    experiment.authority = "consumer";

    AttemptStarted attempt;
    attempt.attempt = AttemptId::from_value(1);
    attempt.generation = AttemptGeneration::first();
    attempt.experiment = ExperimentId::from_value(1);
    attempt.branch = BranchId::from_value(1);
    attempt.worker_authority = "consumer:worker";

    ArtifactReferenced artifact;
    artifact.artifact = ArtifactId::from_value(1);
    artifact.generation = ArtifactGeneration::first();
    artifact.session = ResearchSessionId::from_value(1);
    artifact.content_digest = sha256(std::string_view("consumer artifact"));
    artifact.role = ArtifactRole::ResultArtifact;
    artifact.producer = SubjectId::of(AttemptId::from_value(1));
    artifact.location = "artifact-store://consumer";
    artifact.validation = ValidationState::Validated;
    artifact.authority = "consumer";

    std::vector<RecordDraft> first_batch;
    for (const RecordBody& body : {RecordBody{session}, RecordBody{hypothesis},
                                   RecordBody{branch}, RecordBody{experiment},
                                   RecordBody{attempt}, RecordBody{artifact}}) {
        RecordDraft draft;
        draft.body = body;
        draft.provenance = Provenance::Reported;
        first_batch.push_back(draft);
    }
    auto committed = ledger->append_batch(first_batch);
    if (!committed.ok()) {
        std::printf("append failed: %s\n", committed.status().to_string().c_str());
        return 1;
    }
    std::printf("committed %zu record(s), last sequence %u\n", committed.value().size(),
                ledger->last_sequence().value());

    LedgerSnapshot snapshot = ledger->snapshot();
    auto integrity = snapshot.verify();
    if (!integrity.ok() || !integrity.value().ok) {
        std::printf("integrity failed\n");
        return 1;
    }
    std::printf("integrity: %llu record(s) verified, chain %s\n",
                static_cast<unsigned long long>(integrity.value().checked_records),
                to_hex(integrity.value().chain_digest).substr(0, 16).c_str());

    auto stats = snapshot.stats();
    if (!stats.ok()) {
        std::printf("stats failed: %s\n", stats.status().to_string().c_str());
        return 1;
    }
    std::printf("sessions %llu hypotheses %llu experiments %llu attempts %llu\n",
                static_cast<unsigned long long>(stats.value().sessions),
                static_cast<unsigned long long>(stats.value().hypotheses),
                static_cast<unsigned long long>(stats.value().experiments),
                static_cast<unsigned long long>(stats.value().attempts));

    auto digest = ledger->logical_digest();
    std::printf("logical digest %s\n", to_hex(digest).substr(0, 16).c_str());

    std::vector<Record> records = snapshot.records(RecordSequence::first(), ledger->last_sequence())
                                      .value();
    auto report = replay_committed(records, config);
    if (!report.ok()) {
        std::printf("replay failed: %s\n", report.status().to_string().c_str());
        return 1;
    }
    std::printf("replay: %llu record(s), logical digest %s\n",
                static_cast<unsigned long long>(report.value().records),
                to_hex(report.value().logical_digest).substr(0, 16).c_str());
    if (!(report.value().logical_digest == digest)) {
        std::printf("replay digest does not match the live logical digest\n");
        return 1;
    }
    std::printf("consumer OK\n");
    return 0;
}
