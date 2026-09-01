cmake_minimum_required(VERSION 3.16)

# ── Version ──────────────────────────────────────────────────────────
# Single source of truth: the most recent git tag (v1.0.0 -> 1.0.0),
# so the CLI's --version, the scanner_version written into the
# database and the GitHub release tag can never drift apart.
# Falls back to OFFCAT_VERSION_DEFAULT when there is no .git (source
# tarballs) or no tag yet; override with -DOFFCAT_VERSION=x.y.z.
set(OFFCAT_VERSION_DEFAULT 1.0.0)
set(OFFCAT_VERSION "" CACHE STRING "offcat version (default: latest git tag)")
if(NOT OFFCAT_VERSION)
    find_package(Git QUIET)
    if(Git_FOUND)
        execute_process(
            COMMAND ${GIT_EXECUTABLE} describe --tags --abbrev=0 --match "v[0-9]*"
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
            RESULT_VARIABLE _offcat_git_result
            OUTPUT_VARIABLE _offcat_git_tag
            ERROR_QUIET
            OUTPUT_STRIP_TRAILING_WHITESPACE
        )
    endif()
    if(_offcat_git_result EQUAL 0 AND _offcat_git_tag MATCHES "^v[0-9]")
        # Strip the leading 'v' (v1.0.0 -> 1.0.0).
        string(SUBSTRING "${_offcat_git_tag}" 1 -1 OFFCAT_VERSION)
    else()
        set(OFFCAT_VERSION "${OFFCAT_VERSION_DEFAULT}")
    endif()
endif()
project(offcat VERSION ${OFFCAT_VERSION} LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# -std=c++17 defines __STRICT_ANSI__, which hides POSIX declarations
# (isatty, fileno, localtime_r ...) in glibc/newlib headers unless
# _POSIX_C_SOURCE is defined.
if(UNIX)
    add_compile_definitions(_POSIX_C_SOURCE=200809L)
endif()

# ── Options ──────────────────────────────────────────────────────────
option(OFFCAT_BUILD_TESTS "Build test suite" ON)
option(OFFCAT_USE_OPENSSL "Use OpenSSL for SHA-256/MD5 (fallback to embedded)" OFF)

# Statically link C++ runtime on MinGW to avoid loading mismatched
# libstdc++/libgcc DLLs from other apps (e.g. Meld) present in PATH.
if(WIN32 AND MINGW)
    add_link_options(-static-libgcc -static-libstdc++ -static)
endif()

# ── Dependencies ─────────────────────────────────────────────────────
include(FetchContent)

# DOWNLOAD_EXTRACT_TIMESTAMP was added in CMake 3.24; on older versions the
# unknown keyword would be swallowed into URL_HASH and break the hash check.
if(CMAKE_VERSION VERSION_GREATER_EQUAL 3.24)
    set(OFFCAT_EP_EXTRACT_TIMESTAMP DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
endif()

# SQLite3 amalgamation (prefer a local copy in third_party/, fall back to download)
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/sqlite-amalgamation-3460100/sqlite3.c")
    set(FETCHCONTENT_SOURCE_DIR_SQLITE3 "${CMAKE_CURRENT_SOURCE_DIR}/third_party/sqlite-amalgamation-3460100")
endif()
FetchContent_Declare(
    sqlite3
    URL ${CMAKE_CURRENT_SOURCE_DIR}/third_party/sqlite-amalgamation-3460100.zip
    URL_HASH SHA3_256=af6aae8d3eccc608857c63cf56efbadc70da48b5c719446b353ed88dded1e288
    ${OFFCAT_EP_EXTRACT_TIMESTAMP}
)
FetchContent_MakeAvailable(sqlite3)

add_library(sqlite3 STATIC
    ${sqlite3_SOURCE_DIR}/sqlite3.c
    ${sqlite3_SOURCE_DIR}/sqlite3.h
)
target_include_directories(sqlite3 PUBLIC ${sqlite3_SOURCE_DIR})
target_compile_definitions(sqlite3 PRIVATE
    SQLITE_ENABLE_FTS5
    SQLITE_ENABLE_WAL
)
if(WIN32)
    target_link_libraries(sqlite3 PRIVATE ws2_32)
endif()

# GoogleTest (tests only)
if(OFFCAT_BUILD_TESTS)
    # Prefer a local googletest extraction when available (avoids the
    # FetchContent extraction step); otherwise the vendored zip in
    # third_party/ keeps offline builds reproducible.
    if(NOT DEFINED GOOGLETEST_SOURCE_DIR)
        if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/googletest-1.14.0")
            set(GOOGLETEST_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/googletest-1.14.0")
        endif()
    endif()
    if(GOOGLETEST_SOURCE_DIR)
        set(FETCHCONTENT_SOURCE_DIR_GOOGLETEST "${GOOGLETEST_SOURCE_DIR}")
    endif()
    FetchContent_Declare(
        googletest
        URL ${CMAKE_CURRENT_SOURCE_DIR}/third_party/googletest-1.14.0.zip
        URL_HASH SHA256=8FE1D2F21DC7A1F5A01008D36F3296A7ABFA77C1E3AC05D90EDC290F2B039F54
        ${OFFCAT_EP_EXTRACT_TIMESTAMP}
    )
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)
    enable_testing()
endif()

# ── offcat_core (static library) ────────────────────────────────────
file(GLOB_RECURSE OFFCAT_CORE_SOURCES
    src/core/*.cpp
    src/catalog/*.cpp
    src/scanner/*.cpp
    src/filesystem/*.cpp
    src/container/*.cpp
    src/database/*.cpp
    src/platform/*.cpp
)

add_library(offcat_core STATIC ${OFFCAT_CORE_SOURCES})
target_include_directories(offcat_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_link_libraries(offcat_core PUBLIC sqlite3)

if(WIN32)
    target_compile_definitions(offcat_core PUBLIC _WIN32_WINNT=0x0601 NOMINMAX)
endif()
# Version string (from git tag, see top of file) is part of the core
# ABI contract: the CLI --version and the scanner_version column in
# the scans table both use it, so expose it to every consumer.
target_compile_definitions(offcat_core PUBLIC OFFCAT_VERSION="${OFFCAT_VERSION}")

# ── offcat_providers (static library) ───────────────────────────────
file(GLOB_RECURSE OFFCAT_PROVIDER_SOURCES
    providers/*.cpp
)

add_library(offcat_providers STATIC ${OFFCAT_PROVIDER_SOURCES})
target_include_directories(offcat_providers PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/providers
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_link_libraries(offcat_providers PUBLIC offcat_core)

# ── offcat_server (static library: HTTP viewer service) ─────────────
# Embed web/index.html into web_resources.h so the single binary stays
# self-contained; --web-root overrides it at runtime.
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/web/index.html" WEB_INDEX_HTML)
string(REPLACE "\\" "\\\\" WEB_INDEX_HTML "${WEB_INDEX_HTML}")
string(REPLACE "\"" "\\\"" WEB_INDEX_HTML "${WEB_INDEX_HTML}")
string(REPLACE "\n" "\\n" WEB_INDEX_HTML "${WEB_INDEX_HTML}")
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/web_resources.h.in"
    "${CMAKE_CURRENT_BINARY_DIR}/web_resources.h"
    @ONLY
)

file(GLOB_RECURSE OFFCAT_SERVER_SOURCES
    src/server/*.cpp
)

add_library(offcat_server STATIC ${OFFCAT_SERVER_SOURCES})
target_include_directories(offcat_server PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_BINARY_DIR}
)
target_link_libraries(offcat_server PUBLIC offcat_core)
if(WIN32)
    target_link_libraries(offcat_server PUBLIC ws2_32)
endif()

# ── offcat (CLI executable) ─────────────────────────────────────────
add_executable(offcat
    cli/main.cpp
    cli/serve.cpp
)
target_link_libraries(offcat PRIVATE offcat_core offcat_providers offcat_server)

# ── Tests ────────────────────────────────────────────────────────────
if(OFFCAT_BUILD_TESTS)
    file(GLOB_RECURSE TEST_SOURCES tests/*.cpp)
    add_executable(offcat_tests ${TEST_SOURCES})
    target_link_libraries(offcat_tests PRIVATE
        offcat_core
        offcat_providers
        offcat_server
        GTest::gtest_main
    )
    target_include_directories(offcat_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/tests
        ${CMAKE_CURRENT_SOURCE_DIR}/providers
    )
    include(GoogleTest)
    if(CMAKE_VERSION VERSION_GREATER_EQUAL 3.25)
        # Windows Defender scans freshly linked binaries, which can exceed
        # the default 5 s discovery timeout on a first build.
        gtest_discover_tests(offcat_tests DISCOVERY_TIMEOUT 60)
    else()
        gtest_discover_tests(offcat_tests)
    endif()
endif()
