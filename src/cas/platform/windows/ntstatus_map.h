#pragma once
// Canonical pattern for including ntstatus.h alongside windows.h:
//   define WIN32_NO_STATUS before <windows.h> so winnt.h skips its
//   deprecated STATUS_xxx macros, then include <ntstatus.h> for the
//   canonical definitions. Without this, MSVC emits hundreds of
//   "macro redefinition" warnings AND NTSTATUS itself may not be
//   visible depending on which sub-headers winnt.h picks up.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#define WIN32_NO_STATUS
#include <windows.h>
#undef WIN32_NO_STATUS

// NTSTATUS / PNTSTATUS typedefs. WIN32_NO_STATUS makes winnt.h skip
// these along with the STATUS_xxx constants, and winternl.h's variant
// is gated on conditionals that differ across Windows SDK versions.
// Define them ourselves with the canonical _NTDEF_ guard so any later
// header that checks _NTDEF_ (notably <winfsp/winfsp.h>, which needs
// PNTSTATUS visible) skips its own conflicting typedef.
#ifndef _NTDEF_
typedef long NTSTATUS;
typedef NTSTATUS *PNTSTATUS;
#define _NTDEF_
#endif

#include <ntstatus.h>

namespace cas::win {
NTSTATUS errno_to_ntstatus(int err);
}
