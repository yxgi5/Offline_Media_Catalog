#include "joliet_parser.h"
#include "../udf/udf_unicode.h"
#include "platform/file_util.h"
#include <cstdio>
#include <cstring>

namespace offcat {

// ── Helpers ─────────────────────────────────────────────────────────

static uint32_t joliet_read_both32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

// ── Parser ──────────────────────────────────────────────────────────

JolietParser::JolietParser(const std::string& filepath)
    : filepath_(filepath) {}

JolietParser::~JolietParser() {
    if (file_handle_) {
        std::fclose(static_cast<FILE*>(file_handle_));
    }
}

bool JolietParser::read_sector(int64_t sector, uint8_t* buffer, size_t count) {
    FILE* f = static_cast<FILE*>(file_handle_);
    if (!f) return false;

    if (std::fseek(f, static_cast<long>(sector * ISO_SECTOR_SIZE), SEEK_SET) != 0) {
        return false;
    }
    return std::fread(buffer, ISO_SECTOR_SIZE, count, f) == count;
}

bool JolietParser::open() {
    FILE* f = open_file_utf8(filepath_, "rb");
    if (!f) return false;
    file_handle_ = f;

    uint8_t sector[ISO_SECTOR_SIZE];
    int sector_num = ISO_VOLUME_DESC_SECTOR;

    // Scan volume descriptors for a Joliet SVD
    for (int i = 0; i < 64; i++) {
        if (!read_sector(sector_num, sector, 1)) return false;

        uint8_t type = sector[0];
        if (sector[1] != 'C' || sector[2] != 'D' || sector[3] != '0' ||
            sector[4] != '0' || sector[5] != '1') {
            return false;
        }

        if (type == VDT_SUPPLEMENTARY) {
            // Joliet escape sequence: %/E (0x25 0x2F 0x45)
            // Also accepted: %/@ (0x25 0x2F 0x40), %/C (0x25 0x2F 0x43)
            if (sector[88] == 0x25 && sector[89] == 0x2F &&
                (sector[90] == 0x45 || sector[90] == 0x40 ||
                 sector[90] == 0x43)) {
                volume_info_.has_joliet = true;
                volume_info_.volume_id.assign(
                    reinterpret_cast<const char*>(sector + 40), 32);
                auto trim = [](std::string& s) {
                    while (!s.empty() && s.back() == ' ') s.pop_back();
                };
                trim(volume_info_.volume_id);

                // Root directory record at offset 156
                uint8_t rec_len = sector[156];
                if (rec_len >= 33) {
                    root_extent_ = static_cast<int64_t>(
                        joliet_read_both32(sector + 158));
                    root_size_ = static_cast<int64_t>(
                        joliet_read_both32(sector + 166));
                }
                open_ = true;
                return true;
            }
        } else if (type == VDT_TERMINATOR) {
            return false;  // No Joliet SVD found
        }

        sector_num++;
    }
    return false;
}

std::string JolietParser::decode_joliet_name(const uint8_t* data, size_t length,
                                             bool is_directory) {
    // Strip version suffix ";1" from files (encoded as UCS-2)
    size_t name_units = length / 2;

    if (!is_directory) {
        // Look for ';' (0x003B in UCS-2 BE)
        for (size_t i = 0; i + 1 < name_units; i++) {
            if (data[i*2] == 0x00 && data[i*2+1] == 0x3B) {
                name_units = i;
                break;
            }
        }
    }

    std::string utf8;
    if (decode_utf16be_to_utf8(data, name_units * 2, utf8)) {
        return utf8;
    }

    // Fallback: compatibility decoding of individual units
    std::string result;
    for (size_t i = 0; i < name_units; i++) {
        uint16_t u = static_cast<uint16_t>((data[i*2] << 8) | data[i*2+1]);
        if (u >= 0xD800 && u <= 0xDFFF) continue;
        if (u < 0x80) {
            result.push_back(static_cast<char>(u));
        } else if (u < 0x800) {
            result.push_back(static_cast<char>(0xC0 | (u >> 6)));
            result.push_back(static_cast<char>(0x80 | (u & 0x3F)));
        } else {
            result.push_back(static_cast<char>(0xE0 | (u >> 12)));
            result.push_back(static_cast<char>(0x80 | ((u >> 6) & 0x3F)));
            result.push_back(static_cast<char>(0x80 | (u & 0x3F)));
        }
    }
    return result;
}

bool JolietParser::read_root_directory(std::vector<IsoEntry>& out) {
    return read_directory(root_extent_, root_size_, out);
}

bool JolietParser::read_directory(int64_t extent, int64_t size,
                                  std::vector<IsoEntry>& out) {
    out.clear();

    int64_t sectors = (size + ISO_SECTOR_SIZE - 1) / ISO_SECTOR_SIZE;
    if (sectors > 65536) sectors = 65536;

    std::vector<uint8_t> buffer(ISO_SECTOR_SIZE);

    for (int64_t s = 0; s < sectors; s++) {
        if (!read_sector(extent + s, buffer.data(), 1)) return false;

        size_t offset = 0;
        while (offset < ISO_SECTOR_SIZE) {
            uint8_t length = buffer[offset];
            if (length == 0) break;  // Padding
            if (length < 33 || offset + length > ISO_SECTOR_SIZE) break;

            const uint8_t* rec = buffer.data() + offset;
            IsoEntry entry;
            entry.extent = static_cast<int64_t>(joliet_read_both32(rec + 2));
            entry.size = static_cast<int64_t>(joliet_read_both32(rec + 10));
            entry.mtime = parse_iso_date(rec + 18);
            entry.flags = rec[25];
            entry.is_directory = (rec[25] & 0x02) != 0;

            // File identifier (UCS-2)
            uint8_t name_len = rec[32];
            if (33 + name_len > length) break;
            entry.name = decode_joliet_name(rec + 33, name_len,
                                             entry.is_directory);

            if (!entry.name.empty() && entry.name != "." && entry.name != "..") {
                out.push_back(std::move(entry));
            }
            offset += length;
        }
    }

    return true;
}

} // namespace offcat
