#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "backends/ldpreload_protocol.h"
#include "telemetry_event.h"

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/uio.h>
#include <unistd.h>

namespace {

#ifdef PATH_MAX
constexpr size_t kMaxPathRead = PATH_MAX;
#else
constexpr size_t kMaxPathRead = 4096;
#endif

#if defined(__GNUC__) || defined(__clang__)
#define CAS_PRELOAD_NOINLINE __attribute__((noinline))
#else
#define CAS_PRELOAD_NOINLINE
#endif

using PreloadMsg = cas::ldpreload_protocol::PreloadMsg;

using ReadFn = ssize_t (*)(int, void*, size_t);
using WriteFn = ssize_t (*)(int, const void*, size_t);
using OpenFn = int (*)(const char*, int, ...);
using OpenAtFn = int (*)(int, const char*, int, ...);
using CloseFn = int (*)(int);
using TruncateFn = int (*)(const char*, off_t);
using FtruncateFn = int (*)(int, off_t);
using StatFn = int (*)(const char*, struct stat*);
using FstatFn = int (*)(int, struct stat*);
using SocketFn = int (*)(int, int, int);
using ConnectFn = int (*)(int, const struct sockaddr*, socklen_t);
using SendFn = ssize_t (*)(int, const void*, size_t, int);

ReadFn real_read = nullptr;
WriteFn real_write = nullptr;
OpenFn real_open = nullptr;
OpenFn real_open64 = nullptr;
OpenAtFn real_openat = nullptr;
OpenAtFn real_openat64 = nullptr;
CloseFn real_close = nullptr;
TruncateFn real_truncate = nullptr;
FtruncateFn real_ftruncate = nullptr;
StatFn real_stat = nullptr;
StatFn real_lstat = nullptr;
FstatFn real_fstat = nullptr;
SocketFn real_socket = nullptr;
ConnectFn real_connect = nullptr;
SendFn real_send = nullptr;

pthread_once_t init_once = PTHREAD_ONCE_INIT;
thread_local bool in_hook = false;
// True for the duration of init_real_functions_once on the calling thread.
// dlsym(RTLD_NEXT, ...) can re-enter our interposed hooks (e.g. via libc
// internal helpers); pthread_once is undefined under recursion per POSIX.
// When this flag is set, the wrapper short-circuits and lets the reentrant
// caller fall through to whatever state is available so far. The outer
// pthread_once invocation will finish populating real_* on its own.
thread_local bool init_in_progress = false;

struct HookScope {
    HookScope() { in_hook = true; }
    ~HookScope() { in_hook = false; }
};

template <typename T>
T load_symbol(const char* name) {
    return reinterpret_cast<T>(dlsym(RTLD_NEXT, name));
}

void init_real_functions_once() {
    if (init_in_progress) {
        // Recursive call from within dlsym/glibc lazy resolution; bail out
        // so the outer call can complete without violating pthread_once
        // recursion semantics.
        return;
    }
    init_in_progress = true;
    real_read = load_symbol<ReadFn>("read");
    real_write = load_symbol<WriteFn>("write");
    real_open = load_symbol<OpenFn>("open");
    real_open64 = load_symbol<OpenFn>("open64");
    real_openat = load_symbol<OpenAtFn>("openat");
    real_openat64 = load_symbol<OpenAtFn>("openat64");
    real_close = load_symbol<CloseFn>("close");
    real_truncate = load_symbol<TruncateFn>("truncate");
    real_ftruncate = load_symbol<FtruncateFn>("ftruncate");
    real_stat = load_symbol<StatFn>("stat");
    real_lstat = load_symbol<StatFn>("lstat");
    real_fstat = load_symbol<FstatFn>("fstat");
    real_socket = load_symbol<SocketFn>("socket");
    real_connect = load_symbol<ConnectFn>("connect");
    real_send = load_symbol<SendFn>("send");
    init_in_progress = false;
}

// Fork-safety: pthread_once_t and thread_local state are inherited by the
// child as a memcpy of parent memory. If the parent forks while another
// thread is inside init_real_functions_once, the child wakes with init_once
// still flagged "in progress", which would deadlock the next pthread_once
// call. We register pthread_atfork handlers to:
//   prepare: take the init mutex so no thread can be mid-init across fork
//   parent : release the mutex
//   child  : release the mutex AND reset init_once / per-thread flags
pthread_mutex_t init_atfork_mu = PTHREAD_MUTEX_INITIALIZER;

void atfork_prepare() {
    pthread_mutex_lock(&init_atfork_mu);
}

void atfork_parent() {
    pthread_mutex_unlock(&init_atfork_mu);
}

void atfork_child() {
    // The fork() snapshot may have copied init_once mid-transition; reset it
    // so the surviving child thread can re-run the initializer cleanly.
    pthread_once_t fresh = PTHREAD_ONCE_INIT;
    std::memcpy(&init_once, &fresh, sizeof(init_once));
    in_hook = false;
    init_in_progress = false;
    // The mutex was held by atfork_prepare in this thread; release it so
    // subsequent locks succeed even if the surviving thread is the only one.
    pthread_mutex_unlock(&init_atfork_mu);
}

pthread_once_t atfork_register_once = PTHREAD_ONCE_INIT;

void register_atfork_once() {
    pthread_atfork(atfork_prepare, atfork_parent, atfork_child);
}

void init_real_functions() {
    if (init_in_progress) {
        // We're already inside init on this thread (reentered via dlsym).
        // Returning here avoids a recursive pthread_once which is UB.
        return;
    }
    // Best-effort: register atfork handlers exactly once. Done before the
    // main pthread_once so the child of a fork during init can recover.
    pthread_once(&atfork_register_once, register_atfork_once);
    pthread_once(&init_once, init_real_functions_once);
}

bool open_requires_mode(int flags) {
    if (flags & O_CREAT) {
        return true;
    }
#ifdef O_TMPFILE
    return (flags & O_TMPFILE) == O_TMPFILE;
#else
    return false;
#endif
}

bool valid_socket_path(const char* path) {
    if (!path) {
        return false;
    }
    struct sockaddr_un addr {};
    return std::strlen(path) < sizeof(addr.sun_path);
}

sockaddr_un make_sockaddr(const char* path) {
    struct sockaddr_un addr {};
    addr.sun_family = AF_UNIX;
    std::memcpy(addr.sun_path, path, std::strlen(path) + 1);
    return addr;
}

socklen_t sockaddr_len(const char* path) {
    return static_cast<socklen_t>(offsetof(struct sockaddr_un, sun_path) +
                                  std::strlen(path) + 1);
}

// Per-user default socket path. /tmp/cas_preload.sock is shared across users
// and lets a same-uid attacker race-replace the socket between unlink and
// bind on the daemon side, so we default to a per-user location instead.
// Mirrors the daemon-side helper in ldpreload_backend.cpp.
std::string default_socket_path() {
    const char* xdg = std::getenv("XDG_RUNTIME_DIR");
    if (xdg && xdg[0] != '\0') {
        struct stat st {};
        if (stat(xdg, &st) == 0 && S_ISDIR(st.st_mode)) {
            return std::string(xdg) + "/cas_preload.sock";
        }
    }
    return std::string("/tmp/cas_preload-") +
           std::to_string(static_cast<unsigned long>(geteuid())) + ".sock";
}

std::string configured_socket_path() {
    const char* path = std::getenv("CAS_PRELOAD_SOCKET");
    if (path && path[0] != '\0') {
        return std::string(path);
    }
    return default_socket_path();
}

CAS_PRELOAD_NOINLINE bool path_arg_string(const char* path, std::string& out) {
    auto raw = reinterpret_cast<uintptr_t>(path);
    if (raw == 0) {
        return false;
    }

#ifdef __linux__
    char buffer[kMaxPathRead];
    struct iovec local {};
    local.iov_base = buffer;
    local.iov_len = sizeof(buffer);

    struct iovec remote {};
    remote.iov_base = reinterpret_cast<void*>(raw);
    remote.iov_len = sizeof(buffer);

    ssize_t n = process_vm_readv(getpid(), &local, 1, &remote, 1, 0);
    if (n <= 0) {
        return false;
    }

    void* nul = std::memchr(buffer, '\0', static_cast<size_t>(n));
    if (!nul) {
        return false;
    }
    out.assign(buffer, static_cast<char*>(nul) - buffer);
    return true;
#else
    return false;
#endif
}

// Copy `path` into the wire-protocol buffer. The wire field is fixed at
// kPathSize bytes (NUL-terminated), so paths >= kPathSize must be truncated
// to fit. Returns true if the full path fit, false if it had to be cut.
// Truncation is not an error: the probe still emits an event and sets
// kFlagPathTruncated so the daemon knows downstream tooling must not trust
// the stored path for any allow/deny decision.
bool copy_path_truncating(char (&dest)[cas::ldpreload_protocol::kPathSize],
                          const std::string& path) {
    constexpr std::size_t kMax = cas::ldpreload_protocol::kPathSize;
    bool truncated = path.size() >= kMax;
    std::size_t copy_len = truncated ? (kMax - 1) : path.size();
    std::memcpy(dest, path.c_str(), copy_len);
    dest[copy_len] = '\0';
    return !truncated;
}

std::string path_from_fd(int fd) {
    char proc_path[64];
    int n = std::snprintf(proc_path, sizeof(proc_path), "/proc/self/fd/%d", fd);
    if (n <= 0 || static_cast<size_t>(n) >= sizeof(proc_path)) {
        return {};
    }

    char path[4096];
    ssize_t len = readlink(proc_path, path, sizeof(path) - 1);
    if (len < 0) {
        return {};
    }
    path[len] = '\0';
    return std::string(path);
}

bool is_absolute_path(const char* path) {
    return path && path[0] == '/';
}

CAS_PRELOAD_NOINLINE bool resolve_cwd_path(const char* pathname,
                                           std::string& out) {
    std::string path;
    if (!path_arg_string(pathname, path)) {
        return false;
    }
    if (is_absolute_path(path.c_str())) {
        out = path;
        return true;
    }

    char cwd[4096];
    if (!getcwd(cwd, sizeof(cwd))) {
        return false;
    }
    out = cwd;
    if (!out.empty() && out.back() != '/') {
        out.push_back('/');
    }
    out += path;
    return true;
}

CAS_PRELOAD_NOINLINE bool resolve_openat_path(int dirfd, const char* pathname,
                                              std::string& out) {
    std::string path;
    if (!path_arg_string(pathname, path)) {
        return false;
    }
    if (is_absolute_path(path.c_str())) {
        out = path;
        return true;
    }
    if (dirfd == AT_FDCWD) {
        return resolve_cwd_path(path.c_str(), out);
    }

    std::string base = path_from_fd(dirfd);
    if (base.empty()) {
        return false;
    }
    if (!base.empty() && base.back() == '/') {
        out = base + path;
    } else {
        out = base + "/" + path;
    }
    return true;
}

bool send_full(int fd, const void* data, size_t len) {
    const auto* cursor = static_cast<const char*>(data);
    size_t remaining = len;
    while (remaining > 0) {
        ssize_t n = real_send(fd, cursor, remaining, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        if (n == 0) {
            return false;
        }
        cursor += n;
        remaining -= static_cast<size_t>(n);
    }
    return true;
}

bool recv_verdict(int fd, uint8_t& verdict) {
    while (true) {
        ssize_t n = recv(fd, &verdict, sizeof(verdict), 0);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        return n == static_cast<ssize_t>(sizeof(verdict));
    }
}

void set_socket_timeouts(int fd) {
    struct timeval tv {};
    tv.tv_usec = 200000;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
}

bool preflight_event(cas::OpType op, const std::string& path, uint64_t bytes) {
    if (in_hook) {
        return true;
    }

    HookScope hook_scope;
    init_real_functions();

    if (!real_socket || !real_connect || !real_send || !real_close) {
        return true;
    }

    std::string socket_path = configured_socket_path();
    if (!valid_socket_path(socket_path.c_str())) {
        return true;
    }

    int fd = real_socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        return true;
    }
    set_socket_timeouts(fd);

    struct sockaddr_un addr = make_sockaddr(socket_path.c_str());
    if (real_connect(fd, reinterpret_cast<struct sockaddr*>(&addr),
                     sockaddr_len(socket_path.c_str())) != 0) {
        real_close(fd);
        return true;
    }

    PreloadMsg msg {};
    msg.version = cas::ldpreload_protocol::kCurrentVersion;
    msg.flags = 0;
    msg.op = static_cast<uint8_t>(op);
    msg._reserved = 0;
    msg.pid = static_cast<uint32_t>(getpid());
    msg.bytes = bytes;
    bool full_path = copy_path_truncating(msg.path, path);
    if (!full_path) {
        msg.flags |= cas::ldpreload_protocol::kFlagPathTruncated;
    }

    bool allowed = true;
    if (send_full(fd, &msg, sizeof(msg))) {
        uint8_t verdict = 0;
        if (recv_verdict(fd, verdict) &&
            verdict == static_cast<uint8_t>(cas::Verdict::Deny)) {
            allowed = false;
        }
    }

    real_close(fd);
    return allowed;
}

} // namespace

// Test-only hook: lets unit tests verify the per-user default socket path
// resolution without poking internals. Returns a heap-allocated C string
// (caller frees with free()) instead of a thread-local buffer to avoid
// teardown ordering issues across dlopen/dlclose.
extern "C" char* cas_preload_default_socket_path_for_test() {
    std::string s = default_socket_path();
    char* out = static_cast<char*>(std::malloc(s.size() + 1));
    if (!out) {
        return nullptr;
    }
    std::memcpy(out, s.c_str(), s.size() + 1);
    return out;
}

extern "C" ssize_t read(int fd, void* buf, size_t count) {
    init_real_functions();
    if (!real_read) {
        errno = ENOSYS;
        return -1;
    }

    if (!in_hook) {
        std::string path = path_from_fd(fd);
        if (!preflight_event(cas::OpType::Read, path,
                             static_cast<uint64_t>(count))) {
            errno = EACCES;
            return -1;
        }
    }
    return real_read(fd, buf, count);
}

extern "C" ssize_t write(int fd, const void* buf, size_t count) {
    init_real_functions();
    if (!real_write) {
        errno = ENOSYS;
        return -1;
    }

    if (!in_hook) {
        std::string path = path_from_fd(fd);
        if (!preflight_event(cas::OpType::Write, path,
                             static_cast<uint64_t>(count))) {
            errno = EACCES;
            return -1;
        }
    }
    return real_write(fd, buf, count);
}

extern "C" int open(const char* pathname, int flags, ...) {
    mode_t mode = 0;
    bool has_mode = open_requires_mode(flags);
    if (has_mode) {
        va_list ap;
        va_start(ap, flags);
        mode = static_cast<mode_t>(va_arg(ap, int));
        va_end(ap);
    }

    init_real_functions();
    if (!real_open) {
        errno = ENOSYS;
        return -1;
    }

    std::string path;
    bool have_path = resolve_cwd_path(pathname, path);
    if (!in_hook && have_path &&
        !preflight_event(cas::OpType::Open, path, 0)) {
        errno = EACCES;
        return -1;
    }
    return has_mode ? real_open(pathname, flags, mode)
                    : real_open(pathname, flags);
}

extern "C" int open64(const char* pathname, int flags, ...) {
    mode_t mode = 0;
    bool has_mode = open_requires_mode(flags);
    if (has_mode) {
        va_list ap;
        va_start(ap, flags);
        mode = static_cast<mode_t>(va_arg(ap, int));
        va_end(ap);
    }

    init_real_functions();
    OpenFn open_fn = real_open64 ? real_open64 : real_open;
    if (!open_fn) {
        errno = ENOSYS;
        return -1;
    }

    std::string path;
    bool have_path = resolve_cwd_path(pathname, path);
    if (!in_hook && have_path &&
        !preflight_event(cas::OpType::Open, path, 0)) {
        errno = EACCES;
        return -1;
    }
    return has_mode ? open_fn(pathname, flags, mode)
                    : open_fn(pathname, flags);
}

extern "C" int openat(int dirfd, const char* pathname, int flags, ...) {
    mode_t mode = 0;
    bool has_mode = open_requires_mode(flags);
    if (has_mode) {
        va_list ap;
        va_start(ap, flags);
        mode = static_cast<mode_t>(va_arg(ap, int));
        va_end(ap);
    }

    init_real_functions();
    if (!real_openat) {
        errno = ENOSYS;
        return -1;
    }

    std::string path;
    bool have_path = resolve_openat_path(dirfd, pathname, path);
    if (!in_hook && have_path &&
        !preflight_event(cas::OpType::Open, path, 0)) {
        errno = EACCES;
        return -1;
    }
    return has_mode ? real_openat(dirfd, pathname, flags, mode)
                    : real_openat(dirfd, pathname, flags);
}

extern "C" int openat64(int dirfd, const char* pathname, int flags, ...) {
    mode_t mode = 0;
    bool has_mode = open_requires_mode(flags);
    if (has_mode) {
        va_list ap;
        va_start(ap, flags);
        mode = static_cast<mode_t>(va_arg(ap, int));
        va_end(ap);
    }

    init_real_functions();
    OpenAtFn openat_fn = real_openat64 ? real_openat64 : real_openat;
    if (!openat_fn) {
        errno = ENOSYS;
        return -1;
    }

    std::string path;
    bool have_path = resolve_openat_path(dirfd, pathname, path);
    if (!in_hook && have_path &&
        !preflight_event(cas::OpType::Open, path, 0)) {
        errno = EACCES;
        return -1;
    }
    return has_mode ? openat_fn(dirfd, pathname, flags, mode)
                    : openat_fn(dirfd, pathname, flags);
}

extern "C" int close(int fd) {
    init_real_functions();
    if (!real_close) {
        errno = ENOSYS;
        return -1;
    }

    std::string path = path_from_fd(fd);
    if (!in_hook && !preflight_event(cas::OpType::Close, path, 0)) {
        errno = EACCES;
        return -1;
    }
    return real_close(fd);
}

extern "C" int truncate(const char* path, off_t length) {
    init_real_functions();
    if (!real_truncate) {
        errno = ENOSYS;
        return -1;
    }

    std::string resolved_path;
    bool have_path = resolve_cwd_path(path, resolved_path);
    if (!in_hook && have_path &&
        !preflight_event(cas::OpType::Truncate,
                         resolved_path,
                         static_cast<uint64_t>(length))) {
        errno = EACCES;
        return -1;
    }
    return real_truncate(path, length);
}

extern "C" int ftruncate(int fd, off_t length) {
    init_real_functions();
    if (!real_ftruncate) {
        errno = ENOSYS;
        return -1;
    }

    std::string path = path_from_fd(fd);
    if (!in_hook &&
        !preflight_event(cas::OpType::Truncate, path,
                         static_cast<uint64_t>(length))) {
        errno = EACCES;
        return -1;
    }
    return real_ftruncate(fd, length);
}

extern "C" int stat(const char* path, struct stat* buf) {
    init_real_functions();
    if (!real_stat) {
        errno = ENOSYS;
        return -1;
    }

    std::string resolved_path;
    bool have_path = resolve_cwd_path(path, resolved_path);
    if (!in_hook && have_path &&
        !preflight_event(cas::OpType::Stat,
                         resolved_path, 0)) {
        errno = EACCES;
        return -1;
    }
    return real_stat(path, buf);
}

extern "C" int lstat(const char* path, struct stat* buf) {
    init_real_functions();
    if (!real_lstat) {
        errno = ENOSYS;
        return -1;
    }

    std::string resolved_path;
    bool have_path = resolve_cwd_path(path, resolved_path);
    if (!in_hook && have_path &&
        !preflight_event(cas::OpType::Stat,
                         resolved_path, 0)) {
        errno = EACCES;
        return -1;
    }
    return real_lstat(path, buf);
}

extern "C" int fstat(int fd, struct stat* buf) {
    init_real_functions();
    if (!real_fstat) {
        errno = ENOSYS;
        return -1;
    }

    std::string path = path_from_fd(fd);
    if (!in_hook && !preflight_event(cas::OpType::Stat, path, 0)) {
        errno = EACCES;
        return -1;
    }
    return real_fstat(fd, buf);
}
