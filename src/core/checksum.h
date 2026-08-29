#pragma once

#include <cstdint>
#include <cstddef>
#include <vector>
#include <string>
#include <memory>

namespace offcat {

// ── Abstract checksum interface ─────────────────────────────────────

class ChecksumEngine {
public:
    virtual ~ChecksumEngine() = default;
    virtual std::string name() const = 0;
    virtual size_t digest_size() const = 0;
    virtual void update(const uint8_t* data, size_t length) = 0;
    virtual std::vector<uint8_t> finalize() = 0;
    virtual void reset() = 0;
};

// ── CRC32 (ISO 3309, polynomial 0xEDB88320) ────────────────────────

class CRC32Checksum : public ChecksumEngine {
public:
    CRC32Checksum();
    std::string name() const override { return "crc32"; }
    size_t digest_size() const override { return 4; }
    void update(const uint8_t* data, size_t length) override;
    std::vector<uint8_t> finalize() override;
    void reset() override;

    // Convenience: one-shot computation
    static uint32_t compute(const uint8_t* data, size_t length);

private:
    uint32_t state_ = 0xFFFFFFFF;
    // Lookup table generated at startup by crc32_table_initializer()
    static uint32_t table_[256];
};

// ── SHA-256 (embedded implementation) ───────────────────────────────

class SHA256Checksum : public ChecksumEngine {
public:
    SHA256Checksum();
    std::string name() const override { return "sha256"; }
    size_t digest_size() const override { return 32; }
    void update(const uint8_t* data, size_t length) override;
    std::vector<uint8_t> finalize() override;
    void reset() override;

    static std::vector<uint8_t> compute(const uint8_t* data, size_t length);

private:
    uint32_t state_[8];
    uint8_t buffer_[64];
    uint64_t total_length_;
    size_t buffer_length_;

    void process_block(const uint8_t block[64]);
};

// ── MD5 (embedded implementation) ───────────────────────────────────

class MD5Checksum : public ChecksumEngine {
public:
    MD5Checksum();
    std::string name() const override { return "md5"; }
    size_t digest_size() const override { return 16; }
    void update(const uint8_t* data, size_t length) override;
    std::vector<uint8_t> finalize() override;
    void reset() override;

    static std::vector<uint8_t> compute(const uint8_t* data, size_t length);

private:
    uint32_t state_[4];
    uint8_t buffer_[64];
    uint64_t total_length_;
    size_t buffer_length_;

    void process_block(const uint8_t block[64]);
};

// ── Factory ─────────────────────────────────────────────────────────

std::unique_ptr<ChecksumEngine> create_checksum_engine(const std::string& algorithm);

// Convert digest to hex string
std::string digest_to_hex(const std::vector<uint8_t>& digest);

} // namespace offcat
