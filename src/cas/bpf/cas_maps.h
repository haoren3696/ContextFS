#pragma once
// Shared layout header for cas eBPF maps. Used by both the BPF probe
// (compiled with clang -target bpf) and userspace (compiled with g++).
// Must not include any kernel-only or libc-only headers.

#ifdef __bpf__
  #include <vmlinux.h>
  #include <bpf/bpf_helpers.h>
#else
  // Use kernel-style typedefs so this header is ABI-compatible with
  // libbpf's headers (which pull <linux/types.h> defining __u64 as
  // `unsigned long long`). Defining our own via uint64_t would conflict
  // with that declaration.
  #include <linux/types.h>
#endif

/* Soft-watch op bits live in a portable sibling header so cas_core
 * can use them without pulling <linux/types.h>. */
#include "../cas_op_bits.h"

enum cas_op_type {
    CAS_OP_TYPE_READ     = 0,
    CAS_OP_TYPE_OPEN     = 1,
    CAS_OP_TYPE_EXEC     = 2,
    CAS_OP_TYPE_WRITE    = 3,
    CAS_OP_TYPE_UNLINK   = 4,
    CAS_OP_TYPE_RENAME   = 5,
    CAS_OP_TYPE_TRUNCATE = 6,
    CAS_OP_TYPE_CREATE   = 7,
};

enum cas_verdict { CAS_VERDICT_ALLOW = 0, CAS_VERDICT_SOFT_WATCH = 1 };

struct cas_session_info {
    __u64 session_id;
    __u32 branch_id;
    __u32 policy_version;
    __u8  telemetry_verbosity;
    __u8  _pad[3];
};

struct cas_path_key {
    __u32 dev;
    __u32 _pad;
    __u64 ino;
    __u32 i_generation;
    __u32 _pad2;
};

struct cas_policy_entry {
    __u8 soft_watch;
    __u8 _pad[7];
};

struct cas_telemetry_event {
    __u64 timestamp_ns;
    __u64 session_id;
    __u32 branch_id;
    __u32 policy_version;
    __u8  op_type;
    __u8  verdict;
    __u8  _pad[6];
    __u32 dev;
    __u64 ino;
    __u32 i_generation;
    __u32 pid;
    __u32 uid;
    __u32 gid;
    __u64 bytes;
    __u64 latency_ns;
};

#define CAS_SESSION_MAP_MAX   64
#define CAS_POLICY_MAP_MAX    65536
#define CAS_RINGBUF_BYTES     (256 * 1024)
