#include "research_ledger/identity.hpp"

#include <array>
#include <type_traits>
#include <utility>

namespace research_ledger {
namespace {

using DomainEntry = std::pair<IdentityDomain, std::string_view>;

constexpr std::array<DomainEntry, 28> kDomainNames{{
    {IdentityDomain::None, "none"},
    {IdentityDomain::ResearchSession, "research-session"},
    {IdentityDomain::Hypothesis, "hypothesis"},
    {IdentityDomain::HypothesisGeneration, "hypothesis-generation"},
    {IdentityDomain::Experiment, "experiment"},
    {IdentityDomain::ExperimentGeneration, "experiment-generation"},
    {IdentityDomain::Branch, "branch"},
    {IdentityDomain::Attempt, "attempt"},
    {IdentityDomain::AttemptGeneration, "attempt-generation"},
    {IdentityDomain::ModelCall, "model-call"},
    {IdentityDomain::ToolCall, "tool-call"},
    {IdentityDomain::Dataset, "dataset"},
    {IdentityDomain::Input, "input"},
    {IdentityDomain::Artifact, "artifact"},
    {IdentityDomain::ArtifactGeneration, "artifact-generation"},
    {IdentityDomain::Observation, "observation"},
    {IdentityDomain::Metric, "metric"},
    {IdentityDomain::Failure, "failure"},
    {IdentityDomain::Decision, "decision"},
    {IdentityDomain::Result, "result"},
    {IdentityDomain::ResultGeneration, "result-generation"},
    {IdentityDomain::Worker, "worker"},
    {IdentityDomain::WorkerBoot, "worker-boot"},
    {IdentityDomain::CoordinatorEpoch, "coordinator-epoch"},
    {IdentityDomain::LedgerGeneration, "ledger-generation"},
    {IdentityDomain::RecordSequence, "record-sequence"},
    {IdentityDomain::Record, "record"},
    {IdentityDomain::PolicyGeneration, "policy-generation"},
}};

template <class Value>
std::string render(IdentityDomain domain, Value value) {
    std::string text(identity_domain_name(domain));
    text += ':';
    if constexpr (sizeof(Value) == 8) {
        text += std::to_string(static_cast<std::uint64_t>(value));
    } else {
        text += std::to_string(static_cast<std::uint32_t>(value));
    }
    return text;
}

// Parses "<domain>:<decimal>". The domain must match exactly; a value of zero is
// never a valid identity, so it is rejected here as well as at decode time.
std::optional<std::uint64_t> parse_value(std::string_view text, IdentityDomain expected) {
    const std::size_t separator = text.find(':');
    if (separator == std::string_view::npos || separator == 0) {
        return std::nullopt;
    }
    if (text.substr(0, separator) != identity_domain_name(expected)) {
        return std::nullopt;
    }
    const std::string_view digits = text.substr(separator + 1);
    if (digits.empty() || digits.size() > 20) {
        return std::nullopt;
    }
    std::uint64_t value = 0;
    for (const char character : digits) {
        if (character < '0' || character > '9') {
            return std::nullopt;
        }
        const std::uint64_t digit = static_cast<std::uint64_t>(character - '0');
        if (value > (0xffffffffffffffffull - digit) / 10ull) {
            return std::nullopt;
        }
        value = value * 10ull + digit;
    }
    if (value == kInvalidIdentityValue) {
        return std::nullopt;
    }
    return value;
}

}  // namespace

std::string_view identity_domain_name(IdentityDomain domain) noexcept {
    for (const DomainEntry& entry : kDomainNames) {
        if (entry.first == domain) {
            return entry.second;
        }
    }
    return "none";
}

std::optional<IdentityDomain> parse_identity_domain(std::string_view text) noexcept {
    for (const DomainEntry& entry : kDomainNames) {
        if (entry.second == text) {
            return entry.first;
        }
    }
    return std::nullopt;
}

#define RESEARCH_LEDGER_TO_STRING(TYPE, DOMAIN)          \
    std::string to_string(TYPE value) {                  \
        return render(DOMAIN, value.value());            \
    }

RESEARCH_LEDGER_TO_STRING(ResearchSessionId, IdentityDomain::ResearchSession)
RESEARCH_LEDGER_TO_STRING(HypothesisId, IdentityDomain::Hypothesis)
RESEARCH_LEDGER_TO_STRING(HypothesisGeneration, IdentityDomain::HypothesisGeneration)
RESEARCH_LEDGER_TO_STRING(ExperimentId, IdentityDomain::Experiment)
RESEARCH_LEDGER_TO_STRING(ExperimentGeneration, IdentityDomain::ExperimentGeneration)
RESEARCH_LEDGER_TO_STRING(BranchId, IdentityDomain::Branch)
RESEARCH_LEDGER_TO_STRING(AttemptId, IdentityDomain::Attempt)
RESEARCH_LEDGER_TO_STRING(AttemptGeneration, IdentityDomain::AttemptGeneration)
RESEARCH_LEDGER_TO_STRING(ModelCallId, IdentityDomain::ModelCall)
RESEARCH_LEDGER_TO_STRING(ToolCallId, IdentityDomain::ToolCall)
RESEARCH_LEDGER_TO_STRING(DatasetId, IdentityDomain::Dataset)
RESEARCH_LEDGER_TO_STRING(InputId, IdentityDomain::Input)
RESEARCH_LEDGER_TO_STRING(ArtifactId, IdentityDomain::Artifact)
RESEARCH_LEDGER_TO_STRING(ArtifactGeneration, IdentityDomain::ArtifactGeneration)
RESEARCH_LEDGER_TO_STRING(ObservationId, IdentityDomain::Observation)
RESEARCH_LEDGER_TO_STRING(MetricId, IdentityDomain::Metric)
RESEARCH_LEDGER_TO_STRING(FailureId, IdentityDomain::Failure)
RESEARCH_LEDGER_TO_STRING(DecisionId, IdentityDomain::Decision)
RESEARCH_LEDGER_TO_STRING(ResultId, IdentityDomain::Result)
RESEARCH_LEDGER_TO_STRING(ResultGeneration, IdentityDomain::ResultGeneration)
RESEARCH_LEDGER_TO_STRING(WorkerId, IdentityDomain::Worker)
RESEARCH_LEDGER_TO_STRING(WorkerBootId, IdentityDomain::WorkerBoot)
RESEARCH_LEDGER_TO_STRING(CoordinatorEpoch, IdentityDomain::CoordinatorEpoch)
RESEARCH_LEDGER_TO_STRING(LedgerGeneration, IdentityDomain::LedgerGeneration)
RESEARCH_LEDGER_TO_STRING(RecordSequence, IdentityDomain::RecordSequence)
RESEARCH_LEDGER_TO_STRING(LedgerRecordId, IdentityDomain::Record)
RESEARCH_LEDGER_TO_STRING(PolicyGeneration, IdentityDomain::PolicyGeneration)

#undef RESEARCH_LEDGER_TO_STRING

#define RESEARCH_LEDGER_PARSE(FUNCTION, TYPE, DOMAIN)                       \
    std::optional<TYPE> FUNCTION(std::string_view text) {                   \
        const std::optional<std::uint64_t> value = parse_value(text, DOMAIN); \
        if (!value.has_value()) {                                           \
            return std::nullopt;                                            \
        }                                                                   \
        if constexpr (std::is_same_v<typename TYPE::rep_type, std::uint64_t>) { \
            return TYPE::from_value(value.value());                         \
        } else {                                                            \
            if (value.value() > 0xffffffffull) {                            \
                return std::nullopt;                                        \
            }                                                               \
            return TYPE::from_value(static_cast<std::uint32_t>(value.value())); \
        }                                                                   \
    }

RESEARCH_LEDGER_PARSE(parse_research_session_id, ResearchSessionId, IdentityDomain::ResearchSession)
RESEARCH_LEDGER_PARSE(parse_hypothesis_id, HypothesisId, IdentityDomain::Hypothesis)
RESEARCH_LEDGER_PARSE(parse_experiment_id, ExperimentId, IdentityDomain::Experiment)
RESEARCH_LEDGER_PARSE(parse_branch_id, BranchId, IdentityDomain::Branch)
RESEARCH_LEDGER_PARSE(parse_attempt_id, AttemptId, IdentityDomain::Attempt)
RESEARCH_LEDGER_PARSE(parse_artifact_id, ArtifactId, IdentityDomain::Artifact)
RESEARCH_LEDGER_PARSE(parse_model_call_id, ModelCallId, IdentityDomain::ModelCall)
RESEARCH_LEDGER_PARSE(parse_tool_call_id, ToolCallId, IdentityDomain::ToolCall)
RESEARCH_LEDGER_PARSE(parse_result_id, ResultId, IdentityDomain::Result)
RESEARCH_LEDGER_PARSE(parse_failure_id, FailureId, IdentityDomain::Failure)
RESEARCH_LEDGER_PARSE(parse_decision_id, DecisionId, IdentityDomain::Decision)
RESEARCH_LEDGER_PARSE(parse_observation_id, ObservationId, IdentityDomain::Observation)

#undef RESEARCH_LEDGER_PARSE

}  // namespace research_ledger
