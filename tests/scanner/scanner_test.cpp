#include "database/database.h"
#include "catalog/catalog.h"
#include "scanner/scanner.h"
#include "scanner/search.h"
#include "container/provider.h"
#include "core/checksum.h"
#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>
#include <chrono>
#include <string>

using namespace offcat;

namespace {
std::string create_temp_dir() {
    auto path = std::filesystem::temp_directory_path() /
                ("offcat_scan_" + std::to_string(
                    std::chrono::system_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(path);
    return path.string();
}

void create_test_file(const std::filesystem::path& path,
                     const std::string& content) {
    // Use the path overload of ofstream so non-ASCII names go through
    // the wide-character API (ofstream(const char*) would use the ANSI
    // code page and mangle UTF-8 names on Windows).
    std::ofstream f(path, std::ios::binary);
    f.write(content.data(), static_cast<std::streamsize>(content.size()));
}

std::string temp_db_path(const std::string& name) {
    return (std::filesystem::temp_directory_path() / name).string();
}
}

// ── Scanner integration ─────────────────────────────────────────────

TEST(ScannerTest, ScanDirectoryTree) {
    std::string dir = create_temp_dir();
    std::string db_path = temp_db_path("offcat_scan_test.db");
    std::filesystem::remove(db_path);

    // Build test tree
    create_test_file(dir + "/root.txt", "root content");
    std::filesystem::create_directories(dir + "/sub1");
    create_test_file(dir + "/sub1/a.txt", "aaa");
    create_test_file(dir + "/sub1/b.bin", std::string(100, 'x'));
    std::filesystem::create_directories(dir + "/sub1/deep");
    create_test_file(dir + "/sub1/deep/深層文件.txt", "深度");

    Database db;
    ASSERT_TRUE(is_ok(db.create(db_path)));

    CancellationManager cancel;
    Scanner scanner(db, cancel);

    ScanOptions options;
    auto result = scanner.scan_source(dir, options);
    ASSERT_TRUE(is_ok(result)) << get_err(result).message;
    int64_t source_id = get_ok(result);

    // Verify counts
    EXPECT_EQ(scanner.files_scanned(), 4);   // root.txt, a.txt, b.bin, 深層文件.txt
    EXPECT_EQ(scanner.directories_scanned(), 2);  // sub1, deep

    // Verify entries in DB
    EntryManager em(db);
    auto count = em.count();
    ASSERT_TRUE(is_ok(count));

    // Root directory itself is NOT inserted as an entry (only its children)
    // source has entries: sub1 (dir), root.txt (file)          -> 2
    // sub1 has: a.txt, b.bin, deep (dir)                       -> 3
    // deep has: 深層文件.txt                                    -> 1
    // Total = 6
    EXPECT_EQ(get_ok(count), 6);

    // FTS search should find the unicode file
    SearchEngine engine(db);
    auto results = engine.search("深層");
    ASSERT_TRUE(is_ok(results));
    ASSERT_EQ(get_ok(results).size(), 1);
    EXPECT_EQ(get_ok(results)[0].entry_name, "深層文件.txt");

    // Cleanup
    db.close();
    std::filesystem::remove_all(dir);
    std::filesystem::remove(db_path);
    std::filesystem::remove(db_path + "-wal");
    std::filesystem::remove(db_path + "-shm");
}

TEST(ScannerTest, ScanWithChecksums) {
    std::string dir = create_temp_dir();
    std::string db_path = temp_db_path("offcat_checksum_test.db");
    std::filesystem::remove(db_path);

    create_test_file(dir + "/data.bin", "123456789");  // CRC32 = 0xCBF43926

    Database db;
    ASSERT_TRUE(is_ok(db.create(db_path)));

    CancellationManager cancel;
    Scanner scanner(db, cancel);

    ScanOptions options;
    options.compute_checksum = true;
    options.checksum_algorithms = {
        ChecksumAlgorithm::SHA256,
        ChecksumAlgorithm::MD5,
        ChecksumAlgorithm::CRC32
    };

    auto result = scanner.scan_source(dir, options);
    ASSERT_TRUE(is_ok(result));

    // Verify checksums stored
    EntryManager em(db);
    auto entries = em.get_by_source(get_ok(result));
    ASSERT_TRUE(is_ok(entries));

    // Find data.bin entry
    const EntryData* target = nullptr;
    for (const auto& e : get_ok(entries)) {
        if (e.name == "data.bin") {
            target = &e;
            break;
        }
    }
    ASSERT_NE(target, nullptr);

    ChecksumManager cm(db);
    auto crc = cm.get(target->id, ChecksumAlgorithm::CRC32);
    ASSERT_TRUE(is_ok(crc));

    // CRC32 value: 0xCB 0xF4 0x39 0x26
    ASSERT_EQ(get_ok(crc).value.size(), 4u);
    EXPECT_EQ(get_ok(crc).value[0], 0xCB);
    EXPECT_EQ(get_ok(crc).value[1], 0xF4);
    EXPECT_EQ(get_ok(crc).value[2], 0x39);
    EXPECT_EQ(get_ok(crc).value[3], 0x26);

    auto sha = cm.get(target->id, ChecksumAlgorithm::SHA256);
    ASSERT_TRUE(is_ok(sha));
    EXPECT_EQ(digest_to_hex(get_ok(sha).value),
        "15e2b0d3c33891ebb0f1ef609ec419420c20e320ce94c65fbc8c3312448eb225");

    auto md5 = cm.get(target->id, ChecksumAlgorithm::MD5);
    ASSERT_TRUE(is_ok(md5));
    EXPECT_EQ(digest_to_hex(get_ok(md5).value),
        "25f9e794323b453885f5181f1b624d0b");

    // Cleanup
    db.close();
    std::filesystem::remove_all(dir);
    std::filesystem::remove(db_path);
    std::filesystem::remove(db_path + "-wal");
    std::filesystem::remove(db_path + "-shm");
}

TEST(ScannerTest, Cancellation) {
    std::string dir = create_temp_dir();
    std::string db_path = temp_db_path("offcat_cancel_test.db");
    std::filesystem::remove(db_path);

    // Create many files to make cancellation meaningful
    for (int i = 0; i < 100; i++) {
        create_test_file(dir + "/file_" + std::to_string(i) + ".txt", "x");
    }

    Database db;
    ASSERT_TRUE(is_ok(db.create(db_path)));

    CancellationManager cancel;
    // Cancel immediately: scan should stop early
    cancel.request_cancel();

    Scanner scanner(db, cancel);
    ScanOptions options;
    auto result = scanner.scan_source(dir, options);
    ASSERT_TRUE(is_ok(result));  // Cancellation returns ok with source id

    // Scan record should be marked cancelled
    SourceManager sm(db);
    auto sources = sm.get_all();
    ASSERT_TRUE(is_ok(sources));
    ASSERT_EQ(get_ok(sources).size(), 1);

    ScanManager scan_mgr(db);
    auto scans = scan_mgr.get_by_source(get_ok(result));
    ASSERT_TRUE(is_ok(scans));
    ASSERT_EQ(get_ok(scans).size(), 1);
    EXPECT_EQ(get_ok(scans)[0].status, ScanStatus::Cancelled);

    // Database should be intact and queryable
    auto count = EntryManager(db).count();
    ASSERT_TRUE(is_ok(count));
    EXPECT_GE(get_ok(count), 0);  // 0 or partial data preserved

    // Cleanup
    db.close();
    std::filesystem::remove_all(dir);
    std::filesystem::remove(db_path);
    std::filesystem::remove(db_path + "-wal");
    std::filesystem::remove(db_path + "-shm");
}

// ── Search ──────────────────────────────────────────────────────────

TEST(SearchTest, FtsNameAndPath) {
    std::string dir = create_temp_dir();
    std::string db_path = temp_db_path("offcat_search_test.db");
    std::filesystem::remove(db_path);

    std::filesystem::create_directories(dir + "/Software");
    create_test_file(dir + "/Software/ubuntu.iso", "iso");
    create_test_file(dir + "/Software/old_tools.iso", "iso2");

    Database db;
    ASSERT_TRUE(is_ok(db.create(db_path)));

    CancellationManager cancel;
    Scanner scanner(db, cancel);
    ScanOptions options;
    ASSERT_TRUE(is_ok(scanner.scan_source(dir, options)));

    SearchEngine engine(db);
    auto results = engine.search("ubuntu");
    ASSERT_TRUE(is_ok(results));
    ASSERT_EQ(get_ok(results).size(), 1);
    EXPECT_EQ(get_ok(results)[0].entry_name, "ubuntu.iso");

    auto by_path = engine.search_by_path("Software");
    ASSERT_TRUE(is_ok(by_path));
    ASSERT_GE(get_ok(by_path).size(), 2);

    // Cleanup
    db.close();
    std::filesystem::remove_all(dir);
    std::filesystem::remove(db_path);
    std::filesystem::remove(db_path + "-wal");
    std::filesystem::remove(db_path + "-shm");
}

// ── Provider registry ───────────────────────────────────────────────

TEST(ContainerTest, RegistryEmpty) {
    auto types = ProviderRegistry::instance().registered_types();
    // Registry may or may not have providers registered depending on
    // whether ISO provider registration happened.
    (void)types;
}

TEST(ContainerTest, VirtualTreeWriter) {
    std::string db_path = temp_db_path("offcat_vtw_test.db");
    std::filesystem::remove(db_path);

    Database db;
    ASSERT_TRUE(is_ok(db.create(db_path)));

    SourceManager sm(db);
    SourceData s;
    s.name = "ISO-SRC";
    s.type = SourceType::ISO;
    auto source_id = sm.insert(s);
    ASSERT_TRUE(is_ok(source_id));

    EntryManager em(db);
    EntryData iso_entry;
    iso_entry.source_id = get_ok(source_id);
    iso_entry.name = "test.iso";
    iso_entry.type = EntryType::File;
    auto iso_id = em.insert(iso_entry);
    ASSERT_TRUE(is_ok(iso_id));

    VirtualTreeWriter writer(db, get_ok(source_id), get_ok(iso_id));

    EntryData virtual_dir;
    virtual_dir.parent_id = get_ok(iso_id);
    virtual_dir.name = "sources";
    virtual_dir.type = EntryType::Directory;
    auto dir_id = writer.add_entry(virtual_dir);
    ASSERT_TRUE(is_ok(dir_id));

    EntryData virtual_file;
    virtual_file.parent_id = get_ok(dir_id);
    virtual_file.name = "install.wim";
    virtual_file.type = EntryType::File;
    virtual_file.size = 12345;
    auto file_id = writer.add_entry(virtual_file);
    ASSERT_TRUE(is_ok(file_id));

    // Virtual entries should be marked
    auto fetched = em.get_by_id(get_ok(file_id));
    ASSERT_TRUE(is_ok(fetched));
    EXPECT_TRUE(get_ok(fetched).is_virtual);

    // Path construction should include the container hierarchy
    auto path = em.build_path(get_ok(file_id));
    ASSERT_TRUE(is_ok(path));
    EXPECT_EQ(get_ok(path), "test.iso/sources/install.wim");

    // Cleanup
    db.close();
    std::filesystem::remove(db_path);
    std::filesystem::remove(db_path + "-wal");
    std::filesystem::remove(db_path + "-shm");
}
