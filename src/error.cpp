#include "research_ledger/error.hpp"

#include <array>
#include <utility>

namespace research_ledger {
namespace {

using Entry = std::pair<ErrorCode, std::string_view>;

constexpr std::array<Entry, 32> kErrorNames{{
    {ErrorCode::Ok, "OK"},
    {ErrorCode::InvalidIdentity, "INVALID_IDENTITY"},
    {ErrorCode::DuplicateRecord, "DUPLICATE_RECORD"},
    {ErrorCode::DuplicateCompletion, "DUPLICATE_COMPLETION"},
    {ErrorCode::StaleEpoch, "STALE_EPOCH"},
    {ErrorCode::StaleWorker, "STALE_WORKER"},
    {ErrorCode::StaleGeneration, "STALE_GENERATION"},
    {ErrorCode::StaleAttempt, "STALE_ATTEMPT"},
    {ErrorCode::Cancelled, "CANCELLED"},
    {ErrorCode::AlreadyTerminal, "ALREADY_TERMINAL"},
    {ErrorCode::BrokenLineage, "BROKEN_LINEAGE"},
    {ErrorCode::LineageCycle, "LINEAGE_CYCLE"},
    {ErrorCode::CrossSessionReference, "CROSS_SESSION_REFERENCE"},
    {ErrorCode::MissingDependency, "MISSING_DEPENDENCY"},
    {ErrorCode::InvalidTransition, "INVALID_TRANSITION"},
    {ErrorCode::AccountingOverflow, "ACCOUNTING_OVERFLOW"},
    {ErrorCode::PersistenceCorrupt, "PERSISTENCE_CORRUPT"},
    {ErrorCode::PersistenceTruncated, "PERSISTENCE_TRUNCATED"},
    {ErrorCode::PersistenceUnsupportedVersion, "PERSISTENCE_UNSUPPORTED_VERSION"},
    {ErrorCode::ProtocolError, "PROTOCOL_ERROR"},
    {ErrorCode::PayloadTooLarge, "PAYLOAD_TOO_LARGE"},
    {ErrorCode::IntegrityFailure, "INTEGRITY_FAILURE"},
    {ErrorCode::NotFound, "NOT_FOUND"},
    {ErrorCode::Unsupported, "UNSUPPORTED"},
    {ErrorCode::InternalError, "INTERNAL_ERROR"},
    {ErrorCode::InvalidArgument, "INVALID_ARGUMENT"},
    {ErrorCode::LimitExceeded, "LIMIT_EXCEEDED"},
    {ErrorCode::IoFailure, "IO_FAILURE"},
    {ErrorCode::ShuttingDown, "SHUTTING_DOWN"},
    {ErrorCode::Unauthorized, "UNAUTHORIZED"},
    {ErrorCode::SessionClosed, "SESSION_CLOSED"},
    {ErrorCode::NotReady, "NOT_READY"},
}};

}  // namespace

std::string_view error_code_name(ErrorCode code) noexcept {
    for (const Entry& entry : kErrorNames) {
        if (entry.first == code) {
            return entry.second;
        }
    }
    if (code == ErrorCode::Busy) {
        return "BUSY";
    }
    return "UNKNOWN_ERROR";
}

std::optional<ErrorCode> parse_error_code(std::string_view text) noexcept {
    for (const Entry& entry : kErrorNames) {
        if (entry.second == text) {
            return entry.first;
        }
    }
    if (text == "BUSY") {
        return ErrorCode::Busy;
    }
    return std::nullopt;
}

std::string_view commit_state_name(CommitState state) noexcept {
    switch (state) {
        case CommitState::Invalid:
            return "INVALID";
        case CommitState::Submitted:
            return "SUBMITTED";
        case CommitState::Validated:
            return "VALIDATED";
        case CommitState::Committed:
            return "COMMITTED";
        case CommitState::Rejected:
            return "REJECTED";
        case CommitState::Duplicate:
            return "DUPLICATE";
    }
    return "INVALID";
}

std::string Status::to_string() const {
    std::string text(error_code_name(code));
    if (!message.empty()) {
        text += ": ";
        text += message;
    }
    return text;
}

Status ok_status() { return Status{}; }

Status error_status(ErrorCode code, std::string message) { return Status(code, std::move(message)); }

}  // namespace research_ledger
