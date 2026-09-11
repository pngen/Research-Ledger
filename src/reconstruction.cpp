#include "ledger_state.hpp"

#include <algorithm>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include "research_ledger/replay.hpp"

namespace research_ledger {
namespace {

SubjectKey key_of(const SubjectId& subject) { return SubjectKey{subject.kind(), subject.value()}; }

template <class Tag>
std::string render_measure(const Measure<Tag>& measure) {
    if (!measure.is_known()) {
        return "UNKNOWN";
    }
    std::string text = std::to_string(measure.units());
    text += " (";
    text += provenance_name(measure.provenance());
    text += ")";
    return text;
}

std::string render_monetary(const MonetaryMeasure& measure) {
    if (!measure.known) {
        return "UNKNOWN";
    }
    std::string text = std::to_string(measure.micro_units);
    text += " micro-";
    text += measure.currency;
    text += " (";
    text += provenance_name(measure.provenance);
    text += ")";
    return text;
}

// Canonical rendering of one entity, used both by explanations and by the
// logical digest of reconstructed state. The rendering depends only on
// committed logical state, never on insertion order.
std::string render_session(const SessionView& view) {
    std::string line = "SESSION ";
    line += to_string(view.session);
    line += " state=";
    line += session_state_name(view.state);
    line += " provenance=";
    line += provenance_name(view.provenance);
    line += " label=";
    line += view.label;
    line += " question=";
    line += view.question;
    return line;
}

std::string render_hypothesis(const HypothesisView& view) {
    std::string line = "HYPOTHESIS ";
    line += to_string(view.hypothesis);
    line += " generation=";
    line += std::to_string(view.generation.value());
    line += " status=";
    line += hypothesis_status_name(view.status);
    if (view.parent.has_value()) {
        line += " parent=";
        line += to_string(*view.parent);
        line += ":";
        line += std::to_string(view.parent_generation.value());
    }
    line += " claim=";
    line += view.claim;
    return line;
}

std::string render_experiment(const ExperimentView& view) {
    std::string line = "EXPERIMENT ";
    line += to_string(view.experiment);
    line += " generation=";
    line += std::to_string(view.generation.value());
    line += " branch=";
    line += to_string(view.branch);
    line += " environment=";
    line += view.environment_reference;
    return line;
}

std::string render_branch(const BranchView& view) {
    std::string line = "BRANCH ";
    line += to_string(view.branch);
    line += " kind=";
    line += branch_kind_name(view.kind);
    line += " label=";
    line += view.label;
    return line;
}

std::string render_attempt(const AttemptView& view) {
    std::string line = "ATTEMPT ";
    line += to_string(view.attempt);
    line += " generation=";
    line += std::to_string(view.generation.value());
    line += " state=";
    line += attempt_state_name(view.state);
    line += " experiment=";
    line += to_string(view.experiment);
    if (view.failure.has_value()) {
        line += " failure=";
        line += to_string(*view.failure);
    }
    return line;
}

std::string render_model_call(const ModelCallView& view) {
    std::string line = "MODEL_CALL ";
    line += to_string(view.call);
    line += " attempt=";
    line += to_string(view.attempt);
    line += " model=";
    line += view.model_identity;
    line += "@";
    line += view.model_revision;
    line += " provider=";
    line += view.provider;
    line += " outcome=";
    line += model_call_outcome_name(view.outcome);
    line += " input_tokens=";
    line += render_measure(view.input_tokens);
    line += " output_tokens=";
    line += render_measure(view.output_tokens);
    line += " latency_ns=";
    line += render_measure(view.latency);
    line += " cost=";
    line += render_monetary(view.cost);
    return line;
}

std::string render_tool_call(const ToolCallView& view) {
    std::string line = "TOOL_CALL ";
    line += to_string(view.call);
    line += " attempt=";
    line += to_string(view.attempt);
    line += " tool=";
    line += view.tool_identity;
    line += "@";
    line += view.tool_version;
    line += " state=";
    line += tool_call_state_name(view.state);
    return line;
}

std::string render_artifact(const ArtifactView& view) {
    std::string line = "ARTIFACT ";
    line += to_string(view.artifact);
    line += " generation=";
    line += std::to_string(view.generation.value());
    line += " role=";
    line += artifact_role_name(view.role);
    line += " validation=";
    line += validation_state_name(view.validation);
    line += " digest=";
    line += to_hex(view.content_digest);
    line += " producer=";
    line += view.producer.to_string();
    return line;
}

std::string render_observation(const ObservationView& view) {
    std::string line = "OBSERVATION ";
    line += to_string(view.observation);
    line += " attempt=";
    line += to_string(view.attempt);
    line += " key=";
    line += view.key;
    line += " unit=";
    line += unit_kind_name(view.unit);
    line += " value=";
    line += view.value.to_text();
    line += " provenance=";
    line += provenance_name(view.provenance);
    return line;
}

std::string render_failure(const FailureView& view) {
    std::string line = "FAILURE ";
    line += to_string(view.failure);
    line += " category=";
    line += failure_category_name(view.category);
    line += " scope=";
    line += view.scope.to_string();
    line += " retriable=";
    line += view.retriable ? "true" : "false";
    line += " terminal=";
    line += view.terminal ? "true" : "false";
    line += " message=";
    line += view.message;
    return line;
}

std::string render_decision(const DecisionView& view) {
    std::string line = "DECISION ";
    line += to_string(view.decision);
    line += " subject=";
    line += view.subject.to_string();
    line += " type=";
    line += decision_type_name(view.type);
    line += " outcome=";
    line += decision_outcome_name(view.outcome);
    line += " policy=";
    line += view.policy_identity;
    return line;
}

std::string render_result(const ResultView& view) {
    std::string line = "RESULT ";
    line += to_string(view.result);
    line += " generation=";
    line += std::to_string(view.generation.value());
    line += " status=";
    line += result_status_name(view.status);
    if (view.acceptance_decision.has_value()) {
        line += " acceptance=";
        line += to_string(*view.acceptance_decision);
    }
    if (view.content_digest.has_value()) {
        line += " digest=";
        line += to_hex(*view.content_digest);
    }
    line += " summary=";
    line += view.summary;
    return line;
}

struct DigestEntry {
    std::uint8_t rank = 0;
    std::uint64_t identity = 0;
    std::string line{};
};

}  // namespace

Digest logical_state_digest_of(const LedgerState& state, RecordSequence watermark) {
    std::vector<DigestEntry> entries;
    entries.reserve(state.sessions.size() + state.hypotheses.size() + state.experiments.size() +
                    state.branches.size() + state.attempts.size() + state.model_calls.size() +
                    state.tool_calls.size() + state.artifacts.size() + state.observations.size() +
                    state.failures.size() + state.decisions.size() + state.results.size());
    auto visible = [watermark](RecordSequence sequence) {
        return !sequence.valid() || sequence.value() <= watermark.value();
    };
    for (const auto& entry : state.sessions) {
        if (visible(entry.second.opened_at)) {
            entries.push_back(DigestEntry{1, entry.first.value(), render_session(entry.second)});
        }
    }
    for (const auto& entry : state.hypotheses) {
        if (visible(entry.second.declared_at)) {
            entries.push_back(DigestEntry{2, entry.first.value(), render_hypothesis(entry.second)});
        }
    }
    for (const auto& entry : state.experiments) {
        if (visible(entry.second.declared_at)) {
            entries.push_back(DigestEntry{3, entry.first.value(), render_experiment(entry.second)});
        }
    }
    for (const auto& entry : state.branches) {
        if (visible(entry.second.declared_at)) {
            entries.push_back(DigestEntry{4, entry.first.value(), render_branch(entry.second)});
        }
    }
    for (const auto& entry : state.attempts) {
        if (visible(entry.second.started_at)) {
            entries.push_back(DigestEntry{5, entry.first.value(), render_attempt(entry.second)});
        }
    }
    for (const auto& entry : state.model_calls) {
        if (visible(entry.second.recorded_at)) {
            entries.push_back(DigestEntry{6, entry.first.value(), render_model_call(entry.second)});
        }
    }
    for (const auto& entry : state.tool_calls) {
        if (visible(entry.second.recorded_at)) {
            entries.push_back(DigestEntry{7, entry.first.value(), render_tool_call(entry.second)});
        }
    }
    for (const auto& entry : state.artifacts) {
        if (visible(entry.second.view.recorded_at)) {
            entries.push_back(DigestEntry{8, entry.first.value(), render_artifact(entry.second.view)});
        }
    }
    for (const auto& entry : state.observations) {
        if (visible(entry.second.recorded_at)) {
            entries.push_back(DigestEntry{9, entry.first.value(), render_observation(entry.second)});
        }
    }
    for (const auto& entry : state.failures) {
        if (visible(entry.second.recorded_at)) {
            entries.push_back(DigestEntry{10, entry.first.value(), render_failure(entry.second)});
        }
    }
    for (const auto& entry : state.decisions) {
        if (visible(entry.second.recorded_at)) {
            entries.push_back(DigestEntry{11, entry.first.value(), render_decision(entry.second)});
        }
    }
    for (const auto& entry : state.results) {
        if (visible(entry.second.declared_at)) {
            entries.push_back(DigestEntry{12, entry.first.value(), render_result(entry.second)});
        }
    }
    for (const auto& entry : state.metrics) {
        if (!visible(entry.second.declared_at)) {
            continue;
        }
        std::string line = "METRIC ";
        line += to_string(entry.second.metric);
        line += " key=";
        line += entry.second.canonical_key;
        line += " unit=";
        line += unit_kind_name(entry.second.unit);
        entries.push_back(DigestEntry{13, entry.first.value(), std::move(line)});
    }
    for (const AccountingState& entry : state.accounting) {
        if (!visible(entry.sequence)) {
            continue;
        }
        std::string line = "ACCOUNTING ";
        line += entry.scope.to_string();
        line += " input_tokens=";
        line += render_measure(entry.accounting.model_input_tokens);
        line += " output_tokens=";
        line += render_measure(entry.accounting.model_output_tokens);
        line += " model_calls=";
        line += render_measure(entry.accounting.model_calls);
        line += " tool_calls=";
        line += render_measure(entry.accounting.tool_calls);
        line += " accelerator_nanos=";
        line += render_measure(entry.accounting.accelerator_nanos);
        line += " cpu_nanos=";
        line += render_measure(entry.accounting.cpu_nanos);
        line += " wall_nanos=";
        line += render_measure(entry.accounting.wall_nanos);
        line += " storage_bytes=";
        line += render_measure(entry.accounting.storage_bytes);
        line += " transfer_bytes=";
        line += render_measure(entry.accounting.transfer_bytes);
        line += " energy_micro_joules=";
        line += render_measure(entry.accounting.energy_micro_joules);
        line += " attempts=";
        line += render_measure(entry.accounting.attempts);
        line += " retries=";
        line += render_measure(entry.accounting.retries);
        line += " failure_overhead_nanos=";
        line += render_measure(entry.accounting.failure_overhead_nanos);
        line += " monetary=";
        line += render_monetary(entry.accounting.monetary);
        entries.push_back(DigestEntry{14, entry.sequence.value(), std::move(line)});
    }

    std::sort(entries.begin(), entries.end(), [](const DigestEntry& lhs, const DigestEntry& rhs) {
        if (lhs.rank != rhs.rank) {
            return lhs.rank < rhs.rank;
        }
        if (lhs.identity != rhs.identity) {
            return lhs.identity < rhs.identity;
        }
        return lhs.line < rhs.line;
    });

    DigestBuilder builder;
    for (const DigestEntry& entry : entries) {
        builder.update(entry.line);
        builder.update_u8(0x0a);
    }
    return builder.finish();
}

Result<Digest> logical_state_digest(const Ledger& ledger) {
    return ledger.logical_digest();
}

Digest Ledger::logical_digest() const {
    std::shared_lock lock(state_->mutex);
    return logical_state_digest_of(*state_, state_->last_sequence);
}

std::string Explanation::to_text() const {
    std::string text;
    for (const ExplanationLine& line : lines) {
        text += line.text;
        text += "\n";
    }
    if (truncated) {
        text += "(explanation truncated at the configured maximum)\n";
    }
    return text;
}

Result<Explanation> LedgerSnapshot::explain_result(ResultId result) const {
    auto bundle = supporting_evidence(result);
    if (!bundle.ok()) {
        return bundle.status();
    }
    Explanation explanation;
    const std::size_t maximum = state_->limits.max_explanation_lines;
    auto push = [&](std::string line) {
        if (explanation.lines.size() >= maximum) {
            explanation.truncated = true;
            return false;
        }
        explanation.lines.push_back(ExplanationLine{std::move(line)});
        return true;
    };

    push(render_result(bundle.value().result));
    const auto session = state_->sessions.find(bundle.value().result.session);
    if (session != state_->sessions.end()) {
        push(render_session(session->second));
    }
    for (const HypothesisView& view : bundle.value().hypotheses) {
        push(render_hypothesis(view));
    }
    for (const ExperimentView& view : bundle.value().experiments) {
        push(render_experiment(view));
    }
    for (const AttemptView& view : bundle.value().attempts) {
        push(render_attempt(view));
    }
    for (const ModelCallView& view : bundle.value().model_calls) {
        push(render_model_call(view));
    }
    for (const ToolCallView& view : bundle.value().tool_calls) {
        push(render_tool_call(view));
    }
    for (const ObservationView& view : bundle.value().observations) {
        push(render_observation(view));
    }
    for (const ArtifactView& view : bundle.value().artifacts) {
        push(render_artifact(view));
    }
    for (const FailureView& view : bundle.value().failures) {
        push(render_failure(view));
    }
    for (const DecisionView& view : bundle.value().decisions) {
        push(render_decision(view));
        for (const EvidenceRef& evidence : view.evidence) {
            std::string line = "  EVIDENCE ";
            line += evidence.subject.to_string();
            line += " at sequence ";
            line += std::to_string(evidence.sequence.value());
            push(std::move(line));
        }
    }
    return explanation;
}

Result<EvidenceBundle> LedgerSnapshot::supporting_evidence(ResultId result) const {
    if (!valid()) {
        return Status(ErrorCode::NotReady, "snapshot is not attached to a ledger");
    }
    std::shared_lock lock(state_->mutex);
    const auto entry = state_->results.find(result);
    if (entry == state_->results.end() ||
        entry->second.declared_at.value() > watermark_.value()) {
        return Status(ErrorCode::NotFound, "result is not committed");
    }
    // A snapshot observes exactly the records committed at or before its
    // watermark. Everything below is filtered the same way, so an older
    // snapshot never reports evidence that arrived later.
    const auto visible = [this](RecordSequence sequence) {
        return sequence.value() <= watermark_.value();
    };
    EvidenceBundle bundle;
    bundle.result = entry->second;

    std::unordered_set<SubjectKey, SubjectKeyHash> visited;
    auto note = [&](const SubjectId& subject) {
        return visited.insert(key_of(subject)).second;
    };
    note(SubjectId::of(result));

    // Hypotheses, including the revision lineage they descend from.
    for (const HypothesisId hypothesis : bundle.result.hypotheses) {
        HypothesisId current = hypothesis;
        for (std::uint32_t depth = 0; depth < state_->limits.max_lineage_depth; ++depth) {
            const auto view = state_->hypotheses.find(current);
            if (view == state_->hypotheses.end() ||
                view->second.declared_at.value() > watermark_.value()) {
                return Status(ErrorCode::BrokenLineage, "result hypothesis lineage is broken");
            }
            if (!note(SubjectId::of(current))) {
                break;
            }
            HypothesisView revision = view->second;
            if (revision.last_changed_at.value() > watermark_.value()) {
                // The revision this snapshot cannot see has not happened yet.
                revision.generation = HypothesisGeneration::first();
                revision.status = HypothesisStatus::Proposed;
                revision.last_changed_at = revision.declared_at;
            }
            bundle.hypotheses.push_back(std::move(revision));
            if (!view->second.parent.has_value()) {
                break;
            }
            current = *view->second.parent;
        }
    }

    // Experiments and their parent experiments.
    std::vector<ExperimentId> experiments = bundle.result.experiments;
    for (std::size_t index = 0; index < experiments.size(); ++index) {
        const ExperimentId experiment = experiments[index];
        const auto view = state_->experiments.find(experiment);
        if (view == state_->experiments.end() ||
            view->second.declared_at.value() > watermark_.value()) {
            return Status(ErrorCode::BrokenLineage, "result experiment is missing");
        }
        if (note(SubjectId::of(experiment))) {
            bundle.experiments.push_back(view->second);
        }
        if (view->second.parent_experiment.has_value() &&
            bundle.experiments.size() + experiments.size() < state_->limits.max_lineage_nodes) {
            experiments.push_back(*view->second.parent_experiment);
        }
        if (experiments.size() > state_->limits.max_lineage_nodes) {
            bundle.truncated = true;
            break;
        }
    }

    // Attempts of those experiments, with their calls and observations.
    std::unordered_set<AttemptId> attempt_ids;
    for (const ExperimentView& view : bundle.experiments) {
        for (const AttemptId attempt : view.attempts) {
            attempt_ids.insert(attempt);
        }
    }
    for (const AttemptId attempt : attempt_ids) {
        const auto view = state_->attempts.find(attempt);
        if (view == state_->attempts.end() ||
            view->second.started_at.value() > watermark_.value()) {
            return Status(ErrorCode::BrokenLineage, "result attempt is missing");
        }
        if (!note(SubjectId::of(attempt))) {
            continue;
        }
        AttemptView attempt_view = view->second;
        if (attempt_view.terminated_at.valid() &&
            attempt_view.terminated_at.value() > watermark_.value()) {
            // The attempt was still running when this snapshot was taken.
            attempt_view.state = AttemptState::Running;
            attempt_view.terminated_at = RecordSequence{};
            attempt_view.failure.reset();
        }
        bundle.attempts.push_back(std::move(attempt_view));
    }
    std::sort(bundle.attempts.begin(), bundle.attempts.end(),
              [](const AttemptView& lhs, const AttemptView& rhs) {
                  return lhs.started_at < rhs.started_at;
              });

    for (const auto& call : state_->model_calls) {
        if (!visible(call.second.recorded_at)) {
            continue;
        }
        if (attempt_ids.find(call.second.attempt) != attempt_ids.end() ||
            std::find(bundle.result.model_calls.begin(), bundle.result.model_calls.end(),
                      call.first) != bundle.result.model_calls.end()) {
            bundle.model_calls.push_back(call.second);
        }
    }
    for (const auto& call : state_->tool_calls) {
        if (!visible(call.second.recorded_at)) {
            continue;
        }
        if (attempt_ids.find(call.second.attempt) != attempt_ids.end() ||
            std::find(bundle.result.tool_calls.begin(), bundle.result.tool_calls.end(),
                      call.first) != bundle.result.tool_calls.end()) {
            bundle.tool_calls.push_back(call.second);
        }
    }
    for (const auto& observation : state_->observations) {
        if (!visible(observation.second.recorded_at)) {
            continue;
        }
        if (attempt_ids.find(observation.second.attempt) != attempt_ids.end() ||
            std::find(bundle.result.observations.begin(), bundle.result.observations.end(),
                      observation.first) != bundle.result.observations.end()) {
            bundle.observations.push_back(observation.second);
        }
    }
    std::sort(bundle.model_calls.begin(), bundle.model_calls.end(),
              [](const ModelCallView& lhs, const ModelCallView& rhs) {
                  return lhs.recorded_at < rhs.recorded_at;
              });
    std::sort(bundle.tool_calls.begin(), bundle.tool_calls.end(),
              [](const ToolCallView& lhs, const ToolCallView& rhs) {
                  return lhs.recorded_at < rhs.recorded_at;
              });
    std::sort(bundle.observations.begin(), bundle.observations.end(),
              [](const ObservationView& lhs, const ObservationView& rhs) {
                  return lhs.recorded_at < rhs.recorded_at;
              });

    // Artifacts: the ones the result names, the ones the included attempts
    // produced, and the transitive ancestry of both.
    std::vector<ArtifactId> artifacts = bundle.result.artifacts;
    for (const auto& artifact : state_->artifacts) {
        if (!visible(artifact.second.view.recorded_at)) {
            continue;
        }
        const SubjectId producer = artifact.second.view.producer;
        if (producer.kind() == SubjectKind::Attempt &&
            attempt_ids.find(*producer.as<AttemptId>()) != attempt_ids.end()) {
            artifacts.push_back(artifact.first);
        }
        if (producer.kind() == SubjectKind::Experiment &&
            std::find(bundle.result.experiments.begin(), bundle.result.experiments.end(),
                      *producer.as<ExperimentId>()) != bundle.result.experiments.end()) {
            artifacts.push_back(artifact.first);
        }
    }
    for (std::size_t index = 0; index < artifacts.size(); ++index) {
        const ArtifactId artifact = artifacts[index];
        const auto view = state_->artifacts.find(artifact);
        if (view == state_->artifacts.end() ||
            view->second.view.recorded_at.value() > watermark_.value()) {
            return Status(ErrorCode::BrokenLineage, "result artifact is missing");
        }
        if (note(SubjectId::of(artifact))) {
            ArtifactView artifact_view = view->second.view;
            if (view->second.invalidated_at.valid() &&
                !visible(view->second.invalidated_at)) {
                artifact_view.validation = ValidationState::Validated;
            }
            bundle.artifacts.push_back(std::move(artifact_view));
        } else {
            continue;
        }
        for (const ArtifactId parent : view->second.view.parents) {
            artifacts.push_back(parent);
        }
        if (artifacts.size() > state_->limits.max_lineage_nodes) {
            bundle.truncated = true;
            break;
        }
    }
    std::sort(bundle.artifacts.begin(), bundle.artifacts.end(),
              [](const ArtifactView& lhs, const ArtifactView& rhs) {
                  if (lhs.recorded_at == rhs.recorded_at) {
                      return lhs.artifact < rhs.artifact;
                  }
                  return lhs.recorded_at < rhs.recorded_at;
              });

    // Failures and decisions that concern anything already in the closure.
    for (const auto& failure : state_->failures) {
        if (!visible(failure.second.recorded_at)) {
            continue;
        }
        if (visited.find(key_of(failure.second.scope)) != visited.end()) {
            bundle.failures.push_back(failure.second);
        }
    }
    std::sort(bundle.failures.begin(), bundle.failures.end(),
              [](const FailureView& lhs, const FailureView& rhs) {
                  return lhs.recorded_at < rhs.recorded_at;
              });
    for (const auto& decision : state_->decisions) {
        if (!visible(decision.second.recorded_at)) {
            continue;
        }
        if (visited.find(key_of(decision.second.subject)) != visited.end()) {
            bundle.decisions.push_back(decision.second);
            for (const EvidenceRef& evidence : decision.second.evidence) {
                bundle.cited_evidence.push_back(evidence);
            }
        }
    }
    std::sort(bundle.decisions.begin(), bundle.decisions.end(),
              [](const DecisionView& lhs, const DecisionView& rhs) {
                  return lhs.recorded_at < rhs.recorded_at;
              });

    bundle.nodes_visited = visited.size();
    bundle.complete = !bundle.truncated;
    bundle.reconstructable = bundle.result.status != ResultStatus::Accepted ||
                             bundle.result.acceptance_decision.has_value();
    RecordSequence highest{};
    auto raise = [&highest](RecordSequence sequence) {
        if (sequence.valid() && (!highest.valid() || highest < sequence)) {
            highest = sequence;
        }
    };
    for (const HypothesisView& view : bundle.hypotheses) raise(view.last_changed_at);
    for (const ExperimentView& view : bundle.experiments) raise(view.declared_at);
    for (const AttemptView& view : bundle.attempts) raise(view.terminated_at);
    for (const ModelCallView& view : bundle.model_calls) raise(view.recorded_at);
    for (const ToolCallView& view : bundle.tool_calls) raise(view.recorded_at);
    for (const ObservationView& view : bundle.observations) raise(view.recorded_at);
    for (const ArtifactView& view : bundle.artifacts) raise(view.recorded_at);
    for (const FailureView& view : bundle.failures) raise(view.recorded_at);
    for (const DecisionView& view : bundle.decisions) raise(view.recorded_at);
    raise(bundle.result.last_changed_at);
    bundle.highest_sequence = highest;

    DigestBuilder digest;
    digest.update(render_result(bundle.result));
    for (const HypothesisView& view : bundle.hypotheses) digest.update(render_hypothesis(view));
    for (const ExperimentView& view : bundle.experiments) digest.update(render_experiment(view));
    for (const AttemptView& view : bundle.attempts) digest.update(render_attempt(view));
    for (const ModelCallView& view : bundle.model_calls) digest.update(render_model_call(view));
    for (const ToolCallView& view : bundle.tool_calls) digest.update(render_tool_call(view));
    for (const ObservationView& view : bundle.observations) digest.update(render_observation(view));
    for (const ArtifactView& view : bundle.artifacts) digest.update(render_artifact(view));
    for (const FailureView& view : bundle.failures) digest.update(render_failure(view));
    for (const DecisionView& view : bundle.decisions) digest.update(render_decision(view));
    bundle.lineage_digest = digest.finish();
    return bundle;
}

}  // namespace research_ledger
