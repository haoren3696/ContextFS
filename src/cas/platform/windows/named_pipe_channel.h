#pragma once
#include "control_channel.h"
#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace cas {

class NamedPipeControlChannel : public ControlChannel {
public:
    NamedPipeControlChannel();
    ~NamedPipeControlChannel() override;
    bool start(const std::string& endpoint, RequestHandler handler) override;
    void stop() override;

private:
    void accept_loop();
    void serve_one_client(void* pipe_handle);

    RequestHandler handler_;
    std::wstring pipe_name_w_;
    std::thread accept_thread_;
    std::atomic<bool> stop_{false};
    std::mutex workers_mu_;
    std::vector<std::thread> workers_;
};

}
