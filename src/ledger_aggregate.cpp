#include "ledger_state.hpp"

#include <algorithm>
#include <deque>
#include <unordered_set>
#include <utility>

#include "research_ledger/record_codec.hpp"

namespace research_ledger {
namespace {

SubjectKey key_of(const SubjectId& subject) { return SubjectKey{subject.kind(), subject.value()}; }

void collect_experiment(const LedgerState& state, ExperimentId experiment,
                        std::unordered_set<SubjectKey, SubjectKeyHash>& out);

void insert_attempt(const LedgerState& state, AttemptId attempt,
                    std::unordered_set<SubjectKey, SubjectKeyHash>& out);

void collect_experiment(const LedgerState& state, ExperimentId experiment,
                        std::unordered_set<SubjectKey, SubjectKeyHash>& out) {
    out.insert(SubjectKey{SubjectKind::Experiment, experiment.value()});
    const auto entry = state.experiments.find(experiment);
    if (entry == state.experiments.end()) {
        return;
    }
    for (const AttemptId attempt : entry->second.attempts) {
        insert_attempt(state, attempt, out);
    }
}

void insert_attempt(const LedgerState& state, AttemptId attempt,
                    std::unordered_set<SubjectKey, SubjectKeyHash>& out) {
    (void)state;
    out.insert(SubjectKey{SubjectKind::Attempt, attempt.value()});
}

// Calls and observations are attached to attempts. They are added in one pass
// over each index rather than by walking every attempt, so the closure of a
// session with many attempts stays linear.
void expand_calls(const LedgerState& state, std::unordered_set<SubjectKey, SubjectKeyHash>& out) {
    for (const auto& entry : state.model_calls) {
        if (out.find(SubjectKey{SubjectKind::Attempt, entry.second.attempt.value()}) != out.end()) {
            out.insert(SubjectKey{SubjectKind::ModelCall, entry.first.value()});
        }
    }
    for (const auto& entry : state.tool_calls) {
        if (out.find(SubjectKey{SubjectKind::Attempt, entry.second.attempt.value()}) != out.end()) {
            out.insert(SubjectKey{SubjectKind::ToolCall, entry.first.value()});
        }
    }
    for (const auto& entry : state.observations) {
        if (out.find(SubjectKey{SubjectKind::Attempt, entry.second.attempt.value()}) != out.end()) {
            out.insert(SubjectKey{SubjectKind::Observation, entry.first.value()});
        }
    }
}

}  // namespace

Digest next_chain_digest(const Digest& previous, const RecordHeader& header) {
    DigestBuilder builder;
    builder.update_bytes(previous);
    builder.update_u32(header.sequence.value());
    builder.update_u16(static_cast<std::uint16_t>(header.type));
    builder.update_u64(header.record_id.value());
    builder.update_i64(header.committed_at.unix_nanos);
    builder.update_u8(static_cast<std::uint8_t>(header.provenance));
    builder.update_bytes(header.payload_digest);
    return builder.finish();
}

namespace {

void collect_core(const LedgerState& state, const SubjectId& scope,
                  std::unordered_set<SubjectKey, SubjectKeyHash>& out) {
    if (!scope.valid()) {
        return;
    }
    // The scope of an accounting query is the subject itself plus everything the
    // ledger records beneath it. A record shared by several results is still one
    // record: the set removes shared ancestry, so nothing is counted twice.
    switch (scope.kind()) {
        case SubjectKind::Session: {
            const ResearchSessionId session = *scope.as<ResearchSessionId>();
            out.insert(SubjectKey{SubjectKind::Session, session.value()});
            const auto experiments = state.session_experiments.find(session);
            if (experiments != state.session_experiments.end()) {
                for (const ExperimentId experiment : experiments->second) {
                    collect_experiment(state, experiment, out);
                }
            }
            const auto branches = state.session_branches.find(session);
            if (branches != state.session_branches.end()) {
                for (const BranchId branch : branches->second) {
                    out.insert(SubjectKey{SubjectKind::Branch, branch.value()});
                }
            }
            return;
        }
        case SubjectKind::Hypothesis: {
            const HypothesisId hypothesis = *scope.as<HypothesisId>();
            out.insert(SubjectKey{SubjectKind::Hypothesis, hypothesis.value()});
            const auto entry = state.hypotheses.find(hypothesis);
            if (entry != state.hypotheses.end()) {
                for (const ExperimentId experiment : entry->second.experiments) {
                    collect_experiment(state, experiment, out);
                }
            }
            return;
        }
        case SubjectKind::Experiment: {
            collect_experiment(state, *scope.as<ExperimentId>(), out);
            return;
        }
        case SubjectKind::Branch: {
            const BranchId branch = *scope.as<BranchId>();
            out.insert(SubjectKey{SubjectKind::Branch, branch.value()});
            const auto entry = state.branches.find(branch);
            if (entry != state.branches.end()) {
                for (const ExperimentId experiment : entry->second.experiments) {
                    collect_experiment(state, experiment, out);
                }
            }
            return;
        }
        case SubjectKind::Attempt: {
            insert_attempt(state, *scope.as<AttemptId>(), out);
            return;
        }
        case SubjectKind::Result: {
            const ResultId result = *scope.as<ResultId>();
            out.insert(SubjectKey{SubjectKind::Result, result.value()});
            const auto entry = state.results.find(result);
            if (entry == state.results.end()) {
                return;
            }
            for (const ExperimentId experiment : entry->second.experiments) {
                collect_experiment(state, experiment, out);
            }
            for (const ModelCallId call : entry->second.model_calls) {
                out.insert(SubjectKey{SubjectKind::ModelCall, call.value()});
            }
            for (const ToolCallId call : entry->second.tool_calls) {
                out.insert(SubjectKey{SubjectKind::ToolCall, call.value()});
            }
            for (const ObservationId observation : entry->second.observations) {
                out.insert(SubjectKey{SubjectKind::Observation, observation.value()});
                const auto observation_entry = state.observations.find(observation);
                if (observation_entry != state.observations.end()) {
                    insert_attempt(state, observation_entry->second.attempt, out);
                }
            }
            return;
        }
        case SubjectKind::Observation: {
            out.insert(key_of(scope));
            const auto entry = state.observations.find(*scope.as<ObservationId>());
            if (entry != state.observations.end()) {
                insert_attempt(state, entry->second.attempt, out);
            }
            return;
        }
        case SubjectKind::Failure: {
            out.insert(key_of(scope));
            const auto entry = state.failures.find(*scope.as<FailureId>());
            if (entry != state.failures.end()) {
                collect_core(state, entry->second.scope, out);
            }
            return;
        }
        case SubjectKind::Artifact: {
            out.insert(key_of(scope));
            const auto entry = state.artifacts.find(*scope.as<ArtifactId>());
            if (entry != state.artifacts.end()) {
                collect_core(state, entry->second.view.producer, out);
            }
            return;
        }
        case SubjectKind::ModelCall:
        case SubjectKind::ToolCall:
        case SubjectKind::Decision:
            out.insert(key_of(scope));
            return;
        case SubjectKind::None:
            return;
    }
}

}  // namespace

void collect_accounting_scope(const LedgerState& state, const SubjectId& scope,
                              std::unordered_set<SubjectKey, SubjectKeyHash>& out) {
    collect_core(state, scope, out);
    expand_calls(state, out);
}

std::unordered_set<ExperimentId> accepted_experiments_of(const LedgerState& state,
                                                         ResearchSessionId session,
                                                         RecordSequence watermark) {
    std::unordered_set<ExperimentId> accepted;
    const auto results = state.session_results.find(session);
    if (results == state.session_results.end()) {
        return accepted;
    }
    for (const ResultId id : results->second) {
        const auto entry = state.results.find(id);
        if (entry == state.results.end()) {
            continue;
        }
        if (!entry->second.declared_at.valid() ||
            entry->second.declared_at.value() > watermark.value()) {
            continue;
        }
        if (entry->second.last_changed_at.valid() &&
            entry->second.last_changed_at.value() > watermark.value()) {
            continue;
        }
        if (entry->second.status != ResultStatus::Accepted) {
            continue;
        }
        for (const ExperimentId experiment : entry->second.experiments) {
            accepted.insert(experiment);
        }
    }
    return accepted;
}

void resolve_branch_state(const LedgerState& state, BranchView& view, RecordSequence watermark,
                          const std::unordered_set<ExperimentId>& accepted_experiments) {
    std::uint64_t attempts = 0;
    std::uint64_t completed = 0;
    bool accepted = false;
    for (const ExperimentId experiment : view.experiments) {
        if (accepted_experiments.find(experiment) != accepted_experiments.end()) {
            accepted = true;
        }
        const auto entry = state.experiments.find(experiment);
        if (entry == state.experiments.end()) {
            continue;
        }
        for (const AttemptId attempt : entry->second.attempts) {
            const auto attempt_entry = state.attempts.find(attempt);
            if (attempt_entry == state.attempts.end()) {
                continue;
            }
            if (!attempt_entry->second.started_at.valid() ||
                attempt_entry->second.started_at.value() > watermark.value()) {
                continue;
            }
            attempts += 1;
            if (attempt_entry->second.state == AttemptState::Completed &&
                attempt_entry->second.terminated_at.valid() &&
                attempt_entry->second.terminated_at.value() <= watermark.value()) {
                completed += 1;
            }
        }
    }
    view.attempt_count = attempts;
    view.unresolved = attempts > 0 && completed == 0 && !accepted;
}

Result<AccountingAggregate> LedgerSnapshot::accounting(SubjectId scope) const {
    if (!valid()) {
        return Status(ErrorCode::NotReady, "snapshot is not attached to a ledger");
    }
    if (!scope.valid()) {
        return Status(ErrorCode::InvalidIdentity, "accounting scope is not valid");
    }
    std::shared_lock lock(state_->mutex);
    if (state_->subject_index.find(key_of(scope)) == state_->subject_index.end()) {
        return Status(ErrorCode::NotFound, "accounting scope is not committed");
    }
    std::unordered_set<SubjectKey, SubjectKeyHash> scopes;
    collect_accounting_scope(*state_, scope, scopes);
    std::vector<AccountingVector> contributions;
    contributions.reserve(scopes.size());
    for (const AccountingState& entry : state_->accounting) {
        if (!entry.sequence.valid() || entry.sequence.value() > watermark_.value()) {
            continue;
        }
        if (scopes.find(key_of(entry.scope)) == scopes.end()) {
            continue;
        }
        contributions.push_back(entry.accounting);
    }
    return aggregate_accounting(contributions.data(), contributions.size());
}

Result<std::vector<BranchId>> LedgerSnapshot::unresolved_branches(ResearchSessionId session) const {
    if (!valid()) {
        return Status(ErrorCode::NotReady, "snapshot is not attached to a ledger");
    }
    std::shared_lock lock(state_->mutex);
    if (state_->sessions.find(session) == state_->sessions.end()) {
        return Status(ErrorCode::NotFound, "research session is not committed");
    }
    std::vector<BranchId> result;
    const auto branches = state_->session_branches.find(session);
    if (branches == state_->session_branches.end()) {
        return result;
    }
    const std::unordered_set<ExperimentId> accepted_experiments =
        accepted_experiments_of(*state_, session, watermark_);
    for (const BranchId branch : branches->second) {
        const auto entry = state_->branches.find(branch);
        if (entry == state_->branches.end() ||
            entry->second.declared_at.value() > watermark_.value()) {
            continue;
        }
        BranchView view = entry->second;
        resolve_branch_state(*state_, view, watermark_, accepted_experiments);
        if (view.unresolved) {
            result.push_back(branch);
        }
    }
    std::sort(result.begin(), result.end(), [this](BranchId lhs, BranchId rhs) {
        const auto left = state_->branches.find(lhs);
        const auto right = state_->branches.find(rhs);
        if (left == state_->branches.end() || right == state_->branches.end()) {
            return lhs < rhs;
        }
        return left->second.declared_at < right->second.declared_at;
    });
    return result;
}

Result<LedgerStats> LedgerSnapshot::stats() const {
    if (!valid()) {
        return Status(ErrorCode::NotReady, "snapshot is not attached to a ledger");
    }
    std::shared_lock lock(state_->mutex);
    LedgerStats stats;
    stats.last_sequence = watermark_;
    stats.chain_digest = state_->chain_digest;
    stats.generation = state_->generation;
    stats.epoch = state_->epoch;
    stats.sessions = state_->sessions.size();
    stats.hypotheses = state_->hypotheses.size();
    stats.experiments = state_->experiments.size();
    stats.branches = state_->branches.size();
    stats.attempts = state_->attempts.size();
    stats.model_calls = state_->model_calls.size();
    stats.tool_calls = state_->tool_calls.size();
    stats.artifacts = state_->artifacts.size();
    stats.observations = state_->observations.size();
    stats.failures = state_->failures.size();
    stats.decisions = state_->decisions.size();
    stats.results = state_->results.size();
    stats.accounting_records = state_->accounting.size();
    for (const auto& entry : state_->workers) {
        if (entry.second.live && entry.second.epoch == state_->epoch) {
            stats.live_workers += 1;
        }
    }
    stats.record_counts = state_->record_counts;
    return stats;
}

std::uint64_t LedgerStats::count_of(RecordType type) const noexcept {
    const std::size_t index = static_cast<std::size_t>(type);
    if (index >= record_counts.size()) {
        return 0;
    }
    return record_counts[index];
}

namespace {

void add_issue(IntegrityReport& report, ErrorCode code, RecordSequence sequence, SubjectId subject,
               std::string detail) {
    if (report.issues.size() >= 128) {
        return;
    }
    IntegrityIssue issue;
    issue.code = code;
    issue.sequence = sequence;
    issue.subject = subject;
    issue.detail = std::move(detail);
    report.issues.push_back(std::move(issue));
    report.ok = false;
}

}  // namespace

std::string IntegrityReport::to_text() const {
    std::string text = ok ? "integrity: OK" : "integrity: FAILED";
    text += "\nrecords: ";
    text += std::to_string(records.value());
    text += "\nchain: ";
    text += to_hex(chain_digest);
    text += "\nchecked: ";
    text += std::to_string(checked_records);
    for (const IntegrityIssue& issue : issues) {
        text += "\n  ";
        text += std::string(error_code_name(issue.code));
        text += " at sequence ";
        text += std::to_string(issue.sequence.value());
        if (issue.subject.valid()) {
            text += " subject ";
            text += issue.subject.to_string();
        }
        text += ": ";
        text += issue.detail;
    }
    return text;
}

Result<IntegrityReport> LedgerSnapshot::verify() const {
    if (!valid()) {
        return Status(ErrorCode::NotReady, "snapshot is not attached to a ledger");
    }
    std::shared_lock lock(state_->mutex);
    IntegrityReport report;
    report.records = watermark_;
    const std::uint64_t limit = std::min<std::uint64_t>(watermark_.value(), state_->records.size());
    Digest chain{};
    for (std::uint64_t index = 0; index < limit; ++index) {
        const Record& record = state_->records[static_cast<std::size_t>(index)];
        report.checked_records += 1;
        if (record.header.sequence.value() != index + 1) {
            add_issue(report, ErrorCode::IntegrityFailure, record.header.sequence,
                      state_->record_subjects[static_cast<std::size_t>(index)],
                      "record sequence is not contiguous");
            continue;
        }
        if (record_type_of(record.body) != record.header.type) {
            add_issue(report, ErrorCode::IntegrityFailure, record.header.sequence,
                      state_->record_subjects[static_cast<std::size_t>(index)],
                      "record type does not match its payload");
        }
        auto digest = record_body_digest(record.body, state_->limits);
        if (!digest.ok() || !(digest.value() == record.header.payload_digest)) {
            add_issue(report, ErrorCode::PersistenceCorrupt, record.header.sequence,
                      state_->record_subjects[static_cast<std::size_t>(index)],
                      "payload digest does not match the committed payload");
        }
        const Digest expected = next_chain_digest(chain, record.header);
        if (!(expected == record.header.chain_digest)) {
            add_issue(report, ErrorCode::IntegrityFailure, record.header.sequence,
                      state_->record_subjects[static_cast<std::size_t>(index)],
                      "chain digest does not match the committed chain");
        }
        chain = record.header.chain_digest;
    }
    if (state_->record_index.size() != state_->records.size()) {
        add_issue(report, ErrorCode::DuplicateRecord, RecordSequence{},
                  SubjectId{}, "record identities are not unique");
    }
    report.chain_digest = chain;

    // Lineage checks over the committed graph: every edge points at an existing
    // node with a strictly smaller sequence, which makes a cycle impossible in
    // a healthy ledger. Anything else is reported rather than trusted.
    for (const auto& entry : state_->artifacts) {
        for (const ArtifactId parent : entry.second.view.parents) {
            const auto parent_entry = state_->artifacts.find(parent);
            if (parent_entry == state_->artifacts.end()) {
                add_issue(report, ErrorCode::BrokenLineage, entry.second.view.recorded_at,
                          SubjectId::of(entry.first), "artifact parent is missing");
                continue;
            }
            if (!(parent_entry->second.view.recorded_at < entry.second.view.recorded_at)) {
                add_issue(report, ErrorCode::LineageCycle, entry.second.view.recorded_at,
                          SubjectId::of(entry.first),
                          "artifact parent does not precede the artifact");
            }
        }
    }
    for (const auto& entry : state_->hypotheses) {
        if (!entry.second.parent.has_value()) {
            continue;
        }
        const auto parent = state_->hypotheses.find(*entry.second.parent);
        if (parent == state_->hypotheses.end()) {
            add_issue(report, ErrorCode::BrokenLineage, entry.second.declared_at,
                      SubjectId::of(entry.first), "hypothesis parent is missing");
            continue;
        }
        if (!(parent->second.declared_at < entry.second.declared_at)) {
            add_issue(report, ErrorCode::LineageCycle, entry.second.declared_at,
                      SubjectId::of(entry.first), "hypothesis parent does not precede the hypothesis");
        }
    }
    for (const auto& entry : state_->branches) {
        if (!entry.second.parent_branch.has_value()) {
            continue;
        }
        const auto parent = state_->branches.find(*entry.second.parent_branch);
        if (parent == state_->branches.end()) {
            add_issue(report, ErrorCode::BrokenLineage, entry.second.declared_at,
                      SubjectId::of(entry.first), "branch parent is missing");
            continue;
        }
        if (!(parent->second.declared_at < entry.second.declared_at)) {
            add_issue(report, ErrorCode::LineageCycle, entry.second.declared_at,
                      SubjectId::of(entry.first), "branch parent does not precede the branch");
        }
    }
    for (const auto& entry : state_->decisions) {
        for (const EvidenceRef& evidence : entry.second.evidence) {
            if (!evidence.sequence.valid() ||
                evidence.sequence.value() > state_->records.size() ||
                evidence.sequence.value() >= entry.second.recorded_at.value()) {
                add_issue(report, ErrorCode::InvalidTransition, entry.second.recorded_at,
                          SubjectId::of(entry.first), "decision evidence is not temporally valid");
                continue;
            }
            const std::size_t index = static_cast<std::size_t>(evidence.sequence.value()) - 1;
            if (!(state_->record_subjects[index] == evidence.subject)) {
                add_issue(report, ErrorCode::BrokenLineage, entry.second.recorded_at,
                          SubjectId::of(entry.first), "decision evidence does not concern its subject");
            }
        }
    }
    return report;
}

}  // namespace research_ledger
