#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "research_ledger/error.hpp"

namespace research_ledger {

// Real operating system processes are used for every distributed claim: the
// multiprocess proof starts the coordinator and the workers as independent
// processes, kills a worker with real process termination, and restarts the
// coordinator as a new process.
class ChildProcess {
public:
    ChildProcess() noexcept;
    ~ChildProcess();

    ChildProcess(const ChildProcess&) = delete;
    ChildProcess& operator=(const ChildProcess&) = delete;
    ChildProcess(ChildProcess&& other) noexcept;
    ChildProcess& operator=(ChildProcess&& other) noexcept;

    // Starts a process with stdout and stderr merged into one pipe and no
    // console window.
    static Result<ChildProcess> spawn(const std::string& program,
                                      const std::vector<std::string>& arguments);

    // Blocking read of one line. Returns an error when the stream ends. There
    // is no timeout anywhere in the runtime's process handling.
    Result<std::string> read_line();

    // Writes a line to the child's standard input.
    Status write_line(const std::string& line);

    // Terminates the process with real OS termination (TerminateProcess on
    // Windows, SIGKILL elsewhere).
    Status terminate();
    Status terminate_tree();

    [[nodiscard]] bool running();
    Result<int> wait();
    void close() noexcept;
    [[nodiscard]] std::uint64_t process_id() const noexcept { return process_id_; }

private:
    struct Impl;
    std::unique_ptr<Impl> impl_{};
    std::uint64_t process_id_ = 0;
};

std::uint64_t current_process_id() noexcept;

// Path of the running executable, used by the proof to start sibling programs.
Result<std::string> current_executable_path();

}  // namespace research_ledger
