// UTF-8 path handling tests.
//
// On Windows, narrow std::fopen() interprets paths with the active ANSI
// code page, so UTF-8 paths containing non-ASCII characters (e.g.
// Chinese) fail to open even though the file exists.  open_file_utf8()
// converts the path to UTF-16 on Windows (_wfopen) and passes it
// through on POSIX.  These tests verify the helper directly and through
// the ISO9660 parser with a file whose path contains CJK characters.

#include "platform/file_util.h"
#include "iso/iso9660/iso9660_parser.h"
#include <gtest/gtest.h>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

using namespace offcat;

namespace {

std::filesystem::path test_dir() {
    return std::filesystem::temp_directory_path() / "offcat_file_util_test";
}

// Minimal ISO9660 image: PVD (sector 16), terminator (17) and a root
// directory (sector 18) holding only "." and "..".
std::vector<uint8_t> make_min_iso() {
    constexpr int kSectorSize = 2048;
    constexpr int kPvd = 16, kTerm = 17, kRoot = 18;
    std::vector<uint8_t> img(19 * kSectorSize, 0);

    auto put_both16 = [&](size_t off, uint16_t v) {
        img[off] = static_cast<uint8_t>(v & 0xFF);
        img[off + 1] = static_cast<uint8_t>((v >> 8) & 0xFF);
        img[off + 2] = static_cast<uint8_t>((v >> 8) & 0xFF);
        img[off + 3] = static_cast<uint8_t>(v & 0xFF);
    };
    auto put_both32 = [&](size_t off, uint32_t v) {
        img[off] = static_cast<uint8_t>(v & 0xFF);
        img[off + 1] = static_cast<uint8_t>((v >> 8) & 0xFF);
        img[off + 2] = static_cast<uint8_t>((v >> 16) & 0xFF);
        img[off + 3] = static_cast<uint8_t>((v >> 24) & 0xFF);
        img[off + 4] = static_cast<uint8_t>((v >> 24) & 0xFF);
        img[off + 5] = static_cast<uint8_t>((v >> 16) & 0xFF);
        img[off + 6] = static_cast<uint8_t>((v >> 8) & 0xFF);
        img[off + 7] = static_cast<uint8_t>(v & 0xFF);
    };
    auto put_dir_record = [&](size_t off, uint8_t name) {
        img[off] = 34;                            // record length
        img[off + 1] = 0;                         // extended attr length
        put_both32(off + 2, kRoot);               // extent
        put_both32(off + 10, kSectorSize);        // size
        img[off + 18] = 124; img[off + 19] = 8;   // 2024-08-28 12:00:00
        img[off + 20] = 28; img[off + 21] = 12;
        img[off + 22] = 0; img[off + 23] = 0; img[off + 24] = 0;
        img[off + 25] = 2;                        // directory flag
        img[off + 26] = 0; img[off + 27] = 0;     // unit size / gap
        put_both16(off + 28, 1);                  // volume sequence
        img[off + 32] = 1;                        // name length
        img[off + 33] = name;                     // 0x00 = ".", 0x01 = ".."
    };

    // Primary Volume Descriptor
    size_t p = static_cast<size_t>(kPvd) * kSectorSize;
    img[p] = 1;
    std::memcpy(&img[p + 1], "CD001", 5);
    img[p + 6] = 1;
    put_both32(p + 80, 19);                       // volume space size
    put_both16(p + 128, kSectorSize);             // logical block size
    put_dir_record(p + 156, 0x00);                // root dir record
    img[p + 1024] = 1;                            // file structure version

    // Volume Descriptor Set Terminator
    size_t t = static_cast<size_t>(kTerm) * kSectorSize;
    img[t] = 255;
    std::memcpy(&img[t + 1], "CD001", 5);
    img[t + 6] = 1;

    // Root directory: "." and ".."
    size_t d = static_cast<size_t>(kRoot) * kSectorSize;
    put_dir_record(d, 0x00);
    put_dir_record(d + 34, 0x01);

    return img;
}

} // namespace

TEST(FileUtilTest, OpenChinesePathReadWrite) {
    const auto dir = test_dir();
    std::filesystem::create_directories(dir);
    const auto file = dir / u8"测试文件.txt";
    {
        std::ofstream out(file, std::ios::binary);
        out << "hello offcat 中文";
    }
    ASSERT_TRUE(std::filesystem::exists(file));

    FILE* f = open_file_utf8(file.string(), "rb");
    ASSERT_NE(f, nullptr);
    char buf[64] = {0};
    size_t n = std::fread(buf, 1, sizeof(buf) - 1, f);
    std::fclose(f);
    ASSERT_GT(n, 0u);
    EXPECT_EQ(std::string(buf, n), "hello offcat 中文");
}

TEST(FileUtilTest, MissingFileReturnsNull) {
    FILE* f = open_file_utf8(test_dir().string() + u8"/不存在的文件.txt", "rb");
    EXPECT_EQ(f, nullptr);
}

// Regression guard: under a non-UTF-8 ANSI code page the narrow fopen()
// must not be able to open the same UTF-8 path (that is exactly why
// open_file_utf8 exists).
TEST(FileUtilTest, NarrowFopenFailsOnChinesePath) {
#ifdef _WIN32
    if (GetACP() == 65001) {
        GTEST_SKIP() << "ANSI code page is UTF-8; narrow fopen works";
    }
#else
    GTEST_SKIP() << "POSIX fopen is UTF-8 aware";
#endif
    const auto dir = test_dir();
    std::filesystem::create_directories(dir);
    const auto file = dir / u8"测试文件.txt";
    if (!std::filesystem::exists(file)) {
        std::ofstream out(file, std::ios::binary);
        out << "x";
    }

    FILE* f = std::fopen(file.string().c_str(), "rb");
    EXPECT_EQ(f, nullptr);
    if (f) std::fclose(f);
}

// The ISO parsers open container files by path; a Chinese-named image
// must parse identically to an ASCII-named one.
TEST(FileUtilTest, IsoParserOpensChinesePath) {
    const auto dir = test_dir();
    std::filesystem::create_directories(dir);
    const auto iso_file = dir / u8"测试镜像.iso";
    const auto data = make_min_iso();
    {
        std::ofstream out(iso_file, std::ios::binary);
        out.write(reinterpret_cast<const char*>(data.data()),
                  static_cast<std::streamsize>(data.size()));
    }

    Iso9660Parser parser(iso_file.string());
    ASSERT_TRUE(parser.open());
    EXPECT_TRUE(parser.volume_info().logical_block_size > 0);

    std::vector<IsoEntry> entries;
    ASSERT_TRUE(parser.read_root_directory(entries));
    EXPECT_TRUE(entries.empty());  // only "." and "..", which are filtered
}

// The scanner records a creation time per entry by default; the
// platform helper must return a sane value for an existing file and
// fail cleanly for a missing one.
TEST(FileUtilTest, CreationTimeUtc) {
    const auto dir = test_dir();
    std::filesystem::create_directories(dir);
    const auto file = dir / "creation_time.bin";
    {
        std::ofstream out(file, std::ios::binary);
        out << "x";
    }

    int64_t birth = 0;
    bool ok = get_creation_time_utc(file.string(), birth);
#ifdef _WIN32
    // Windows always provides a creation time.
    ASSERT_TRUE(ok);
    EXPECT_GT(birth, 0);
    EXPECT_LE(birth, static_cast<int64_t>(std::time(nullptr)) + 1);
#else
    // macOS provides a birth time; Linux may not (returns false).
    if (ok) {
        EXPECT_GT(birth, 0);
    }
#endif

    // Missing file must fail cleanly (never a bogus timestamp).
    int64_t missing_birth = 12345;
    EXPECT_FALSE(get_creation_time_utc(
        (file.string() + ".nope"), missing_birth));
    EXPECT_EQ(missing_birth, 12345);  // untouched on failure

    std::filesystem::remove(file);
}

// Windows `long` is 32-bit, so std::fseek() cannot reach offsets beyond
// 2 GiB; ISO images in the wild routinely exceed that (this project's
// test set includes a 4.6 GiB image).  The 64-bit helper must round-trip
// a sparse file at 3 GiB.
TEST(FileUtilTest, SeekBeyond2GiB) {
    const auto file = test_dir() / "big_sparse.bin";
    std::filesystem::create_directories(test_dir());
    constexpr int64_t kTailOff = int64_t(3) * 1024 * 1024 * 1024 - 4;

    {
        FILE* f = open_file_utf8(file.string(), "wb");
        ASSERT_NE(f, nullptr);
        ASSERT_EQ(fseek_64(f, kTailOff, SEEK_SET), 0);
        ASSERT_EQ(std::fwrite("TAIL", 1, 4, f), 4u);
        std::fclose(f);
    }

    {
        FILE* f = open_file_utf8(file.string(), "rb");
        ASSERT_NE(f, nullptr);
        ASSERT_EQ(fseek_64(f, kTailOff, SEEK_SET), 0);
        char buf[8] = {0};
        ASSERT_EQ(std::fread(buf, 1, 4, f), 4u);
        EXPECT_EQ(std::string(buf, 4), "TAIL");
        std::fclose(f);
    }

    std::filesystem::remove(file);
}
