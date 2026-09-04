#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <functional>

namespace offcat {

// ── UDF constants ───────────────────────────────────────────────────

constexpr int UDF_SECTOR_SIZE = 2048;
constexpr int UDF_ANCHOR_SECTOR = 256;

// Descriptor tag identifiers
constexpr uint16_t TAG_PVD      = 1;    // Primary Volume Descriptor
constexpr uint16_t TAG_AVDP     = 2;    // Anchor Volume Descriptor Pointer
constexpr uint16_t TAG_PD       = 5;    // Partition Descriptor
constexpr uint16_t TAG_LVD      = 6;    // Logical Volume Descriptor
constexpr uint16_t TAG_USD      = 7;    // Unallocated Space Descriptor
constexpr uint16_t TAG_TD       = 8;    // Terminating Descriptor
constexpr uint16_t TAG_FSD      = 256;  // File Set Descriptor
constexpr uint16_t TAG_FID      = 257;  // File Identifier Descriptor
constexpr uint16_t TAG_AE       = 258;  // Allocation Extent Descriptor
constexpr uint16_t TAG_FE       = 261;  // File Entry
constexpr uint16_t TAG_EFE      = 266;  // Extended File Entry

// ── UDF structures ──────────────────────────────────────────────────

struct UdfTag {
    uint16_t identifier = 0;
    uint16_t version = 2;
    uint8_t checksum = 0;
    uint16_t serial = 0;
    uint32_t location = 0;
};

struct UdfLongAd {
    uint32_t extent_length = 0;
    uint32_t location = 0;
    uint16_t partition_ref = 0;
};

struct UdfEntry {
    std::string name;            // UTF-8
    bool is_directory = false;
    int64_t size = 0;
    int64_t mtime = 0;
    int64_t extent_location = 0; // Partition-relative block
    uint16_t partition_ref = 0;
    bool is_metadata = false;    // From metadata partition (UDF 2.50+)
};

// ── UDF Parser ──────────────────────────────────────────────────────

class UdfParser {
public:
    explicit UdfParser(const std::string& filepath);
    ~UdfParser();

    UdfParser(const UdfParser&) = delete;
    UdfParser& operator=(const UdfParser&) = delete;

    // Detect UDF structure (NSR02/NSR03), fill volume info
    bool open();

    std::string volume_identifier() const { return volume_identifier_; }
    std::string filesystem_type() const { return fs_type_; }
    bool is_open() const { return open_; }

    // Enumerate root directory entries (names decoded via OSTA rules)
    bool read_root_directory(std::vector<UdfEntry>& out);

    // Read raw sector data
    bool read_sector(int64_t sector, uint8_t* buffer, size_t count);

    // Read tag at a given sector; validates checksum
    bool read_tag(int64_t sector, UdfTag& tag);

    // Read a descriptor body after validating its tag
    bool read_descriptor(int64_t sector, uint16_t expected_tag,
                         uint8_t* buffer, size_t size);

    // Read a file entry (FE/EFE) ICB; returns parsed info
    bool read_file_entry(int64_t block, uint16_t partition_ref,
                         int64_t& extent, int64_t& size,
                         bool& is_directory, int64_t& mtime,
                         std::vector<UdfLongAd>& ads);

    // Read a File Identifier Descriptor
    bool read_fid(const uint8_t* data, size_t available,
                  std::string& name, bool& is_directory,
                  UdfLongAd& icb, bool& is_parent);

    // Translate partition-relative block to absolute sector
    int64_t partition_to_absolute(uint16_t partition_ref, int64_t block);

    // Walk a directory given its ICB (recursion guard via `depth`)
    bool read_directory(const UdfLongAd& icb, std::vector<UdfEntry>& out,
                        int depth);

private:
    std::string filepath_;
    void* file_handle_ = nullptr;
    bool open_ = false;

    std::string volume_identifier_;
    std::string fs_type_;        // "NSR02" or "NSR03"

    // Partition map: partition number -> start sector
    std::vector<int64_t> partition_starts_;
    std::vector<int64_t> partition_lengths_;

    int64_t fsd_location_ = 0;   // File Set Descriptor location
    uint16_t fsd_partition_ = 0;
    int64_t root_icb_location_ = 0;
    uint16_t root_partition_ = 0;
    bool root_is_metadata_ = false;
    int64_t metadata_file_location_ = 0;   // UDF 2.50 metadata partition

    bool parse_anchor();
    bool parse_vds(int64_t vds_sector, int64_t vds_length);
    bool parse_lvd(const uint8_t* desc, size_t size);
    bool parse_pd(const uint8_t* desc, size_t size, int index);
    bool parse_fsd(const uint8_t* desc, size_t size);
    bool parse_partition_maps(const uint8_t* data, size_t length);

    // Fallback for genisoimage-style images whose LVD/PD pointers are
    // garbage: rebase partition 0 on the FSD sector found right after
    // the anchor and use the first resolvable directory FE as the root
    // (depth-first head tree), skipping junk/placeholder candidates.
    bool locate_root_in_head(std::vector<UdfEntry>& out);

    // Read allocation extent (possibly chained via Allocation Extent)
    bool read_allocation_extent(int64_t sector, int64_t length,
                                std::vector<uint8_t>& out);
};

} // namespace offcat
