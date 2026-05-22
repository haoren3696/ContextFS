#pragma once
#include <string>

namespace cas::win {

enum class PreflightResult { Ok, NoSdkRuntime, NoLauncher, MountUnavailable };

PreflightResult preflight(const std::string& mountpoint,
                          std::string& error_out);

}
