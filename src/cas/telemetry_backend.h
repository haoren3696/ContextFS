#pragma once

#include "telemetry_event.h"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace cas {

using OpMask = uint32_t;

struct BackendConfig {
    std::string store_path;
    std::string mount_path;
    std::unordered_map<std::string, std::string> params;
};

struct SessionInfo {
    std::string cgroup_path;
    uint64_t session_id = 0;
    uint32_t branch_id = 0;
    uint32_t policy_version = 0;
    uint8_t  verbosity = 0;
};

struct PolicyRule {
    std::string path_pattern;
    OpMask      soft_watch = 0;  // bitmask of ops to watch
};

struct PolicyRules {
    std::vector<PolicyRule> rules;
};

struct Capabilities {
    OpMask supported_ops = 0;  // bitmask of (1u << static_cast<int>(op)) values
    bool   pre_op_verdicts = false;
    bool   requires_cgroup = false;
    bool   requires_root = false;
};

class TelemetryBackend {
public:
    virtual ~TelemetryBackend() = default;
    virtual std::string name() const = 0;
    virtual bool start(const BackendConfig& cfg, EventCallback cb) = 0;
    // Must quiesce callbacks before returning.
    virtual void stop() = 0;
    virtual bool register_session(const SessionInfo& info) = 0;
    virtual bool unregister_session(const std::string& cgroup_path) = 0;
    virtual bool install_policy(const PolicyRules& rules) = 0;
    virtual Capabilities capabilities() const = 0;

    // Processor backends override this to filter/enrich events and set verdicts.
    // Source backends leave the default (pass-through).
    //
    // Implementations are STRONGLY encouraged to be noexcept: process_event
    // is invoked from source threads (FUSE handlers, BPF poll loops, etc.)
    // and an exception leaking into a C-style callback would be UB. The
    // registry will catch and skip any thrown exception, but events from
    // this processor will be effectively dropped (its mutation is discarded
    // and the chain continues with the pre-call event), so a throwing
    // processor silently degrades into a no-op.
    virtual TelemetryEvent process_event(const TelemetryEvent& ev) { return ev; }
};

} // namespace cas
