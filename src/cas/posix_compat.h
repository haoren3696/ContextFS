#pragma once

// Compilation shim for cas_core. Including this gives access to POSIX-shaped
// file I/O names — open/read/write/close/lseek/fsync/fdatasync — on every
// supported platform.
//
// This is NOT a semantic shim. It does not make Windows-specific behaviors
// (true symlinks, mode-bit fidelity, directory fsync as a kernel-side
// barrier) correct. Path operations (unlink/mkdir/lstat/readlink) are
// expected to go through std::filesystem, not through this header.
//
// Intentionally never included by any public header: callers that include
// "bootstrap.h" or "daemon.h" must not have these macros leak into their
// translation units (where e.g. `#define close _close` would mangle method
// names like `std::ifstream::close`).

#ifdef _WIN32
  #include <io.h>
  #include <fcntl.h>
  #include <sys/stat.h>
  #include <cstdint>

  // MSVC <sys/stat.h> lacks these predicates.
  #ifndef S_ISREG
    #define S_ISREG(m) (((m) & _S_IFMT) == _S_IFREG)
  #endif
  #ifndef S_ISDIR
    #define S_ISDIR(m) (((m) & _S_IFMT) == _S_IFDIR)
  #endif
  #ifndef S_ISLNK
    #define S_IFLNK    0120000
    #define S_ISLNK(m) 0
  #endif
  #ifndef S_ISSOCK
    #define S_ISSOCK(m) 0
  #endif
  #ifndef S_ISFIFO
    #define S_ISFIFO(m) 0
  #endif

  // POSIX names for MSVC CRT. open/read/write/close/lseek/fsync/fdatasync —
  // only this small set; everything else routes through std::filesystem.
  #ifndef _CAS_POSIX_COMPAT_FUNCS_DEFINED
    #define _CAS_POSIX_COMPAT_FUNCS_DEFINED 1
    #define open       _open
    #define read       _read
    #define write      _write
    #define close      _close
    #define lseek      _lseeki64
    #define fsync      _commit
    #define fdatasync  _commit
  #endif

  using ssize_t = std::int64_t;
  using mode_t  = unsigned short;

  // Open flags absent from MSVC are silently zero (handled at fs layer).
  #ifndef O_DIRECTORY
    #define O_DIRECTORY 0
  #endif
  #ifndef O_NOFOLLOW
    #define O_NOFOLLOW 0
  #endif
  #ifndef O_CLOEXEC
    #define O_CLOEXEC 0
  #endif
#else
  #include <fcntl.h>
  #include <sys/stat.h>
  #include <unistd.h>
  // POSIX always opens files in binary mode. MSVC's <fcntl.h> defines
  // O_BINARY to toggle that behavior; on POSIX we make it a no-op so
  // callers can OR it into every open() unconditionally without leaking
  // platform conditionals into the call sites.
  #ifndef O_BINARY
    #define O_BINARY 0
  #endif
  // macOS has no fdatasync(2). fsync(2) is too lenient — it succeeds
  // even on /dev/null and on character devices generally, which makes
  // the fsync_objects error path untestable and weakens the durability
  // guarantee cas relies on. fcntl(fd, F_FULLFSYNC) is the canonical
  // macOS equivalent: it forces a platter-level flush on regular files
  // and returns EINVAL on /dev/null, matching Linux fdatasync's
  // failure semantics for the unit test.
  #ifdef __APPLE__
    static inline int cas_fdatasync_apple(int fd) {
        return ::fcntl(fd, F_FULLFSYNC, 0);
    }
    #define fdatasync cas_fdatasync_apple
  #endif
#endif
