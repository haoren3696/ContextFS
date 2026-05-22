#include "ntstatus_map.h"
#include <cerrno>
#include <cstdio>

namespace cas::win {
NTSTATUS errno_to_ntstatus(int err) {
    switch (err) {
        case 0:        return STATUS_SUCCESS;
        case ENOENT:   return STATUS_OBJECT_NAME_NOT_FOUND;
        case EACCES:   return STATUS_ACCESS_DENIED;
        case EEXIST:   return STATUS_OBJECT_NAME_COLLISION;
        case EISDIR:   return STATUS_FILE_IS_A_DIRECTORY;
        case ENOTDIR:  return STATUS_NOT_A_DIRECTORY;
        case ENOSPC:   return STATUS_DISK_FULL;
#ifdef ESTALE
        // POSIX-only errno; MSVC's UCRT didn't define it until a recent
        // SDK. Guard so older toolchains still compile.
        case ESTALE:   return STATUS_FILE_INVALID;
#endif
        case EIO:      return STATUS_UNEXPECTED_IO_ERROR;
        default:
            std::fprintf(stderr, "cas: unmapped errno %d\n", err);
            return STATUS_UNSUCCESSFUL;
    }
}
}
