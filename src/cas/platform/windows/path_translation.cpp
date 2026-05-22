#include "path_translation.h"
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace cas::win {

std::string narrow(const wchar_t* wpath) {
    if (!wpath || !*wpath) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, wpath, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return {};
    std::string out((size_t)(n - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wpath, -1, out.data(), n, nullptr, nullptr);
    for (char& c : out) if (c == '\\') c = '/';
    if (out.empty() || out.front() != '/') out.insert(out.begin(), '/');
    return out;
}

std::wstring widen(const std::string& path) {
    if (path.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, path.data(), (int)path.size(), nullptr, 0);
    if (n <= 0) return {};
    std::wstring out((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, path.data(), (int)path.size(), out.data(), n);
    for (wchar_t& c : out) if (c == L'/') c = L'\\';
    if (out.empty() || out.front() != L'\\') out.insert(out.begin(), L'\\');
    return out;
}

} // namespace cas::win
