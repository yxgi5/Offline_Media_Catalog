#pragma once

#include <cstdint>
#include <cstdio>
#include <string>

namespace offcat {

// Open a file given a UTF-8 encoded path.
//
// On Windows the narrow std::fopen() interprets the path with the
// active ANSI code page, so UTF-8 paths containing non-ASCII
// characters (e.g. Chinese) silently fail to open.  This helper
// converts the path to UTF-16 and uses _wfopen() instead.  On POSIX
// the UTF-8 path is passed through to fopen() unchanged.
FILE* open_file_utf8(const std::string& path, const char* mode);

// 64-bit seek.  Windows `long` is 32-bit, so std::fseek() cannot reach
// file offsets beyond 2 GiB; ISO images in the wild are routinely
// larger.  POSIX uses fseeko() with a 64-bit off_t.
int fseek_64(FILE* f, int64_t offset, int origin);

// Best-effort creation time (unix epoch seconds) for a UTF-8 path.
//
// Returns false when the platform cannot provide one (e.g. Linux
// without statx support) or the lookup fails; callers treat that as
// "unknown" and keep the timestamp NULL.
bool get_creation_time_utc(const std::string& path, int64_t& out);

} // namespace offcat
