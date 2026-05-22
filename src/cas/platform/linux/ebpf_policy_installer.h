#pragma once
#include "policy_installer.h"
#include "ebpf_loader.h"
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace cas {

class EbpfPolicyInstaller : public PolicyInstaller {
public:
    explicit EbpfPolicyInstaller(EbpfLoader& loader) : loader_(loader) {}
    bool      available()   const override { return loader_.available(); }
    uint64_t  total_drops() const override { return loader_.total_drops(); }

    bool register_session(const std::string& cgroup_path,
                          uint64_t session_id,
                          uint32_t branch_id,
                          uint32_t policy_version,
                          uint8_t  verbosity) override {
        return loader_.register_session(cgroup_path, session_id, branch_id,
                                        policy_version, verbosity);
    }

    bool unregister_session(const std::string& cgroup_path) override {
        return loader_.unregister_session(cgroup_path);
    }

    size_t install_policy(const std::vector<Entry>& entries) override;

private:
    EbpfLoader& loader_;
};

}  // namespace cas
