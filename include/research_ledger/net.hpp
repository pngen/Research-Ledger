#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "research_ledger/error.hpp"
#include "research_ledger/limits.hpp"
#include "research_ledger/protocol.hpp"

namespace research_ledger {

inline constexpr std::uintptr_t kInvalidSocketHandle =
    static_cast<std::uintptr_t>(~static_cast<std::uintptr_t>(0));

class NetworkRuntime {
public:
    NetworkRuntime();
    ~NetworkRuntime();
    NetworkRuntime(const NetworkRuntime&) = delete;
    NetworkRuntime& operator=(const NetworkRuntime&) = delete;
};

class TcpSocket {
public:
    TcpSocket() noexcept = default;
    explicit TcpSocket(std::uintptr_t handle) noexcept;
    ~TcpSocket();

    TcpSocket(TcpSocket&& other) noexcept;
    TcpSocket& operator=(TcpSocket&& other) noexcept;
    TcpSocket(const TcpSocket&) = delete;
    TcpSocket& operator=(const TcpSocket&) = delete;

    [[nodiscard]] bool valid() const noexcept { return handle_ != kInvalidSocketHandle; }
    [[nodiscard]] std::uintptr_t handle() const noexcept { return handle_; }
    void close() noexcept;
    void shutdown_both() noexcept;

private:
    std::uintptr_t handle_ = kInvalidSocketHandle;
};

class TcpListener {
public:
    TcpListener() noexcept = default;
    ~TcpListener();

    TcpListener(TcpListener&& other) noexcept;
    TcpListener& operator=(TcpListener&& other) noexcept;
    TcpListener(const TcpListener&) = delete;
    TcpListener& operator=(const TcpListener&) = delete;

    Status listen_loopback(std::uint16_t port, int backlog);
    Result<TcpSocket> accept_one(std::string& peer);
    void close() noexcept;

    [[nodiscard]] bool valid() const noexcept { return handle_ != kInvalidSocketHandle; }
    [[nodiscard]] std::uint16_t bound_port() const noexcept { return port_; }

private:
    std::uintptr_t handle_ = kInvalidSocketHandle;
    std::uint16_t port_ = 0;
};

Result<TcpSocket> connect_loopback(std::uint16_t port, const std::string& host);

// Exact-length send/receive. A short read or write is a typed error: partial
// frames never become valid frames.
Status send_all(TcpSocket& socket, std::span<const std::byte> bytes);
Result<std::vector<std::byte>> receive_exact(TcpSocket& socket, std::size_t count);
Result<FrameView> receive_frame(TcpSocket& socket, const Limits& limits, std::vector<std::byte>& storage);
Status send_frame(TcpSocket& socket, MessageType type, std::uint64_t correlation_id,
                  const AuthorityEnvelope& authority, std::span<const std::byte> payload,
                  const Limits& limits);

}  // namespace research_ledger
