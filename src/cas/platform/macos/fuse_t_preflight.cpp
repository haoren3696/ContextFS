#include "fuse_t_preflight.h"

#include <cstdlib>
#include <filesystem>

namespace cas::macos {

PreflightResult preflight_check(const std::string& fuse_t_bundle_path,
                                bool pkg_config_finds_fuse_t,
                                std::string& err) {
    std::error_code ec;
    bool bundle_ok =
        std::filesystem::is_directory(fuse_t_bundle_path, ec) && !ec;
    if (bundle_ok || pkg_config_finds_fuse_t) {
        err.clear();
        return PreflightResult::Ok;
    }
    err =
        "fuse-t not installed or not detected.\n"
        "Install it from https://www.fuse-t.org or via:\n"
        "  brew install --cask macos-fuse-t/cask/fuse-t";
    return PreflightResult::NotInstalled;
}

static bool pkgconfig_finds_fuse_t() {
    return std::system("pkg-config --exists fuse-t >/dev/null 2>&1") == 0;
}

PreflightResult preflight(std::string& err) {
    return preflight_check("/Library/Filesystems/fuse-t.fs",
                           pkgconfig_finds_fuse_t(), err);
}

}  // namespace cas::macos
