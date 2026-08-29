// Rock Ridge (SUSP/RRIP) parsing and ISO provider recursion tests.
//
// A tiny ISO9660 + Rock Ridge image is synthesized in memory (no
// external tools needed), then exercised through:
//   1. parse_directory_record  - SUSP records (NM/PX/TF/SL), placeholders
//   2. Iso9660Parser           - directory enumeration with RR names
//   3. IsoProvider::scan       - rr_moved restoration, recursion, depth

#include "iso/iso9660/iso9660_parser.h"
#include "iso/iso_provider.h"
#include "database/database.h"
#include "catalog/catalog.h"
#include "container/provider.h"
#include "scanner/search.h"
#include "scanner/scanner.h"
#include <gtest/gtest.h>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
#include <vector>
#include <string>

using namespace offcat;

namespace {

// ── Byte helpers ────────────────────────────────────────────────────

void put_le16(std::vector<uint8_t>& v, size_t off, uint16_t val) {
    v[off] = static_cast<uint8_t>(val & 0xFF);
    v[off + 1] = static_cast<uint8_t>((val >> 8) & 0xFF);
}

void put_le32(std::vector<uint8_t>& v, size_t off, uint32_t val) {
    for (int i = 0; i < 4; i++) {
        v[off + i] = static_cast<uint8_t>((val >> (8 * i)) & 0xFF);
    }
}

void put_both32(std::vector<uint8_t>& v, size_t off, uint32_t val) {
    put_le32(v, off, val);
    put_le32(v, off + 4, val);
}

// ISO 9660 7-byte date: 2024-06-15 12:34:56 GMT+0
const uint8_t kDate[7] = {124, 6, 15, 12, 34, 56, 0};

// ── Sector layout of the synthesized image ──────────────────────────
const int kPvd = 16, kTerm = 17, kRoot = 18, kMoved = 19, kTop = 20,
          kDeep = 21, kFile = 22;
const int64_t kSectorSize = 2048;

// ── SUSP record builders ────────────────────────────────────────────

std::vector<uint8_t> susp_nm(const std::string& name, bool cont = false) {
    std::vector<uint8_t> r(5 + name.size(), 0);
    r[0] = 'N'; r[1] = 'M';
    r[2] = static_cast<uint8_t>(5 + name.size());
    r[3] = 1;
    r[4] = cont ? 0x01 : 0x00;
    std::memcpy(r.data() + 5, name.data(), name.size());
    return r;
}

std::vector<uint8_t> susp_px(uint32_t mode) {
    std::vector<uint8_t> r(36, 0);
    r[0] = 'P'; r[1] = 'X'; r[2] = 36; r[3] = 1;
    put_le32(r, 4, mode);
    return r;
}

std::vector<uint8_t> susp_tf() {
    std::vector<uint8_t> r(12, 0);
    r[0] = 'T'; r[1] = 'F'; r[2] = 12; r[3] = 1;
    r[4] = 0x02;  // modify time, short (7-byte) form
    std::memcpy(r.data() + 5, kDate, 7);
    return r;
}

// SL with the given components; component 0 gets `first_flags`.
std::vector<uint8_t> susp_sl(const std::vector<std::string>& components,
                             uint8_t first_flags = 0) {
    std::vector<uint8_t> r;
    r.push_back('S'); r.push_back('L');
    r.push_back(0);  // length placeholder
    r.push_back(1);  // version
    r.push_back(0);  // record flags
    for (size_t i = 0; i < components.size(); i++) {
        r.push_back(i == 0 ? first_flags : 0x00);
        r.push_back(static_cast<uint8_t>(components[i].size()));
        r.insert(r.end(), components[i].begin(), components[i].end());
    }
    r[2] = static_cast<uint8_t>(r.size());
    return r;
}

// ── Directory record builder ────────────────────────────────────────

struct DirRecordSpec {
    int64_t extent = 0;
    int64_t size = 0;
    uint8_t flags = 0x00;          // 0x02 = directory
    std::string iso_name;          // raw identifier bytes
    std::vector<uint8_t> susp;     // SUSP records after the name
};

std::vector<uint8_t> make_dir_record(const DirRecordSpec& spec) {
    size_t name_len = spec.iso_name.size();
    size_t base = 33 + name_len;
    if (base & 1) base++;
    size_t total = base + spec.susp.size();
    if (total & 1) total++;

    std::vector<uint8_t> r(total, 0);
    r[0] = static_cast<uint8_t>(total);
    r[1] = 0;  // extended attribute length
    put_both32(r, 2, static_cast<uint32_t>(spec.extent));
    put_both32(r, 10, static_cast<uint32_t>(spec.size));
    std::memcpy(r.data() + 18, kDate, 7);
    r[25] = spec.flags;
    put_le16(r, 28, 1);            // volume sequence number (LE)
    put_le16(r, 30, 1);            // ... and BE
    r[32] = static_cast<uint8_t>(name_len);
    std::memcpy(r.data() + 33, spec.iso_name.data(), name_len);
    if (!spec.susp.empty()) {
        std::memcpy(r.data() + base, spec.susp.data(), spec.susp.size());
    }
    return r;
}

// Build a Rock Ridge test image:
//   sector 16: PVD with root record (extent 18, 1 sector)
//   sector 17: volume descriptor terminator
//   sector 18: root        -> rr_moved, placeholder 0x02, top-dir
//   sector 19: rr_moved    -> deep-dir (NM name)
//   sector 20: top-dir     -> readme.txt (NM name)
//   sector 21: deep-dir    -> link (SL symlink)
//   sector 22: file data "DATA"
std::vector<uint8_t> build_rr_image() {
    const int64_t SZ = kSectorSize;

    std::vector<uint8_t> img;
    img.resize((kFile + 1) * 2048, 0);

    // ── sector kRoot: root directory ──
    {
        std::vector<uint8_t> sec(2048, 0);
        size_t off = 0;
        auto put = [&](const std::vector<uint8_t>& rec) {
            std::memcpy(sec.data() + off, rec.data(), rec.size());
            off += rec.size();
        };
        put(make_dir_record({kRoot, SZ, 0x02, std::string(1, '\0')}));  // "."
        put(make_dir_record({kRoot, SZ, 0x02, std::string(1, '\x01')}));  // ".."
        put(make_dir_record({kMoved, SZ, 0x02, "RRMOVE_",
                             susp_nm("rr_moved")}));
        // Placeholder 0x02: deep directory relocated to rr_moved
        put(make_dir_record({0, 0, 0x02, std::string(1, '\x02')}));
        put(make_dir_record({kTop, SZ, 0x02, "TOP____",
                             susp_nm("top-dir")}));
        std::memcpy(img.data() + kRoot * 2048, sec.data(), 2048);
    }

    // ── sector kMoved: rr_moved ──
    {
        std::vector<uint8_t> sec(2048, 0);
        size_t off = 0;
        auto put = [&](const std::vector<uint8_t>& rec) {
            std::memcpy(sec.data() + off, rec.data(), rec.size());
            off += rec.size();
        };
        put(make_dir_record({kMoved, SZ, 0x02, std::string(1, '\0')}));
        put(make_dir_record({kMoved, SZ, 0x02, std::string(1, '\x01')}));
        auto susp = susp_nm("deep-dir");
        auto px = susp_px(0755);
        auto tf = susp_tf();
        susp.insert(susp.end(), px.begin(), px.end());
        susp.insert(susp.end(), tf.begin(), tf.end());
        put(make_dir_record({kDeep, SZ, 0x02, "DEEP___", susp}));
        std::memcpy(img.data() + kMoved * 2048, sec.data(), 2048);
    }

    // ── sector kTop: top-dir (recursion target) ──
    {
        std::vector<uint8_t> sec(2048, 0);
        size_t off = 0;
        auto put = [&](const std::vector<uint8_t>& rec) {
            std::memcpy(sec.data() + off, rec.data(), rec.size());
            off += rec.size();
        };
        put(make_dir_record({kTop, SZ, 0x02, std::string(1, '\0')}));
        put(make_dir_record({kTop, SZ, 0x02, std::string(1, '\x01')}));
        put(make_dir_record({kFile, 4, 0x00, "FILE.TXT;1",
                             susp_nm("readme.txt")}));
        std::memcpy(img.data() + kTop * 2048, sec.data(), 2048);
    }

    // ── sector kDeep: deep-dir (restored via placeholder) ──
    {
        std::vector<uint8_t> sec(2048, 0);
        size_t off = 0;
        auto put = [&](const std::vector<uint8_t>& rec) {
            std::memcpy(sec.data() + off, rec.data(), rec.size());
            off += rec.size();
        };
        put(make_dir_record({kDeep, SZ, 0x02, std::string(1, '\0')}));
        put(make_dir_record({kDeep, SZ, 0x02, std::string(1, '\x01')}));
        auto susp = susp_nm("link");
        auto sl = susp_sl({"bin", "tool"}, 0x08);  // /bin/tool
        susp.insert(susp.end(), sl.begin(), sl.end());
        put(make_dir_record({kFile, 0, 0x00, "LINK____;1", susp}));
        std::memcpy(img.data() + kDeep * 2048, sec.data(), 2048);
    }

    // ── sector kFile: file data ──
    std::memcpy(img.data() + kFile * 2048, "DATA", 4);

    // ── sector kTerm: terminator ──
    {
        std::vector<uint8_t> sec(2048, 0);
        sec[0] = 255;  // Volume Descriptor Set Terminator
        std::memcpy(sec.data() + 1, "CD001", 5);
        sec[6] = 1;
        std::memcpy(img.data() + kTerm * 2048, sec.data(), 2048);
    }

    // ── sector kPvd: PVD with root record ──
    {
        std::vector<uint8_t> sec(2048, 0);
        sec[0] = 1;  // Primary Volume Descriptor
        std::memcpy(sec.data() + 1, "CD001", 5);
        sec[6] = 1;
        // Root directory record at offset 156, with an SP record in its
        // system use area (offset byte 0 => no relocation)
        std::vector<uint8_t> sp = {'S', 'P', 7, 1, 0, 0};
        auto rec = make_dir_record({kRoot, SZ, 0x02, std::string(1, '\0'),
                                    sp});
        std::memcpy(sec.data() + 156, rec.data(), rec.size());
        std::memcpy(img.data() + kPvd * 2048, sec.data(), 2048);
    }

    return img;
}

void write_file(const std::filesystem::path& path,
                const std::vector<uint8_t>& data) {
    std::ofstream f(path, std::ios::binary);
    f.write(reinterpret_cast<const char*>(data.data()),
            static_cast<std::streamsize>(data.size()));
}

std::string temp_db_path(const std::string& name) {
    return (std::filesystem::temp_directory_path() / name).string();
}

void cleanup_db(const std::string& path) {
    std::filesystem::remove(path);
    std::filesystem::remove(path + "-wal");
    std::filesystem::remove(path + "-shm");
}

} // namespace

// ── parse_directory_record: SUSP records ────────────────────────────

TEST(RrParseTest, NmNameOverridesIsoName) {
    std::vector<uint8_t> rec = make_dir_record(
        {21, 4, 0x00, "FILE.TXT;1", susp_nm("readme.txt")});
    IsoEntry e;
    size_t consumed = parse_directory_record(rec.data(), rec.size(), e);
    EXPECT_GT(consumed, 0u);
    EXPECT_TRUE(e.has_rr);
    EXPECT_EQ(e.rr_name, "readme.txt");
    EXPECT_EQ(e.name, "readme.txt");
    EXPECT_FALSE(e.is_directory);
}

TEST(RrParseTest, NmContinuationSegmentsAreJoined) {
    auto s1 = susp_nm("very-long-na");
    auto s2 = susp_nm("me-continued", true);
    s1.insert(s1.end(), s2.begin(), s2.end());
    std::vector<uint8_t> rec = make_dir_record({21, 4, 0x00, "X.;1", s1});
    IsoEntry e;
    size_t consumed = parse_directory_record(rec.data(), rec.size(), e);
    EXPECT_GT(consumed, 0u);
    EXPECT_EQ(e.rr_name, "very-long-name-continued");
    EXPECT_EQ(e.name, "very-long-name-continued");
}

TEST(RrParseTest, PxModeIsLittleEndian) {
    std::vector<uint8_t> rec = make_dir_record(
        {21, 4, 0x00, "X.;1", susp_px(0755)});
    IsoEntry e;
    parse_directory_record(rec.data(), rec.size(), e);
    EXPECT_TRUE(e.has_rr);
    EXPECT_EQ(e.mode, 0755);
}

TEST(RrParseTest, TfTimestampWins) {
    std::vector<uint8_t> rec = make_dir_record(
        {21, 4, 0x00, "X.;1", susp_tf()});
    IsoEntry e;
    parse_directory_record(rec.data(), rec.size(), e);
    EXPECT_EQ(e.mtime, parse_iso_date(kDate));
}

TEST(RrParseTest, SlSymlinkTarget) {
    std::vector<uint8_t> rec = make_dir_record(
        {21, 0, 0x00, "LINK____;1", susp_sl({"bin", "tool"}, 0x08)});
    IsoEntry e;
    parse_directory_record(rec.data(), rec.size(), e);
    EXPECT_TRUE(e.is_symlink);
    EXPECT_EQ(e.rr_link_target, "/bin/tool");
}

TEST(RrParseTest, DeepDirectoryPlaceholderDetected) {
    std::vector<uint8_t> rec = make_dir_record(
        {0, 0, 0x02, std::string(1, '\x02')});
    IsoEntry e;
    parse_directory_record(rec.data(), rec.size(), e);
    EXPECT_TRUE(e.is_directory);
    EXPECT_TRUE(e.is_rr_placeholder);
    EXPECT_EQ(e.rr_placeholder, 2);
    // Display name must not leak the raw control byte
    EXPECT_NE(e.name, std::string(1, '\x02'));
}

// ── Iso9660Parser: directory enumeration with RR names ──────────────

TEST(Iso9660RrTest, ReadRootDirectory) {
    std::filesystem::path img = std::filesystem::temp_directory_path() /
                                "offcat_rr_root.iso";
    write_file(img, build_rr_image());

    {
        Iso9660Parser parser(img.string());
        ASSERT_TRUE(parser.open());

    std::vector<IsoEntry> entries;
    ASSERT_TRUE(parser.read_root_directory(entries));
    ASSERT_EQ(entries.size(), 3u);  // rr_moved, placeholder, top-dir

    bool saw_moved = false, saw_placeholder = false, saw_top = false;
    for (const auto& e : entries) {
        if (e.name == "rr_moved") saw_moved = true;
        if (e.name == "top-dir") saw_top = true;
        if (e.is_rr_placeholder) {
            saw_placeholder = true;
            EXPECT_EQ(e.rr_placeholder, 2);
        }
    }
    EXPECT_TRUE(saw_moved);
    EXPECT_TRUE(saw_placeholder);
    EXPECT_TRUE(saw_top);

    // RR names inside rr_moved / top-dir
    std::vector<IsoEntry> moved;
    ASSERT_TRUE(parser.read_directory(kMoved, kSectorSize, moved));
    ASSERT_EQ(moved.size(), 1u);
    EXPECT_EQ(moved[0].name, "deep-dir");
    EXPECT_EQ(moved[0].mode, 0755);
    EXPECT_GT(moved[0].mtime, 0);

    std::vector<IsoEntry> top;
    ASSERT_TRUE(parser.read_directory(kTop, kSectorSize, top));
    ASSERT_EQ(top.size(), 1u);
    EXPECT_EQ(top[0].name, "readme.txt");

    std::vector<IsoEntry> deep;
    ASSERT_TRUE(parser.read_directory(kDeep, kSectorSize, deep));
    ASSERT_EQ(deep.size(), 1u);
    EXPECT_EQ(deep[0].name, "link");
    EXPECT_TRUE(deep[0].is_symlink);
    EXPECT_EQ(deep[0].rr_link_target, "/bin/tool");
    }  // parser closed before removing the image

    std::filesystem::remove(img);
}

// ── IsoProvider: recursion + rr_moved restoration ───────────────────

namespace {

struct ScanResult {
    std::vector<EntryData> children_of_container;
    std::vector<EntryData> all_children;
};

void scan_rr_image(int max_depth, const std::string& db_path,
                   const std::filesystem::path& img_dir,
                   ScanResult& out) {
    Database db;
    ASSERT_TRUE(is_ok(db.create(db_path)));

    SourceManager sm(db);
    SourceData s;
    s.name = "IMG";
    s.type = SourceType::ISO;
    s.source_path = img_dir.string();
    auto source_id = sm.insert(s);
    ASSERT_TRUE(is_ok(source_id));

    EntryManager em(db);
    EntryData iso_entry;
    iso_entry.source_id = get_ok(source_id);
    iso_entry.name = "test.iso";
    iso_entry.type = EntryType::File;
    auto iso_id = em.insert(iso_entry);
    ASSERT_TRUE(is_ok(iso_id));

    IsoProvider provider;
    ContainerOptions opts;
    opts.max_depth = max_depth;
    ASSERT_TRUE(provider.scan(get_ok(iso_id), db, opts));

    auto children = em.get_children(get_ok(iso_id));
    ASSERT_TRUE(is_ok(children));
    out.children_of_container = get_ok(children);

    auto all = em.get_by_source(get_ok(source_id));
    ASSERT_TRUE(is_ok(all));
    out.all_children = get_ok(all);

    db.close();
}

} // namespace

TEST(IsoProviderRrTest, RestoresPlaceholderAndRecurses) {
    std::string db_path = temp_db_path("offcat_rr_scan.db");
    std::filesystem::remove(db_path);
    std::filesystem::path img_dir =
        std::filesystem::temp_directory_path() / "offcat_rr_img";
    std::filesystem::create_directories(img_dir);
    write_file(img_dir / "test.iso", build_rr_image());

    ScanResult res;
    scan_rr_image(2, db_path, img_dir, res);

    // Root level: rr_moved, deep-dir (restored), top-dir
    ASSERT_EQ(res.children_of_container.size(), 3u);
    std::set<std::string> root_names;
    for (const auto& e : res.children_of_container) root_names.insert(e.name);
    EXPECT_TRUE(root_names.count("rr_moved") == 1);
    EXPECT_TRUE(root_names.count("deep-dir") == 1);   // placeholder restored
    EXPECT_TRUE(root_names.count("top-dir") == 1);
    EXPECT_TRUE(root_names.count("rr_moved/2") == 0); // no temporary name

    // No control-character names anywhere
    for (const auto& e : res.all_children) {
        EXPECT_FALSE(e.name.empty());
        for (unsigned char c : e.name) EXPECT_GE(c, 0x20);
    }

    // Recursion (depth=2): readme.txt under top-dir
    bool found_readme = false, found_link = false, found_link_symlink = false;
    bool found_deep_mode = false;
    for (const auto& e : res.all_children) {
        if (e.name == "readme.txt") {
            found_readme = true;
            EXPECT_EQ(e.type, EntryType::File);
        }
        if (e.name == "link") {
            found_link = true;
            found_link_symlink = (e.type == EntryType::Symlink);
        }
        if (e.name == "deep-dir") {
            found_deep_mode = (e.mode == 0755);
        }
    }
    EXPECT_TRUE(found_readme);
    EXPECT_TRUE(found_link);
    EXPECT_TRUE(found_link_symlink);
    EXPECT_TRUE(found_deep_mode);

    // FTS: virtual entries searchable
    Database db;
    ASSERT_TRUE(is_ok(db.open(db_path)));
    SearchEngine engine(db);
    auto results = engine.search("readme");
    ASSERT_TRUE(is_ok(results));
    bool hit = false;
    for (const auto& r : get_ok(results)) {
        if (r.entry_name == "readme.txt") hit = true;
    }
    EXPECT_TRUE(hit);
    db.close();

    cleanup_db(db_path);
    std::filesystem::remove_all(img_dir);
}

TEST(IsoProviderRrTest, DepthOneDoesNotRecurse) {
    std::string db_path = temp_db_path("offcat_rr_depth1.db");
    std::filesystem::remove(db_path);
    std::filesystem::path img_dir =
        std::filesystem::temp_directory_path() / "offcat_rr_img1";
    std::filesystem::create_directories(img_dir);
    write_file(img_dir / "test.iso", build_rr_image());

    ScanResult res;
    scan_rr_image(1, db_path, img_dir, res);

    // Only the container's direct children are inserted.
    EXPECT_EQ(res.children_of_container.size(), 3u);
    for (const auto& e : res.all_children) {
        EXPECT_TRUE(e.name != "readme.txt" && e.name != "link");
    }

    cleanup_db(db_path);
    std::filesystem::remove_all(img_dir);
}

// ── Scanner: single-file source expands containers ──────────────────

TEST(IsoProviderRrTest, ScannerExpandsSingleFileSource) {
    // Scanner-based expansion goes through ProviderRegistry, which the
    // CLI main() populates; tests must register the provider explicitly.
    register_iso_provider();

    std::string db_path = temp_db_path("offcat_rr_scanner.db");
    std::filesystem::remove(db_path);
    std::filesystem::path img_dir =
        std::filesystem::temp_directory_path() / "offcat_rr_scanfile";
    std::filesystem::create_directories(img_dir);
    write_file(img_dir / "test.iso", build_rr_image());

    Database db;
    ASSERT_TRUE(is_ok(db.create(db_path)));

    CancellationManager cancel;
    Scanner scanner(db, cancel);
    ScanOptions options;
    options.scan_containers = true;
    options.max_container_depth = 2;

    // Scanning the ISO file itself (not a directory containing it)
    auto result = scanner.scan_source((img_dir / "test.iso").string(), options);
    ASSERT_TRUE(is_ok(result)) << get_err(result).message;
    EXPECT_EQ(scanner.files_scanned(), 1);
    EXPECT_EQ(scanner.errors_count(), 0);

    // Container expanded: root children plus recursion
    EntryManager em(db);
    auto entries = em.get_by_source(get_ok(result));
    ASSERT_TRUE(is_ok(entries));
    EXPECT_EQ(get_ok(entries).size(), 7u);  // test.iso + 6 virtual

    bool found_readme = false;
    for (const auto& e : get_ok(entries)) {
        if (e.name == "readme.txt") found_readme = true;
    }
    EXPECT_TRUE(found_readme);

    // Without --containers the ISO is catalogued but not expanded
    std::string db2_path = temp_db_path("offcat_rr_scanner2.db");
    std::filesystem::remove(db2_path);
    Database db2;
    ASSERT_TRUE(is_ok(db2.create(db2_path)));
    CancellationManager cancel2;
    Scanner scanner2(db2, cancel2);
    ScanOptions options2;  // scan_containers = false
    auto result2 = scanner2.scan_source((img_dir / "test.iso").string(), options2);
    ASSERT_TRUE(is_ok(result2));
    auto entries2 = EntryManager(db2).get_by_source(get_ok(result2));
    ASSERT_TRUE(is_ok(entries2));
    EXPECT_EQ(get_ok(entries2).size(), 1u);
    db2.close();

    db.close();
    cleanup_db(db_path);
    cleanup_db(db2_path);
    std::filesystem::remove_all(img_dir);
}
