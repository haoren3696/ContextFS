#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "backends/ldpreload_backend.h"

#include "backends/ldpreload_protocol.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <fnmatch.h>
#include <poll.h>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace cas {
namespace {

constexpr std::chrono::milliseconds kClientIdleTimeout{250};
constexpr int kPollTimeoutMs = 50;
constexpr int kConnectProbeTimeoutMs = 50;
constexpr std::size_t kMaxClients = 256;

using ldpreload_protocol::PreloadMsg;

struct PeerCred {
    bool valid = false;
    pid_t pid = 0;
    uid_t uid = 0;
    gid_t gid = 0;
};

struct ClientState {
    int fd = -1;
    std::array<char, sizeof(PreloadMsg)> buffer {};
    std::size_t bytes_read = 0;
    PeerCred peer {};
    std::chrono::steady_clock::time_point deadline;
};

struct SocketIdentity {
    uint64_t dev = 0;
    uint64_t ino = 0;
};

OpMask op_bit(OpType op) {
    return 1u << static_cast<unsigned>(op);
}

bool valid_socket_path(const std::string& path) {
    struct sockaddr_un addr {};
    return !path.empty() && path.size() < sizeof(addr.sun_path);
}

sockaddr_un make_sockaddr(const std::string& path) {
    struct sockaddr_un addr {};
    addr.sun_family = AF_UNIX;
    std::memcpy(addr.sun_path, path.c_str(), path.size() + 1);
    return addr;
}

socklen_t sockaddr_len(const std::string& path) {
    return static_cast<socklen_t>(offsetof(struct sockaddr_un, sun_path) +
                                  path.size() + 1);
}

bool set_cloexec(int fd) {
    int flags = fcntl(fd, F_GETFD);
    if (flags < 0) {
        return false;
    }
    return fcntl(fd, F_SETFD, flags | FD_CLOEXEC) == 0;
}

bool set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL);
    if (flags < 0) {
        return false;
    }
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

void close_fd(int& fd) {
    if (fd >= 0) {
        close(fd);
        fd = -1;
    }
}

bool socket_identity(const std::string& path, SocketIdentity& identity) {
    struct stat st {};
    if (lstat(path.c_str(), &st) != 0 || !S_ISSOCK(st.st_mode)) {
        return false;
    }
    identity.dev = static_cast<uint64_t>(st.st_dev);
    identity.ino = static_cast<uint64_t>(st.st_ino);
    return true;
}

bool socket_identity_matches(const std::string& path,
                             const SocketIdentity& expected) {
    SocketIdentity actual {};
    return socket_identity(path, actual) &&
           actual.dev == expected.dev &&
           actual.ino == expected.ino;
}

bool socket_accepts_connections(const std::string& path) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        return true;
    }
    if (!set_cloexec(fd) || !set_nonblocking(fd)) {
        close(fd);
        return true;
    }

    struct sockaddr_un addr = make_sockaddr(path);
    if (connect(fd, reinterpret_cast<struct sockaddr*>(&addr),
                sockaddr_len(path)) == 0) {
        close(fd);
        return true;
    }

    int connect_errno = errno;
    if (connect_errno == EAGAIN || connect_errno == EWOULDBLOCK ||
        connect_errno == EALREADY || connect_errno == EISCONN ||
        connect_errno == EINTR) {
        close(fd);
        return true;
    }
    if (connect_errno != EINPROGRESS) {
        close(fd);
        return false;
    }

    struct pollfd pfd {};
    pfd.fd = fd;
    pfd.events = POLLOUT;

    int ready = 0;
    do {
        ready = poll(&pfd, 1, kConnectProbeTimeoutMs);
    } while (ready < 0 && errno == EINTR);

    if (ready <= 0) {
        close(fd);
        return true;
    }

    int socket_error = 0;
    socklen_t len = sizeof(socket_error);
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &socket_error, &len) != 0) {
        close(fd);
        return true;
    }

    close(fd);
    if (socket_error == 0) {
        return true;
    }
    return socket_error == EINPROGRESS ||
           socket_error == EAGAIN ||
           socket_error == EWOULDBLOCK ||
           socket_error == EALREADY ||
           socket_error == EISCONN;
}

bool prepare_socket_path_for_bind(const std::string& path) {
    struct stat st {};
    if (lstat(path.c_str(), &st) != 0) {
        return errno == ENOENT;
    }
    if (!S_ISSOCK(st.st_mode)) {
        return false;
    }
    if (socket_accepts_connections(path)) {
        return false;
    }
    return unlink(path.c_str()) == 0 || errno == ENOENT;
}

void unlink_socket_if_owned(const std::string& path,
                            const SocketIdentity& identity,
                            bool identity_valid) {
    if (!path.empty() && identity_valid &&
        socket_identity_matches(path, identity)) {
        unlink(path.c_str());
    }
}

uint64_t now_ns() {
    struct timespec ts {};
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }
    return static_cast<uint64_t>(ts.tv_sec) * 1000000000ull +
           static_cast<uint64_t>(ts.tv_nsec);
}

bool write_verdict(int fd, uint8_t verdict) {
    const auto* cursor = reinterpret_cast<const char*>(&verdict);
    std::size_t remaining = sizeof(verdict);
    while (remaining > 0) {
        ssize_t n = send(fd, cursor, remaining, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                struct pollfd pfd {};
                pfd.fd = fd;
                pfd.events = POLLOUT;
                int ready = poll(&pfd, 1, kPollTimeoutMs);
                if (ready > 0) {
                    continue;
                }
            }
            return false;
        }
        if (n == 0) {
            return false;
        }
        cursor += n;
        remaining -= static_cast<std::size_t>(n);
    }
    return true;
}

bool op_from_wire(uint8_t op, OpType& out) {
    switch (static_cast<OpType>(op)) {
    case OpType::Read:
    case OpType::Write:
    case OpType::Open:
    case OpType::Close:
    case OpType::Truncate:
    case OpType::Stat:
        out = static_cast<OpType>(op);
        return true;
    default:
        return false;
    }
}

PeerCred peer_credentials(int fd) {
    PeerCred peer {};
#ifdef SO_PEERCRED
    struct ucred cred {};
    socklen_t len = sizeof(cred);
    if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &len) == 0 &&
        len == sizeof(cred)) {
        peer.valid = true;
        peer.pid = cred.pid;
        peer.uid = cred.uid;
        peer.gid = cred.gid;
    }
#else
    (void)fd;
#endif
    return peer;
}

bool peer_authorized(const PeerCred& peer) {
    return peer.valid && peer.uid == geteuid();
}

std::string msg_path(const PreloadMsg& msg) {
    return std::string(
        msg.path,
        strnlen(msg.path, cas::ldpreload_protocol::kPathSize));
}

TelemetryEvent make_event(const PreloadMsg& msg, OpType op,
                          const PeerCred& peer) {
    TelemetryEvent ev{};
    ev.timestamp_ns = now_ns();
    ev.op = op;
    ev.verdict = Verdict::Allow;
    ev.path = msg_path(msg);
    ev.pid = peer.valid ? static_cast<uint32_t>(peer.pid) : msg.pid;
    ev.uid = peer.valid ? static_cast<uint32_t>(peer.uid) : 0;
    ev.gid = peer.valid ? static_cast<uint32_t>(peer.gid) : 0;
    ev.bytes = msg.bytes;
    ev.backend = "ldpreload";
    ev.extra.emplace_back("wire_pid", std::to_string(msg.pid));
    if (msg.flags & cas::ldpreload_protocol::kFlagPathTruncated) {
        ev.extra.emplace_back("path_truncated", "1");
    }
    return ev;
}

bool path_matches_pattern(const std::string& pattern, const std::string& path) {
    if (pattern.empty()) {
        return false;
    }
    if (pattern == "**") {
        return true;
    }
    if (fnmatch(pattern.c_str(), path.c_str(), 0) == 0) {
        return true;
    }
    for (std::size_t pos = path.find('/'); pos != std::string::npos;
         pos = path.find('/', pos + 1)) {
        const char* suffix = path.c_str() + pos + 1;
        if (*suffix != '\0' &&
            fnmatch(pattern.c_str(), suffix, 0) == 0) {
            return true;
        }
    }
    return false;
}

Verdict policy_verdict(const PolicyRules& rules, const TelemetryEvent& ev) {
    OpMask bit = op_bit(ev.op);
    for (const PolicyRule& rule : rules.rules) {
        if ((rule.soft_watch & bit) &&
            path_matches_pattern(rule.path_pattern, ev.path)) {
            return Verdict::SoftWatch;
        }
    }
    return Verdict::Allow;
}

// Compute the per-user default socket path. /tmp is shared across users and
// even within a single uid lets a same-user attacker race to replace the
// socket between unlink and bind. Prefer $XDG_RUNTIME_DIR (typically
// /run/user/$UID, mode 0700) when available; fall back to a uid-suffixed
// path under /tmp otherwise.
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

std::string configured_socket_path(const BackendConfig& cfg) {
    auto it = cfg.params.find("socket");
    if (it != cfg.params.end() && !it->second.empty()) {
        return it->second;
    }
    return default_socket_path();
}

void erase_client(std::vector<ClientState>& clients, std::size_t index) {
    close_fd(clients[index].fd);
    clients.erase(clients.begin() + static_cast<std::ptrdiff_t>(index));
}

} // namespace

std::string ldpreload_default_socket_path() {
    return default_socket_path();
}

LdpreloadBackend::~LdpreloadBackend() {
    stop();
}

std::string LdpreloadBackend::name() const {
    return "ldpreload";
}

bool LdpreloadBackend::start(const BackendConfig& cfg, EventCallback cb) {
    stop();

    std::string socket_path = configured_socket_path(cfg);
    if (!valid_socket_path(socket_path)) {
        return false;
    }

    int listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        return false;
    }
    if (!set_cloexec(listen_fd) || !set_nonblocking(listen_fd)) {
        close(listen_fd);
        return false;
    }

    if (!prepare_socket_path_for_bind(socket_path)) {
        close(listen_fd);
        return false;
    }

    struct sockaddr_un addr = make_sockaddr(socket_path);
    if (bind(listen_fd, reinterpret_cast<struct sockaddr*>(&addr),
             sockaddr_len(socket_path)) != 0) {
        close(listen_fd);
        SocketIdentity empty {};
        unlink_socket_if_owned(socket_path, empty, false);
        return false;
    }
    if (chmod(socket_path.c_str(), 0600) != 0) {
        close(listen_fd);
        SocketIdentity identity {};
        if (socket_identity(socket_path, identity)) {
            unlink_socket_if_owned(socket_path, identity, true);
        }
        return false;
    }

    if (listen(listen_fd, SOMAXCONN) != 0) {
        close(listen_fd);
        SocketIdentity identity {};
        if (socket_identity(socket_path, identity)) {
            unlink_socket_if_owned(socket_path, identity, true);
        }
        return false;
    }

    SocketIdentity identity {};
    if (!socket_identity(socket_path, identity)) {
        close(listen_fd);
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(mu_);
        listen_fd_ = listen_fd;
        socket_path_ = socket_path;
        socket_identity_valid_ = true;
        socket_dev_ = identity.dev;
        socket_ino_ = identity.ino;
        cb_ = std::move(cb);
        policy_rules_ = PolicyRules{};
        stop_.store(false);
    }

    try {
        accept_thread_ = std::thread([this] { accept_loop(); });
    } catch (...) {
        {
            std::lock_guard<std::mutex> lock(mu_);
            listen_fd_ = -1;
            socket_path_.clear();
            socket_identity_valid_ = false;
            socket_dev_ = 0;
            socket_ino_ = 0;
            cb_ = nullptr;
            policy_rules_ = PolicyRules{};
            stop_.store(true);
        }
        close(listen_fd);
        unlink_socket_if_owned(socket_path, identity, true);
        return false;
    }

    return true;
}

void LdpreloadBackend::stop() {
    stop_.store(true);

    int listen_fd = -1;
    std::string socket_path;
    SocketIdentity identity {};
    bool identity_valid = false;
    {
        std::lock_guard<std::mutex> lock(mu_);
        listen_fd = listen_fd_;
        listen_fd_ = -1;
        socket_path = socket_path_;
        identity_valid = socket_identity_valid_;
        identity.dev = socket_dev_;
        identity.ino = socket_ino_;
        socket_identity_valid_ = false;
        socket_dev_ = 0;
        socket_ino_ = 0;
    }

    if (listen_fd >= 0) {
        shutdown(listen_fd, SHUT_RDWR);
        close(listen_fd);
    }

    if (accept_thread_.joinable()) {
        accept_thread_.join();
    }

    unlink_socket_if_owned(socket_path, identity, identity_valid);

    {
        std::lock_guard<std::mutex> lock(mu_);
        cb_ = nullptr;
        socket_path_.clear();
        policy_rules_ = PolicyRules{};
    }
}

bool LdpreloadBackend::register_session(const SessionInfo& info) {
    (void)info;
    return true;
}

bool LdpreloadBackend::unregister_session(const std::string& cgroup_path) {
    (void)cgroup_path;
    return true;
}

bool LdpreloadBackend::install_policy(const PolicyRules& rules) {
    std::lock_guard<std::mutex> lock(mu_);
    policy_rules_ = rules;
    return true;
}

Capabilities LdpreloadBackend::capabilities() const {
    Capabilities caps{};
    caps.supported_ops = op_bit(OpType::Read) |
                         op_bit(OpType::Write) |
                         op_bit(OpType::Open) |
                         op_bit(OpType::Close) |
                         op_bit(OpType::Truncate) |
                         op_bit(OpType::Stat);
    // The probe library calls preflight_event() *before* invoking the real
    // libc function and translates Verdict::Deny into errno=EACCES, so this
    // backend can deny operations pre-op. Advertise the capability honestly.
    caps.pre_op_verdicts = true;
    caps.requires_cgroup = false;
    caps.requires_root = false;
    return caps;
}

void LdpreloadBackend::accept_loop() {
    std::vector<ClientState> clients;

    while (true) {
        int listen_fd = -1;
        {
            std::lock_guard<std::mutex> lock(mu_);
            listen_fd = listen_fd_;
        }

        if (stop_.load() && listen_fd < 0) {
            break;
        }

        auto now = std::chrono::steady_clock::now();
        for (std::size_t i = 0; i < clients.size();) {
            if (now >= clients[i].deadline) {
                erase_client(clients, i);
            } else {
                ++i;
            }
        }

        std::size_t client_count_before_poll = clients.size();
        std::vector<struct pollfd> fds;
        fds.reserve(clients.size() + 1);
        if (listen_fd >= 0 && !stop_.load()) {
            struct pollfd pfd {};
            pfd.fd = listen_fd;
            pfd.events = POLLIN;
            fds.push_back(pfd);
        }
        std::size_t client_offset = fds.size();
        for (const ClientState& client : clients) {
            struct pollfd pfd {};
            pfd.fd = client.fd;
            pfd.events = POLLIN | POLLHUP | POLLERR;
            fds.push_back(pfd);
        }

        int ready = 0;
        if (!fds.empty()) {
            ready = poll(fds.data(), fds.size(), kPollTimeoutMs);
            if (ready < 0 && errno == EINTR) {
                continue;
            }
            if (ready < 0) {
                break;
            }
        } else {
            poll(nullptr, 0, kPollTimeoutMs);
        }

        if (!fds.empty() && client_offset == 1 &&
            (fds[0].revents & POLLIN) && listen_fd >= 0 && !stop_.load()) {
            while (clients.size() < kMaxClients) {
                int client_fd = accept(listen_fd, nullptr, nullptr);
                if (client_fd < 0) {
                    if (errno == EINTR) {
                        continue;
                    }
                    if (errno == EAGAIN || errno == EWOULDBLOCK ||
                        errno == EBADF || errno == EINVAL) {
                        break;
                    }
                    break;
                }
                set_cloexec(client_fd);
                if (!set_nonblocking(client_fd)) {
                    close(client_fd);
                    continue;
                }

                PeerCred peer = peer_credentials(client_fd);
                if (!peer_authorized(peer)) {
                    close(client_fd);
                    continue;
                }

                ClientState client {};
                client.fd = client_fd;
                client.peer = peer;
                client.deadline = now + kClientIdleTimeout;
                clients.push_back(client);
            }

            if (clients.size() >= kMaxClients) {
                while (true) {
                    int client_fd = accept(listen_fd, nullptr, nullptr);
                    if (client_fd < 0) {
                        if (errno == EINTR) {
                            continue;
                        }
                        break;
                    }
                    close(client_fd);
                }
            }
        }

        for (std::size_t count = client_count_before_poll; count > 0;) {
            --count;
            std::size_t i = count;
            std::size_t poll_index = client_offset + i;
            if (poll_index >= fds.size() ||
                !(fds[poll_index].revents & (POLLIN | POLLHUP | POLLERR))) {
                continue;
            }

            bool close_client = false;
            while (!close_client &&
                   clients[i].bytes_read < sizeof(PreloadMsg)) {
                char* dst = clients[i].buffer.data() + clients[i].bytes_read;
                std::size_t remaining =
                    sizeof(PreloadMsg) - clients[i].bytes_read;
                ssize_t n = recv(clients[i].fd, dst, remaining, 0);
                if (n > 0) {
                    clients[i].bytes_read += static_cast<std::size_t>(n);
                    clients[i].deadline =
                        std::chrono::steady_clock::now() + kClientIdleTimeout;
                    continue;
                }
                if (n == 0) {
                    close_client = true;
                    break;
                }
                if (errno == EINTR) {
                    continue;
                }
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    break;
                }
                close_client = true;
            }

            if (!close_client &&
                clients[i].bytes_read == sizeof(PreloadMsg)) {
                PreloadMsg msg {};
                std::memcpy(&msg, clients[i].buffer.data(), sizeof(msg));

                Verdict verdict = Verdict::Allow;
                OpType op = OpType::Read;
                if (msg.version !=
                    cas::ldpreload_protocol::kCurrentVersion) {
                    // Politely refuse mismatched probes: drop the connection
                    // without emitting an event or sending a verdict, rather
                    // than crashing. The probe-side will see ECONNRESET.
                    close_client = true;
                } else if (!op_from_wire(msg.op, op)) {
                    close_client = true;
                } else if (!stop_.load()) {
                    TelemetryEvent ev =
                        make_event(msg, op, clients[i].peer);
                    EventCallback cb;
                    {
                        std::lock_guard<std::mutex> lock(mu_);
                        verdict = policy_verdict(policy_rules_, ev);
                        ev.verdict = verdict;
                        cb = cb_;
                    }
                    if (cb && !stop_.load()) {
                        cb(std::move(ev));
                    }
                }

                if (!close_client) {
                    write_verdict(clients[i].fd,
                                  static_cast<uint8_t>(verdict));
                    close_client = true;
                }
            }

            if (close_client) {
                erase_client(clients, i);
            }
        }
    }

    for (ClientState& client : clients) {
        close_fd(client.fd);
    }
}

} // namespace cas
