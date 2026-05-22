#pragma once

#include "telemetry_backend.h"

namespace cas {

class BpftimeBackend : public TelemetryBackend {
public:
    BpftimeBackend() = default;

    std::string name() const override;
    bool start(const BackendConfig& cfg, EventCallback cb) override;
    void stop() override;
    bool register_session(const SessionInfo& info) override;
    bool unregister_session(const std::string& cgroup_path) override;
    bool install_policy(const PolicyRules& rules) override;
    Capabilities capabilities() const override;
};

} // namespace cas
