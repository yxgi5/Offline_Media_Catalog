#include "iso/udf/udf_unicode.h"
#include <gtest/gtest.h>
#include <string>
#include <vector>

using namespace offcat;

// ── UTF-8 validation ────────────────────────────────────────────────

TEST(Utf8ValidationTest, ValidSequences) {
    const uint8_t ascii[] = "hello";
    EXPECT_TRUE(is_valid_utf8(ascii, 5));

    const uint8_t chinese[] = {0xE4, 0xB8, 0xAD};  // 中
    EXPECT_TRUE(is_valid_utf8(chinese, 3));

    const uint8_t emoji[] = {0xF0, 0x9F, 0x98, 0x80};  // 😀
    EXPECT_TRUE(is_valid_utf8(emoji, 4));

    const uint8_t mixed[] = {0xE6, 0x97, 0xA5, 0xE6, 0x9C, 0xAC, 0xE8, 0xAA, 0x9E};
    EXPECT_TRUE(is_valid_utf8(mixed, 9));  // 日本語
}

TEST(Utf8ValidationTest, InvalidSequences) {
    const uint8_t bad_continuation[] = {0xE4, 0x28, 0xA1};  // invalid
    EXPECT_FALSE(is_valid_utf8(bad_continuation, 3));

    const uint8_t overlong[] = {0xC0, 0xAF};  // overlong 2-byte
    EXPECT_FALSE(is_valid_utf8(overlong, 2));

    const uint8_t surrogate[] = {0xED, 0xA0, 0x80};  // surrogate U+D800
    EXPECT_FALSE(is_valid_utf8(surrogate, 3));

    const uint8_t too_high[] = {0xF4, 0x90, 0x80, 0x80};  // > U+10FFFF
    EXPECT_FALSE(is_valid_utf8(too_high, 4));

    const uint8_t truncated[] = {0xE4, 0xB8};  // truncated 3-byte
    EXPECT_FALSE(is_valid_utf8(truncated, 2));
}

// ── UTF-16BE → UTF-8 ────────────────────────────────────────────────

TEST(Utf16BeTest, BasicLatin) {
    // "Hello" in UTF-16BE
    const uint8_t data[] = {0x00, 0x48, 0x00, 0x65, 0x00, 0x6C, 0x00, 0x6C, 0x00, 0x6F};
    std::string out;
    EXPECT_TRUE(decode_utf16be_to_utf8(data, sizeof(data), out));
    EXPECT_EQ(out, "Hello");
}

TEST(Utf16BeTest, ChineseCharacters) {
    // 中文 in UTF-16BE
    const uint8_t data[] = {0x4E, 0x2D, 0x65, 0x87};
    std::string out;
    EXPECT_TRUE(decode_utf16be_to_utf8(data, sizeof(data), out));
    EXPECT_EQ(out, "中文");
}

TEST(Utf16BeTest, SurrogatePairs) {
    // 😀 (U+1F600) as surrogate pair D83D DE00 in UTF-16BE
    const uint8_t data[] = {0xD8, 0x3D, 0xDE, 0x00};
    std::string out;
    EXPECT_TRUE(decode_utf16be_to_utf8(data, sizeof(data), out));
    EXPECT_EQ(out, "😀");
}

TEST(Utf16BeTest, LoneSurrogateRejected) {
    // Lone high surrogate (invalid)
    const uint8_t data[] = {0xD8, 0x3D, 0x00, 0x41};
    std::string out;
    EXPECT_FALSE(decode_utf16be_to_utf8(data, sizeof(data), out));
}

TEST(Utf16BeTest, OddLengthRejected) {
    const uint8_t data[] = {0x00, 0x41, 0x00};
    std::string out;
    EXPECT_FALSE(decode_utf16be_to_utf8(data, sizeof(data), out));
}

// ── OSTA Compressed Unicode ─────────────────────────────────────────

TEST(UdfNameTest, CompressionId16_Utf16) {
    // Compression ID 16, 3 chars, "abc" in UTF-16BE
    const uint8_t data[] = {16, 3, 0x00, 0x61, 0x00, 0x62, 0x00, 0x63};
    auto result = decode_udf_name(data, sizeof(data));
    EXPECT_EQ(result.utf8, "abc");
    EXPECT_EQ(result.encoding, "udf-cs0");
    EXPECT_EQ(result.confidence, 100);
}

TEST(UdfNameTest, CompressionId16_Chinese) {
    // Compression ID 16, 2 chars, 中文
    const uint8_t data[] = {16, 2, 0x4E, 0x2D, 0x65, 0x87};
    auto result = decode_udf_name(data, sizeof(data));
    EXPECT_EQ(result.utf8, "中文");
    EXPECT_EQ(result.encoding, "udf-cs0");
}

TEST(UdfNameTest, CompressionId16_Emoji) {
    // Compression ID 16, 2 code units, 😀
    const uint8_t data[] = {16, 2, 0xD8, 0x3D, 0xDE, 0x00};
    auto result = decode_udf_name(data, sizeof(data));
    EXPECT_EQ(result.utf8, "😀");
}

TEST(UdfNameTest, CompressionId8_Latin1) {
    // Compression ID 8, char count 4, "café" (é = 0xE9 in Latin-1)
    const uint8_t data[] = {8, 4, 'c', 'a', 'f', 0xE9};
    auto result = decode_udf_name(data, sizeof(data));
    EXPECT_EQ(result.utf8, "café");
    EXPECT_EQ(result.encoding, "udf-cs0");
}

TEST(UdfNameTest, CompressionId8_Ascii) {
    const uint8_t data[] = {8, 5, 'h', 'e', 'l', 'l', 'o'};
    auto result = decode_udf_name(data, sizeof(data));
    EXPECT_EQ(result.utf8, "hello");
}

TEST(UdfNameTest, CompressionIdBelow8) {
    // Compression ID 1 (old spec), raw bytes
    const uint8_t data[] = {1, 't', 'e', 's', 't'};
    auto result = decode_udf_name(data, sizeof(data));
    EXPECT_EQ(result.utf8, "test");
    EXPECT_EQ(result.confidence, 70);
}

// genisoimage writes compression IDs 8/16 without the UDF character
// count field (characters start right after the ID).  The first
// character byte must not be mistaken for a count.
TEST(UdfNameTest, GenisoimageId8NoCount) {
    // {8, 't','o','r','r','e','n','t'} -> "torrent" (not "orrent")
    const uint8_t data[] = {8, 't', 'o', 'r', 'r', 'e', 'n', 't'};
    auto result = decode_udf_name(data, sizeof(data));
    EXPECT_EQ(result.utf8, "torrent");
}

TEST(UdfNameTest, GenisoimageId8NoCountDigitStart) {
    // {8, '0','8','0','3','.','r','a','r'} -> "0803.rar" (not "803.rar")
    const uint8_t data[] = {8, '0', '8', '0', '3', '.', 'r', 'a', 'r'};
    auto result = decode_udf_name(data, sizeof(data));
    EXPECT_EQ(result.utf8, "0803.rar");
}

TEST(UdfNameTest, GenisoimageId16NoCountAscii) {
    // {16, 0x00,'K', 0x00,'U'} -> "KU" (0x00 is the first UTF-16BE
    // high byte, not a count of zero)
    const uint8_t data[] = {16, 0x00, 'K', 0x00, 'U'};
    auto result = decode_udf_name(data, sizeof(data));
    EXPECT_EQ(result.utf8, "KU");
}

TEST(UdfNameTest, GenisoimageId16NoCountChinese) {
    // {16, 0x4E,0x2D, 0x65,0x87} -> "中文" (0x4E is the first UTF-16BE
    // high byte, not a count of 78)
    const uint8_t data[] = {16, 0x4E, 0x2D, 0x65, 0x87};
    auto result = decode_udf_name(data, sizeof(data));
    EXPECT_EQ(result.utf8, "中文");
}

TEST(UdfNameTest, EmptyInput) {
    auto result = decode_udf_name(nullptr, 0);
    EXPECT_TRUE(result.utf8.empty());
    EXPECT_EQ(result.confidence, 0);
}

// ── Heuristic recovery ──────────────────────────────────────────────

TEST(HeuristicTest, RawUtf8PassThrough) {
    // Already valid UTF-8
    const uint8_t data[] = {0xE6, 0x97, 0xA5, 0xE6, 0x9C, 0xAC};  // 日本
    auto result = heuristic_decode_name(data, sizeof(data));
    EXPECT_EQ(result.utf8, "日本");
    EXPECT_EQ(result.confidence, 80);
}

TEST(HeuristicTest, Latin1Mojibake) {
    // Bytes that are not valid UTF-8: 0xE9 0xE8 (Latin-1 é è)
    const uint8_t data[] = {0xE9, 0xE8};
    auto result = heuristic_decode_name(data, sizeof(data));
    EXPECT_EQ(result.utf8, "éè");
    EXPECT_EQ(result.confidence, 40);
}

// ── D-string ────────────────────────────────────────────────────────

TEST(UdfDStringTest, TerminatedString) {
    // "NSR02" followed by terminator then padding
    const uint8_t data[] = {'N', 'S', 'R', '0', '2', 0x00, 0x00, 0x00};
    auto result = decode_udf_dstring(data, sizeof(data));
    EXPECT_EQ(result.utf8, "NSR02");
}

TEST(UdfDStringTest, CompressedUnicodeTerminated) {
    // "中文" in CS0 UTF-16BE with 0x00 terminator
    const uint8_t data[] = {16, 2, 0x4E, 0x2D, 0x65, 0x87, 0x00};
    auto result = decode_udf_dstring(data, sizeof(data));
    EXPECT_EQ(result.utf8, "中文");
}
