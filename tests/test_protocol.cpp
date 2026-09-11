#include "test_framework.hpp"
#include "test_support.hpp"

#include "research_ledger/codec.hpp"
#include "research_ledger/net.hpp"
#include "research_ledger/protocol.hpp"
#include "research_ledger/record_codec.hpp"
#include "research_ledger/version.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace research_ledger {
namespace {

using namespace research_ledger::test;

// Offsets inside the fixed frame header documented in protocol.hpp.
inline constexpr std::size_t kFrameMagicOffset = 0;
inline constexpr std::size_t kFrameVersionOffset = 4;
inline constexpr std::size_t kFrameTypeOffset = 6;
inline constexpr std::size_t kFrameFlagsOffset = 8;
inline constexpr std::size_t kFramePayloadLengthOffset = 10;
inline constexpr std::size_t kFrameCorrelationOffset = 14;

std::vector<std::byte> bytes_of(std::string_view text) {
    const auto* begin = reinterpret_cast<const std::byte*>(text.data());
    return std::vector<std::byte>(begin, begin + text.size());
}

void put_u16(std::vector<std::byte>& bytes, std::size_t offset, std::uint16_t value) {
    bytes[offset] = static_cast<std::byte>(value & 0xffu);
    bytes[offset + 1] = static_cast<std::byte>((value >> 8) & 0xffu);
}

void put_u32(std::vector<std::byte>& bytes, std::size_t offset, std::uint32_t value) {
    for (std::size_t index = 0; index < 4; ++index) {
        bytes[offset + index] =
            static_cast<std::byte>((value >> (8u * static_cast<unsigned>(index))) & 0xffu);
    }
}

std::uint32_t read_u32(std::span<const std::byte> bytes, std::size_t offset) {
    std::uint32_t value = 0;
    for (std::size_t index = 0; index < 4; ++index) {
        value |= static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(bytes[offset + index]))
                 << (8u * static_cast<unsigned>(index));
    }
    return value;
}

bool same_bytes(std::span<const std::byte> lhs, const std::vector<std::byte>& rhs) {
    if (lhs.size() != rhs.size()) {
        return false;
    }
    for (std::size_t index = 0; index < lhs.size(); ++index) {
        if (lhs[index] != rhs[index]) {
            return false;
        }
    }
    return true;
}

AuthorityEnvelope frame_authority() {
    AuthorityEnvelope authority;
    authority.ledger = LedgerGeneration::first();
    authority.epoch = CoordinatorEpoch::from_value(3);
    authority.worker = WorkerId::from_value(11);
    authority.worker_boot = WorkerBootId::from_value(0x0000000300000009ull);
    return authority;
}

std::vector<RecordDraft> sample_drafts() {
    RecordDraft first = session_opened(1, "autonomous research session", "does the ledger hold?");
    first.record_id = LedgerRecordId::from_value(4242);
    RecordDraft second = hypothesis_declared(1, 1, "a bounded ledger reconstructs results");
    RecordDraft third = branch_declared(1, 1, BranchKind::Retry, 1);
    third.idempotent = true;
    return std::vector<RecordDraft>{std::move(first), std::move(second), std::move(third)};
}

// Every payload codec must reject a prefix at any offset and any trailing byte,
// and it must do so with the protocol error code rather than a persistence one.
template <class Decode>
void check_payload_is_exact(const std::vector<std::byte>& payload, const Limits& limits,
                            Decode decode) {
    RL_CHECK(!payload.empty());
    for (std::size_t length = 0; length < payload.size(); ++length) {
        const std::span<const std::byte> prefix(payload.data(), length);
        RL_CHECK_CODE(decode(prefix, limits), ErrorCode::ProtocolError);
    }
    std::vector<std::byte> extended = payload;
    extended.push_back(std::byte{0x00});
    RL_CHECK_CODE(decode(extended, limits), ErrorCode::ProtocolError);
}

RL_TEST(protocol_crc32_matches_the_known_check_vector) {
    const std::vector<std::byte> check = bytes_of("123456789");
    RL_CHECK_EQ(crc32(check), 0xcbf43926u);
    RL_CHECK_EQ(crc32(std::span<const std::byte>{}), 0u);

    const std::vector<std::byte> near_miss = bytes_of("123456788");
    RL_CHECK(crc32(near_miss) != crc32(check));

    // One flipped bit changes the checksum.
    std::vector<std::byte> flipped = check;
    flipped[3] ^= std::byte{0x01};
    RL_CHECK(crc32(flipped) != crc32(check));

    // A frame header checksums every preceding header byte and stores the value
    // immediately before the payload.
    Limits limits;
    auto frame = encode_frame(MessageType::Hello, 1, frame_authority(), bytes_of("body"), limits);
    RL_CHECK_OK(frame);
    RL_CHECK_EQ(frame.value().size(), kFrameHeaderSize + 4u);
    const std::uint32_t stored = read_u32(frame.value(), kFrameHeaderSize - 4);
    RL_CHECK_EQ(stored, crc32(std::span<const std::byte>(frame.value().data(),
                                                         kFrameHeaderSize - 4)));
}

RL_TEST(protocol_frames_round_trip_for_every_message_type) {
    Limits limits;
    const AuthorityEnvelope authority = frame_authority();
    const std::vector<std::byte> payload = bytes_of("payload-bytes");

    for (std::uint16_t raw = 1; raw <= static_cast<std::uint16_t>(MessageType::ErrorReply); ++raw) {
        const MessageType type = static_cast<MessageType>(raw);
        RL_CHECK(parse_message_type(message_type_name(type)) == type);
        auto frame = encode_frame(type, 99, authority, payload, limits);
        RL_CHECK_OK(frame);
        RL_CHECK_EQ(frame.value().size(), kFrameHeaderSize + payload.size());
        auto view = decode_frame(frame.value(), limits);
        RL_CHECK_OK(view);
        RL_CHECK(view.value().type == type);
        RL_CHECK_EQ(view.value().correlation_id, 99u);
        RL_CHECK(view.value().epoch == authority.epoch);
        RL_CHECK(view.value().worker == authority.worker);
        RL_CHECK(view.value().worker_boot == authority.worker_boot);
        RL_CHECK(same_bytes(view.value().payload, payload));
        RL_CHECK_EQ(view.value().payload.size(), payload.size());
    }

    // An empty payload is a valid frame.
    auto empty = encode_frame(MessageType::StatusRequest, 7, authority, std::span<const std::byte>{},
                              limits);
    RL_CHECK_OK(empty);
    RL_CHECK_EQ(empty.value().size(), kFrameHeaderSize);
    auto empty_view = decode_frame(empty.value(), limits);
    RL_CHECK_OK(empty_view);
    RL_CHECK(empty_view.value().payload.empty());
    RL_CHECK(empty_view.value().type == MessageType::StatusRequest);

    // An oversized payload is rejected before a frame is built.
    const std::vector<std::byte> oversized(static_cast<std::size_t>(limits.max_frame_size) + 1u,
                                           std::byte{0});
    RL_CHECK_CODE(encode_frame(MessageType::Hello, 1, authority, oversized, limits),
                  ErrorCode::PayloadTooLarge);
}

RL_TEST(protocol_frame_rejections_are_typed) {
    Limits limits;
    const AuthorityEnvelope authority = frame_authority();
    const std::vector<std::byte> payload = bytes_of("framed-payload");
    auto encoded = encode_frame(MessageType::AppendRequest, 5, authority, payload, limits);
    RL_CHECK_OK(encoded);
    const std::vector<std::byte>& frame = encoded.value();
    RL_CHECK_EQ(frame.size(), kFrameHeaderSize + payload.size());

    // Truncation at every byte offset is rejected: a partial frame is not a frame.
    for (std::size_t length = 0; length < frame.size(); ++length) {
        const std::span<const std::byte> prefix(frame.data(), length);
        RL_CHECK_CODE(decode_frame(prefix, limits), ErrorCode::ProtocolError);
    }
    // Bad magic.
    {
        std::vector<std::byte> broken = frame;
        put_u32(broken, kFrameMagicOffset, 0x31504c53u);
        RL_CHECK_CODE(decode_frame(broken, limits), ErrorCode::ProtocolError);
    }
    // Unsupported protocol version.
    {
        std::vector<std::byte> broken = frame;
        put_u16(broken, kFrameVersionOffset, static_cast<std::uint16_t>(kProtocolVersion + 1u));
        RL_CHECK_CODE(decode_frame(broken, limits), ErrorCode::ProtocolError);
    }
    {
        std::vector<std::byte> broken = frame;
        put_u16(broken, kFrameVersionOffset, 0u);
        RL_CHECK_CODE(decode_frame(broken, limits), ErrorCode::ProtocolError);
    }
    // Unknown message type.
    {
        std::vector<std::byte> broken = frame;
        put_u16(broken, kFrameTypeOffset, 0u);
        RL_CHECK_CODE(decode_frame(broken, limits), ErrorCode::ProtocolError);
    }
    {
        std::vector<std::byte> broken = frame;
        put_u16(broken, kFrameTypeOffset, 0x0063u);
        RL_CHECK_CODE(decode_frame(broken, limits), ErrorCode::ProtocolError);
    }
    // Non-zero flags are not a feature yet.
    {
        std::vector<std::byte> broken = frame;
        put_u16(broken, kFrameFlagsOffset, 1u);
        RL_CHECK_CODE(decode_frame(broken, limits), ErrorCode::ProtocolError);
    }
    // An oversized declared payload is rejected without allocating it.
    {
        std::vector<std::byte> broken = frame;
        put_u32(broken, kFramePayloadLengthOffset, limits.max_frame_size + 1u);
        RL_CHECK_CODE(decode_frame(broken, limits), ErrorCode::PayloadTooLarge);
    }
    // A declared payload length that disagrees with the frame is a protocol error.
    {
        std::vector<std::byte> broken = frame;
        put_u32(broken, kFramePayloadLengthOffset, static_cast<std::uint32_t>(payload.size()) + 1u);
        RL_CHECK_CODE(decode_frame(broken, limits), ErrorCode::ProtocolError);
    }
    {
        std::vector<std::byte> broken = frame;
        put_u32(broken, kFramePayloadLengthOffset, 0u);
        RL_CHECK_CODE(decode_frame(broken, limits), ErrorCode::ProtocolError);
    }
    // A corrupted header byte fails the checksum.
    {
        std::vector<std::byte> broken = frame;
        broken[kFrameCorrelationOffset] ^= std::byte{0x20};
        RL_CHECK_CODE(decode_frame(broken, limits), ErrorCode::IntegrityFailure);
    }
    {
        std::vector<std::byte> broken = frame;
        broken[kFrameHeaderSize - 2] ^= std::byte{0x80};
        RL_CHECK_CODE(decode_frame(broken, limits), ErrorCode::IntegrityFailure);
    }
    // Trailing bytes after the declared payload are rejected.
    {
        std::vector<std::byte> broken = frame;
        broken.push_back(std::byte{0x00});
        RL_CHECK_CODE(decode_frame(broken, limits), ErrorCode::ProtocolError);
    }
    // A frame shorter than its header, and an empty frame.
    RL_CHECK_CODE(decode_frame(std::span<const std::byte>{}, limits), ErrorCode::ProtocolError);
    RL_CHECK_CODE(
        decode_frame(std::span<const std::byte>(frame.data(), kFrameHeaderSize - 1u), limits),
        ErrorCode::ProtocolError);
    // The unmodified frame still decodes.
    RL_CHECK_OK(decode_frame(frame, limits));
}

RL_TEST(protocol_payload_codecs_round_trip) {
    Limits limits;

    // HELLO
    {
        HelloMessage message;
        message.worker = WorkerId::from_value(11);
        message.worker_boot = WorkerBootId::from_value(0x77);
        message.authority = "worker:11";
        auto encoded = encode_hello(message, limits);
        RL_CHECK_OK(encoded);
        auto decoded = decode_hello(encoded.value(), limits);
        RL_CHECK_OK(decoded);
        RL_CHECK(decoded.value().worker == message.worker);
        RL_CHECK(decoded.value().worker_boot == message.worker_boot);
        RL_CHECK_EQ(decoded.value().authority, message.authority);
        check_payload_is_exact(encoded.value(), limits,
                               [](std::span<const std::byte> bytes, const Limits& bounds) {
                                   return decode_hello(bytes, bounds);
                               });
    }
    // HELLO_ACK
    {
        HelloAckMessage message;
        message.epoch = CoordinatorEpoch::from_value(4);
        message.generation = LedgerGeneration::from_value(2);
        message.worker_boot = WorkerBootId::from_value(0x1f);
        message.coordinator = "research-ledger-coordinator";
        message.code = ErrorCode::Ok;
        message.detail = "admitted";
        auto encoded = encode_hello_ack(message, limits);
        RL_CHECK_OK(encoded);
        auto decoded = decode_hello_ack(encoded.value(), limits);
        RL_CHECK_OK(decoded);
        RL_CHECK(decoded.value().epoch == message.epoch);
        RL_CHECK(decoded.value().generation == message.generation);
        RL_CHECK(decoded.value().worker_boot == message.worker_boot);
        RL_CHECK_EQ(decoded.value().coordinator, message.coordinator);
        RL_CHECK(decoded.value().code == message.code);
        RL_CHECK_EQ(decoded.value().detail, message.detail);
    }
    // APPEND_REQUEST with several drafts and domain tagged identities.
    {
        AppendRequestMessage message;
        message.drafts = sample_drafts();
        auto encoded = encode_append_request(message, limits);
        RL_CHECK_OK(encoded);
        auto decoded = decode_append_request(encoded.value(), limits);
        RL_CHECK_OK(decoded);
        RL_CHECK_EQ(decoded.value().drafts.size(), message.drafts.size());
        for (std::size_t index = 0; index < message.drafts.size(); ++index) {
            RL_CHECK(decoded.value().drafts[index].record_id == message.drafts[index].record_id);
            RL_CHECK(decoded.value().drafts[index].provenance == message.drafts[index].provenance);
            RL_CHECK_EQ(decoded.value().drafts[index].idempotent, message.drafts[index].idempotent);
            RL_CHECK(record_type_of(decoded.value().drafts[index].body) ==
                     record_type_of(message.drafts[index].body));
            auto left = encode_draft(decoded.value().drafts[index], limits);
            auto right = encode_draft(message.drafts[index], limits);
            RL_CHECK_OK(left);
            RL_CHECK_OK(right);
            RL_CHECK(left.value() == right.value());
        }
    }
    // APPEND_REPLY
    {
        AppendReplyMessage message;
        message.code = ErrorCode::Ok;
        message.outcomes.push_back(
            AppendOutcome{RecordSequence::from_value(1), CommitState::Committed, false});
        message.outcomes.push_back(
            AppendOutcome{RecordSequence::from_value(2), CommitState::Duplicate, true});
        message.outcomes.push_back(
            AppendOutcome{RecordSequence::from_value(3), CommitState::Rejected, false});
        auto encoded = encode_append_reply(message, limits);
        RL_CHECK_OK(encoded);
        auto decoded = decode_append_reply(encoded.value(), limits);
        RL_CHECK_OK(decoded);
        RL_CHECK(decoded.value().code == message.code);
        RL_CHECK_EQ(decoded.value().outcomes.size(), message.outcomes.size());
        for (std::size_t index = 0; index < message.outcomes.size(); ++index) {
            RL_CHECK(decoded.value().outcomes[index].sequence == message.outcomes[index].sequence);
            RL_CHECK(decoded.value().outcomes[index].state == message.outcomes[index].state);
            RL_CHECK_EQ(decoded.value().outcomes[index].duplicate,
                        message.outcomes[index].duplicate);
        }
    }
    // STATUS_REPLY
    {
        StatusReplyMessage message;
        message.last_sequence = RecordSequence::from_value(17);
        message.generation = LedgerGeneration::from_value(2);
        message.epoch = CoordinatorEpoch::from_value(3);
        message.chain_digest = sha256("chain");
        message.sessions = 1;
        message.hypotheses = 2;
        message.experiments = 3;
        message.attempts = 4;
        message.results = 5;
        message.decisions = 6;
        message.live_workers = 7;
        auto encoded = encode_status_reply(message, limits);
        RL_CHECK_OK(encoded);
        auto decoded = decode_status_reply(encoded.value(), limits);
        RL_CHECK_OK(decoded);
        RL_CHECK(decoded.value().last_sequence == message.last_sequence);
        RL_CHECK(decoded.value().generation == message.generation);
        RL_CHECK(decoded.value().epoch == message.epoch);
        RL_CHECK(decoded.value().chain_digest == message.chain_digest);
        RL_CHECK_EQ(decoded.value().sessions, message.sessions);
        RL_CHECK_EQ(decoded.value().hypotheses, message.hypotheses);
        RL_CHECK_EQ(decoded.value().experiments, message.experiments);
        RL_CHECK_EQ(decoded.value().attempts, message.attempts);
        RL_CHECK_EQ(decoded.value().results, message.results);
        RL_CHECK_EQ(decoded.value().decisions, message.decisions);
        RL_CHECK_EQ(decoded.value().live_workers, message.live_workers);
        check_payload_is_exact(encoded.value(), limits,
                               [](std::span<const std::byte> bytes, const Limits& bounds) {
                                   return decode_status_reply(bytes, bounds);
                               });
    }
    // SHUTDOWN
    {
        ShutdownMessage message;
        message.reason = "operator requested shutdown";
        auto encoded = encode_shutdown(message, limits);
        RL_CHECK_OK(encoded);
        auto decoded = decode_shutdown(encoded.value(), limits);
        RL_CHECK_OK(decoded);
        RL_CHECK_EQ(decoded.value().reason, message.reason);
        check_payload_is_exact(encoded.value(), limits,
                               [](std::span<const std::byte> bytes, const Limits& bounds) {
                                   return decode_shutdown(bytes, bounds);
                               });
    }
    // ERROR
    {
        ErrorMessage message;
        message.code = ErrorCode::StaleWorker;
        message.detail = "worker incarnation is fenced";
        auto encoded = encode_error(message, limits);
        RL_CHECK_OK(encoded);
        auto decoded = decode_error(encoded.value(), limits);
        RL_CHECK_OK(decoded);
        RL_CHECK(decoded.value().code == message.code);
        RL_CHECK_EQ(decoded.value().detail, message.detail);
        check_payload_is_exact(encoded.value(), limits,
                               [](std::span<const std::byte> bytes, const Limits& bounds) {
                                   return decode_error(bytes, bounds);
                               });
    }
}

RL_TEST(protocol_payload_truncation_and_trailing_bytes_are_rejected) {
    Limits limits;

    HelloMessage hello;
    hello.worker = WorkerId::from_value(1);
    hello.worker_boot = WorkerBootId::from_value(2);
    hello.authority = "worker:1";
    auto encoded_hello = encode_hello(hello, limits);
    RL_CHECK_OK(encoded_hello);
    check_payload_is_exact(encoded_hello.value(), limits,
                           [](std::span<const std::byte> bytes, const Limits& bounds) {
                               return decode_hello(bytes, bounds);
                           });

    HelloAckMessage ack;
    ack.epoch = CoordinatorEpoch::first();
    ack.generation = LedgerGeneration::first();
    ack.worker_boot = WorkerBootId::from_value(2);
    ack.coordinator = "coordinator";
    auto encoded_ack = encode_hello_ack(ack, limits);
    RL_CHECK_OK(encoded_ack);
    check_payload_is_exact(encoded_ack.value(), limits,
                           [](std::span<const std::byte> bytes, const Limits& bounds) {
                               return decode_hello_ack(bytes, bounds);
                           });

    AppendRequestMessage request;
    request.drafts = sample_drafts();
    auto encoded_request = encode_append_request(request, limits);
    RL_CHECK_OK(encoded_request);
    check_payload_is_exact(encoded_request.value(), limits,
                           [](std::span<const std::byte> bytes, const Limits& bounds) {
                               return decode_append_request(bytes, bounds);
                           });

    AppendReplyMessage reply;
    reply.outcomes.push_back(
        AppendOutcome{RecordSequence::from_value(1), CommitState::Committed, false});
    auto encoded_reply = encode_append_reply(reply, limits);
    RL_CHECK_OK(encoded_reply);
    check_payload_is_exact(encoded_reply.value(), limits,
                           [](std::span<const std::byte> bytes, const Limits& bounds) {
                               return decode_append_reply(bytes, bounds);
                           });
}

RL_TEST(protocol_append_request_bounds_are_checked_before_allocation) {
    Limits limits;

    // A declared draft count beyond the limit is rejected by the reader before
    // anything is reserved for it.
    {
        ByteWriter writer;
        writer.u32(limits.max_records_per_append_frame + 1u);
        auto payload = writer.take();
        RL_CHECK_CODE(decode_append_request(payload, limits), ErrorCode::LimitExceeded);
    }
    {
        ByteWriter writer;
        writer.u32(0xffffffffu);
        auto payload = writer.take();
        RL_CHECK_CODE(decode_append_request(payload, limits), ErrorCode::LimitExceeded);
    }
    {
        ByteWriter writer;
        writer.count(limits.max_records_per_append_frame, limits.max_records_per_append_frame);
        auto payload = writer.take();
        // The declared count is legal but the drafts behind it are missing.
        RL_CHECK_CODE(decode_append_request(payload, limits), ErrorCode::ProtocolError);
    }
    // The writer refuses to build an oversized request in the first place.
    {
        AppendRequestMessage message;
        message.drafts = sample_drafts();
        while (message.drafts.size() <= limits.max_records_per_append_frame) {
            message.drafts.push_back(session_opened(1));
        }
        RL_CHECK_CODE(encode_append_request(message, limits), ErrorCode::LimitExceeded);
    }
    // A declared draft length beyond the frame bound is rejected as well.
    {
        ByteWriter writer;
        writer.count(1u, limits.max_records_per_append_frame);
        writer.u32(limits.max_frame_size + 1u);
        auto payload = writer.take();
        RL_CHECK_CODE(decode_append_request(payload, limits), ErrorCode::PayloadTooLarge);
    }
    // A record body declaring an absurd element count is rejected before the
    // count is used to reserve anything.
    {
        ByteWriter writer;
        writer.u16(static_cast<std::uint16_t>(RecordType::SessionOpened));
        write_identity(writer, ResearchSessionId::from_value(1));
        writer.string("label", limits.max_label_length);
        writer.string("question", limits.max_string_length);
        writer.u32(0xffffffffu);
        auto payload = writer.take();
        RL_CHECK_CODE(decode_record_body(payload, limits), ErrorCode::LimitExceeded);
    }
}

RL_TEST(protocol_frames_cross_a_real_loopback_socket) {
    NetworkRuntime network;
    TcpListener listener;
    RL_CHECK(listener.listen_loopback(0, 4).ok());
    RL_CHECK(listener.valid());
    const std::uint16_t port = listener.bound_port();
    RL_CHECK(port != 0);

    Limits limits;
    const AuthorityEnvelope authority = frame_authority();

    HelloMessage hello;
    hello.worker = authority.worker;
    hello.worker_boot = authority.worker_boot;
    hello.authority = "worker:11";
    auto hello_payload = encode_hello(hello, limits);
    RL_CHECK_OK(hello_payload);

    AppendRequestMessage request;
    request.drafts = sample_drafts();
    auto request_payload = encode_append_request(request, limits);
    RL_CHECK_OK(request_payload);

    struct Received {
        bool accepted = false;
        bool failed = false;
        bool first_ok = false;
        bool second_ok = false;
        MessageType first_type = MessageType::Hello;
        MessageType second_type = MessageType::Hello;
        std::uint64_t first_correlation = 0;
        std::uint64_t second_correlation = 0;
        std::vector<std::byte> first_payload{};
        std::vector<std::byte> second_payload{};
        std::string peer{};
        std::string error{};
    } received;

    // The listener is bound and the receiver is running before the sender
    // connects. Both sides make progress without a sleep: the sender writes two
    // frames, the receiver reads exactly two frames, and the join returns.
    std::thread receiver([&listener, &received, limits]() {
        std::string peer;
        auto socket = listener.accept_one(peer);
        if (!socket.ok()) {
            received.failed = true;
            received.error = socket.status().message;
            return;
        }
        received.accepted = true;
        received.peer = peer;

        std::vector<std::byte> storage;
        auto first = receive_frame(socket.value(), limits, storage);
        if (!first.ok()) {
            received.failed = true;
            received.error = first.status().message;
            socket.value().close();
            return;
        }
        received.first_ok = true;
        received.first_type = first.value().type;
        received.first_correlation = first.value().correlation_id;
        received.first_payload.assign(first.value().payload.begin(), first.value().payload.end());

        auto second = receive_frame(socket.value(), limits, storage);
        if (!second.ok()) {
            received.failed = true;
            received.error = second.status().message;
            socket.value().close();
            return;
        }
        received.second_ok = true;
        received.second_type = second.value().type;
        received.second_correlation = second.value().correlation_id;
        received.second_payload.assign(second.value().payload.begin(), second.value().payload.end());
        socket.value().close();
    });

    // Nothing between starting the receiver and joining it may throw: a failed
    // check here would unwind past a joinable thread and terminate the process.
    Status outcome{};
    {
        auto client = connect_loopback(port, "127.0.0.1");
        if (!client.ok()) {
            outcome = client.status();
            listener.close();
        } else {
            TcpSocket socket = client.take();
            outcome = send_frame(socket, MessageType::Hello, 1, authority, hello_payload.value(),
                                 limits);
            if (outcome.ok()) {
                outcome = send_frame(socket, MessageType::AppendRequest, 2, authority,
                                     request_payload.value(), limits);
            }
            socket.close();
        }
    }
    receiver.join();

    RL_CHECK_MESSAGE(outcome.ok(),
                     std::string("client side failed: ") +
                         std::string(research_ledger::error_code_name(outcome.code)) + " (" +
                         outcome.message + ")");
    RL_CHECK(!received.failed);
    RL_CHECK_EQ(received.error, std::string{});
    RL_CHECK(received.accepted);
    RL_CHECK(received.first_ok);
    RL_CHECK(received.second_ok);
    RL_CHECK(received.first_type == MessageType::Hello);
    RL_CHECK(received.second_type == MessageType::AppendRequest);
    RL_CHECK_EQ(received.first_correlation, 1u);
    RL_CHECK_EQ(received.second_correlation, 2u);
    RL_CHECK(same_bytes(received.first_payload, hello_payload.value()));
    RL_CHECK(same_bytes(received.second_payload, request_payload.value()));

    // The bytes that crossed the socket decode to the messages that were sent.
    auto decoded_hello = decode_hello(received.first_payload, limits);
    RL_CHECK_OK(decoded_hello);
    RL_CHECK(decoded_hello.value().worker == hello.worker);
    RL_CHECK(decoded_hello.value().worker_boot == hello.worker_boot);
    RL_CHECK_EQ(decoded_hello.value().authority, hello.authority);

    auto decoded_request = decode_append_request(received.second_payload, limits);
    RL_CHECK_OK(decoded_request);
    RL_CHECK_EQ(decoded_request.value().drafts.size(), request.drafts.size());
    for (std::size_t index = 0; index < request.drafts.size(); ++index) {
        RL_CHECK(record_type_of(decoded_request.value().drafts[index].body) ==
                 record_type_of(request.drafts[index].body));
        RL_CHECK(decoded_request.value().drafts[index].record_id ==
                 request.drafts[index].record_id);
    }

    // The listener reports the loopback peer it accepted.
    RL_CHECK(received.peer.find("127.0.0.1:") == 0u);
}

}  // namespace
}  // namespace research_ledger
