#include "research_ledger/net.hpp"

#include <array>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

// The frame header carries a protocol version; the value itself is owned by
// version.hpp and is not repeated here.
#include "research_ledger/version.hpp"

namespace research_ledger {
namespace {

#ifdef _WIN32
using NativeSocket = SOCKET;
constexpr NativeSocket kInvalid = INVALID_SOCKET;

int last_error() noexcept { return WSAGetLastError(); }

void close_native(NativeSocket socket) noexcept { closesocket(socket); }

Status set_reuse(NativeSocket socket) noexcept {
    BOOL enabled = TRUE;
    if (setsockopt(socket, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&enabled),
                   static_cast<int>(sizeof(enabled))) != 0) {
        return Status(ErrorCode::IoFailure, "setsockopt(SO_REUSEADDR) failed");
    }
    return Status{};
}

Status disable_nagle(NativeSocket socket) noexcept {
    BOOL enabled = TRUE;
    if (setsockopt(socket, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&enabled),
                   static_cast<int>(sizeof(enabled))) != 0) {
        return Status(ErrorCode::IoFailure, "setsockopt(TCP_NODELAY) failed");
    }
    return Status{};
}
#else
using NativeSocket = int;
constexpr NativeSocket kInvalid = -1;

int last_error() noexcept { return errno; }

void close_native(NativeSocket socket) noexcept { ::close(socket); }

Status set_reuse(NativeSocket socket) noexcept {
    int enabled = 1;
    if (setsockopt(socket, SOL_SOCKET, SO_REUSEADDR, &enabled, static_cast<socklen_t>(sizeof(enabled))) !=
        0) {
        return Status(ErrorCode::IoFailure, "setsockopt(SO_REUSEADDR) failed");
    }
    return Status{};
}

Status disable_nagle(NativeSocket socket) noexcept {
    int enabled = 1;
    if (setsockopt(socket, IPPROTO_TCP, TCP_NODELAY, &enabled, static_cast<socklen_t>(sizeof(enabled))) !=
        0) {
        return Status(ErrorCode::IoFailure, "setsockopt(TCP_NODELAY) failed");
    }
    return Status{};
}
#endif

NativeSocket to_native(std::uintptr_t handle) noexcept { return static_cast<NativeSocket>(handle); }
std::uintptr_t to_handle(NativeSocket socket) noexcept { return static_cast<std::uintptr_t>(socket); }

// A single send()/recv() call never moves more than this many bytes. The
// transport loops over chunks so that one request can never ask the kernel to
// transfer an unbounded amount in a single call.
inline constexpr std::size_t kChunkSize = 1u << 20;

// Upper bound on the buffer receive_exact() will allocate. Every caller passes
// a count that is already bounded by Limits::max_frame_size; this is the
// second, unconditional bound, so a corrupted or hostile length can never turn
// into an unbounded allocation.
inline constexpr std::size_t kMaximumReceiveAllocation = 64u << 20;

// Offset of the frame checksum inside the fixed header. Every other field is
// read through the codec that owns the layout (frame_payload_length), so this
// file does not repeat the frame's structure.
inline constexpr std::size_t kChecksumOffset = 42;

std::uint32_t read_u32_le(const std::byte* bytes) noexcept {
    return static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(bytes[0])) |
           (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(bytes[1])) << 8) |
           (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(bytes[2])) << 16) |
           (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(bytes[3])) << 24);
}

}  // namespace

NetworkRuntime::NetworkRuntime() {
#ifdef _WIN32
    WSADATA data{};
    static_cast<void>(WSAStartup(MAKEWORD(2, 2), &data));
#endif
}

NetworkRuntime::~NetworkRuntime() {
#ifdef _WIN32
    WSACleanup();
#endif
}

TcpSocket::TcpSocket(std::uintptr_t handle) noexcept : handle_(handle) {}

TcpSocket::~TcpSocket() { close(); }

TcpSocket::TcpSocket(TcpSocket&& other) noexcept : handle_(other.handle_) {
    other.handle_ = kInvalidSocketHandle;
}

TcpSocket& TcpSocket::operator=(TcpSocket&& other) noexcept {
    if (this != &other) {
        close();
        handle_ = other.handle_;
        other.handle_ = kInvalidSocketHandle;
    }
    return *this;
}

void TcpSocket::close() noexcept {
    if (handle_ != kInvalidSocketHandle) {
        close_native(to_native(handle_));
        handle_ = kInvalidSocketHandle;
    }
}

void TcpSocket::shutdown_both() noexcept {
    if (handle_ != kInvalidSocketHandle) {
#ifdef _WIN32
        static_cast<void>(::shutdown(to_native(handle_), SD_BOTH));
#else
        static_cast<void>(::shutdown(to_native(handle_), SHUT_RDWR));
#endif
    }
}

TcpListener::~TcpListener() { close(); }

TcpListener::TcpListener(TcpListener&& other) noexcept : handle_(other.handle_), port_(other.port_) {
    other.handle_ = kInvalidSocketHandle;
    other.port_ = 0;
}

TcpListener& TcpListener::operator=(TcpListener&& other) noexcept {
    if (this != &other) {
        close();
        handle_ = other.handle_;
        port_ = other.port_;
        other.handle_ = kInvalidSocketHandle;
        other.port_ = 0;
    }
    return *this;
}

void TcpListener::close() noexcept {
    if (handle_ != kInvalidSocketHandle) {
        close_native(to_native(handle_));
        handle_ = kInvalidSocketHandle;
    }
}

Status TcpListener::listen_loopback(std::uint16_t port, int backlog) {
    close();
    const NativeSocket socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (socket == kInvalid) {
        return Status(ErrorCode::IoFailure,
                      "socket() failed with error " + std::to_string(last_error()));
    }
    const Status reuse = set_reuse(socket);
    if (!reuse.ok()) {
        close_native(socket);
        return reuse;
    }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(port);
    if (::bind(socket, reinterpret_cast<sockaddr*>(&address), static_cast<int>(sizeof(address))) != 0) {
        close_native(socket);
        return Status(ErrorCode::IoFailure,
                      "bind() failed with error " + std::to_string(last_error()));
    }
    if (::listen(socket, backlog) != 0) {
        close_native(socket);
        return Status(ErrorCode::IoFailure,
                      "listen() failed with error " + std::to_string(last_error()));
    }
    sockaddr_in bound{};
#ifdef _WIN32
    int length = static_cast<int>(sizeof(bound));
#else
    socklen_t length = static_cast<socklen_t>(sizeof(bound));
#endif
    if (getsockname(socket, reinterpret_cast<sockaddr*>(&bound), &length) != 0) {
        close_native(socket);
        return Status(ErrorCode::IoFailure,
                      "getsockname() failed with error " + std::to_string(last_error()));
    }
    handle_ = to_handle(socket);
    port_ = ntohs(bound.sin_port);
    return Status{};
}

Result<TcpSocket> TcpListener::accept_one(std::string& peer) {
    if (handle_ == kInvalidSocketHandle) {
        return Status(ErrorCode::IoFailure, "listener is not bound");
    }
    sockaddr_in address{};
#ifdef _WIN32
    int length = static_cast<int>(sizeof(address));
#else
    socklen_t length = static_cast<socklen_t>(sizeof(address));
#endif
    const NativeSocket accepted =
        ::accept(to_native(handle_), reinterpret_cast<sockaddr*>(&address), &length);
    if (accepted == kInvalid) {
        return Status(ErrorCode::IoFailure,
                      "accept() failed with error " + std::to_string(last_error()));
    }
    static_cast<void>(disable_nagle(accepted));
    std::array<char, 32> buffer{};
#ifdef _WIN32
    const char* text = inet_ntop(AF_INET, &address.sin_addr, buffer.data(),
                                 static_cast<DWORD>(buffer.size()));
#else
    const char* text = inet_ntop(AF_INET, &address.sin_addr, buffer.data(),
                                 static_cast<socklen_t>(buffer.size()));
#endif
    peer = text != nullptr ? std::string(text) : std::string("?");
    peer += ":";
    peer += std::to_string(static_cast<std::uint32_t>(ntohs(address.sin_port)));
    return TcpSocket(to_handle(accepted));
}

Result<TcpSocket> connect_loopback(std::uint16_t port, const std::string& host) {
    const NativeSocket socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (socket == kInvalid) {
        return Status(ErrorCode::IoFailure,
                      "socket() failed with error " + std::to_string(last_error()));
    }
    static_cast<void>(disable_nagle(socket));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    if (inet_pton(AF_INET, host.c_str(), &address.sin_addr) != 1) {
        close_native(socket);
        return Status(ErrorCode::InvalidArgument, "invalid loopback address: " + host);
    }
    if (::connect(socket, reinterpret_cast<sockaddr*>(&address), static_cast<int>(sizeof(address))) !=
        0) {
        const int code = last_error();
        close_native(socket);
        return Status(ErrorCode::IoFailure, "connect() failed with error " + std::to_string(code));
    }
    return TcpSocket(to_handle(socket));
}

Status send_all(TcpSocket& socket, std::span<const std::byte> bytes) {
    if (!socket.valid()) {
        return Status(ErrorCode::IoFailure, "cannot send on a closed socket");
    }
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const std::size_t remaining = bytes.size() - offset;
        const std::size_t chunk = remaining > kChunkSize ? kChunkSize : remaining;
#ifdef _WIN32
        const int sent = ::send(to_native(socket.handle()),
                                reinterpret_cast<const char*>(bytes.data() + offset),
                                static_cast<int>(chunk), 0);
#else
        const ssize_t sent = ::send(to_native(socket.handle()),
                                    reinterpret_cast<const char*>(bytes.data() + offset), chunk, 0);
#endif
        if (sent <= 0) {
            return Status(ErrorCode::IoFailure,
                          "send() failed with error " + std::to_string(last_error()));
        }
        offset += static_cast<std::size_t>(sent);
    }
    return Status{};
}

Result<std::vector<std::byte>> receive_exact(TcpSocket& socket, std::size_t count) {
    if (!socket.valid()) {
        return Status(ErrorCode::IoFailure, "cannot receive on a closed socket");
    }
    if (count > kMaximumReceiveAllocation) {
        return Status(ErrorCode::LimitExceeded,
                      "exact-length read of " + std::to_string(count) +
                          " bytes exceeds the maximum receive allocation");
    }
    std::vector<std::byte> buffer(count);
    std::size_t offset = 0;
    while (offset < count) {
        const std::size_t remaining = count - offset;
        const std::size_t chunk = remaining > kChunkSize ? kChunkSize : remaining;
#ifdef _WIN32
        const int received = ::recv(to_native(socket.handle()),
                                    reinterpret_cast<char*>(buffer.data() + offset),
                                    static_cast<int>(chunk), 0);
#else
        const ssize_t received = ::recv(to_native(socket.handle()),
                                        reinterpret_cast<char*>(buffer.data() + offset), chunk, 0);
#endif
        if (received == 0) {
            return Status(ErrorCode::IoFailure, "peer closed the connection");
        }
        if (received < 0) {
            return Status(ErrorCode::IoFailure,
                          "recv() failed with error " + std::to_string(last_error()));
        }
        offset += static_cast<std::size_t>(received);
    }
    return buffer;
}

Result<FrameView> receive_frame(TcpSocket& socket, const Limits& limits,
                                std::vector<std::byte>& storage) {
    // The fixed header is read first so the declared payload length can be
    // checked against Limits::max_frame_size before anything is allocated for
    // it. A frame whose header is not trustworthy is rejected before the
    // checksum stage; decode_frame() repeats every one of these checks on the
    // assembled frame and is the only function that produces a FrameView.
    Result<std::vector<std::byte>> header = receive_exact(socket, kFrameHeaderSize);
    if (!header.ok()) {
        return header.status();
    }
    const std::vector<std::byte>& header_bytes = header.value();
    // The codec validates the fixed prefix (magic, protocol version, message
    // type, flags) and the declared payload length against the configured
    // bound. The payload is read only after that check, so a hostile length can
    // never drive the allocation.
    Result<std::uint32_t> declared = frame_payload_length(
        std::span<const std::byte>(header_bytes.data(), header_bytes.size()), limits);
    if (!declared.ok()) {
        return declared.status();
    }
    std::vector<std::byte> payload;
    if (declared.value() != 0) {
        Result<std::vector<std::byte>> body =
            receive_exact(socket, static_cast<std::size_t>(declared.value()));
        if (!body.ok()) {
            return body.status();
        }
        payload = body.take();
    }
    storage.clear();
    storage.reserve(kFrameHeaderSize + static_cast<std::size_t>(declared.value()));
    storage.insert(storage.end(), header_bytes.begin(), header_bytes.end());
    storage.insert(storage.end(), payload.begin(), payload.end());
    // The frame is parsed exactly once, by the codec that owns the layout, so
    // the header this function inspected and the header the caller sees cannot
    // disagree.
    return decode_frame(std::span<const std::byte>(storage.data(), storage.size()), limits);
}

Status send_frame(TcpSocket& socket, MessageType type, std::uint64_t correlation_id,
                  const AuthorityEnvelope& authority, std::span<const std::byte> payload,
                  const Limits& limits) {
    Result<std::vector<std::byte>> frame = encode_frame(type, correlation_id, authority, payload, limits);
    if (!frame.ok()) {
        return frame.status();
    }
    const std::vector<std::byte>& bytes = frame.value();
    // The layout is the codec's; the frame's own checksum lives at a fixed
    // offset inside it, so a frame built here always leaves with a valid one.
    const std::uint32_t checksum =
        crc32(std::span<const std::byte>(bytes.data(), kChecksumOffset));
    if (bytes.size() < kFrameHeaderSize || checksum != read_u32_le(bytes.data() + kChecksumOffset)) {
        return Status(ErrorCode::ProtocolError, "encoded frame header is inconsistent");
    }
    return send_all(socket, std::span<const std::byte>(bytes.data(), bytes.size()));
}

}  // namespace research_ledger
