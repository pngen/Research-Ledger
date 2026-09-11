#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>

#include "research_ledger/digest.hpp"
#include "research_ledger/error.hpp"
#include "research_ledger/provenance.hpp"

namespace research_ledger {

// A measure is either a known value with provenance or explicitly unknown.
// Unknown never decays into zero: an aggregate is known only when every
// contributor is known and at least one contributor exists.
template <class Tag>
class Measure {
public:
    using tag_type = Tag;

    constexpr Measure() noexcept = default;

    [[nodiscard]] static constexpr Measure known(std::uint64_t units, Provenance provenance) noexcept {
        Measure measure;
        measure.known_ = true;
        measure.units_ = units;
        measure.provenance_ = provenance;
        return measure;
    }

    [[nodiscard]] static constexpr Measure unknown(Provenance provenance = Provenance::Unknown) noexcept {
        Measure measure;
        measure.provenance_ = provenance;
        return measure;
    }

    [[nodiscard]] constexpr bool is_known() const noexcept { return known_; }
    [[nodiscard]] constexpr std::uint64_t units() const noexcept { return units_; }
    [[nodiscard]] constexpr Provenance provenance() const noexcept { return provenance_; }
    [[nodiscard]] constexpr bool is_zero() const noexcept { return known_ && units_ == 0; }

    friend constexpr bool operator==(const Measure& lhs, const Measure& rhs) noexcept {
        return lhs.known_ == rhs.known_ && lhs.units_ == rhs.units_ &&
               lhs.provenance_ == rhs.provenance_;
    }

private:
    bool known_ = false;
    std::uint64_t units_ = 0;
    Provenance provenance_ = Provenance::Unknown;
};

// Exact monetary measure in micro-units of a stated currency. Money is never
// carried in binary floating point, because authoritative totals must be exact.
struct MonetaryMeasure {
    bool known = false;
    std::int64_t micro_units = 0;
    std::string currency{};
    Provenance provenance = Provenance::Unknown;

    [[nodiscard]] static MonetaryMeasure unknown(Provenance provenance = Provenance::Unknown);
    [[nodiscard]] static MonetaryMeasure known_amount(std::int64_t micro_units_in, std::string currency_in,
                                                      Provenance provenance);

    friend bool operator==(const MonetaryMeasure& lhs, const MonetaryMeasure& rhs) noexcept {
        return lhs.known == rhs.known && lhs.micro_units == rhs.micro_units &&
               lhs.currency == rhs.currency && lhs.provenance == rhs.provenance;
    }
};

struct InputTokenTag { static constexpr std::string_view name = "model_input_tokens"; };
struct OutputTokenTag { static constexpr std::string_view name = "model_output_tokens"; };
struct ModelCallMeasureTag { static constexpr std::string_view name = "model_calls"; };
struct ToolCallMeasureTag { static constexpr std::string_view name = "tool_calls"; };
struct AcceleratorNanosTag { static constexpr std::string_view name = "accelerator_nanos"; };
struct CpuNanosTag { static constexpr std::string_view name = "cpu_nanos"; };
struct WallNanosTag { static constexpr std::string_view name = "wall_nanos"; };
struct StorageBytesTag { static constexpr std::string_view name = "storage_bytes"; };
struct TransferBytesTag { static constexpr std::string_view name = "transfer_bytes"; };
struct EnergyMicroJoulesTag { static constexpr std::string_view name = "energy_micro_joules"; };
struct AttemptMeasureTag { static constexpr std::string_view name = "attempts"; };
struct RetryTag { static constexpr std::string_view name = "retries"; };
struct FailureOverheadNanosTag { static constexpr std::string_view name = "failure_overhead_nanos"; };

using InputTokens = Measure<InputTokenTag>;
using OutputTokens = Measure<OutputTokenTag>;
using ModelCalls = Measure<ModelCallMeasureTag>;
using ToolCalls = Measure<ToolCallMeasureTag>;
using AcceleratorNanos = Measure<AcceleratorNanosTag>;
using CpuNanos = Measure<CpuNanosTag>;
using WallNanos = Measure<WallNanosTag>;
using StorageBytes = Measure<StorageBytesTag>;
using TransferBytes = Measure<TransferBytesTag>;
using EnergyMicroJoules = Measure<EnergyMicroJoulesTag>;
using AttemptCount = Measure<AttemptMeasureTag>;
using RetryCount = Measure<RetryTag>;
using FailureOverheadNanos = Measure<FailureOverheadNanosTag>;

// Research consumption as recorded by the ledger. Fields are named and typed:
// a token count cannot be passed where a byte count is expected.
struct AccountingVector {
    InputTokens model_input_tokens{};
    OutputTokens model_output_tokens{};
    ModelCalls model_calls{};
    ToolCalls tool_calls{};
    AcceleratorNanos accelerator_nanos{};
    CpuNanos cpu_nanos{};
    WallNanos wall_nanos{};
    StorageBytes storage_bytes{};
    TransferBytes transfer_bytes{};
    EnergyMicroJoules energy_micro_joules{};
    AttemptCount attempts{};
    RetryCount retries{};
    FailureOverheadNanos failure_overhead_nanos{};
    MonetaryMeasure monetary{};

    [[nodiscard]] bool any_known() const noexcept;
    [[nodiscard]] bool all_known() const noexcept;
    [[nodiscard]] bool is_empty() const noexcept;
};

// Checked elementwise accumulation. Every field is added with overflow
// detection; a detected overflow is an ACCOUNTING_OVERFLOW failure and leaves
// the accumulator untouched.
Result<AccountingVector> accumulate(const AccountingVector& total, const AccountingVector& addition);

// Sum of a sequence of contributions. Contributions marked unavailable are
// counted, and the corresponding total stays unknown instead of becoming zero.
struct AccountingAggregate {
    AccountingVector total{};
    std::uint64_t contributions = 0;
    std::uint64_t contributions_with_unknown_fields = 0;
    bool complete = false;  // true only when every contribution is fully known
    bool empty = true;      // no contributions were found at all
};

Result<AccountingAggregate> aggregate_accounting(const AccountingVector* contributions,
                                                 std::size_t count);

// --- typed observation values ----------------------------------------------
enum class MetricValueKind : std::uint8_t {
    Unknown = 0,
    SignedInteger = 1,
    UnsignedInteger = 2,
    Decimal = 3,
    DurationNanos = 4,
    Bytes = 5,
    Count = 6,
    Rate = 7,
    Ratio = 8,
    Boolean = 9,
    Category = 10,
    Digest = 11,
};

std::string_view metric_value_kind_name(MetricValueKind kind) noexcept;
std::optional<MetricValueKind> parse_metric_value_kind(std::string_view text) noexcept;

enum class UnitKind : std::uint8_t {
    None = 0,
    Count = 1,
    Tokens = 2,
    Bytes = 3,
    Nanoseconds = 4,
    Seconds = 5,
    Ratio = 6,
    Percent = 7,
    CurrencyMicroUnits = 8,
    Watts = 9,
    Joules = 10,
    Celsius = 11,
    Category = 12,
    Custom = 13,
};

std::string_view unit_kind_name(UnitKind unit) noexcept;
std::optional<UnitKind> parse_unit_kind(std::string_view text) noexcept;

// A declared unit accepts only compatible value kinds. An observation that
// carries bytes under a nanosecond unit is rejected rather than stored.
bool unit_accepts_value(UnitKind unit, MetricValueKind kind) noexcept;

struct DigestValue {
    Digest digest{};
    friend bool operator==(const DigestValue& lhs, const DigestValue& rhs) noexcept {
        return lhs.digest == rhs.digest;
    }
};

// A typed metric value. Exactly one alternative is active, selected by kind().
class MetricValue {
public:
    MetricValue() noexcept = default;

    [[nodiscard]] static MetricValue unknown() noexcept;
    [[nodiscard]] static Result<MetricValue> signed_integer(std::int64_t value);
    [[nodiscard]] static Result<MetricValue> unsigned_integer(std::uint64_t value);
    [[nodiscard]] static Result<MetricValue> decimal(double value);
    [[nodiscard]] static Result<MetricValue> duration_nanos(std::uint64_t value);
    [[nodiscard]] static Result<MetricValue> bytes(std::uint64_t value);
    [[nodiscard]] static Result<MetricValue> count(std::uint64_t value);
    [[nodiscard]] static Result<MetricValue> rate(double value);
    [[nodiscard]] static Result<MetricValue> ratio(double value);
    [[nodiscard]] static Result<MetricValue> boolean(bool value);
    [[nodiscard]] static Result<MetricValue> category(std::string value);
    [[nodiscard]] static Result<MetricValue> digest(Digest value);

    [[nodiscard]] MetricValueKind kind() const noexcept { return kind_; }
    [[nodiscard]] bool is_unknown() const noexcept { return kind_ == MetricValueKind::Unknown; }

    [[nodiscard]] const std::int64_t& as_signed_integer() const { return std::get<std::int64_t>(payload_); }
    [[nodiscard]] const std::uint64_t& as_unsigned_integer() const { return std::get<std::uint64_t>(payload_); }
    [[nodiscard]] const double& as_decimal() const { return std::get<double>(payload_); }
    [[nodiscard]] const bool& as_boolean() const { return std::get<bool>(payload_); }
    [[nodiscard]] const std::string& as_category() const { return std::get<std::string>(payload_); }
    [[nodiscard]] const Digest& as_digest() const { return std::get<DigestValue>(payload_).digest; }

    // Canonical text used by explanations, the CLI and the digest of the
    // reconstructed logical state.
    [[nodiscard]] std::string to_text() const;

    friend bool operator==(const MetricValue& lhs, const MetricValue& rhs) noexcept;

private:
    using Payload = std::variant<std::monostate, std::int64_t, std::uint64_t, double, bool, std::string,
                                 DigestValue>;
    MetricValueKind kind_ = MetricValueKind::Unknown;
    Payload payload_{};
};

}  // namespace research_ledger