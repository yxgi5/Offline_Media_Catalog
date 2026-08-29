#include "iso9660_parser.h"
#include <cstdio>
#include <cstring>
#include <ctime>
#include <algorithm>

namespace offcat {

// ── Helpers ─────────────────────────────────────────────────────────

static uint16_t read_le16(const uint8_t* p) {
    return static_cast<uint16_t>(p[0] | (p[1] << 8));
}

static uint32_t read_le32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

// Both-byte-order 32-bit field (LE first, then BE)
static uint32_t read_both32(const uint8_t* p) {
    return read_le32(p);  // LE value is authoritative
}

// ── Directory record parsing ────────────────────────────────────────

int64_t parse_iso_date(const uint8_t* data) {
    // ISO 9660 date: Y(1) M(1) D(1) h(1) m(1) s(1) offset(1)
    int year = data[0];
    int month = data[1];
    int day = data[2];
    int hour = data[3];
    int minute = data[4];
    int second = data[5];
    int offset = static_cast<int8_t>(data[6]);  // 15-min intervals from GMT

    if (year == 0 && month == 0 && day == 0) return 0;

    std::tm tm{};
    tm.tm_year = year;           // years since 1900
    tm.tm_mon = month - 1;
    tm.tm_mday = day;
    tm.tm_hour = hour;
    tm.tm_min = minute;
    tm.tm_sec = second;
    tm.tm_isdst = -1;

    time_t t = std::mktime(&tm);
    if (t == static_cast<time_t>(-1)) return 0;
    // Apply timezone offset (15-minute units)
    t -= offset * 900LL;
    return static_cast<int64_t>(t);
}

size_t parse_directory_record(const uint8_t* data, size_t available,
                              IsoEntry& out) {
    if (available < 1) return 0;
    uint8_t length = data[0];
    if (length == 0) return 0;  // Padding at end of sector
    if (length < 33 || length > available) return 0;

    uint8_t ext_attr_len = data[1];
    out.extent = static_cast<int64_t>(read_both32(data + 2));
    out.size = static_cast<int64_t>(read_both32(data + 10));
    out.mtime = parse_iso_date(data + 18);
    out.flags = data[25];

    // File identifier
    uint8_t name_len = data[32];
    if (33 + name_len > length) return 0;
    const uint8_t* name_data = data + 33;

    out.is_directory = (out.flags & 0x02) != 0;

    // Names: "FILE.EXT;1" for files, "DIR" for directories
    std::string raw_name(reinterpret_cast<const char*>(name_data), name_len);

    if (!out.is_directory) {
        // Strip version suffix ";1" (or ";<n>")
        auto semicolon = raw_name.find(';');
        if (semicolon != std::string::npos) {
            raw_name = raw_name.substr(0, semicolon);
        }
    }

    // ISO 9660 names are ASCII (d-characters); keep as-is
    out.name = raw_name;
    return length;
}

// ── Parser ──────────────────────────────────────────────────────────

Iso9660Parser::Iso9660Parser(const std::string& filepath)
    : filepath_(filepath) {}

Iso9660Parser::~Iso9660Parser() {
    if (file_handle_) {
        std::fclose(static_cast<FILE*>(file_handle_));
    }
}

bool Iso9660Parser::open() {
    FILE* f = std::fopen(filepath_.c_str(), "rb");
    if (!f) return false;
    file_handle_ = f;

    // Check magic at sector 16
    uint8_t sector[ISO_SECTOR_SIZE];
    if (!read_sector(ISO_VOLUME_DESC_SECTOR, sector, 1)) return false;

    if (sector[1] != 'C' || sector[2] != 'D' || sector[3] != '0' ||
        sector[4] != '0' || sector[5] != '1') {
        // Could still be UDF-only; not ISO9660
        return false;
    }

    open_ = parse_volume_descriptors();
    return open_;
}

bool Iso9660Parser::read_sector(int64_t sector, uint8_t* buffer, size_t count) {
    FILE* f = static_cast<FILE*>(file_handle_);
    if (!f) return false;

    if (std::fseek(f, static_cast<long>(sector * ISO_SECTOR_SIZE), SEEK_SET) != 0) {
        return false;
    }
    return std::fread(buffer, ISO_SECTOR_SIZE, count, f) == count;
}

bool Iso9660Parser::parse_volume_descriptors() {
    uint8_t sector[ISO_SECTOR_SIZE];
    int sector_num = ISO_VOLUME_DESC_SECTOR;

    for (int i = 0; i < 64; i++) {  // Safety limit
        if (!read_sector(sector_num, sector, 1)) return false;

        uint8_t type = sector[0];
        if (sector[1] != 'C' || sector[2] != 'D' || sector[3] != '0' ||
            sector[4] != '0' || sector[5] != '1') {
            return false;  // Corrupted descriptor chain
        }

        if (type == VDT_PRIMARY_VOLUME) {
            volume_info_.system_id.assign(
                reinterpret_cast<const char*>(sector + 8), 32);
            volume_info_.volume_id.assign(
                reinterpret_cast<const char*>(sector + 40), 32);
            volume_info_.volume_space_size =
                static_cast<int64_t>(read_both32(sector + 80));
            volume_info_.logical_block_size = read_le16(sector + 128);
            if (volume_info_.logical_block_size <= 0) {
                volume_info_.logical_block_size = ISO_SECTOR_SIZE;
            }
            volume_info_.volume_set_id.assign(
                reinterpret_cast<const char*>(sector + 190), 128);
            volume_info_.publisher.assign(
                reinterpret_cast<const char*>(sector + 318), 128);
            volume_info_.application.assign(
                reinterpret_cast<const char*>(sector + 574), 128);

            // Trim trailing spaces
            auto trim = [](std::string& s) {
                while (!s.empty() && s.back() == ' ') s.pop_back();
            };
            trim(volume_info_.system_id);
            trim(volume_info_.volume_id);
            trim(volume_info_.volume_set_id);
            trim(volume_info_.publisher);
            trim(volume_info_.application);
        } else if (type == VDT_SUPPLEMENTARY) {
            // Check for Joliet escape sequence at offset 88
            // Escape sequences: %/@, %/C, %/E
            if (sector[88] == 0x25 && sector[89] == 0x2F) {
                volume_info_.has_joliet = true;
                volume_info_.volume_id.assign(
                    reinterpret_cast<const char*>(sector + 40), 32);
                auto trim = [](std::string& s) {
                    while (!s.empty() && s.back() == ' ') s.pop_back();
                };
                trim(volume_info_.volume_id);
            }
        } else if (type == VDT_TERMINATOR) {
            return true;
        }

        sector_num++;
    }
    return false;
}

bool Iso9660Parser::read_root_directory(std::vector<IsoEntry>& out) {
    // Root dir record is in PVD at offset 156
    uint8_t sector[ISO_SECTOR_SIZE];
    if (!read_sector(ISO_VOLUME_DESC_SECTOR, sector, 1)) return false;
    if (sector[0] != VDT_PRIMARY_VOLUME) return false;

    IsoEntry root;
    size_t consumed = parse_directory_record(sector + 156,
                                              ISO_SECTOR_SIZE - 156, root);
    if (consumed == 0 || !root.is_directory) return false;

    return read_directory(root.extent, root.size, out);
}

bool Iso9660Parser::read_directory(int64_t extent, int64_t size,
                                   std::vector<IsoEntry>& out) {
    out.clear();

    // Directories can span multiple sectors; records may be padded with
    // 0x00 (records of length 0 mean "skip to next sector")
    int64_t sectors = (size + ISO_SECTOR_SIZE - 1) / ISO_SECTOR_SIZE;
    if (sectors > 65536) sectors = 65536;  // Safety

    std::vector<uint8_t> buffer(ISO_SECTOR_SIZE);

    for (int64_t s = 0; s < sectors; s++) {
        if (!read_sector(extent + s, buffer.data(), 1)) return false;

        size_t offset = 0;
        while (offset < ISO_SECTOR_SIZE) {
            if (buffer[offset] == 0) {
                // Padding: skip to next sector
                break;
            }
            IsoEntry entry;
            size_t consumed = parse_directory_record(buffer.data() + offset,
                                                      ISO_SECTOR_SIZE - offset,
                                                      entry);
            if (consumed == 0) break;

            // Skip "." and ".." entries
            if (entry.name != "." && entry.name != "..") {
                out.push_back(std::move(entry));
            }
            offset += consumed;
        }
    }

    return true;
}

} // namespace offcat
