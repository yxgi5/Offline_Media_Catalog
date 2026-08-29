#include "core/checksum.h"
#include <cstring>
#include <sstream>
#include <iomanip>
#include <algorithm>

namespace offcat {

// ── CRC32 ───────────────────────────────────────────────────────────

// CRC32 (ISO 3309, reflected polynomial 0xEDB88320). The lookup table is
// generated lazily on first use instead of being written out, which
// avoids transcription errors in a hand-written 256-entry table.

uint32_t CRC32Checksum::table_[256];

CRC32Checksum::CRC32Checksum() {
    static const bool table_ready = []() {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) {
                c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            }
            CRC32Checksum::table_[i] = c;
        }
        return true;
    }();
    (void)table_ready;
    reset();
}

void CRC32Checksum::reset() {
    state_ = 0xFFFFFFFF;
}

void CRC32Checksum::update(const uint8_t* data, size_t length) {
    uint32_t crc = state_;
    for (size_t i = 0; i < length; i++) {
        crc = table_[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    }
    state_ = crc;
}

std::vector<uint8_t> CRC32Checksum::finalize() {
    uint32_t result = state_ ^ 0xFFFFFFFF;
    std::vector<uint8_t> digest(4);
    // Store as big-endian
    digest[0] = static_cast<uint8_t>((result >> 24) & 0xFF);
    digest[1] = static_cast<uint8_t>((result >> 16) & 0xFF);
    digest[2] = static_cast<uint8_t>((result >> 8) & 0xFF);
    digest[3] = static_cast<uint8_t>(result & 0xFF);
    return digest;
}

uint32_t CRC32Checksum::compute(const uint8_t* data, size_t length) {
    CRC32Checksum crc;
    crc.update(data, length);
    auto digest = crc.finalize();
    return (static_cast<uint32_t>(digest[0]) << 24) |
           (static_cast<uint32_t>(digest[1]) << 16) |
           (static_cast<uint32_t>(digest[2]) << 8) |
           static_cast<uint32_t>(digest[3]);
}

// ── SHA-256 ─────────────────────────────────────────────────────────

static const uint32_t sha256_k[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5,
    0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
    0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
    0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
    0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5,
    0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

static inline uint32_t rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
static inline uint32_t ch(uint32_t x, uint32_t y, uint32_t z) { return (x & y) ^ (~x & z); }
static inline uint32_t maj(uint32_t x, uint32_t y, uint32_t z) { return (x & y) ^ (x & z) ^ (y & z); }
static inline uint32_t sigma0(uint32_t x) { return rotr(x, 2) ^ rotr(x, 13) ^ rotr(x, 22); }
static inline uint32_t sigma1(uint32_t x) { return rotr(x, 6) ^ rotr(x, 11) ^ rotr(x, 25); }
static inline uint32_t gamma0(uint32_t x) { return rotr(x, 7) ^ rotr(x, 18) ^ (x >> 3); }
static inline uint32_t gamma1(uint32_t x) { return rotr(x, 17) ^ rotr(x, 19) ^ (x >> 10); }

SHA256Checksum::SHA256Checksum() { reset(); }

void SHA256Checksum::reset() {
    state_[0] = 0x6a09e667; state_[1] = 0xbb67ae85;
    state_[2] = 0x3c6ef372; state_[3] = 0xa54ff53a;
    state_[4] = 0x510e527f; state_[5] = 0x9b05688c;
    state_[6] = 0x1f83d9ab; state_[7] = 0x5be0cd19;
    total_length_ = 0;
    buffer_length_ = 0;
}

void SHA256Checksum::process_block(const uint8_t block[64]) {
    uint32_t w[64];
    for (int i = 0; i < 16; i++) {
        w[i] = (static_cast<uint32_t>(block[i*4]) << 24) |
               (static_cast<uint32_t>(block[i*4+1]) << 16) |
               (static_cast<uint32_t>(block[i*4+2]) << 8) |
               static_cast<uint32_t>(block[i*4+3]);
    }
    for (int i = 16; i < 64; i++) {
        w[i] = gamma1(w[i-2]) + w[i-7] + gamma0(w[i-15]) + w[i-16];
    }

    uint32_t a = state_[0], b = state_[1], c = state_[2], d = state_[3];
    uint32_t e = state_[4], f = state_[5], g = state_[6], h = state_[7];

    for (int i = 0; i < 64; i++) {
        uint32_t t1 = h + sigma1(e) + ch(e, f, g) + sha256_k[i] + w[i];
        uint32_t t2 = sigma0(a) + maj(a, b, c);
        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }

    state_[0] += a; state_[1] += b; state_[2] += c; state_[3] += d;
    state_[4] += e; state_[5] += f; state_[6] += g; state_[7] += h;
}

void SHA256Checksum::update(const uint8_t* data, size_t length) {
    total_length_ += length;
    size_t offset = 0;

    if (buffer_length_ > 0) {
        size_t copy_len = std::min(length, 64 - buffer_length_);
        std::memcpy(buffer_ + buffer_length_, data, copy_len);
        buffer_length_ += copy_len;
        offset += copy_len;
        if (buffer_length_ == 64) {
            process_block(buffer_);
            buffer_length_ = 0;
        }
    }

    while (offset + 64 <= length) {
        process_block(data + offset);
        offset += 64;
    }

    if (offset < length) {
        buffer_length_ = length - offset;
        std::memcpy(buffer_, data + offset, buffer_length_);
    }
}

std::vector<uint8_t> SHA256Checksum::finalize() {
    uint64_t total_bits = total_length_ * 8;

    // Padding
    uint8_t pad = 0x80;
    update(&pad, 1);
    pad = 0;
    while (buffer_length_ != 56) {
        update(&pad, 1);
    }

    // Length in big-endian
    uint8_t len_bytes[8];
    for (int i = 7; i >= 0; i--) {
        len_bytes[i] = static_cast<uint8_t>(total_bits & 0xFF);
        total_bits >>= 8;
    }
    update(len_bytes, 8);

    std::vector<uint8_t> digest(32);
    for (int i = 0; i < 8; i++) {
        digest[i*4]   = static_cast<uint8_t>((state_[i] >> 24) & 0xFF);
        digest[i*4+1] = static_cast<uint8_t>((state_[i] >> 16) & 0xFF);
        digest[i*4+2] = static_cast<uint8_t>((state_[i] >> 8) & 0xFF);
        digest[i*4+3] = static_cast<uint8_t>(state_[i] & 0xFF);
    }
    return digest;
}

std::vector<uint8_t> SHA256Checksum::compute(const uint8_t* data, size_t length) {
    SHA256Checksum sha;
    sha.update(data, length);
    return sha.finalize();
}

// ── MD5 ─────────────────────────────────────────────────────────────

static inline uint32_t md5_F(uint32_t x, uint32_t y, uint32_t z) { return (x & y) | (~x & z); }
static inline uint32_t md5_G(uint32_t x, uint32_t y, uint32_t z) { return (x & z) | (y & ~z); }
static inline uint32_t md5_H(uint32_t x, uint32_t y, uint32_t z) { return x ^ y ^ z; }
static inline uint32_t md5_I(uint32_t x, uint32_t y, uint32_t z) { return y ^ (x | ~z); }
static inline uint32_t md5_rotl(uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }

MD5Checksum::MD5Checksum() { reset(); }

void MD5Checksum::reset() {
    state_[0] = 0x67452301;
    state_[1] = 0xefcdab89;
    state_[2] = 0x98badcfe;
    state_[3] = 0x10325476;
    total_length_ = 0;
    buffer_length_ = 0;
}

void MD5Checksum::process_block(const uint8_t block[64]) {
    uint32_t M[16];
    for (int i = 0; i < 16; i++) {
        M[i] = static_cast<uint32_t>(block[i*4]) |
               (static_cast<uint32_t>(block[i*4+1]) << 8) |
               (static_cast<uint32_t>(block[i*4+2]) << 16) |
               (static_cast<uint32_t>(block[i*4+3]) << 24);
    }

    uint32_t A = state_[0], B = state_[1], C = state_[2], D = state_[3];

    static const uint32_t T[] = {
        0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee,
        0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
        0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be,
        0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
        0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa,
        0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
        0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed,
        0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
        0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c,
        0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
        0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05,
        0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
        0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039,
        0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
        0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1,
        0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391
    };

    static const int s[] = {
        7,12,17,22, 7,12,17,22, 7,12,17,22, 7,12,17,22,
        5, 9,14,20, 5, 9,14,20, 5, 9,14,20, 5, 9,14,20,
        4,11,16,23, 4,11,16,23, 4,11,16,23, 4,11,16,23,
        6,10,15,21, 6,10,15,21, 6,10,15,21, 6,10,15,21
    };

    for (int i = 0; i < 64; i++) {
        uint32_t F, g;
        if (i < 16) {
            F = md5_F(B, C, D);
            g = i;
        } else if (i < 32) {
            F = md5_G(B, C, D);
            g = (5*i + 1) % 16;
        } else if (i < 48) {
            F = md5_H(B, C, D);
            g = (3*i + 5) % 16;
        } else {
            F = md5_I(B, C, D);
            g = (7*i) % 16;
        }
        F = F + A + T[i] + M[g];
        A = D; D = C; C = B;
        B = B + md5_rotl(F, s[i]);
    }

    state_[0] += A; state_[1] += B; state_[2] += C; state_[3] += D;
}

void MD5Checksum::update(const uint8_t* data, size_t length) {
    total_length_ += length;
    size_t offset = 0;

    if (buffer_length_ > 0) {
        size_t copy_len = std::min(length, 64 - buffer_length_);
        std::memcpy(buffer_ + buffer_length_, data, copy_len);
        buffer_length_ += copy_len;
        offset += copy_len;
        if (buffer_length_ == 64) {
            process_block(buffer_);
            buffer_length_ = 0;
        }
    }

    while (offset + 64 <= length) {
        process_block(data + offset);
        offset += 64;
    }

    if (offset < length) {
        buffer_length_ = length - offset;
        std::memcpy(buffer_, data + offset, buffer_length_);
    }
}

std::vector<uint8_t> MD5Checksum::finalize() {
    uint64_t total_bits = total_length_ * 8;

    uint8_t pad = 0x80;
    update(&pad, 1);
    pad = 0;
    while (buffer_length_ != 56) {
        update(&pad, 1);
    }

    // Length in little-endian for MD5
    uint8_t len_bytes[8];
    for (int i = 0; i < 8; i++) {
        len_bytes[i] = static_cast<uint8_t>(total_bits & 0xFF);
        total_bits >>= 8;
    }
    update(len_bytes, 8);

    std::vector<uint8_t> digest(16);
    for (int i = 0; i < 4; i++) {
        digest[i*4]   = static_cast<uint8_t>(state_[i] & 0xFF);
        digest[i*4+1] = static_cast<uint8_t>((state_[i] >> 8) & 0xFF);
        digest[i*4+2] = static_cast<uint8_t>((state_[i] >> 16) & 0xFF);
        digest[i*4+3] = static_cast<uint8_t>((state_[i] >> 24) & 0xFF);
    }
    return digest;
}

std::vector<uint8_t> MD5Checksum::compute(const uint8_t* data, size_t length) {
    MD5Checksum md5;
    md5.update(data, length);
    return md5.finalize();
}

// ── Factory ─────────────────────────────────────────────────────────

std::unique_ptr<ChecksumEngine> create_checksum_engine(const std::string& algorithm) {
    if (algorithm == "sha256") return std::make_unique<SHA256Checksum>();
    if (algorithm == "md5")    return std::make_unique<MD5Checksum>();
    if (algorithm == "crc32")  return std::make_unique<CRC32Checksum>();
    return nullptr;
}

std::string digest_to_hex(const std::vector<uint8_t>& digest) {
    std::ostringstream oss;
    for (uint8_t b : digest) {
        oss << std::hex << std::setfill('0') << std::setw(2) << static_cast<int>(b);
    }
    return oss.str();
}

} // namespace offcat
