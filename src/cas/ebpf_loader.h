#pragma once
#include <cstdint>
#include <string>

namespace cas {

class EbpfLoader {
public:
    EbpfLoader();
    ~EbpfLoader();

    bool load_and_attach();
    void detach();

    bool available() const { return skel_ != nullptr; }
    int session_map_fd() const;
    int policy_map_fd() const;
    int ringbuf_map_fd() const;
    int drops_map_fd() const;

    uint64_t cgroup_id_from_path(const std::string& cgroup_path) const;

    bool register_session(const std::string& cgroup_path,
                          uint64_t session_id,
                          uint32_t branch_id,
                          uint32_t policy_version,
                          uint8_t verbosity);
    bool unregister_session(const std::string& cgroup_path);

    uint64_t total_drops() const;

private:
    struct cas_probe_skel_wrapper;
    struct cas_probe_skel_wrapper* skel_ = nullptr;
};

} // namespace cas
