// UDF parser resilience tests.
//
// Two synthetic images are built in memory (no external tools):
//   1. A genisoimage -udf style image: the LVD FSD pointer is 0, the
//      partition maps are empty and the PD partition start is junk, but
//      the whole directory tree lives right after the anchor with block
//      numbers relative to the FSD sector and 8-byte short_ad allocation
//      descriptors.  The parser must fall back to the head scan.
//   2. A standard image: valid LVD FSD pointer, partition map and
//      16-byte long_ad descriptors.  The normal path must keep working.

#include "iso/udf/udf_parser.h"
#include <gtest/gtest.h>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace offcat;

namespace {

// ── Byte helpers ────────────────────────────────────────────────────

void put_le16(std::vector<uint8_t>& v, size_t off, uint16_t val) {
    v[off] = static_cast<uint8_t>(val & 0xFF);
    v[off + 1] = static_cast<uint8_t>((val >> 8) & 0xFF);
}

void put_le32(std::vector<uint8_t>& v, size_t off, uint32_t val) {
    for (int i = 0; i < 4; i++) {
        v[off + i] = static_cast<uint8_t>((val >> (8 * i)) & 0xFF);
    }
}

void put_le64(std::vector<uint8_t>& v, size_t off, uint64_t val) {
    for (int i = 0; i < 8; i++) {
        v[off + i] = static_cast<uint8_t>((val >> (8 * i)) & 0xFF);
    }
}

// 16-byte descriptor tag with a valid checksum (sum of all bytes except
// byte 4 equals byte 4).
void make_tag(std::vector<uint8_t>& sec, size_t off, uint16_t id,
              uint32_t location) {
    put_le16(sec, off, id);          // tag identifier
    put_le16(sec, off + 2, 2);       // tag version (UDF 2.00)
    sec[off + 4] = 0;                // checksum placeholder
    sec[off + 5] = 0;                // reserved
    put_le16(sec, off + 6, 0);       // serial
    put_le16(sec, off + 8, 0);       // desc crc
    put_le16(sec, off + 10, 0);      // desc crc length
    put_le32(sec, off + 12, location);

    uint8_t sum = 0;
    for (int i = 0; i < 16; i++) {
        if (i == 4) continue;
        sum = static_cast<uint8_t>((sum + sec[off + i]) & 0xFF);
    }
    sec[off + 4] = sum;
}

// ── Descriptor builders ─────────────────────────────────────────────

// File Entry (tag 261).  `alloc` holds the allocation descriptors to
// write after the 176-byte header.
void put_fe(std::vector<uint8_t>& img, int64_t sector, uint32_t tag_loc,
            uint8_t file_type, uint64_t info_len,
            const std::vector<uint8_t>& alloc) {
    auto sec = img.data() + sector * 2048;
    std::memset(sec, 0, 2048);
    make_tag(img, static_cast<size_t>(sector) * 2048, TAG_FE, tag_loc);
    sec[16 + 11] = file_type;        // file type inside the ICB tag
    put_le64(img, static_cast<size_t>(sector) * 2048 + 56, info_len);
    put_le32(img, static_cast<size_t>(sector) * 2048 + 172,
             static_cast<uint32_t>(alloc.size()));
    std::memcpy(sec + 176, alloc.data(), alloc.size());
}

// Append one File Identifier Descriptor to `img` inside `sector` at
// `off`; returns the new offset (4-byte aligned).
size_t put_fid(std::vector<uint8_t>& img, int64_t sector, size_t off,
               uint8_t chars, const std::string& name, uint32_t icb_loc,
               uint32_t icb_len = 2048) {
    const size_t base = static_cast<size_t>(sector) * 2048;
    std::vector<uint8_t>& sec = img;
    make_tag(sec, base + off, TAG_FID, 0);
    put_le16(sec, base + off + 16, 1);   // file version
    sec[base + off + 18] = chars;        // file characteristics
    const size_t name_bytes = name.empty() ? 0 : 1 + name.size();
    sec[base + off + 19] = static_cast<uint8_t>(name_bytes);
    put_le32(sec, base + off + 20, icb_len);  // ICB extent length
    put_le32(sec, base + off + 24, icb_loc);  // ICB location
    put_le16(sec, base + off + 28, 0);        // ICB partition ref
    put_le16(sec, base + off + 36, 0);        // implementation use length
    if (!name.empty()) {
        sec[base + off + 38] = 0;        // compression ID 0 (8-bit CS0)
        std::memcpy(sec.data() + base + off + 39, name.data(), name.size());
    }
    size_t total = 38 + name_bytes;
    return off + ((total + 3) & ~3u);
}

// ── Image layouts ───────────────────────────────────────────────────
//
// Head tree (both images use the same FSD-relative block numbers so the
// genisoimage layout can be shared; the standard image just adds valid
// LVD/PD/FSD pointers and long_ad descriptors):
//   FSD@257, root FE@259 (loc 2), root data@260 (loc 3),
//   sub dir FE@261 (loc 4), sub data@262 (loc 5),
//   file FE@263 (loc 6), file data@264 (loc 7)

constexpr int64_t kSector = 2048;
constexpr int64_t kFsd = 257, kRootFe = 259, kRootData = 260;
constexpr int64_t kSubFe = 261, kSubData = 262;
constexpr int64_t kFileFe = 263, kFileData = 264;

// Shared VDS/tree skeleton.  `fsd_loc` is the LVD FSD pointer
// (0 mimics genisoimage), `pd_start` the PD partition start (junk in
// genisoimage images), `long_ad` selects 16-byte descriptors.
std::vector<uint8_t> build_udf_image(uint32_t fsd_loc, uint32_t pd_start,
                                     bool long_ad) {
    std::vector<uint8_t> img(static_cast<size_t>(kFileData + 1) * kSector, 0);
    auto sec = [&](int64_t s) { return img.data() + s * kSector; };

    // ── Volume Descriptor Sequence (sectors 32..37) ──
    // PVD at 32 (volume id d-string at 24)
    make_tag(img, 32 * kSector, TAG_PVD, 1);
    std::memcpy(sec(32) + 24, "GENISO-TEST", 11);

    // PD at 34 (partition start at 192, length at 196)
    make_tag(img, 34 * kSector, TAG_PD, 3);
    put_le32(img, 34 * kSector + 192, pd_start);
    put_le32(img, 34 * kSector + 196, 1700000000);

    // LVD at 35: logical volume contents use long_ad at 248
    // (extent len 2048, location = FSD pointer), map table length at 376
    make_tag(img, 35 * kSector, TAG_LVD, 4);
    put_le32(img, 35 * kSector + 248, 2048);   // long_ad extent length
    put_le32(img, 35 * kSector + 252, fsd_loc);  // long_ad location
    put_le32(img, 35 * kSector + 376, 0);      // map table length

    // TD at 37
    make_tag(img, 37 * kSector, TAG_TD, 7);

    // ── Anchor at 256 ──
    make_tag(img, 256 * kSector, TAG_AVDP, 256);
    put_le32(img, 256 * kSector + 16, 32768);  // main VDS extent length
    put_le32(img, 256 * kSector + 20, 32);     // main VDS location

    // ── FSD at 257 (root ICB left as garbage: all zeros) ──
    make_tag(img, kFsd * kSector, TAG_FSD, 0);

    // TD at 258
    make_tag(img, 258 * kSector, TAG_TD, 0);

    // Root dir FE at 259 (loc 2): data at 260 (loc 3), 88 bytes
    {
        std::vector<uint8_t> alloc;
        if (long_ad) {
            alloc.resize(16, 0);
            put_le32(alloc, 0, 88);
            put_le32(alloc, 4, 3);
            put_le16(alloc, 8, 0);
        } else {
            alloc.resize(8, 0);
            put_le32(alloc, 0, 88);
            put_le32(alloc, 4, 3);
        }
        put_fe(img, kRootFe, 2, 4, 88, alloc);
    }

    // Root data at 260: parent FID + "sub" directory (ICB loc 4)
    {
        size_t off = 0;
        off = put_fid(img, kRootData, off, 0x0A, "", 2);
        put_fid(img, kRootData, off, 0x02, "sub", 4);
    }

    // Sub dir FE at 261 (loc 4): data at 262 (loc 5), 88 bytes
    {
        std::vector<uint8_t> alloc;
        if (long_ad) {
            alloc.resize(16, 0);
            put_le32(alloc, 0, 88);
            put_le32(alloc, 4, 5);
            put_le16(alloc, 8, 0);
        } else {
            alloc.resize(8, 0);
            put_le32(alloc, 0, 88);
            put_le32(alloc, 4, 5);
        }
        put_fe(img, kSubFe, 4, 4, 88, alloc);
    }

    // Sub data at 262: parent FID + "hello.txt" file (ICB loc 6)
    {
        size_t off = 0;
        off = put_fid(img, kSubData, off, 0x0A, "", 4);
        put_fid(img, kSubData, off, 0x00, "hello.txt", 6, 16);
    }

    // File FE at 263 (loc 6): data at 264 (loc 7), 16 bytes
    {
        std::vector<uint8_t> alloc;
        if (long_ad) {
            alloc.resize(16, 0);
            put_le32(alloc, 0, 16);
            put_le32(alloc, 4, 7);
            put_le16(alloc, 8, 0);
        } else {
            alloc.resize(8, 0);
            put_le32(alloc, 0, 16);
            put_le32(alloc, 4, 7);
        }
        put_fe(img, kFileFe, 6, 5, 16, alloc);
    }

    // File data at 264
    std::memcpy(sec(kFileData), "GENISOIMAGE-TEST!", 17);

    return img;
}

void write_image(const std::filesystem::path& path,
                 const std::vector<uint8_t>& data) {
    std::ofstream f(path, std::ios::binary);
    f.write(reinterpret_cast<const char*>(data.data()),
            static_cast<std::streamsize>(data.size()));
}

} // namespace

// ── genisoimage-style image ─────────────────────────────────────────

TEST(UdfParserTest, GenisoimageStyleShortAdImage) {
    // LVD FSD pointer 0, junk PD start, short_ad descriptors.
    auto img = build_udf_image(0, 2247471, /*long_ad=*/false);
    auto path = std::filesystem::temp_directory_path() / "offcat_udf_geniso.iso";
    write_image(path, img);

    {
        UdfParser parser(path.string());
        ASSERT_TRUE(parser.open());

        std::vector<UdfEntry> root;
        ASSERT_TRUE(parser.read_root_directory(root));
        ASSERT_EQ(root.size(), 1u);
        EXPECT_EQ(root[0].name, "sub");
        EXPECT_TRUE(root[0].is_directory);

        // Recurse into the sub directory.
        UdfLongAd sub_icb;
        sub_icb.extent_length = 2048;
        sub_icb.location = root[0].extent_location;
        sub_icb.partition_ref = root[0].partition_ref;
        std::vector<UdfEntry> sub;
        ASSERT_TRUE(parser.read_directory(sub_icb, sub, 1));
        ASSERT_EQ(sub.size(), 1u);
        EXPECT_EQ(sub[0].name, "hello.txt");
        EXPECT_FALSE(sub[0].is_directory);

        // The file FE uses short_ad and is 16 bytes (below the old
        // 176-byte threshold) — both must work.
        int64_t extent = 0, size = 0, mtime = 0;
        bool is_dir = false;
        std::vector<UdfLongAd> ads;
        ASSERT_TRUE(parser.read_file_entry(sub[0].extent_location,
                                           sub[0].partition_ref,
                                           extent, size, is_dir, mtime, ads));
        EXPECT_FALSE(is_dir);
        EXPECT_EQ(size, 16);
        EXPECT_EQ(extent, 7);
        ASSERT_EQ(ads.size(), 1u);
        EXPECT_EQ(ads[0].location, 7u);

        // File data must be readable at the resolved sector.
        uint8_t buf[kSector];
        ASSERT_TRUE(parser.read_sector(parser.partition_to_absolute(0, extent),
                                       buf, 1));
        EXPECT_EQ(std::memcmp(buf, "GENISOIMAGE-TEST!", 17), 0);
    }

    std::filesystem::remove(path);
}

// ── standard image (valid pointers, long_ad) ────────────────────────

TEST(UdfParserTest, StandardLongAdImage) {
    // Valid FSD pointer (FSD is 2 blocks into partition 100) and a
    // sensible PD start; 16-byte long_ad descriptors.
    auto img = build_udf_image(2, 100, /*long_ad=*/true);
    auto path = std::filesystem::temp_directory_path() / "offcat_udf_std.iso";
    write_image(path, img);

    {
        UdfParser parser(path.string());
        ASSERT_TRUE(parser.open());

        std::vector<UdfEntry> root;
        ASSERT_TRUE(parser.read_root_directory(root));
        ASSERT_EQ(root.size(), 1u);
        EXPECT_EQ(root[0].name, "sub");
        EXPECT_TRUE(root[0].is_directory);

        // Long_ad file entry still parses.
        int64_t extent = 0, size = 0, mtime = 0;
        bool is_dir = false;
        std::vector<UdfLongAd> ads;
        ASSERT_TRUE(parser.read_file_entry(6, 0, extent, size, is_dir,
                                           mtime, ads));
        EXPECT_EQ(size, 16);
        EXPECT_EQ(extent, 7);
        EXPECT_EQ(ads.size(), 1u);
        EXPECT_EQ(ads[0].partition_ref, 0u);
    }

    std::filesystem::remove(path);
}

// ── broken FSD pointer but head tree intact ─────────────────────────

TEST(UdfParserTest, BogusFsdPointerFallsBackToHead) {
    // The LVD claims the FSD is 500 blocks into the partition, which
    // points past the end of the image; the real tree is in the head.
    auto img = build_udf_image(500, 100, /*long_ad=*/false);
    auto path = std::filesystem::temp_directory_path() / "offcat_udf_fsd.iso";
    write_image(path, img);

    {
        UdfParser parser(path.string());
        ASSERT_TRUE(parser.open());

        std::vector<UdfEntry> root;
        ASSERT_TRUE(parser.read_root_directory(root));
        ASSERT_EQ(root.size(), 1u);
        EXPECT_EQ(root[0].name, "sub");
    }

    std::filesystem::remove(path);
}

// ── wrapped root (root holds fewer entries than its children) ───────
//
// Mirrors the real-world "collection folder" images: the volume root
// directory contains a single entry (the collection folder) whose
// children hold many more entries than the root itself.  Picking the
// candidate with the most entries would select the collection folder's
// contents as the root, losing the wrapper level.

TEST(UdfParserTest, WrappedRootPicksFirstResolvableDirectory) {
    constexpr int64_t kFsd = 257, kRootFe = 259, kRootData = 260;
    constexpr int64_t kColFe = 261, kColData = 262;
    constexpr int64_t kSubAFe = 263, kSubAData = 264;

    std::vector<uint8_t> img(static_cast<size_t>(kSubAData + 1) * kSector, 0);

    // VDS + anchor + FSD + TD skeleton (same as build_udf_image).
    make_tag(img, 35 * kSector, TAG_LVD, 4);
    put_le32(img, 35 * kSector + 248, 2048);
    put_le32(img, 35 * kSector + 252, 0);   // FSD pointer 0 (genisoimage)
    make_tag(img, 37 * kSector, TAG_TD, 7);
    make_tag(img, 256 * kSector, TAG_AVDP, 256);
    put_le32(img, 256 * kSector + 16, 32768);
    put_le32(img, 256 * kSector + 20, 32);
    make_tag(img, kFsd * kSector, TAG_FSD, 0);
    make_tag(img, 258 * kSector, TAG_TD, 0);

    // Root dir FE@259 (loc 2): data@260 holds "collection" only.
    {
        std::vector<uint8_t> alloc(8, 0);
        put_le32(alloc, 0, 88);
        put_le32(alloc, 4, 3);
        put_fe(img, kRootFe, 2, 4, 88, alloc);
        size_t off = 0;
        off = put_fid(img, kRootData, off, 0x0A, "", 2);
        put_fid(img, kRootData, off, 0x02, "collection", 4);
    }

    // collection dir FE@261 (loc 4): data@262 holds two sub dirs.
    {
        std::vector<uint8_t> alloc(8, 0);
        put_le32(alloc, 0, 88);
        put_le32(alloc, 4, 5);
        put_fe(img, kColFe, 4, 4, 88, alloc);
        size_t off = 0;
        off = put_fid(img, kColData, off, 0x0A, "", 4);
        off = put_fid(img, kColData, off, 0x02, "subA", 6);
        put_fid(img, kColData, off, 0x02, "subB", 8);
    }

    // subA dir FE@263 (loc 6): data@264 holds three files — more
    // entries than the root, which used to defeat the "most entries"
    // heuristic.
    {
        std::vector<uint8_t> alloc(8, 0);
        put_le32(alloc, 0, 88);
        put_le32(alloc, 4, 7);
        put_fe(img, kSubAFe, 6, 4, 88, alloc);
        size_t off = 0;
        off = put_fid(img, kSubAData, off, 0x0A, "", 6);
        off = put_fid(img, kSubAData, off, 0x00, "f1.txt", 0, 16);
        off = put_fid(img, kSubAData, off, 0x00, "f2.txt", 0, 16);
        put_fid(img, kSubAData, off, 0x00, "f3.txt", 0, 16);
    }

    auto path =
        std::filesystem::temp_directory_path() / "offcat_udf_wrapped.iso";
    write_image(path, img);

    {
        UdfParser parser(path.string());
        ASSERT_TRUE(parser.open());

        std::vector<UdfEntry> root;
        ASSERT_TRUE(parser.read_root_directory(root));
        ASSERT_EQ(root.size(), 1u);
        EXPECT_EQ(root[0].name, "collection");
        EXPECT_TRUE(root[0].is_directory);

        // The wrapper level must stay reachable: its children are the
        // two sub directories, not subA's three files.
        UdfLongAd col_icb;
        col_icb.extent_length = 2048;
        col_icb.location = root[0].extent_location;
        col_icb.partition_ref = root[0].partition_ref;
        std::vector<UdfEntry> col;
        ASSERT_TRUE(parser.read_directory(col_icb, col, 1));
        ASSERT_EQ(col.size(), 2u);
        EXPECT_EQ(col[0].name, "subA");
        EXPECT_EQ(col[1].name, "subB");
    }

    std::filesystem::remove(path);
}

// ── junk FE before the real root is skipped ─────────────────────────
//
// Some genisoimage images start with a leftover directory FE whose
// child references point nowhere (garbage ICBs, e.g. Rock Ridge stub
// entries).  The head-tree scan must skip it and land on the real root.

TEST(UdfParserTest, JunkHeadDirectoryIsSkipped) {
    constexpr int64_t kFsd = 257, kJunkFe = 259, kJunkData = 260;
    constexpr int64_t kRootFe = 261, kRootData = 262;
    constexpr int64_t kRealFe = 263, kRealData = 264;

    std::vector<uint8_t> img(static_cast<size_t>(kRealData + 1) * kSector, 0);

    make_tag(img, 35 * kSector, TAG_LVD, 4);
    put_le32(img, 35 * kSector + 248, 2048);
    put_le32(img, 35 * kSector + 252, 0);
    make_tag(img, 37 * kSector, TAG_TD, 7);
    make_tag(img, 256 * kSector, TAG_AVDP, 256);
    put_le32(img, 256 * kSector + 16, 32768);
    put_le32(img, 256 * kSector + 20, 32);
    make_tag(img, kFsd * kSector, TAG_FSD, 0);
    make_tag(img, 258 * kSector, TAG_TD, 0);

    // Junk dir FE@259 (loc 2): claims a child "junkdir" whose ICB
    // points far past the end of the image (garbage location).
    {
        std::vector<uint8_t> alloc(8, 0);
        put_le32(alloc, 0, 88);
        put_le32(alloc, 4, 3);
        put_fe(img, kJunkFe, 2, 4, 88, alloc);
        size_t off = 0;
        off = put_fid(img, kJunkData, off, 0x0A, "", 2);
        put_fid(img, kJunkData, off, 0x02, "junkdir", 400);
    }

    // Real root FE@261 (loc 4): data@262 holds "real".
    {
        std::vector<uint8_t> alloc(8, 0);
        put_le32(alloc, 0, 88);
        put_le32(alloc, 4, 5);
        put_fe(img, kRootFe, 4, 4, 88, alloc);
        size_t off = 0;
        off = put_fid(img, kRootData, off, 0x0A, "", 4);
        put_fid(img, kRootData, off, 0x02, "real", 6);
    }

    // real dir FE@263 (loc 6): data@264 holds "hello.txt".
    {
        std::vector<uint8_t> alloc(8, 0);
        put_le32(alloc, 0, 88);
        put_le32(alloc, 4, 7);
        put_fe(img, kRealFe, 6, 4, 88, alloc);
        size_t off = 0;
        off = put_fid(img, kRealData, off, 0x0A, "", 6);
        put_fid(img, kRealData, off, 0x00, "hello.txt", 0, 16);
    }

    auto path =
        std::filesystem::temp_directory_path() / "offcat_udf_junk.iso";
    write_image(path, img);

    {
        UdfParser parser(path.string());
        ASSERT_TRUE(parser.open());

        std::vector<UdfEntry> root;
        ASSERT_TRUE(parser.read_root_directory(root));
        ASSERT_EQ(root.size(), 1u);
        EXPECT_EQ(root[0].name, "real");
        EXPECT_TRUE(root[0].is_directory);
    }

    std::filesystem::remove(path);
}
