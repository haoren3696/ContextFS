#pragma once

#include "telemetry_backend.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <sys/types.h>
#include <unordered_map>

namespace cas {

class PtraceBackend : public TelemetryBackend {
public:
    PtraceBackend() = default;
    ~PtraceBackend() override;

    std::string name() const override;
    bool start(const BackendConfig& cfg, EventCallback cb) override;
    void stop() override;
    bool register_session(const SessionInfo& info) override;
    bool unregister_session(const std::string& cgroup_path) override;
    bool install_policy(const PolicyRules& rules) override;
    Capabilities capabilities() const override;

    bool attach_pid(pid_t pid);
    bool detach_pid(pid_t pid);

private:
    struct TraceState;

    void trace_loop(std::shared_ptr<TraceState> state);
    bool detach_requested(const std::shared_ptr<TraceState>& state) const;
    void mark_completed(const std::shared_ptr<TraceState>& state,
                        bool attached, bool exited, int error);
    void reap_completed();
    void emit_event(TelemetryEvent ev);

    EventCallback callback_;
    std::atomic<bool> stop_{false};
    std::unordered_map<pid_t, std::shared_ptr<TraceState>> traces_;
    mutable std::mutex mutex_;
};

} // namespace cas
