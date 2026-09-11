#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "research_ledger/digest.hpp"
#include "research_ledger/error.hpp"
#include "research_ledger/identity.hpp"
#include "research_ledger/model.hpp"
#include "research_ledger/record.hpp"

namespace research_ledger {

// A reconstructed supporting-evidence closure for a result. Everything a
// reader needs to establish what produced the result, in deterministic order.
struct EvidenceBundle {
    ResultView result{};

    std::vector<EvidenceRef> cited_evidence{};
    std::vector<HypothesisView> hypotheses{};
    std::vector<ExperimentView> experiments{};
    std::vector<AttemptView> attempts{};
    std::vector<ModelCallView> model_calls{};
    std::vector<ToolCallView> tool_calls{};
    std::vector<ArtifactView> artifacts{};
    std::vector<ObservationView> observations{};
    std::vector<FailureView> failures{};
    std::vector<DecisionView> decisions{};

    RecordSequence highest_sequence{};
    std::uint64_t nodes_visited = 0;
    bool complete = false;      // every direct evidence reference resolved
    bool reconstructable = false;  // an acceptance decision exists for an accepted result
    bool truncated = false;     // a bound was reached before the closure completed
    Digest lineage_digest{};

    [[nodiscard]] bool accepted() const noexcept {
        return result.status == ResultStatus::Accepted;
    }
};

struct ExplanationLine {
    std::string text{};
};

// A deterministic, human-readable account of why a result holds the state it
// holds. The text is derived from committed records only.
struct Explanation {
    std::vector<ExplanationLine> lines{};
    bool truncated = false;

    [[nodiscard]] std::string to_text() const;
};

struct IntegrityIssue {
    ErrorCode code = ErrorCode::Ok;
    RecordSequence sequence{};
    SubjectId subject{};
    std::string detail{};
};

struct IntegrityReport {
    bool ok = true;
    RecordSequence records{};
    Digest chain_digest{};
    std::uint64_t checked_records = 0;
    std::vector<IntegrityIssue> issues{};

    [[nodiscard]] std::string to_text() const;
};

struct LedgerStats {
    RecordSequence last_sequence{};
    Digest chain_digest{};
    LedgerGeneration generation{};
    CoordinatorEpoch epoch{};
    std::uint64_t sessions = 0;
    std::uint64_t hypotheses = 0;
    std::uint64_t experiments = 0;
    std::uint64_t branches = 0;
    std::uint64_t attempts = 0;
    std::uint64_t model_calls = 0;
    std::uint64_t tool_calls = 0;
    std::uint64_t artifacts = 0;
    std::uint64_t observations = 0;
    std::uint64_t failures = 0;
    std::uint64_t decisions = 0;
    std::uint64_t results = 0;
    std::uint64_t accounting_records = 0;
    std::uint64_t live_workers = 0;
    // Committed record count per record type, indexed by RecordType.
    std::vector<std::uint64_t> record_counts{};

    [[nodiscard]] std::uint64_t count_of(RecordType type) const noexcept;
};

}  // namespace research_ledger
