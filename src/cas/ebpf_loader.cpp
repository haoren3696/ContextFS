#include "ebpf_loader.h"
#ifdef AGENTVFS_EBPF
#include <bpf/libbpf.h>
#include <bpf/bpf.h>
#include "bpf/cas_maps.h"
#include "cas_probe.skel.h"
#endif
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace cas {

struct EbpfLoader::cas_probe_skel_wrapper {
#ifdef AGENTVFS_EBPF
    cas_probe* skel = nullptr;
#endif
};

EbpfLoader::EbpfLoader() = default;
EbpfLoader::~EbpfLoader() { detach(); }

bool EbpfLoader::load_and_attach() {
#ifdef AGENTVFS_EBPF
    struct stat st;
    if (stat("/sys/kernel/btf/vmlinux", &st) != 0) {
        std::fprintf(stderr, "agentvfs: /sys/kernel/btf/vmlinux missing; eBPF disabled\n");
        return false;
    }
    auto w = new cas_probe_skel_wrapper{};
    w->skel = cas_probe__open_and_load();
    if (!w->skel) { delete w; std::fprintf(stderr, "agentvfs: cas_probe__open_and_load failed\n"); return false; }
    if (cas_probe__attach(w->skel) != 0) {
        std::fprintf(stderr, "agentvfs: cas_probe__attach failed: %s\n", std::strerror(errno));
        cas_probe__destroy(w->skel);
        delete w;
        return false;
    }
    skel_ = w;
    return true;
#else
    std::fprintf(stderr, "agentvfs: compiled without eBPF support\n");
    return false;
#endif
}

void EbpfLoader::detach() {
#ifdef AGENTVFS_EBPF
    if (!skel_) return;
    cas_probe__destroy(skel_->skel);
    delete skel_;
    skel_ = nullptr;
#endif
}

int EbpfLoader::session_map_fd() const {
#ifdef AGENTVFS_EBPF
    return skel_ ? bpf_map__fd(skel_->skel->maps.session_map) : -1;
#else
    return -1;
#endif
}
int EbpfLoader::policy_map_fd() const {
#ifdef AGENTVFS_EBPF
    return skel_ ? bpf_map__fd(skel_->skel->maps.policy_map) : -1;
#else
    return -1;
#endif
}
int EbpfLoader::ringbuf_map_fd() const {
#ifdef AGENTVFS_EBPF
    return skel_ ? bpf_map__fd(skel_->skel->maps.telemetry_ringbuf) : -1;
#else
    return -1;
#endif
}
int EbpfLoader::drops_map_fd() const {
#ifdef AGENTVFS_EBPF
    return skel_ ? bpf_map__fd(skel_->skel->maps.telemetry_drops) : -1;
#else
    return -1;
#endif
}

uint64_t EbpfLoader::cgroup_id_from_path(const std::string& cgroup_path) const {
    int fd = open(cgroup_path.c_str(), O_RDONLY | O_DIRECTORY);
    if (fd < 0) return 0;
    struct stat st;
    if (fstat(fd, &st) != 0) { close(fd); return 0; }
    close(fd);
    return (uint64_t)st.st_ino;
}

bool EbpfLoader::register_session(const std::string& cgroup_path,
                                   uint64_t session_id,
                                   uint32_t branch_id,
                                   uint32_t policy_version,
                                   uint8_t verbosity) {
#ifdef AGENTVFS_EBPF
    if (!skel_) return false;
    uint64_t cg = cgroup_id_from_path(cgroup_path);
    if (!cg) return false;
    struct cas_session_info v = {};
    v.session_id = session_id;
    v.branch_id = branch_id;
    v.policy_version = policy_version;
    v.telemetry_verbosity = verbosity;
    return bpf_map_update_elem(session_map_fd(), &cg, &v, BPF_ANY) == 0;
#else
    (void)cgroup_path; (void)session_id; (void)branch_id; (void)policy_version; (void)verbosity;
    return false;
#endif
}

bool EbpfLoader::unregister_session(const std::string& cgroup_path) {
#ifdef AGENTVFS_EBPF
    if (!skel_) return false;
    uint64_t cg = cgroup_id_from_path(cgroup_path);
    if (!cg) return false;
    return bpf_map_delete_elem(session_map_fd(), &cg) == 0;
#else
    (void)cgroup_path;
    return false;
#endif
}

uint64_t EbpfLoader::total_drops() const {
#ifdef AGENTVFS_EBPF
    if (!skel_) return 0;
    int fd = drops_map_fd();
    int nr = libbpf_num_possible_cpus();
    if (nr <= 0) return 0;
    __u64 vals[256] = {};
    if (nr > 256) nr = 256;
    __u32 k = 0;
    if (bpf_map_lookup_elem(fd, &k, vals) != 0) return 0;
    __u64 total = 0;
    for (int i = 0; i < nr; i++) total += vals[i];
    return total;
#else
    return 0;
#endif
}

} // namespace cas