#include "udf_unicode.h"

namespace offcat {

// ── UTF-8 validation ────────────────────────────────────────────────

bool is_valid_utf8(const uint8_t* data, size_t length) {
    size_t i = 0;
    while (i < length) {
        uint8_t b = data[i];
        if (b < 0x80) {
            i++;
        } else if ((b & 0xE0) == 0xC0) {
            if (i + 1 >= length || (data[i+1] & 0xC0) != 0x80) return false;
            if (b < 0xC2) return false;  // overlong
            i += 2;
        } else if ((b & 0xF0) == 0xE0) {
            if (i + 2 >= length) return false;
            if ((data[i+1] & 0xC0) != 0x80 || (data[i+2] & 0xC0) != 0x80) return false;
            if (b == 0xE0 && data[i+1] < 0xA0) return false;  // overlong
            if (b == 0xED && data[i+1] >= 0xA0) return false;  // surrogate
            i += 3;
        } else if ((b & 0xF8) == 0xF0) {
            if (i + 3 >= length) return false;
            if ((data[i+1] & 0xC0) != 0x80 || (data[i+2] & 0xC0) != 0x80 ||
                (data[i+3] & 0xC0) != 0x80) return false;
            if (b == 0xF0 && data[i+1] < 0x90) return false;  // overlong
            if (b > 0xF4) return false;  // > U+10FFFF
            if (b == 0xF4 && data[i+1] >= 0x90) return false;
            i += 4;
        } else {
            return false;
        }
    }
    return true;
}

// ── UTF-16BE → UTF-8 ────────────────────────────────────────────────

static void append_utf8_codepoint(uint32_t cp, std::string& out) {
    if (cp < 0x80) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

bool decode_utf16be_to_utf8(const uint8_t* data, size_t byte_length,
                            std::string& out) {
    if (byte_length % 2 != 0) return false;

    out.clear();
    size_t units = byte_length / 2;
    for (size_t i = 0; i < units; i++) {
        uint16_t u = static_cast<uint16_t>((data[i*2] << 8) | data[i*2+1]);

        if (u >= 0xD800 && u <= 0xDBFF) {
            // High surrogate: need a following low surrogate
            if (i + 1 >= units) return false;
            uint16_t low = static_cast<uint16_t>(
                (data[(i+1)*2] << 8) | data[(i+1)*2+1]);
            if (low < 0xDC00 || low > 0xDFFF) return false;
            uint32_t cp = 0x10000 + ((static_cast<uint32_t>(u) - 0xD800) << 10) +
                          (low - 0xDC00);
            append_utf8_codepoint(cp, out);
            i++;
        } else if (u >= 0xDC00 && u <= 0xDFFF) {
            // Lone low surrogate: invalid in strict mode
            return false;
        } else {
            append_utf8_codepoint(u, out);
        }
    }
    return true;
}

// ── Compressed Unicode decoding ─────────────────────────────────────

DecodedName decode_udf_name(const uint8_t* data, size_t length) {
    DecodedName result;
    result.encoding = "udf-cs0";
    result.confidence = 100;

    if (length == 0) {
        result.encoding = "compatibility";
        result.confidence = 0;
        return result;
    }

    uint8_t compression_id = data[0];

    // Compression ID 16: CS0 with 16-bit characters (UTF-16BE)
    if (compression_id == 16) {
        // UDF spec: data[1] is the character count and the characters
        // follow at data[2].  genisoimage writes the same ID without
        // the count (characters start at data[1]); detect it because a
        // real count must be non-zero and fit in the remaining bytes.
        const bool spec_format =
            length >= 2 && data[1] > 0 &&
            static_cast<size_t>(data[1]) * 2 <= length - 2;
        size_t char_start = spec_format ? 2 : 1;
        size_t byte_length = 0;
        if (spec_format) {
            byte_length = static_cast<size_t>(data[1]) * 2;
        } else {
            byte_length = length - 1;
            if (byte_length % 2 != 0) byte_length--;  // UTF-16BE units
        }

        std::string utf8;
        if (decode_utf16be_to_utf8(data + char_start, byte_length, utf8)) {
            result.utf8 = utf8;
            return result;
        }

        // Fall through to compatibility parsing
        result.encoding = "compatibility";
        result.confidence = 60;

        // Try to salvage: decode code units individually, skipping
        // invalid surrogate halves
        std::string salvaged;
        size_t units = byte_length / 2;
        for (size_t i = 0; i < units; i++) {
            uint16_t u = static_cast<uint16_t>(data[char_start + i*2] << 8 |
                                               data[char_start + i*2 + 1]);
            if (u >= 0xD800 && u <= 0xDFFF) continue;  // skip lone surrogates
            append_utf8_codepoint(u, salvaged);
        }
        result.utf8 = salvaged;
        return result;
    }

    // Compression ID 8: CS0 with 8-bit characters (usually Latin-1)
    if (compression_id == 8) {
        // UDF spec: data[1] is the character count.  genisoimage omits
        // the count and starts characters at data[1]; a count that
        // cannot fit in the remaining bytes signals that form.
        const bool spec_format =
            length >= 2 && data[1] > 0 &&
            static_cast<size_t>(data[1]) <= length - 2;
        size_t char_start = spec_format ? 2 : 1;
        size_t byte_length = spec_format ? static_cast<size_t>(data[1])
                                         : length - 1;

        // 8-bit CS0: characters 0x20-0x7E are ASCII, 0x80+ typically
        // Latin-1 or extended ASCII
        std::string out;
        out.reserve(byte_length);
        for (size_t i = 0; i < byte_length; i++) {
            uint8_t c = data[char_start + i];
            if (c < 0x80) {
                out.push_back(static_cast<char>(c));
            } else {
                // Latin-1 → UTF-8
                append_utf8_codepoint(c, out);
            }
        }
        result.utf8 = out;
        return result;
    }

    // Compression IDs 0-7: single byte, 8-bit chars
    if (compression_id < 8) {
        size_t byte_length = length - 1;
        std::string out;
        out.reserve(byte_length);
        for (size_t i = 0; i < byte_length; i++) {
            uint8_t c = data[1 + i];
            if (c < 0x80) {
                out.push_back(static_cast<char>(c));
            } else {
                append_utf8_codepoint(c, out);
            }
        }
        result.encoding = "compatibility";
        result.confidence = 70;
        result.utf8 = out;
        return result;
    }

    // Unknown compression ID: heuristic recovery
    result = heuristic_decode_name(data + 1, length - 1);
    return result;
}

DecodedName decode_udf_dstring(const uint8_t* data, size_t length) {
    // d-string: compressed unicode terminated by 0x00
    size_t name_len = 0;
    while (name_len < length && data[name_len] != 0x00) {
        name_len++;
    }
    if (name_len == 0) {
        return decode_udf_name(data, length);  // No terminator found
    }
    // If the first byte is a valid compression ID (8, 16, or <8),
    // treat as compressed unicode; otherwise the data is a raw byte
    // string (common in real-world volume descriptors).
    if (name_len >= 2 &&
        (data[0] == 8 || data[0] == 16 || data[0] < 8)) {
        return decode_udf_name(data, name_len);
    }
    // Raw byte string terminated by 0x00
    DecodedName result;
    result.utf8.assign(reinterpret_cast<const char*>(data), name_len);
    result.encoding = "raw";
    result.confidence = 90;
    return result;
}

// ── Heuristic recovery ──────────────────────────────────────────────

DecodedName heuristic_decode_name(const uint8_t* data, size_t length) {
    DecodedName result;
    result.encoding = "heuristic";
    result.confidence = 40;

    if (length == 0) return result;

    // Case 1: Data is already valid UTF-8 → use as-is
    if (is_valid_utf8(data, length)) {
        result.utf8.assign(reinterpret_cast<const char*>(data), length);
        result.confidence = 80;
        return result;
    }

    // Case 2: Latin-1 mojibake (e.g., UTF-8 bytes interpreted as Latin-1)
    // Convert each byte to Unicode codepoint
    std::string out;
    out.reserve(length);
    for (size_t i = 0; i < length; i++) {
        append_utf8_codepoint(data[i], out);
    }
    result.utf8 = out;
    return result;
}

} // namespace offcat
