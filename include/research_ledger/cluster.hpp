#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "research_ledger/error.hpp"
#include "research_ledger/ledger.hpp"
#include "research_ledger/limits.hpp"
#include "research_ledger/net.hpp"
#include "research_ledger/protocol.hpp"

namespace research_ledger {

struct CoordinatorConfig {
    std::string persistence_path{};
    std::string identity = "research-ledger-coordinator";
    std::uint16_t port = 0;  // 0 selects an ephemeral port
    // Persist the committed ledger after every successful append batch. The
    // snapshot format is a full image, so this is O(committed records) per
    // batch: correctness first, and the cost is stated rather than hidden.
    bool persist_on_append = true;
    Limits limits{};
};

// Reference coordinator: authoritative ingestion point for multiple worker
// processes over framed TCP. The coordinator owns the committed ledger, the
// coordinator epoch and the live worker registry.
class Coordinator {
public:
    static Result<std::unique_ptr<Coordinator>> start(const CoordinatorConfig& config);
    ~Coordinator();

    Coordinator(const Coordinator&) = delete;
    Coordinator& operator=(const Coordinator&) = delete;

    [[nodiscard]] std::uint16_t port() const noexcept { return port_; }
    [[nodiscard]] CoordinatorEpoch epoch() const;
    [[nodiscard]] std::shared_ptr<Ledger> ledger() const noexcept { return ledger_; }

    // Blocking accept loop. Returns when shutdown is requested or when the
    // listener is closed.
    Status serve();
    Status request_shutdown(const std::string& reason);
    Status join();

private:
    struct Impl;
    explicit Coordinator(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_{};
    std::shared_ptr<Ledger> ledger_{};
    std::uint16_t port_ = 0;
};

struct WorkerConfig {
    std::string host = "127.0.0.1";
    std::uint16_t port = 0;
    WorkerId worker{};
    WorkerBootId worker_boot{};
    std::string authority{};
    Limits limits{};
};

// Worker side of the ingestion boundary. Every request carries the worker
// incarnation it was issued under, so a fenced incarnation is rejected by the
// coordinator instead of being trusted silently.
//
// A WorkerClient owns one connection and is not thread-safe: use one client per
// thread. The Coordinator is safe to serve many connections at once.
class WorkerClient {
public:
    static Result<std::unique_ptr<WorkerClient>> connect(const WorkerConfig& config);
    ~WorkerClient();

    WorkerClient(const WorkerClient&) = delete;
    WorkerClient& operator=(const WorkerClient&) = delete;

    Result<HelloAckMessage> handshake();
    Result<std::vector<AppendOutcome>> append(const std::vector<RecordDraft>& drafts);
    Result<StatusReplyMessage> status();
    Status shutdown(const std::string& reason);
    // Raw frame access used by the adversarial tests to send bytes the
    // coordinator must reject.
    Status send_raw_frame(MessageType type, std::uint64_t correlation_id,
                          const AuthorityEnvelope& authority, std::span<const std::byte> payload);
    Result<std::vector<std::byte>> receive_raw_frame();
    [[nodiscard]] const AuthorityEnvelope& authority() const noexcept { return authority_; }
    void close() noexcept;

private:
    WorkerClient();
    std::shared_ptr<TcpSocket> socket_{};
    AuthorityEnvelope authority_{};
    HelloAckMessage acknowledgement_{};
    bool handshake_done_ = false;
    Limits limits_{};
    std::uint64_t next_correlation_ = 1;
};

}  // namespace research_ledger
