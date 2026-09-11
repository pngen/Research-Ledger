#include "ledger_state.hpp"

#include <algorithm>
#include <deque>
#include <unordered_set>
#include <utility>

namespace research_ledger {
namespace {

// A snapshot observes records committed at or before its watermark and nothing
// that was committed afterwards.
bool visible(RecordSequence sequence, RecordSequence watermark) {
    return sequence.valid() && sequence.value() <= watermark.value();
}

}  // namespace

Result<SessionView> LedgerSnapshot::session(ResearchSessionId session) const {
    if (!valid()) {
        return Status(ErrorCode::NotReady, "snapshot is not attached to a ledger");
    }
    std::shared_lock lock(state_->mutex);
    const auto iterator = state_->sessions.find(session);
    if (iterator == state_->sessions.end()) {
        return Status(ErrorCode::NotFound, "research session is not committed");
    }
    if (!visible(iterator->second.opened_at, watermark_)) {
        return Status(ErrorCode::NotFound, "research session is not committed at this snapshot");
    }
    SessionView view = iterator->second;
    if (view.state == SessionState::Closed && !visible(view.closed_at, watermark_)) {
        view.state = SessionState::Open;
        view.closed_at = RecordSequence{};
    }
    return view;
}

Result<std::vector<SessionView>> LedgerSnapshot::sessions() const {
    if (!valid()) {
        return Status(ErrorCode::NotReady, "snapshot is not attached to a ledger");
    }
    std::shared_lock lock(state_->mutex);
    std::vector<SessionView> result;
    for (const auto& entry : state_->sessions) {
        if (visible(entry.second.opened_at, watermark_)) {
            result.push_back(entry.second);
        }
    }
    std::sort(result.begin(), result.end(), [](const SessionView& lhs, const SessionView& rhs) {
        return lhs.opened_at < rhs.opened_at;
    });
    return result;
}

Result<HypothesisView> LedgerSnapshot::hypothesis(HypothesisId hypothesis) const {
    if (!valid()) {
        return Status(ErrorCode::NotReady, "snapshot is not attached to a ledger");
    }
    std::shared_lock lock(state_->mutex);
    const auto iterator = state_->hypotheses.find(hypothesis);
    if (iterator == state_->hypotheses.end() ||
        iterator->second.declared_at.value() > watermark_.value()) {
        return Status(ErrorCode::NotFound, "hypothesis is not committed at this snapshot");
    }
    HypothesisView view = iterator->second;
    if (!visible(view.last_changed_at, watermark_)) {
        view.generation = HypothesisGeneration::first();
        view.status = HypothesisStatus::Proposed;
        view.last_changed_at = view.declared_at;
    }
    return view;
}

Result<std::vector<HypothesisId>> LedgerSnapshot::hypothesis_ancestry(HypothesisId hypothesis) const {
    if (!valid()) {
        return Status(ErrorCode::NotReady, "snapshot is not attached to a ledger");
    }
    std::shared_lock lock(state_->mutex);
    std::vector<HypothesisId> ancestry;
    std::unordered_set<HypothesisId> visited;
    HypothesisId current = hypothesis;
    for (std::uint32_t depth = 0; depth < state_->limits.max_lineage_depth; ++depth) {
        const auto iterator = state_->hypotheses.find(current);
        if (iterator == state_->hypotheses.end()) {
            if (ancestry.empty()) {
                return Status(ErrorCode::NotFound, "hypothesis is not committed");
            }
            return Status(ErrorCode::BrokenLineage, "hypothesis ancestry is broken");
        }
        if (!visited.insert(current).second) {
            return Status(ErrorCode::LineageCycle, "hypothesis ancestry contains a cycle");
        }
        ancestry.push_back(current);
        if (!iterator->second.parent.has_value()) {
            return ancestry;
        }
        current = *iterator->second.parent;
    }
    return Status(ErrorCode::LimitExceeded, "hypothesis ancestry exceeds the configured depth");
}

Result<std::vector<HypothesisId>> LedgerSnapshot::hypothesis_descendants(
    HypothesisId hypothesis) const {
    if (!valid()) {
        return Status(ErrorCode::NotReady, "snapshot is not attached to a ledger");
    }
    std::shared_lock lock(state_->mutex);
    if (state_->hypotheses.find(hypothesis) == state_->hypotheses.end()) {
        return Status(ErrorCode::NotFound, "hypothesis is not committed");
    }
    std::vector<HypothesisId> result;
    std::deque<HypothesisId> queue{hypothesis};
    std::unordered_set<HypothesisId> visited{hypothesis};
    while (!queue.empty()) {
        const HypothesisId current = queue.front();
        queue.pop_front();
        const auto children = state_->hypothesis_children.find(current);
        if (children == state_->hypothesis_children.end()) {
            continue;
        }
        for (const HypothesisId child : children->second) {
            if (!visited.insert(child).second) {
                return Status(ErrorCode::LineageCycle, "hypothesis descendants contain a cycle");
            }
            result.push_back(child);
            queue.push_back(child);
            if (result.size() > state_->limits.max_lineage_nodes) {
                return Status(ErrorCode::LimitExceeded,
                              "hypothesis descendant count exceeds the configured maximum");
            }
        }
    }
    std::sort(result.begin(), result.end(), [this](HypothesisId lhs, HypothesisId rhs) {
        const auto left = state_->hypotheses.find(lhs);
        const auto right = state_->hypotheses.find(rhs);
        if (left == state_->hypotheses.end() || right == state_->hypotheses.end()) {
            return lhs < rhs;
        }
        return left->second.declared_at < right->second.declared_at;
    });
    return result;
}

Result<BranchView> LedgerSnapshot::branch(BranchId branch) const {
    if (!valid()) {
        return Status(ErrorCode::NotReady, "snapshot is not attached to a ledger");
    }
    std::shared_lock lock(state_->mutex);
    const auto iterator = state_->branches.find(branch);
    if (iterator == state_->branches.end() ||
        iterator->second.declared_at.value() > watermark_.value()) {
        return Status(ErrorCode::NotFound, "branch is not committed at this snapshot");
    }
    BranchView view = iterator->second;
    // Resolution and unresolved-branch listing share one implementation so the
    // two queries can never disagree.
    resolve_branch_state(*state_, view, watermark_,
                         accepted_experiments_of(*state_, view.session, watermark_));
    return view;
}

Result<std::vector<BranchId>> LedgerSnapshot::branch_ancestry(BranchId branch) const {
    if (!valid()) {
        return Status(ErrorCode::NotReady, "snapshot is not attached to a ledger");
    }
    std::shared_lock lock(state_->mutex);
    std::vector<BranchId> ancestry;
    std::unordered_set<BranchId> visited;
    BranchId current = branch;
    for (std::uint32_t depth = 0; depth < state_->limits.max_lineage_depth; ++depth) {
        const auto iterator = state_->branches.find(current);
        if (iterator == state_->branches.end()) {
            if (ancestry.empty()) {
                return Status(ErrorCode::NotFound, "branch is not committed");
            }
            return Status(ErrorCode::BrokenLineage, "branch ancestry is broken");
        }
        if (!visited.insert(current).second) {
            return Status(ErrorCode::LineageCycle, "branch ancestry contains a cycle");
        }
        ancestry.push_back(current);
        if (!iterator->second.parent_branch.has_value()) {
            return ancestry;
        }
        current = *iterator->second.parent_branch;
    }
    return Status(ErrorCode::LimitExceeded, "branch ancestry exceeds the configured depth");
}

Result<ExperimentView> LedgerSnapshot::experiment(ExperimentId experiment) const {
    if (!valid()) {
        return Status(ErrorCode::NotReady, "snapshot is not attached to a ledger");
    }
    std::shared_lock lock(state_->mutex);
    const auto iterator = state_->experiments.find(experiment);
    if (iterator == state_->experiments.end() ||
        iterator->second.declared_at.value() > watermark_.value()) {
        return Status(ErrorCode::NotFound, "experiment is not committed at this snapshot");
    }
    ExperimentView view = iterator->second;
    view.attempts.clear();
    for (const AttemptId attempt : iterator->second.attempts) {
        const auto attempt_entry = state_->attempts.find(attempt);
        if (attempt_entry != state_->attempts.end() &&
            visible(attempt_entry->second.started_at, watermark_)) {
            view.attempts.push_back(attempt);
        }
    }
    return view;
}

Result<AttemptView> LedgerSnapshot::attempt(AttemptId attempt) const {
    if (!valid()) {
        return Status(ErrorCode::NotReady, "snapshot is not attached to a ledger");
    }
    std::shared_lock lock(state_->mutex);
    const auto iterator = state_->attempts.find(attempt);
    if (iterator == state_->attempts.end()) {
        return Status(ErrorCode::NotFound, "attempt is not committed");
    }
    if (!visible(iterator->second.started_at, watermark_)) {
        return Status(ErrorCode::NotFound, "attempt is not committed at this snapshot");
    }
    AttemptView view = iterator->second;
    if (view.terminated_at.valid() && !visible(view.terminated_at, watermark_)) {
        // The attempt was still running when this snapshot was taken.
        view.state = AttemptState::Running;
        view.terminated_at = RecordSequence{};
        view.failure.reset();
    }
    return view;
}

Result<std::vector<AttemptView>> LedgerSnapshot::experiment_attempts(ExperimentId experiment) const {
    if (!valid()) {
        return Status(ErrorCode::NotReady, "snapshot is not attached to a ledger");
    }
    std::shared_lock lock(state_->mutex);
    const auto iterator = state_->experiments.find(experiment);
    if (iterator == state_->experiments.end()) {
        return Status(ErrorCode::NotFound, "experiment is not committed");
    }
    std::vector<AttemptView> result;
    for (const AttemptId attempt : iterator->second.attempts) {
        const auto entry = state_->attempts.find(attempt);
        if (entry != state_->attempts.end() && visible(entry->second.started_at, watermark_)) {
            result.push_back(entry->second);
        }
    }
    std::sort(result.begin(), result.end(), [](const AttemptView& lhs, const AttemptView& rhs) {
        return lhs.started_at < rhs.started_at;
    });
    return result;
}

Result<std::vector<AttemptView>> LedgerSnapshot::branch_attempts(BranchId branch) const {
    if (!valid()) {
        return Status(ErrorCode::NotReady, "snapshot is not attached to a ledger");
    }
    std::shared_lock lock(state_->mutex);
    const auto iterator = state_->branches.find(branch);
    if (iterator == state_->branches.end()) {
        return Status(ErrorCode::NotFound, "branch is not committed");
    }
    std::vector<AttemptView> result;
    for (const ExperimentId experiment : iterator->second.experiments) {
        const auto entry = state_->experiments.find(experiment);
        if (entry == state_->experiments.end()) {
            continue;
        }
        for (const AttemptId attempt : entry->second.attempts) {
            const auto attempt_entry = state_->attempts.find(attempt);
            if (attempt_entry != state_->attempts.end() &&
                visible(attempt_entry->second.started_at, watermark_)) {
                result.push_back(attempt_entry->second);
            }
        }
    }
    std::sort(result.begin(), result.end(), [](const AttemptView& lhs, const AttemptView& rhs) {
        return lhs.started_at < rhs.started_at;
    });
    return result;
}

Result<std::vector<ModelCallView>> LedgerSnapshot::model_calls(AttemptId attempt) const {
    if (!valid()) {
        return Status(ErrorCode::NotReady, "snapshot is not attached to a ledger");
    }
    std::shared_lock lock(state_->mutex);
    std::vector<ModelCallView> result;
    for (const auto& entry : state_->model_calls) {
        if (entry.second.attempt == attempt && visible(entry.second.recorded_at, watermark_)) {
            result.push_back(entry.second);
        }
    }
    std::sort(result.begin(), result.end(), [](const ModelCallView& lhs, const ModelCallView& rhs) {
        return lhs.recorded_at < rhs.recorded_at;
    });
    if (result.size() > state_->limits.max_query_results) {
        return Status(ErrorCode::LimitExceeded, "query result exceeds the configured maximum");
    }
    return result;
}

Result<std::vector<ToolCallView>> LedgerSnapshot::tool_calls(AttemptId attempt) const {
    if (!valid()) {
        return Status(ErrorCode::NotReady, "snapshot is not attached to a ledger");
    }
    std::shared_lock lock(state_->mutex);
    std::vector<ToolCallView> result;
    for (const auto& entry : state_->tool_calls) {
        if (entry.second.attempt == attempt && visible(entry.second.recorded_at, watermark_)) {
            result.push_back(entry.second);
        }
    }
    std::sort(result.begin(), result.end(), [](const ToolCallView& lhs, const ToolCallView& rhs) {
        return lhs.recorded_at < rhs.recorded_at;
    });
    if (result.size() > state_->limits.max_query_results) {
        return Status(ErrorCode::LimitExceeded, "query result exceeds the configured maximum");
    }
    return result;
}

Result<std::vector<ObservationView>> LedgerSnapshot::observations(AttemptId attempt) const {
    if (!valid()) {
        return Status(ErrorCode::NotReady, "snapshot is not attached to a ledger");
    }
    std::shared_lock lock(state_->mutex);
    std::vector<ObservationView> result;
    for (const auto& entry : state_->observations) {
        if (entry.second.attempt == attempt && visible(entry.second.recorded_at, watermark_)) {
            result.push_back(entry.second);
        }
    }
    std::sort(result.begin(), result.end(),
              [](const ObservationView& lhs, const ObservationView& rhs) {
                  return lhs.recorded_at < rhs.recorded_at;
              });
    if (result.size() > state_->limits.max_query_results) {
        return Status(ErrorCode::LimitExceeded, "query result exceeds the configured maximum");
    }
    return result;
}

Result<ArtifactView> LedgerSnapshot::artifact(ArtifactId artifact) const {
    if (!valid()) {
        return Status(ErrorCode::NotReady, "snapshot is not attached to a ledger");
    }
    std::shared_lock lock(state_->mutex);
    const auto iterator = state_->artifacts.find(artifact);
    if (iterator == state_->artifacts.end()) {
        return Status(ErrorCode::NotFound, "artifact is not committed");
    }
    if (!visible(iterator->second.view.recorded_at, watermark_)) {
        return Status(ErrorCode::NotFound, "artifact is not committed at this snapshot");
    }
    ArtifactView view = iterator->second.view;
    if (iterator->second.invalidated_at.valid() &&
        !visible(iterator->second.invalidated_at, watermark_)) {
        // The invalidation this snapshot cannot see has not happened yet.
        view.validation = ValidationState::Validated;
    }
    return view;
}

Result<std::vector<ArtifactView>> LedgerSnapshot::artifact_ancestry(ArtifactId artifact) const {
    if (!valid()) {
        return Status(ErrorCode::NotReady, "snapshot is not attached to a ledger");
    }
    std::shared_lock lock(state_->mutex);
    const auto start = state_->artifacts.find(artifact);
    if (start == state_->artifacts.end()) {
        return Status(ErrorCode::NotFound, "artifact is not committed");
    }
    std::vector<ArtifactView> result;
    std::unordered_set<ArtifactId> visited;
    std::deque<ArtifactId> queue{artifact};
    visited.insert(artifact);
    while (!queue.empty()) {
        const ArtifactId current = queue.front();
        queue.pop_front();
        const auto entry = state_->artifacts.find(current);
        if (entry == state_->artifacts.end()) {
            return Status(ErrorCode::BrokenLineage, "artifact ancestry references an unknown artifact");
        }
        if (current != artifact) {
            result.push_back(entry->second.view);
        }
        for (const ArtifactId parent : entry->second.view.parents) {
            if (!visited.insert(parent).second) {
                continue;
            }
            queue.push_back(parent);
        }
        if (result.size() > state_->limits.max_lineage_nodes) {
            return Status(ErrorCode::LimitExceeded,
                          "artifact ancestry exceeds the configured maximum node count");
        }
    }
    std::sort(result.begin(), result.end(), [](const ArtifactView& lhs, const ArtifactView& rhs) {
        if (lhs.recorded_at == rhs.recorded_at) {
            return lhs.artifact < rhs.artifact;
        }
        return lhs.recorded_at < rhs.recorded_at;
    });
    return result;
}

Result<std::vector<ArtifactView>> LedgerSnapshot::artifact_descendants(ArtifactId artifact) const {
    if (!valid()) {
        return Status(ErrorCode::NotReady, "snapshot is not attached to a ledger");
    }
    std::shared_lock lock(state_->mutex);
    const auto start = state_->artifacts.find(artifact);
    if (start == state_->artifacts.end()) {
        return Status(ErrorCode::NotFound, "artifact is not committed");
    }
    std::vector<ArtifactView> result;
    std::unordered_set<ArtifactId> visited;
    std::deque<ArtifactId> queue{artifact};
    visited.insert(artifact);
    while (!queue.empty()) {
        const ArtifactId current = queue.front();
        queue.pop_front();
        const auto entry = state_->artifacts.find(current);
        if (entry == state_->artifacts.end()) {
            return Status(ErrorCode::BrokenLineage, "artifact lineage references an unknown artifact");
        }
        for (const ArtifactId child : entry->second.children) {
            const auto child_entry = state_->artifacts.find(child);
            if (child_entry == state_->artifacts.end()) {
                return Status(ErrorCode::BrokenLineage,
                              "artifact lineage references an unknown artifact");
            }
            if (!visited.insert(child).second) {
                return Status(ErrorCode::LineageCycle, "artifact lineage contains a cycle");
            }
            result.push_back(child_entry->second.view);
            queue.push_back(child);
        }
        if (result.size() > state_->limits.max_lineage_nodes) {
            return Status(ErrorCode::LimitExceeded,
                          "artifact descendants exceed the configured maximum node count");
        }
    }
    std::sort(result.begin(), result.end(), [](const ArtifactView& lhs, const ArtifactView& rhs) {
        if (lhs.recorded_at == rhs.recorded_at) {
            return lhs.artifact < rhs.artifact;
        }
        return lhs.recorded_at < rhs.recorded_at;
    });
    return result;
}

Result<FailureView> LedgerSnapshot::failure(FailureId failure) const {
    if (!valid()) {
        return Status(ErrorCode::NotReady, "snapshot is not attached to a ledger");
    }
    std::shared_lock lock(state_->mutex);
    const auto iterator = state_->failures.find(failure);
    if (iterator == state_->failures.end() ||
        iterator->second.recorded_at.value() > watermark_.value()) {
        return Status(ErrorCode::NotFound, "failure is not committed at this snapshot");
    }
    return iterator->second;
}

Result<std::vector<FailureView>> LedgerSnapshot::failures() const {
    if (!valid()) {
        return Status(ErrorCode::NotReady, "snapshot is not attached to a ledger");
    }
    std::shared_lock lock(state_->mutex);
    std::vector<FailureView> result;
    for (const auto& entry : state_->failures) {
        if (visible(entry.second.recorded_at, watermark_)) {
            result.push_back(entry.second);
        }
    }
    std::sort(result.begin(), result.end(), [](const FailureView& lhs, const FailureView& rhs) {
        return lhs.recorded_at < rhs.recorded_at;
    });
    if (result.size() > state_->limits.max_query_results) {
        return Status(ErrorCode::LimitExceeded, "query result exceeds the configured maximum");
    }
    return result;
}

Result<std::vector<FailureView>> LedgerSnapshot::failures_of(SubjectId scope) const {
    if (!valid()) {
        return Status(ErrorCode::NotReady, "snapshot is not attached to a ledger");
    }
    std::shared_lock lock(state_->mutex);
    std::vector<FailureView> result;
    for (const auto& entry : state_->failures) {
        if (entry.second.scope == scope && visible(entry.second.recorded_at, watermark_)) {
            result.push_back(entry.second);
        }
    }
    std::sort(result.begin(), result.end(), [](const FailureView& lhs, const FailureView& rhs) {
        return lhs.recorded_at < rhs.recorded_at;
    });
    return result;
}

Result<DecisionView> LedgerSnapshot::decision(DecisionId decision) const {
    if (!valid()) {
        return Status(ErrorCode::NotReady, "snapshot is not attached to a ledger");
    }
    std::shared_lock lock(state_->mutex);
    const auto iterator = state_->decisions.find(decision);
    if (iterator == state_->decisions.end() ||
        iterator->second.recorded_at.value() > watermark_.value()) {
        return Status(ErrorCode::NotFound, "decision is not committed at this snapshot");
    }
    return iterator->second;
}

Result<std::vector<DecisionView>> LedgerSnapshot::decisions_of(SubjectId subject) const {
    if (!valid()) {
        return Status(ErrorCode::NotReady, "snapshot is not attached to a ledger");
    }
    std::shared_lock lock(state_->mutex);
    std::vector<DecisionView> result;
    for (const auto& entry : state_->decisions) {
        if (entry.second.subject == subject && visible(entry.second.recorded_at, watermark_)) {
            result.push_back(entry.second);
        }
    }
    std::sort(result.begin(), result.end(), [](const DecisionView& lhs, const DecisionView& rhs) {
        return lhs.recorded_at < rhs.recorded_at;
    });
    if (result.size() > state_->limits.max_query_results) {
        return Status(ErrorCode::LimitExceeded, "query result exceeds the configured maximum");
    }
    return result;
}

Result<ResultView> LedgerSnapshot::result(ResultId result) const {
    if (!valid()) {
        return Status(ErrorCode::NotReady, "snapshot is not attached to a ledger");
    }
    std::shared_lock lock(state_->mutex);
    const auto iterator = state_->results.find(result);
    if (iterator == state_->results.end()) {
        return Status(ErrorCode::NotFound, "result is not committed");
    }
    if (!visible(iterator->second.declared_at, watermark_)) {
        return Status(ErrorCode::NotFound, "result is not committed at this snapshot");
    }
    ResultView view = iterator->second;
    if (!visible(view.last_changed_at, watermark_)) {
        // Only the declaration is visible: the result is still a candidate.
        view.generation = ResultGeneration::first();
        view.status = ResultStatus::Candidate;
        view.last_changed_at = view.declared_at;
        view.last_decision.reset();
        view.acceptance_decision.reset();
    }
    return view;
}

Result<std::vector<ResultView>> LedgerSnapshot::results_in_session(
    ResearchSessionId session) const {
    if (!valid()) {
        return Status(ErrorCode::NotReady, "snapshot is not attached to a ledger");
    }
    std::shared_lock lock(state_->mutex);
    if (state_->sessions.find(session) == state_->sessions.end()) {
        return Status(ErrorCode::NotFound, "research session is not committed");
    }
    std::vector<ResultView> result;
    const auto iterator = state_->session_results.find(session);
    if (iterator != state_->session_results.end()) {
        for (const ResultId id : iterator->second) {
            const auto entry = state_->results.find(id);
            if (entry != state_->results.end() && visible(entry->second.declared_at, watermark_)) {
                result.push_back(entry->second);
            }
        }
    }
    std::sort(result.begin(), result.end(), [](const ResultView& lhs, const ResultView& rhs) {
        return lhs.declared_at < rhs.declared_at;
    });
    return result;
}

Result<RecordSequence> LedgerSnapshot::subject_sequence(SubjectId subject) const {
    if (!valid()) {
        return Status(ErrorCode::NotReady, "snapshot is not attached to a ledger");
    }
    if (!subject.valid()) {
        return Status(ErrorCode::InvalidIdentity, "subject identity is not valid");
    }
    std::shared_lock lock(state_->mutex);
    const auto iterator = state_->subject_index.find(SubjectKey{subject.kind(), subject.value()});
    if (iterator == state_->subject_index.end() ||
        iterator->second.sequence.value() > watermark_.value()) {
        return Status(ErrorCode::NotFound, "subject is not committed at this snapshot");
    }
    return iterator->second.sequence;
}

Result<Record> LedgerSnapshot::record_at(RecordSequence sequence) const {
    if (!valid()) {
        return Status(ErrorCode::NotReady, "snapshot is not attached to a ledger");
    }
    std::shared_lock lock(state_->mutex);
    if (!sequence.valid() || sequence.value() > watermark_.value()) {
        return Status(ErrorCode::NotFound, "record is not committed at this snapshot");
    }
    const std::size_t index = static_cast<std::size_t>(sequence.value()) - 1;
    if (index >= state_->records.size()) {
        return Status(ErrorCode::NotFound, "record is not committed");
    }
    return state_->records[index];
}

Result<std::vector<Record>> LedgerSnapshot::records(RecordSequence from, RecordSequence to) const {
    if (!valid()) {
        return Status(ErrorCode::NotReady, "snapshot is not attached to a ledger");
    }
    if (!from.valid() || !to.valid() || to.value() < from.value()) {
        return Status(ErrorCode::InvalidArgument, "record range is not valid");
    }
    std::shared_lock lock(state_->mutex);
    const std::uint64_t last = std::min<std::uint64_t>(to.value(), watermark_.value());
    if (from.value() > last) {
        return std::vector<Record>{};
    }
    if (last - from.value() + 1 > state_->limits.max_query_results) {
        return Status(ErrorCode::LimitExceeded, "record range exceeds the configured maximum");
    }
    std::vector<Record> result;
    result.reserve(static_cast<std::size_t>(last - from.value() + 1));
    for (std::uint64_t sequence = from.value(); sequence <= last; ++sequence) {
        const std::size_t index = static_cast<std::size_t>(sequence) - 1;
        if (index < state_->records.size()) {
            result.push_back(state_->records[index]);
        }
    }
    return result;
}

}  // namespace research_ledger
