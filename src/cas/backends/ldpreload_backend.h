#pragma once

#include "telemetry_backend.h"

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace cas {

// Returns the per-user default control-socket path used when no explicit
// `socket` parameter is configured. Prefers $XDG_RUNTIME_DIR/cas_preload.sock
// (per-user, mode 0700) and falls back to /tmp/cas_preload-${UID}.sock so a
// same-uid attacker cannot trivially squat on the path.
std::string ldpreload_default_socket_path();

class LdpreloadBackend : public TelemetryBackend {
public:
    LdpreloadBackend() = default;
    ~LdpreloadBackend() override;

    std::string name() const override;
    bool start(const BackendConfig& cfg, EventCallback cb) override;
    void stop() override;
    bool register_session(const SessionInfo& info) override;
    bool unregister_session(const std::string& cgroup_path) override;
    bool install_policy(const PolicyRules& rules) override;
    Capabilities capabilities() const override;

private:
    void accept_loop();

    EventCallback cb_;
    PolicyRules policy_rules_;
    int listen_fd_ = -1;
    std::string socket_path_;
    bool socket_identity_valid_ = false;
    uint64_t socket_dev_ = 0;
    uint64_t socket_ino_ = 0;
    std::atomic<bool> stop_{false};
    std::thread accept_thread_;
    std::mutex mu_;
};

} // namespace cas
