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

int fseek_64(FILE* f, int64_t offset, int origin) {
    return _fseeki64(f, offset, origin);
}

bool get_creation_time_utc(const std::string& path, int64_t& out) {
    // Convert the UTF-8 path to UTF-16 (same code page issue as
    // open_file_utf8), then use GetFileAttributesExW which fills the
    // creation time without opening a handle.
    int wide_len = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                       path.c_str(), -1, nullptr, 0);
    if (wide_len <= 0) {
        return false;
    }
    std::wstring wide_path(static_cast<size_t>(wide_len), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path.c_str(), -1,
                            &wide_path[0], wide_len) == 0) {
        return false;
    }
    WIN32_FILE_ATTRIBUTE_DATA data;
    if (!GetFileAttributesExW(wide_path.c_str(), GetFileExInfoStandard, &data)) {
        return false;
    }
    // FILETIME counts 100ns intervals since 1601-01-01; convert to
    // unix seconds (offset 11644473600 = 1601..1970 gap).
    ULARGE_INTEGER ft;
    ft.LowPart = data.ftCreationTime.dwLowDateTime;
    ft.HighPart = data.ftCreationTime.dwHighDateTime;
    out = static_cast<int64_t>(ft.QuadPart / 10000000ULL - 11644473600ULL);
    return true;
}

} // namespace offcat

#else // POSIX

#include <cstdio>
#include <sys/stat.h>

namespace offcat {

FILE* open_file_utf8(const std::string& path, const char* mode) {
    return std::fopen(path.c_str(), mode);
}

int fseek_64(FILE* f, int64_t offset, int origin) {
    return fseeko(f, static_cast<off_t>(offset), origin);
}

bool get_creation_time_utc(const std::string& path, int64_t& out) {
#if defined(__APPLE__)
    struct stat st;
    if (stat(path.c_str(), &st) != 0) {
        return false;
    }
    out = static_cast<int64_t>(st.st_birthtimespec.tv_sec);
    return true;
#else
    // Linux: no portable birth time API (statx is arch-dependent);
    // keep the timestamp NULL rather than mapping a wrong field.
    (void)path;
    (void)out;
    return false;
#endif
}

} // namespace offcat

#endif
