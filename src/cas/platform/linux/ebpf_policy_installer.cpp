#include "ebpf_policy_installer.h"
#include "bpf/cas_maps.h"
#ifdef AGENTVFS_EBPF
#include <bpf/bpf.h>
#endif

namespace cas {

size_t EbpfPolicyInstaller::install_policy(const std::vector<Entry>& entries) {
#ifdef AGENTVFS_EBPF
    int pfd = loader_.policy_map_fd();
    // Clear existing entries
    {
        std::vector<cas_path_key> keys;
        cas_path_key next_k;
        // `storage` lives outside the loop so &storage is valid across
        // iterations — declaring it inside the body would leave `prev`
        // pointing into out-of-scope storage on the next call.
        cas_path_key storage;
        cas_path_key* prev = nullptr;
        while (bpf_map_get_next_key(pfd, prev, &next_k) == 0) {
            keys.push_back(next_k);
            storage = next_k;
            prev = &storage;
        }
        for (auto& k : keys) bpf_map_delete_elem(pfd, &k);
    }
    // Install new entries
    size_t installed = 0;
    for (const auto& entry : entries) {
        struct cas_path_key k = {};
        k.dev = entry.dev;
        k.ino = entry.ino;
        k.i_generation = 0;
        struct cas_policy_entry v = {};
        v.soft_watch = entry.soft_watch_bits;
        if (bpf_map_update_elem(pfd, &k, &v, BPF_ANY) == 0) {
            installed++;
        }
    }
    return installed;
#else
    (void)entries;
    return 0;
#endif
}

}  // namespace cas
