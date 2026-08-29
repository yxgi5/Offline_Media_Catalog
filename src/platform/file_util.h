#pragma once

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

} // namespace offcat
