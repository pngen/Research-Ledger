#include "research_ledger/record.hpp"

namespace research_ledger {

std::string_view record_type_name(RecordType type) noexcept {
    switch (type) {
        case RecordType::SessionOpened:
            return "SESSION_OPENED";
        case RecordType::SessionClosed:
            return "SESSION_CLOSED";
        case RecordType::SessionAnnotation:
            return "SESSION_ANNOTATION";
        case RecordType::HypothesisDeclared:
            return "HYPOTHESIS_DECLARED";
        case RecordType::HypothesisStatusChanged:
            return "HYPOTHESIS_STATUS_CHANGED";
        case RecordType::ExperimentDeclared:
            return "EXPERIMENT_DECLARED";
        case RecordType::BranchDeclared:
            return "BRANCH_DECLARED";
        case RecordType::AttemptStarted:
            return "ATTEMPT_STARTED";
        case RecordType::AttemptCompleted:
            return "ATTEMPT_COMPLETED";
        case RecordType::AttemptFailed:
            return "ATTEMPT_FAILED";
        case RecordType::AttemptCancelled:
            return "ATTEMPT_CANCELLED";
        case RecordType::ModelCallRecorded:
            return "MODEL_CALL_RECORDED";
        case RecordType::ToolCallRecorded:
            return "TOOL_CALL_RECORDED";
        case RecordType::ArtifactReferenced:
            return "ARTIFACT_REFERENCED";
        case RecordType::ArtifactInvalidated:
            return "ARTIFACT_INVALIDATED";
        case RecordType::MetricDeclared:
            return "METRIC_DECLARED";
        case RecordType::ObservationRecorded:
            return "OBSERVATION_RECORDED";
        case RecordType::FailureRecorded:
            return "FAILURE_RECORDED";
        case RecordType::DecisionRecorded:
            return "DECISION_RECORDED";
        case RecordType::ResultDeclared:
            return "RESULT_DECLARED";
        case RecordType::ResultStatusChanged:
            return "RESULT_STATUS_CHANGED";
        case RecordType::AccountingRecorded:
            return "ACCOUNTING_RECORDED";
    }
    return "UNKNOWN_RECORD";
}

std::optional<RecordType> parse_record_type(std::string_view text) noexcept {
    for (std::uint16_t raw = 1; raw <= kRecordTypeCount; ++raw) {
        const auto type = static_cast<RecordType>(raw);
        if (record_type_name(type) == text) {
            return type;
        }
    }
    return std::nullopt;
}

RecordType record_type_of(const RecordBody& body) noexcept {
    return static_cast<RecordType>(body.index() + 1);
}

}  // namespace research_ledger
