#include "backends/lua_backend.h"

#include <string>

namespace cas {
namespace {

std::string configured_script_path(const BackendConfig& cfg) {
    auto it = cfg.params.find("script_path");
    if (it != cfg.params.end()) {
        return it->second;
    }

    it = cfg.params.find("script");
    if (it != cfg.params.end()) {
        return it->second;
    }

    return {};
}

} // namespace

std::string LuaBackend::name() const {
    return "lua";
}

bool LuaBackend::start(const BackendConfig& cfg, EventCallback cb) {
    (void)cb;

    stop();
    const std::string script_path = configured_script_path(cfg);
    if (script_path.empty()) {
        return false;
    }

    // Lua/LuaJIT execution is intentionally not integrated in Task 10.
    return false;
}

void LuaBackend::stop() {
}

bool LuaBackend::register_session(const SessionInfo& info) {
    (void)info;
    return true;
}

bool LuaBackend::unregister_session(const std::string& cgroup_path) {
    (void)cgroup_path;
    return true;
}

bool LuaBackend::install_policy(const PolicyRules& rules) {
    (void)rules;
    return true;
}

Capabilities LuaBackend::capabilities() const {
    // Stub contract: this is a processor backend (supported_ops == 0). When
    // real LuaJIT/Lua integration lands and process_event actually executes a
    // script, leave supported_ops == 0 (still a processor) but flip
    // pre_op_verdicts to true if the script can return Deny pre-op. Update
    // start() and the unit test in the same change.
    Capabilities caps{};
    caps.supported_ops = 0;
    caps.pre_op_verdicts = false;
    caps.requires_cgroup = false;
    caps.requires_root = false;
    return caps;
}

TelemetryEvent LuaBackend::process_event(const TelemetryEvent& ev) {
    return ev;
}

} // namespace cas
