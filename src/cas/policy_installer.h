#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace cas {

// Abstract surface for kernel-side policy/session enforcement.
// Linux supplies an eBPF impl in the daemon binary; cas_core uses
// only this interface, never libbpf directly. The default
// implementations are no-ops so platforms without an installer
// still produce correct (empty) status responses.
class PolicyInstaller {
public:
    virtual ~PolicyInstaller() = default;
    virtual bool      available()   const { return false; }
    virtual uint64_t  total_drops() const { return 0; }

    // Anonymous parameters in the base class silence -Wunused-parameter
    // without changing the override surface — derived classes name them
    // freely.
    virtual bool register_session(const std::string& /*cgroup_path*/,
                                  uint64_t /*session_id*/,
                                  uint32_t /*branch_id*/,
                                  uint32_t /*policy_version*/,
                                  uint8_t  /*verbosity*/)         { return true; }
    virtual bool unregister_session(const std::string& /*cgroup_path*/) { return true; }

    struct Entry { uint32_t dev; uint64_t ino; uint8_t soft_watch_bits; };
    virtual size_t install_policy(const std::vector<Entry>& /*entries*/) { return 0; }
};

}  // namespace cas
