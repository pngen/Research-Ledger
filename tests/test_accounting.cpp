#include "test_framework.hpp"
#include "test_support.hpp"

#include "research_ledger/ledger.hpp"
#include "research_ledger/quantity.hpp"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <limits>
#include <memory>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace research_ledger {
namespace {

using namespace research_ledger::test;

// Every query is read through this helper so that a failed query is reported
// through the framework instead of being dereferenced.
template <class T>
inline T expect(Result<T> result, const char* what) {
    RL_CHECK_MESSAGE(result.ok(), std::string(what) + ": " +
                                      std::string(error_code_name(result.code())) + " (" +
                                      result.message() + ")");
    return result.take();
}

// A Status is not a Result: it is reported through the same controlled path.
inline void require_ok(const Status& status, const char* what) {
    RL_CHECK_MESSAGE(status.ok(), std::string(what) + ": " +
                                      std::string(error_code_name(status.code)) + " (" +
                                      status.message + ")");
}

// Scratch files live in the agent's own temporary directory, never in the
// repository working tree.
inline std::string temp_path(const char* name) {
    std::error_code error;
    std::filesystem::path root = std::filesystem::temp_directory_path(error);
    if (error) {
        root = std::filesystem::path(".");
    }
    root /= "rlagent";
    std::filesystem::create_directories(root, error);
    root /= name;
    return root.string();
}

// A contribution in which every named quantity is stated, so that complete
// aggregates can be distinguished from aggregates with unknown fields.
inline AccountingVector known_vector(std::uint64_t input_tokens,
                                     std::int64_t monetary_micro_units,
                                     Provenance provenance = Provenance::Measured) {
    AccountingVector accounting;
    accounting.model_input_tokens = InputTokens::known(input_tokens, provenance);
    accounting.model_output_tokens = OutputTokens::known(input_tokens / 2u, provenance);
    accounting.model_calls = ModelCalls::known(1, provenance);
    accounting.tool_calls = ToolCalls::known(1, provenance);
    accounting.accelerator_nanos = AcceleratorNanos::known(100, provenance);
    accounting.cpu_nanos = CpuNanos::known(200, provenance);
    accounting.wall_nanos = WallNanos::known(300, provenance);
    accounting.storage_bytes = StorageBytes::known(400, provenance);
    accounting.transfer_bytes = TransferBytes::known(500, provenance);
    accounting.energy_micro_joules = EnergyMicroJoules::known(600, provenance);
    accounting.attempts = AttemptCount::known(1, provenance);
    accounting.retries = RetryCount::known(0, provenance);
    accounting.failure_overhead_nanos = FailureOverheadNanos::known(700, provenance);
    accounting.monetary = MonetaryMeasure::known_amount(monetary_micro_units, "USD", provenance);
    return accounting;
}

// session 1, hypothesis 1, branch 1, experiment 1, two running attempts, one
// artifact and two candidate results that share the same experiment.
inline Result<std::shared_ptr<Ledger>> ledger_with_two_attempts(const LedgerConfig& config) {
    auto created = Ledger::create(config);
    if (!created.ok()) {
        return created.status();
    }
    std::shared_ptr<Ledger> ledger = created.take();
    const std::vector<RecordDraft> batch{
        session_opened(1),
        hypothesis_declared(1, 1, "accounting is exact"),
        branch_declared(1, 1, BranchKind::Root),
        experiment_declared(1, 1, 1, 1),
        attempt_started(1, 1, 1),
        attempt_started(2, 1, 1),
        artifact_referenced(1, 1, SubjectId::of(AttemptId::from_value(1)), ArtifactRole::Dataset,
                            "dataset"),
        result_declared(1, 1, 1, 1, {ArtifactId::from_value(1)}),
        result_declared(2, 1, 1, 1, {ArtifactId::from_value(1)})};
    auto appended = ledger->append_batch(batch);
    if (!appended.ok()) {
        return appended.status();
    }
    return ledger;
}

inline SubjectId attempt_scope(std::uint64_t attempt) {
    return SubjectId::of(AttemptId::from_value(attempt));
}

// --- typed measures ---------------------------------------------------------

RL_TEST(accounting_measures_distinguish_unknown_from_zero) {
    const AccountingVector empty;
    RL_CHECK(empty.is_empty());
    RL_CHECK(!empty.any_known());
    RL_CHECK(!empty.all_known());

    AccountingVector partial;
    partial.model_input_tokens = InputTokens::known(0, Provenance::Measured);
    RL_CHECK(!partial.is_empty());
    RL_CHECK(partial.any_known());
    RL_CHECK(!partial.all_known());
    RL_CHECK(partial.model_input_tokens.is_zero());
    RL_CHECK(partial.model_input_tokens.is_known());
    // An unknown quantity is not zero: it is a statement that the ledger does
    // not know the value.
    RL_CHECK(!partial.model_output_tokens.is_known());
    RL_CHECK(!partial.model_output_tokens.is_zero());
    RL_CHECK(!partial.monetary.known);

    const AccountingVector full = known_vector(10, 20);
    RL_CHECK(full.all_known());
    RL_CHECK(full.any_known());
    RL_CHECK(!full.is_empty());

    // Known plus unknown stays unknown instead of decaying to zero.
    auto sum = accumulate(partial, full);
    RL_CHECK_OK(sum);
    RL_CHECK(!sum.value().model_output_tokens.is_known());
    RL_CHECK(!sum.value().model_output_tokens.is_zero());
    RL_CHECK_EQ(sum.value().model_output_tokens.units(), 0u);
    RL_CHECK(sum.value().model_input_tokens.is_known());
    RL_CHECK_EQ(sum.value().model_input_tokens.units(), 10u);
    RL_CHECK(!sum.value().all_known());

    // An aggregate with no contributions at all is empty, not zero.
    auto aggregate = aggregate_accounting(nullptr, 0);
    RL_CHECK_OK(aggregate);
    RL_CHECK(aggregate.value().empty);
    RL_CHECK(!aggregate.value().complete);
    RL_CHECK_EQ(aggregate.value().contributions, 0u);
    RL_CHECK_EQ(aggregate.value().contributions_with_unknown_fields, 0u);
    RL_CHECK(aggregate.value().total.is_empty());
    RL_CHECK(!aggregate.value().total.model_input_tokens.is_known());
    RL_CHECK(!aggregate.value().total.model_input_tokens.is_zero());
}

// --- checked accumulation ---------------------------------------------------

RL_TEST(accounting_accumulation_is_checked) {
    const std::uint64_t huge = std::numeric_limits<std::uint64_t>::max() - 5u;

    AccountingVector total;
    total.model_input_tokens = InputTokens::known(huge, Provenance::Measured);
    AccountingVector addition;
    addition.model_input_tokens = InputTokens::known(6, Provenance::Measured);

    auto overflow = accumulate(total, addition);
    RL_CHECK_CODE(overflow, ErrorCode::AccountingOverflow);
    // The accumulator is untouched by a refused accumulation.
    RL_CHECK(total.model_input_tokens.is_known());
    RL_CHECK_EQ(total.model_input_tokens.units(), huge);
    AccountingVector two;
    two.model_input_tokens = InputTokens::known(5, Provenance::Measured);
    auto exact = accumulate(total, two);
    RL_CHECK_OK(exact);
    RL_CHECK_EQ(exact.value().model_input_tokens.units(),
                std::numeric_limits<std::uint64_t>::max());

    // Monetary accumulation is checked in both directions.
    AccountingVector rich;
    rich.monetary = MonetaryMeasure::known_amount(std::numeric_limits<std::int64_t>::max(), "USD",
                                                  Provenance::Measured);
    AccountingVector one_micro;
    one_micro.monetary = MonetaryMeasure::known_amount(1, "USD", Provenance::Measured);
    RL_CHECK_CODE(accumulate(rich, one_micro), ErrorCode::AccountingOverflow);
    RL_CHECK_EQ(rich.monetary.micro_units, std::numeric_limits<std::int64_t>::max());

    AccountingVector debt;
    debt.monetary = MonetaryMeasure::known_amount(std::numeric_limits<std::int64_t>::min(), "USD",
                                                  Provenance::Measured);
    AccountingVector minus_one;
    minus_one.monetary = MonetaryMeasure::known_amount(-1, "USD", Provenance::Measured);
    RL_CHECK_CODE(accumulate(debt, minus_one), ErrorCode::AccountingOverflow);
    RL_CHECK_EQ(debt.monetary.micro_units, std::numeric_limits<std::int64_t>::min());

    // Money in different currencies is never added silently.
    AccountingVector euro;
    euro.monetary = MonetaryMeasure::known_amount(500, "EUR", Provenance::Measured);
    RL_CHECK_CODE(accumulate(rich, euro), ErrorCode::Unsupported);
    RL_CHECK_EQ(rich.monetary.currency, std::string("USD"));

    // An unavailable monetary contribution makes the total unknown without
    // turning it into zero, and the currency is still stated.
    AccountingVector unknown_money;
    unknown_money.monetary = MonetaryMeasure::unknown(Provenance::Measured);
    auto mixed = accumulate(rich, unknown_money);
    RL_CHECK_OK(mixed);
    RL_CHECK(!mixed.value().monetary.known);
    RL_CHECK_EQ(mixed.value().monetary.currency, std::string("USD"));
}

RL_TEST(accounting_monetary_totals_are_exact_micro_units) {
    AccountingVector total;
    total.monetary = MonetaryMeasure::known_amount(0, "USD", Provenance::Measured);
    std::int64_t expected = 0;
    for (std::uint32_t index = 0; index < 10000u; ++index) {
        AccountingVector contribution;
        contribution.monetary =
            MonetaryMeasure::known_amount(1000001, "USD", Provenance::Measured);
        auto sum = accumulate(total, contribution);
        RL_CHECK_OK(sum);
        total = sum.take();
        expected += 1000001;
    }
    RL_CHECK_EQ(expected, 10000010000);
    RL_CHECK(total.monetary.known);
    RL_CHECK_EQ(total.monetary.micro_units, expected);

    AccountingVector refund;
    refund.monetary = MonetaryMeasure::known_amount(-3, "USD", Provenance::Measured);
    auto settled = accumulate(total, refund);
    RL_CHECK_OK(settled);
    RL_CHECK_EQ(settled.value().monetary.micro_units, expected - 3);
}

// --- aggregation across scopes ----------------------------------------------

RL_TEST(accounting_aggregates_across_attempt_experiment_branch_hypothesis_session_and_result) {
    auto created = ledger_with_two_attempts(local_config());
    RL_CHECK_OK(created);
    const std::shared_ptr<Ledger>& book = created.value();

    RL_CHECK_OK(book->append(accounting_recorded(attempt_scope(1), known_vector(100, 1500))));
    RL_CHECK_OK(book->append(accounting_recorded(attempt_scope(2), known_vector(250, 2500))));

    auto snapshot = book->snapshot();
    const auto total_of = [&snapshot](SubjectId scope, const char* what,
                                      std::uint64_t expected_tokens, std::int64_t expected_micro,
                                      std::uint64_t expected_contributions) {
        const AccountingAggregate aggregate = expect(snapshot.accounting(scope), what);
        RL_CHECK(!aggregate.empty);
        RL_CHECK_EQ(aggregate.contributions, expected_contributions);
        RL_CHECK(aggregate.complete);
        RL_CHECK_EQ(aggregate.contributions_with_unknown_fields, 0u);
        RL_CHECK_EQ(aggregate.total.model_input_tokens.units(), expected_tokens);
        RL_CHECK(aggregate.total.model_input_tokens.is_known());
        RL_CHECK_EQ(aggregate.total.model_output_tokens.units(), expected_tokens / 2u);
        RL_CHECK_EQ(aggregate.total.monetary.micro_units, expected_micro);
        RL_CHECK(aggregate.total.monetary.known);
        RL_CHECK_EQ(aggregate.total.attempts.units(), expected_contributions);
        RL_CHECK(aggregate.total.all_known());
        return aggregate;
    };

    // The attempt scope is the record itself.
    total_of(attempt_scope(1), "attempt 1", 100, 1500, 1u);
    total_of(attempt_scope(2), "attempt 2", 250, 2500, 1u);
    // Every wider scope is the deterministic sum of what it contains.
    total_of(SubjectId::of(ExperimentId::from_value(1)), "experiment 1", 350, 4000, 2u);
    total_of(SubjectId::of(BranchId::from_value(1)), "branch 1", 350, 4000, 2u);
    total_of(SubjectId::of(HypothesisId::from_value(1)), "hypothesis 1", 350, 4000, 2u);
    total_of(SubjectId::of(ResearchSessionId::from_value(1)), "session 1", 350, 4000, 2u);
    total_of(SubjectId::of(ResultId::from_value(1)), "result 1", 350, 4000, 2u);
    total_of(SubjectId::of(ResultId::from_value(2)), "result 2", 350, 4000, 2u);

    // The same query twice returns the same total.
    const AccountingAggregate first =
        expect(snapshot.accounting(SubjectId::of(ResearchSessionId::from_value(1))), "session 1");
    const AccountingAggregate second =
        expect(snapshot.accounting(SubjectId::of(ResearchSessionId::from_value(1))), "session 1");
    RL_CHECK_EQ(first.total.model_input_tokens.units(), second.total.model_input_tokens.units());
    RL_CHECK_EQ(first.contributions, second.contributions);
    RL_CHECK_EQ(first.total.monetary.micro_units, second.total.monetary.micro_units);
    // Nothing broader than an attempt was charged by this test.
    RL_CHECK_EQ(expect(book->snapshot().stats(), "stats").accounting_records, 2u);
}

RL_TEST(accounting_aggregate_reports_unknown_and_empty_honestly) {
    auto created = ledger_with_two_attempts(local_config());
    RL_CHECK_OK(created);
    const std::shared_ptr<Ledger>& book = created.value();

    RL_CHECK_OK(book->append(accounting_recorded(attempt_scope(1), known_vector(10, 100))));
    AccountingVector partial;
    partial.model_input_tokens = InputTokens::known(5, Provenance::Measured);
    RL_CHECK_OK(book->append(accounting_recorded(attempt_scope(1), partial)));

    auto snapshot = book->snapshot();
    const AccountingAggregate aggregate = expect(snapshot.accounting(attempt_scope(1)),
                                                 "attempt 1 accounting");
    RL_CHECK(!aggregate.empty);
    RL_CHECK(!aggregate.complete);
    RL_CHECK_EQ(aggregate.contributions, 2u);
    RL_CHECK_EQ(aggregate.contributions_with_unknown_fields, 1u);
    RL_CHECK_EQ(aggregate.total.model_input_tokens.units(), 15u);
    RL_CHECK(aggregate.total.model_input_tokens.is_known());
    // The fields the partial contribution did not state stay unknown.
    RL_CHECK(!aggregate.total.model_output_tokens.is_known());
    RL_CHECK(!aggregate.total.model_output_tokens.is_zero());
    RL_CHECK(!aggregate.total.monetary.known);
    RL_CHECK(!aggregate.total.all_known());
    RL_CHECK(!aggregate.total.is_empty());

    // A scope with no accounting at all is empty, and an empty aggregate is not
    // a zero total.
    const AccountingAggregate none = expect(snapshot.accounting(attempt_scope(2)),
                                            "attempt 2 accounting");
    RL_CHECK(none.empty);
    RL_CHECK(!none.complete);
    RL_CHECK_EQ(none.contributions, 0u);
    RL_CHECK(none.total.is_empty());
    RL_CHECK(!none.total.model_input_tokens.is_known());
    RL_CHECK(!none.total.monetary.known);

    // A scope that is not committed is not found rather than empty.
    RL_CHECK_CODE(snapshot.accounting(SubjectId::of(AttemptId::from_value(777))),
                  ErrorCode::NotFound);
    RL_CHECK_CODE(snapshot.accounting(SubjectId{}), ErrorCode::InvalidIdentity);
}

RL_TEST(accounting_counts_shared_ancestry_once) {
    auto created = ledger_with_two_attempts(local_config());
    RL_CHECK_OK(created);
    const std::shared_ptr<Ledger>& book = created.value();

    // One accounting record, attached to the only attempt both results descend
    // from.
    RL_CHECK_OK(book->append(accounting_recorded(attempt_scope(1), known_vector(400, 4000))));

    auto snapshot = book->snapshot();
    const AccountingAggregate session_total =
        expect(snapshot.accounting(SubjectId::of(ResearchSessionId::from_value(1))),
               "session 1 accounting");
    RL_CHECK_EQ(session_total.contributions, 1u);
    RL_CHECK_EQ(session_total.total.model_input_tokens.units(), 400u);
    RL_CHECK_EQ(session_total.total.monetary.micro_units, 4000);

    // Both results see the same shared record once, and the session total is
    // that record once, not once per result.
    const AccountingAggregate first = expect(snapshot.accounting(SubjectId::of(ResultId::from_value(1))),
                                             "result 1 accounting");
    const AccountingAggregate second = expect(snapshot.accounting(SubjectId::of(ResultId::from_value(2))),
                                              "result 2 accounting");
    RL_CHECK_EQ(first.contributions, 1u);
    RL_CHECK_EQ(second.contributions, 1u);
    RL_CHECK_EQ(first.total.model_input_tokens.units(), 400u);
    RL_CHECK_EQ(second.total.model_input_tokens.units(), 400u);
    RL_CHECK_EQ(session_total.total.model_input_tokens.units(), 400u);

    // The hypothesis scope reaches the same attempts through its experiments.
    const AccountingAggregate hypothesis_total =
        expect(snapshot.accounting(SubjectId::of(HypothesisId::from_value(1))),
               "hypothesis 1 accounting");
    RL_CHECK_EQ(hypothesis_total.contributions, 1u);
    RL_CHECK_EQ(hypothesis_total.total.model_input_tokens.units(), 400u);

    // A second record concerning the same scope is a second contribution: the
    // set removes shared subjects, not committed records.
    RL_CHECK_OK(book->append(accounting_recorded(attempt_scope(1), known_vector(100, 1000))));
    const AccountingAggregate grown =
        expect(book->snapshot().accounting(SubjectId::of(ResearchSessionId::from_value(1))),
               "session 1 accounting");
    RL_CHECK_EQ(grown.contributions, 2u);
    RL_CHECK_EQ(grown.total.model_input_tokens.units(), 500u);
    RL_CHECK_EQ(grown.total.monetary.micro_units, 5000);
}

RL_TEST(accounting_currency_conflict_surfaces_through_the_ledger) {
    auto created = ledger_with_two_attempts(local_config());
    RL_CHECK_OK(created);
    const std::shared_ptr<Ledger>& book = created.value();

    AccountingVector dollars = known_vector(100, 1500);
    AccountingVector euros = known_vector(200, 2500);
    euros.monetary = MonetaryMeasure::known_amount(2500, "EUR", Provenance::Measured);
    RL_CHECK_OK(book->append(accounting_recorded(attempt_scope(1), dollars)));
    RL_CHECK_OK(book->append(accounting_recorded(attempt_scope(2), euros)));

    auto snapshot = book->snapshot();
    // Each attempt alone is exact.
    RL_CHECK_OK(snapshot.accounting(attempt_scope(1)));
    RL_CHECK_OK(snapshot.accounting(attempt_scope(2)));
    // The wider scope cannot be added, and says so instead of guessing a rate.
    RL_CHECK_CODE(snapshot.accounting(SubjectId::of(ResearchSessionId::from_value(1))),
                  ErrorCode::Unsupported);
    RL_CHECK_CODE(snapshot.accounting(SubjectId::of(ExperimentId::from_value(1))),
                  ErrorCode::Unsupported);

    // An accounting record that states no known quantity at all is refused.
    AccountingVector nothing;
    nothing.monetary = MonetaryMeasure::unknown(Provenance::Measured);
    RL_CHECK_CODE(book->append(accounting_recorded(attempt_scope(1), nothing)),
                  ErrorCode::InvalidArgument);
    // A known monetary amount must name its currency.
    AccountingVector anonymous = known_vector(1, 0);
    anonymous.monetary = MonetaryMeasure::known_amount(10, "", Provenance::Measured);
    RL_CHECK_CODE(book->append(accounting_recorded(attempt_scope(1), anonymous)),
                  ErrorCode::InvalidArgument);
    // A scope that is not a committed subject cannot be charged.
    RL_CHECK_CODE(book->append(accounting_recorded(SubjectId::of(AttemptId::from_value(404)),
                                                   known_vector(1, 1))),
                  ErrorCode::MissingDependency);
}

RL_TEST(accounting_provenance_is_never_stronger_than_its_weakest_contributor) {
    const auto sum_provenance = [](Provenance lhs, Provenance rhs) {
        AccountingVector left;
        left.model_input_tokens = InputTokens::known(1, lhs);
        AccountingVector right;
        right.model_input_tokens = InputTokens::known(2, rhs);
        auto total = accumulate(left, right);
        RL_CHECK_OK(total);
        RL_CHECK_EQ(total.value().model_input_tokens.units(), 3u);
        return total.value().model_input_tokens.provenance();
    };

    // A derived total is never reported as measured.
    RL_CHECK(sum_provenance(Provenance::Measured, Provenance::Measured) == Provenance::Derived);
    RL_CHECK(sum_provenance(Provenance::Measured, Provenance::Reported) == Provenance::Derived);
    // The weakest contributor decides the provenance of the total.
    RL_CHECK(sum_provenance(Provenance::Measured, Provenance::Estimated) == Provenance::Estimated);
    RL_CHECK(sum_provenance(Provenance::Measured, Provenance::Synthetic) == Provenance::Synthetic);
    RL_CHECK(sum_provenance(Provenance::Estimated, Provenance::Measured) == Provenance::Estimated);
    RL_CHECK(sum_provenance(Provenance::Reconstructed, Provenance::Estimated) ==
             Provenance::Estimated);
    RL_CHECK(sum_provenance(Provenance::Synthetic, Provenance::Unknown) == Provenance::Unknown);
    // An unavailable contribution contributes its provenance to the unknown total.
    AccountingVector measured;
    measured.model_input_tokens = InputTokens::known(5, Provenance::Measured);
    AccountingVector missing;
    missing.model_input_tokens = InputTokens::unknown(Provenance::Synthetic);
    auto unknown = accumulate(measured, missing);
    RL_CHECK_OK(unknown);
    RL_CHECK(!unknown.value().model_input_tokens.is_known());
    RL_CHECK(unknown.value().model_input_tokens.provenance() == Provenance::Synthetic);

    // The same rule holds for an aggregate the ledger computes itself.
    auto created = ledger_with_two_attempts(local_config());
    RL_CHECK_OK(created);
    const std::shared_ptr<Ledger>& book = created.value();
    RL_CHECK_OK(book->append(accounting_recorded(attempt_scope(1),
                                                 known_vector(10, 100, Provenance::Measured))));
    RL_CHECK_OK(book->append(accounting_recorded(attempt_scope(2),
                                                 known_vector(20, 200, Provenance::Estimated))));
    auto snapshot = book->snapshot();
    const AccountingAggregate aggregate =
        expect(snapshot.accounting(SubjectId::of(ResearchSessionId::from_value(1))),
               "session 1 accounting");
    RL_CHECK_EQ(aggregate.total.model_input_tokens.units(), 30u);
    RL_CHECK(aggregate.total.model_input_tokens.provenance() == Provenance::Estimated);
    RL_CHECK(aggregate.total.monetary.known);
    RL_CHECK_EQ(aggregate.total.monetary.micro_units, 300);
    RL_CHECK(aggregate.total.monetary.provenance == Provenance::Estimated);
}

// --- typed observation values -----------------------------------------------

RL_TEST(accounting_observation_values_are_typed) {
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double infinity = std::numeric_limits<double>::infinity();

    // Non-finite fractional values are rejected before they can be stored.
    RL_CHECK_CODE(MetricValue::ratio(nan), ErrorCode::InvalidArgument);
    RL_CHECK_CODE(MetricValue::rate(infinity), ErrorCode::InvalidArgument);
    RL_CHECK_CODE(MetricValue::decimal(-infinity), ErrorCode::InvalidArgument);
    RL_CHECK_CODE(MetricValue::decimal(nan), ErrorCode::InvalidArgument);
    // Finite ones are accepted.
    RL_CHECK_OK(MetricValue::ratio(0.5));
    RL_CHECK_OK(MetricValue::rate(12.5));
    RL_CHECK_OK(MetricValue::signed_integer(-7));
    RL_CHECK_OK(MetricValue::unsigned_integer(7));
    RL_CHECK_OK(MetricValue::boolean(true));
    RL_CHECK_OK(MetricValue::category("baseline"));
    RL_CHECK_OK(MetricValue::digest(digest_of("observation")));

    // A declared unit accepts only compatible value kinds.
    RL_CHECK(unit_accepts_value(UnitKind::Ratio, MetricValueKind::Ratio));
    RL_CHECK(unit_accepts_value(UnitKind::Ratio, MetricValueKind::Unknown));
    RL_CHECK(!unit_accepts_value(UnitKind::Nanoseconds, MetricValueKind::Bytes));
    RL_CHECK(!unit_accepts_value(UnitKind::Bytes, MetricValueKind::DurationNanos));
    RL_CHECK(!unit_accepts_value(UnitKind::Ratio, MetricValueKind::Digest));
    RL_CHECK(unit_accepts_value(UnitKind::Custom, MetricValueKind::Digest));

    auto created = ledger_with_two_attempts(local_config());
    RL_CHECK_OK(created);
    const std::shared_ptr<Ledger>& book = created.value();

    // A value that does not match the stated unit is refused, not reinterpreted.
    const MetricValue bytes_value = expect(MetricValue::bytes(4096), "bytes value");
    RL_CHECK_CODE(book->append(observation_recorded(1, 1, "latency", bytes_value,
                                                    UnitKind::Nanoseconds)),
                  ErrorCode::InvalidArgument);
    // A value whose kind differs from the declared metric kind is refused.
    RL_CHECK_OK(book->append(metric_declared(1, 1, "accuracy", UnitKind::Ratio,
                                             MetricValueKind::Ratio)));
    ObservationRecorded mismatched;
    mismatched.observation = ObservationId::from_value(2);
    mismatched.attempt = AttemptId::from_value(1);
    mismatched.metric = MetricId::from_value(1);
    mismatched.key = "accuracy";
    mismatched.unit = UnitKind::Ratio;
    mismatched.value = expect(MetricValue::rate(0.75), "rate value");
    mismatched.measured_at = now_timestamp();
    mismatched.source = "benchmark";
    mismatched.authority = "worker:1";
    RL_CHECK_CODE(book->append(draft_of(mismatched, Provenance::Measured)),
                  ErrorCode::InvalidArgument);
    ObservationRecorded wrong_unit;
    wrong_unit.observation = ObservationId::from_value(3);
    wrong_unit.attempt = AttemptId::from_value(1);
    wrong_unit.metric = MetricId::from_value(1);
    wrong_unit.key = "accuracy";
    wrong_unit.unit = UnitKind::Bytes;
    wrong_unit.value = bytes_value;
    wrong_unit.measured_at = now_timestamp();
    wrong_unit.source = "benchmark";
    wrong_unit.authority = "worker:1";
    RL_CHECK_CODE(book->append(draft_of(wrong_unit, Provenance::Measured)),
                  ErrorCode::InvalidArgument);
    // A metric must state a unit and a value kind that agree with each other.
    RL_CHECK_CODE(book->append(metric_declared(2, 1, "latency", UnitKind::None,
                                               MetricValueKind::DurationNanos)),
                  ErrorCode::InvalidArgument);
    RL_CHECK_CODE(book->append(metric_declared(3, 1, "latency", UnitKind::Bytes,
                                               MetricValueKind::DurationNanos)),
                  ErrorCode::InvalidArgument);

    // An unmeasured value is stored as unknown, never as zero.
    RL_CHECK_OK(book->append(observation_recorded(1, 1, "accuracy", MetricValue::unknown(),
                                                  UnitKind::Ratio)));
    // Every value kind round trips through the committed record.
    const MetricValue rate_value = expect(MetricValue::rate(2.5), "rate");
    const MetricValue boolean_value = expect(MetricValue::boolean(false), "boolean");
    const MetricValue signed_value = expect(MetricValue::signed_integer(-42), "signed");
    const MetricValue unsigned_value = expect(MetricValue::unsigned_integer(42), "unsigned");
    const MetricValue category_value = expect(MetricValue::category("baseline"), "category");
    const MetricValue digest_value = expect(MetricValue::digest(digest_of("payload")), "digest");
    RL_CHECK_OK(book->append(observation_recorded(4, 1, "throughput", rate_value,
                                                  UnitKind::Ratio)));
    RL_CHECK_OK(book->append(observation_recorded(5, 1, "converged", boolean_value,
                                                  UnitKind::Count)));
    RL_CHECK_OK(book->append(observation_recorded(6, 1, "delta", signed_value,
                                                  UnitKind::Count)));
    RL_CHECK_OK(book->append(observation_recorded(7, 1, "samples", unsigned_value,
                                                  UnitKind::Count)));
    RL_CHECK_OK(book->append(observation_recorded(8, 1, "regime", category_value,
                                                  UnitKind::Category)));
    RL_CHECK_OK(book->append(observation_recorded(9, 1, "corpus", digest_value,
                                                  UnitKind::Custom)));

    auto snapshot = book->snapshot();
    const std::vector<ObservationView> observations =
        expect(snapshot.observations(AttemptId::from_value(1)), "observations of attempt 1");
    RL_CHECK_EQ(observations.size(), 7u);
    const auto find = [&observations](std::uint64_t id) -> const ObservationView* {
        for (const ObservationView& view : observations) {
            if (view.observation == ObservationId::from_value(id)) {
                return &view;
            }
        }
        return nullptr;
    };
    const ObservationView* unknown_view = find(1);
    RL_CHECK(unknown_view != nullptr);
    if (unknown_view != nullptr) {
        RL_CHECK(unknown_view->value.is_unknown());
        RL_CHECK(unknown_view->value.kind() == MetricValueKind::Unknown);
        RL_CHECK_EQ(unknown_view->value.to_text(), std::string("UNKNOWN"));
        RL_CHECK(unknown_view->unit == UnitKind::Ratio);
    }
    const ObservationView* rate_view = find(4);
    const ObservationView* boolean_view = find(5);
    const ObservationView* signed_view = find(6);
    const ObservationView* unsigned_view = find(7);
    const ObservationView* category_view = find(8);
    const ObservationView* digest_view = find(9);
    RL_CHECK(rate_view != nullptr && rate_view->value == rate_value);
    RL_CHECK(boolean_view != nullptr && boolean_view->value == boolean_value);
    RL_CHECK(signed_view != nullptr && signed_view->value == signed_value);
    RL_CHECK(unsigned_view != nullptr && unsigned_view->value == unsigned_value);
    RL_CHECK(category_view != nullptr && category_view->value == category_value);
    RL_CHECK(digest_view != nullptr && digest_view->value == digest_value);
    if (rate_view != nullptr) {
        RL_CHECK(rate_view->value.kind() == MetricValueKind::Rate);
        RL_CHECK(rate_view->provenance == Provenance::Measured);
    }
    if (digest_view != nullptr) {
        RL_CHECK(digest_view->value.as_digest() == digest_of("payload"));
        RL_CHECK_EQ(digest_view->value.to_text(), to_hex(digest_of("payload")));
    }
    if (signed_view != nullptr) {
        RL_CHECK_EQ(signed_view->value.as_signed_integer(), -42);
    }
    if (boolean_view != nullptr) {
        RL_CHECK(!boolean_view->value.as_boolean());
    }

    // The same values survive a persistence round trip unchanged.
    const std::string path = temp_path("accounting-observations.bin");
    require_ok(book->save(path), "save");
    auto reloaded = Ledger::load(path, local_config());
    RL_CHECK_OK(reloaded);
    const std::vector<ObservationView> after =
        expect(reloaded.value()->snapshot().observations(AttemptId::from_value(1)),
               "observations after reload");
    RL_CHECK_EQ(after.size(), observations.size());
    for (std::size_t index = 0; index < after.size() && index < observations.size(); ++index) {
        RL_CHECK(after[index].value == observations[index].value);
        RL_CHECK(after[index].unit == observations[index].unit);
    }
    std::remove(path.c_str());
}

}  // namespace
}  // namespace research_ledger
