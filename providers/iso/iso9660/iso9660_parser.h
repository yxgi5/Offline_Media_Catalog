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
    std::string name;              // UTF-8 (ISO name, or RR NM name if present)
    bool is_directory = false;
    int64_t extent = 0;            // Start sector
    int64_t size = 0;
    int64_t mtime = 0;             // 0 if unknown
    int flags = 0;

    // Rock Ridge (RRIP 1.10/1.12) extensions, parsed from the
    // System Use Area of the directory record
    bool has_rr = false;           // Any SUSP/RRIP record present
    std::string rr_name;           // NM record(s) joined: the real name
    std::string rr_link_target;    // SL record: symlink target (raw)
    int64_t mode = 0;              // PX record: POSIX mode (0 = unknown)
    bool is_symlink = false;       // SL record present
    bool is_rr_placeholder = false; // Single-byte name (0x02-0x09): deep
                                     // directory relocated to rr_moved
    int rr_placeholder = 0;        // Placeholder byte value (2..9)
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

// Parse one directory record; returns consumed bytes or 0 on error.
// Rock Ridge (SUSP/RRIP) records in the System Use Area are parsed and
// applied to `out` (NM name, PX mode, TF time, SL symlink, placeholder
// detection).  `sector_reader` is used for CE (continuation area)
// records and may be empty, in which case CE records are skipped.
using SectorReader = std::function<bool(int64_t, uint8_t*, size_t)>;
size_t parse_directory_record(const uint8_t* data, size_t available,
                              IsoEntry& out,
                              const SectorReader& sector_reader = {});

} // namespace offcat
