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
#include <thread>

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

#ifdef _WIN32
    // Creation time is recorded by default; Windows always provides it.
    auto entries = em.get_by_source(source_id);
    ASSERT_TRUE(is_ok(entries));
    bool saw_file = false;
    for (const auto& e : get_ok(entries)) {
        if (e.type == EntryType::File) {
            saw_file = true;
            EXPECT_GT(e.birthtime, 0) << "birthtime missing for " << e.name;
        }
    }
    EXPECT_TRUE(saw_file);
#endif

    // Cleanup
    db.close();
    std::filesystem::remove_all(dir);
    std::filesystem::remove(db_path);
    std::filesystem::remove(db_path + "-wal");
    std::filesystem::remove(db_path + "-shm");
}

// The summary counter that feeds the CLI's "were not content-probed"
// hint: with probing off, every file >= 1 MiB without a .iso extension
// is counted (renamed images are otherwise skipped silently); with
// probing on, nothing is left unprobed.
TEST(ScannerTest, UnprobedLargeFileCount) {
    std::string dir = create_temp_dir();
    std::string db_path = temp_db_path("offcat_unprobed_test.db");
    std::filesystem::remove(db_path);

    create_test_file(dir + "/movie.bin", std::string(1 << 20, 'x'));
    create_test_file(dir + "/note.txt", "small");
    create_test_file(dir + "/real.iso", std::string(2048, '\0'));

    Database db;
    ASSERT_TRUE(is_ok(db.create(db_path)));

    CancellationManager cancel;
    Scanner scanner(db, cancel);

    // Default options: probing off -> exactly one large unprobed file.
    ScanOptions options;
    auto result = scanner.scan_source(dir, options);
    ASSERT_TRUE(is_ok(result)) << get_err(result).message;
    EXPECT_EQ(scanner.large_unprobed_files(), 1);

    // With probing enabled nothing is "left unprobed"; the counter
    // resets at the start of each scan.
    options.probe_containers = true;
    result = scanner.scan_source(dir, options);
    ASSERT_TRUE(is_ok(result)) << get_err(result).message;
    EXPECT_EQ(scanner.large_unprobed_files(), 0);

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

TEST(ScannerTest, RescanReplacesEntries) {
    std::string dir = create_temp_dir();
    std::string db_path = temp_db_path("offcat_rescan_test.db");
    std::filesystem::remove(db_path);

    create_test_file(dir + "/keep.txt", "keep");
    create_test_file(dir + "/remove_me.txt", "bye");

    Database db;
    ASSERT_TRUE(is_ok(db.create(db_path)));

    CancellationManager cancel;
    Scanner scanner(db, cancel);
    ScanOptions options;

    // First scan
    ASSERT_TRUE(is_ok(scanner.scan_source(dir, options)));
    EXPECT_EQ(scanner.files_scanned(), 2);

    SourceManager sm(db);
    EntryManager em(db);

    // Rescan of the same path must replace, not duplicate
    ASSERT_TRUE(is_ok(scanner.scan_source(dir, options)));
    auto sources = sm.get_all();
    ASSERT_TRUE(is_ok(sources));
    ASSERT_EQ(get_ok(sources).size(), 1);
    auto entries = em.get_by_source(get_ok(sources)[0].id);
    ASSERT_TRUE(is_ok(entries));
    ASSERT_EQ(get_ok(entries).size(), 2);

    // Remove a file and rescan: the stale entry must disappear
    std::filesystem::remove(dir + "/remove_me.txt");
    ASSERT_TRUE(is_ok(scanner.scan_source(dir, options)));
    auto sources2 = sm.get_all();
    ASSERT_TRUE(is_ok(sources2));
    ASSERT_EQ(get_ok(sources2).size(), 1);
    auto entries2 = em.get_by_source(get_ok(sources2)[0].id);
    ASSERT_TRUE(is_ok(entries2));
    ASSERT_EQ(get_ok(entries2).size(), 1);
    EXPECT_EQ(get_ok(entries2)[0].name, "keep.txt");

    // FTS index must not retain rows of the removed entry
    SearchEngine engine(db);
    auto results = engine.search("remove_me");
    ASSERT_TRUE(is_ok(results));
    EXPECT_EQ(get_ok(results).size(), 0);

    // Cleanup
    db.close();
    std::filesystem::remove_all(dir);
    std::filesystem::remove(db_path);
    std::filesystem::remove(db_path + "-wal");
    std::filesystem::remove(db_path + "-shm");
}

TEST(ScannerTest, BatchedScanLargeTree) {
    std::string dir = create_temp_dir();
    std::string db_path = temp_db_path("offcat_batch_test.db");
    std::filesystem::remove(db_path);

    // 2500 files: crosses two batch checkpoints (1000, 2000) plus tail
    for (int i = 0; i < 2500; i++) {
        create_test_file(dir + "/f" + std::to_string(i) + ".txt", "x");
    }

    Database db;
    ASSERT_TRUE(is_ok(db.create(db_path)));

    CancellationManager cancel;
    Scanner scanner(db, cancel);
    ScanOptions options;

    // First scan: batched commits must not lose or duplicate entries
    ASSERT_TRUE(is_ok(scanner.scan_source(dir, options)));
    EXPECT_EQ(scanner.files_scanned(), 2500);

    SourceManager sm(db);
    EntryManager em(db);
    auto sources = sm.get_all();
    ASSERT_TRUE(is_ok(sources));
    ASSERT_EQ(get_ok(sources).size(), 1);
    auto entries = em.get_by_source(get_ok(sources)[0].id);
    ASSERT_TRUE(is_ok(entries));
    ASSERT_EQ(get_ok(entries).size(), 2500);

    // FTS search still works across batch boundaries
    SearchEngine engine(db);
    auto results = engine.search("f2499");
    ASSERT_TRUE(is_ok(results));
    ASSERT_GE(get_ok(results).size(), 1);
    EXPECT_EQ(get_ok(results)[0].entry_name, "f2499.txt");

    // Rescan: 2500 rows removed inside the switch transaction, then
    // re-inserted; the catalog must still hold exactly one source
    ASSERT_TRUE(is_ok(scanner.scan_source(dir, options)));
    auto sources2 = sm.get_all();
    ASSERT_TRUE(is_ok(sources2));
    ASSERT_EQ(get_ok(sources2).size(), 1);
    auto entries2 = em.get_by_source(get_ok(sources2)[0].id);
    ASSERT_TRUE(is_ok(entries2));
    ASSERT_EQ(get_ok(entries2).size(), 2500);

    db.close();
    std::filesystem::remove_all(dir);
    std::filesystem::remove(db_path);
    std::filesystem::remove(db_path + "-wal");
    std::filesystem::remove(db_path + "-shm");
}

TEST(ScannerTest, CancelledRescanPreservesOldData) {
    std::string dir = create_temp_dir();
    std::string db_path = temp_db_path("offcat_cancel_rescan_test.db");
    std::filesystem::remove(db_path);

    create_test_file(dir + "/keep.txt", "keep");
    create_test_file(dir + "/old.txt", "old");

    Database db;
    ASSERT_TRUE(is_ok(db.create(db_path)));

    CancellationManager cancel;
    Scanner scanner(db, cancel);
    ScanOptions options;

    // Baseline scan
    ASSERT_TRUE(is_ok(scanner.scan_source(dir, options)));

    SourceManager sm(db);
    auto sources = sm.get_all();
    ASSERT_TRUE(is_ok(sources));
    ASSERT_EQ(get_ok(sources).size(), 1);
    int64_t old_id = get_ok(sources)[0].id;

    // Cancel before the rescan: the scan stops before any entry is
    // committed, the shadow source is dropped and the previous data
    // stays untouched.
    cancel.request_cancel();
    auto result = scanner.scan_source(dir, options);
    ASSERT_TRUE(is_err(result));
    EXPECT_EQ(get_err(result).code, 0);  // cancellation, not failure

    auto sources2 = sm.get_all();
    ASSERT_TRUE(is_ok(sources2));
    ASSERT_EQ(get_ok(sources2).size(), 1);  // shadow source dropped
    EXPECT_EQ(get_ok(sources2)[0].id, old_id);  // same source id

    auto entries = EntryManager(db).get_by_source(old_id);
    ASSERT_TRUE(is_ok(entries));
    ASSERT_EQ(get_ok(entries).size(), 2);  // all previous entries intact

    db.close();
    std::filesystem::remove_all(dir);
    std::filesystem::remove(db_path);
    std::filesystem::remove(db_path + "-wal");
    std::filesystem::remove(db_path + "-shm");
}

TEST(ScannerTest, CancelledRescanAfterCommittedBatches) {
    std::string dir = create_temp_dir();
    std::string db_path = temp_db_path("offcat_cancel_partial_test.db");
    std::filesystem::remove(db_path);

    create_test_file(dir + "/keep.txt", "keep");

    Database db;
    ASSERT_TRUE(is_ok(db.create(db_path)));

    CancellationManager cancel;
    Scanner scanner(db, cancel);
    ScanOptions options;

    // Baseline scan
    ASSERT_TRUE(is_ok(scanner.scan_source(dir, options)));

    SourceManager sm(db);
    auto sources = sm.get_all();
    ASSERT_TRUE(is_ok(sources));
    ASSERT_EQ(get_ok(sources).size(), 1);
    int64_t old_id = get_ok(sources)[0].id;
    auto old_entries = EntryManager(db).get_by_source(old_id);
    ASSERT_TRUE(is_ok(old_entries));
    size_t old_count = get_ok(old_entries).size();

    // Enough files that the rescan crosses at least one batch checkpoint
    // (BATCH_SIZE = 1000) before it can be cancelled.
    for (int i = 0; i < 2500; i++) {
        create_test_file(dir + "/f" + std::to_string(i) + ".txt", "x");
    }

    // Run the rescan on a worker thread; cancel it once at least one
    // batch has been committed.  A second connection observes committed
    // batches because WAL-mode readers see committed data.
    std::thread worker([&]() {
        // Cancellation is asserted through the database state after
        // join, not through the worker's return value.
        (void)scanner.scan_source(dir, options);
    });

    Database observer;
    ASSERT_TRUE(is_ok(observer.open(db_path)));
    int64_t committed = 0;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (committed < static_cast<int64_t>(old_count) + 1000 &&
           std::chrono::steady_clock::now() < deadline) {
        auto count = EntryManager(observer).count();
        if (is_ok(count)) committed = get_ok(count);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    EXPECT_GE(committed, static_cast<int64_t>(old_count) + 1000)
        << "rescan never reached a batch checkpoint";
    cancel.request_cancel();
    worker.join();
    observer.close();

    // The shadow tree (committed batches included) must be gone and the
    // old tree must be exactly as it was.
    auto sources2 = sm.get_all();
    ASSERT_TRUE(is_ok(sources2));
    ASSERT_EQ(get_ok(sources2).size(), 1);
    EXPECT_EQ(get_ok(sources2)[0].id, old_id);

    auto entries2 = EntryManager(db).get_by_source(old_id);
    ASSERT_TRUE(is_ok(entries2));
    ASSERT_EQ(get_ok(entries2).size(), old_count);

    db.close();
    std::filesystem::remove_all(dir);
    std::filesystem::remove(db_path);
    std::filesystem::remove(db_path + "-wal");
    std::filesystem::remove(db_path + "-shm");
}

TEST(ScannerTest, OrphanedScanRecoveredAtNextScan) {
    std::string dir = create_temp_dir();
    std::string db_path = temp_db_path("offcat_orphan_test.db");
    std::filesystem::remove(db_path);
    create_test_file(dir + "/a.txt", "a");

    Database db;
    ASSERT_TRUE(is_ok(db.create(db_path)));

    // Simulate crash leftovers: two sources whose scan rows are still
    // InProgress (batched scans killed between checkpoints).  Two orphans
    // exercise the multi-row cleanup path — the SELECT cursor must not
    // skip rows while the cleanup deletes them.
    SourceManager sm(db);
    for (int i = 0; i < 2; ++i) {
        SourceData source;
        source.name = "orphan" + std::to_string(i);
        source.type = SourceType::Directory;
        source.source_path = dir;
        auto src_result = sm.insert(source);
        ASSERT_TRUE(is_ok(src_result));
        ScanData scan;
        scan.source_id = get_ok(src_result);
        scan.status = ScanStatus::InProgress;
        auto scan_result = ScanManager(db).insert(scan);
        ASSERT_TRUE(is_ok(scan_result));
    }

    // The next scan must clean the orphan and proceed normally.
    CancellationManager cancel;
    Scanner scanner(db, cancel);
    ScanOptions options;
    auto result = scanner.scan_source(dir, options);
    ASSERT_TRUE(is_ok(result)) << get_err(result).message;

    auto sources = sm.get_all();
    ASSERT_TRUE(is_ok(sources));
    ASSERT_EQ(get_ok(sources).size(), 1);  // orphans removed
    // The fresh scan row is Completed; had the orphans survived there
    // would be three scan rows for this source (InProgress + Completed).
    // (SQLite may reuse the orphan's rowid, so the ids are not compared.)
    int64_t new_id = get_ok(sources)[0].id;
    auto scans = ScanManager(db).get_by_source(new_id);
    ASSERT_TRUE(is_ok(scans));
    ASSERT_EQ(get_ok(scans).size(), 1);
    EXPECT_EQ(get_ok(scans)[0].status, ScanStatus::Completed);

    auto entries = EntryManager(db).get_by_source(new_id);
    ASSERT_TRUE(is_ok(entries));
    ASSERT_EQ(get_ok(entries).size(), 1);
    EXPECT_EQ(get_ok(entries)[0].name, "a.txt");

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
