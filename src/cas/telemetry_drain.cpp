#include "telemetry_drain.h"
#include "daemon.h"
#include "posix_compat.h"
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>

namespace cas {

TelemetryDrain::TelemetryDrain(Daemon& d, std::string dir)
    : daemon_(d), out_dir_(std::move(dir)) {}

TelemetryDrain::~TelemetryDrain() { stop(); }

bool TelemetryDrain::start() {
    std::lock_guard<std::mutex> lock(mu_);
    if (out_) {
        return true;
    }

    if (mkdir(out_dir_.c_str(), 0755) != 0 && errno != EEXIST) {
        return false;
    }

    char path[512];
    std::snprintf(path, sizeof(path), "%s/%llu.ndjson", out_dir_.c_str(),
                  (unsigned long long)daemon_.session_id());
    out_ = std::fopen(path, "a");
    return out_ != nullptr;
}

void TelemetryDrain::stop() {
    std::lock_guard<std::mutex> lock(mu_);
    if (out_) {
        std::fclose(out_);
        out_ = nullptr;
    }
}

void TelemetryDrain::write_event(const TelemetryEvent& ev) {
    std::lock_guard<std::mutex> lock(mu_);
    if (!out_) {
        return;
    }

    std::string line = event_to_json(ev);
    std::fwrite(line.data(), 1, line.size(), out_);
    std::fputc('\n', out_);
    std::fflush(out_);
}

} // namespace cas
