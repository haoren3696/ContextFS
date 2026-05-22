#pragma once

#include "ebpf_loader.h"
#include "telemetry_backend.h"

#include <atomic>
#include <cstddef>
#include <thread>

namespace cas {

class Daemon;

class EbpfBackend : public TelemetryBackend {
public:
    explicit EbpfBackend(Daemon& daemon);
    ~EbpfBackend() override;

    EbpfLoader& loader() { return loader_; }
    const EbpfLoader& loader() const { return loader_; }

    std::string name() const override;
    bool start(const BackendConfig& cfg, EventCallback cb) override;
    void stop() override;
    bool register_session(const SessionInfo& info) override;
    bool unregister_session(const std::string& cgroup_path) override;
    bool install_policy(const PolicyRules& rules) override;
    Capabilities capabilities() const override;

private:
    static int handle_ringbuf_event(void* ctx, void* data, size_t len);
    void poll_ringbuf();

    Daemon& daemon_;
    EbpfLoader loader_;
    EventCallback callback_;
    std::atomic<bool> stop_{false};
    std::thread poll_thread_;
};

} // namespace cas
