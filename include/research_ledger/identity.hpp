#pragma once

#include <compare>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace research_ledger {

// Identity domains never share a representation in the public API. A
// HypothesisId cannot be passed where an ExperimentId is expected, and a
// serialized identity carries its domain tag, so decoding one domain as another
// is an explicit INVALID_IDENTITY failure instead of a plausible value.
enum class IdentityDomain : std::uint8_t {
    None = 0,
    ResearchSession = 1,
    Hypothesis = 2,
    HypothesisGeneration = 3,
    Experiment = 4,
    ExperimentGeneration = 5,
    Branch = 6,
    Attempt = 7,
    AttemptGeneration = 8,
    ModelCall = 9,
    ToolCall = 10,
    Dataset = 11,
    Input = 12,
    Artifact = 13,
    ArtifactGeneration = 14,
    Observation = 15,
    Metric = 16,
    Failure = 17,
    Decision = 18,
    Result = 19,
    ResultGeneration = 20,
    Worker = 21,
    WorkerBoot = 22,
    CoordinatorEpoch = 23,
    LedgerGeneration = 24,
    RecordSequence = 25,
    Record = 26,
    PolicyGeneration = 27,
};

std::string_view identity_domain_name(IdentityDomain domain) noexcept;
std::optional<IdentityDomain> parse_identity_domain(std::string_view text) noexcept;

// Zero is the invalid value in every identity domain. It is never issued by the
// runtime, and an identity that decodes to zero is rejected. Generation zero is
// likewise never valid authority: the first committed generation of an entity is
// generation one.
inline constexpr std::uint64_t kInvalidIdentityValue = 0;
inline constexpr std::uint32_t kInvalidGenerationValue = 0;
inline constexpr std::uint32_t kFirstGeneration = 1;

template <class Tag>
class StrongId {
public:
    using tag_type = Tag;
    using rep_type = std::uint64_t;

    static constexpr IdentityDomain domain = Tag::domain;
    static constexpr bool represents_generation = false;

    constexpr StrongId() noexcept = default;
    constexpr explicit StrongId(std::uint64_t value) noexcept : value_(value) {}

    [[nodiscard]] static constexpr StrongId from_value(std::uint64_t value) noexcept {
        return StrongId(value);
    }

    [[nodiscard]] constexpr std::uint64_t value() const noexcept { return value_; }
    [[nodiscard]] constexpr bool valid() const noexcept { return value_ != kInvalidIdentityValue; }

    friend constexpr bool operator==(StrongId lhs, StrongId rhs) noexcept {
        return lhs.value_ == rhs.value_;
    }
    friend constexpr std::strong_ordering operator<=>(StrongId lhs, StrongId rhs) noexcept {
        return lhs.value_ <=> rhs.value_;
    }

private:
    std::uint64_t value_ = kInvalidIdentityValue;
};

template <class Tag>
class Generation {
public:
    using tag_type = Tag;
    using rep_type = std::uint32_t;

    static constexpr IdentityDomain domain = Tag::domain;
    static constexpr bool represents_generation = true;

    constexpr Generation() noexcept = default;
    constexpr explicit Generation(std::uint32_t value) noexcept : value_(value) {}

    [[nodiscard]] static constexpr Generation from_value(std::uint32_t value) noexcept {
        return Generation(value);
    }
    [[nodiscard]] static constexpr Generation first() noexcept {
        return Generation(kFirstGeneration);
    }

    [[nodiscard]] constexpr std::uint32_t value() const noexcept { return value_; }
    [[nodiscard]] constexpr bool valid() const noexcept { return value_ != kInvalidGenerationValue; }

    // Next generation of the same entity. Returns an invalid generation on
    // overflow rather than wrapping into a generation that was already issued.
    [[nodiscard]] constexpr Generation next() const noexcept {
        if (value_ == 0xffffffffu) {
            return Generation{};
        }
        return Generation(value_ + 1u);
    }

    friend constexpr bool operator==(Generation lhs, Generation rhs) noexcept {
        return lhs.value_ == rhs.value_;
    }
    friend constexpr std::strong_ordering operator<=>(Generation lhs, Generation rhs) noexcept {
        return lhs.value_ <=> rhs.value_;
    }

private:
    std::uint32_t value_ = kInvalidGenerationValue;
};

struct ResearchSessionTag { static constexpr IdentityDomain domain = IdentityDomain::ResearchSession; };
struct HypothesisTag { static constexpr IdentityDomain domain = IdentityDomain::Hypothesis; };
struct HypothesisGenerationTag { static constexpr IdentityDomain domain = IdentityDomain::HypothesisGeneration; };
struct ExperimentTag { static constexpr IdentityDomain domain = IdentityDomain::Experiment; };
struct ExperimentGenerationTag { static constexpr IdentityDomain domain = IdentityDomain::ExperimentGeneration; };
struct BranchTag { static constexpr IdentityDomain domain = IdentityDomain::Branch; };
struct AttemptTag { static constexpr IdentityDomain domain = IdentityDomain::Attempt; };
struct AttemptGenerationTag { static constexpr IdentityDomain domain = IdentityDomain::AttemptGeneration; };
struct ModelCallTag { static constexpr IdentityDomain domain = IdentityDomain::ModelCall; };
struct ToolCallTag { static constexpr IdentityDomain domain = IdentityDomain::ToolCall; };
struct DatasetTag { static constexpr IdentityDomain domain = IdentityDomain::Dataset; };
struct InputTag { static constexpr IdentityDomain domain = IdentityDomain::Input; };
struct ArtifactTag { static constexpr IdentityDomain domain = IdentityDomain::Artifact; };
struct ArtifactGenerationTag { static constexpr IdentityDomain domain = IdentityDomain::ArtifactGeneration; };
struct ObservationTag { static constexpr IdentityDomain domain = IdentityDomain::Observation; };
struct MetricTag { static constexpr IdentityDomain domain = IdentityDomain::Metric; };
struct FailureTag { static constexpr IdentityDomain domain = IdentityDomain::Failure; };
struct DecisionTag { static constexpr IdentityDomain domain = IdentityDomain::Decision; };
struct ResultTag { static constexpr IdentityDomain domain = IdentityDomain::Result; };
struct ResultGenerationTag { static constexpr IdentityDomain domain = IdentityDomain::ResultGeneration; };
struct WorkerTag { static constexpr IdentityDomain domain = IdentityDomain::Worker; };
struct WorkerBootTag { static constexpr IdentityDomain domain = IdentityDomain::WorkerBoot; };
struct CoordinatorEpochTag { static constexpr IdentityDomain domain = IdentityDomain::CoordinatorEpoch; };
struct LedgerGenerationTag { static constexpr IdentityDomain domain = IdentityDomain::LedgerGeneration; };
struct RecordSequenceTag { static constexpr IdentityDomain domain = IdentityDomain::RecordSequence; };
struct RecordTag { static constexpr IdentityDomain domain = IdentityDomain::Record; };
struct PolicyGenerationTag { static constexpr IdentityDomain domain = IdentityDomain::PolicyGeneration; };

using ResearchSessionId = StrongId<ResearchSessionTag>;
using HypothesisId = StrongId<HypothesisTag>;
using HypothesisGeneration = Generation<HypothesisGenerationTag>;
using ExperimentId = StrongId<ExperimentTag>;
using ExperimentGeneration = Generation<ExperimentGenerationTag>;
using BranchId = StrongId<BranchTag>;
using AttemptId = StrongId<AttemptTag>;
using AttemptGeneration = Generation<AttemptGenerationTag>;
using ModelCallId = StrongId<ModelCallTag>;
using ToolCallId = StrongId<ToolCallTag>;
using DatasetId = StrongId<DatasetTag>;
using InputId = StrongId<InputTag>;
using ArtifactId = StrongId<ArtifactTag>;
using ArtifactGeneration = Generation<ArtifactGenerationTag>;
using ObservationId = StrongId<ObservationTag>;
using MetricId = StrongId<MetricTag>;
using FailureId = StrongId<FailureTag>;
using DecisionId = StrongId<DecisionTag>;
using ResultId = StrongId<ResultTag>;
using ResultGeneration = Generation<ResultGenerationTag>;
using WorkerId = StrongId<WorkerTag>;
using WorkerBootId = StrongId<WorkerBootTag>;
using CoordinatorEpoch = Generation<CoordinatorEpochTag>;
using LedgerGeneration = Generation<LedgerGenerationTag>;
using RecordSequence = Generation<RecordSequenceTag>;
using LedgerRecordId = StrongId<RecordTag>;
using PolicyGeneration = Generation<PolicyGenerationTag>;

// Authority envelope carried by every committed record: which ledger
// generation, which coordinator epoch, and which worker incarnation produced
// the record. Authority is not inferred from the payload.
struct AuthorityEnvelope {
    LedgerGeneration ledger{};
    CoordinatorEpoch epoch{};
    WorkerId worker{};
    WorkerBootId worker_boot{};

    friend bool operator==(const AuthorityEnvelope& lhs, const AuthorityEnvelope& rhs) noexcept {
        return lhs.ledger == rhs.ledger && lhs.epoch == rhs.epoch && lhs.worker == rhs.worker &&
               lhs.worker_boot == rhs.worker_boot;
    }
};

// Textual form of every identity, used by the CLI, by explanations and by the
// tests. The form is stable and includes the domain.
std::string to_string(ResearchSessionId value);
std::string to_string(HypothesisId value);
std::string to_string(HypothesisGeneration value);
std::string to_string(ExperimentId value);
std::string to_string(ExperimentGeneration value);
std::string to_string(BranchId value);
std::string to_string(AttemptId value);
std::string to_string(AttemptGeneration value);
std::string to_string(ModelCallId value);
std::string to_string(ToolCallId value);
std::string to_string(DatasetId value);
std::string to_string(InputId value);
std::string to_string(ArtifactId value);
std::string to_string(ArtifactGeneration value);
std::string to_string(ObservationId value);
std::string to_string(MetricId value);
std::string to_string(FailureId value);
std::string to_string(DecisionId value);
std::string to_string(ResultId value);
std::string to_string(ResultGeneration value);
std::string to_string(WorkerId value);
std::string to_string(WorkerBootId value);
std::string to_string(CoordinatorEpoch value);
std::string to_string(LedgerGeneration value);
std::string to_string(RecordSequence value);
std::string to_string(LedgerRecordId value);
std::string to_string(PolicyGeneration value);

// Parsing accepts exactly the form produced by to_string.
std::optional<ResearchSessionId> parse_research_session_id(std::string_view text);
std::optional<HypothesisId> parse_hypothesis_id(std::string_view text);
std::optional<ExperimentId> parse_experiment_id(std::string_view text);
std::optional<BranchId> parse_branch_id(std::string_view text);
std::optional<AttemptId> parse_attempt_id(std::string_view text);
std::optional<ArtifactId> parse_artifact_id(std::string_view text);
std::optional<ModelCallId> parse_model_call_id(std::string_view text);
std::optional<ToolCallId> parse_tool_call_id(std::string_view text);
std::optional<ResultId> parse_result_id(std::string_view text);
std::optional<FailureId> parse_failure_id(std::string_view text);
std::optional<DecisionId> parse_decision_id(std::string_view text);
std::optional<ObservationId> parse_observation_id(std::string_view text);

}  // namespace research_ledger

namespace std {

#define RESEARCH_LEDGER_HASH_SPECIALIZATION(TYPE)                    \
    template <>                                                      \
    struct hash<TYPE> {                                              \
        std::size_t operator()(const TYPE& value) const noexcept {   \
            return std::hash<std::uint64_t>{}(value.value());        \
        }                                                            \
    };

template <class Tag>
struct hash<::research_ledger::Generation<Tag>> {
    std::size_t operator()(const ::research_ledger::Generation<Tag>& value) const noexcept {
        return std::hash<std::uint32_t>{}(value.value());
    }
};

RESEARCH_LEDGER_HASH_SPECIALIZATION(::research_ledger::ResearchSessionId)
RESEARCH_LEDGER_HASH_SPECIALIZATION(::research_ledger::HypothesisId)
RESEARCH_LEDGER_HASH_SPECIALIZATION(::research_ledger::ExperimentId)
RESEARCH_LEDGER_HASH_SPECIALIZATION(::research_ledger::BranchId)
RESEARCH_LEDGER_HASH_SPECIALIZATION(::research_ledger::AttemptId)
RESEARCH_LEDGER_HASH_SPECIALIZATION(::research_ledger::ModelCallId)
RESEARCH_LEDGER_HASH_SPECIALIZATION(::research_ledger::ToolCallId)
RESEARCH_LEDGER_HASH_SPECIALIZATION(::research_ledger::DatasetId)
RESEARCH_LEDGER_HASH_SPECIALIZATION(::research_ledger::InputId)
RESEARCH_LEDGER_HASH_SPECIALIZATION(::research_ledger::ArtifactId)
RESEARCH_LEDGER_HASH_SPECIALIZATION(::research_ledger::ObservationId)
RESEARCH_LEDGER_HASH_SPECIALIZATION(::research_ledger::MetricId)
RESEARCH_LEDGER_HASH_SPECIALIZATION(::research_ledger::FailureId)
RESEARCH_LEDGER_HASH_SPECIALIZATION(::research_ledger::DecisionId)
RESEARCH_LEDGER_HASH_SPECIALIZATION(::research_ledger::ResultId)
RESEARCH_LEDGER_HASH_SPECIALIZATION(::research_ledger::WorkerId)
RESEARCH_LEDGER_HASH_SPECIALIZATION(::research_ledger::WorkerBootId)
RESEARCH_LEDGER_HASH_SPECIALIZATION(::research_ledger::LedgerRecordId)

#undef RESEARCH_LEDGER_HASH_SPECIALIZATION

}  // namespace std
