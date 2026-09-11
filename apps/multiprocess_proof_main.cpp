// apps/multiprocess_proof_main.cpp
//
// Real multiprocess proof for the research ledger.
//
// Every process here is a real operating system process started through
// research_ledger::ChildProcess, and every claim below is checked against the
// coordinator, against the persisted snapshot, or against a ledger reloaded
// from that snapshot. Nothing is simulated in-process and nothing is inferred
// from a timeout: every wait is a blocking read of a line the other process
// promised to print.
//
//   research-ledger-multiprocess-proof [--tools DIR] [--work DIR]
//
// --tools defaults to the directory of the running executable (the three
// programs are built side by side), --work to a fresh directory under the
// system temporary directory that the proof creates and removes again.
//
// Every child is driven over the standard streams research_ledger::ChildProcess
// creates for it: the proof reads the child's readiness line and every response
// with ChildProcess::read_line and sends every command with
// ChildProcess::write_line. Standard input of a child is command input and its
// standard output is the response stream, exactly as the programs document.
//
// Steps, in order:
//
//   1. worker A creates the session and commits the main, failure and retry
//      scenarios (27 records).
//   2. worker B, a different worker identity, accepts the retry result: one
//      atomic batch holding the acceptance decision and the status change.
//   3. worker A's OS process is killed with real termination; the 29 records it
//      committed are still there.
//   4. a fresh incarnation of worker A's identity is admitted with a different
//      worker boot identity and appends successfully (30 records).
//   5. traffic claiming the killed incarnation's boot identity is rejected with
//      STALE_WORKER, and a raw frame claiming the pre-restart coordinator epoch
//      is rejected with STALE_EPOCH.
//   6. the coordinator OS process is killed and restarted on the same state
//      path: the epoch is higher, the persisted history is intact, the logical
//      digest of the reconstructed state is unchanged, a fresh worker appends,
//      and the accepted result reconstructs to the same lineage.
//   7. after a graceful shutdown the persisted snapshot loads, verifies, still
//      carries the failed attempt, and replays to the same logical digest as
//      the reloaded ledger.
//
// Durability: the coordinator persists a committed batch before it sends the
// append reply, so an acknowledged append is already in the snapshot. The proof
// still follows the last append on a connection with a status request on that
// same connection, because the reply to that request is an end-to-end check of
// the committed prefix (last sequence, chain digest, epoch and worker boot) that
// the later steps compare against, and because it proves the connection is
// still serving before the process is killed.
//
// Documented identity scheme (implemented by apps/worker_main.cpp):
//   create-session 1     Session 1
//   scenario-main    1 1 Hypothesis 1, Branch 1 (Root), Experiment 1, Attempt 1,
//                        ModelCall 1, ToolCall 1, Artifact 1 (Intermediate),
//                        Metric 1, Observation 1, Artifact 2 (ResultArtifact),
//                        Result 1
//   scenario-failure 1 1 Hypothesis 2, Branch 2 (Fork of 1), Experiment 2,
//                        Attempt 2, Failure 1
//   scenario-retry   1 1 Branch 3 (Retry of 2), Experiment 3, Attempt 3,
//                        ModelCall 2, Artifact 3 (ResultArtifact), Result 2
//   accept 2 1           Decision 1 over Result 2

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "research_ledger/digest.hpp"
#include "research_ledger/error.hpp"
#include "research_ledger/ledger.hpp"
#include "research_ledger/persistence.hpp"
#include "research_ledger/process.hpp"
#include "research_ledger/replay.hpp"

namespace fs = std::filesystem;
namespace rl = research_ledger;

namespace {

// --- controlled failure -----------------------------------------------------
//
// A step that does not hold is reported as one line and a non-zero exit code.
// The proof never crashes and never aborts: everything it needs to say is a
// value it checked.

struct Failure {
    std::string detail;
};

[[noreturn]] void fail(std::string detail) { throw Failure{std::move(detail)}; }

// --- documented identities and prefix lengths -------------------------------

constexpr std::uint64_t kWorkerA = 11;
constexpr std::uint64_t kWorkerB = 12;
constexpr std::uint64_t kWorkerC = 13;
constexpr std::uint64_t kWorkerD = 14;

constexpr std::uint64_t kRecordsAfterScenarios = 27;
constexpr std::uint64_t kRecordsAfterAccept = 29;
constexpr std::uint64_t kRecordsAfterReincarnation = 30;
constexpr std::uint64_t kRecordsAfterRestartAppend = 31;

constexpr rl::ResultId kRetryResult = rl::ResultId::from_value(2);
constexpr rl::DecisionId kAcceptance = rl::DecisionId::from_value(1);
constexpr rl::AttemptId kFailedAttempt = rl::AttemptId::from_value(2);
constexpr rl::FailureId kFailure = rl::FailureId::from_value(1);
constexpr rl::ResearchSessionId kSession = rl::ResearchSessionId::from_value(1);

// --- text helpers -----------------------------------------------------------

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

std::vector<std::string> split_tokens(const std::string& line) {
    std::vector<std::string> tokens;
    std::size_t index = 0;
    while (index < line.size()) {
        while (index < line.size() && (line[index] == ' ' || line[index] == '\t')) {
            ++index;
        }
        if (index >= line.size()) {
            break;
        }
        const std::size_t start = index;
        while (index < line.size() && line[index] != ' ' && line[index] != '\t') {
            ++index;
        }
        tokens.push_back(line.substr(start, index - start));
    }
    return tokens;
}

std::optional<std::string> field_text(std::string_view line, std::string_view key) {
    const std::string needle = " " + std::string(key) + "=";
    const std::size_t position = line.find(needle);
    if (position == std::string_view::npos) {
        return std::nullopt;
    }
    const std::size_t start = position + needle.size();
    const std::size_t end = line.find(' ', start);
    const std::size_t length = end == std::string_view::npos ? std::string_view::npos : end - start;
    return std::string(line.substr(start, length));
}

std::uint64_t require_number(std::string_view line, std::string_view key, const std::string& what) {
    const std::optional<std::string> text = field_text(line, key);
    if (!text.has_value()) {
        fail(what + ": the line '" + std::string(line) + "' does not carry " + std::string(key) + "=");
    }
    std::uint64_t value = 0;
    if (!parse_unsigned(*text, value)) {
        fail(what + ": field " + std::string(key) + " of '" + std::string(line) + "' is not a number");
    }
    return value;
}

std::string require_text(std::string_view line, std::string_view key, const std::string& what) {
    const std::optional<std::string> text = field_text(line, key);
    if (!text.has_value()) {
        fail(what + ": the line '" + std::string(line) + "' does not carry " + std::string(key) + "=");
    }
    return *text;
}

std::string describe(const rl::Status& status) {
    std::string text(rl::error_code_name(status.code));
    if (!status.message.empty()) {
        text += " (";
        text += status.message;
        text += ")";
    }
    return text;
}

std::string number_text(std::uint64_t value) { return std::to_string(value); }


// --- child processes --------------------------------------------------------

class Child {
public:
    Child(rl::ChildProcess process, std::string label)
        : process_(std::move(process)), label_(std::move(label)) {}

    Child(const Child&) = delete;
    Child& operator=(const Child&) = delete;

    // Starts a real process with research_ledger::ChildProcess::spawn: standard
    // output and standard error of the child are merged into one pipe and
    // standard input is a pipe the parent writes commands to.
    static std::unique_ptr<Child> spawn(const fs::path& program,
                                        const std::vector<std::string>& arguments,
                                        const std::string& label) {
        auto spawned = rl::ChildProcess::spawn(program.string(), arguments);
        if (!spawned.ok()) {
            fail("cannot start " + label + ": " + describe(spawned.status()));
        }
        return std::make_unique<Child>(spawned.take(), label);
    }

    void send(const std::string& line) {
        const rl::Status status = process_.write_line(line);
        if (!status.ok()) {
            fail("cannot send '" + line + "' to " + label_ + ": " + describe(status));
        }
    }

    std::string receive() {
        auto line = process_.read_line();
        if (!line.ok()) {
            fail(label_ + " ended its output stream before it answered: " + describe(line.status()));
        }
        return line.take();
    }

    // Real termination: the process is ended by the operating system and the
    // exit is reaped, so a killed process can never be mistaken for one that
    // shut itself down.
    void kill(const std::string& what) {
        if (process_id() == 0) {
            fail(what + ": " + label_ + " has no live process to terminate");
        }
        if (!process_.running()) {
            fail(what + ": " + label_ + " had already exited before it was terminated");
        }
        const rl::Status terminated = process_.terminate_tree();
        if (!terminated.ok()) {
            fail(what + ": terminating " + label_ + " failed: " + describe(terminated));
        }
        const int code = wait_or_fail(what);
        if (code == 0) {
            fail(what + ": " + label_ + " exited cleanly instead of being terminated");
        }
        close();
    }

    int wait_or_fail(const std::string& what) {
        auto code = process_.wait();
        if (!code.ok()) {
            fail(what + ": waiting for " + label_ + " failed: " + describe(code.status()));
        }
        return code.value();
    }

    void close() noexcept { process_.close(); }

    void stop() noexcept {
        if (process_id() == 0) {
            return;
        }
        if (process_.running()) {
            static_cast<void>(process_.terminate_tree());
        }
        static_cast<void>(process_.wait());
        close();
    }

    [[nodiscard]] std::uint64_t process_id() const noexcept { return process_.process_id(); }
    [[nodiscard]] const std::string& label() const noexcept { return label_; }

private:
    rl::ChildProcess process_;
    std::string label_;
};

// --- observed state ---------------------------------------------------------

struct WorkerStatus {
    std::uint64_t last = 0;
    std::string chain{};
    std::uint64_t epoch = 0;
    std::uint64_t boot = 0;
};

struct WorkerHandle {
    Child* child = nullptr;
    std::uint64_t id = 0;
    std::uint64_t boot = 0;
    std::uint64_t epoch = 0;
    std::uint64_t generation = 0;
};

struct PersistedState {
    std::uint64_t records = 0;
    std::uint64_t epoch = 0;
    rl::Digest chain{};
    rl::Digest logical{};
    rl::Digest lineage{};
    bool accepted = false;
    bool reconstructable = false;
    bool complete = false;
    bool failed_attempt_present = false;
    bool failure_present = false;
    std::uint64_t cited_evidence = 0;
    std::uint64_t decisions = 0;
};

// --- the proof --------------------------------------------------------------

class Proof {
public:
    Proof(fs::path tools, fs::path work, std::string state)
        : tools_(std::move(tools)), work_(std::move(work)), state_(std::move(state)) {}

    ~Proof() {
        for (const std::unique_ptr<Child>& child : children_) {
            child->stop();
        }
        children_.clear();
        remove_work_directory();
    }

    void run() {
        const fs::path coordinator_program = resolve_tool("research-ledger-coordinator");
        const fs::path worker_program = resolve_tool("research-ledger-worker");

        std::cout << "# tools " << tools_.string() << std::endl;
        std::cout << "# work " << work_.string() << std::endl;

        // --- step 1 ---------------------------------------------------------
        start_coordinator(coordinator_program, "the coordinator is started");
        const std::uint64_t first_epoch = epoch_;

        WorkerHandle worker_a = start_worker(worker_program, kWorkerA, std::nullopt);
        expect(*worker_a.child, "create-session 1 multiprocess proof", "OK session=1");
        expect(*worker_a.child, "scenario-main 1 1",
               "OK main hypothesis=1 branch=1 experiment=1 attempt=1 model_call=1 tool_call=1 "
               "artifact=2 observation=1 result=1");
        expect(*worker_a.child, "scenario-failure 1 1",
               "OK failure hypothesis=2 branch=2 experiment=2 attempt=2 failure=1");
        expect(*worker_a.child, "scenario-retry 1 1",
               "OK retry branch=3 experiment=3 attempt=3 artifact=3 result=2");
        const WorkerStatus scenarios = read_status(*worker_a.child, "worker A");
        require_equal(scenarios.last, kRecordsAfterScenarios,
                      "the committed prefix after the three scenarios");
        require_equal(scenarios.epoch, first_epoch, "the epoch worker A appended under");
        std::cout << "PROOF 1: worker " << worker_a.id << " created session 1 and committed the "
                  << "main, failure and retry scenarios (" << scenarios.last << " records, epoch "
                  << scenarios.epoch << ")" << std::endl;

        // --- step 2 ---------------------------------------------------------
        WorkerHandle worker_b = start_worker(worker_program, kWorkerB, std::nullopt);
        expect(*worker_b.child, "accept 2 1", "OK accepted result=2 decision=1");
        const WorkerStatus accepted = read_status(*worker_b.child, "worker B");
        require_equal(accepted.last, kRecordsAfterAccept,
                      "the committed prefix after the acceptance batch");
        if (accepted.chain.empty()) {
            fail("worker B reported an empty chain digest after the acceptance batch");
        }
        std::cout << "PROOF 2: worker " << worker_b.id << " accepted result 2 with decision 1 in one "
                  << "atomic batch (" << accepted.last << " records, chain " << accepted.chain << ")"
                  << std::endl;

        // --- step 3 ---------------------------------------------------------
        const std::uint64_t killed_pid = worker_a.child->process_id();
        const std::uint64_t killed_boot = worker_a.boot;
        worker_a.child->kill("step 3 kills worker A with real termination");
        const WorkerStatus survived = read_status(*worker_b.child, "worker B");
        require_equal(survived.last, accepted.last,
                      "the committed prefix after worker A was killed");
        if (survived.chain != accepted.chain) {
            fail("the committed chain digest changed when worker A was killed: " + accepted.chain +
                 " became " + survived.chain);
        }
        std::cout << "PROOF 3: worker " << worker_a.id << " (process " << killed_pid
                  << ") was killed with real termination; its " << survived.last
                  << " committed records are still present (chain " << survived.chain << ")"
                  << std::endl;

        // --- step 4 ---------------------------------------------------------
        WorkerHandle worker_a2 = start_worker(worker_program, kWorkerA, std::nullopt);
        if (worker_a2.boot == killed_boot) {
            fail("the fresh incarnation of worker " + number_text(kWorkerA) +
                 " kept the killed incarnation's boot identity " + number_text(killed_boot));
        }
        require_equal(worker_a2.epoch, survived.epoch, "the epoch of the fresh incarnation");
        expect(*worker_a2.child, "create-session 2 second incarnation", "OK session=2");
        const WorkerStatus reincarnated = read_status(*worker_a2.child, "the fresh incarnation");
        require_equal(reincarnated.last, kRecordsAfterReincarnation,
                      "the committed prefix after the fresh incarnation appended");
        std::cout << "PROOF 4: a fresh incarnation of worker " << worker_a.id
                  << " was admitted with boot " << worker_a2.boot << " (the killed incarnation held "
                  << killed_boot << ") and appended record " << reincarnated.last << std::endl;

        // --- step 5, first half ---------------------------------------------
        reject(*worker_a2.child, "raw-status " + number_text(survived.epoch) + " " +
                                     number_text(killed_boot),
               "STALE_WORKER",
               "step 5 sends traffic claiming the killed incarnation's boot identity");
        std::cout << "PROOF 5: a frame claiming the killed incarnation (worker " << worker_a.id
                  << ", boot " << killed_boot << ") under the current epoch was rejected with "
                  << "STALE_WORKER" << std::endl;

        const PersistedState before_kill = read_persisted_state("before the coordinator was killed");
        require_equal(before_kill.records, kRecordsAfterReincarnation,
                      "the durable record count before the coordinator was killed");

        // --- step 6, first half: real coordinator death ----------------------
        const std::uint64_t coordinator_pid = coordinator_->process_id();
        coordinator_->kill("step 6 kills the coordinator with real termination");
        exit_cleanly(*worker_b.child, "worker B");
        exit_cleanly(*worker_a2.child, "the fresh incarnation of worker A");

        start_coordinator(coordinator_program, "the coordinator is restarted");
        if (!(epoch_ > first_epoch)) {
            fail("the restarted coordinator reports epoch " + number_text(epoch_) +
                 ", which is not higher than " + number_text(first_epoch));
        }
        const std::uint64_t second_epoch = epoch_;

        // --- step 5, second half --------------------------------------------
        WorkerHandle worker_c = start_worker(worker_program, kWorkerC, std::nullopt);
        reject(*worker_c.child,
               "raw-status " + number_text(first_epoch) + " " + number_text(worker_c.boot),
               "STALE_EPOCH",
               "step 5 sends a raw frame claiming the pre-restart coordinator epoch");
        std::cout << "PROOF 5: a raw frame claiming the pre-restart epoch " << first_epoch
                  << " was rejected with STALE_EPOCH after the coordinator was restarted under epoch "
                  << second_epoch << std::endl;
        exit_cleanly(*worker_c.child, "worker C");

        // --- step 6, second half: durable history and logical state ---------
        const PersistedState after_restart =
            read_persisted_state("after the coordinator was restarted");
        require_equal(after_restart.records, before_kill.records,
                      "the durable record count after the restart");
        if (after_restart.chain != before_kill.chain) {
            fail("the persisted chain digest changed across the restart: " +
                 rl::to_hex(before_kill.chain) + " became " + rl::to_hex(after_restart.chain));
        }
        if (after_restart.logical != before_kill.logical) {
            fail("the logical digest of the reconstructed state changed across the restart: " +
                 rl::to_hex(before_kill.logical) + " became " + rl::to_hex(after_restart.logical));
        }
        if (!(after_restart.epoch > before_kill.epoch)) {
            fail("the persisted epoch did not advance: " + number_text(before_kill.epoch) +
                 " became " + number_text(after_restart.epoch));
        }
        std::cout << "PROOF 6: the coordinator (process " << coordinator_pid
                  << ") was killed and restarted: epoch " << first_epoch << " -> " << second_epoch
                  << ", " << after_restart.records << " records intact, logical digest "
                  << rl::to_hex(after_restart.logical) << " unchanged" << std::endl;

        WorkerHandle worker_d = start_worker(worker_program, kWorkerD, std::nullopt);
        require_equal(worker_d.epoch, second_epoch, "the epoch of the worker started after the restart");
        expect(*worker_d.child, "create-session 3 after the restart", "OK session=3");
        const WorkerStatus restarted_append = read_status(*worker_d.child, "worker D");
        require_equal(restarted_append.last, kRecordsAfterRestartAppend,
                      "the committed prefix after the worker started after the restart appended");
        require_equal(restarted_append.epoch, second_epoch, "the epoch reported after the restart");
        const PersistedState after_append =
            read_persisted_state("after the worker started after the restart appended");
        if (after_append.lineage != before_kill.lineage) {
            fail("the lineage of the accepted result changed: " + rl::to_hex(before_kill.lineage) +
                 " became " + rl::to_hex(after_append.lineage));
        }
        if (!after_append.accepted || !after_append.reconstructable) {
            fail("the accepted result no longer reconstructs after the restart");
        }
        std::cout << "PROOF 6: a fresh worker appended record " << restarted_append.last
                  << " under epoch " << second_epoch << " and result 2 still reconstructs to lineage "
                  << rl::to_hex(after_append.lineage) << std::endl;

        // --- step 7 ---------------------------------------------------------
        exit_cleanly(*worker_d.child, "worker D");
        coordinator_->send("shutdown");
        const int coordinator_exit = coordinator_->wait_or_fail("step 7 shuts the coordinator down");
        if (coordinator_exit != 0) {
            fail("the coordinator exited with code " + std::to_string(coordinator_exit) +
                 " instead of a clean shutdown");
        }
        coordinator_->close();

        const PersistedState final_state = read_persisted_state("in the final verification");
        require_equal(final_state.records, kRecordsAfterRestartAppend,
                      "the final durable record count");
        if (!final_state.complete || !final_state.accepted || !final_state.reconstructable) {
            fail("the accepted result does not reconstruct from the persisted snapshot");
        }
        if (!final_state.failed_attempt_present || !final_state.failure_present) {
            fail("the failed attempt and its failure are not part of the reconstructed lineage");
        }
        if (final_state.lineage != before_kill.lineage) {
            fail("the final lineage digest differs from the digest observed before the restart");
        }
        if (final_state.decisions != 1u) {
            fail("the persisted history carries " + number_text(final_state.decisions) +
                 " decisions instead of one");
        }
        std::cout << "PROOF 7: after the graceful shutdown the persisted snapshot verifies ("
                  << final_state.records << " records), result 2 reconstructs with the failed attempt "
                  << kFailedAttempt.value() << " still in history, and " << final_state.cited_evidence
                  << " cited evidence pairs resolve to the sequences the loaded ledger reports"
                  << std::endl;

        remove_work_directory();
        std::cout << "PROOF OK" << std::endl;
    }

private:
    // --- process helpers ----------------------------------------------------

    fs::path resolve_tool(const std::string& name) const {
        const std::vector<fs::path> candidates{tools_ / (name + ".exe"), tools_ / name};
        for (const fs::path& candidate : candidates) {
            std::error_code code;
            if (fs::is_regular_file(candidate, code)) {
                return candidate;
            }
        }
        fail("cannot find " + name + " in " + tools_.string() +
             "; build it next to this program or pass --tools DIR");
    }

    Child& start_coordinator(const fs::path& program, const std::string& what) {
        const std::vector<std::string> arguments{"--port", "0", "--state", state_, "--identity",
                                                 "multiprocess-proof-coordinator"};
        std::unique_ptr<Child> child = Child::spawn(program, arguments, "coordinator");
        const std::string ready = child->receive();
        const std::vector<std::string> tokens = split_tokens(ready);
        if (tokens.size() != 4u || tokens[0] != "READY") {
            fail(what + ": the coordinator did not report readiness, it printed '" + ready + "'");
        }
        std::uint64_t port = 0;
        std::uint64_t epoch = 0;
        std::uint64_t generation = 0;
        if (!parse_unsigned(tokens[1], port) || !parse_unsigned(tokens[2], epoch) ||
            !parse_unsigned(tokens[3], generation)) {
            fail(what + ": the readiness line '" + ready + "' is not READY <port> <epoch> <generation>");
        }
        if (port == 0 || port > 65535u) {
            fail(what + ": the coordinator bound port " + number_text(port));
        }
        port_ = port;
        epoch_ = epoch;
        generation_ = generation;
        children_.push_back(std::move(child));
        coordinator_ = children_.back().get();
        return *coordinator_;
    }

    WorkerHandle start_worker(const fs::path& program, std::uint64_t worker,
                              std::optional<std::uint64_t> previous_boot) {
        std::vector<std::string> arguments{"--port", number_text(port_), "--worker",
                                           number_text(worker)};
        if (previous_boot.has_value()) {
            arguments.push_back("--previous-boot");
            arguments.push_back(number_text(*previous_boot));
        }
        std::unique_ptr<Child> child =
            Child::spawn(program, arguments, "worker " + number_text(worker));
        const std::string ready = child->receive();
        if (ready.rfind("WORKER READY ", 0) != 0) {
            fail("worker " + number_text(worker) + " did not report readiness, it printed '" + ready +
                 "'");
        }
        WorkerHandle handle;
        handle.id = require_number(ready, "worker", "the worker readiness line");
        handle.boot = require_number(ready, "boot", "the worker readiness line");
        handle.epoch = require_number(ready, "epoch", "the worker readiness line");
        handle.generation = require_number(ready, "generation", "the worker readiness line");
        if (handle.id != worker) {
            fail("worker " + number_text(worker) + " reported identity " + number_text(handle.id));
        }
        if (handle.boot == 0) {
            fail("worker " + number_text(worker) + " was admitted without a boot identity");
        }
        require_equal(handle.generation, generation_, "the ledger generation of a worker");
        children_.push_back(std::move(child));
        handle.child = children_.back().get();
        return handle;
    }

    std::string expect(Child& child, const std::string& command, const std::string& expected) {
        child.send(command);
        const std::string answer = child.receive();
        if (answer != expected) {
            fail("'" + command + "' answered '" + answer + "' instead of '" + expected + "'");
        }
        return answer;
    }

    void reject(Child& child, const std::string& command, const std::string& expected_code,
                const std::string& what) {
        child.send(command);
        const std::string answer = child.receive();
        const std::string prefix = "ERR " + expected_code + " ";
        if (answer.rfind(prefix, 0) != 0) {
            fail(what + ": '" + command + "' answered '" + answer + "' instead of " + expected_code);
        }
    }

    WorkerStatus read_status(Child& child, const std::string& what) {
        // The coordinator persists a committed batch before it sends the
        // append reply, so this request is not a durability barrier any more:
        // it is the end-to-end check of the committed prefix that the later
        // steps compare against, and it proves the connection still answers
        // before the process is killed.
        child.send("status");
        const std::string answer = child.receive();
        if (answer.rfind("OK last=", 0) != 0) {
            fail(what + ": status answered '" + answer + "'");
        }
        WorkerStatus status;
        status.last = require_number(answer, "last", what + " status");
        status.chain = require_text(answer, "chain", what + " status");
        status.epoch = require_number(answer, "epoch", what + " status");
        status.boot = require_number(answer, "boot", what + " status");
        return status;
    }

    void exit_cleanly(Child& child, const std::string& what) {
        expect(child, "exit", "OK bye");
        const int code = child.wait_or_fail(what + " exits");
        if (code != 0) {
            fail(what + " exited with code " + std::to_string(code) + " instead of 0");
        }
        child.close();
    }

    // --- persisted state ----------------------------------------------------

    PersistedState read_persisted_state(const std::string& what) {
        auto snapshot = rl::load_snapshot_file(state_, rl::Limits{});
        if (!snapshot.ok()) {
            fail(what + ": the persisted snapshot " + state_ + " cannot be read: " +
                 describe(snapshot.status()));
        }
        rl::LedgerConfig replay_config;
        replay_config.generation = snapshot.value().generation;
        auto replay = rl::replay_committed(snapshot.value().records, replay_config);
        if (!replay.ok()) {
            fail(what + ": replaying the persisted records failed: " + describe(replay.status()));
        }
        if (!replay.value().integrity.ok) {
            fail(what + ": the persisted records report integrity issues: " +
                 replay.value().integrity.to_text());
        }

        auto ledger = rl::Ledger::load(state_, rl::LedgerConfig{});
        if (!ledger.ok()) {
            fail(what + ": loading the persisted ledger failed: " + describe(ledger.status()));
        }
        const rl::LedgerSnapshot loaded = ledger.value()->snapshot();

        auto integrity = loaded.verify();
        if (!integrity.ok()) {
            fail(what + ": verification of the loaded ledger failed: " + describe(integrity.status()));
        }
        if (!integrity.value().ok || !integrity.value().issues.empty()) {
            fail(what + ": the loaded ledger reports integrity issues: " + integrity.value().to_text());
        }
        if (integrity.value().checked_records != loaded.watermark().value()) {
            fail(what + ": verification covered " +
                 number_text(integrity.value().checked_records) + " of " +
                 number_text(loaded.watermark().value()) + " committed records");
        }

        auto session = loaded.session(kSession);
        if (!session.ok()) {
            fail(what + ": research session " + number_text(kSession.value()) +
                 " is missing from the loaded ledger: " + describe(session.status()));
        }

        auto bundle = loaded.supporting_evidence(kRetryResult);
        if (!bundle.ok()) {
            fail(what + ": the accepted result does not reconstruct: " + describe(bundle.status()));
        }
        auto result = loaded.result(kRetryResult);
        if (!result.ok()) {
            fail(what + ": the accepted result is missing: " + describe(result.status()));
        }
        if (result.value().status != rl::ResultStatus::Accepted) {
            fail(what + ": result " + number_text(kRetryResult.value()) + " is " +
                 std::string(rl::result_status_name(result.value().status)) + " instead of ACCEPTED");
        }

        auto decision = loaded.decision(kAcceptance);
        if (!decision.ok()) {
            fail(what + ": the acceptance decision is missing: " + describe(decision.status()));
        }
        std::uint64_t cited = 0;
        for (const rl::EvidenceRef& evidence : decision.value().evidence) {
            auto reported = loaded.subject_sequence(evidence.subject);
            if (!reported.ok()) {
                fail(what + ": the cited evidence subject " + evidence.subject.to_string() +
                     " is not committed: " + describe(reported.status()));
            }
            if (!(reported.value() == evidence.sequence)) {
                fail(what + ": the decision cites " + evidence.subject.to_string() + " at sequence " +
                     number_text(evidence.sequence.value()) + " but the ledger reports sequence " +
                     number_text(reported.value().value()));
            }
            ++cited;
        }
        if (cited == 0) {
            fail(what + ": the acceptance decision cites no evidence");
        }

        if (!(replay.value().logical_digest == ledger.value()->logical_digest())) {
            fail(what + ": replaying the committed records yields logical digest " +
                 rl::to_hex(replay.value().logical_digest) +
                 " while the reloaded ledger reports " +
                 rl::to_hex(ledger.value()->logical_digest()));
        }
        if (!(replay.value().chain_digest == snapshot.value().chain_digest)) {
            fail(what + ": the replayed chain digest differs from the persisted chain digest");
        }

        PersistedState state;
        state.records = snapshot.value().records.size();
        state.epoch = snapshot.value().epoch.value();
        state.chain = snapshot.value().chain_digest;
        state.logical = replay.value().logical_digest;
        state.lineage = bundle.value().lineage_digest;
        state.accepted = bundle.value().accepted();
        state.reconstructable = bundle.value().reconstructable;
        state.complete = bundle.value().complete;
        state.cited_evidence = cited;
        for (const rl::AttemptView& attempt : bundle.value().attempts) {
            if (attempt.attempt == kFailedAttempt && attempt.state == rl::AttemptState::Failed) {
                state.failed_attempt_present = true;
            }
        }
        for (const rl::FailureView& failure : bundle.value().failures) {
            if (failure.failure == kFailure &&
                failure.category == rl::FailureCategory::Execution &&
                failure.scope == rl::SubjectId::of(kFailedAttempt)) {
                state.failure_present = true;
            }
        }
        auto stats = loaded.stats();
        if (!stats.ok()) {
            fail(what + ": the ledger statistics cannot be read: " + describe(stats.status()));
        }
        state.decisions = stats.value().decisions;
        return state;
    }

    static void require_equal(std::uint64_t actual, std::uint64_t expected, const char* what) {
        if (actual != expected) {
            fail(std::string(what) + " is " + number_text(actual) + " instead of " +
                 number_text(expected));
        }
    }

    void remove_work_directory() noexcept {
        if (work_.empty() || !work_.has_filename()) {
            return;
        }
        std::error_code code;
        fs::remove_all(work_, code);
    }

    fs::path tools_{};
    fs::path work_{};
    std::string state_{};
    std::vector<std::unique_ptr<Child>> children_{};
    Child* coordinator_ = nullptr;
    std::uint64_t port_ = 0;
    std::uint64_t epoch_ = 0;
    std::uint64_t generation_ = 0;
};

// --- options ----------------------------------------------------------------

struct Options {
    fs::path tools{};
    fs::path work{};
};

void print_usage() {
    std::cout << "usage: research-ledger-multiprocess-proof [--tools DIR] [--work DIR]" << std::endl;
}

fs::path executable_directory() {
    auto path = rl::current_executable_path();
    if (!path.ok()) {
        fail("cannot determine the path of the running executable: " + describe(path.status()));
    }
    return fs::path(path.value()).parent_path();
}

fs::path default_work_directory() {
    std::error_code code;
    const fs::path base = fs::temp_directory_path(code);
    if (code) {
        fail("cannot determine the system temporary directory: " + code.message());
    }
    return base / ("research-ledger-proof-" + number_text(rl::current_process_id()));
}

bool parse_options(int argc, char** argv, Options& options) {
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument(argv[index]);
        if (index + 1 >= argc) {
            return false;
        }
        const std::string value(argv[index + 1]);
        if (argument == "--tools") {
            options.tools = fs::path(value);
        } else if (argument == "--work") {
            options.work = fs::path(value);
        } else {
            return false;
        }
        ++index;
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    Options options;
    if (!parse_options(argc, argv, options)) {
        print_usage();
        return 2;
    }
    if (options.tools.empty()) {
        options.tools = executable_directory();
    }
    if (options.work.empty()) {
        options.work = default_work_directory();
    }

    try {
        std::error_code code;
        fs::remove_all(options.work, code);
        code.clear();
        static_cast<void>(fs::create_directories(options.work, code));
        if (code) {
            fail("cannot create the work directory " + options.work.string() + ": " + code.message());
        }
        if (!fs::is_directory(options.work)) {
            fail("the work directory " + options.work.string() + " is not a directory");
        }
        const std::string state = (options.work / "ledger-state.rls").string();
        Proof proof(options.tools, options.work, state);
        proof.run();
        return 0;
    } catch (const Failure& failure) {
        std::cout << "PROOF FAILED: " << failure.detail << std::endl;
        return 1;
    } catch (const std::exception& error) {
        std::cout << "PROOF FAILED: unexpected exception: " << error.what() << std::endl;
        return 1;
    }
}
