#include "platform/file_util.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <wchar.h>
#include <cwchar>

namespace offcat {

FILE* open_file_utf8(const std::string& path, const char* mode) {
    // Convert the UTF-8 path to UTF-16; only that form reaches the
    // wide variants of the CRT, which do not depend on the ANSI code page.
    int wide_len = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                       path.c_str(), -1, nullptr, 0);
    if (wide_len <= 0) {
        return nullptr;
    }
    std::wstring wide_path(static_cast<size_t>(wide_len), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path.c_str(), -1,
                            &wide_path[0], wide_len) == 0) {
        return nullptr;
    }
    // mode is ASCII by contract; widen it character by character
    std::wstring wide_mode;
    for (const char* p = mode; *p != '\0'; ++p) {
        wide_mode.push_back(static_cast<wchar_t>(*p));
    }
    return _wfopen(wide_path.c_str(), wide_mode.c_str());
}

} // namespace offcat

#else // POSIX

namespace offcat {

FILE* open_file_utf8(const std::string& path, const char* mode) {
    return std::fopen(path.c_str(), mode);
}

} // namespace offcat

#endif
