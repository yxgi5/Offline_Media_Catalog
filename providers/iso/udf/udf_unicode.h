#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace offcat {

// Result of decoding a UDF d-string / OSTA Compressed Unicode name
struct DecodedName {
    std::string utf8;              // Decoded name in UTF-8
    std::string encoding;          // "udf-cs0", "compatibility", "heuristic"
    int confidence = 0;            // 100 = strict, lower = recovered
};

// Decode OSTA Compressed Unicode per UDF/OSTA spec (section 21)
// Handles: Compression ID, Character length, CS0 8-bit, UTF-16BE,
// surrogate pairs, invalid sequences.
DecodedName decode_udf_name(const uint8_t* data, size_t length);

// Decode a UDF d-string: bytes until terminator (0x00), then
// passed through Compressed Unicode decoding.
DecodedName decode_udf_dstring(const uint8_t* data, size_t length);

// Low-level: decode CS0 16-bit (UTF-16BE) with surrogate pair handling.
// Returns false if the byte stream is invalid.
bool decode_utf16be_to_utf8(const uint8_t* data, size_t byte_length,
                            std::string& out);

// Validate a UTF-8 byte sequence (strict).
bool is_valid_utf8(const uint8_t* data, size_t length);

// Compatibility recovery: best-effort conversion when the name is not
// valid Compressed Unicode (e.g., raw ASCII bytes or mojibake patterns).
DecodedName heuristic_decode_name(const uint8_t* data, size_t length);

} // namespace offcat
