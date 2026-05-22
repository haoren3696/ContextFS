#pragma once
#include "control_channel.h"
#include <atomic>
#include <string>
#include <thread>

namespace cas {

class Daemon;

class ControlSocket : public ControlChannel {
public:
    explicit ControlSocket(Daemon& daemon);
    ~ControlSocket() override;

    bool start(const std::string& endpoint, RequestHandler handler) override;
    void stop() override;

private:
    void accept_loop();

    RequestHandler handler_;
    std::string socket_path_;
    int listen_fd_ = -1;
    std::thread th_;
    std::atomic<bool> stop_{false};
};

} // namespace cas
