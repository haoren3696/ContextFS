#pragma once
#include <string>

namespace cas::win {
std::string  narrow(const wchar_t* wpath);            // \foo\bar -> /foo/bar (UTF-8)
inline std::string narrow(const std::wstring& s) { return narrow(s.c_str()); }
std::wstring widen(const std::string& path);          // /foo/bar -> \foo\bar (UTF-16)
}
