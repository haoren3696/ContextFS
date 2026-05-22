#include "backends/ebpf_backend.h"

#include "daemon.h"
#include "inode_map.h"

#ifdef AGENTVFS_EBPF
#include "bpf/cas_maps.h"
#include <bpf/libbpf.h>
#endif

#include <cerrno>
#include <cstdio>
#include <utility>

namespace cas {
namespace {

OpMask op_bit(OpType op) {
    return 1u << static_cast<unsigned>(op);
}

#ifdef AGENTVFS_EBPF
OpType map_op(uint8_t op_type) {
    switch (op_type) {
    case CAS_OP_TYPE_READ:
        return OpType::Read;
    case CAS_OP_TYPE_WRITE:
        return OpType::Write;
    case CAS_OP_TYPE_OPEN:
        return OpType::Open;
    case CAS_OP_TYPE_UNLINK:
        return OpType::Unlink;
    case CAS_OP_TYPE_RENAME:
        return OpType::Rename;
    case CAS_OP_TYPE_TRUNCATE:
        return OpType::Truncate;
    case CAS_OP_TYPE_EXEC:
        return OpType::Exec;
    case CAS_OP_TYPE_CREATE:
        return OpType::Create;
    default:
        return OpType::Read;
    }
}

Verdict map_verdict(uint8_t verdict) {
    return verdict == CAS_VERDICT_SOFT_WATCH ? Verdict::SoftWatch : Verdict::Allow;
}
#endif

} // namespace

EbpfBackend::EbpfBackend(Daemon& daemon) : daemon_(daemon) {}

EbpfBackend::~EbpfBackend() {
    stop();
}

std::string EbpfBackend::name() const {
    return "ebpf";
}

bool EbpfBackend::start(const BackendConfig& cfg, EventCallback cb) {
    (void)cfg;
    stop();
    callback_ = std::move(cb);
    stop_.store(false);

    if (!loader_.load_and_attach()) {
        callback_ = nullptr;
        return false;
    }

#ifdef AGENTVFS_EBPF
    if (!loader_.available()) {
        callback_ = nullptr;
        return false;
    }
    if (loader_.ringbuf_map_fd() < 0) {
        loader_.detach();
        callback_ = nullptr;
        return false;
    }
    poll_thread_ = std::thread([this] { poll_ringbuf(); });
    return true;
#else
    callback_ = nullptr;
    return false;
#endif
}

void EbpfBackend::stop() {
    stop_.store(true);
    if (poll_thread_.joinable()) {
        poll_thread_.join();
    }
    loader_.detach();
    callback_ = nullptr;
}

bool EbpfBackend::register_session(const SessionInfo& info) {
    return loader_.register_session(info.cgroup_path, info.session_id,
                                    info.branch_id, info.policy_version,
                                    info.verbosity);
}

bool EbpfBackend::unregister_session(const std::string& cgroup_path) {
    return loader_.unregister_session(cgroup_path);
}

// install_policy is intentionally a no-op for the eBPF backend.
//
// Policy rules for eBPF are pushed straight into the kernel BPF policy map
// by ControlSocket's `policy.install` handler via loader().policy_map_fd();
// it bypasses the registry-level fanout because the wire format is
// kernel-specific (struct cas_policy_entry) and the map fd is private to
// the loader. The TelemetryRegistry still calls install_policy() on every
// backend so userspace processors (lua/wasm) get the rule set; for eBPF the
// map has already been populated and there is nothing more to do here.
bool EbpfBackend::install_policy(const PolicyRules& rules) {
    (void)rules;
    return true;
}

Capabilities EbpfBackend::capabilities() const {
    Capabilities caps{};
    caps.supported_ops = op_bit(OpType::Read) |
                         op_bit(OpType::Write) |
                         op_bit(OpType::Unlink) |
                         op_bit(OpType::Rename);
    caps.pre_op_verdicts = false;
    caps.requires_cgroup = true;
    caps.requires_root = true;
    return caps;
}

int EbpfBackend::handle_ringbuf_event(void* ctx, void* data, size_t len) {
#ifdef AGENTVFS_EBPF
    auto* self = static_cast<EbpfBackend*>(ctx);
    if (!self || len < sizeof(cas_telemetry_event)) {
        return 0;
    }

    const auto* raw = static_cast<const cas_telemetry_event*>(data);
    InodeKey key{static_cast<uint64_t>(raw->dev), raw->ino, raw->i_generation};

    TelemetryEvent ev{};
    ev.timestamp_ns = raw->timestamp_ns;
    ev.session_id = raw->session_id;
    ev.branch_id = raw->branch_id;
    ev.policy_version = raw->policy_version;
    ev.op = map_op(raw->op_type);
    ev.verdict = map_verdict(raw->verdict);
    ev.dev = raw->dev;
    ev.ino = raw->ino;
    ev.i_generation = raw->i_generation;
    ev.path = self->daemon_.inode_map().get(key);
    ev.pid = raw->pid;
    ev.uid = raw->uid;
    ev.gid = raw->gid;
    ev.bytes = raw->bytes;
    ev.latency_ns = raw->latency_ns;
    ev.backend = "ebpf";

    if (self->callback_) {
        self->callback_(std::move(ev));
    }
#else
    (void)ctx;
    (void)data;
    (void)len;
#endif
    return 0;
}

void EbpfBackend::poll_ringbuf() {
#ifdef AGENTVFS_EBPF
    int fd = loader_.ringbuf_map_fd();
    if (fd < 0) {
        return;
    }

    struct ring_buffer* rb = ring_buffer__new(
        fd, &EbpfBackend::handle_ringbuf_event, this, nullptr);
    if (!rb) {
        std::fprintf(stderr, "agentvfs: failed to create eBPF telemetry ring buffer\n");
        return;
    }

    while (!stop_.load()) {
        int rc = ring_buffer__poll(rb, 200);
        if (rc < 0 && rc != -EINTR) {
            break;
        }
    }

    ring_buffer__free(rb);
#endif
}

} // namespace cas
