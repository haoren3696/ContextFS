#include "backends/wasm_backend.h"

#include <string>

namespace cas {
namespace {

std::string configured_module_path(const BackendConfig& cfg) {
    auto it = cfg.params.find("module_path");
    if (it != cfg.params.end()) {
        return it->second;
    }

    it = cfg.params.find("module");
    if (it != cfg.params.end()) {
        return it->second;
    }

    return {};
}

} // namespace

std::string WasmBackend::name() const {
    return "wasm";
}

bool WasmBackend::start(const BackendConfig& cfg, EventCallback cb) {
    (void)cb;

    stop();
    const std::string module_path = configured_module_path(cfg);
    if (module_path.empty()) {
        return false;
    }

    // Task 9 keeps Wasm execution unavailable until a compatible WAMR
    // dependency and API contract are selected and verified.
    return false;
}

void WasmBackend::stop() {
}

bool WasmBackend::register_session(const SessionInfo& info) {
    (void)info;
    return true;
}

bool WasmBackend::unregister_session(const std::string& cgroup_path) {
    (void)cgroup_path;
    return true;
}

bool WasmBackend::install_policy(const PolicyRules& rules) {
    (void)rules;
    return true;
}

Capabilities WasmBackend::capabilities() const {
    // Stub contract: this is a processor backend (supported_ops == 0). When
    // real WAMR integration lands and process_event actually executes a
    // module, leave supported_ops == 0 (still a processor) but flip
    // pre_op_verdicts to true if the module can return Deny pre-op. Update
    // start() and the unit test in the same change.
    Capabilities caps{};
    caps.supported_ops = 0;
    caps.pre_op_verdicts = false;
    caps.requires_cgroup = false;
    caps.requires_root = false;
    return caps;
}

TelemetryEvent WasmBackend::process_event(const TelemetryEvent& ev) {
    return ev;
}

} // namespace cas
