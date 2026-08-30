#include "udf_parser.h"
#include "udf_unicode.h"
#include "core/logger.h"
#include "platform/file_util.h"
#include <cstdio>
#include <cstring>
#include <ctime>

namespace offcat {

// ── Byte-order helpers (UDF uses little-endian) ─────────────────────

static uint16_t u16le(const uint8_t* p) {
    return static_cast<uint16_t>(p[0] | (p[1] << 8));
}

static uint32_t u32le(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

static uint64_t u64le(const uint8_t* p) {
    return static_cast<uint64_t>(u32le(p)) |
           (static_cast<uint64_t>(u32le(p + 4)) << 32);
}

// UDF timestamp: 12 bytes (type_and_tz(1) year(2) month(1) day(1)
// hour(1) minute(1) second(1) centiseconds(1) hundreds(1) tens(1) units(1))
static int64_t parse_udf_timestamp(const uint8_t* p) {
    int tz = static_cast<int8_t>(p[0]);
    int year = u16le(p + 1);
    int month = p[3];
    int day = p[4];
    int hour = p[5];
    int minute = p[6];
    int second = p[7];

    if (year == 0) return 0;

    std::tm tm{};
    tm.tm_year = year - 1900;
    tm.tm_mon = month - 1;
    tm.tm_mday = day;
    tm.tm_hour = hour;
    tm.tm_min = minute;
    tm.tm_sec = second;
    tm.tm_isdst = -1;

    time_t t = std::mktime(&tm);
    if (t == static_cast<time_t>(-1)) return 0;

    // Timezone: 0 = UTC, else offset in 15-min units
    // (positive = east of GMT)
    if (tz != 0) {
        int offset_minutes = tz * 15;
        t -= offset_minutes * 60;
    }
    return static_cast<int64_t>(t);
}

// ── Tag handling ────────────────────────────────────────────────────

static bool parse_tag(const uint8_t* data, size_t size, UdfTag& tag) {
    if (size < 16) return false;
    tag.identifier = u16le(data + 0);
    tag.version = u16le(data + 2);
    tag.checksum = data[4];
    tag.serial = u16le(data + 6);
    tag.location = u32le(data + 12);

    // Validate checksum: sum of all bytes except byte 4 == byte 4
    uint8_t sum = 0;
    for (int i = 0; i < 16; i++) {
        if (i == 4) continue;
        sum = static_cast<uint8_t>((sum + data[i]) & 0xFF);
    }
    return sum == tag.checksum;
}

// ── Parser core ─────────────────────────────────────────────────────

UdfParser::UdfParser(const std::string& filepath) : filepath_(filepath) {}

UdfParser::~UdfParser() {
    if (file_handle_) {
        std::fclose(static_cast<FILE*>(file_handle_));
    }
}

bool UdfParser::read_sector(int64_t sector, uint8_t* buffer, size_t count) {
    FILE* f = static_cast<FILE*>(file_handle_);
    if (!f) return false;
    if (fseek_64(f, sector * UDF_SECTOR_SIZE, SEEK_SET) != 0) {
        return false;
    }
    return std::fread(buffer, UDF_SECTOR_SIZE, count, f) == count;
}

bool UdfParser::read_tag(int64_t sector, UdfTag& tag) {
    uint8_t data[16];
    FILE* f = static_cast<FILE*>(file_handle_);
    if (!f) return false;
    if (fseek_64(f, sector * UDF_SECTOR_SIZE, SEEK_SET) != 0) {
        return false;
    }
    if (std::fread(data, 1, 16, f) != 16) return false;
    return parse_tag(data, 16, tag);
}

bool UdfParser::read_descriptor(int64_t sector, uint16_t expected_tag,
                                uint8_t* buffer, size_t size) {
    UdfTag tag;
    if (!read_tag(sector, tag)) return false;
    if (tag.identifier != expected_tag) return false;

    FILE* f = static_cast<FILE*>(file_handle_);
    if (fseek_64(f, sector * UDF_SECTOR_SIZE, SEEK_SET) != 0) {
        return false;
    }
    size_t to_read = size < UDF_SECTOR_SIZE ? size : UDF_SECTOR_SIZE;
    return std::fread(buffer, 1, to_read, f) == to_read;
}

bool UdfParser::open() {
    FILE* f = open_file_utf8(filepath_, "rb");
    if (!f) return false;
    file_handle_ = f;
    return parse_anchor();
}

bool UdfParser::parse_anchor() {
    // Anchor Volume Descriptor Pointer: at sector 256, with fallbacks
    // at 512, 1024, 2048 (per spec: 256, 256*N)
    const int64_t candidates[] = {256, 512, 1024, 2048, 4096};

    for (int64_t sector : candidates) {
        uint8_t data[UDF_SECTOR_SIZE];
        UdfTag tag;
        if (!read_tag(sector, tag)) continue;
        if (tag.identifier != TAG_AVDP) continue;
        if (!read_sector(sector, data, 1)) continue;

        // AVDP: tag(16) main_vds_extent(16) reserve_vds_extent(16)
        int64_t main_vds_length = static_cast<int64_t>(u32le(data + 16));
        int64_t main_vds_location = static_cast<int64_t>(u32le(data + 20));

        if (main_vds_length <= 0 || main_vds_location < 0) continue;

        // Parse the VDS; the NSR magic is in the LVD "logical volume
        // contents use" field, not necessarily at VDS start
        if (parse_vds(main_vds_location, main_vds_length)) {
            open_ = true;
            return true;
        }
    }
    return false;
}

bool UdfParser::parse_vds(int64_t vds_sector, int64_t vds_length) {
    // Volume Descriptor Sequence: chain of descriptors, each 2048 bytes,
    // terminated by TAG_TD
    int64_t count = vds_length / UDF_SECTOR_SIZE;
    if (count <= 0 || count > 512) count = 512;

    int pd_index = 0;

    for (int64_t i = 0; i < count; i++) {
        int64_t sector = vds_sector + i;
        UdfTag tag;
        if (!read_tag(sector, tag)) return false;
        if (tag.identifier == TAG_TD) return true;

        uint8_t desc[UDF_SECTOR_SIZE];
        if (!read_sector(sector, desc, 1)) return false;

        switch (tag.identifier) {
            case TAG_PVD: {
                // Volume identifier: bytes 24..55 (d-string, 32 bytes)
                auto decoded = decode_udf_dstring(desc + 24, 32);
                if (!decoded.utf8.empty()) {
                    volume_identifier_ = decoded.utf8;
                }
                break;
            }
            case TAG_PD:
                parse_pd(desc, sizeof(desc), pd_index);
                pd_index++;
                break;
            case TAG_LVD:
                parse_lvd(desc, sizeof(desc));
                break;
            default:
                break;
        }
    }
    return false;
}

bool UdfParser::parse_pd(const uint8_t* desc, size_t size, int index) {
    // ECMA-167 3/10.5 Partition Descriptor:
    // tag(16) recording_date(12) partition_contents(32)
    // partition_contents_use(128) access_type(4)
    // partition_starting_location(4) partition_length(4) ...
    if (size < 200) return false;

    uint32_t partition_start = u32le(desc + 192);
    uint32_t partition_length = u32le(desc + 196);

    while (static_cast<int>(partition_starts_.size()) <= index) {
        partition_starts_.push_back(0);
        partition_lengths_.push_back(0);
    }
    partition_starts_[index] = partition_start;
    partition_lengths_[index] = partition_length;
    return true;
}

bool UdfParser::parse_lvd(const uint8_t* desc, size_t size) {
    // ECMA-167 3/10.6 Logical Volume Descriptor:
    // tag(16) volume_seq_number(4) desc_char_set(64)
    // logical_volume_id(128) logical_block_size(4) domain_identifier(32)
    // logical_volume_contents_use(128) map_table_length(4)
    // number_of_partition_maps(4) impl_identifier(32) impl_use(128)
    // integrity_seq_extent(16) partition_maps(variable)
    if (size < 560) return false;

    // Logical volume contents use at 248 (contains NSR02/NSR03)
    auto contents = decode_udf_dstring(desc + 248, 128);
    if (!contents.utf8.empty()) {
        fs_type_ = contents.utf8;
        if (fs_type_.size() > 5) fs_type_ = fs_type_.substr(0, 5);
    }

    // For NSR02/NSR03, the first 16 bytes of logical volume contents
    // use is a long_ad pointing to the File Set Descriptor
    fsd_location_ = u32le(desc + 248 + 4);
    fsd_partition_ = u16le(desc + 248 + 8);

    // Map table length at 376, partition maps start at 560
    uint32_t map_length = u32le(desc + 376);
    if (map_length > size - 560) map_length = static_cast<uint32_t>(size - 560);
    return parse_partition_maps(desc + 560, map_length);
}

bool UdfParser::parse_partition_maps(const uint8_t* data, size_t length) {
    // Partition maps: type(1) length(1) data(variable)
    size_t offset = 0;
    while (offset + 6 <= length) {
        uint8_t type = data[offset];
        uint8_t length_field = data[offset + 1];

        if (type == 1 && length_field == 6) {
            // Type 1: partition number (2 bytes at offset+2)
            uint16_t partition_number = u16le(data + offset + 2);
            // The partition start was recorded from the PD sequence;
            // ensure the map table is consistent
            if (partition_starts_.size() > partition_number) {
                // Partition already known from PD parsing
            }
            offset += 6;
        } else if (type == 2 && length_field >= 6) {
            // Type 2: metadata partition (UDF 2.50+, NSR03)
            // Metadata file location at offset+2 (long_ad)
            metadata_file_location_ = u32le(data + offset + 6);
            offset += length_field;
        } else {
            break;
        }
    }
    return true;
}

// Parse allocation descriptors (ECMA-167 3/7.2).  Standard images use
// 16-byte long_ad records; genisoimage-style images use 8-byte short_ad
// records (their len_alloc is 8).  Distinguish by alignment: long_ad
// records always come in 16-byte multiples, short_ad in 8-byte ones.
static void parse_allocation_descriptors(const uint8_t* data,
                                         uint32_t len_alloc,
                                         std::vector<UdfLongAd>& ads) {
    size_t ad_size = (len_alloc % 16 == 0) ? 16 : 8;
    size_t alloc_end = len_alloc;
    if (alloc_end > UDF_SECTOR_SIZE) alloc_end = UDF_SECTOR_SIZE;

    size_t pos = 0;
    while (pos + ad_size <= alloc_end) {
        UdfLongAd ad;
        ad.extent_length = u32le(data + pos) & 0x3FFFFFFF;
        ad.location = u32le(data + pos + 4);
        ad.partition_ref = (ad_size == 16) ? u16le(data + pos + 8) : 0;
        ads.push_back(ad);
        pos += ad_size;
    }
}

bool UdfParser::read_file_entry(int64_t block, uint16_t partition_ref,
                                int64_t& extent, int64_t& size,
                                bool& is_directory, int64_t& mtime,
                                std::vector<UdfLongAd>& ads) {
    int64_t abs_sector = partition_to_absolute(partition_ref, block);
    if (abs_sector < 0) return false;

    UdfTag tag;
    if (!read_tag(abs_sector, tag)) return false;
    if (tag.identifier != TAG_FE && tag.identifier != TAG_EFE) return false;

    uint8_t desc[UDF_SECTOR_SIZE];
    if (!read_sector(abs_sector, desc, 1)) return false;

    // File type in ICB tag: ICB tag starts at offset 16, file type at
    // offset 11 within the ICB tag (ECMA-167 4/14.9.2)
    uint8_t file_type = desc[16 + 11];
    is_directory = (file_type == 4);

    if (tag.identifier == TAG_FE) {
        // ECMA-167 4/14.9 File Entry:
        // tag(16) icb_tag(20) uid(4) gid(4) permissions(4)
        // file_link_count(2) record_format(1) display(1) record_len(4)
        // information_length(8) logical_blocks(8) access_time(12)
        // modification_time(12) attribute_time(12) checkpoint(4)
        // ext_attr_icb(16) impl_id(32) unique_id(8)
        // len_ext_attr(4) len_alloc_desc(4)
        size = static_cast<int64_t>(u64le(desc + 56));
        mtime = parse_udf_timestamp(desc + 84);
        uint32_t len_alloc = u32le(desc + 172);
        ads.clear();
        parse_allocation_descriptors(desc + 176, len_alloc, ads);
    } else {
        // TAG_EFE: Extended File Entry (ECMA-167 4/14.10):
        // Same as FE up to information_length(56), then object_size(8)
        // logical_blocks(8) access_time(12) modification_time(12)
        // creation_time(12) attribute_time(12) checkpoint(4) reserved(4)
        // ext_attr_icb(16) impl_id(32) unique_id(8)
        // len_ext_attr(4) len_alloc_desc(4)
        size = static_cast<int64_t>(u64le(desc + 56));
        mtime = parse_udf_timestamp(desc + 84);
        uint32_t len_alloc = u32le(desc + 196);
        ads.clear();
        parse_allocation_descriptors(desc + 200, len_alloc, ads);
    }

    // First allocation descriptor is the file extent
    if (!ads.empty()) {
        extent = ads[0].location;
    }
    return true;
}

bool UdfParser::read_fid(const uint8_t* data, size_t available,
                         std::string& name, bool& is_directory,
                         UdfLongAd& icb, bool& is_parent) {
    // ECMA-167 4/14.4 File Identifier Descriptor:
    // tag(16) file_version(2) file_characteristics(1)
    // len_fi(1) icb(16) len_impl_use(2) impl_use(variable) file_id
    if (available < 38) return false;

    uint8_t file_chars = data[18];
    uint8_t len_fi = data[19];
    uint16_t len_impl_use = u16le(data + 36);

    icb.extent_length = u32le(data + 20);
    icb.location = u32le(data + 24);
    icb.partition_ref = u16le(data + 28);

    size_t total = 38 + len_fi + len_impl_use;
    if (total > available) return false;

    is_directory = (file_chars & 0x02) != 0;
    is_parent = (file_chars & 0x08) != 0;

    if (len_fi > 0) {
        auto decoded = decode_udf_name(data + 38 + len_impl_use, len_fi);
        name = decoded.utf8;
    } else {
        name.clear();
    }
    return true;
}

bool UdfParser::read_directory(const UdfLongAd& icb, std::vector<UdfEntry>& out,
                               int depth) {
    if (depth > 64) return false;  // Safety

    // Read the directory file entry to get its extent
    int64_t extent = icb.location;
    int64_t size = icb.extent_length;
    bool is_dir = false;
    int64_t mtime = 0;
    std::vector<UdfLongAd> ads;

    if (!read_file_entry(icb.location, icb.partition_ref, extent, size,
                         is_dir, mtime, ads)) {
        return false;
    }
    if (!is_dir) return false;

    // Read directory data (usually a single contiguous extent)
    int64_t abs_sector = partition_to_absolute(icb.partition_ref, extent);
    if (abs_sector < 0) return false;

    int64_t sectors_needed = (size + UDF_SECTOR_SIZE - 1) / UDF_SECTOR_SIZE;
    if (sectors_needed <= 0) sectors_needed = 1;
    if (sectors_needed > 8192) return false;

    std::vector<uint8_t> dir_data;
    dir_data.resize(static_cast<size_t>(sectors_needed) * UDF_SECTOR_SIZE);
    for (int64_t s = 0; s < sectors_needed; s++) {
        if (!read_sector(abs_sector + s,
                         dir_data.data() + static_cast<size_t>(s) * UDF_SECTOR_SIZE,
                         1)) {
            return false;
        }
    }

    // genisoimage-style images may under-report the directory size in
    // the File Entry (e.g. 88 bytes for a root that actually spans
    // several sectors) and may split File Identifier Descriptors
    // across sector boundaries (a FID header that ends near the end of
    // a sector, with the file name continuing in the next sector).
    // Treat the directory data as one contiguous byte stream: whenever
    // a FID does not fit into the data read so far, extend by one more
    // sector and keep going.  Padding (zeroes) ends the stream.
    out.clear();
    size_t offset = 0;

    while (true) {
        if (offset + 38 > dir_data.size()) break;  // Stream ended

        const uint8_t* fid_data = dir_data.data() + offset;

        // Padding (zeroed region) marks the end of the stream
        if (fid_data[0] == 0 && fid_data[1] == 0 &&
            fid_data[2] == 0 && fid_data[3] == 0) {
            break;
        }

        // Anything that does not start with a FID tag is not part of
        // the directory stream (e.g. leftover garbage in an
        // under-allocated extent)
        if (u16le(fid_data) != TAG_FID) break;

        // FID record length is implicit: 38 + len_fi + len_impl_use
        uint8_t len_fi = fid_data[19];
        uint16_t len_impl_use = u16le(fid_data + 36);
        size_t fid_total = 38 + len_fi + len_impl_use;

        if (offset + fid_total > dir_data.size()) {
            // The FID (header or name) runs past the end of the data
            // read so far: pull in the next sector and retry.
            if (sectors_needed >= 8192) break;
            uint8_t probe[UDF_SECTOR_SIZE];
            if (!read_sector(abs_sector + sectors_needed, probe, 1)) break;
            bool nonzero = false;
            for (int i = 0; i < UDF_SECTOR_SIZE && !nonzero; i++) {
                if (probe[i] != 0) nonzero = true;
            }
            if (!nonzero) break;  // All zeroes: stream really ended
            dir_data.insert(dir_data.end(), probe, probe + UDF_SECTOR_SIZE);
            sectors_needed++;
            continue;
        }

        uint8_t file_chars = fid_data[18];
        bool is_dir_flag = (file_chars & 0x02) != 0;
        bool is_parent = (file_chars & 0x08) != 0;
        bool is_deleted = (file_chars & 0x04) != 0;

        if (!is_parent && !is_deleted) {
            std::string name;
            if (len_fi > 0) {
                auto decoded = decode_udf_name(
                    fid_data + 38 + len_impl_use, len_fi);
                name = decoded.utf8;
            }

            if (!name.empty() && name != "." && name != "..") {
                UdfEntry entry;
                entry.name = name;
                entry.is_directory = is_dir_flag;
                entry.extent_location = u32le(fid_data + 24);
                entry.partition_ref = u16le(fid_data + 28);
                entry.size = u32le(fid_data + 20) & 0x3FFFFFFF;
                entry.mtime = mtime;
                out.push_back(std::move(entry));
            }
        }

        // Align to 4-byte boundary
        offset += (fid_total + 3) & ~3;
    }

    return true;
}

int64_t UdfParser::partition_to_absolute(uint16_t partition_ref, int64_t block) {
    if (partition_ref >= partition_starts_.size()) {
        // Unknown partition: try the first one
        if (partition_starts_.empty()) return -1;
        return partition_starts_[0] + block;
    }
    return partition_starts_[partition_ref] + block;
}

bool UdfParser::read_root_directory(std::vector<UdfEntry>& out) {
    // File Set Descriptor (ECMA-167 4/14.1):
    // tag(16) recording_date(12) interchange_level(2) max_interchange(2)
    // char_set_list(8) max_char_set_list(8) file_set_number(4)
    // file_set_desc_number(4) lv_id_char_set(64) logical_vol_id(128)
    // file_set_char_set(64) file_set_id(32) copyright_id(32)
    // abstract_id(32) root_directory_icb(16)
    // Root ICB long_ad at offset 408

    if (fsd_location_ != 0) {
        // Standard path: FSD located via the LVD logical volume
        // contents use long_ad.
        int64_t abs_fsd = partition_to_absolute(fsd_partition_, fsd_location_);
        if (abs_fsd >= 0) {
            UdfTag fsd_tag;
            if (read_tag(abs_fsd, fsd_tag) && fsd_tag.identifier == TAG_FSD) {
                uint8_t desc[UDF_SECTOR_SIZE];
                if (read_descriptor(abs_fsd, TAG_FSD, desc, sizeof(desc))) {
                    // Root directory ICB (long_ad)
                    UdfLongAd root_icb;
                    root_icb.extent_length = u32le(desc + 408);
                    root_icb.location = u32le(desc + 412);
                    root_icb.partition_ref = u16le(desc + 416);

                    if (root_icb.extent_length != 0 && root_icb.location != 0 &&
                        read_directory(root_icb, out, 0)) {
                        return true;
                    }
                }
            } else {
                LOG_VERBOSE("UDF: expected FSD (tag " + std::to_string(TAG_FSD) +
                            ") at sector " + std::to_string(abs_fsd) +
                            " but found tag " + std::to_string(fsd_tag.identifier));
            }
        }
    }

    // Fallback for genisoimage-style images: the LVD FSD pointer,
    // partition maps and PD partition start are garbage, but the whole
    // directory tree lives right after the anchor with block numbers
    // relative to the FSD sector itself.
    return locate_root_in_head(out);
}

// genisoimage -udf produces non-standard images where the FSD pointer
// in the LVD is 0 and the PD partition start is junk; the real tree is
// stored immediately after the anchor (AVDP) sector and every block
// number is relative to the FSD sector.  Rebase partition 0 on the FSD
// sector and use the first directory File Entry after it as the root.
bool UdfParser::locate_root_in_head(std::vector<UdfEntry>& out) {
    const int64_t kScan = 512;
    const int64_t kAnchor = UDF_ANCHOR_SECTOR;

    // 1. Find the FSD right after the anchor sector.
    int64_t fsd_abs = -1;
    for (int64_t s = kAnchor + 1; s < kAnchor + kScan; s++) {
        UdfTag tag;
        if (read_tag(s, tag) && tag.identifier == TAG_FSD) {
            fsd_abs = s;
            break;
        }
    }
    if (fsd_abs < 0) return false;

    // 2. Rebase partition 0 so block numbers are FSD-relative.
    if (partition_starts_.empty()) {
        partition_starts_.push_back(fsd_abs);
    } else {
        partition_starts_[0] = fsd_abs;
    }

    // 3. genisoimage may emit several directory File Entries before the
    // real root (a leftover FE with a tiny/under-allocated extent).
    // Pick the candidate whose directory stream holds the most entries
    // (e.g. the true root spans 2 sectors with 61 FIDs, while the
    // leftover only has a couple of garbage FIDs).
    std::vector<UdfEntry> best;
    int candidates = 0;
    for (int64_t s = fsd_abs + 1; s < fsd_abs + kScan; s++) {
        UdfTag tag;
        if (!read_tag(s, tag)) continue;
        if (tag.identifier != TAG_FE && tag.identifier != TAG_EFE) continue;

        uint8_t desc[UDF_SECTOR_SIZE];
        if (!read_sector(s, desc, 1)) continue;
        if (desc[16 + 11] != 4) continue;  // file type must be directory

        UdfLongAd root_icb;
        root_icb.extent_length = UDF_SECTOR_SIZE;
        root_icb.location = s - partition_starts_[0];
        root_icb.partition_ref = 0;

        std::vector<UdfEntry> tmp;
        if (!read_directory(root_icb, tmp, 0)) continue;
        if (tmp.size() > best.size()) best = std::move(tmp);
        if (++candidates >= 4) break;  // Enough candidates
    }
    if (best.empty()) return false;
    out = std::move(best);
    return true;
}

bool UdfParser::read_allocation_extent(int64_t sector, int64_t length,
                                       std::vector<uint8_t>& out) {
    out.clear();
    int64_t sectors = (length + UDF_SECTOR_SIZE - 1) / UDF_SECTOR_SIZE;
    if (sectors <= 0 || sectors > 1024) return false;

    out.resize(static_cast<size_t>(sectors) * UDF_SECTOR_SIZE);
    for (int64_t s = 0; s < sectors; s++) {
        if (!read_sector(sector + s, out.data() + static_cast<size_t>(s) * UDF_SECTOR_SIZE, 1)) {
            return false;
        }
    }
    return true;
}

bool UdfParser::parse_fsd(const uint8_t* desc, size_t size) {
    (void)desc;
    (void)size;
    // FSD parsing happens in read_root_directory
    return true;
}

} // namespace offcat
