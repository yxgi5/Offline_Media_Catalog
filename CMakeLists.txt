cmake_minimum_required(VERSION 3.16)
project(offcat VERSION 0.1.0 LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

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

# SQLite3 amalgamation (prefer a local copy in third_party/, fall back to download)
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/sqlite-amalgamation-3460100/sqlite3.c")
    set(FETCHCONTENT_SOURCE_DIR_SQLITE3 "${CMAKE_CURRENT_SOURCE_DIR}/third_party/sqlite-amalgamation-3460100")
endif()
FetchContent_Declare(
    sqlite3
    URL ${CMAKE_CURRENT_SOURCE_DIR}/third_party/sqlite-amalgamation-3460100.zip
    URL_HASH SHA3_256=5e46e370e29e389660770e39b81af33d57d2e20f32fc2a85398b85353a6a467d
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
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
    # Prefer a local googletest checkout when available (avoids
    # network download); otherwise fall back to FetchContent URL.
    if(NOT DEFINED GOOGLETEST_SOURCE_DIR)
        if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/googletest")
            set(GOOGLETEST_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/googletest")
        elseif(EXISTS "$ENV{USERPROFILE}/Desktop/googletest")
            set(GOOGLETEST_SOURCE_DIR "$ENV{USERPROFILE}/Desktop/googletest")
        endif()
    endif()
    if(GOOGLETEST_SOURCE_DIR)
        set(FETCHCONTENT_SOURCE_DIR_GOOGLETEST "${GOOGLETEST_SOURCE_DIR}")
    endif()
    FetchContent_Declare(
        googletest
        URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.zip
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
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

# ── offcat (CLI executable) ─────────────────────────────────────────
add_executable(offcat
    cli/main.cpp
    cli/serve.cpp
)
target_link_libraries(offcat PRIVATE offcat_core offcat_providers)
if(WIN32)
    target_link_libraries(offcat PRIVATE ws2_32)
endif()

# ── Tests ────────────────────────────────────────────────────────────
if(OFFCAT_BUILD_TESTS)
    file(GLOB_RECURSE TEST_SOURCES tests/*.cpp)
    add_executable(offcat_tests ${TEST_SOURCES})
    target_link_libraries(offcat_tests PRIVATE
        offcat_core
        offcat_providers
        GTest::gtest_main
    )
    target_include_directories(offcat_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/tests
        ${CMAKE_CURRENT_SOURCE_DIR}/providers
    )
    include(GoogleTest)
    gtest_discover_tests(offcat_tests)
endif()
