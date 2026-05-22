#pragma once

#include "telemetry_backend.h"

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace cas {

class TelemetryRegistry {
public:
    using DrainCallback = std::function<void(TelemetryEvent)>;

    struct BackendRuntimeStatus {
        std::string name;
        Capabilities capabilities;
        bool started = false;
        std::string status;
    };

    explicit TelemetryRegistry(DrainCallback drain);
    // Backend stop() implementations must quiesce callbacks before returning;
    // the destructor calls stop_all() for callbacks that capture this registry.
    ~TelemetryRegistry();

    // Register a backend. MUST be called before start_all(); calls after
    // start_all() are rejected (assert + stderr warning) because the new
    // backend would never be started and its events would be silently lost.
    void add(std::unique_ptr<TelemetryBackend> b);
    void start_all(const BackendConfig& cfg);
    void stop_all();
    bool register_session(const SessionInfo& info);
    bool unregister_session(const std::string& cgroup_path);
    void install_policy(const PolicyRules& rules);

    static Verdict merge_verdicts(const std::vector<Verdict>& verdicts);

    const std::vector<std::unique_ptr<TelemetryBackend>>& backends() const {
        return backends_;
    }
    std::vector<BackendRuntimeStatus> backend_statuses() const;

private:
    struct BackendState {
        TelemetryBackend* backend = nullptr;
        Capabilities capabilities;
        bool is_source = false;
        bool started = false;
        std::string status = "not started";
    };

    void on_event(TelemetryBackend* source, TelemetryEvent event);
    std::vector<TelemetryBackend*> configured_snapshot_locked(bool source) const;
    std::vector<TelemetryBackend*> active_backend_snapshot_locked() const;
    std::vector<TelemetryBackend*> active_processor_snapshot_locked() const;
    BackendState* state_for_locked(TelemetryBackend* backend);

    DrainCallback drain_;
    std::vector<std::unique_ptr<TelemetryBackend>> backends_;
    std::vector<BackendState> states_;
    mutable std::mutex mu_;
    bool started_ = false;
    bool start_attempted_ = false;
};

} // namespace cas
