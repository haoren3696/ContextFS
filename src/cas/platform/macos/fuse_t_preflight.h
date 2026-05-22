#pragma once
#include <string>

namespace cas::macos {

enum class PreflightResult { Ok, NotInstalled };

// Pure decision logic — exposed for unit testing. Returns Ok if the
// fuse-t bundle directory exists OR pkg_config_finds_fuse_t is true.
// Otherwise returns NotInstalled and writes install instructions to err.
PreflightResult preflight_check(const std::string& fuse_t_bundle_path,
                                bool pkg_config_finds_fuse_t,
                                std::string& err);

// Production entry: probes /Library/Filesystems/fuse-t.fs and
// `pkg-config --exists fuse-t`, then calls preflight_check.
PreflightResult preflight(std::string& err);

}  // namespace cas::macos
