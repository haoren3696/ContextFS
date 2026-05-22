#pragma once

#include <cstddef>
#include <cstdint>

namespace cas::ldpreload_protocol {

constexpr std::size_t kPathSize = 256;

// Wire-protocol version. Bumped whenever the on-the-wire layout of PreloadMsg
// changes. The daemon-side parser refuses messages whose version does not
// match kCurrentVersion.
constexpr uint8_t kCurrentVersion = 1;

// Bit flags for PreloadMsg::flags.
constexpr uint8_t kFlagPathTruncated = 1u << 0;

// Wire format (272 bytes):
//   version : u8   ; protocol version, must equal kCurrentVersion
//   flags   : u8   ; bit-flags, e.g. kFlagPathTruncated
//   op      : u8   ; OpType enum value
//   _pad    : u8   ; reserved, must be zero
//   pid     : u32  ; client pid (peer credentials override on server)
//   bytes   : u64  ; payload byte count for read/write/truncate
//   path    : [256]; NUL-terminated absolute path (truncated if too long)
struct PreloadMsg {
    uint8_t  version;
    uint8_t  flags;
    uint8_t  op;
    uint8_t  _reserved;
    uint32_t pid;
    uint64_t bytes;
    char     path[kPathSize];
};

static_assert(offsetof(PreloadMsg, version) == 0,
              "unexpected PreloadMsg version offset");
static_assert(offsetof(PreloadMsg, flags) == 1,
              "unexpected PreloadMsg flags offset");
static_assert(offsetof(PreloadMsg, op) == 2,
              "unexpected PreloadMsg op offset");
static_assert(offsetof(PreloadMsg, pid) == 4,
              "unexpected PreloadMsg pid offset");
static_assert(offsetof(PreloadMsg, bytes) == 8,
              "unexpected PreloadMsg bytes offset");
static_assert(offsetof(PreloadMsg, path) == 16,
              "unexpected PreloadMsg path offset");
static_assert(sizeof(PreloadMsg) == 272, "unexpected PreloadMsg size");

} // namespace cas::ldpreload_protocol
