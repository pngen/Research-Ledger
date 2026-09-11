#include "research_ledger/cluster.hpp"

#include <algorithm>
#include <atomic>
#include <exception>
#include <fstream>
#include <mutex>
#include <utility>

namespace research_ledger {
namespace {

// A frame must carry the authority of an admitted incarnation. The epoch is
// checked first, so traffic from before a coordinator restart is rejected
// before any worker bookkeeping is consulted.
Status check_frame_authority(const FrameView& frame, CoordinatorEpoch epoch,
                             const AuthorityEnvelope& connection) {
    if (!frame.epoch.valid() || !(frame.epoch == epoch)) {
        return Status(ErrorCode::StaleEpoch, "frame carries a superseded coordinator epoch");
    }
    if (!frame.worker.valid() || !frame.worker_boot.valid()) {
        return Status(ErrorCode::Unauthorized, "frame carries no worker authority");
    }
    if (!(frame.worker == connection.worker) || !(frame.worker_boot == connection.worker_boot)) {
        return Status(ErrorCode::StaleWorker, "frame carries a superseded worker incarnation");
    }
    return Status{};
}

}  // namespace

struct Coordinator::Impl {
    CoordinatorConfig config{};
    Limits limits{};
    TcpListener listener{};
    std::shared_ptr<Ledger> ledger{};
    NetworkRuntime network{};
    std::atomic<bool> shutting_down{false};
    std::mutex mutex{};
    std::mutex persist_mutex{};
    std::vector<std::thread> handlers{};
    std::vector<std::shared_ptr<TcpSocket>> connections{};
    std::vector<std::shared_ptr<TcpSocket>> accepting{};
    // Each admitted connection takes its own counter value, so no two handler
    // threads share mutable state while they hand out boot identities.
    std::atomic<std::uint32_t> next_boot{1};
    std::string shutdown_reason{};
    RecordSequence persisted_sequence{};

    Status persist() {
        if (config.persistence_path.empty()) {
            return Status{};
        }

        // Serialized: a snapshot is written from a copy of committed records
        // taken under the ledger's shared lock, and no two writers may race for
        // the same temporary file.
        std::lock_guard<std::mutex> guard(persist_mutex);
        return ledger->save(config.persistence_path);
    }
};

Coordinator::Coordinator(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

Coordinator::~Coordinator() {
    if (impl_ != nullptr) {
        request_shutdown("coordinator is shutting down");
        join();
        impl_->listener.close();
    }
}

Result<std::unique_ptr<Coordinator>> Coordinator::start(const CoordinatorConfig& config) {
    auto impl = std::make_unique<Impl>();
    impl->config = config;
    impl->limits = config.limits;

    LedgerConfig ledger_config;
    ledger_config.limits = config.limits;
    ledger_config.require_worker_admission = true;

    bool restored = false;
    if (!config.persistence_path.empty()) {
        std::ifstream probe(config.persistence_path, std::ios::binary);
        restored = probe.good();
        probe.close();
    }
    Result<std::shared_ptr<Ledger>> ledger =
        restored ? Ledger::load(config.persistence_path, ledger_config)
                 : Ledger::create(ledger_config);
    if (!ledger.ok()) {
        return ledger.status();
    }
    impl->ledger = ledger.value();
    if (restored) {
        // A restarted coordinator is a new authority epoch. Prior-epoch traffic
        // and prior-epoch worker incarnations are no longer current.
        auto epoch = impl->ledger->advance_epoch();
        if (!epoch.ok()) {
            return epoch.status();
        }
    }
    impl->persist();

    auto listener_status = impl->listener.listen_loopback(config.port, 32);
    if (!listener_status.ok()) {
        return listener_status;
    }
    auto coordinator = std::unique_ptr<Coordinator>(new Coordinator(std::move(impl)));
    coordinator->ledger_ = coordinator->impl_->ledger;
    coordinator->port_ = coordinator->impl_->listener.bound_port();
    return coordinator;
}

CoordinatorEpoch Coordinator::epoch() const {
    if (ledger_ == nullptr) {
        return CoordinatorEpoch{};
    }
    return ledger_->epoch();
}

Status Coordinator::serve() {
    if (impl_ == nullptr) {
        return Status(ErrorCode::NotReady, "coordinator is not started");
    }
    for (;;) {
        if (impl_->shutting_down.load()) {
            break;
        }
        std::string peer;
        auto socket = impl_->listener.accept_one(peer);
        if (!socket.ok()) {
            if (impl_->shutting_down.load()) {
                break;
            }
            break;
        }
        auto shared = std::make_shared<TcpSocket>(socket.take());
        {
            std::lock_guard<std::mutex> guard(impl_->mutex);
            impl_->connections.push_back(shared);
        }
        std::thread handler([this, shared]() {
            try {
            AuthorityEnvelope connection{};
            Limits limits = impl_->limits;
            std::vector<std::byte> storage;
            bool hello_done = false;
            for (;;) {
                auto frame = receive_frame(*shared, limits, storage);
                if (!frame.ok()) {
                    break;
                }
                const FrameView view = frame.value();
                Status authority = Status{};
                if (!hello_done) {
                    if (view.type != MessageType::Hello) {
                        authority = Status(ErrorCode::ProtocolError,
                                           "a connection must start with a hello frame");
                    } else {
                        auto hello = decode_hello(view.payload, limits);
                        if (!hello.ok()) {
                            authority = hello.status();
                        } else if (!hello.value().worker.valid()) {
                            authority = Status(ErrorCode::InvalidIdentity,
                                               "hello carries an invalid worker identity");
                        } else {
                            const std::uint32_t boot_counter =
                                impl_->next_boot.fetch_add(1, std::memory_order_relaxed);
                            auto admitted = impl_->ledger->admit_worker_incarnation(
                                hello.value().worker, hello.value().worker_boot, boot_counter);
                            if (!admitted.ok()) {
                                authority = admitted.status();
                            } else {
                                connection = admitted.take();
                                hello_done = true;
                                HelloAckMessage ack;
                                ack.epoch = connection.epoch;
                                ack.generation = connection.ledger;
                                ack.worker_boot = connection.worker_boot;
                                ack.coordinator = impl_->config.identity;
                                auto payload = encode_hello_ack(ack, limits);
                                if (payload.ok()) {
                                    send_frame(*shared, MessageType::HelloAck, view.correlation_id,
                                               connection, payload.value(), limits);
                                }
                                continue;
                            }
                        }
                    }
                    ErrorMessage message;
                    message.code = authority.code;
                    message.detail = authority.message;
                    auto payload = encode_error(message, limits);
                    if (payload.ok()) {
                        send_frame(*shared, MessageType::ErrorReply, view.correlation_id, connection,
                                   payload.value(), limits);
                    }
                    break;
                }

                authority = check_frame_authority(view, impl_->ledger->epoch(), connection);
                if (!authority.ok()) {
                    ErrorMessage message;
                    message.code = authority.code;
                    message.detail = authority.message;
                    auto payload = encode_error(message, limits);
                    if (payload.ok()) {
                        send_frame(*shared, MessageType::ErrorReply, view.correlation_id, connection,
                                   payload.value(), limits);
                    }
                    break;
                }

                switch (view.type) {
                    case MessageType::AppendRequest: {
                        auto request = decode_append_request(view.payload, limits);
                        if (!request.ok()) {
                            ErrorMessage message;
                            message.code = request.code();
                            message.detail = request.message();
                            auto payload = encode_error(message, limits);
                            if (payload.ok()) {
                                send_frame(*shared, MessageType::ErrorReply, view.correlation_id,
                                           connection, payload.value(), limits);
                            }
                            break;
                        }
                        AppendReplyMessage reply;
                        auto outcomes = impl_->ledger->append_batch_with_authority(
                            request.value().drafts, connection);
                        if (!outcomes.ok()) {
                            reply.code = outcomes.code();
                            reply.detail = outcomes.message();
                        } else {
                            reply.outcomes = outcomes.value();
                        }
                        auto payload = encode_append_reply(reply, limits);
                        if (payload.ok()) {
                            send_frame(*shared, MessageType::AppendReply, view.correlation_id,
                                       connection, payload.value(), limits);
                        }
                        if (outcomes.ok() && !outcomes.value().empty() && impl_->config.persist_on_append) {
                            impl_->persist();
                        }
                        break;
                    }
                    case MessageType::StatusRequest: {
                        auto snapshot = impl_->ledger->snapshot();
                        auto stats = snapshot.stats();
                        if (!stats.ok()) {
                            break;
                        }
                        StatusReplyMessage reply;
                        reply.last_sequence = stats.value().last_sequence;
                        reply.generation = stats.value().generation;
                        reply.epoch = stats.value().epoch;
                        reply.chain_digest = stats.value().chain_digest;
                        reply.sessions = stats.value().sessions;
                        reply.hypotheses = stats.value().hypotheses;
                        reply.experiments = stats.value().experiments;
                        reply.attempts = stats.value().attempts;
                        reply.results = stats.value().results;
                        reply.decisions = stats.value().decisions;
                        reply.live_workers = stats.value().live_workers;
                        auto payload = encode_status_reply(reply, limits);
                        if (payload.ok()) {
                            send_frame(*shared, MessageType::StatusReply, view.correlation_id,
                                       connection, payload.value(), limits);
                        }
                        break;
                    }
                    case MessageType::ShutdownRequest: {
                        auto request = decode_shutdown(view.payload, limits);
                        ShutdownMessage reply;
                        reply.reason = request.ok() ? request.value().reason : std::string{};
                        auto payload = encode_shutdown(reply, limits);
                        if (payload.ok()) {
                            send_frame(*shared, MessageType::ShutdownReply, view.correlation_id,
                                       connection, payload.value(), limits);
                        }
                        impl_->persist();
                        request_shutdown("shutdown requested by a worker");
                        break;
                    }
                    default: {
                        ErrorMessage message;
                        message.code = ErrorCode::ProtocolError;
                        message.detail = "unexpected message type";
                        auto payload = encode_error(message, limits);
                        if (payload.ok()) {
                            send_frame(*shared, MessageType::ErrorReply, view.correlation_id,
                                       connection, payload.value(), limits);
                        }
                        break;
                    }
                }
            }
            if (hello_done) {
                impl_->ledger->fence_worker_boot(connection.worker, connection.worker_boot);
            }
            } catch (const std::exception&) {
                // One connection failed unexpectedly. The coordinator is a
                // server: it closes that connection and keeps serving.
            } catch (...) {
            }
            shared->close();
        });
        {
            std::lock_guard<std::mutex> guard(impl_->mutex);
            impl_->handlers.push_back(std::move(handler));
        }
    }
    Status result{};
    if (!impl_->shutting_down.load()) {
        result = Status(ErrorCode::IoFailure, "coordinator listener stopped accepting connections");
    }
    impl_->persist();
    // Stop every live connection so that handler threads can finish, then join
    // them. No lock is held while joining.
    std::vector<std::shared_ptr<TcpSocket>> connections;
    {
        std::lock_guard<std::mutex> guard(impl_->mutex);
        connections = impl_->connections;
    }
    for (const std::shared_ptr<TcpSocket>& socket : connections) {
        socket->shutdown_both();
    }
    std::vector<std::thread> handlers;
    {
        std::lock_guard<std::mutex> guard(impl_->mutex);
        handlers.swap(impl_->handlers);
    }
    for (std::thread& thread : handlers) {
        if (thread.joinable()) {
            thread.join();
        }
    }
    {
        std::lock_guard<std::mutex> guard(impl_->mutex);
        impl_->connections.clear();
    }
    return result;
}

Status Coordinator::request_shutdown(const std::string& reason) {
    if (impl_ == nullptr) {
        return Status(ErrorCode::NotReady, "coordinator is not started");
    }
    {
        std::lock_guard<std::mutex> guard(impl_->mutex);
        impl_->shutdown_reason = reason;
    }
    const bool already = impl_->shutting_down.exchange(true);
    if (!already) {
        impl_->listener.close();
        std::vector<std::shared_ptr<TcpSocket>> connections;
        {
            std::lock_guard<std::mutex> guard(impl_->mutex);
            connections = impl_->connections;
        }
        for (const std::shared_ptr<TcpSocket>& socket : connections) {
            socket->shutdown_both();
        }
    }
    return Status{};
}

Status Coordinator::join() {
    if (impl_ == nullptr) {
        return Status{};
    }
    std::vector<std::thread> handlers;
    {
        std::lock_guard<std::mutex> guard(impl_->mutex);
        handlers.swap(impl_->handlers);
    }
    for (std::thread& thread : handlers) {
        if (thread.joinable()) {
            thread.join();
        }
    }
    return Status{};
}

WorkerClient::WorkerClient() = default;

WorkerClient::~WorkerClient() { close(); }

void WorkerClient::close() noexcept {
    if (socket_ != nullptr) {
        socket_->shutdown_both();
        socket_->close();
    }
}

Result<std::unique_ptr<WorkerClient>> WorkerClient::connect(const WorkerConfig& config) {
    if (!config.worker.valid()) {
        return Status(ErrorCode::InvalidIdentity, "worker identity is not valid");
    }
    auto socket = connect_loopback(config.port, config.host);
    if (!socket.ok()) {
        return socket.status();
    }
    std::unique_ptr<WorkerClient> client(new WorkerClient());
    client->socket_ = std::make_shared<TcpSocket>(socket.take());
    client->limits_ = config.limits;
    client->authority_.ledger = LedgerGeneration::first();
    client->authority_.epoch = CoordinatorEpoch::first();
    client->authority_.worker = config.worker;
    client->authority_.worker_boot = config.worker_boot;

    HelloMessage hello;
    hello.worker = config.worker;
    hello.worker_boot = config.worker_boot;
    hello.authority = config.authority;
    auto payload = encode_hello(hello, config.limits);
    if (!payload.ok()) {
        return payload.status();
    }
    Status sent = send_frame(*client->socket_, MessageType::Hello, client->next_correlation_++,
                             client->authority_, payload.value(), config.limits);
    if (!sent.ok()) {
        return sent;
    }
    auto reply = client->receive_raw_frame();
    if (!reply.ok()) {
        return reply.status();
    }
    std::vector<std::byte> storage = reply.take();
    auto frame = decode_frame(storage, config.limits);
    if (!frame.ok()) {
        return frame.status();
    }
    if (frame.value().type == MessageType::ErrorReply) {
        auto message = decode_error(frame.value().payload, config.limits);
        if (!message.ok()) {
            return message.status();
        }
        return Status(message.value().code, message.value().detail);
    }
    if (frame.value().type != MessageType::HelloAck) {
        return Status(ErrorCode::ProtocolError, "coordinator did not acknowledge the handshake");
    }
    auto ack = decode_hello_ack(frame.value().payload, config.limits);
    if (!ack.ok()) {
        return ack.status();
    }
    if (ack.value().code != ErrorCode::Ok) {
        return Status(ack.value().code, ack.value().detail);
    }
    client->authority_.ledger = ack.value().generation;
    client->authority_.epoch = ack.value().epoch;
    client->authority_.worker_boot = ack.value().worker_boot;
    client->acknowledgement_ = ack.value();
    client->handshake_done_ = true;
    return client;
}

Result<HelloAckMessage> WorkerClient::handshake() {
    if (!handshake_done_) {
        return Status(ErrorCode::NotReady, "worker client did not complete its handshake");
    }
    return acknowledgement_;
}

Result<std::vector<AppendOutcome>> WorkerClient::append(const std::vector<RecordDraft>& drafts) {
    if (socket_ == nullptr || !socket_->valid()) {
        return Status(ErrorCode::IoFailure, "worker client is not connected");
    }
    AppendRequestMessage request;
    request.drafts = drafts;
    auto payload = encode_append_request(request, limits_);
    if (!payload.ok()) {
        return payload.status();
    }
    const std::uint64_t correlation = next_correlation_++;
    Status sent = send_frame(*socket_, MessageType::AppendRequest, correlation, authority_,
                             payload.value(), limits_);
    if (!sent.ok()) {
        return sent;
    }
    auto reply = receive_raw_frame();
    if (!reply.ok()) {
        return reply.status();
    }
    std::vector<std::byte> storage = reply.take();
    auto frame = decode_frame(storage, limits_);
    if (!frame.ok()) {
        return frame.status();
    }
    if (frame.value().correlation_id != correlation) {
        return Status(ErrorCode::ProtocolError, "reply correlation does not match the request");
    }
    if (frame.value().type == MessageType::ErrorReply) {
        auto message = decode_error(frame.value().payload, limits_);
        if (!message.ok()) {
            return message.status();
        }
        return Status(message.value().code, message.value().detail);
    }
    if (frame.value().type != MessageType::AppendReply) {
        return Status(ErrorCode::ProtocolError, "unexpected reply to an append request");
    }
    auto message = decode_append_reply(frame.value().payload, limits_);
    if (!message.ok()) {
        return message.status();
    }
    if (message.value().code != ErrorCode::Ok) {
        return Status(message.value().code, message.value().detail);
    }
    return message.value().outcomes;
}

Result<StatusReplyMessage> WorkerClient::status() {
    if (socket_ == nullptr || !socket_->valid()) {
        return Status(ErrorCode::IoFailure, "worker client is not connected");
    }
    const std::uint64_t correlation = next_correlation_++;
    Status sent = send_frame(*socket_, MessageType::StatusRequest, correlation, authority_, {}, limits_);
    if (!sent.ok()) {
        return sent;
    }
    auto reply = receive_raw_frame();
    if (!reply.ok()) {
        return reply.status();
    }
    std::vector<std::byte> storage = reply.take();
    auto frame = decode_frame(storage, limits_);
    if (!frame.ok()) {
        return frame.status();
    }
    if (frame.value().type == MessageType::ErrorReply) {
        auto message = decode_error(frame.value().payload, limits_);
        if (!message.ok()) {
            return message.status();
        }
        return Status(message.value().code, message.value().detail);
    }
    if (frame.value().type != MessageType::StatusReply) {
        return Status(ErrorCode::ProtocolError, "unexpected reply to a status request");
    }
    return decode_status_reply(frame.value().payload, limits_);
}

Status WorkerClient::shutdown(const std::string& reason) {
    if (socket_ == nullptr || !socket_->valid()) {
        return Status(ErrorCode::IoFailure, "worker client is not connected");
    }
    ShutdownMessage message;
    message.reason = reason;
    auto payload = encode_shutdown(message, limits_);
    if (!payload.ok()) {
        return payload.status();
    }
    Status sent = send_frame(*socket_, MessageType::ShutdownRequest, next_correlation_++,
                             authority_, payload.value(), limits_);
    if (!sent.ok()) {
        return sent;
    }
    auto reply = receive_raw_frame();
    if (!reply.ok()) {
        return reply.status();
    }
    return Status{};
}

Status WorkerClient::send_raw_frame(MessageType type, std::uint64_t correlation_id,
                                    const AuthorityEnvelope& authority,
                                    std::span<const std::byte> payload) {
    if (socket_ == nullptr || !socket_->valid()) {
        return Status(ErrorCode::IoFailure, "worker client is not connected");
    }
    return send_frame(*socket_, type, correlation_id, authority, payload, limits_);
}

Result<std::vector<std::byte>> WorkerClient::receive_raw_frame() {
    if (socket_ == nullptr || !socket_->valid()) {
        return Status(ErrorCode::IoFailure, "worker client is not connected");
    }
    auto header = receive_exact(*socket_, kFrameHeaderSize);
    if (!header.ok()) {
        return header.status();
    }
    auto declared = frame_payload_length(header.value(), limits_);
    if (!declared.ok()) {
        return declared.status();
    }
    std::vector<std::byte> frame = header.take();
    if (declared.value() != 0) {
        auto payload = receive_exact(*socket_, declared.value());
        if (!payload.ok()) {
            return payload.status();
        }
        const std::vector<std::byte>& bytes = payload.value();
        frame.insert(frame.end(), bytes.begin(), bytes.end());
    }
    return frame;
}

}  // namespace research_ledger
