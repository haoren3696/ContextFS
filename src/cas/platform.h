#pragma once
#include <string>
#include <vector>

namespace cas {

class Daemon;

struct MountOptions {
    std::string mountpoint;
    bool foreground = false;
    bool single_threaded = false;
    std::vector<std::string> passthrough_args;
};

// Per-platform entry point. Blocks until filesystem unmounts.
// Returns process exit code.
int run_filesystem(Daemon& daemon, const MountOptions& opts);

}  // namespace cas
