#include "backends/ptrace_backend.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <limits>
#include <memory>
#include <thread>
#include <utility>
#include <vector>

#ifdef AGENTVFS_PTRACE
#include <csignal>
#include <fcntl.h>
#include <sys/ptrace.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <sys/user.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace cas {

struct PtraceBackend::TraceState {
    explicit TraceState(pid_t trace_pid) : pid(trace_pid) {}

    pid_t pid = -1;
    std::thread thread;
    std::mutex start_mutex;
    std::condition_variable start_cv;
    bool start_complete = false;
    bool start_ok = false;
    int start_errno = 0;
    bool detach_requested = false;
    bool completed = false;
    bool attached = true;
    bool exited = false;
    int error = 0;
};

namespace {

OpMask op_bit(OpType op) {
    return 1u << static_cast<unsigned>(op);
}

uint64_t now_ns() {
    struct timespec ts {};
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }
    return static_cast<uint64_t>(ts.tv_sec) * 1000000000ull +
           static_cast<uint64_t>(ts.tv_nsec);
}

std::string to_string_u64(uint64_t value) {
    return std::to_string(value);
}

std::string to_string_i64(int64_t value) {
    return std::to_string(value);
}

bool parse_pid_token(const std::string& token, pid_t& pid) {
    if (token.empty()) {
        return false;
    }

    char* end = nullptr;
    errno = 0;
    long parsed = std::strtol(token.c_str(), &end, 10);
    if (errno != 0 || end == token.c_str() || *end != '\0') {
        return false;
    }
    if (parsed <= 0 ||
        parsed > static_cast<long>(std::numeric_limits<pid_t>::max())) {
        return false;
    }

    pid = static_cast<pid_t>(parsed);
    return true;
}

std::string trim_copy(const std::string& value) {
    const char* ws = " \t\r\n";
    size_t first = value.find_first_not_of(ws);
    if (first == std::string::npos) {
        return {};
    }
    size_t last = value.find_last_not_of(ws);
    return value.substr(first, last - first + 1);
}

bool parse_pid_list(const std::string& value, std::vector<pid_t>& pids) {
    size_t start = 0;
    while (start <= value.size()) {
        size_t comma = value.find(',', start);
        std::string token = trim_copy(
            value.substr(start, comma == std::string::npos
                                    ? std::string::npos
                                    : comma - start));
        if (!token.empty()) {
            pid_t pid = 0;
            if (!parse_pid_token(token, pid)) {
                return false;
            }
            pids.push_back(pid);
        }
        if (comma == std::string::npos) {
            break;
        }
        start = comma + 1;
    }
    return true;
}

#ifdef AGENTVFS_PTRACE
struct SyscallSnapshot {
    long nr = -1;
    uint64_t args[6] {};
    OpType op = OpType::Read;
    bool tracked = false;
};

bool syscall_is(long nr, long expected) {
    return nr == expected;
}

struct OpenHowCompat {
    uint64_t flags = 0;
    uint64_t mode = 0;
    uint64_t resolve = 0;
};

bool read_tracee_memory(pid_t pid, uint64_t remote_addr, void* dst,
                        size_t len) {
    if (remote_addr == 0 || dst == nullptr || len == 0) {
        return false;
    }

    struct iovec local {};
    local.iov_base = dst;
    local.iov_len = len;
    struct iovec remote {};
    remote.iov_base = reinterpret_cast<void*>(remote_addr);
    remote.iov_len = len;
    ssize_t copied = process_vm_readv(pid, &local, 1, &remote, 1, 0);
    if (copied == static_cast<ssize_t>(len)) {
        return true;
    }

    auto* out = static_cast<unsigned char*>(dst);
    size_t offset = 0;
    while (offset < len) {
        errno = 0;
        long word = ptrace(PTRACE_PEEKDATA, pid,
                           reinterpret_cast<void*>(remote_addr + offset),
                           nullptr);
        if (word == -1 && errno != 0) {
            return false;
        }
        size_t chunk = std::min(sizeof(word), len - offset);
        std::memcpy(out + offset, &word, chunk);
        offset += chunk;
    }
    return true;
}

bool read_openat2_flags(pid_t pid, const uint64_t args[6], uint64_t& flags) {
    if (args[2] == 0 || args[3] < sizeof(uint64_t)) {
        return false;
    }

    OpenHowCompat how {};
    size_t read_len = std::min(static_cast<uint64_t>(sizeof(how)), args[3]);
    if (!read_tracee_memory(pid, args[2], &how, read_len)) {
        return false;
    }
    flags = how.flags;
    return true;
}

bool map_syscall(pid_t pid, long nr, const uint64_t args[6], OpType& op) {
#ifdef SYS_read
    if (syscall_is(nr, SYS_read)) {
        op = OpType::Read;
        return true;
    }
#endif
#ifdef SYS_pread64
    if (syscall_is(nr, SYS_pread64)) {
        op = OpType::Read;
        return true;
    }
#endif
#ifdef SYS_readv
    if (syscall_is(nr, SYS_readv)) {
        op = OpType::Read;
        return true;
    }
#endif
#ifdef SYS_preadv
    if (syscall_is(nr, SYS_preadv)) {
        op = OpType::Read;
        return true;
    }
#endif
#ifdef SYS_preadv2
    if (syscall_is(nr, SYS_preadv2)) {
        op = OpType::Read;
        return true;
    }
#endif

#ifdef SYS_write
    if (syscall_is(nr, SYS_write)) {
        op = OpType::Write;
        return true;
    }
#endif
#ifdef SYS_pwrite64
    if (syscall_is(nr, SYS_pwrite64)) {
        op = OpType::Write;
        return true;
    }
#endif
#ifdef SYS_writev
    if (syscall_is(nr, SYS_writev)) {
        op = OpType::Write;
        return true;
    }
#endif
#ifdef SYS_pwritev
    if (syscall_is(nr, SYS_pwritev)) {
        op = OpType::Write;
        return true;
    }
#endif
#ifdef SYS_pwritev2
    if (syscall_is(nr, SYS_pwritev2)) {
        op = OpType::Write;
        return true;
    }
#endif

#ifdef SYS_open
    if (syscall_is(nr, SYS_open)) {
        op = (args[1] & O_CREAT) ? OpType::Create : OpType::Open;
        return true;
    }
#endif
#ifdef SYS_openat
    if (syscall_is(nr, SYS_openat)) {
        op = (args[2] & O_CREAT) ? OpType::Create : OpType::Open;
        return true;
    }
#endif
#ifdef SYS_openat2
    if (syscall_is(nr, SYS_openat2)) {
        uint64_t flags = 0;
        op = read_openat2_flags(pid, args, flags) && (flags & O_CREAT)
                 ? OpType::Create
                 : OpType::Open;
        return true;
    }
#endif
#ifdef SYS_creat
    if (syscall_is(nr, SYS_creat)) {
        op = OpType::Create;
        return true;
    }
#endif
#ifdef SYS_mkdir
    if (syscall_is(nr, SYS_mkdir)) {
        op = OpType::Create;
        return true;
    }
#endif
#ifdef SYS_mkdirat
    if (syscall_is(nr, SYS_mkdirat)) {
        op = OpType::Create;
        return true;
    }
#endif
#ifdef SYS_mknod
    if (syscall_is(nr, SYS_mknod)) {
        op = OpType::Create;
        return true;
    }
#endif
#ifdef SYS_mknodat
    if (syscall_is(nr, SYS_mknodat)) {
        op = OpType::Create;
        return true;
    }
#endif
#ifdef SYS_symlink
    if (syscall_is(nr, SYS_symlink)) {
        op = OpType::Create;
        return true;
    }
#endif
#ifdef SYS_symlinkat
    if (syscall_is(nr, SYS_symlinkat)) {
        op = OpType::Create;
        return true;
    }
#endif
#ifdef SYS_link
    if (syscall_is(nr, SYS_link)) {
        op = OpType::Create;
        return true;
    }
#endif
#ifdef SYS_linkat
    if (syscall_is(nr, SYS_linkat)) {
        op = OpType::Create;
        return true;
    }
#endif

#ifdef SYS_close
    if (syscall_is(nr, SYS_close)) {
        op = OpType::Close;
        return true;
    }
#endif
#ifdef SYS_unlink
    if (syscall_is(nr, SYS_unlink)) {
        op = OpType::Unlink;
        return true;
    }
#endif
#ifdef SYS_unlinkat
    if (syscall_is(nr, SYS_unlinkat)) {
        op = OpType::Unlink;
        return true;
    }
#endif
#ifdef SYS_rename
    if (syscall_is(nr, SYS_rename)) {
        op = OpType::Rename;
        return true;
    }
#endif
#ifdef SYS_renameat
    if (syscall_is(nr, SYS_renameat)) {
        op = OpType::Rename;
        return true;
    }
#endif
#ifdef SYS_renameat2
    if (syscall_is(nr, SYS_renameat2)) {
        op = OpType::Rename;
        return true;
    }
#endif
#ifdef SYS_truncate
    if (syscall_is(nr, SYS_truncate)) {
        op = OpType::Truncate;
        return true;
    }
#endif
#ifdef SYS_ftruncate
    if (syscall_is(nr, SYS_ftruncate)) {
        op = OpType::Truncate;
        return true;
    }
#endif
#ifdef SYS_stat
    if (syscall_is(nr, SYS_stat)) {
        op = OpType::Stat;
        return true;
    }
#endif
#ifdef SYS_lstat
    if (syscall_is(nr, SYS_lstat)) {
        op = OpType::Stat;
        return true;
    }
#endif
#ifdef SYS_fstat
    if (syscall_is(nr, SYS_fstat)) {
        op = OpType::Stat;
        return true;
    }
#endif
#ifdef SYS_newfstatat
    if (syscall_is(nr, SYS_newfstatat)) {
        op = OpType::Stat;
        return true;
    }
#endif
#ifdef SYS_statx
    if (syscall_is(nr, SYS_statx)) {
        op = OpType::Stat;
        return true;
    }
#endif
#ifdef SYS_execve
    if (syscall_is(nr, SYS_execve)) {
        op = OpType::Exec;
        return true;
    }
#endif
#ifdef SYS_execveat
    if (syscall_is(nr, SYS_execveat)) {
        op = OpType::Exec;
        return true;
    }
#endif

    return false;
}

bool get_syscall_state(pid_t pid, SyscallSnapshot& snapshot, int64_t& ret,
                       bool& is_entry) {
#if defined(__x86_64__)
    struct user_regs_struct regs {};
    if (ptrace(PTRACE_GETREGS, pid, nullptr, &regs) != 0) {
        return false;
    }

    snapshot.nr = static_cast<long>(regs.orig_rax);
    snapshot.args[0] = static_cast<uint64_t>(regs.rdi);
    snapshot.args[1] = static_cast<uint64_t>(regs.rsi);
    snapshot.args[2] = static_cast<uint64_t>(regs.rdx);
    snapshot.args[3] = static_cast<uint64_t>(regs.r10);
    snapshot.args[4] = static_cast<uint64_t>(regs.r8);
    snapshot.args[5] = static_cast<uint64_t>(regs.r9);
    snapshot.tracked = map_syscall(pid, snapshot.nr, snapshot.args,
                                   snapshot.op);
    ret = static_cast<int64_t>(regs.rax);
    is_entry = ret == -ENOSYS;
    return true;
#else
    (void)pid;
    snapshot = SyscallSnapshot{};
    ret = 0;
    is_entry = false;
    return false;
#endif
}

TelemetryEvent make_syscall_event(pid_t pid, const SyscallSnapshot& snapshot,
                                  int64_t ret) {
    TelemetryEvent ev{};
    ev.timestamp_ns = now_ns();
    ev.op = snapshot.op;
    ev.verdict = Verdict::Allow;
    ev.pid = static_cast<uint32_t>(pid);
    ev.backend = "ptrace";
    ev.extra.emplace_back("syscall_nr", to_string_i64(snapshot.nr));
    for (size_t i = 0; i < 6; ++i) {
        ev.extra.emplace_back("arg" + std::to_string(i),
                              to_string_u64(snapshot.args[i]));
    }
    ev.extra.emplace_back("ret", to_string_i64(ret));
    if (ret > 0 && (ev.op == OpType::Read || ev.op == OpType::Write)) {
        ev.bytes = static_cast<uint64_t>(ret);
    }
    return ev;
}

void interrupt_tracee(pid_t pid) {
    if (pid > 0) {
        ptrace(PTRACE_INTERRUPT, pid, nullptr, nullptr);
    }
}

bool resume_syscall(pid_t pid, int signal_to_deliver) {
    return ptrace(PTRACE_SYSCALL, pid, nullptr,
                  reinterpret_cast<void*>(
                      static_cast<intptr_t>(signal_to_deliver))) == 0;
}
#endif

} // namespace

PtraceBackend::~PtraceBackend() {
    stop();
}

std::string PtraceBackend::name() const {
    return "ptrace";
}

bool PtraceBackend::start(const BackendConfig& cfg, EventCallback cb) {
    stop();

    std::vector<pid_t> requested_pids;
    auto it = cfg.params.find("pids");
    if (it != cfg.params.end() && !parse_pid_list(it->second, requested_pids)) {
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        callback_ = std::move(cb);
        stop_.store(false);
    }

    for (pid_t pid : requested_pids) {
        if (!attach_pid(pid)) {
            stop();
            return false;
        }
    }

    return true;
}

void PtraceBackend::stop() {
    stop_.store(true);

    std::vector<std::shared_ptr<TraceState>> states;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        callback_ = nullptr;
        for (auto& entry : traces_) {
            entry.second->detach_requested = true;
            states.push_back(entry.second);
        }
    }

    for (const auto& state : states) {
        if (state->thread.joinable()) {
            state->thread.join();
        }
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& state : states) {
            if (state->completed && !state->attached) {
                traces_.erase(state->pid);
            }
        }
    }
}

bool PtraceBackend::register_session(const SessionInfo& info) {
    (void)info;
    return true;
}

bool PtraceBackend::unregister_session(const std::string& cgroup_path) {
    (void)cgroup_path;
    return true;
}

bool PtraceBackend::install_policy(const PolicyRules& rules) {
    (void)rules;
    return true;
}

Capabilities PtraceBackend::capabilities() const {
    // The ptrace backend can only extract syscall args/return values via
    // PTRACE_GETREGS on x86_64 (see get_syscall_state). On any other
    // architecture attach_pid() returns false and no events flow, so we
    // must not advertise syscall coverage we cannot deliver.
    //
    // Single-PID limitation: the current implementation traces only the
    // explicitly attached pid. It does NOT follow forks, clones, or
    // vforks (PTRACE_O_TRACEFORK / TRACECLONE / TRACEVFORK are not set).
    // Multi-process / threaded targets must register every interesting
    // pid manually. TODO: add fork/clone/vfork following as a follow-up.
    Capabilities caps{};
#if defined(__x86_64__)
    caps.supported_ops = op_bit(OpType::Read) |
                         op_bit(OpType::Write) |
                         op_bit(OpType::Open) |
                         op_bit(OpType::Close) |
                         op_bit(OpType::Unlink) |
                         op_bit(OpType::Rename) |
                         op_bit(OpType::Truncate) |
                         op_bit(OpType::Stat) |
                         op_bit(OpType::Exec) |
                         op_bit(OpType::Create);
#else
    caps.supported_ops = 0;
#endif
    caps.pre_op_verdicts = false;
    caps.requires_cgroup = false;
    caps.requires_root = false;
    return caps;
}

bool PtraceBackend::attach_pid(pid_t pid) {
    if (pid <= 0) {
        return false;
    }

    reap_completed();

#ifdef AGENTVFS_PTRACE
#if !defined(__x86_64__)
    (void)pid;
    return false;
#else
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (traces_.find(pid) != traces_.end()) {
            return true;
        }
    }

    auto state = std::make_shared<TraceState>(pid);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stop_.load() || traces_.find(pid) != traces_.end()) {
            return false;
        }
        traces_.emplace(pid, state);
    }

    try {
        state->thread = std::thread([this, state] { trace_loop(state); });
    } catch (...) {
        std::lock_guard<std::mutex> lock(mutex_);
        traces_.erase(pid);
        return false;
    }

    {
        std::unique_lock<std::mutex> lock(state->start_mutex);
        state->start_cv.wait(lock, [&] { return state->start_complete; });
    }

    if (!state->start_ok) {
        errno = state->start_errno;
        if (state->thread.joinable()) {
            state->thread.join();
        }
        std::lock_guard<std::mutex> lock(mutex_);
        traces_.erase(pid);
        return false;
    }
    return true;
#endif
#else
    (void)pid;
    return false;
#endif
}

bool PtraceBackend::detach_pid(pid_t pid) {
    if (pid <= 0) {
        return false;
    }

    reap_completed();

#ifdef AGENTVFS_PTRACE
    std::shared_ptr<TraceState> state;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = traces_.find(pid);
        if (it == traces_.end()) {
            return false;
        }
        state = it->second;
        state->detach_requested = true;
    }

    if (state->thread.joinable()) {
        state->thread.join();
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (state->completed && !state->attached) {
        traces_.erase(pid);
        return true;
    }
    return false;
#else
    (void)pid;
    return false;
#endif
}

bool PtraceBackend::detach_requested(
    const std::shared_ptr<TraceState>& state) const {
    if (stop_.load()) {
        return true;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    return state->detach_requested;
}

void PtraceBackend::mark_completed(const std::shared_ptr<TraceState>& state,
                                   bool attached, bool exited, int error) {
    std::lock_guard<std::mutex> lock(mutex_);
    state->completed = true;
    state->attached = attached;
    state->exited = exited;
    state->error = error;
}

void PtraceBackend::reap_completed() {
    std::vector<std::shared_ptr<TraceState>> completed;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto it = traces_.begin(); it != traces_.end();) {
            const auto& state = it->second;
            if (state->completed && !state->attached) {
                completed.push_back(state);
                it = traces_.erase(it);
            } else {
                ++it;
            }
        }
    }

    for (const auto& state : completed) {
        if (state->thread.joinable()) {
            state->thread.join();
        }
    }
}

void PtraceBackend::emit_event(TelemetryEvent ev) {
    if (stop_.load()) {
        return;
    }

    EventCallback cb;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        cb = callback_;
    }
    if (!stop_.load() && cb) {
        cb(std::move(ev));
    }
}

void PtraceBackend::trace_loop(std::shared_ptr<TraceState> state) {
#ifdef AGENTVFS_PTRACE
    pid_t pid = state->pid;
    bool have_snapshot = false;
    bool interrupt_sent = false;
    SyscallSnapshot snapshot{};

    auto signal_start = [&](bool ok, int start_errno) {
        {
            std::lock_guard<std::mutex> lock(state->start_mutex);
            state->start_ok = ok;
            state->start_errno = start_errno;
            state->start_complete = true;
        }
        state->start_cv.notify_all();
    };

    // PTRACE_O_TRACEEXEC is required so the kernel reports execve() via a
    // PTRACE_EVENT_EXEC stop instead of a regular syscall-exit. Without it,
    // execve looks like a stale syscall-exit that pairs with the "before
    // exec" entry snapshot, leaking garbage args from the previous image.
    if (ptrace(PTRACE_SEIZE, pid, nullptr,
               reinterpret_cast<void*>(PTRACE_O_TRACESYSGOOD |
                                       PTRACE_O_TRACEEXEC)) != 0) {
        int seize_errno = errno;
        signal_start(false, seize_errno);
        mark_completed(state, false, false, seize_errno);
        return;
    }
    signal_start(true, 0);
    interrupt_tracee(pid);
    interrupt_sent = true;

    while (true) {
        bool should_detach = detach_requested(state);
        if (should_detach && !interrupt_sent) {
            interrupt_tracee(pid);
            interrupt_sent = true;
        }

        int status = 0;
        pid_t got = waitpid(pid, &status, __WALL | WNOHANG);
        if (got == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }
        if (got < 0) {
            if (errno == EINTR) {
                continue;
            }
            mark_completed(state, false, false, errno);
            break;
        }
        if (got != pid) {
            continue;
        }

        if (WIFEXITED(status) || WIFSIGNALED(status)) {
            mark_completed(state, false, true, 0);
            break;
        }
        if (!WIFSTOPPED(status)) {
            continue;
        }
        interrupt_sent = false;

        int stop_signal = WSTOPSIG(status);
        int ptrace_event = status >> 16;
        if (detach_requested(state)) {
            while (true) {
                if (ptrace(PTRACE_DETACH, pid, nullptr, nullptr) == 0) {
                    mark_completed(state, false, false, 0);
                    break;
                }
                int detach_errno = errno;
                if (detach_errno == ESRCH || detach_errno == ECHILD) {
                    mark_completed(state, false, false, detach_errno);
                    break;
                }
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    state->error = detach_errno;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            break;
        }

        if (ptrace_event == PTRACE_EVENT_STOP) {
            if (!resume_syscall(pid, 0)) {
                mark_completed(state, false, false, errno);
                break;
            }
            continue;
        }

        if (ptrace_event == PTRACE_EVENT_EXEC) {
            // The tracee successfully replaced its image. Any in-flight
            // syscall-entry snapshot belongs to the old image (typically
            // the execve entry itself) and is no longer valid: pairing it
            // with the post-exec syscall-exit would emit an event with
            // garbage args. Drop the snapshot and emit a clean Exec event
            // for the new image.
            have_snapshot = false;
            TelemetryEvent ev{};
            ev.timestamp_ns = now_ns();
            ev.op = OpType::Exec;
            ev.verdict = Verdict::Allow;
            ev.pid = static_cast<uint32_t>(pid);
            ev.backend = "ptrace";
            ev.extra.emplace_back("event", "exec");
            emit_event(std::move(ev));
            if (!resume_syscall(pid, 0)) {
                mark_completed(state, false, false, errno);
                break;
            }
            continue;
        }

        if (stop_signal == (SIGTRAP | 0x80)) {
            SyscallSnapshot current{};
            int64_t ret = 0;
            bool is_entry = false;
            if (get_syscall_state(pid, current, ret, is_entry)) {
                if (is_entry) {
                    snapshot = current;
                    have_snapshot = true;
                } else if (have_snapshot) {
                    have_snapshot = false;
                    if (snapshot.tracked) {
                        emit_event(make_syscall_event(pid, snapshot, ret));
                    }
                }
            }
            if (!resume_syscall(pid, 0)) {
                mark_completed(state, false, false, errno);
                break;
            }
            continue;
        }

        int deliver_signal = stop_signal == SIGTRAP ? 0 : stop_signal;
        if (!resume_syscall(pid, deliver_signal)) {
            mark_completed(state, false, false, errno);
            break;
        }
    }
#else
    (void)state;
#endif
}

} // namespace cas
