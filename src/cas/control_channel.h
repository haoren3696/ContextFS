#pragma once
#include <functional>
#include <string>
#include <string_view>

namespace cas {

class ControlChannel {
public:
    using RequestHandler = std::function<std::string(std::string_view request)>;
    virtual ~ControlChannel() = default;
    virtual bool start(const std::string& endpoint, RequestHandler handler) = 0;
    virtual void stop() = 0;
};

} // namespace cas
