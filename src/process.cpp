#include "research_ledger/process.hpp"

#include <array>
#include <span>
#include <string>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <csignal>
#include <cerrno>
#include <spawn.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace research_ledger {
namespace {

#ifdef _WIN32
std::string last_error_text(const char* operation) {
    return std::string(operation) + " failed with error " +
           std::to_string(static_cast<std::uint32_t>(GetLastError()));
}

// Every handle the child inherits is named explicitly in the process attribute
// list, so no unrelated inheritable handle of this process can leak into a
// child. Children are created suspended and assigned to a job object before
// they run, which is what makes terminate_tree() able to end a process that
// spawned processes of its own.
class AttributeList {
public:
    AttributeList() = default;
    ~AttributeList() {
        if (list_ != nullptr) {
            HeapFree(GetProcessHeap(), 0, list_);
        }
    }
    AttributeList(const AttributeList&) = delete;
    AttributeList& operator=(const AttributeList&) = delete;

    bool valid() const noexcept { return list_ != nullptr; }

    Status build(std::span<HANDLE> handles) {
        SIZE_T size = 0;
        InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
        if (size == 0) {
            return Status(ErrorCode::IoFailure, "InitializeProcThreadAttributeList sizing failed");
        }
        list_ = static_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(HeapAlloc(GetProcessHeap(), 0, size));
        if (list_ == nullptr) {
            return Status(ErrorCode::IoFailure, "HeapAlloc failed for the process attribute list");
        }
        if (!InitializeProcThreadAttributeList(list_, 1, 0, &size)) {
            return Status(ErrorCode::IoFailure, "InitializeProcThreadAttributeList failed");
        }
        if (!UpdateProcThreadAttribute(list_, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, handles.data(),
                                       handles.size() * sizeof(HANDLE), nullptr, nullptr)) {
            return Status(ErrorCode::IoFailure, "UpdateProcThreadAttribute failed");
        }
        return Status{};
    }

    LPPROC_THREAD_ATTRIBUTE_LIST get() const noexcept { return list_; }

private:
    LPPROC_THREAD_ATTRIBUTE_LIST list_ = nullptr;
};

Status write_native(HANDLE handle, const char* data, std::size_t size) {
    std::size_t offset = 0;
    while (offset < size) {
        const std::size_t remaining = size - offset;
        const DWORD chunk =
            static_cast<DWORD>(remaining > (1u << 20) ? (1u << 20) : remaining);
        DWORD written = 0;
        if (!WriteFile(handle, data + offset, chunk, &written, nullptr) || written == 0) {
            return Status(ErrorCode::IoFailure, "WriteFile on the child input pipe failed");
        }
        offset += static_cast<std::size_t>(written);
    }
    return Status{};
}
#else
std::string last_error_text(const char* operation) {
    return std::string(operation) + " failed with error " + std::to_string(errno);
}

Status write_native(int fd, const char* data, std::size_t size) {
    std::size_t offset = 0;
    while (offset < size) {
        const ssize_t written = ::write(fd, data + offset, size - offset);
        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            return Status(ErrorCode::IoFailure, "write on the child input pipe failed");
        }
        if (written == 0) {
            return Status(ErrorCode::IoFailure, "write on the child input pipe made no progress");
        }
        offset += static_cast<std::size_t>(written);
    }
    return Status{};
}
#endif

}  // namespace

struct ChildProcess::Impl {
#ifdef _WIN32
    HANDLE process = nullptr;
    HANDLE job = nullptr;
    HANDLE read_pipe = nullptr;
    HANDLE write_pipe = nullptr;
    std::string buffer{};
#else
    pid_t pid = -1;
    int read_fd = -1;
    int write_fd = -1;
    std::string buffer{};
#endif
};

ChildProcess::ChildProcess() noexcept = default;

ChildProcess::~ChildProcess() { close(); }

ChildProcess::ChildProcess(ChildProcess&& other) noexcept
    : impl_(std::move(other.impl_)), process_id_(other.process_id_) {
    other.process_id_ = 0;
}

ChildProcess& ChildProcess::operator=(ChildProcess&& other) noexcept {
    if (this != &other) {
        close();
        impl_ = std::move(other.impl_);
        process_id_ = other.process_id_;
        other.process_id_ = 0;
    }
    return *this;
}

std::uint64_t current_process_id() noexcept {
#ifdef _WIN32
    return static_cast<std::uint64_t>(GetCurrentProcessId());
#else
    return static_cast<std::uint64_t>(getpid());
#endif
}

Result<ChildProcess> ChildProcess::spawn(const std::string& program,
                                         const std::vector<std::string>& arguments) {
#ifdef _WIN32
    SECURITY_ATTRIBUTES attributes{};
    attributes.nLength = sizeof(attributes);
    attributes.bInheritHandle = TRUE;

    // stdout and stderr share one pipe, so an interleaved child cannot fill a
    // pipe nobody drains. CreatePipe returns the READ end first and the WRITE
    // end second: the child writes to the write end and the parent reads the
    // read end, and the parent's end is the one that must not be inherited.
    HANDLE parent_read = nullptr;
    HANDLE child_write = nullptr;
    if (!CreatePipe(&parent_read, &child_write, &attributes, 0)) {
        return Status(ErrorCode::IoFailure, last_error_text("CreatePipe"));
    }
    if (!SetHandleInformation(parent_read, HANDLE_FLAG_INHERIT, 0)) {
        CloseHandle(parent_read);
        CloseHandle(child_write);
        return Status(ErrorCode::IoFailure, last_error_text("SetHandleInformation"));
    }
    HANDLE child_read = nullptr;
    HANDLE parent_write = nullptr;
    if (!CreatePipe(&child_read, &parent_write, &attributes, 0)) {
        CloseHandle(parent_read);
        CloseHandle(child_write);
        return Status(ErrorCode::IoFailure, last_error_text("CreatePipe"));
    }
    if (!SetHandleInformation(parent_write, HANDLE_FLAG_INHERIT, 0)) {
        CloseHandle(parent_read);
        CloseHandle(child_write);
        CloseHandle(child_read);
        CloseHandle(parent_write);
        return Status(ErrorCode::IoFailure, last_error_text("SetHandleInformation"));
    }

    std::array<HANDLE, 3> inherited{child_read, child_write, child_write};
    AttributeList attribute_list;
    const Status built = attribute_list.build(std::span<HANDLE>(inherited.data(), inherited.size()));
    if (!built.ok()) {
        CloseHandle(parent_read);
        CloseHandle(child_write);
        CloseHandle(child_read);
        CloseHandle(parent_write);
        return built;
    }

    std::string command_line = "\"" + program + "\"";
    for (const std::string& argument : arguments) {
        command_line += " \"";
        command_line += argument;
        command_line += "\"";
    }
    std::vector<char> mutable_line(command_line.begin(), command_line.end());
    mutable_line.push_back('\0');

    STARTUPINFOEXA startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = child_read;
    startup.StartupInfo.hStdOutput = child_write;
    startup.StartupInfo.hStdError = child_write;
    startup.lpAttributeList = attribute_list.get();
    PROCESS_INFORMATION information{};
    // CREATE_NO_WINDOW keeps every child console-free; CREATE_SUSPENDED lets the
    // job object be attached before the child executes a single instruction.
    const DWORD flags = CREATE_NO_WINDOW | CREATE_SUSPENDED;
    if (!CreateProcessA(nullptr, mutable_line.data(), nullptr, nullptr, TRUE, flags, nullptr, nullptr,
                        &startup.StartupInfo, &information)) {
        const std::string detail = last_error_text("CreateProcess");
        CloseHandle(parent_read);
        CloseHandle(child_write);
        CloseHandle(child_read);
        CloseHandle(parent_write);
        return Status(ErrorCode::IoFailure, detail + " for " + program);
    }
    // Only the parent's ends stay open here; the child's ends are inherited by
    // the child and are closed in this process immediately after the spawn.
    CloseHandle(child_read);
    CloseHandle(child_write);

    // A job object is best effort: a process that already belongs to a
    // non-breakaway job cannot hand its children to another one, and the
    // runtime then keeps real per-process termination instead of failing the
    // spawn.
    HANDLE job = CreateJobObjectA(nullptr, nullptr);
    if (job != nullptr) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION extended{};
        extended.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, &extended,
                                     static_cast<DWORD>(sizeof(extended))) ||
            !AssignProcessToJobObject(job, information.hProcess)) {
            CloseHandle(job);
            job = nullptr;
        }
    }
    static_cast<void>(ResumeThread(information.hThread));
    CloseHandle(information.hThread);

    ChildProcess child;
    child.impl_ = std::make_unique<Impl>();
    child.impl_->process = information.hProcess;
    child.impl_->job = job;
    child.impl_->read_pipe = parent_read;
    child.impl_->write_pipe = parent_write;
    child.process_id_ = static_cast<std::uint64_t>(information.dwProcessId);
    return child;
#else
    int out_pipe[2] = {-1, -1};
    if (pipe(out_pipe) != 0) {
        return Status(ErrorCode::IoFailure, last_error_text("pipe"));
    }
    int in_pipe[2] = {-1, -1};
    if (pipe(in_pipe) != 0) {
        ::close(out_pipe[0]);
        ::close(out_pipe[1]);
        return Status(ErrorCode::IoFailure, last_error_text("pipe"));
    }

    std::vector<std::string> storage;
    storage.push_back(program);
    for (const std::string& argument : arguments) {
        storage.push_back(argument);
    }
    std::vector<char*> argv;
    argv.reserve(storage.size() + 1);
    for (std::string& entry : storage) {
        argv.push_back(entry.data());
    }
    argv.push_back(nullptr);

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, in_pipe[0], STDIN_FILENO);
    posix_spawn_file_actions_adddup2(&actions, out_pipe[1], STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&actions, out_pipe[1], STDERR_FILENO);
    posix_spawn_file_actions_addclose(&actions, in_pipe[0]);
    posix_spawn_file_actions_addclose(&actions, in_pipe[1]);
    posix_spawn_file_actions_addclose(&actions, out_pipe[0]);
    posix_spawn_file_actions_addclose(&actions, out_pipe[1]);
    // The child becomes the leader of its own process group so the whole tree
    // it may create can be terminated as one unit.
    posix_spawnattr_t spawn_attributes;
    posix_spawnattr_init(&spawn_attributes);
    posix_spawnattr_setflags(&spawn_attributes, static_cast<short>(POSIX_SPAWN_SETPGROUP));
    posix_spawnattr_setpgroup(&spawn_attributes, 0);
    pid_t pid = -1;
    const int rc = posix_spawn(&pid, program.c_str(), &actions, &spawn_attributes, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    posix_spawnattr_destroy(&spawn_attributes);
    ::close(in_pipe[0]);
    ::close(out_pipe[1]);
    if (rc != 0) {
        ::close(in_pipe[1]);
        ::close(out_pipe[0]);
        return Status(ErrorCode::IoFailure,
                      "posix_spawn failed with error " + std::to_string(rc) + " for " + program);
    }
    ChildProcess child;
    child.impl_ = std::make_unique<Impl>();
    child.impl_->pid = pid;
    child.impl_->read_fd = out_pipe[0];
    child.impl_->write_fd = in_pipe[1];
    child.process_id_ = static_cast<std::uint64_t>(pid);
    return child;
#endif
}

Result<std::string> ChildProcess::read_line() {
    if (impl_ == nullptr) {
        return Status(ErrorCode::IoFailure, "child process is not open");
    }
    for (;;) {
        const std::size_t newline = impl_->buffer.find('\n');
        if (newline != std::string::npos) {
            std::string line = impl_->buffer.substr(0, newline);
            impl_->buffer.erase(0, newline + 1);
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            return line;
        }
        std::array<char, 512> chunk{};
#ifdef _WIN32
        DWORD read = 0;
        const BOOL ok =
            ReadFile(impl_->read_pipe, chunk.data(), static_cast<DWORD>(chunk.size()), &read, nullptr);
        if (!ok || read == 0) {
            return Status(ErrorCode::IoFailure, "child output stream ended");
        }
#else
        const ssize_t read = ::read(impl_->read_fd, chunk.data(), chunk.size());
        if (read < 0 && errno == EINTR) {
            continue;
        }
        if (read <= 0) {
            return Status(ErrorCode::IoFailure, "child output stream ended");
        }
#endif
        // read_line() blocks until a line arrives or the stream ends. There is
        // no timeout anywhere in the runtime's process handling.
        impl_->buffer.append(chunk.data(), static_cast<std::size_t>(read));
    }
}

Status ChildProcess::write_line(const std::string& line) {
    if (impl_ == nullptr) {
        return Status(ErrorCode::IoFailure, "child process is not open");
    }
    std::string terminated = line;
    if (terminated.empty() || terminated.back() != '\n') {
        terminated.push_back('\n');
    }
#ifdef _WIN32
    return write_native(impl_->write_pipe, terminated.data(), terminated.size());
#else
    return write_native(impl_->write_fd, terminated.data(), terminated.size());
#endif
}

Status ChildProcess::terminate() {
    if (impl_ == nullptr) {
        return Status(ErrorCode::IoFailure, "child process is not open");
    }
#ifdef _WIN32
    if (!TerminateProcess(impl_->process, 1)) {
        return Status(ErrorCode::IoFailure, last_error_text("TerminateProcess"));
    }
    return Status{};
#else
    if (kill(impl_->pid, SIGKILL) != 0) {
        return Status(ErrorCode::IoFailure, last_error_text("kill"));
    }
    return Status{};
#endif
}

Status ChildProcess::terminate_tree() {
    if (impl_ == nullptr) {
        return Status(ErrorCode::IoFailure, "child process is not open");
    }
#ifdef _WIN32
    // Terminating the job ends the child and every process the child created,
    // so a killed worker cannot leave a descendant holding the coordinator's
    // connection open.
    if (impl_->job != nullptr) {
        if (!TerminateJobObject(impl_->job, 1)) {
            return Status(ErrorCode::IoFailure, last_error_text("TerminateJobObject"));
        }
        return Status{};
    }
    return terminate();
#else
    // The child leads its own process group, so the negated pid addresses the
    // whole group.
    if (kill(-impl_->pid, SIGKILL) != 0) {
        if (errno == ESRCH) {
            return Status{};
        }
        return Status(ErrorCode::IoFailure, last_error_text("kill(process group)"));
    }
    return Status{};
#endif
}

bool ChildProcess::running() {
    if (impl_ == nullptr) {
        return false;
    }
#ifdef _WIN32
    return WaitForSingleObject(impl_->process, 0) == WAIT_TIMEOUT;
#else
    int status = 0;
    const pid_t result = waitpid(impl_->pid, &status, WNOHANG);
    return result == 0;
#endif
}

Result<int> ChildProcess::wait() {
    if (impl_ == nullptr) {
        return Status(ErrorCode::IoFailure, "child process is not open");
    }
#ifdef _WIN32
    const DWORD result = WaitForSingleObject(impl_->process, INFINITE);
    if (result != WAIT_OBJECT_0) {
        return Status(ErrorCode::IoFailure, last_error_text("WaitForSingleObject"));
    }
    DWORD code = 0;
    if (!GetExitCodeProcess(impl_->process, &code)) {
        return Status(ErrorCode::IoFailure, last_error_text("GetExitCodeProcess"));
    }
    return static_cast<int>(code);
#else
    int status = 0;
    if (waitpid(impl_->pid, &status, 0) < 0) {
        return Status(ErrorCode::IoFailure, last_error_text("waitpid"));
    }
    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }
    if (WIFSIGNALED(status)) {
        return 128 + WTERMSIG(status);
    }
    return 1;
#endif
}

void ChildProcess::close() noexcept {
    if (impl_ == nullptr) {
        return;
    }
#ifdef _WIN32
    if (impl_->write_pipe != nullptr) {
        CloseHandle(impl_->write_pipe);
        impl_->write_pipe = nullptr;
    }
    if (impl_->read_pipe != nullptr) {
        CloseHandle(impl_->read_pipe);
        impl_->read_pipe = nullptr;
    }
    if (impl_->job != nullptr) {
        // KILL_ON_JOB_CLOSE means a process that outlived its handle cannot
        // outlive the runtime's ownership of it.
        CloseHandle(impl_->job);
        impl_->job = nullptr;
    }
    if (impl_->process != nullptr) {
        CloseHandle(impl_->process);
        impl_->process = nullptr;
    }
#else
    if (impl_->write_fd >= 0) {
        ::close(impl_->write_fd);
        impl_->write_fd = -1;
    }
    if (impl_->read_fd >= 0) {
        ::close(impl_->read_fd);
        impl_->read_fd = -1;
    }
#endif
    impl_.reset();
    process_id_ = 0;
}

Result<std::string> current_executable_path() {
#ifdef _WIN32
    // GetModuleFileNameA reports truncation only through its return value, so
    // the path is captured with an exactly sized buffer instead of a guess.
    DWORD capacity = 1024;
    for (;;) {
        std::vector<char> buffer(static_cast<std::size_t>(capacity));
        const DWORD written = GetModuleFileNameA(nullptr, buffer.data(), capacity);
        if (written == 0) {
            return Status(ErrorCode::IoFailure, last_error_text("GetModuleFileNameA"));
        }
        if (written < capacity) {
            return std::string(buffer.data(), static_cast<std::size_t>(written));
        }
        if (capacity >= 32768) {
            return Status(ErrorCode::IoFailure, "executable path exceeds the supported length");
        }
        capacity *= 2;
    }
#else
    std::array<char, 1024> buffer{};
    const ssize_t written = ::readlink("/proc/self/exe", buffer.data(), buffer.size() - 1);
    if (written <= 0) {
        return Status(ErrorCode::IoFailure, last_error_text("readlink(/proc/self/exe)"));
    }
    return std::string(buffer.data(), static_cast<std::size_t>(written));
#endif
}

}  // namespace research_ledger
