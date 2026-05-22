#include "branch_name.h"
#include <cctype>

namespace cas {

bool is_valid_branch_name(const std::string& name) {
    if (name.empty() || name.size() > 64) return false;
    for (char c : name) {
        if (!(std::isalnum((unsigned char)c) || c == '-' || c == '_')) return false;
    }

    // Reserve "main" (used as the default branch) and git-convention-adjacent
    // names plus store subdirectory names so future collisions are impossible.
    static const char* kReserved[] = {
        "main", "HEAD", "FETCH_HEAD", "ORIG_HEAD", "MERGE_HEAD",
        "refs", "objects", "tmp", "telemetry"
    };
    for (const char* reserved : kReserved) {
        if (name == reserved) return false;
    }
    return true;
}

} // namespace cas
