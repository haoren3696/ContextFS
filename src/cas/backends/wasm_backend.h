#pragma once

#include "telemetry_backend.h"

namespace cas {

class WasmBackend : public TelemetryBackend {
public:
    WasmBackend() = default;

    std::string name() const override;
    bool start(const BackendConfig& cfg, EventCallback cb) override;
    void stop() override;
    bool register_session(const SessionInfo& info) override;
    bool unregister_session(const std::string& cgroup_path) override;
    bool install_policy(const PolicyRules& rules) override;
    Capabilities capabilities() const override;
    TelemetryEvent process_event(const TelemetryEvent& ev) override;
};

} // namespace cas
