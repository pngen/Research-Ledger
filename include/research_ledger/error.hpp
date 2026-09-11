#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace research_ledger {

// Typed outcome of every fallible operation. External behaviour never depends
// on free-form message text: callers switch on the code.
enum class ErrorCode : std::uint16_t {
    Ok = 0,
    InvalidIdentity,
    DuplicateRecord,
    DuplicateCompletion,
    StaleEpoch,
    StaleWorker,
    StaleGeneration,
    StaleAttempt,
    Cancelled,
    AlreadyTerminal,
    BrokenLineage,
    LineageCycle,
    CrossSessionReference,
    MissingDependency,
    InvalidTransition,
    AccountingOverflow,
    PersistenceCorrupt,
    PersistenceTruncated,
    PersistenceUnsupportedVersion,
    ProtocolError,
    PayloadTooLarge,
    IntegrityFailure,
    NotFound,
    Unsupported,
    InternalError,
    InvalidArgument,
    LimitExceeded,
    IoFailure,
    ShuttingDown,
    Unauthorized,
    SessionClosed,
    NotReady,
    Busy,
};

std::string_view error_code_name(ErrorCode code) noexcept;
std::optional<ErrorCode> parse_error_code(std::string_view text) noexcept;

// Commit semantics are explicit: a record is submitted, then validated, then
// committed, or it is rejected. Only committed records are historical truth.
enum class CommitState : std::uint8_t {
    Invalid = 0,
    Submitted = 1,
    Validated = 2,
    Committed = 3,
    Rejected = 4,
    Duplicate = 5,
};

std::string_view commit_state_name(CommitState state) noexcept;

struct Status {
    ErrorCode code = ErrorCode::Ok;
    std::string message{};

    Status() = default;
    Status(ErrorCode c, std::string msg) : code(c), message(std::move(msg)) {}

    [[nodiscard]] bool ok() const noexcept { return code == ErrorCode::Ok; }
    [[nodiscard]] std::string to_string() const;
};

Status ok_status();
Status error_status(ErrorCode code, std::string message);

template <class T>
class Result {
public:
    Result(T value) : value_(std::move(value)) {}
    Result(Status status) : status_(std::move(status)) {}

    [[nodiscard]] bool ok() const noexcept { return status_.ok(); }
    [[nodiscard]] explicit operator bool() const noexcept { return ok(); }
    [[nodiscard]] const Status& status() const noexcept { return status_; }
    [[nodiscard]] ErrorCode code() const noexcept { return status_.code; }
    [[nodiscard]] const std::string& message() const noexcept { return status_.message; }

    [[nodiscard]] const T& value() const noexcept { return *value_; }
    [[nodiscard]] T& value() noexcept { return *value_; }
    [[nodiscard]] T take() noexcept { return std::move(*value_); }

private:
    Status status_{};
    std::optional<T> value_{};
};

template <class T>
Result<T> make_ok(T value) {
    return Result<T>(std::move(value));
}

template <class T>
Result<T> make_error(ErrorCode code, std::string message) {
    return Result<T>(Status(code, std::move(message)));
}

}  // namespace research_ledger
