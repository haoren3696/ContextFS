#pragma once
#include <string>
#include <string_view>

namespace cas {
class Daemon;

namespace control_protocol {
std::string dispatch(Daemon& daemon, std::string_view line);
} // namespace control_protocol
} // namespace cas
