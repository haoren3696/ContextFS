#include "backends/fanotify_backend.h"

#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <sys/types.h>
#include <utility>

#ifdef AGENTVFS_FANOTIFY
#include <climits>
#include <cstdint>
#include <fcntl.h>
#include <sys/epoll.h>
#include <sys/fanotify.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef O_LARGEFILE
#define O_LARGEFILE 0
#endif
#endif

namespace cas {
namespace fanotify_detail {

using WriteFn = ssize_t (*)(int, const void*, size_t);

bool write_full_response(int fd, const void* data, size_t len, WriteFn write_fn) {
    while (true) {
        ssize_t written = write_fn(fd, data, len);
        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        return written == static_cast<ssize_t>(len);
    }
}

// Linux's /proc/self/fd/N readlink appends this exact 10-byte sentinel when
// the target inode has been unlinked since the fd was opened.
static constexpr char kDeletedSuffix[] = " (deleted)";
static constexpr std::size_t kDeletedSuffixLen = sizeof(kDeletedSuffix) - 1; // 10

ProcLinkResult parse_proc_link_target(const char* buf, ssize_t len, std::size_t buf_size) {
    ProcLinkResult result;
    if (len < 0 || buf == nullptr) {
        result.status = ProcLinkStatus::Error;
        return result;
    }
    // readlink fills up to buf_size bytes without NUL-terminating. If it
    // returns exactly buf_size, the kernel may have truncated the target —
    // we cannot tell, so treat as Truncated rather than silently accept.
    if (static_cast<std::size_t>(len) >= buf_size) {
        result.status = ProcLinkStatus::Truncated;
        return result;
    }

    std::size_t ulen = static_cast<std::size_t>(len);
    // Detect the trailing " (deleted)" sentinel exactly: length must be
    // at least 10 and the final 10 bytes must match byte-for-byte. Substrings
    // like "(deleted)" without a leading space are NOT stripped.
    if (ulen >= kDeletedSuffixLen &&
        std::memcmp(buf + ulen - kDeletedSuffixLen, kDeletedSuffix,
                    kDeletedSuffixLen) == 0) {
        result.path.assign(buf, ulen - kDeletedSuffixLen);
        result.status = ProcLinkStatus::Deleted;
        return result;
    }

    result.path.assign(buf, ulen);
    result.status = ProcLinkStatus::Ok;
    return result;
}

} // namespace fanotify_detail

namespace {

OpMask op_bit(OpType op) {
    return 1u << static_cast<unsigned>(op);
}

#ifdef AGENTVFS_FANOTIFY
void close_fd(int& fd) {
    if (fd >= 0) {
        close(fd);
        fd = -1;
    }
}

uint64_t fanotify_watch_mask() {
    uint64_t mask = FAN_ACCESS | FAN_MODIFY | FAN_OPEN |
                    FAN_CLOSE_WRITE | FAN_CLOSE_NOWRITE |
                    FAN_ACCESS_PERM | FAN_OPEN_PERM;
#ifdef FAN_OPEN_EXEC
    mask |= FAN_OPEN_EXEC;
#endif
#ifdef FAN_OPEN_EXEC_PERM
    mask |= FAN_OPEN_EXEC_PERM;
#endif
    return mask;
}

uint64_t fanotify_permission_mask() {
    uint64_t mask = FAN_ACCESS_PERM | FAN_OPEN_PERM;
#ifdef FAN_OPEN_EXEC_PERM
    mask |= FAN_OPEN_EXEC_PERM;
#endif
    return mask;
}

uint64_t now_ns() {
    struct timespec ts {};
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }
    return static_cast<uint64_t>(ts.tv_sec) * 1000000000ull +
           static_cast<uint64_t>(ts.tv_nsec);
}

// Resolves the path of an open fd via /proc/self/fd/N. Returns a structured
// result so callers can distinguish "deleted between event and lookup" and
// "kernel truncated the target" from a clean read. The caller is expected to
// surface those states via TelemetryEvent::extra rather than fabricating
// a path silently.
fanotify_detail::ProcLinkResult fd_path(int fd) {
    char proc_path[64];
    std::snprintf(proc_path, sizeof(proc_path), "/proc/self/fd/%d", fd);

    // PATH_MAX + 1 capacity, request PATH_MAX so any return value of len ==
    // buf_size (here PATH_MAX) is detectable as truncation by the helper.
    char buf[PATH_MAX + 1];
    ssize_t len = readlink(proc_path, buf, PATH_MAX);
    return fanotify_detail::parse_proc_link_target(buf, len, PATH_MAX);
}

std::string mask_to_hex(uint64_t mask) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "0x%llx",
                  static_cast<unsigned long long>(mask));
    return std::string(buf);
}

OpType op_from_mask(uint64_t mask) {
#ifdef FAN_OPEN_EXEC_PERM
    if (mask & FAN_OPEN_EXEC_PERM) {
        return OpType::Exec;
    }
#endif
#ifdef FAN_OPEN_EXEC
    if (mask & FAN_OPEN_EXEC) {
        return OpType::Exec;
    }
#endif
    if (mask & (FAN_ACCESS_PERM | FAN_ACCESS)) {
        return OpType::Read;
    }
    if (mask & FAN_MODIFY) {
        return OpType::Write;
    }
    if (mask & (FAN_CLOSE_WRITE | FAN_CLOSE_NOWRITE)) {
        return OpType::Close;
    }
    if (mask & (FAN_OPEN_PERM | FAN_OPEN)) {
        return OpType::Open;
    }
    // Fallback: unrecognized mask. We currently report Open so the event still
    // carries through, but this loses fidelity. TODO: surface a dedicated
    // OpType::Unknown sentinel once the schema gains one.
    return OpType::Open;
}

void fill_stat_fields(TelemetryEvent& ev, int fd) {
    struct stat st {};
    if (fstat(fd, &st) != 0) {
        return;
    }
    ev.dev = static_cast<uint32_t>(st.st_dev);
    ev.ino = static_cast<uint64_t>(st.st_ino);
    ev.uid = static_cast<uint32_t>(st.st_uid);
    ev.gid = static_cast<uint32_t>(st.st_gid);
}

TelemetryEvent make_event(const struct fanotify_event_metadata& metadata) {
    TelemetryEvent ev{};
    ev.timestamp_ns = now_ns();
    ev.op = op_from_mask(metadata.mask);
    // Verdict is meaningful only for permission events (FAN_*_PERM). For pure
    // notification events the field is unused by consumers; we default to Allow
    // for schema simplicity rather than implying a verdict was applied.
    ev.verdict = Verdict::Allow;

    auto resolved = fd_path(metadata.fd);
    ev.path = std::move(resolved.path);
    switch (resolved.status) {
        case fanotify_detail::ProcLinkStatus::Deleted:
            ev.extra.emplace_back("deleted", "1");
            break;
        case fanotify_detail::ProcLinkStatus::Truncated:
            ev.extra.emplace_back("path_truncated", "1");
            break;
        case fanotify_detail::ProcLinkStatus::Error:
        case fanotify_detail::ProcLinkStatus::Ok:
            break;
    }

    ev.pid = static_cast<uint32_t>(metadata.pid);
    ev.backend = "fanotify";
    ev.extra.emplace_back("raw_mask", mask_to_hex(metadata.mask));
    fill_stat_fields(ev, metadata.fd);
    return ev;
}

bool allow_permission_event(int fan_fd, int event_fd) {
    struct fanotify_response response {};
    response.fd = event_fd;
    response.response = FAN_ALLOW;
    return fanotify_detail::write_full_response(
        fan_fd, &response, sizeof(response), write);
}
#endif

} // namespace

FanotifyBackend::~FanotifyBackend() {
    stop();
}

std::string FanotifyBackend::name() const {
    return "fanotify";
}

bool FanotifyBackend::start(const BackendConfig& cfg, EventCallback cb) {
    stop();

#ifdef AGENTVFS_FANOTIFY
    if (cfg.mount_path.empty()) {
        return false;
    }

    int fan_fd = fanotify_init(FAN_CLASS_CONTENT | FAN_NONBLOCK | FAN_CLOEXEC,
                               O_RDONLY | O_LARGEFILE);
    if (fan_fd < 0) {
        return false;
    }

    if (fanotify_mark(fan_fd, FAN_MARK_ADD | FAN_MARK_MOUNT,
                      fanotify_watch_mask(), AT_FDCWD,
                      cfg.mount_path.c_str()) != 0) {
        close(fan_fd);
        return false;
    }

    int epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (epoll_fd < 0) {
        close(fan_fd);
        return false;
    }

    struct epoll_event event {};
    event.events = EPOLLIN;
    event.data.fd = fan_fd;
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fan_fd, &event) != 0) {
        close(epoll_fd);
        close(fan_fd);
        return false;
    }

    fan_fd_ = fan_fd;
    epoll_fd_ = epoll_fd;
    mount_path_ = cfg.mount_path;
    callback_ = std::move(cb);
    stop_.store(false);

    try {
        poll_thread_ = std::thread([this] { poll_loop(); });
    } catch (...) {
        close_fd(epoll_fd_);
        close_fd(fan_fd_);
        callback_ = nullptr;
        mount_path_.clear();
        stop_.store(true);
        return false;
    }
    return true;
#else
    (void)cfg;
    (void)cb;
    return false;
#endif
}

void FanotifyBackend::stop() {
    stop_.store(true);
    if (poll_thread_.joinable()) {
        poll_thread_.join();
    }

#ifdef AGENTVFS_FANOTIFY
    close_fd(epoll_fd_);
    close_fd(fan_fd_);
#endif
    callback_ = nullptr;
    mount_path_.clear();
}

bool FanotifyBackend::register_session(const SessionInfo& info) {
    (void)info;
    return true;
}

bool FanotifyBackend::unregister_session(const std::string& cgroup_path) {
    (void)cgroup_path;
    return true;
}

bool FanotifyBackend::install_policy(const PolicyRules& rules) {
    (void)rules;
    return true;
}

Capabilities FanotifyBackend::capabilities() const {
    Capabilities caps{};
    caps.supported_ops = op_bit(OpType::Read) |
                         op_bit(OpType::Write) |
                         op_bit(OpType::Open) |
                         op_bit(OpType::Close) |
                         op_bit(OpType::Exec);
    caps.pre_op_verdicts = false;
    caps.requires_cgroup = false;
    caps.requires_root = true;
    return caps;
}

void FanotifyBackend::poll_loop() {
#ifdef AGENTVFS_FANOTIFY
    if (epoll_fd_ < 0 || fan_fd_ < 0) {
        return;
    }

    while (!stop_.load()) {
        struct epoll_event ready {};
        int n = epoll_wait(epoll_fd_, &ready, 1, 200);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        if (n == 0) {
            continue;
        }

        while (!stop_.load()) {
            if (fan_fd_ < 0) {
                break;
            }

            alignas(struct fanotify_event_metadata) char buf[8192];
            ssize_t len = read(fan_fd_, buf, sizeof(buf));
            if (len < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    break;
                }
                if (errno == EINTR) {
                    continue;
                }
                stop_.store(true);
                break;
            }
            if (len == 0) {
                break;
            }

            auto* metadata =
                reinterpret_cast<struct fanotify_event_metadata*>(buf);
            ssize_t remaining = len;
            for (; FAN_EVENT_OK(metadata, remaining);
                 metadata = FAN_EVENT_NEXT(metadata, remaining)) {
                if (metadata->vers != FANOTIFY_METADATA_VERSION) {
                    stop_.store(true);
                    break;
                }
                if (metadata->mask & FAN_Q_OVERFLOW) {
                    continue;
                }
                if (metadata->fd < 0) {
                    continue;
                }
                if (fan_fd_ < 0 || stop_.load()) {
                    close(metadata->fd);
                    continue;
                }

                TelemetryEvent ev = make_event(*metadata);
                bool response_ok = true;
                if (metadata->mask & fanotify_permission_mask()) {
                    response_ok = allow_permission_event(fan_fd_, metadata->fd);
                    if (!response_ok) {
                        close_fd(fan_fd_);
                        stop_.store(true);
                    }
                }
                close(metadata->fd);

                if (!response_ok) {
                    continue;
                }
                if (!stop_.load() && callback_) {
                    callback_(std::move(ev));
                }
            }
        }
    }

    close_fd(epoll_fd_);
    close_fd(fan_fd_);
#endif
}

} // namespace cas
