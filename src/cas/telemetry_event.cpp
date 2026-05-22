#include "telemetry_event.h"

#include <string>

namespace cas {

const char* optype_to_string(OpType op) {
    switch (op) {
    case OpType::Read: return "read";
    case OpType::Write: return "write";
    case OpType::Open: return "open";
    case OpType::Close: return "close";
    case OpType::Unlink: return "unlink";
    case OpType::Rename: return "rename";
    case OpType::Truncate: return "truncate";
    case OpType::Stat: return "stat";
    case OpType::Exec: return "exec";
    case OpType::Create: return "create";
    }
    return "unknown";
}

const char* verdict_to_string(Verdict v) {
    switch (v) {
    case Verdict::Allow: return "allow";
    case Verdict::SoftWatch: return "soft_watch";
    case Verdict::Deny: return "deny";
    }
    return "unknown";
}

namespace {

void append_json_string(std::string& out, const std::string& value) {
    static constexpr char hex[] = "0123456789abcdef";

    out.push_back('"');
    for (unsigned char ch : value) {
        switch (ch) {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            if (ch < 0x20) {
                out += "\\u00";
                out.push_back(hex[ch >> 4]);
                out.push_back(hex[ch & 0x0f]);
            } else {
                out.push_back(static_cast<char>(ch));
            }
            break;
        }
    }
    out.push_back('"');
}

void append_json_key(std::string& out, const char* key) {
    out.push_back('"');
    out += key;
    out += "\":";
}

void append_comma_key(std::string& out, const char* key) {
    out.push_back(',');
    append_json_key(out, key);
}

} // namespace

std::string event_to_json(const TelemetryEvent& ev) {
    std::string out;
    out.reserve(256 + ev.path.size() + ev.backend.size());

    out.push_back('{');
    append_json_key(out, "ts");
    out += std::to_string(ev.timestamp_ns);
    append_comma_key(out, "session_id");
    out += std::to_string(ev.session_id);
    append_comma_key(out, "branch_id");
    out += std::to_string(ev.branch_id);
    append_comma_key(out, "policy_version");
    out += std::to_string(ev.policy_version);
    append_comma_key(out, "op");
    append_json_string(out, optype_to_string(ev.op));
    append_comma_key(out, "verdict");
    append_json_string(out, verdict_to_string(ev.verdict));
    append_comma_key(out, "dev");
    out += std::to_string(ev.dev);
    append_comma_key(out, "ino");
    out += std::to_string(ev.ino);
    append_comma_key(out, "i_generation");
    out += std::to_string(ev.i_generation);
    append_comma_key(out, "path");
    append_json_string(out, ev.path);
    append_comma_key(out, "pid");
    out += std::to_string(ev.pid);
    append_comma_key(out, "uid");
    out += std::to_string(ev.uid);
    append_comma_key(out, "gid");
    out += std::to_string(ev.gid);
    append_comma_key(out, "bytes");
    out += std::to_string(ev.bytes);
    append_comma_key(out, "latency_ns");
    out += std::to_string(ev.latency_ns);
    append_comma_key(out, "backend");
    append_json_string(out, ev.backend);

    if (!ev.extra.empty()) {
        append_comma_key(out, "extra");
        out.push_back('{');
        for (size_t i = 0; i < ev.extra.size(); ++i) {
            if (i != 0) {
                out.push_back(',');
            }
            append_json_string(out, ev.extra[i].first);
            out.push_back(':');
            append_json_string(out, ev.extra[i].second);
        }
        out.push_back('}');
    }

    out.push_back('}');
    return out;
}

} // namespace cas
