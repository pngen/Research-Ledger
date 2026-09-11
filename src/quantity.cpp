#include "research_ledger/quantity.hpp"

#include <cmath>
#include <cstdio>
#include <limits>
#include <utility>

namespace research_ledger {
namespace {

// Higher rank means weaker evidence. A total is never stronger than its
// weakest contributor.
int provenance_rank(Provenance provenance) noexcept {
    switch (provenance) {
        case Provenance::Measured:
            return 0;
        case Provenance::Reported:
            return 1;
        case Provenance::Derived:
            return 2;
        case Provenance::Reconstructed:
            return 3;
        case Provenance::Estimated:
            return 4;
        case Provenance::Synthetic:
            return 5;
        case Provenance::Unknown:
            return 6;
    }
    return 6;
}

Provenance weaker(Provenance lhs, Provenance rhs) noexcept {
    return provenance_rank(lhs) >= provenance_rank(rhs) ? lhs : rhs;
}

template <class MeasureType>
Result<MeasureType> add_measure(const MeasureType& lhs, const MeasureType& rhs) {
    if (!lhs.is_known() || !rhs.is_known()) {
        return MeasureType::unknown(weaker(lhs.provenance(), rhs.provenance()));
    }
    const std::uint64_t left = lhs.units();
    const std::uint64_t right = rhs.units();
    if (left > std::numeric_limits<std::uint64_t>::max() - right) {
        return Status(ErrorCode::AccountingOverflow, "accounting accumulation overflowed");
    }
    return MeasureType::known(left + right, weaker(Provenance::Derived, weaker(lhs.provenance(), rhs.provenance())));
}

}  // namespace

std::string_view provenance_name(Provenance provenance) noexcept {
    switch (provenance) {
        case Provenance::Unknown:
            return "UNKNOWN";
        case Provenance::Measured:
            return "MEASURED";
        case Provenance::Reported:
            return "REPORTED";
        case Provenance::Derived:
            return "DERIVED";
        case Provenance::Estimated:
            return "ESTIMATED";
        case Provenance::Synthetic:
            return "SYNTHETIC";
        case Provenance::Reconstructed:
            return "RECONSTRUCTED";
    }
    return "UNKNOWN";
}

std::optional<Provenance> parse_provenance(std::string_view text) noexcept {
    if (text == "MEASURED") return Provenance::Measured;
    if (text == "REPORTED") return Provenance::Reported;
    if (text == "DERIVED") return Provenance::Derived;
    if (text == "ESTIMATED") return Provenance::Estimated;
    if (text == "SYNTHETIC") return Provenance::Synthetic;
    if (text == "RECONSTRUCTED") return Provenance::Reconstructed;
    if (text == "UNKNOWN") return Provenance::Unknown;
    return std::nullopt;
}

MonetaryMeasure MonetaryMeasure::unknown(Provenance provenance) {
    MonetaryMeasure measure;
    measure.known = false;
    measure.provenance = provenance;
    return measure;
}

MonetaryMeasure MonetaryMeasure::known_amount(std::int64_t micro_units_in, std::string currency_in,
                                              Provenance provenance) {
    MonetaryMeasure measure;
    measure.known = true;
    measure.micro_units = micro_units_in;
    measure.currency = std::move(currency_in);
    measure.provenance = provenance;
    return measure;
}

bool AccountingVector::any_known() const noexcept {
    return model_input_tokens.is_known() || model_output_tokens.is_known() || model_calls.is_known() ||
           tool_calls.is_known() || accelerator_nanos.is_known() || cpu_nanos.is_known() ||
           wall_nanos.is_known() || storage_bytes.is_known() || transfer_bytes.is_known() ||
           energy_micro_joules.is_known() || attempts.is_known() || retries.is_known() ||
           failure_overhead_nanos.is_known() || monetary.known;
}

bool AccountingVector::all_known() const noexcept {
    return model_input_tokens.is_known() && model_output_tokens.is_known() && model_calls.is_known() &&
           tool_calls.is_known() && accelerator_nanos.is_known() && cpu_nanos.is_known() &&
           wall_nanos.is_known() && storage_bytes.is_known() && transfer_bytes.is_known() &&
           energy_micro_joules.is_known() && attempts.is_known() && retries.is_known() &&
           failure_overhead_nanos.is_known() && monetary.known;
}

bool AccountingVector::is_empty() const noexcept { return !any_known(); }

Result<AccountingVector> accumulate(const AccountingVector& total, const AccountingVector& addition) {
    AccountingVector result;

#define RESEARCH_LEDGER_ACCUMULATE(FIELD)                        \
    {                                                            \
        auto sum = add_measure(total.FIELD, addition.FIELD);     \
        if (!sum.ok()) {                                         \
            return sum.status();                                 \
        }                                                        \
        result.FIELD = sum.value();                              \
    }

    RESEARCH_LEDGER_ACCUMULATE(model_input_tokens)
    RESEARCH_LEDGER_ACCUMULATE(model_output_tokens)
    RESEARCH_LEDGER_ACCUMULATE(model_calls)
    RESEARCH_LEDGER_ACCUMULATE(tool_calls)
    RESEARCH_LEDGER_ACCUMULATE(accelerator_nanos)
    RESEARCH_LEDGER_ACCUMULATE(cpu_nanos)
    RESEARCH_LEDGER_ACCUMULATE(wall_nanos)
    RESEARCH_LEDGER_ACCUMULATE(storage_bytes)
    RESEARCH_LEDGER_ACCUMULATE(transfer_bytes)
    RESEARCH_LEDGER_ACCUMULATE(energy_micro_joules)
    RESEARCH_LEDGER_ACCUMULATE(attempts)
    RESEARCH_LEDGER_ACCUMULATE(retries)
    RESEARCH_LEDGER_ACCUMULATE(failure_overhead_nanos)

#undef RESEARCH_LEDGER_ACCUMULATE

    if (!total.monetary.known || !addition.monetary.known) {
        result.monetary = MonetaryMeasure::unknown(
            weaker(total.monetary.provenance, addition.monetary.provenance));
        if (result.monetary.currency.empty()) {
            result.monetary.currency = !total.monetary.currency.empty() ? total.monetary.currency
                                                                       : addition.monetary.currency;
        }
    } else {
        if (total.monetary.currency != addition.monetary.currency) {
            return Status(ErrorCode::Unsupported,
                          "cannot accumulate monetary measures in different currencies");
        }
        const std::int64_t left = total.monetary.micro_units;
        const std::int64_t right = addition.monetary.micro_units;
        if ((right > 0 && left > std::numeric_limits<std::int64_t>::max() - right) ||
            (right < 0 && left < std::numeric_limits<std::int64_t>::min() - right)) {
            return Status(ErrorCode::AccountingOverflow, "monetary accumulation overflowed");
        }
        result.monetary = MonetaryMeasure::known_amount(
            left + right, total.monetary.currency,
            weaker(Provenance::Derived,
                   weaker(total.monetary.provenance, addition.monetary.provenance)));
    }
    return result;
}

Result<AccountingAggregate> aggregate_accounting(const AccountingVector* contributions,
                                                 std::size_t count) {
    AccountingAggregate aggregate;
    if (count == 0) {
        aggregate.empty = true;
        aggregate.complete = false;
        return aggregate;
    }
    aggregate.empty = false;
    aggregate.total = contributions[0];
    aggregate.contributions = 1;
    aggregate.contributions_with_unknown_fields = contributions[0].all_known() ? 0 : 1;
    for (std::size_t index = 1; index < count; ++index) {
        auto sum = accumulate(aggregate.total, contributions[index]);
        if (!sum.ok()) {
            return sum.status();
        }
        aggregate.total = sum.value();
        aggregate.contributions += 1;
        if (!contributions[index].all_known()) {
            aggregate.contributions_with_unknown_fields += 1;
        }
    }
    aggregate.complete = aggregate.contributions_with_unknown_fields == 0;
    return aggregate;
}

std::string_view metric_value_kind_name(MetricValueKind kind) noexcept {
    switch (kind) {
        case MetricValueKind::Unknown:
            return "UNKNOWN";
        case MetricValueKind::SignedInteger:
            return "SIGNED_INTEGER";
        case MetricValueKind::UnsignedInteger:
            return "UNSIGNED_INTEGER";
        case MetricValueKind::Decimal:
            return "DECIMAL";
        case MetricValueKind::DurationNanos:
            return "DURATION_NANOS";
        case MetricValueKind::Bytes:
            return "BYTES";
        case MetricValueKind::Count:
            return "COUNT";
        case MetricValueKind::Rate:
            return "RATE";
        case MetricValueKind::Ratio:
            return "RATIO";
        case MetricValueKind::Boolean:
            return "BOOLEAN";
        case MetricValueKind::Category:
            return "CATEGORY";
        case MetricValueKind::Digest:
            return "DIGEST";
    }
    return "UNKNOWN";
}

std::optional<MetricValueKind> parse_metric_value_kind(std::string_view text) noexcept {
    for (std::uint8_t raw = 0; raw <= static_cast<std::uint8_t>(MetricValueKind::Digest); ++raw) {
        const auto kind = static_cast<MetricValueKind>(raw);
        if (metric_value_kind_name(kind) == text) {
            return kind;
        }
    }
    return std::nullopt;
}

std::string_view unit_kind_name(UnitKind unit) noexcept {
    switch (unit) {
        case UnitKind::None:
            return "NONE";
        case UnitKind::Count:
            return "COUNT";
        case UnitKind::Tokens:
            return "TOKENS";
        case UnitKind::Bytes:
            return "BYTES";
        case UnitKind::Nanoseconds:
            return "NANOSECONDS";
        case UnitKind::Seconds:
            return "SECONDS";
        case UnitKind::Ratio:
            return "RATIO";
        case UnitKind::Percent:
            return "PERCENT";
        case UnitKind::CurrencyMicroUnits:
            return "CURRENCY_MICRO_UNITS";
        case UnitKind::Watts:
            return "WATTS";
        case UnitKind::Joules:
            return "JOULES";
        case UnitKind::Celsius:
            return "CELSIUS";
        case UnitKind::Category:
            return "CATEGORY";
        case UnitKind::Custom:
            return "CUSTOM";
    }
    return "NONE";
}

std::optional<UnitKind> parse_unit_kind(std::string_view text) noexcept {
    for (std::uint8_t raw = 0; raw <= static_cast<std::uint8_t>(UnitKind::Custom); ++raw) {
        const auto unit = static_cast<UnitKind>(raw);
        if (unit_kind_name(unit) == text) {
            return unit;
        }
    }
    return std::nullopt;
}

bool unit_accepts_value(UnitKind unit, MetricValueKind kind) noexcept {
    if (unit == UnitKind::Custom || unit == UnitKind::None) {
        return true;
    }
    switch (kind) {
        case MetricValueKind::Unknown:
            return true;
        case MetricValueKind::DurationNanos:
            return unit == UnitKind::Nanoseconds;
        case MetricValueKind::Bytes:
            return unit == UnitKind::Bytes;
        case MetricValueKind::Count:
            return unit == UnitKind::Count || unit == UnitKind::Tokens;
        case MetricValueKind::Rate:
            return unit == UnitKind::Ratio || unit == UnitKind::Percent || unit == UnitKind::Count ||
                   unit == UnitKind::Watts;
        case MetricValueKind::Ratio:
            return unit == UnitKind::Ratio || unit == UnitKind::Percent;
        case MetricValueKind::Boolean:
            return unit == UnitKind::Count;  // a boolean is accepted as a 0/1 count
        case MetricValueKind::Category:
            return unit == UnitKind::Category;
        case MetricValueKind::Digest:
            return false;
        case MetricValueKind::SignedInteger:
        case MetricValueKind::UnsignedInteger:
            return unit == UnitKind::Count || unit == UnitKind::Tokens || unit == UnitKind::Bytes ||
                   unit == UnitKind::Nanoseconds || unit == UnitKind::Seconds ||
                   unit == UnitKind::CurrencyMicroUnits || unit == UnitKind::Watts ||
                   unit == UnitKind::Joules || unit == UnitKind::Celsius;
        case MetricValueKind::Decimal:
            return unit == UnitKind::Seconds || unit == UnitKind::Ratio || unit == UnitKind::Percent ||
                   unit == UnitKind::Watts || unit == UnitKind::Joules || unit == UnitKind::Celsius;
    }
    return false;
}

MetricValue MetricValue::unknown() noexcept { return MetricValue{}; }

Result<MetricValue> MetricValue::signed_integer(std::int64_t value) {
    MetricValue result;
    result.kind_ = MetricValueKind::SignedInteger;
    result.payload_ = value;
    return result;
}

Result<MetricValue> MetricValue::unsigned_integer(std::uint64_t value) {
    MetricValue result;
    result.kind_ = MetricValueKind::UnsignedInteger;
    result.payload_ = value;
    return result;
}

// Every fractional metric kind validates its input the same way: NaN and
// infinity are rejected, so a comparison or a digest never depends on a
// non-finite bit pattern. Each factory constructs its own value directly:
// routing one factory through another is how a stack overflow is written.
Result<MetricValue> MetricValue::decimal(double value) {
    if (!std::isfinite(value)) {
        return Status(ErrorCode::InvalidArgument, "decimal metric value is not finite");
    }
    MetricValue result;
    result.kind_ = MetricValueKind::Decimal;
    result.payload_ = value;
    return result;
}

Result<MetricValue> MetricValue::duration_nanos(std::uint64_t value) {
    MetricValue result;
    result.kind_ = MetricValueKind::DurationNanos;
    result.payload_ = value;
    return result;
}

Result<MetricValue> MetricValue::bytes(std::uint64_t value) {
    MetricValue result;
    result.kind_ = MetricValueKind::Bytes;
    result.payload_ = value;
    return result;
}

Result<MetricValue> MetricValue::count(std::uint64_t value) {
    MetricValue result;
    result.kind_ = MetricValueKind::Count;
    result.payload_ = value;
    return result;
}

Result<MetricValue> MetricValue::rate(double value) {
    if (!std::isfinite(value)) {
        return Status(ErrorCode::InvalidArgument, "rate metric value is not finite");
    }
    MetricValue result;
    result.kind_ = MetricValueKind::Rate;
    result.payload_ = value;
    return result;
}

Result<MetricValue> MetricValue::ratio(double value) {
    if (!std::isfinite(value)) {
        return Status(ErrorCode::InvalidArgument, "ratio metric value is not finite");
    }
    MetricValue result;
    result.kind_ = MetricValueKind::Ratio;
    result.payload_ = value;
    return result;
}

Result<MetricValue> MetricValue::boolean(bool value) {
    MetricValue result;
    result.kind_ = MetricValueKind::Boolean;
    result.payload_ = value;
    return result;
}

Result<MetricValue> MetricValue::category(std::string value) {
    MetricValue result;
    result.kind_ = MetricValueKind::Category;
    result.payload_ = std::move(value);
    return result;
}

Result<MetricValue> MetricValue::digest(Digest value) {
    MetricValue result;
    result.kind_ = MetricValueKind::Digest;
    result.payload_ = DigestValue{value};
    return result;
}

std::string MetricValue::to_text() const {
    switch (kind_) {
        case MetricValueKind::Unknown:
            return "UNKNOWN";
        case MetricValueKind::SignedInteger:
            return std::to_string(std::get<std::int64_t>(payload_));
        case MetricValueKind::UnsignedInteger:
        case MetricValueKind::DurationNanos:
        case MetricValueKind::Bytes:
        case MetricValueKind::Count:
            return std::to_string(std::get<std::uint64_t>(payload_));
        case MetricValueKind::Decimal:
        case MetricValueKind::Rate:
        case MetricValueKind::Ratio: {
            char buffer[64] = {};
            const double value = std::get<double>(payload_);
            std::snprintf(buffer, sizeof(buffer), "%.17g", value);
            return std::string(buffer);
        }
        case MetricValueKind::Boolean:
            return std::get<bool>(payload_) ? "true" : "false";
        case MetricValueKind::Category:
            return std::get<std::string>(payload_);
        case MetricValueKind::Digest:
            return to_hex(std::get<DigestValue>(payload_).digest);
    }
    return "UNKNOWN";
}

bool operator==(const MetricValue& lhs, const MetricValue& rhs) noexcept {
    if (lhs.kind_ != rhs.kind_) {
        return false;
    }
    return lhs.payload_ == rhs.payload_;
}

}  // namespace research_ledger
