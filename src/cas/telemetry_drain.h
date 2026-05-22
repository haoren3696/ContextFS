#pragma once
#include "telemetry_event.h"

#include <cstdio>
#include <mutex>
#include <string>

namespace cas {

class Daemon;

class TelemetryDrain {
public:
    TelemetryDrain(Daemon& daemon, std::string out_dir);
    ~TelemetryDrain();

    bool start();
    void stop();
    void write_event(const TelemetryEvent& ev);

private:
    Daemon& daemon_;
    std::string out_dir_;
    std::mutex mu_;
    FILE* out_ = nullptr;
};

} // namespace cas
