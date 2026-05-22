#pragma once
// Soft-watch op bits — matches spec §eBPF policy map.
//
// Kept here as a tiny portable header (no <linux/types.h>) so cas_core
// can reference these constants on any platform without dragging in
// kernel headers. bpf/cas_maps.h (the shared BPF/userspace ABI header)
// includes this file so the BPF probe and userspace stay in sync.
#define CAS_OP_READ      0x01
#define CAS_OP_WRITE     0x02
#define CAS_OP_EXEC      0x04
#define CAS_OP_UNLINK    0x08
#define CAS_OP_RENAME    0x10
#define CAS_OP_TRUNCATE  0x20
#define CAS_OP_CREATE    0x40
