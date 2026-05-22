#include "backends/bpftime_backend.h"

namespace cas {

std::string BpftimeBackend::name() const {
    return "bpftime";
}

bool BpftimeBackend::start(const BackendConfig& cfg, EventCallback cb) {
    (void)cfg;
    (void)cb;

    stop();

    // This task only introduces the backend surface. Until a compatible
    // bpftime runtime API is integrated, do not include, link, or call bpftime.
    return false;
}

void BpftimeBackend::stop() {
}

bool BpftimeBackend::register_session(const SessionInfo& info) {
    (void)info;
    return true;
}

bool BpftimeBackend::unregister_session(const std::string& cgroup_path) {
    (void)cgroup_path;
    return true;
}

bool BpftimeBackend::install_policy(const PolicyRules& rules) {
    (void)rules;
    return true;
}

Capabilities BpftimeBackend::capabilities() const {
    // Stub contract: supported_ops MUST stay 0 while start() returns false.
    // TelemetryRegistry classifies a backend as a source iff supported_ops != 0,
    // so advertising op coverage we cannot deliver makes the registry expect
    // events that never arrive. When real bpftime runtime integration lands,
    // re-enable the actual op mask here in the same change that makes start()
    // succeed, and update the unit test to match.
    Capabilities caps{};
    caps.supported_ops = 0;
    caps.pre_op_verdicts = false;
    caps.requires_cgroup = false;
    caps.requires_root = false;
    return caps;
}

} // namespace cas
