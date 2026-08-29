#include "core/checksum.h"
#include <gtest/gtest.h>
#include <string>
#include <vector>

using namespace offcat;

// ── CRC32 known vectors ─────────────────────────────────────────────

TEST(CRC32Test, EmptyInput) {
    EXPECT_EQ(CRC32Checksum::compute(nullptr, 0), 0x00000000u);
}

TEST(CRC32Test, CheckValue123456789) {
    const char* data = "123456789";
    EXPECT_EQ(CRC32Checksum::compute(
        reinterpret_cast<const uint8_t*>(data), 9), 0xCBF43926u);
}

TEST(CRC32Test, HelloWorld) {
    const char* data = "Hello, World!";
    // Verified with zlib crc32
    EXPECT_EQ(CRC32Checksum::compute(
        reinterpret_cast<const uint8_t*>(data), 13), 0xEC4AC3D0u);
}

TEST(CRC32Test, StreamingMatchesOneShot) {
    std::vector<uint8_t> data(10000);
    for (size_t i = 0; i < data.size(); i++) {
        data[i] = static_cast<uint8_t>(i * 31);
    }

    uint32_t one_shot = CRC32Checksum::compute(data.data(), data.size());

    CRC32Checksum crc;
    for (size_t i = 0; i < data.size(); i += 997) {
        size_t chunk = std::min<size_t>(997, data.size() - i);
        crc.update(data.data() + i, chunk);
    }
    auto digest = crc.finalize();
    uint32_t streamed = (static_cast<uint32_t>(digest[0]) << 24) |
                        (static_cast<uint32_t>(digest[1]) << 16) |
                        (static_cast<uint32_t>(digest[2]) << 8) |
                        static_cast<uint32_t>(digest[3]);
    EXPECT_EQ(one_shot, streamed);
}

TEST(CRC32Test, DigestSize) {
    CRC32Checksum crc;
    EXPECT_EQ(crc.digest_size(), 4u);
    crc.update(reinterpret_cast<const uint8_t*>("abc"), 3);
    auto digest = crc.finalize();
    EXPECT_EQ(digest.size(), 4u);
}

// ── SHA-256 known vectors ───────────────────────────────────────────

TEST(SHA256Test, EmptyInput) {
    auto digest = SHA256Checksum::compute(nullptr, 0);
    EXPECT_EQ(digest_to_hex(digest),
        "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
}

TEST(SHA256Test, CheckValueABC) {
    auto digest = SHA256Checksum::compute(
        reinterpret_cast<const uint8_t*>("abc"), 3);
    EXPECT_EQ(digest_to_hex(digest),
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}

TEST(SHA256Test, CheckValue123456789) {
    auto digest = SHA256Checksum::compute(
        reinterpret_cast<const uint8_t*>("123456789"), 9);
    EXPECT_EQ(digest_to_hex(digest),
        "15e2b0d3c33891ebb0f1ef609ec419420c20e320ce94c65fbc8c3312448eb225");
}

TEST(SHA256Test, LongInput) {
    // 1,000,000 'a' characters (known NIST vector)
    std::vector<uint8_t> data(1000000, 'a');
    auto digest = SHA256Checksum::compute(data.data(), data.size());
    EXPECT_EQ(digest_to_hex(digest),
        "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

// ── MD5 known vectors ───────────────────────────────────────────────

TEST(MD5Test, EmptyInput) {
    auto digest = MD5Checksum::compute(nullptr, 0);
    EXPECT_EQ(digest_to_hex(digest), "d41d8cd98f00b204e9800998ecf8427e");
}

TEST(MD5Test, CheckValueABC) {
    auto digest = MD5Checksum::compute(
        reinterpret_cast<const uint8_t*>("abc"), 3);
    EXPECT_EQ(digest_to_hex(digest), "900150983cd24fb0d6963f7d28e17f72");
}

TEST(MD5Test, CheckValue123456789) {
    auto digest = MD5Checksum::compute(
        reinterpret_cast<const uint8_t*>("123456789"), 9);
    EXPECT_EQ(digest_to_hex(digest), "25f9e794323b453885f5181f1b624d0b");
}

// ── Factory ─────────────────────────────────────────────────────────

TEST(ChecksumFactoryTest, CreateAlgorithms) {
    EXPECT_NE(create_checksum_engine("sha256"), nullptr);
    EXPECT_NE(create_checksum_engine("md5"), nullptr);
    EXPECT_NE(create_checksum_engine("crc32"), nullptr);
    EXPECT_EQ(create_checksum_engine("unknown"), nullptr);
}

TEST(ChecksumFactoryTest, Names) {
    EXPECT_EQ(create_checksum_engine("sha256")->name(), "sha256");
    EXPECT_EQ(create_checksum_engine("md5")->name(), "md5");
    EXPECT_EQ(create_checksum_engine("crc32")->name(), "crc32");
}
