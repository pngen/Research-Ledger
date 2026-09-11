#include "test_framework.hpp"
#include "test_support.hpp"

#include <string>
#include <unordered_set>

namespace research_ledger {
namespace {

using namespace research_ledger::test;

RL_TEST(identity_domains_are_distinct_types) {
    // Identity domains do not share a representation: the domains differ, the
    // textual forms differ, and a serialized identity carries its domain tag.
    RL_CHECK(IdentityDomain::Hypothesis != IdentityDomain::Experiment);
    RL_CHECK(ResearchSessionId::domain == IdentityDomain::ResearchSession);
    RL_CHECK(HypothesisId::domain == IdentityDomain::Hypothesis);
    RL_CHECK(AttemptId::domain == IdentityDomain::Attempt);
    RL_CHECK(ResultId::domain == IdentityDomain::Result);
    RL_CHECK(!HypothesisId::represents_generation);
    RL_CHECK(HypothesisGeneration::represents_generation);

    RL_CHECK_EQ(to_string(HypothesisId::from_value(7)), std::string("hypothesis:7"));
    RL_CHECK_EQ(to_string(ExperimentId::from_value(7)), std::string("experiment:7"));
    RL_CHECK_EQ(to_string(HypothesisGeneration::from_value(3)), std::string("hypothesis-generation:3"));
    RL_CHECK(to_string(HypothesisId::from_value(7)) != to_string(ExperimentId::from_value(7)));
}

RL_TEST(identity_zero_is_never_valid) {
    RL_CHECK(!ResearchSessionId{}.valid());
    RL_CHECK(!HypothesisId::from_value(0).valid());
    RL_CHECK(!HypothesisGeneration{}.valid());
    RL_CHECK(!RecordSequence{}.valid());
    RL_CHECK(!CoordinatorEpoch{}.valid());
    RL_CHECK(HypothesisGeneration::first().valid());
    RL_CHECK_EQ(HypothesisGeneration::first().value(), 1u);
    RL_CHECK(!HypothesisGeneration::from_value(0xffffffffu).next().valid());
}

RL_TEST(identity_serialization_rejects_a_foreign_domain) {
    // The same numeric value in another domain is not the same identity: the
    // decoder rejects the domain tag rather than reinterpreting the bytes.
    ByteWriter writer;
    write_identity(writer, HypothesisId::from_value(42));
    RL_CHECK(writer.ok());

    Limits limits;
    ByteReader reader(writer.buffer(), limits);
    RL_CHECK_CODE(reader.read_strong_id<ExperimentId>(), ErrorCode::InvalidIdentity);

    ByteReader second(writer.buffer(), limits);
    auto hypothesis = second.read_strong_id<HypothesisId>();
    RL_CHECK_OK(hypothesis);
    RL_CHECK_EQ(hypothesis.value().value(), 42u);
}

RL_TEST(identity_generation_zero_is_not_authority) {
    ByteWriter writer;
    write_entity_generation(writer, HypothesisGeneration::from_value(0));
    Limits limits;
    ByteReader reader(writer.buffer(), limits);
    RL_CHECK_CODE(reader.read_generation<HypothesisGeneration>(), ErrorCode::InvalidIdentity);
}

RL_TEST(identity_text_parsing_is_exact) {
    auto hypothesis = parse_hypothesis_id("hypothesis:12");
    RL_CHECK(hypothesis.has_value());
    RL_CHECK_EQ(hypothesis->value(), 12u);
    RL_CHECK(!parse_hypothesis_id("experiment:12").has_value());
    RL_CHECK(!parse_hypothesis_id("hypothesis:0").has_value());
    RL_CHECK(!parse_hypothesis_id("hypothesis:").has_value());
    RL_CHECK(!parse_hypothesis_id("hypothesis:12x").has_value());
    RL_CHECK(!parse_hypothesis_id("hypothesis:99999999999999999999999").has_value());
    RL_CHECK(!parse_hypothesis_id("12").has_value());
}

RL_TEST(subject_identities_keep_their_domain) {
    const SubjectId hypothesis = SubjectId::of(HypothesisId::from_value(5));
    const SubjectId experiment = SubjectId::of(ExperimentId::from_value(5));
    RL_CHECK(hypothesis.kind() == SubjectKind::Hypothesis);
    RL_CHECK(experiment.kind() == SubjectKind::Experiment);
    RL_CHECK(hypothesis != experiment);
    RL_CHECK_EQ(hypothesis.to_string(), std::string("hypothesis:5"));
    RL_CHECK_EQ(experiment.to_string(), std::string("experiment:5"));
    RL_CHECK(!SubjectId{}.valid());
    RL_CHECK_EQ(SubjectId{}.to_string(), std::string("none"));
    auto typed = hypothesis.as<HypothesisId>();
    RL_CHECK(typed.has_value());
    RL_CHECK(!hypothesis.as<ExperimentId>().has_value());
}

RL_TEST(identity_hashing_separates_domains) {
    std::unordered_set<HypothesisId> hypotheses;
    hypotheses.insert(HypothesisId::from_value(1));
    hypotheses.insert(HypothesisId::from_value(1));
    hypotheses.insert(HypothesisId::from_value(2));
    RL_CHECK_EQ(hypotheses.size(), 2u);
}

RL_TEST(enum_names_round_trip) {
    RL_CHECK(parse_hypothesis_status("NOT_SUPPORTED") == HypothesisStatus::NotSupported);
    RL_CHECK(parse_branch_kind("ALTERNATE_METHOD") == BranchKind::AlternateMethod);
    RL_CHECK(parse_failure_category("OUTCOME_UNKNOWN") == FailureCategory::OutcomeUnknown);
    RL_CHECK(parse_result_status("SUPERSEDED") == ResultStatus::Superseded);
    RL_CHECK(parse_provenance("SYNTHETIC") == Provenance::Synthetic);
    RL_CHECK(!parse_provenance("PROBABLY_REAL").has_value());
    RL_CHECK(parse_unit_kind("NANOSECONDS") == UnitKind::Nanoseconds);
    RL_CHECK(parse_error_code("STALE_EPOCH") == ErrorCode::StaleEpoch);
    RL_CHECK(!parse_error_code("MOSTLY_FINE").has_value());
    RL_CHECK(parse_record_type("RESULT_STATUS_CHANGED") == RecordType::ResultStatusChanged);
    RL_CHECK(parse_message_type("APPEND_REQUEST") == MessageType::AppendRequest);
}

RL_TEST(unit_and_value_kind_compatibility_is_enforced) {
    RL_CHECK(unit_accepts_value(UnitKind::Nanoseconds, MetricValueKind::DurationNanos));
    RL_CHECK(!unit_accepts_value(UnitKind::Nanoseconds, MetricValueKind::Bytes));
    RL_CHECK(!unit_accepts_value(UnitKind::Bytes, MetricValueKind::DurationNanos));
    RL_CHECK(unit_accepts_value(UnitKind::Ratio, MetricValueKind::Ratio));
    RL_CHECK(unit_accepts_value(UnitKind::Custom, MetricValueKind::Digest));
    RL_CHECK(!unit_accepts_value(UnitKind::Count, MetricValueKind::Digest));
}

RL_TEST(digest_is_sha256_and_chains) {
    // Published SHA-256 of the empty string and of "abc".
    RL_CHECK_EQ(to_hex(sha256(std::string_view{})),
                std::string("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
    RL_CHECK_EQ(to_hex(sha256(std::string_view{"abc"})),
                std::string("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
    const Digest first = sha256("first");
    const Digest second = sha256("second");
    RL_CHECK(chain_digest(first, second) != chain_digest(second, first));
    RL_CHECK(chain_digest(first, second) == chain_digest(first, second));
    auto parsed = digest_from_hex(to_hex(first));
    RL_CHECK(parsed.has_value());
    RL_CHECK(parsed.value() == first);
    RL_CHECK(!digest_from_hex("zz").has_value());
    RL_CHECK(Digest{}.is_zero());
    RL_CHECK(!first.is_zero());
}

RL_TEST(crc32_matches_known_vector) {
    const char* text = "123456789";
    const std::span<const std::byte> bytes(reinterpret_cast<const std::byte*>(text), 9);
    RL_CHECK_EQ(crc32(bytes), 0xcbf43926u);
}

}  // namespace
}  // namespace research_ledger
