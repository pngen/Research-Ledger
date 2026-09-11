#include "research_ledger/protocol.hpp"

#include <array>
#include <utility>

#include "research_ledger/codec.hpp"
#include "research_ledger/version.hpp"

namespace research_ledger {
namespace {

// CRC-32 (IEEE 802.3), reflected, with the standard polynomial.
constexpr std::uint32_t kCrc32Polynomial = 0xedb88320u;

const std::array<std::uint32_t, 256>& crc32_table() {
    static const std::array<std::uint32_t, 256> table = []() {
        std::array<std::uint32_t, 256> values{};
        for (std::uint32_t index = 0; index < 256; ++index) {
            std::uint32_t value = index;
            for (int bit = 0; bit < 8; ++bit) {
                value = (value & 1u) != 0u ? (value >> 1) ^ kCrc32Polynomial : value >> 1;
            }
            values[index] = value;
        }
        return values;
    }();
    return table;
}

std::optional<MessageType> decoded_type(std::uint16_t raw) noexcept {
    if (raw == 0 || raw > static_cast<std::uint16_t>(MessageType::ErrorReply)) {
        return std::nullopt;
    }
    return static_cast<MessageType>(raw);
}

Status require_text(const std::string& text, std::uint32_t maximum, const char* what) {
    if (text.size() > maximum) {
        return Status(ErrorCode::LimitExceeded, std::string(what) + " exceeds the configured maximum");
    }
    return Status{};
}

}  // namespace

std::string_view message_type_name(MessageType type) noexcept {
    switch (type) {
        case MessageType::Hello:
            return "HELLO";
        case MessageType::HelloAck:
            return "HELLO_ACK";
        case MessageType::AppendRequest:
            return "APPEND_REQUEST";
        case MessageType::AppendReply:
            return "APPEND_REPLY";
        case MessageType::StatusRequest:
            return "STATUS_REQUEST";
        case MessageType::StatusReply:
            return "STATUS_REPLY";
        case MessageType::ShutdownRequest:
            return "SHUTDOWN_REQUEST";
        case MessageType::ShutdownReply:
            return "SHUTDOWN_REPLY";
        case MessageType::ErrorReply:
            return "ERROR_REPLY";
    }
    return "UNKNOWN";
}

std::optional<MessageType> parse_message_type(std::string_view text) noexcept {
    for (std::uint16_t raw = 1; raw <= static_cast<std::uint16_t>(MessageType::ErrorReply); ++raw) {
        const auto type = static_cast<MessageType>(raw);
        if (message_type_name(type) == text) {
            return type;
        }
    }
    return std::nullopt;
}

std::uint32_t crc32(std::span<const std::byte> bytes) noexcept {
    const std::array<std::uint32_t, 256>& table = crc32_table();
    std::uint32_t crc = 0xffffffffu;
    for (const std::byte byte : bytes) {
        crc = table[(crc ^ static_cast<std::uint32_t>(byte)) & 0xffu] ^ (crc >> 8);
    }
    return crc ^ 0xffffffffu;
}

Result<std::vector<std::byte>> encode_frame(MessageType type, std::uint64_t correlation_id,
                                            const AuthorityEnvelope& authority,
                                            std::span<const std::byte> payload,
                                            const Limits& limits) {
    if (payload.size() > limits.max_frame_size) {
        return Status(ErrorCode::PayloadTooLarge, "frame payload exceeds the configured maximum");
    }
    ByteWriter writer(kFrameHeaderSize + payload.size());
    writer.u32(kFrameMagic);
    writer.u16(kProtocolVersion);
    writer.u16(static_cast<std::uint16_t>(type));
    writer.u16(0);
    writer.u32(static_cast<std::uint32_t>(payload.size()));
    writer.u64(correlation_id);
    writer.u32(authority.epoch.value());
    writer.u64(authority.worker.value());
    writer.u64(authority.worker_boot.value());
    if (!writer.ok() || writer.size() != kFrameHeaderSize - 4) {
        return Status(ErrorCode::InternalError, "frame header layout is inconsistent");
    }
    const std::uint32_t checksum = crc32(writer.buffer());
    writer.u32(checksum);
    writer.bytes(payload);
    if (!writer.ok() || writer.size() != kFrameHeaderSize + payload.size()) {
        return Status(ErrorCode::InternalError, "frame layout is inconsistent");
    }
    return writer.take();
}

Result<FrameView> decode_frame(std::span<const std::byte> frame, const Limits& limits) {
    if (frame.size() < kFrameHeaderSize) {
        return Status(ErrorCode::ProtocolError, "frame is shorter than its header");
    }
    ByteReader reader(frame, limits, ErrorCode::ProtocolError);
    auto magic = reader.u32();
    if (!magic.ok()) {
        return magic.status();
    }
    if (magic.value() != kFrameMagic) {
        return Status(ErrorCode::ProtocolError, "frame magic does not match");
    }
    auto version = reader.u16();
    auto raw_type = reader.u16();
    auto flags = reader.u16();
    auto payload_length = reader.u32();
    auto correlation = reader.u64();
    auto epoch = reader.u32();
    auto worker = reader.u64();
    auto boot = reader.u64();
    if (!version.ok() || !raw_type.ok() || !flags.ok() || !payload_length.ok() ||
        !correlation.ok() || !epoch.ok() || !worker.ok() || !boot.ok()) {
        return Status(ErrorCode::ProtocolError, "frame header is truncated");
    }
    const std::span<const std::byte> header = frame.subspan(0, kFrameHeaderSize - 4);
    auto checksum = reader.u32();
    if (!checksum.ok()) {
        return checksum.status();
    }
    const std::optional<MessageType> type = decoded_type(raw_type.value());
    if (!type.has_value()) {
        return Status(ErrorCode::ProtocolError, "frame carries an unknown message type");
    }
    if (version.value() != kProtocolVersion) {
        return Status(ErrorCode::ProtocolError, "frame protocol version is not supported");
    }
    if (flags.value() != 0) {
        return Status(ErrorCode::ProtocolError, "frame flags must be zero");
    }
    if (payload_length.value() > limits.max_frame_size) {
        return Status(ErrorCode::PayloadTooLarge,
                      "frame declares a payload larger than the configured maximum");
    }
    if (frame.size() != kFrameHeaderSize + payload_length.value()) {
        return Status(ErrorCode::ProtocolError, "frame length does not match the declared payload");
    }
    if (crc32(header) != checksum.value()) {
        return Status(ErrorCode::IntegrityFailure, "frame checksum does not match");
    }
    FrameView view;
    view.type = *type;
    view.correlation_id = correlation.value();
    view.epoch = CoordinatorEpoch::from_value(epoch.value());
    view.worker = WorkerId::from_value(worker.value());
    view.worker_boot = WorkerBootId::from_value(boot.value());
    view.payload = frame.subspan(kFrameHeaderSize, payload_length.value());
    return view;
}

Result<std::uint32_t> frame_payload_length(std::span<const std::byte> header, const Limits& limits) {
    if (header.size() != kFrameHeaderSize) {
        return Status(ErrorCode::ProtocolError, "frame header is not a complete header");
    }
    ByteReader reader(header, limits, ErrorCode::ProtocolError);
    auto magic = reader.u32();
    if (!magic.ok()) {
        return magic.status();
    }
    if (magic.value() != kFrameMagic) {
        return Status(ErrorCode::ProtocolError, "frame magic does not match");
    }
    auto version = reader.u16();
    if (!version.ok()) {
        return version.status();
    }
    if (version.value() != kProtocolVersion) {
        return Status(ErrorCode::ProtocolError, "frame protocol version is not supported");
    }
    auto type = reader.u16();
    if (!type.ok()) {
        return type.status();
    }
    if (!decoded_type(type.value()).has_value()) {
        return Status(ErrorCode::ProtocolError, "frame carries an unknown message type");
    }
    auto flags = reader.u16();
    if (!flags.ok()) {
        return flags.status();
    }
    if (flags.value() != 0) {
        return Status(ErrorCode::ProtocolError, "frame flags must be zero");
    }
    auto payload_length = reader.u32();
    if (!payload_length.ok()) {
        return payload_length.status();
    }
    if (payload_length.value() > limits.max_frame_size) {
        return Status(ErrorCode::PayloadTooLarge,
                      "frame declares a payload larger than the configured maximum");
    }
    return payload_length.value();
}

Result<std::vector<std::byte>> encode_hello(const HelloMessage& message, const Limits& limits) {
    ByteWriter writer(64);
    write_identity(writer, message.worker);
    write_identity(writer, message.worker_boot);
    writer.string(message.authority, limits.max_string_length);
    if (!writer.ok()) {
        return writer.status();
    }
    return writer.take();
}

Result<HelloMessage> decode_hello(std::span<const std::byte> payload, const Limits& limits) {
    ByteReader reader(payload, limits, ErrorCode::ProtocolError);
    HelloMessage message;
    auto worker = reader.read_strong_id<WorkerId>();
    if (!worker.ok()) {
        return worker.status();
    }
    message.worker = worker.value();
    // The previous boot identity is optional: a worker that has never been
    // admitted sends none, and zero is never a valid boot identity.
    auto boot = reader.optional_identity_value(IdentityDomain::WorkerBoot);
    if (!boot.ok()) {
        return boot.status();
    }
    message.worker_boot = WorkerBootId::from_value(boot.value());
    auto authority = reader.string(limits.max_string_length);
    if (!authority.ok()) {
        return authority.status();
    }
    message.authority = authority.take();
    Status consumed = reader.expect_consumed("hello");
    if (!consumed.ok()) {
        return consumed;
    }
    return message;
}

Result<std::vector<std::byte>> encode_hello_ack(const HelloAckMessage& message, const Limits& limits) {
    ByteWriter writer(64);
    write_generation(writer, IdentityDomain::CoordinatorEpoch, message.epoch.value());
    write_generation(writer, IdentityDomain::LedgerGeneration, message.generation.value());
    write_identity(writer, message.worker_boot);
    writer.string(message.coordinator, limits.max_string_length);
    writer.u16(static_cast<std::uint16_t>(message.code));
    writer.string(message.detail, limits.max_string_length);
    if (!writer.ok()) {
        return writer.status();
    }
    return writer.take();
}

Result<HelloAckMessage> decode_hello_ack(std::span<const std::byte> payload, const Limits& limits) {
    ByteReader reader(payload, limits, ErrorCode::ProtocolError);
    HelloAckMessage message;
    auto epoch = reader.generation_value(IdentityDomain::CoordinatorEpoch);
    if (!epoch.ok()) {
        return epoch.status();
    }
    message.epoch = CoordinatorEpoch::from_value(epoch.value());
    auto generation = reader.generation_value(IdentityDomain::LedgerGeneration);
    if (!generation.ok()) {
        return generation.status();
    }
    message.generation = LedgerGeneration::from_value(generation.value());
    auto boot = reader.read_strong_id<WorkerBootId>();
    if (!boot.ok()) {
        return boot.status();
    }
    message.worker_boot = boot.value();
    auto coordinator = reader.string(limits.max_string_length);
    if (!coordinator.ok()) {
        return coordinator.status();
    }
    message.coordinator = coordinator.take();
    auto code = reader.u16();
    if (!code.ok()) {
        return code.status();
    }
    if (code.value() > static_cast<std::uint16_t>(ErrorCode::Busy)) {
        return Status(ErrorCode::ProtocolError, "hello acknowledgement carries an unknown status");
    }
    message.code = static_cast<ErrorCode>(code.value());
    auto detail = reader.string(limits.max_string_length);
    if (!detail.ok()) {
        return detail.status();
    }
    message.detail = detail.take();
    Status consumed = reader.expect_consumed("hello acknowledgement");
    if (!consumed.ok()) {
        return consumed;
    }
    return message;
}

Result<std::vector<std::byte>> encode_append_request(const AppendRequestMessage& message,
                                                     const Limits& limits) {
    if (message.drafts.size() > limits.max_records_per_append_frame) {
        return Status(ErrorCode::LimitExceeded, "append request exceeds the configured record count");
    }
    ByteWriter writer(512);
    writer.count(static_cast<std::uint32_t>(message.drafts.size()),
                 limits.max_records_per_append_frame);
    for (const RecordDraft& draft : message.drafts) {
        auto encoded = encode_draft(draft, limits);
        if (!encoded.ok()) {
            return encoded.status();
        }
        writer.u32(static_cast<std::uint32_t>(encoded.value().size()));
        writer.bytes(encoded.value());
    }
    if (!writer.ok()) {
        return writer.status();
    }
    if (writer.size() > limits.max_frame_size) {
        return Status(ErrorCode::PayloadTooLarge, "append request exceeds the configured frame size");
    }
    return writer.take();
}

Result<AppendRequestMessage> decode_append_request(std::span<const std::byte> payload,
                                                   const Limits& limits) {
    ByteReader reader(payload, limits, ErrorCode::ProtocolError);
    AppendRequestMessage message;
    auto count = reader.count(limits.max_records_per_append_frame);
    if (!count.ok()) {
        return count.status();
    }
    message.drafts.reserve(count.value());
    for (std::uint32_t index = 0; index < count.value(); ++index) {
        auto length = reader.u32();
        if (!length.ok()) {
            return length.status();
        }
        if (length.value() > limits.max_frame_size) {
            return Status(ErrorCode::PayloadTooLarge, "draft length exceeds the configured maximum");
        }
        auto encoded = reader.bytes(length.value());
        if (!encoded.ok()) {
            return encoded.status();
        }
        auto draft = decode_draft(encoded.value(), limits);
        if (!draft.ok()) {
            return draft.status();
        }
        message.drafts.push_back(draft.take());
    }
    Status consumed = reader.expect_consumed("append request");
    if (!consumed.ok()) {
        return consumed;
    }
    return message;
}

Result<std::vector<std::byte>> encode_append_reply(const AppendReplyMessage& message,
                                                   const Limits& limits) {
    ByteWriter writer(256);
    writer.u16(static_cast<std::uint16_t>(message.code));
    writer.string(message.detail, limits.max_string_length);
    writer.count(static_cast<std::uint32_t>(message.outcomes.size()),
                 limits.max_records_per_append_frame);
    for (const AppendOutcome& outcome : message.outcomes) {
        write_generation(writer, IdentityDomain::RecordSequence, outcome.sequence.value());
        writer.u8(static_cast<std::uint8_t>(outcome.state));
        writer.boolean(outcome.duplicate);
    }
    if (!writer.ok()) {
        return writer.status();
    }
    return writer.take();
}

Result<AppendReplyMessage> decode_append_reply(std::span<const std::byte> payload,
                                               const Limits& limits) {
    ByteReader reader(payload, limits, ErrorCode::ProtocolError);
    AppendReplyMessage message;
    auto code = reader.u16();
    if (!code.ok()) {
        return code.status();
    }
    if (code.value() > static_cast<std::uint16_t>(ErrorCode::Busy)) {
        return Status(ErrorCode::ProtocolError, "append reply carries an unknown status");
    }
    message.code = static_cast<ErrorCode>(code.value());
    auto detail = reader.string(limits.max_string_length);
    if (!detail.ok()) {
        return detail.status();
    }
    message.detail = detail.take();
    auto count = reader.count(limits.max_records_per_append_frame);
    if (!count.ok()) {
        return count.status();
    }
    message.outcomes.reserve(count.value());
    for (std::uint32_t index = 0; index < count.value(); ++index) {
        auto sequence = reader.generation_value(IdentityDomain::RecordSequence);
        if (!sequence.ok()) {
            return sequence.status();
        }
        auto state = reader.u8();
        if (!state.ok()) {
            return state.status();
        }
        if (state.value() > static_cast<std::uint8_t>(CommitState::Duplicate)) {
            return Status(ErrorCode::ProtocolError, "append reply carries an unknown commit state");
        }
        auto duplicate = reader.boolean();
        if (!duplicate.ok()) {
            return duplicate.status();
        }
        AppendOutcome outcome;
        outcome.sequence = RecordSequence::from_value(sequence.value());
        outcome.state = static_cast<CommitState>(state.value());
        outcome.duplicate = duplicate.value();
        message.outcomes.push_back(outcome);
    }
    Status consumed = reader.expect_consumed("append reply");
    if (!consumed.ok()) {
        return consumed;
    }
    return message;
}

Result<std::vector<std::byte>> encode_status_reply(const StatusReplyMessage& message,
                                                   const Limits& limits) {
    // The reply is fixed-width: every field is a bounded scalar or a digest.
    (void)limits;
    ByteWriter writer(128);
    write_generation(writer, IdentityDomain::RecordSequence, message.last_sequence.value());
    write_generation(writer, IdentityDomain::LedgerGeneration, message.generation.value());
    write_generation(writer, IdentityDomain::CoordinatorEpoch, message.epoch.value());
    writer.digest(message.chain_digest);
    writer.u64(message.sessions);
    writer.u64(message.hypotheses);
    writer.u64(message.experiments);
    writer.u64(message.attempts);
    writer.u64(message.results);
    writer.u64(message.decisions);
    writer.u64(message.live_workers);
    if (!writer.ok()) {
        return writer.status();
    }
    return writer.take();
}

Result<StatusReplyMessage> decode_status_reply(std::span<const std::byte> payload,
                                               const Limits& limits) {
    ByteReader reader(payload, limits, ErrorCode::ProtocolError);
    StatusReplyMessage message;
    auto sequence = reader.generation_value(IdentityDomain::RecordSequence);
    if (!sequence.ok()) {
        return sequence.status();
    }
    message.last_sequence = RecordSequence::from_value(sequence.value());
    auto generation = reader.generation_value(IdentityDomain::LedgerGeneration);
    if (!generation.ok()) {
        return generation.status();
    }
    message.generation = LedgerGeneration::from_value(generation.value());
    auto epoch = reader.generation_value(IdentityDomain::CoordinatorEpoch);
    if (!epoch.ok()) {
        return epoch.status();
    }
    message.epoch = CoordinatorEpoch::from_value(epoch.value());
    auto chain = reader.digest();
    if (!chain.ok()) {
        return chain.status();
    }
    message.chain_digest = chain.value();
#define RESEARCH_LEDGER_READ_COUNTER(FIELD)          \
    {                                                \
        auto value = reader.u64();                   \
        if (!value.ok()) {                           \
            return value.status();                   \
        }                                            \
        message.FIELD = value.value();               \
    }
    RESEARCH_LEDGER_READ_COUNTER(sessions)
    RESEARCH_LEDGER_READ_COUNTER(hypotheses)
    RESEARCH_LEDGER_READ_COUNTER(experiments)
    RESEARCH_LEDGER_READ_COUNTER(attempts)
    RESEARCH_LEDGER_READ_COUNTER(results)
    RESEARCH_LEDGER_READ_COUNTER(decisions)
    RESEARCH_LEDGER_READ_COUNTER(live_workers)
#undef RESEARCH_LEDGER_READ_COUNTER
    Status consumed = reader.expect_consumed("status reply");
    if (!consumed.ok()) {
        return consumed;
    }
    return message;
}

Result<std::vector<std::byte>> encode_shutdown(const ShutdownMessage& message, const Limits& limits) {
    ByteWriter writer(64);
    writer.string(message.reason, limits.max_string_length);
    if (!writer.ok()) {
        return writer.status();
    }
    return writer.take();
}

Result<ShutdownMessage> decode_shutdown(std::span<const std::byte> payload, const Limits& limits) {
    ByteReader reader(payload, limits, ErrorCode::ProtocolError);
    ShutdownMessage message;
    auto reason = reader.string(limits.max_string_length);
    if (!reason.ok()) {
        return reason.status();
    }
    message.reason = reason.take();
    Status consumed = reader.expect_consumed("shutdown");
    if (!consumed.ok()) {
        return consumed;
    }
    return message;
}

Result<std::vector<std::byte>> encode_error(const ErrorMessage& message, const Limits& limits) {
    ByteWriter writer(64);
    writer.u16(static_cast<std::uint16_t>(message.code));
    writer.string(message.detail, limits.max_string_length);
    if (!writer.ok()) {
        return writer.status();
    }
    return writer.take();
}

Result<ErrorMessage> decode_error(std::span<const std::byte> payload, const Limits& limits) {
    ByteReader reader(payload, limits, ErrorCode::ProtocolError);
    ErrorMessage message;
    auto code = reader.u16();
    if (!code.ok()) {
        return code.status();
    }
    if (code.value() > static_cast<std::uint16_t>(ErrorCode::Busy)) {
        return Status(ErrorCode::ProtocolError, "error reply carries an unknown status");
    }
    message.code = static_cast<ErrorCode>(code.value());
    auto detail = reader.string(limits.max_string_length);
    if (!detail.ok()) {
        return detail.status();
    }
    message.detail = detail.take();
    Status consumed = reader.expect_consumed("error reply");
    if (!consumed.ok()) {
        return consumed;
    }
    return message;
}

}  // namespace research_ledger
