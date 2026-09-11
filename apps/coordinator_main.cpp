// apps/coordinator_main.cpp
//
// Reference coordinator process for the multiprocess proof.
//
// The coordinator owns the committed ledger, the coordinator epoch and the live
// worker registry. It is a real operating system process: the proof starts it,
// kills it and restarts it on the same state path, and observes that the
// committed history and the logical digest of the reconstructed state survive.
//
//   research-ledger-coordinator [--port N] [--state PATH] [--identity NAME]
//
// The single line
//
//   READY <port> <epoch> <generation>
//
// is printed and flushed once the listener is bound, so a caller never has to
// guess when the coordinator is reachable.
//
// Standard input is a command stream handled by a separate thread while the
// accept loop runs on the main thread. The line "shutdown" requests a clean
// shutdown; end of input is not a shutdown request, so a caller that closes the
// coordinator's command stream does not stop it.

#include <charconv>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include "research_ledger/cluster.hpp"
#include "research_ledger/error.hpp"
#include "research_ledger/ledger.hpp"

namespace rl = research_ledger;

namespace {

struct Options {
    std::uint16_t port = 0;  // zero selects an ephemeral port
    std::string state{};
    std::string identity = "research-ledger-coordinator";
};

bool parse_unsigned(std::string_view text, std::uint64_t& out) {
    if (text.empty()) {
        return false;
    }
    std::uint64_t value = 0;
    const char* const first = text.data();
    const char* const last = text.data() + text.size();
    const std::from_chars_result parsed = std::from_chars(first, last, value);
    if (parsed.ec != std::errc{} || parsed.ptr != last) {
        return false;
    }
    out = value;
    return true;
}

void print_usage() {
    std::cout << "usage: research-ledger-coordinator [--port N] [--state PATH] [--identity NAME]"
              << std::endl;
}

bool parse_options(int argc, char** argv, Options& options) {
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument(argv[index]);
        if (index + 1 >= argc) {
            return false;
        }
        const std::string_view value(argv[index + 1]);
        if (argument == "--port") {
            std::uint64_t parsed = 0;
            if (!parse_unsigned(value, parsed) || parsed > 65535) {
                return false;
            }
            options.port = static_cast<std::uint16_t>(parsed);
        } else if (argument == "--state") {
            options.state = std::string(value);
        } else if (argument == "--identity") {
            options.identity = std::string(value);
        } else {
            return false;
        }
        ++index;
    }
    return true;
}

std::string strip_carriage_return(std::string line) {
    if (!line.empty() && line.back() == '\r') {
        line.pop_back();
    }
    return line;
}

}  // namespace

int main(int argc, char** argv) {
    Options options;
    if (!parse_options(argc, argv, options)) {
        print_usage();
        return 2;
    }

    rl::CoordinatorConfig config;
    config.persistence_path = options.state;
    config.identity = options.identity;
    config.port = options.port;

    auto started = rl::Coordinator::start(config);
    if (!started.ok()) {
        std::cout << "ERROR " << rl::error_code_name(started.code()) << " " << started.message()
                  << std::endl;
        return 1;
    }
    // Shared rather than owned by main: the command thread below keeps the
    // coordinator alive until it finishes, so an exiting process can never
    // destroy the coordinator while that thread is still parked in a blocking
    // read of the command stream.
    std::shared_ptr<rl::Coordinator> coordinator = started.take();

    std::cout << "READY " << coordinator->port() << " " << coordinator->epoch().value() << " "
              << coordinator->ledger()->generation().value() << std::endl;

    std::thread commands([running = coordinator]() {
        std::string line;
        while (std::getline(std::cin, line)) {
            line = strip_carriage_return(std::move(line));
            if (line == "shutdown") {
                static_cast<void>(running->request_shutdown("shutdown requested on the command stream"));
            }
            // Every other line is ignored: the command stream is not
            // authoritative and never mutates the ledger.
        }
        // End of input is not a shutdown request: the coordinator keeps
        // serving until it is asked to stop.
    });
    commands.detach();

    const rl::Status served = coordinator->serve();
    if (!served.ok()) {
        std::cout << "ERROR " << rl::error_code_name(served.code) << " " << served.message
                  << std::endl;
        return 1;
    }
    return 0;
}
