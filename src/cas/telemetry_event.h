#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace cas {

enum class OpType : uint8_t {
    Read = 0, Write, Open, Close, Unlink, Rename, Truncate, Stat, Exec, Create
};

enum class Verdict : uint8_t {
    Allow = 0, SoftWatch, Deny
};

struct TelemetryEvent {
    uint64_t    timestamp_ns = 0;
    uint64_t    session_id = 0;
    uint32_t    branch_id = 0;
    uint32_t    policy_version = 0;
    OpType      op = OpType::Read;
    Verdict     verdict = Verdict::Allow;
    uint32_t    dev = 0;
    uint64_t    ino = 0;
    uint32_t    i_generation = 0;
    std::string path;
    uint32_t    pid = 0;
    uint32_t    uid = 0;
    uint32_t    gid = 0;
    uint64_t    bytes = 0;
    uint64_t    latency_ns = 0;
    std::string backend;
    std::vector<std::pair<std::string, std::string>> extra;
};

using EventCallback = std::function<void(TelemetryEvent)>;

const char* optype_to_string(OpType op);
const char* verdict_to_string(Verdict v);
std::string event_to_json(const TelemetryEvent& ev);

} // namespace cas
