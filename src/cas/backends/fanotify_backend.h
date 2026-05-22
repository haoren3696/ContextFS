#pragma once

#include "telemetry_backend.h"

#include <atomic>
#include <cstddef>
#include <string>
#include <thread>

namespace cas {

namespace fanotify_detail {

// Status returned by parse_proc_link_target.
enum class ProcLinkStatus {
    Ok,         // Path resolved cleanly.
    Deleted,    // Path was suffixed with " (deleted)" (file unlinked).
    Truncated,  // readlink hit the buffer limit; path is unreliable.
    Error,      // readlink reported a negative length.
};

struct ProcLinkResult {
    std::string path;
    ProcLinkStatus status = ProcLinkStatus::Error;
};

// Parses a buffer filled by readlink("/proc/self/fd/N", buf, buf_size).
//   - `buf` is the raw (NOT NUL-terminated) readlink output bytes.
//   - `len` is the value returned by readlink (bytes written, may be < 0).
//   - `buf_size` is the size passed to readlink as the cap. If `len == buf_size`
//     the kernel may have truncated the result; we treat that as Truncated.
// On Deleted, the trailing " (deleted)" suffix is stripped from the returned path.
ProcLinkResult parse_proc_link_target(const char* buf, ssize_t len, std::size_t buf_size);

} // namespace fanotify_detail

class FanotifyBackend : public TelemetryBackend {
public:
    FanotifyBackend() = default;
    ~FanotifyBackend() override;

    std::string name() const override;
    bool start(const BackendConfig& cfg, EventCallback cb) override;
    void stop() override;
    bool register_session(const SessionInfo& info) override;
    bool unregister_session(const std::string& cgroup_path) override;
    bool install_policy(const PolicyRules& rules) override;
    Capabilities capabilities() const override;

private:
    void poll_loop();

    EventCallback callback_;
    int fan_fd_ = -1;
    int epoll_fd_ = -1;
    std::string mount_path_;
    std::atomic<bool> stop_{false};
    std::thread poll_thread_;
};

} // namespace cas
