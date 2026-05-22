#include <vmlinux.h>
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_tracing.h>
#include <bpf/bpf_core_read.h>
#include "cas_maps.h"

char LICENSE[] SEC("license") = "GPL";

struct {
    __uint(type, BPF_MAP_TYPE_HASH);
    __uint(max_entries, CAS_SESSION_MAP_MAX);
    __type(key, __u64);
    __type(value, struct cas_session_info);
} session_map SEC(".maps");

struct {
    __uint(type, BPF_MAP_TYPE_HASH);
    __uint(max_entries, CAS_POLICY_MAP_MAX);
    __type(key, struct cas_path_key);
    __type(value, struct cas_policy_entry);
} policy_map SEC(".maps");

struct {
    __uint(type, BPF_MAP_TYPE_RINGBUF);
    __uint(max_entries, CAS_RINGBUF_BYTES);
} telemetry_ringbuf SEC(".maps");

struct {
    __uint(type, BPF_MAP_TYPE_PERCPU_ARRAY);
    __uint(max_entries, 1);
    __type(key, __u32);
    __type(value, __u64);
} telemetry_drops SEC(".maps");

static __always_inline void inc_drops(void) {
    __u32 k = 0;
    __u64* p = bpf_map_lookup_elem(&telemetry_drops, &k);
    if (p) (*p)++;
}

static __always_inline void
emit_event(struct cas_session_info* sess, __u8 op_type, __u8 verdict,
           struct inode* ino, __u64 bytes, __u64 latency_ns) {
    struct cas_telemetry_event* ev =
        bpf_ringbuf_reserve(&telemetry_ringbuf, sizeof(*ev), 0);
    if (!ev) { inc_drops(); return; }
    ev->timestamp_ns   = bpf_ktime_get_ns();
    ev->session_id     = sess->session_id;
    ev->branch_id      = sess->branch_id;
    ev->policy_version = sess->policy_version;
    ev->op_type        = op_type;
    ev->verdict        = verdict;
    __builtin_memset(ev->_pad, 0, sizeof(ev->_pad));
    struct super_block* sb = BPF_CORE_READ(ino, i_sb);
    ev->dev            = BPF_CORE_READ(sb, s_dev);
    ev->ino            = BPF_CORE_READ(ino, i_ino);
    ev->i_generation   = BPF_CORE_READ(ino, i_generation);
    __u64 id           = bpf_get_current_pid_tgid();
    ev->pid            = id >> 32;
    __u64 ug           = bpf_get_current_uid_gid();
    ev->uid            = ug & 0xffffffff;
    ev->gid            = ug >> 32;
    ev->bytes          = bytes;
    ev->latency_ns     = latency_ns;
    bpf_ringbuf_submit(ev, 0);
}

static __always_inline void
observe(struct inode* ino, __u8 op_type, __u8 op_bit, __u64 bytes) {
    __u64 cg = bpf_get_current_cgroup_id();
    struct cas_session_info* sess = bpf_map_lookup_elem(&session_map, &cg);
    if (!sess) return;

    struct cas_path_key k = {};
    struct super_block* sb = BPF_CORE_READ(ino, i_sb);
    k.dev          = BPF_CORE_READ(sb, s_dev);
    k.ino          = BPF_CORE_READ(ino, i_ino);
    k.i_generation = BPF_CORE_READ(ino, i_generation);
    struct cas_policy_entry* pe = bpf_map_lookup_elem(&policy_map, &k);
    int watched = pe && (pe->soft_watch & op_bit);

    if (watched || sess->telemetry_verbosity >= 2) {
        emit_event(sess, op_type,
                   watched ? CAS_VERDICT_SOFT_WATCH : CAS_VERDICT_ALLOW,
                   ino, bytes, 0);
    }
}

SEC("fentry/vfs_read")
int BPF_PROG(cas_fentry_vfs_read, struct file* file, char* buf, size_t count) {
    struct inode* ino = BPF_CORE_READ(file, f_inode);
    if (!ino) return 0;
    observe(ino, CAS_OP_TYPE_READ, CAS_OP_READ, count);
    return 0;
}

SEC("fentry/vfs_write")
int BPF_PROG(cas_fentry_vfs_write, struct file* file, const char* buf, size_t count) {
    struct inode* ino = BPF_CORE_READ(file, f_inode);
    if (!ino) return 0;
    observe(ino, CAS_OP_TYPE_WRITE, CAS_OP_WRITE, count);
    return 0;
}

// vfs_unlink signature changed around kernel 6.3:
//   pre-6.3: (struct user_namespace*, inode*, dentry*, inode**)
//   6.3+   : (struct mnt_idmap*,      inode*, dentry*, inode**)
// Target is 6.17; on kernels 5.5–6.2 this probe will fail verifier attach.
// load_and_attach() tolerates partial probe failure (whole skeleton fails,
// daemon falls back to "no eBPF"). See spec §"Kernel Requirements".
SEC("fentry/vfs_unlink")
int BPF_PROG(cas_fentry_vfs_unlink, struct mnt_idmap* idmap,
             struct inode* dir, struct dentry* dentry, struct inode** delegated_inode) {
    struct inode* ino = BPF_CORE_READ(dentry, d_inode);
    if (!ino) return 0;
    observe(ino, CAS_OP_TYPE_UNLINK, CAS_OP_UNLINK, 0);
    return 0;
}

SEC("fentry/vfs_rename")
int BPF_PROG(cas_fentry_vfs_rename, struct renamedata* rd) {
    struct dentry* od = BPF_CORE_READ(rd, old_dentry);
    struct inode* ino = BPF_CORE_READ(od, d_inode);
    if (!ino) return 0;
    observe(ino, CAS_OP_TYPE_RENAME, CAS_OP_RENAME, 0);
    return 0;
}
