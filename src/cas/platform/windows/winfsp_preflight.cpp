#include "winfsp_preflight.h"
// ntstatus_map.h sets up WIN32_NO_STATUS before <windows.h> so the
// STATUS_xxx macros (re)defined in <ntstatus.h> via winfsp.h don't
// double-define winnt.h's deprecated variants.
#include "ntstatus_map.h"
#include <winfsp/winfsp.h>
#include <cstdio>

namespace cas::win {

PreflightResult preflight(const std::string& mountpoint,
                          std::string& error_out) {
    if (FspLoad(nullptr) != STATUS_SUCCESS) {
        error_out = "WinFsp runtime not loaded. Install WinFsp 2.0+ "
                    "from https://winfsp.dev";
        return PreflightResult::NoSdkRuntime;
    }
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) {
        error_out = "OpenSCManager failed";
        return PreflightResult::NoLauncher;
    }
    SC_HANDLE svc = OpenServiceW(scm, L"WinFsp.Launcher", SERVICE_QUERY_STATUS);
    CloseServiceHandle(scm);
    if (!svc) {
        error_out = "WinFsp.Launcher service not installed. Install WinFsp "
                    "2.0+ from https://winfsp.dev";
        return PreflightResult::NoLauncher;
    }
    CloseServiceHandle(svc);

    // Convert the mountpoint to wide chars without `widen()`'s
    // virtual-path mangling. The mountpoint is a real Windows path
    // (e.g. "Z:" or "C:\mount") — running it through widen() would
    // prepend a stray leading backslash and break drive letters.
    std::wstring wm;
    if (!mountpoint.empty()) {
        int n = MultiByteToWideChar(CP_UTF8, 0, mountpoint.data(),
                                    (int)mountpoint.size(), nullptr, 0);
        if (n > 0) {
            wm.assign((size_t)n, L'\0');
            MultiByteToWideChar(CP_UTF8, 0, mountpoint.data(),
                                (int)mountpoint.size(), wm.data(), n);
        }
    }
    while (!wm.empty() && (wm.back() == L'\\' || wm.back() == L'/'))
        wm.pop_back();
    // L"" + ANSI literal → wide literal at compile time; the WinFsp
    // macro itself is ANSI but FspFileSystemPreflight wants PWSTR.
    // wm.data() returns wchar_t* (non-const) in C++17, matching the
    // PWSTR parameter without a const_cast.
    NTSTATUS s = FspFileSystemPreflight(
        const_cast<PWSTR>(L"" FSP_FSCTL_DISK_DEVICE_NAME), wm.data());
    if (!NT_SUCCESS(s)) {
        char buf[96];
        std::snprintf(buf, sizeof(buf),
            "FspFileSystemPreflight failed 0x%lx", (unsigned long)s);
        error_out = buf;
        return PreflightResult::MountUnavailable;
    }
    return PreflightResult::Ok;
}

}
