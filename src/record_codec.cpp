#include "research_ledger/record_codec.hpp"

#include <bit>
#include <cmath>
#include <string>
#include <utility>

#include "research_ledger/codec.hpp"

namespace research_ledger {
namespace {

Status malformed(ByteReader& reader, std::string message) {
    return reader.fail(reader.malformed_code(), std::move(message));
}

IdentityDomain domain_of(SubjectKind kind) noexcept {
    switch (kind) {
        case SubjectKind::None:
            return IdentityDomain::None;
        case SubjectKind::Session:
            return IdentityDomain::ResearchSession;
        case SubjectKind::Hypothesis:
            return IdentityDomain::Hypothesis;
        case SubjectKind::Experiment:
            return IdentityDomain::Experiment;
        case SubjectKind::Branch:
            return IdentityDomain::Branch;
        case SubjectKind::Attempt:
            return IdentityDomain::Attempt;
        case SubjectKind::ModelCall:
            return IdentityDomain::ModelCall;
        case SubjectKind::ToolCall:
            return IdentityDomain::ToolCall;
        case SubjectKind::Artifact:
            return IdentityDomain::Artifact;
        case SubjectKind::Observation:
            return IdentityDomain::Observation;
        case SubjectKind::Failure:
            return IdentityDomain::Failure;
        case SubjectKind::Decision:
            return IdentityDomain::Decision;
        case SubjectKind::Result:
            return IdentityDomain::Result;
    }
    return IdentityDomain::None;
}

SubjectId subject_from(SubjectKind kind, std::uint64_t value) noexcept {
    switch (kind) {
        case SubjectKind::None:
            return SubjectId{};
        case SubjectKind::Session:
            return SubjectId::of(ResearchSessionId::from_value(value));
        case SubjectKind::Hypothesis:
            return SubjectId::of(HypothesisId::from_value(value));
        case SubjectKind::Experiment:
            return SubjectId::of(ExperimentId::from_value(value));
        case SubjectKind::Branch:
            return SubjectId::of(BranchId::from_value(value));
        case SubjectKind::Attempt:
            return SubjectId::of(AttemptId::from_value(value));
        case SubjectKind::ModelCall:
            return SubjectId::of(ModelCallId::from_value(value));
        case SubjectKind::ToolCall:
            return SubjectId::of(ToolCallId::from_value(value));
        case SubjectKind::Artifact:
            return SubjectId::of(ArtifactId::from_value(value));
        case SubjectKind::Observation:
            return SubjectId::of(ObservationId::from_value(value));
        case SubjectKind::Failure:
            return SubjectId::of(FailureId::from_value(value));
        case SubjectKind::Decision:
            return SubjectId::of(DecisionId::from_value(value));
        case SubjectKind::Result:
            return SubjectId::of(ResultId::from_value(value));
    }
    return SubjectId{};
}

template <class Enum>
void write_enum(ByteWriter& writer, Enum value) {
    writer.u8(static_cast<std::uint8_t>(value));
}

// Provenance of an individual quantity may be UNKNOWN: an accounting field that
// nobody measured is provenance-unknown, which is different from a record whose
// provenance was never stated.
Result<Provenance> read_quantity_provenance(ByteReader& reader) {
    auto raw = reader.u8();
    if (!raw.ok()) {
        return raw.status();
    }
    if (raw.value() > static_cast<std::uint8_t>(Provenance::Reconstructed)) {
        return malformed(reader, "invalid provenance value");
    }
    return static_cast<Provenance>(raw.value());
}

template <class Enum>
Result<Enum> read_enum(ByteReader& reader, std::uint8_t last, const char* what) {
    auto raw = reader.u8();
    if (!raw.ok()) {
        return raw.status();
    }
    if (raw.value() == 0 || raw.value() > last) {
        return malformed(reader, std::string("invalid ") + what + " value");
    }
    return static_cast<Enum>(raw.value());
}

template <class Tag>
void write_measure(ByteWriter& writer, const Measure<Tag>& measure) {
    writer.boolean(measure.is_known());
    writer.u64(measure.units());
    write_enum(writer, measure.provenance());
}

template <class Tag>
Result<Measure<Tag>> read_measure(ByteReader& reader) {
    auto known = reader.boolean();
    if (!known.ok()) {
        return known.status();
    }
    auto units = reader.u64();
    if (!units.ok()) {
        return units.status();
    }
    auto provenance = read_quantity_provenance(reader);
    if (!provenance.ok()) {
        return provenance.status();
    }
    if (known.value()) {
        return Measure<Tag>::known(units.value(), provenance.value());
    }
    return Measure<Tag>::unknown(provenance.value());
}

void write_monetary(ByteWriter& writer, const MonetaryMeasure& measure, const Limits& limits) {
    writer.boolean(measure.known);
    writer.i64(measure.micro_units);
    writer.string(measure.currency, limits.max_label_length);
    write_enum(writer, measure.provenance);
}

Result<MonetaryMeasure> read_monetary(ByteReader& reader, const Limits& limits) {
    auto known = reader.boolean();
    if (!known.ok()) {
        return known.status();
    }
    auto micro = reader.i64();
    if (!micro.ok()) {
        return micro.status();
    }
    auto currency = reader.string(limits.max_label_length);
    if (!currency.ok()) {
        return currency.status();
    }
    auto provenance = read_quantity_provenance(reader);
    if (!provenance.ok()) {
        return provenance.status();
    }
    MonetaryMeasure measure;
    measure.known = known.value();
    measure.micro_units = micro.value();
    measure.currency = currency.take();
    measure.provenance = provenance.value();
    return measure;
}

void write_accounting(ByteWriter& writer, const AccountingVector& accounting, const Limits& limits) {
    write_measure(writer, accounting.model_input_tokens);
    write_measure(writer, accounting.model_output_tokens);
    write_measure(writer, accounting.model_calls);
    write_measure(writer, accounting.tool_calls);
    write_measure(writer, accounting.accelerator_nanos);
    write_measure(writer, accounting.cpu_nanos);
    write_measure(writer, accounting.wall_nanos);
    write_measure(writer, accounting.storage_bytes);
    write_measure(writer, accounting.transfer_bytes);
    write_measure(writer, accounting.energy_micro_joules);
    write_measure(writer, accounting.attempts);
    write_measure(writer, accounting.retries);
    write_measure(writer, accounting.failure_overhead_nanos);
    write_monetary(writer, accounting.monetary, limits);
}

Result<AccountingVector> read_accounting(ByteReader& reader, const Limits& limits) {
    AccountingVector accounting;

#define RESEARCH_LEDGER_READ_MEASURE(FIELD)                     \
    {                                                           \
        auto measure = read_measure<decltype(accounting.FIELD)::tag_type>(reader); \
        if (!measure.ok()) {                                    \
            return measure.status();                            \
        }                                                       \
        accounting.FIELD = measure.value();                     \
    }

    RESEARCH_LEDGER_READ_MEASURE(model_input_tokens)
    RESEARCH_LEDGER_READ_MEASURE(model_output_tokens)
    RESEARCH_LEDGER_READ_MEASURE(model_calls)
    RESEARCH_LEDGER_READ_MEASURE(tool_calls)
    RESEARCH_LEDGER_READ_MEASURE(accelerator_nanos)
    RESEARCH_LEDGER_READ_MEASURE(cpu_nanos)
    RESEARCH_LEDGER_READ_MEASURE(wall_nanos)
    RESEARCH_LEDGER_READ_MEASURE(storage_bytes)
    RESEARCH_LEDGER_READ_MEASURE(transfer_bytes)
    RESEARCH_LEDGER_READ_MEASURE(energy_micro_joules)
    RESEARCH_LEDGER_READ_MEASURE(attempts)
    RESEARCH_LEDGER_READ_MEASURE(retries)
    RESEARCH_LEDGER_READ_MEASURE(failure_overhead_nanos)

#undef RESEARCH_LEDGER_READ_MEASURE

    auto monetary = read_monetary(reader, limits);
    if (!monetary.ok()) {
        return monetary.status();
    }
    accounting.monetary = monetary.value();
    return accounting;
}

void write_optional_digest(ByteWriter& writer, const std::optional<Digest>& digest) {
    writer.boolean(digest.has_value());
    if (digest.has_value()) {
        writer.digest(*digest);
    }
}

Result<std::optional<Digest>> read_optional_digest(ByteReader& reader) {
    auto present = reader.boolean();
    if (!present.ok()) {
        return present.status();
    }
    if (!present.value()) {
        return std::optional<Digest>{};
    }
    auto digest = reader.digest();
    if (!digest.ok()) {
        return digest.status();
    }
    return std::optional<Digest>{digest.value()};
}

void write_metric_value(ByteWriter& writer, const MetricValue& value, const Limits& limits) {
    write_enum(writer, value.kind());
    switch (value.kind()) {
        case MetricValueKind::Unknown:
            break;
        case MetricValueKind::SignedInteger:
            writer.i64(value.as_signed_integer());
            break;
        case MetricValueKind::UnsignedInteger:
        case MetricValueKind::DurationNanos:
        case MetricValueKind::Bytes:
        case MetricValueKind::Count:
            writer.u64(value.as_unsigned_integer());
            break;
        case MetricValueKind::Decimal:
        case MetricValueKind::Rate:
        case MetricValueKind::Ratio:
            writer.u64(std::bit_cast<std::uint64_t>(value.as_decimal()));
            break;
        case MetricValueKind::Boolean:
            writer.boolean(value.as_boolean());
            break;
        case MetricValueKind::Category:
            writer.string(value.as_category(), limits.max_string_length);
            break;
        case MetricValueKind::Digest:
            writer.digest(value.as_digest());
            break;
    }
}

Result<MetricValue> read_metric_value(ByteReader& reader, const Limits& limits) {
    auto raw_kind = reader.u8();
    if (!raw_kind.ok()) {
        return raw_kind.status();
    }
    if (raw_kind.value() > static_cast<std::uint8_t>(MetricValueKind::Digest)) {
        return malformed(reader, "invalid metric value kind");
    }
    const auto kind = static_cast<MetricValueKind>(raw_kind.value());
    switch (kind) {
        case MetricValueKind::Unknown:
            return MetricValue::unknown();
        case MetricValueKind::SignedInteger: {
            auto value = reader.i64();
            if (!value.ok()) {
                return value.status();
            }
            return MetricValue::signed_integer(value.value());
        }
        case MetricValueKind::UnsignedInteger: {
            auto value = reader.u64();
            if (!value.ok()) {
                return value.status();
            }
            return MetricValue::unsigned_integer(value.value());
        }
        case MetricValueKind::DurationNanos: {
            auto value = reader.u64();
            if (!value.ok()) {
                return value.status();
            }
            return MetricValue::duration_nanos(value.value());
        }
        case MetricValueKind::Bytes: {
            auto value = reader.u64();
            if (!value.ok()) {
                return value.status();
            }
            return MetricValue::bytes(value.value());
        }
        case MetricValueKind::Count: {
            auto value = reader.u64();
            if (!value.ok()) {
                return value.status();
            }
            return MetricValue::count(value.value());
        }
        case MetricValueKind::Decimal:
        case MetricValueKind::Rate:
        case MetricValueKind::Ratio: {
            auto bits = reader.u64();
            if (!bits.ok()) {
                return bits.status();
            }
            const double value = std::bit_cast<double>(bits.value());
            if (!std::isfinite(value)) {
                return malformed(reader, "non-finite fractional metric value");
            }
            if (kind == MetricValueKind::Decimal) {
                return MetricValue::decimal(value);
            }
            if (kind == MetricValueKind::Rate) {
                return MetricValue::rate(value);
            }
            return MetricValue::ratio(value);
        }
        case MetricValueKind::Boolean: {
            auto value = reader.boolean();
            if (!value.ok()) {
                return value.status();
            }
            return MetricValue::boolean(value.value());
        }
        case MetricValueKind::Category: {
            auto value = reader.string(limits.max_string_length);
            if (!value.ok()) {
                return value.status();
            }
            return MetricValue::category(value.take());
        }
        case MetricValueKind::Digest: {
            auto value = reader.digest();
            if (!value.ok()) {
                return value.status();
            }
            return MetricValue::digest(value.value());
        }
    }
    return malformed(reader, "unreachable metric value kind");
}

void write_subject(ByteWriter& writer, const SubjectId& subject) {
    writer.u8(static_cast<std::uint8_t>(subject.kind()));
    if (subject.valid()) {
        write_strong_id(writer, subject.domain(), subject.value());
    }
}

Result<SubjectId> read_subject(ByteReader& reader) {
    auto raw_kind = reader.u8();
    if (!raw_kind.ok()) {
        return raw_kind.status();
    }
    if (raw_kind.value() > static_cast<std::uint8_t>(SubjectKind::Result)) {
        return malformed(reader, "invalid subject kind");
    }
    const auto kind = static_cast<SubjectKind>(raw_kind.value());
    if (kind == SubjectKind::None) {
        return SubjectId{};
    }
    auto raw_value = reader.identity_value(domain_of(kind));
    if (!raw_value.ok()) {
        return raw_value.status();
    }
    if (raw_value.value() == 0) {
        return malformed(reader, "subject identity zero is never valid");
    }
    return subject_from(kind, raw_value.value());
}

void write_evidence(ByteWriter& writer, const EvidenceRef& evidence) {
    write_subject(writer, evidence.subject);
    write_generation(writer, IdentityDomain::RecordSequence, evidence.sequence.value());
}

Result<EvidenceRef> read_evidence(ByteReader& reader) {
    auto subject = read_subject(reader);
    if (!subject.ok()) {
        return subject.status();
    }
    auto sequence = reader.generation_value(IdentityDomain::RecordSequence);
    if (!sequence.ok()) {
        return sequence.status();
    }
    if (sequence.value() == 0) {
        return malformed(reader, "evidence sequence zero is never valid");
    }
    EvidenceRef evidence;
    evidence.subject = subject.value();
    evidence.sequence = RecordSequence::from_value(sequence.value());
    return evidence;
}

// --- per record payload codecs ---------------------------------------------

void write_session_opened(ByteWriter& writer, const SessionOpened& body, const Limits& limits) {
    write_identity(writer, body.session);
    writer.string(body.label, limits.max_label_length);
    writer.string(body.question, limits.max_string_length);
    writer.count(static_cast<std::uint32_t>(body.metadata.size()), limits.max_metadata_entries);
    for (const MetadataEntry& entry : body.metadata) {
        writer.string(entry.key, limits.max_metadata_key_length);
        writer.string(entry.value, limits.max_metadata_value_length);
    }
    writer.string(body.authority, limits.max_string_length);
}

Result<SessionOpened> read_session_opened(ByteReader& reader, const Limits& limits) {
    SessionOpened body;
    auto session = reader.read_strong_id<ResearchSessionId>();
    if (!session.ok()) {
        return session.status();
    }
    body.session = session.value();
    auto label = reader.string(limits.max_label_length);
    if (!label.ok()) {
        return label.status();
    }
    body.label = label.take();
    auto question = reader.string(limits.max_string_length);
    if (!question.ok()) {
        return question.status();
    }
    body.question = question.take();
    auto count = reader.count(limits.max_metadata_entries);
    if (!count.ok()) {
        return count.status();
    }
    body.metadata.reserve(count.value());
    for (std::uint32_t index = 0; index < count.value(); ++index) {
        MetadataEntry entry;
        auto key = reader.string(limits.max_metadata_key_length);
        if (!key.ok()) {
            return key.status();
        }
        entry.key = key.take();
        auto value = reader.string(limits.max_metadata_value_length);
        if (!value.ok()) {
            return value.status();
        }
        entry.value = value.take();
        body.metadata.push_back(std::move(entry));
    }
    auto authority = reader.string(limits.max_string_length);
    if (!authority.ok()) {
        return authority.status();
    }
    body.authority = authority.take();
    return body;
}

void write_session_closed(ByteWriter& writer, const SessionClosed& body, const Limits& limits) {
    write_identity(writer, body.session);
    writer.string(body.note, limits.max_string_length);
}

Result<SessionClosed> read_session_closed(ByteReader& reader, const Limits& limits) {
    SessionClosed body;
    auto session = reader.read_strong_id<ResearchSessionId>();
    if (!session.ok()) {
        return session.status();
    }
    body.session = session.value();
    auto note = reader.string(limits.max_string_length);
    if (!note.ok()) {
        return note.status();
    }
    body.note = note.take();
    return body;
}

void write_session_annotation(ByteWriter& writer, const SessionAnnotation& body, const Limits& limits) {
    write_identity(writer, body.session);
    writer.string(body.note, limits.max_string_length);
}

Result<SessionAnnotation> read_session_annotation(ByteReader& reader, const Limits& limits) {
    SessionAnnotation body;
    auto session = reader.read_strong_id<ResearchSessionId>();
    if (!session.ok()) {
        return session.status();
    }
    body.session = session.value();
    auto note = reader.string(limits.max_string_length);
    if (!note.ok()) {
        return note.status();
    }
    body.note = note.take();
    return body;
}

void write_hypothesis_declared(ByteWriter& writer, const HypothesisDeclared& body,
                               const Limits& limits) {
    write_identity(writer, body.hypothesis);
    write_entity_generation(writer, body.generation);
    write_identity(writer, body.session);
    writer.string(body.claim, limits.max_string_length);
    writer.boolean(body.parent.has_value());
    if (body.parent.has_value()) {
        write_identity(writer, *body.parent);
        write_entity_generation(writer, body.parent_generation);
    }
    writer.string(body.authority, limits.max_string_length);
}

Result<HypothesisDeclared> read_hypothesis_declared(ByteReader& reader, const Limits& limits) {
    HypothesisDeclared body;
    auto hypothesis = reader.read_strong_id<HypothesisId>();
    if (!hypothesis.ok()) {
        return hypothesis.status();
    }
    body.hypothesis = hypothesis.value();
    auto generation = reader.read_generation<HypothesisGeneration>();
    if (!generation.ok()) {
        return generation.status();
    }
    body.generation = generation.value();
    auto session = reader.read_strong_id<ResearchSessionId>();
    if (!session.ok()) {
        return session.status();
    }
    body.session = session.value();
    auto claim = reader.string(limits.max_string_length);
    if (!claim.ok()) {
        return claim.status();
    }
    body.claim = claim.take();
    auto has_parent = reader.boolean();
    if (!has_parent.ok()) {
        return has_parent.status();
    }
    if (has_parent.value()) {
        auto parent = reader.read_strong_id<HypothesisId>();
        if (!parent.ok()) {
            return parent.status();
        }
        body.parent = parent.value();
        auto parent_generation = reader.read_generation<HypothesisGeneration>();
        if (!parent_generation.ok()) {
            return parent_generation.status();
        }
        body.parent_generation = parent_generation.value();
    }
    auto authority = reader.string(limits.max_string_length);
    if (!authority.ok()) {
        return authority.status();
    }
    body.authority = authority.take();
    return body;
}

void write_hypothesis_status_changed(ByteWriter& writer, const HypothesisStatusChanged& body,
                                     const Limits& limits) {
    write_identity(writer, body.hypothesis);
    write_entity_generation(writer, body.generation);
    write_enum(writer, body.from);
    write_enum(writer, body.to);
    writer.boolean(body.decision.has_value());
    if (body.decision.has_value()) {
        write_identity(writer, *body.decision);
    }
    writer.string(body.authority, limits.max_string_length);
}

Result<HypothesisStatusChanged> read_hypothesis_status_changed(ByteReader& reader,
                                                              const Limits& limits) {
    HypothesisStatusChanged body;
    auto hypothesis = reader.read_strong_id<HypothesisId>();
    if (!hypothesis.ok()) {
        return hypothesis.status();
    }
    body.hypothesis = hypothesis.value();
    auto generation = reader.read_generation<HypothesisGeneration>();
    if (!generation.ok()) {
        return generation.status();
    }
    body.generation = generation.value();
    auto from = read_enum<HypothesisStatus>(reader, static_cast<std::uint8_t>(HypothesisStatus::Inconclusive),
                                            "hypothesis status");
    if (!from.ok()) {
        return from.status();
    }
    body.from = from.value();
    auto to = read_enum<HypothesisStatus>(reader, static_cast<std::uint8_t>(HypothesisStatus::Inconclusive),
                                          "hypothesis status");
    if (!to.ok()) {
        return to.status();
    }
    body.to = to.value();
    auto has_decision = reader.boolean();
    if (!has_decision.ok()) {
        return has_decision.status();
    }
    if (has_decision.value()) {
        auto decision = reader.read_strong_id<DecisionId>();
        if (!decision.ok()) {
            return decision.status();
        }
        body.decision = decision.value();
    }
    auto authority = reader.string(limits.max_string_length);
    if (!authority.ok()) {
        return authority.status();
    }
    body.authority = authority.take();
    return body;
}

void write_experiment_declared(ByteWriter& writer, const ExperimentDeclared& body,
                               const Limits& limits) {
    write_identity(writer, body.experiment);
    write_entity_generation(writer, body.generation);
    write_identity(writer, body.session);
    writer.count(static_cast<std::uint32_t>(body.hypotheses.size()), limits.max_hypotheses_per_experiment);
    for (const HypothesisId hypothesis : body.hypotheses) {
        write_identity(writer, hypothesis);
    }
    write_identity(writer, body.branch);
    writer.boolean(body.parent_experiment.has_value());
    if (body.parent_experiment.has_value()) {
        write_identity(writer, *body.parent_experiment);
    }
    writer.string(body.environment_reference, limits.max_string_length);
    writer.count(static_cast<std::uint32_t>(body.inputs.size()), limits.max_inputs);
    for (const ExperimentInput& input : body.inputs) {
        write_identity(writer, input.input);
        writer.string(input.reference, limits.max_string_length);
    }
    writer.count(static_cast<std::uint32_t>(body.expected_outputs.size()), limits.max_expected_outputs);
    for (const std::string& output : body.expected_outputs) {
        writer.string(output, limits.max_string_length);
    }
    writer.string(body.authority, limits.max_string_length);
}

Result<ExperimentDeclared> read_experiment_declared(ByteReader& reader, const Limits& limits) {
    ExperimentDeclared body;
    auto experiment = reader.read_strong_id<ExperimentId>();
    if (!experiment.ok()) {
        return experiment.status();
    }
    body.experiment = experiment.value();
    auto generation = reader.read_generation<ExperimentGeneration>();
    if (!generation.ok()) {
        return generation.status();
    }
    body.generation = generation.value();
    auto session = reader.read_strong_id<ResearchSessionId>();
    if (!session.ok()) {
        return session.status();
    }
    body.session = session.value();
    auto hypothesis_count = reader.count(limits.max_hypotheses_per_experiment);
    if (!hypothesis_count.ok()) {
        return hypothesis_count.status();
    }
    body.hypotheses.reserve(hypothesis_count.value());
    for (std::uint32_t index = 0; index < hypothesis_count.value(); ++index) {
        auto hypothesis = reader.read_strong_id<HypothesisId>();
        if (!hypothesis.ok()) {
            return hypothesis.status();
        }
        body.hypotheses.push_back(hypothesis.value());
    }
    auto branch = reader.read_strong_id<BranchId>();
    if (!branch.ok()) {
        return branch.status();
    }
    body.branch = branch.value();
    auto has_parent = reader.boolean();
    if (!has_parent.ok()) {
        return has_parent.status();
    }
    if (has_parent.value()) {
        auto parent = reader.read_strong_id<ExperimentId>();
        if (!parent.ok()) {
            return parent.status();
        }
        body.parent_experiment = parent.value();
    }
    auto environment = reader.string(limits.max_string_length);
    if (!environment.ok()) {
        return environment.status();
    }
    body.environment_reference = environment.take();
    auto input_count = reader.count(limits.max_inputs);
    if (!input_count.ok()) {
        return input_count.status();
    }
    body.inputs.reserve(input_count.value());
    for (std::uint32_t index = 0; index < input_count.value(); ++index) {
        ExperimentInput input;
        auto identity = reader.read_strong_id<InputId>();
        if (!identity.ok()) {
            return identity.status();
        }
        input.input = identity.value();
        auto reference = reader.string(limits.max_string_length);
        if (!reference.ok()) {
            return reference.status();
        }
        input.reference = reference.take();
        body.inputs.push_back(std::move(input));
    }
    auto output_count = reader.count(limits.max_expected_outputs);
    if (!output_count.ok()) {
        return output_count.status();
    }
    body.expected_outputs.reserve(output_count.value());
    for (std::uint32_t index = 0; index < output_count.value(); ++index) {
        auto output = reader.string(limits.max_string_length);
        if (!output.ok()) {
            return output.status();
        }
        body.expected_outputs.push_back(output.take());
    }
    auto authority = reader.string(limits.max_string_length);
    if (!authority.ok()) {
        return authority.status();
    }
    body.authority = authority.take();
    return body;
}

void write_branch_declared(ByteWriter& writer, const BranchDeclared& body, const Limits& limits) {
    write_identity(writer, body.branch);
    write_identity(writer, body.session);
    write_enum(writer, body.kind);
    writer.boolean(body.parent_branch.has_value());
    if (body.parent_branch.has_value()) {
        write_identity(writer, *body.parent_branch);
    }
    writer.string(body.label, limits.max_label_length);
    writer.string(body.authority, limits.max_string_length);
}

Result<BranchDeclared> read_branch_declared(ByteReader& reader, const Limits& limits) {
    BranchDeclared body;
    auto branch = reader.read_strong_id<BranchId>();
    if (!branch.ok()) {
        return branch.status();
    }
    body.branch = branch.value();
    auto session = reader.read_strong_id<ResearchSessionId>();
    if (!session.ok()) {
        return session.status();
    }
    body.session = session.value();
    auto kind = read_enum<BranchKind>(reader, static_cast<std::uint8_t>(BranchKind::MergedEvidence),
                                      "branch kind");
    if (!kind.ok()) {
        return kind.status();
    }
    body.kind = kind.value();
    auto has_parent = reader.boolean();
    if (!has_parent.ok()) {
        return has_parent.status();
    }
    if (has_parent.value()) {
        auto parent = reader.read_strong_id<BranchId>();
        if (!parent.ok()) {
            return parent.status();
        }
        body.parent_branch = parent.value();
    }
    auto label = reader.string(limits.max_label_length);
    if (!label.ok()) {
        return label.status();
    }
    body.label = label.take();
    auto authority = reader.string(limits.max_string_length);
    if (!authority.ok()) {
        return authority.status();
    }
    body.authority = authority.take();
    return body;
}

void write_attempt_started(ByteWriter& writer, const AttemptStarted& body, const Limits& limits) {
    write_identity(writer, body.attempt);
    write_entity_generation(writer, body.generation);
    write_identity(writer, body.experiment);
    write_identity(writer, body.branch);
    writer.string(body.worker_authority, limits.max_string_length);
}

Result<AttemptStarted> read_attempt_started(ByteReader& reader, const Limits& limits) {
    AttemptStarted body;
    auto attempt = reader.read_strong_id<AttemptId>();
    if (!attempt.ok()) {
        return attempt.status();
    }
    body.attempt = attempt.value();
    auto generation = reader.read_generation<AttemptGeneration>();
    if (!generation.ok()) {
        return generation.status();
    }
    body.generation = generation.value();
    auto experiment = reader.read_strong_id<ExperimentId>();
    if (!experiment.ok()) {
        return experiment.status();
    }
    body.experiment = experiment.value();
    auto branch = reader.read_strong_id<BranchId>();
    if (!branch.ok()) {
        return branch.status();
    }
    body.branch = branch.value();
    auto authority = reader.string(limits.max_string_length);
    if (!authority.ok()) {
        return authority.status();
    }
    body.worker_authority = authority.take();
    return body;
}

void write_attempt_completed(ByteWriter& writer, const AttemptCompleted& body, const Limits& limits) {
    write_identity(writer, body.attempt);
    write_entity_generation(writer, body.generation);
    writer.string(body.outcome_reference, limits.max_string_length);
}

Result<AttemptCompleted> read_attempt_completed(ByteReader& reader, const Limits& limits) {
    AttemptCompleted body;
    auto attempt = reader.read_strong_id<AttemptId>();
    if (!attempt.ok()) {
        return attempt.status();
    }
    body.attempt = attempt.value();
    auto generation = reader.read_generation<AttemptGeneration>();
    if (!generation.ok()) {
        return generation.status();
    }
    body.generation = generation.value();
    auto reference = reader.string(limits.max_string_length);
    if (!reference.ok()) {
        return reference.status();
    }
    body.outcome_reference = reference.take();
    return body;
}

void write_attempt_failed(ByteWriter& writer, const AttemptFailed& body) {
    write_identity(writer, body.attempt);
    write_entity_generation(writer, body.generation);
    write_identity(writer, body.failure);
}

Result<AttemptFailed> read_attempt_failed(ByteReader& reader) {
    AttemptFailed body;
    auto attempt = reader.read_strong_id<AttemptId>();
    if (!attempt.ok()) {
        return attempt.status();
    }
    body.attempt = attempt.value();
    auto generation = reader.read_generation<AttemptGeneration>();
    if (!generation.ok()) {
        return generation.status();
    }
    body.generation = generation.value();
    auto failure = reader.read_strong_id<FailureId>();
    if (!failure.ok()) {
        return failure.status();
    }
    body.failure = failure.value();
    return body;
}

void write_attempt_cancelled(ByteWriter& writer, const AttemptCancelled& body, const Limits& limits) {
    write_identity(writer, body.attempt);
    write_entity_generation(writer, body.generation);
    writer.string(body.reason, limits.max_string_length);
}

Result<AttemptCancelled> read_attempt_cancelled(ByteReader& reader, const Limits& limits) {
    AttemptCancelled body;
    auto attempt = reader.read_strong_id<AttemptId>();
    if (!attempt.ok()) {
        return attempt.status();
    }
    body.attempt = attempt.value();
    auto generation = reader.read_generation<AttemptGeneration>();
    if (!generation.ok()) {
        return generation.status();
    }
    body.generation = generation.value();
    auto reason = reader.string(limits.max_string_length);
    if (!reason.ok()) {
        return reason.status();
    }
    body.reason = reason.take();
    return body;
}

void write_model_call_recorded(ByteWriter& writer, const ModelCallRecorded& body,
                               const Limits& limits) {
    write_identity(writer, body.call);
    write_identity(writer, body.attempt);
    writer.string(body.model_identity, limits.max_string_length);
    writer.string(body.model_revision, limits.max_string_length);
    writer.string(body.provider, limits.max_string_length);
    writer.string(body.configuration_digest, limits.max_string_length);
    write_optional_digest(writer, body.input_reference);
    write_optional_digest(writer, body.output_reference);
    write_measure(writer, body.input_tokens);
    write_measure(writer, body.output_tokens);
    write_measure(writer, body.latency);
    write_monetary(writer, body.cost, limits);
    write_enum(writer, body.outcome);
    writer.count(static_cast<std::uint32_t>(body.parent_calls.size()), limits.max_calls_per_attempt);
    for (const ModelCallId parent : body.parent_calls) {
        write_identity(writer, parent);
    }
    writer.string(body.authority, limits.max_string_length);
}

Result<ModelCallRecorded> read_model_call_recorded(ByteReader& reader, const Limits& limits) {
    ModelCallRecorded body;
    auto call = reader.read_strong_id<ModelCallId>();
    if (!call.ok()) {
        return call.status();
    }
    body.call = call.value();
    auto attempt = reader.read_strong_id<AttemptId>();
    if (!attempt.ok()) {
        return attempt.status();
    }
    body.attempt = attempt.value();
    auto model_identity = reader.string(limits.max_string_length);
    if (!model_identity.ok()) {
        return model_identity.status();
    }
    body.model_identity = model_identity.take();
    auto model_revision = reader.string(limits.max_string_length);
    if (!model_revision.ok()) {
        return model_revision.status();
    }
    body.model_revision = model_revision.take();
    auto provider = reader.string(limits.max_string_length);
    if (!provider.ok()) {
        return provider.status();
    }
    body.provider = provider.take();
    auto configuration = reader.string(limits.max_string_length);
    if (!configuration.ok()) {
        return configuration.status();
    }
    body.configuration_digest = configuration.take();
    auto input_reference = read_optional_digest(reader);
    if (!input_reference.ok()) {
        return input_reference.status();
    }
    body.input_reference = input_reference.value();
    auto output_reference = read_optional_digest(reader);
    if (!output_reference.ok()) {
        return output_reference.status();
    }
    body.output_reference = output_reference.value();
    auto input_tokens = read_measure<InputTokenTag>(reader);
    if (!input_tokens.ok()) {
        return input_tokens.status();
    }
    body.input_tokens = input_tokens.value();
    auto output_tokens = read_measure<OutputTokenTag>(reader);
    if (!output_tokens.ok()) {
        return output_tokens.status();
    }
    body.output_tokens = output_tokens.value();
    auto latency = read_measure<WallNanosTag>(reader);
    if (!latency.ok()) {
        return latency.status();
    }
    body.latency = latency.value();
    auto cost = read_monetary(reader, limits);
    if (!cost.ok()) {
        return cost.status();
    }
    body.cost = cost.value();
    auto outcome = read_enum<ModelCallOutcome>(reader, static_cast<std::uint8_t>(ModelCallOutcome::OutcomeUnknown),
                                               "model call outcome");
    if (!outcome.ok()) {
        return outcome.status();
    }
    body.outcome = outcome.value();
    auto parent_count = reader.count(limits.max_calls_per_attempt);
    if (!parent_count.ok()) {
        return parent_count.status();
    }
    body.parent_calls.reserve(parent_count.value());
    for (std::uint32_t index = 0; index < parent_count.value(); ++index) {
        auto parent = reader.read_strong_id<ModelCallId>();
        if (!parent.ok()) {
            return parent.status();
        }
        body.parent_calls.push_back(parent.value());
    }
    auto authority = reader.string(limits.max_string_length);
    if (!authority.ok()) {
        return authority.status();
    }
    body.authority = authority.take();
    return body;
}

void write_tool_call_recorded(ByteWriter& writer, const ToolCallRecorded& body, const Limits& limits) {
    write_identity(writer, body.call);
    write_identity(writer, body.attempt);
    writer.string(body.tool_identity, limits.max_string_length);
    writer.string(body.tool_version, limits.max_string_length);
    write_optional_digest(writer, body.request_reference);
    write_optional_digest(writer, body.output_reference);
    write_enum(writer, body.state);
    write_accounting(writer, body.accounting, limits);
    writer.string(body.authority, limits.max_string_length);
}

Result<ToolCallRecorded> read_tool_call_recorded(ByteReader& reader, const Limits& limits) {
    ToolCallRecorded body;
    auto call = reader.read_strong_id<ToolCallId>();
    if (!call.ok()) {
        return call.status();
    }
    body.call = call.value();
    auto attempt = reader.read_strong_id<AttemptId>();
    if (!attempt.ok()) {
        return attempt.status();
    }
    body.attempt = attempt.value();
    auto tool_identity = reader.string(limits.max_string_length);
    if (!tool_identity.ok()) {
        return tool_identity.status();
    }
    body.tool_identity = tool_identity.take();
    auto tool_version = reader.string(limits.max_string_length);
    if (!tool_version.ok()) {
        return tool_version.status();
    }
    body.tool_version = tool_version.take();
    auto request_reference = read_optional_digest(reader);
    if (!request_reference.ok()) {
        return request_reference.status();
    }
    body.request_reference = request_reference.value();
    auto output_reference = read_optional_digest(reader);
    if (!output_reference.ok()) {
        return output_reference.status();
    }
    body.output_reference = output_reference.value();
    auto state = read_enum<ToolCallState>(reader, static_cast<std::uint8_t>(ToolCallState::OutcomeUnknown),
                                          "tool call state");
    if (!state.ok()) {
        return state.status();
    }
    body.state = state.value();
    auto accounting = read_accounting(reader, limits);
    if (!accounting.ok()) {
        return accounting.status();
    }
    body.accounting = accounting.value();
    auto authority = reader.string(limits.max_string_length);
    if (!authority.ok()) {
        return authority.status();
    }
    body.authority = authority.take();
    return body;
}

void write_artifact_referenced(ByteWriter& writer, const ArtifactReferenced& body,
                               const Limits& limits) {
    write_identity(writer, body.artifact);
    write_entity_generation(writer, body.generation);
    write_identity(writer, body.session);
    writer.digest(body.content_digest);
    write_enum(writer, body.role);
    writer.string(body.media_type, limits.max_string_length);
    write_subject(writer, body.producer);
    writer.string(body.location, limits.max_location_length);
    writer.count(static_cast<std::uint32_t>(body.parents.size()), limits.max_parents_per_artifact);
    for (const ArtifactId parent : body.parents) {
        write_identity(writer, parent);
    }
    write_enum(writer, body.validation);
    writer.string(body.authority, limits.max_string_length);
}

Result<ArtifactReferenced> read_artifact_referenced(ByteReader& reader, const Limits& limits) {
    ArtifactReferenced body;
    auto artifact = reader.read_strong_id<ArtifactId>();
    if (!artifact.ok()) {
        return artifact.status();
    }
    body.artifact = artifact.value();
    auto generation = reader.read_generation<ArtifactGeneration>();
    if (!generation.ok()) {
        return generation.status();
    }
    body.generation = generation.value();
    auto session = reader.read_strong_id<ResearchSessionId>();
    if (!session.ok()) {
        return session.status();
    }
    body.session = session.value();
    auto digest = reader.digest();
    if (!digest.ok()) {
        return digest.status();
    }
    body.content_digest = digest.value();
    auto role = read_enum<ArtifactRole>(reader, static_cast<std::uint8_t>(ArtifactRole::Other), "artifact role");
    if (!role.ok()) {
        return role.status();
    }
    body.role = role.value();
    auto media_type = reader.string(limits.max_string_length);
    if (!media_type.ok()) {
        return media_type.status();
    }
    body.media_type = media_type.take();
    auto producer = read_subject(reader);
    if (!producer.ok()) {
        return producer.status();
    }
    body.producer = producer.value();
    auto location = reader.string(limits.max_location_length);
    if (!location.ok()) {
        return location.status();
    }
    body.location = location.take();
    auto parent_count = reader.count(limits.max_parents_per_artifact);
    if (!parent_count.ok()) {
        return parent_count.status();
    }
    body.parents.reserve(parent_count.value());
    for (std::uint32_t index = 0; index < parent_count.value(); ++index) {
        auto parent = reader.read_strong_id<ArtifactId>();
        if (!parent.ok()) {
            return parent.status();
        }
        body.parents.push_back(parent.value());
    }
    auto validation = read_enum<ValidationState>(reader, static_cast<std::uint8_t>(ValidationState::Invalidated),
                                                 "validation state");
    if (!validation.ok()) {
        return validation.status();
    }
    body.validation = validation.value();
    auto authority = reader.string(limits.max_string_length);
    if (!authority.ok()) {
        return authority.status();
    }
    body.authority = authority.take();
    return body;
}

void write_artifact_invalidated(ByteWriter& writer, const ArtifactInvalidated& body,
                                const Limits& limits) {
    write_identity(writer, body.artifact);
    write_entity_generation(writer, body.generation);
    writer.boolean(body.failure.has_value());
    if (body.failure.has_value()) {
        write_identity(writer, *body.failure);
    }
    writer.boolean(body.decision.has_value());
    if (body.decision.has_value()) {
        write_identity(writer, *body.decision);
    }
    writer.string(body.reason, limits.max_string_length);
}

Result<ArtifactInvalidated> read_artifact_invalidated(ByteReader& reader, const Limits& limits) {
    ArtifactInvalidated body;
    auto artifact = reader.read_strong_id<ArtifactId>();
    if (!artifact.ok()) {
        return artifact.status();
    }
    body.artifact = artifact.value();
    auto generation = reader.read_generation<ArtifactGeneration>();
    if (!generation.ok()) {
        return generation.status();
    }
    body.generation = generation.value();
    auto has_failure = reader.boolean();
    if (!has_failure.ok()) {
        return has_failure.status();
    }
    if (has_failure.value()) {
        auto failure = reader.read_strong_id<FailureId>();
        if (!failure.ok()) {
            return failure.status();
        }
        body.failure = failure.value();
    }
    auto has_decision = reader.boolean();
    if (!has_decision.ok()) {
        return has_decision.status();
    }
    if (has_decision.value()) {
        auto decision = reader.read_strong_id<DecisionId>();
        if (!decision.ok()) {
            return decision.status();
        }
        body.decision = decision.value();
    }
    auto reason = reader.string(limits.max_string_length);
    if (!reason.ok()) {
        return reason.status();
    }
    body.reason = reason.take();
    return body;
}

void write_metric_declared(ByteWriter& writer, const MetricDeclared& body, const Limits& limits) {
    write_identity(writer, body.metric);
    write_identity(writer, body.session);
    writer.string(body.canonical_key, limits.max_string_length);
    write_enum(writer, body.unit);
    writer.u8(static_cast<std::uint8_t>(body.value_kind));
    writer.string(body.description, limits.max_string_length);
    writer.string(body.authority, limits.max_string_length);
}

Result<MetricDeclared> read_metric_declared(ByteReader& reader, const Limits& limits) {
    MetricDeclared body;
    auto metric = reader.read_strong_id<MetricId>();
    if (!metric.ok()) {
        return metric.status();
    }
    body.metric = metric.value();
    auto session = reader.read_strong_id<ResearchSessionId>();
    if (!session.ok()) {
        return session.status();
    }
    body.session = session.value();
    auto key = reader.string(limits.max_string_length);
    if (!key.ok()) {
        return key.status();
    }
    body.canonical_key = key.take();
    auto unit = read_enum<UnitKind>(reader, static_cast<std::uint8_t>(UnitKind::Custom), "unit kind");
    if (!unit.ok()) {
        return unit.status();
    }
    body.unit = unit.value();
    auto value_kind = reader.u8();
    if (!value_kind.ok()) {
        return value_kind.status();
    }
    if (value_kind.value() > static_cast<std::uint8_t>(MetricValueKind::Digest)) {
        return malformed(reader, "invalid metric value kind");
    }
    body.value_kind = static_cast<MetricValueKind>(value_kind.value());
    auto description = reader.string(limits.max_string_length);
    if (!description.ok()) {
        return description.status();
    }
    body.description = description.take();
    auto authority = reader.string(limits.max_string_length);
    if (!authority.ok()) {
        return authority.status();
    }
    body.authority = authority.take();
    return body;
}

void write_observation_recorded(ByteWriter& writer, const ObservationRecorded& body,
                                const Limits& limits) {
    write_identity(writer, body.observation);
    write_identity(writer, body.attempt);
    writer.boolean(body.metric.has_value());
    if (body.metric.has_value()) {
        write_identity(writer, *body.metric);
    }
    writer.string(body.key, limits.max_string_length);
    write_enum(writer, body.unit);
    write_metric_value(writer, body.value, limits);
    writer.i64(body.measured_at.unix_nanos);
    writer.string(body.source, limits.max_string_length);
    writer.string(body.authority, limits.max_string_length);
}

Result<ObservationRecorded> read_observation_recorded(ByteReader& reader, const Limits& limits) {
    ObservationRecorded body;
    auto observation = reader.read_strong_id<ObservationId>();
    if (!observation.ok()) {
        return observation.status();
    }
    body.observation = observation.value();
    auto attempt = reader.read_strong_id<AttemptId>();
    if (!attempt.ok()) {
        return attempt.status();
    }
    body.attempt = attempt.value();
    auto has_metric = reader.boolean();
    if (!has_metric.ok()) {
        return has_metric.status();
    }
    if (has_metric.value()) {
        auto metric = reader.read_strong_id<MetricId>();
        if (!metric.ok()) {
            return metric.status();
        }
        body.metric = metric.value();
    }
    auto key = reader.string(limits.max_string_length);
    if (!key.ok()) {
        return key.status();
    }
    body.key = key.take();
    auto unit = read_enum<UnitKind>(reader, static_cast<std::uint8_t>(UnitKind::Custom), "unit kind");
    if (!unit.ok()) {
        return unit.status();
    }
    body.unit = unit.value();
    auto value = read_metric_value(reader, limits);
    if (!value.ok()) {
        return value.status();
    }
    body.value = value.value();
    auto measured_at = reader.i64();
    if (!measured_at.ok()) {
        return measured_at.status();
    }
    body.measured_at = TimestampNs{measured_at.value()};
    auto source = reader.string(limits.max_string_length);
    if (!source.ok()) {
        return source.status();
    }
    body.source = source.take();
    auto authority = reader.string(limits.max_string_length);
    if (!authority.ok()) {
        return authority.status();
    }
    body.authority = authority.take();
    return body;
}

void write_failure_recorded(ByteWriter& writer, const FailureRecorded& body, const Limits& limits) {
    write_identity(writer, body.failure);
    write_subject(writer, body.scope);
    write_enum(writer, body.category);
    writer.string(body.message, limits.max_failure_message_length);
    writer.boolean(body.retriable);
    writer.boolean(body.terminal);
    writer.boolean(body.predecessor.has_value());
    if (body.predecessor.has_value()) {
        write_identity(writer, *body.predecessor);
    }
    writer.boolean(body.recovery_attempt.has_value());
    if (body.recovery_attempt.has_value()) {
        write_identity(writer, *body.recovery_attempt);
    }
    writer.string(body.authority, limits.max_string_length);
}

Result<FailureRecorded> read_failure_recorded(ByteReader& reader, const Limits& limits) {
    FailureRecorded body;
    auto failure = reader.read_strong_id<FailureId>();
    if (!failure.ok()) {
        return failure.status();
    }
    body.failure = failure.value();
    auto scope = read_subject(reader);
    if (!scope.ok()) {
        return scope.status();
    }
    body.scope = scope.value();
    auto category = read_enum<FailureCategory>(reader, static_cast<std::uint8_t>(FailureCategory::Unknown),
                                               "failure category");
    if (!category.ok()) {
        return category.status();
    }
    body.category = category.value();
    auto message = reader.string(limits.max_failure_message_length);
    if (!message.ok()) {
        return message.status();
    }
    body.message = message.take();
    auto retriable = reader.boolean();
    if (!retriable.ok()) {
        return retriable.status();
    }
    body.retriable = retriable.value();
    auto terminal = reader.boolean();
    if (!terminal.ok()) {
        return terminal.status();
    }
    body.terminal = terminal.value();
    auto has_predecessor = reader.boolean();
    if (!has_predecessor.ok()) {
        return has_predecessor.status();
    }
    if (has_predecessor.value()) {
        auto predecessor = reader.read_strong_id<FailureId>();
        if (!predecessor.ok()) {
            return predecessor.status();
        }
        body.predecessor = predecessor.value();
    }
    auto has_recovery = reader.boolean();
    if (!has_recovery.ok()) {
        return has_recovery.status();
    }
    if (has_recovery.value()) {
        auto recovery = reader.read_strong_id<AttemptId>();
        if (!recovery.ok()) {
            return recovery.status();
        }
        body.recovery_attempt = recovery.value();
    }
    auto authority = reader.string(limits.max_string_length);
    if (!authority.ok()) {
        return authority.status();
    }
    body.authority = authority.take();
    return body;
}

void write_decision_recorded(ByteWriter& writer, const DecisionRecorded& body, const Limits& limits) {
    write_identity(writer, body.decision);
    write_identity(writer, body.session);
    write_subject(writer, body.subject);
    write_enum(writer, body.type);
    write_enum(writer, body.outcome);
    writer.string(body.policy_identity, limits.max_string_length);
    writer.boolean(body.policy_generation.valid());
    if (body.policy_generation.valid()) {
        write_generation(writer, IdentityDomain::PolicyGeneration, body.policy_generation.value());
    }
    writer.count(static_cast<std::uint32_t>(body.evidence.size()), limits.max_evidence_refs_per_decision);
    for (const EvidenceRef& evidence : body.evidence) {
        write_evidence(writer, evidence);
    }
    writer.string(body.explanation, limits.max_explanation_length);
    writer.string(body.authority, limits.max_string_length);
}

Result<DecisionRecorded> read_decision_recorded(ByteReader& reader, const Limits& limits) {
    DecisionRecorded body;
    auto decision = reader.read_strong_id<DecisionId>();
    if (!decision.ok()) {
        return decision.status();
    }
    body.decision = decision.value();
    auto session = reader.read_strong_id<ResearchSessionId>();
    if (!session.ok()) {
        return session.status();
    }
    body.session = session.value();
    auto subject = read_subject(reader);
    if (!subject.ok()) {
        return subject.status();
    }
    body.subject = subject.value();
    auto type = read_enum<DecisionType>(reader, static_cast<std::uint8_t>(DecisionType::Audit), "decision type");
    if (!type.ok()) {
        return type.status();
    }
    body.type = type.value();
    auto outcome = read_enum<DecisionOutcome>(reader, static_cast<std::uint8_t>(DecisionOutcome::Recorded),
                                              "decision outcome");
    if (!outcome.ok()) {
        return outcome.status();
    }
    body.outcome = outcome.value();
    auto policy = reader.string(limits.max_string_length);
    if (!policy.ok()) {
        return policy.status();
    }
    body.policy_identity = policy.take();
    auto has_policy_generation = reader.boolean();
    if (!has_policy_generation.ok()) {
        return has_policy_generation.status();
    }
    if (has_policy_generation.value()) {
        auto generation = reader.generation_value(IdentityDomain::PolicyGeneration);
        if (!generation.ok()) {
            return generation.status();
        }
        if (generation.value() == 0) {
            return malformed(reader, "policy generation zero is never valid");
        }
        body.policy_generation = PolicyGeneration::from_value(generation.value());
    }
    auto evidence_count = reader.count(limits.max_evidence_refs_per_decision);
    if (!evidence_count.ok()) {
        return evidence_count.status();
    }
    body.evidence.reserve(evidence_count.value());
    for (std::uint32_t index = 0; index < evidence_count.value(); ++index) {
        auto evidence = read_evidence(reader);
        if (!evidence.ok()) {
            return evidence.status();
        }
        body.evidence.push_back(evidence.value());
    }
    auto explanation = reader.string(limits.max_explanation_length);
    if (!explanation.ok()) {
        return explanation.status();
    }
    body.explanation = explanation.take();
    auto authority = reader.string(limits.max_string_length);
    if (!authority.ok()) {
        return authority.status();
    }
    body.authority = authority.take();
    return body;
}

void write_result_declared(ByteWriter& writer, const ResultDeclared& body, const Limits& limits) {
    write_identity(writer, body.result);
    write_entity_generation(writer, body.generation);
    write_identity(writer, body.session);
    writer.string(body.summary, limits.max_string_length);
    writer.count(static_cast<std::uint32_t>(body.hypotheses.size()), limits.max_hypotheses_per_experiment);
    for (const HypothesisId hypothesis : body.hypotheses) {
        write_identity(writer, hypothesis);
    }
    writer.count(static_cast<std::uint32_t>(body.experiments.size()), limits.max_experiments_per_result);
    for (const ExperimentId experiment : body.experiments) {
        write_identity(writer, experiment);
    }
    writer.count(static_cast<std::uint32_t>(body.artifacts.size()), limits.max_artifacts_per_result);
    for (const ArtifactId artifact : body.artifacts) {
        write_identity(writer, artifact);
    }
    writer.count(static_cast<std::uint32_t>(body.observations.size()), limits.max_observations_per_result);
    for (const ObservationId observation : body.observations) {
        write_identity(writer, observation);
    }
    writer.count(static_cast<std::uint32_t>(body.model_calls.size()), limits.max_calls_per_attempt);
    for (const ModelCallId call : body.model_calls) {
        write_identity(writer, call);
    }
    writer.count(static_cast<std::uint32_t>(body.tool_calls.size()), limits.max_calls_per_attempt);
    for (const ToolCallId call : body.tool_calls) {
        write_identity(writer, call);
    }
    write_optional_digest(writer, body.content_digest);
    writer.string(body.authority, limits.max_string_length);
}

Result<ResultDeclared> read_result_declared(ByteReader& reader, const Limits& limits) {
    ResultDeclared body;
    auto result = reader.read_strong_id<ResultId>();
    if (!result.ok()) {
        return result.status();
    }
    body.result = result.value();
    auto generation = reader.read_generation<ResultGeneration>();
    if (!generation.ok()) {
        return generation.status();
    }
    body.generation = generation.value();
    auto session = reader.read_strong_id<ResearchSessionId>();
    if (!session.ok()) {
        return session.status();
    }
    body.session = session.value();
    auto summary = reader.string(limits.max_string_length);
    if (!summary.ok()) {
        return summary.status();
    }
    body.summary = summary.take();

    auto hypothesis_count = reader.count(limits.max_hypotheses_per_experiment);
    if (!hypothesis_count.ok()) {
        return hypothesis_count.status();
    }
    body.hypotheses.reserve(hypothesis_count.value());
    for (std::uint32_t index = 0; index < hypothesis_count.value(); ++index) {
        auto hypothesis = reader.read_strong_id<HypothesisId>();
        if (!hypothesis.ok()) {
            return hypothesis.status();
        }
        body.hypotheses.push_back(hypothesis.value());
    }

    auto experiment_count = reader.count(limits.max_experiments_per_result);
    if (!experiment_count.ok()) {
        return experiment_count.status();
    }
    body.experiments.reserve(experiment_count.value());
    for (std::uint32_t index = 0; index < experiment_count.value(); ++index) {
        auto experiment = reader.read_strong_id<ExperimentId>();
        if (!experiment.ok()) {
            return experiment.status();
        }
        body.experiments.push_back(experiment.value());
    }

    auto artifact_count = reader.count(limits.max_artifacts_per_result);
    if (!artifact_count.ok()) {
        return artifact_count.status();
    }
    body.artifacts.reserve(artifact_count.value());
    for (std::uint32_t index = 0; index < artifact_count.value(); ++index) {
        auto artifact = reader.read_strong_id<ArtifactId>();
        if (!artifact.ok()) {
            return artifact.status();
        }
        body.artifacts.push_back(artifact.value());
    }

    auto observation_count = reader.count(limits.max_observations_per_result);
    if (!observation_count.ok()) {
        return observation_count.status();
    }
    body.observations.reserve(observation_count.value());
    for (std::uint32_t index = 0; index < observation_count.value(); ++index) {
        auto observation = reader.read_strong_id<ObservationId>();
        if (!observation.ok()) {
            return observation.status();
        }
        body.observations.push_back(observation.value());
    }

    auto model_call_count = reader.count(limits.max_calls_per_attempt);
    if (!model_call_count.ok()) {
        return model_call_count.status();
    }
    body.model_calls.reserve(model_call_count.value());
    for (std::uint32_t index = 0; index < model_call_count.value(); ++index) {
        auto call = reader.read_strong_id<ModelCallId>();
        if (!call.ok()) {
            return call.status();
        }
        body.model_calls.push_back(call.value());
    }

    auto tool_call_count = reader.count(limits.max_calls_per_attempt);
    if (!tool_call_count.ok()) {
        return tool_call_count.status();
    }
    body.tool_calls.reserve(tool_call_count.value());
    for (std::uint32_t index = 0; index < tool_call_count.value(); ++index) {
        auto call = reader.read_strong_id<ToolCallId>();
        if (!call.ok()) {
            return call.status();
        }
        body.tool_calls.push_back(call.value());
    }

    auto content_digest = read_optional_digest(reader);
    if (!content_digest.ok()) {
        return content_digest.status();
    }
    body.content_digest = content_digest.value();
    auto authority = reader.string(limits.max_string_length);
    if (!authority.ok()) {
        return authority.status();
    }
    body.authority = authority.take();
    return body;
}

void write_result_status_changed(ByteWriter& writer, const ResultStatusChanged& body) {
    write_identity(writer, body.result);
    write_entity_generation(writer, body.generation);
    write_enum(writer, body.from);
    write_enum(writer, body.to);
    write_identity(writer, body.decision);
}

Result<ResultStatusChanged> read_result_status_changed(ByteReader& reader) {
    ResultStatusChanged body;
    auto result = reader.read_strong_id<ResultId>();
    if (!result.ok()) {
        return result.status();
    }
    body.result = result.value();
    auto generation = reader.read_generation<ResultGeneration>();
    if (!generation.ok()) {
        return generation.status();
    }
    body.generation = generation.value();
    auto from = read_enum<ResultStatus>(reader, static_cast<std::uint8_t>(ResultStatus::Retracted),
                                        "result status");
    if (!from.ok()) {
        return from.status();
    }
    body.from = from.value();
    auto to = read_enum<ResultStatus>(reader, static_cast<std::uint8_t>(ResultStatus::Retracted),
                                      "result status");
    if (!to.ok()) {
        return to.status();
    }
    body.to = to.value();
    auto decision = reader.read_strong_id<DecisionId>();
    if (!decision.ok()) {
        return decision.status();
    }
    body.decision = decision.value();
    return body;
}

void write_accounting_recorded(ByteWriter& writer, const AccountingRecorded& body,
                               const Limits& limits) {
    write_subject(writer, body.scope);
    write_accounting(writer, body.accounting, limits);
    writer.string(body.source, limits.max_string_length);
    writer.string(body.authority, limits.max_string_length);
}

Result<AccountingRecorded> read_accounting_recorded(ByteReader& reader, const Limits& limits) {
    AccountingRecorded body;
    auto scope = read_subject(reader);
    if (!scope.ok()) {
        return scope.status();
    }
    body.scope = scope.value();
    auto accounting = read_accounting(reader, limits);
    if (!accounting.ok()) {
        return accounting.status();
    }
    body.accounting = accounting.value();
    auto source = reader.string(limits.max_string_length);
    if (!source.ok()) {
        return source.status();
    }
    body.source = source.take();
    auto authority = reader.string(limits.max_string_length);
    if (!authority.ok()) {
        return authority.status();
    }
    body.authority = authority.take();
    return body;
}

}  // namespace

Result<std::vector<std::byte>> encode_record_body(const RecordBody& body, const Limits& limits) {
    ByteWriter writer(512);
    writer.u16(static_cast<std::uint16_t>(record_type_of(body)));
    std::visit(
        [&](const auto& payload) {
            using Payload = std::decay_t<decltype(payload)>;
            if constexpr (std::is_same_v<Payload, SessionOpened>) {
                write_session_opened(writer, payload, limits);
            } else if constexpr (std::is_same_v<Payload, SessionClosed>) {
                write_session_closed(writer, payload, limits);
            } else if constexpr (std::is_same_v<Payload, SessionAnnotation>) {
                write_session_annotation(writer, payload, limits);
            } else if constexpr (std::is_same_v<Payload, HypothesisDeclared>) {
                write_hypothesis_declared(writer, payload, limits);
            } else if constexpr (std::is_same_v<Payload, HypothesisStatusChanged>) {
                write_hypothesis_status_changed(writer, payload, limits);
            } else if constexpr (std::is_same_v<Payload, ExperimentDeclared>) {
                write_experiment_declared(writer, payload, limits);
            } else if constexpr (std::is_same_v<Payload, BranchDeclared>) {
                write_branch_declared(writer, payload, limits);
            } else if constexpr (std::is_same_v<Payload, AttemptStarted>) {
                write_attempt_started(writer, payload, limits);
            } else if constexpr (std::is_same_v<Payload, AttemptCompleted>) {
                write_attempt_completed(writer, payload, limits);
            } else if constexpr (std::is_same_v<Payload, AttemptFailed>) {
                write_attempt_failed(writer, payload);
            } else if constexpr (std::is_same_v<Payload, AttemptCancelled>) {
                write_attempt_cancelled(writer, payload, limits);
            } else if constexpr (std::is_same_v<Payload, ModelCallRecorded>) {
                write_model_call_recorded(writer, payload, limits);
            } else if constexpr (std::is_same_v<Payload, ToolCallRecorded>) {
                write_tool_call_recorded(writer, payload, limits);
            } else if constexpr (std::is_same_v<Payload, ArtifactReferenced>) {
                write_artifact_referenced(writer, payload, limits);
            } else if constexpr (std::is_same_v<Payload, ArtifactInvalidated>) {
                write_artifact_invalidated(writer, payload, limits);
            } else if constexpr (std::is_same_v<Payload, MetricDeclared>) {
                write_metric_declared(writer, payload, limits);
            } else if constexpr (std::is_same_v<Payload, ObservationRecorded>) {
                write_observation_recorded(writer, payload, limits);
            } else if constexpr (std::is_same_v<Payload, FailureRecorded>) {
                write_failure_recorded(writer, payload, limits);
            } else if constexpr (std::is_same_v<Payload, DecisionRecorded>) {
                write_decision_recorded(writer, payload, limits);
            } else if constexpr (std::is_same_v<Payload, ResultDeclared>) {
                write_result_declared(writer, payload, limits);
            } else if constexpr (std::is_same_v<Payload, ResultStatusChanged>) {
                write_result_status_changed(writer, payload);
            } else if constexpr (std::is_same_v<Payload, AccountingRecorded>) {
                write_accounting_recorded(writer, payload, limits);
            }
        },
        body);
    if (!writer.ok()) {
        return writer.status();
    }
    return writer.take();
}

Result<RecordBody> decode_record_body(std::span<const std::byte> payload, const Limits& limits) {
    ByteReader reader(payload, limits, ErrorCode::PersistenceCorrupt);
    auto raw_type = reader.u16();
    if (!raw_type.ok()) {
        return raw_type.status();
    }
    if (raw_type.value() == 0 || raw_type.value() > kRecordTypeCount) {
        return malformed(reader, "invalid record type");
    }
    const auto type = static_cast<RecordType>(raw_type.value());

#define RESEARCH_LEDGER_DECODE_CASE(TYPE_ENUM, PAYLOAD)                       \
    case RecordType::TYPE_ENUM: {                                             \
        auto body = read_##PAYLOAD(reader, limits);                           \
        if (!body.ok()) {                                                     \
            return body.status();                                             \
        }                                                                     \
        RecordBody decoded = body.take();                                     \
        Status consumed = reader.expect_consumed(record_type_name(type));     \
        if (!consumed.ok()) {                                                 \
            return consumed;                                                  \
        }                                                                     \
        return decoded;                                                       \
    }

    switch (type) {
        RESEARCH_LEDGER_DECODE_CASE(SessionOpened, session_opened)
        RESEARCH_LEDGER_DECODE_CASE(SessionClosed, session_closed)
        RESEARCH_LEDGER_DECODE_CASE(SessionAnnotation, session_annotation)
        RESEARCH_LEDGER_DECODE_CASE(HypothesisDeclared, hypothesis_declared)
        RESEARCH_LEDGER_DECODE_CASE(HypothesisStatusChanged, hypothesis_status_changed)
        RESEARCH_LEDGER_DECODE_CASE(ExperimentDeclared, experiment_declared)
        RESEARCH_LEDGER_DECODE_CASE(BranchDeclared, branch_declared)
        RESEARCH_LEDGER_DECODE_CASE(AttemptStarted, attempt_started)
        RESEARCH_LEDGER_DECODE_CASE(AttemptCompleted, attempt_completed)
        case RecordType::AttemptFailed: {
            auto body = read_attempt_failed(reader);
            if (!body.ok()) {
                return body.status();
            }
            RecordBody decoded = body.take();
            Status consumed = reader.expect_consumed(record_type_name(type));
            if (!consumed.ok()) {
                return consumed;
            }
            return decoded;
        }
        RESEARCH_LEDGER_DECODE_CASE(AttemptCancelled, attempt_cancelled)
        RESEARCH_LEDGER_DECODE_CASE(ModelCallRecorded, model_call_recorded)
        RESEARCH_LEDGER_DECODE_CASE(ToolCallRecorded, tool_call_recorded)
        RESEARCH_LEDGER_DECODE_CASE(ArtifactReferenced, artifact_referenced)
        RESEARCH_LEDGER_DECODE_CASE(ArtifactInvalidated, artifact_invalidated)
        RESEARCH_LEDGER_DECODE_CASE(MetricDeclared, metric_declared)
        RESEARCH_LEDGER_DECODE_CASE(ObservationRecorded, observation_recorded)
        RESEARCH_LEDGER_DECODE_CASE(FailureRecorded, failure_recorded)
        RESEARCH_LEDGER_DECODE_CASE(DecisionRecorded, decision_recorded)
        RESEARCH_LEDGER_DECODE_CASE(ResultDeclared, result_declared)
        case RecordType::ResultStatusChanged: {
            auto body = read_result_status_changed(reader);
            if (!body.ok()) {
                return body.status();
            }
            RecordBody decoded = body.take();
            Status consumed = reader.expect_consumed(record_type_name(type));
            if (!consumed.ok()) {
                return consumed;
            }
            return decoded;
        }
        RESEARCH_LEDGER_DECODE_CASE(AccountingRecorded, accounting_recorded)
    }

#undef RESEARCH_LEDGER_DECODE_CASE

    return malformed(reader, "unreachable record type");
}

Result<std::vector<std::byte>> encode_draft(const RecordDraft& draft, const Limits& limits) {
    ByteWriter writer(512);
    write_enum(writer, draft.provenance);
    writer.boolean(draft.idempotent);
    write_strong_id(writer, IdentityDomain::Record, draft.record_id.value());
    auto body = encode_record_body(draft.body, limits);
    if (!body.ok()) {
        return body.status();
    }
    writer.bytes(body.value());
    if (!writer.ok()) {
        return writer.status();
    }
    return writer.take();
}

Result<RecordDraft> decode_draft(std::span<const std::byte> payload, const Limits& limits) {
    ByteReader reader(payload, limits, ErrorCode::ProtocolError);
    RecordDraft draft;
    auto provenance = read_enum<Provenance>(reader, static_cast<std::uint8_t>(Provenance::Reconstructed),
                                            "provenance");
    if (!provenance.ok()) {
        return provenance.status();
    }
    draft.provenance = provenance.value();
    auto idempotent = reader.boolean();
    if (!idempotent.ok()) {
        return idempotent.status();
    }
    draft.idempotent = idempotent.value();
    auto record_id = reader.identity_value(IdentityDomain::Record);
    if (!record_id.ok()) {
        return record_id.status();
    }
    draft.record_id = LedgerRecordId::from_value(record_id.value());
    auto remaining = reader.bytes(reader.remaining());
    if (!remaining.ok()) {
        return remaining.status();
    }
    auto body = decode_record_body(remaining.value(), limits);
    if (!body.ok()) {
        return body.status();
    }
    draft.body = body.take();
    return draft;
}

Result<std::vector<std::byte>> encode_record(const Record& record, const Limits& limits) {
    ByteWriter writer(512);
    write_generation(writer, IdentityDomain::RecordSequence, record.header.sequence.value());
    writer.u16(static_cast<std::uint16_t>(record.header.type));
    write_identity(writer, record.header.record_id);
    write_generation(writer, IdentityDomain::LedgerGeneration, record.header.authority.ledger.value());
    write_generation(writer, IdentityDomain::CoordinatorEpoch, record.header.authority.epoch.value());
    write_identity(writer, record.header.authority.worker);
    write_identity(writer, record.header.authority.worker_boot);
    writer.i64(record.header.committed_at.unix_nanos);
    write_enum(writer, record.header.provenance);
    writer.digest(record.header.payload_digest);
    writer.digest(record.header.chain_digest);
    auto body = encode_record_body(record.body, limits);
    if (!body.ok()) {
        return body.status();
    }
    writer.bytes(body.value());
    if (!writer.ok()) {
        return writer.status();
    }
    return writer.take();
}

Result<Record> decode_record(std::span<const std::byte> payload, const Limits& limits) {
    ByteReader reader(payload, limits, ErrorCode::PersistenceCorrupt);
    Record record;
    auto sequence = reader.generation_value(IdentityDomain::RecordSequence);
    if (!sequence.ok()) {
        return sequence.status();
    }
    if (sequence.value() == 0) {
        return malformed(reader, "record sequence zero is never valid");
    }
    record.header.sequence = RecordSequence::from_value(sequence.value());
    auto type = reader.u16();
    if (!type.ok()) {
        return type.status();
    }
    if (type.value() == 0 || type.value() > kRecordTypeCount) {
        return malformed(reader, "invalid record type");
    }
    record.header.type = static_cast<RecordType>(type.value());
    auto record_id = reader.read_strong_id<LedgerRecordId>();
    if (!record_id.ok()) {
        return record_id.status();
    }
    record.header.record_id = record_id.value();
    auto ledger = reader.generation_value(IdentityDomain::LedgerGeneration);
    if (!ledger.ok()) {
        return ledger.status();
    }
    record.header.authority.ledger = LedgerGeneration::from_value(ledger.value());
    auto epoch = reader.generation_value(IdentityDomain::CoordinatorEpoch);
    if (!epoch.ok()) {
        return epoch.status();
    }
    record.header.authority.epoch = CoordinatorEpoch::from_value(epoch.value());
    auto worker = reader.read_strong_id<WorkerId>();
    if (!worker.ok()) {
        return worker.status();
    }
    record.header.authority.worker = worker.value();
    auto boot = reader.read_strong_id<WorkerBootId>();
    if (!boot.ok()) {
        return boot.status();
    }
    record.header.authority.worker_boot = boot.value();
    auto committed_at = reader.i64();
    if (!committed_at.ok()) {
        return committed_at.status();
    }
    record.header.committed_at = TimestampNs{committed_at.value()};
    auto provenance = read_enum<Provenance>(reader, static_cast<std::uint8_t>(Provenance::Reconstructed),
                                            "provenance");
    if (!provenance.ok()) {
        return provenance.status();
    }
    record.header.provenance = provenance.value();
    auto payload_digest = reader.digest();
    if (!payload_digest.ok()) {
        return payload_digest.status();
    }
    record.header.payload_digest = payload_digest.value();
    auto chain = reader.digest();
    if (!chain.ok()) {
        return chain.status();
    }
    record.header.chain_digest = chain.value();
    auto remaining = reader.bytes(reader.remaining());
    if (!remaining.ok()) {
        return remaining.status();
    }
    auto body = decode_record_body(remaining.value(), limits);
    if (!body.ok()) {
        return body.status();
    }
    record.body = body.take();
    if (record_type_of(record.body) != record.header.type) {
        return Status(ErrorCode::PersistenceCorrupt, "record type does not match the decoded body");
    }
    if (record.header.authority.ledger.value() == 0 || record.header.authority.epoch.value() == 0) {
        return Status(ErrorCode::PersistenceCorrupt, "record carries an invalid authority generation");
    }
    return record;
}

Result<Digest> record_body_digest(const RecordBody& body, const Limits& limits) {
    auto encoded = encode_record_body(body, limits);
    if (!encoded.ok()) {
        return encoded.status();
    }
    return sha256(encoded.value());
}

}  // namespace research_ledger
