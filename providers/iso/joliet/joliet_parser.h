#pragma once

#include "../iso9660/iso9660_parser.h"
#include <cstdint>
#include <string>
#include <vector>

namespace offcat {

// ── Joliet parser ───────────────────────────────────────────────────
//
// Joliet is an ISO9660 extension using a Supplementary Volume
// Descriptor (SVD) with escape sequence %/E and UCS-2 (UTF-16BE)
// filenames. Directory records share the ISO9660 layout.

class JolietParser {
public:
    explicit JolietParser(const std::string& filepath);
    ~JolietParser();

    JolietParser(const JolietParser&) = delete;
    JolietParser& operator=(const JolietParser&) = delete;

    // Validate SVD presence with Joliet escape sequence
    bool open();

    // Enumerate root directory entries (names decoded as UTF-8)
    bool read_root_directory(std::vector<IsoEntry>& out);

    // Enumerate a directory extent (names decoded as UTF-8)
    bool read_directory(int64_t extent, int64_t size,
                        std::vector<IsoEntry>& out);

    const IsoVolumeInfo& volume_info() const { return volume_info_; }

    // Read raw sector data
    bool read_sector(int64_t sector, uint8_t* buffer, size_t count);

private:
    std::string filepath_;
    void* file_handle_ = nullptr;
    IsoVolumeInfo volume_info_;
    int64_t root_extent_ = 0;
    int64_t root_size_ = 0;
    bool open_ = false;

    // Decode a Joliet file identifier (UCS-2, UTF-16BE) to UTF-8
    static std::string decode_joliet_name(const uint8_t* data, size_t length,
                                          bool is_directory);
};

} // namespace offcat
