#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <optional>
#include <functional>

namespace offcat {

// ── ISO9660 structures ──────────────────────────────────────────────

constexpr int ISO_SECTOR_SIZE = 2048;
constexpr int ISO_VOLUME_DESC_SECTOR = 16;
constexpr char ISO_MAGIC[] = "CD001";

constexpr int VDT_BOOT_RECORD     = 0;
constexpr int VDT_PRIMARY_VOLUME   = 1;
constexpr int VDT_SUPPLEMENTARY    = 2;
constexpr int VDT_VOLUME_PARTITION = 3;
constexpr int VDT_TERMINATOR       = 255;

struct IsoVolumeInfo {
    std::string volume_id;         // Volume identifier
    std::string system_id;
    std::string volume_set_id;
    std::string publisher;
    std::string application;
    int64_t volume_space_size = 0;
    bool has_joliet = false;       // SVD with Joliet escape sequence
    int logical_block_size = 2048;
};

struct IsoEntry {
    std::string name;              // UTF-8
    bool is_directory = false;
    int64_t extent = 0;            // Start sector
    int64_t size = 0;
    int64_t mtime = 0;             // 0 if unknown
    int flags = 0;
};

// ── ISO9660 Parser ──────────────────────────────────────────────────

class Iso9660Parser {
public:
    // Read-only view over the ISO file
    explicit Iso9660Parser(const std::string& filepath);
    ~Iso9660Parser();

    Iso9660Parser(const Iso9660Parser&) = delete;
    Iso9660Parser& operator=(const Iso9660Parser&) = delete;

    // Validate magic; fills volume_info_
    bool open();

    const IsoVolumeInfo& volume_info() const { return volume_info_; }

    // Enumerate the root directory entries (non-recursive)
    bool read_root_directory(std::vector<IsoEntry>& out);

    // Enumerate entries of a directory extent
    bool read_directory(int64_t extent, int64_t size,
                        std::vector<IsoEntry>& out);

    // Read raw sector data
    bool read_sector(int64_t sector, uint8_t* buffer, size_t count);

private:
    std::string filepath_;
    void* file_handle_ = nullptr;  // FILE* as void*
    IsoVolumeInfo volume_info_;
    bool open_ = false;

    // Find the primary volume descriptor; returns true on success
    bool parse_volume_descriptors();
};

// Compute date from ISO 9660 7-byte date field
int64_t parse_iso_date(const uint8_t* data);

// Parse one directory record; returns consumed bytes or 0 on error
size_t parse_directory_record(const uint8_t* data, size_t available,
                              IsoEntry& out);

} // namespace offcat
