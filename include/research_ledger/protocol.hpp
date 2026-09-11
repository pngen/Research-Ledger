#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "research_ledger/error.hpp"
#include "research_ledger/evidence.hpp"
#include "research_ledger/identity.hpp"
#include "research_ledger/limits.hpp"
#include "research_ledger/record.hpp"
#include "research_ledger/record_codec.hpp"

namespace research_ledger {

// Bounded framed transport. Every frame carries its own authority. Field
// offsets are listed because the layout is fixed and shared:
//
//   offset  size  field
//        0     4  magic             'R','L','P','1'
//        4     2  protocol_version  must equal kProtocolVersion
//        6     2  message_type      must be a known message type
//        8     2  flags             must be zero
//       10     4  payload_length    bounded by Limits::max_frame_size
//       14     8  correlation_id    echoed by the reply
//       22     4  coordinator_epoch authority claim of the sender
//       26     8  worker            authority claim of the sender
//       34     8  worker_boot       authority claim of the sender
//       42     4  checksum          CRC-32 over bytes 0..41
//       46       payload           payload_length bytes
//
// A frame is rejected when it is truncated, oversized, carries an unknown
// message type, carries non-zero flags, fails its checksum, or when the payload
// of a typed message is not consumed exactly.
enum class MessageType : std::uint16_t {
    Hello = 1,
    HelloAck = 2,
    AppendRequest = 3,
    AppendReply = 4,
    StatusRequest = 5,
    StatusReply = 6,
    ShutdownRequest = 7,
    ShutdownReply = 8,
    ErrorReply = 9,
};

std::string_view message_type_name(MessageType type) noexcept;
std::optional<MessageType> parse_message_type(std::string_view text) noexcept;

inline constexpr std::uint32_t kFrameMagic = 0x31504c52u;  // "RLP1"
inline constexpr std::uint32_t kFrameHeaderSize = 46;

struct FrameView {
    MessageType type = MessageType::Hello;
    std::uint64_t correlation_id = 0;
    CoordinatorEpoch epoch{};
    WorkerId worker{};
    WorkerBootId worker_boot{};
    std::span<const std::byte> payload{};
};

// CRC-32 (IEEE 802.3), computed in this repository.
std::uint32_t crc32(std::span<const std::byte> bytes) noexcept;

Result<std::vector<std::byte>> encode_frame(MessageType type, std::uint64_t correlation_id,
                                            const AuthorityEnvelope& authority,
                                            std::span<const std::byte> payload,
                                            const Limits& limits);
Result<FrameView> decode_frame(std::span<const std::byte> frame, const Limits& limits);

// Payload length declared by a complete frame header. The fixed prefix (magic,
// protocol version, message type, flags) and the declared length are validated
// against the same bounds decode_frame uses; the caller can therefore size its
// read before it has the payload, without duplicating the layout.
Result<std::uint32_t> frame_payload_length(std::span<const std::byte> header, const Limits& limits);

// --- message payloads -------------------------------------------------------
struct HelloMessage {
    WorkerId worker{};
    WorkerBootId worker_boot{};
    std::string authority{};
};

struct HelloAckMessage {
    CoordinatorEpoch epoch{};
    LedgerGeneration generation{};
    // Assigned by the coordinator. A worker never invents its own boot
    // identity: the coordinator mints one per admitted incarnation, which is
    // what makes an older incarnation's traffic stale by construction.
    WorkerBootId worker_boot{};
    std::string coordinator{};
    ErrorCode code = ErrorCode::Ok;
    std::string detail{};
};

struct AppendRequestMessage {
    std::vector<RecordDraft> drafts{};
};

struct AppendReplyMessage {
    std::vector<AppendOutcome> outcomes{};
    ErrorCode code = ErrorCode::Ok;
    std::string detail{};
};

struct StatusReplyMessage {
    RecordSequence last_sequence{};
    LedgerGeneration generation{};
    CoordinatorEpoch epoch{};
    Digest chain_digest{};
    std::uint64_t sessions = 0;
    std::uint64_t hypotheses = 0;
    std::uint64_t experiments = 0;
    std::uint64_t attempts = 0;
    std::uint64_t results = 0;
    std::uint64_t decisions = 0;
    std::uint64_t live_workers = 0;
};

struct ShutdownMessage {
    std::string reason{};
};

struct ErrorMessage {
    ErrorCode code = ErrorCode::Ok;
    std::string detail{};
};

// Payload codecs. Each one consumes its input exactly.
Result<std::vector<std::byte>> encode_hello(const HelloMessage& message, const Limits& limits);
Result<HelloMessage> decode_hello(std::span<const std::byte> payload, const Limits& limits);
Result<std::vector<std::byte>> encode_hello_ack(const HelloAckMessage& message, const Limits& limits);
Result<HelloAckMessage> decode_hello_ack(std::span<const std::byte> payload, const Limits& limits);
Result<std::vector<std::byte>> encode_append_request(const AppendRequestMessage& message,
                                                     const Limits& limits);
Result<AppendRequestMessage> decode_append_request(std::span<const std::byte> payload,
                                                   const Limits& limits);
Result<std::vector<std::byte>> encode_append_reply(const AppendReplyMessage& message,
                                                   const Limits& limits);
Result<AppendReplyMessage> decode_append_reply(std::span<const std::byte> payload,
                                               const Limits& limits);
Result<std::vector<std::byte>> encode_status_reply(const StatusReplyMessage& message,
                                                   const Limits& limits);
Result<StatusReplyMessage> decode_status_reply(std::span<const std::byte> payload,
                                               const Limits& limits);
Result<std::vector<std::byte>> encode_shutdown(const ShutdownMessage& message, const Limits& limits);
Result<ShutdownMessage> decode_shutdown(std::span<const std::byte> payload, const Limits& limits);
Result<std::vector<std::byte>> encode_error(const ErrorMessage& message, const Limits& limits);
Result<ErrorMessage> decode_error(std::span<const std::byte> payload, const Limits& limits);

}  // namespace research_ledger
