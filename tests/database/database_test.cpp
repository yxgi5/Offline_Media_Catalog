#include "database/database.h"
#include "catalog/catalog.h"
#include <gtest/gtest.h>
#include <filesystem>
#include <string>

using namespace offcat;

namespace {
std::string temp_db_path(const std::string& name) {
    return (std::filesystem::temp_directory_path() / name).string();
}
}

// ── Database basics ─────────────────────────────────────────────────

TEST(DatabaseTest, CreateAndOpen) {
    std::string path = temp_db_path("offcat_test_create.db");
    std::filesystem::remove(path);

    Database db;
    auto result = db.create(path);
    ASSERT_TRUE(is_ok(result)) << get_err(result).message;
    EXPECT_TRUE(db.is_open());

    // Re-open
    Database db2;
    result = db2.open(path);
    EXPECT_TRUE(is_ok(result));
    db2.close();
    db.close();

    std::filesystem::remove(path);
    // Also remove WAL/SHM files
    std::filesystem::remove(path + "-wal");
    std::filesystem::remove(path + "-shm");
}

TEST(DatabaseTest, WALModeEnabled) {
    std::string path = temp_db_path("offcat_test_wal.db");
    std::filesystem::remove(path);

    Database db;
    auto result = db.create(path);
    ASSERT_TRUE(is_ok(result));

    auto wal_result = db.execute("PRAGMA journal_mode;");
    ASSERT_TRUE(is_ok(wal_result));
    // journal_mode pragma returns a row
    {
        Statement stmt(db, "PRAGMA journal_mode;");
        ASSERT_TRUE(stmt.is_valid());
        EXPECT_TRUE(stmt.step());
        EXPECT_EQ(stmt.column_text(0), "wal");
    }

    db.close();
    std::filesystem::remove(path);
    std::filesystem::remove(path + "-wal");
    std::filesystem::remove(path + "-shm");
}

TEST(DatabaseTest, FTS5Available) {
    std::string path = temp_db_path("offcat_test_fts.db");
    std::filesystem::remove(path);

    Database db;
    ASSERT_TRUE(is_ok(db.create(path)));

    auto result = db.execute(
        "INSERT INTO entry_fts(rowid, name, path, source_name) "
        "VALUES (1, 'test.txt', 'dir/test.txt', 'HDD-001')");
    EXPECT_TRUE(is_ok(result));

    // FTS5 contentless tables (content='') do not store column values,
    // so only the rowid is meaningful in the result row.
    {
        Statement stmt(db, "SELECT rowid FROM entry_fts WHERE entry_fts MATCH 'test'");
        ASSERT_TRUE(stmt.is_valid());
        EXPECT_TRUE(stmt.step());
        EXPECT_EQ(stmt.column_int64(0), 1);
    }

    db.close();
    std::filesystem::remove(path);
    std::filesystem::remove(path + "-wal");
    std::filesystem::remove(path + "-shm");
}

TEST(DatabaseTest, TransactionRollback) {
    std::string path = temp_db_path("offcat_test_txn.db");
    std::filesystem::remove(path);

    Database db;
    ASSERT_TRUE(is_ok(db.create(path)));

    SourceManager sm(db);
    SourceData s;
    s.name = "SRC-1";
    s.type = SourceType::Directory;

    {
        Transaction txn(db);
        auto id = sm.insert(s);
        ASSERT_TRUE(is_ok(id));
        // Deliberately not committed -> rollback on destruction
    }

    EXPECT_EQ(get_ok(sm.count()), 0);

    {
        Transaction txn(db);
        auto id = sm.insert(s);
        ASSERT_TRUE(is_ok(id));
        ASSERT_TRUE(is_ok(txn.commit()));
    }

    EXPECT_EQ(get_ok(sm.count()), 1);

    db.close();
    std::filesystem::remove(path);
    std::filesystem::remove(path + "-wal");
    std::filesystem::remove(path + "-shm");
}

// ── SourceManager ───────────────────────────────────────────────────

TEST(SourceManagerTest, CRUD) {
    std::string path = temp_db_path("offcat_test_source.db");
    std::filesystem::remove(path);

    Database db;
    ASSERT_TRUE(is_ok(db.create(path)));

    SourceManager sm(db);
    SourceData s;
    s.name = "HDD-001";
    s.type = SourceType::Volume;
    s.source_path = "D:\\";
    s.label = "DATA";
    s.serial = "ABC123";
    s.filesystem = "NTFS";
    s.size = 1024LL * 1024 * 1024 * 1024;

    auto id = sm.insert(s);
    ASSERT_TRUE(is_ok(id));
    EXPECT_GT(get_ok(id), 0);

    auto fetched = sm.get_by_id(get_ok(id));
    ASSERT_TRUE(is_ok(fetched));
    EXPECT_EQ(get_ok(fetched).name, "HDD-001");
    EXPECT_EQ(get_ok(fetched).type, SourceType::Volume);
    EXPECT_EQ(get_ok(fetched).label, "DATA");
    EXPECT_EQ(get_ok(fetched).filesystem, "NTFS");

    auto all = sm.get_all();
    ASSERT_TRUE(is_ok(all));
    EXPECT_EQ(get_ok(all).size(), 1);

    db.close();
    std::filesystem::remove(path);
    std::filesystem::remove(path + "-wal");
    std::filesystem::remove(path + "-shm");
}

// ── EntryManager ────────────────────────────────────────────────────

TEST(EntryManagerTest, TreeStructure) {
    std::string path = temp_db_path("offcat_test_entry.db");
    std::filesystem::remove(path);

    Database db;
    ASSERT_TRUE(is_ok(db.create(path)));

    SourceManager sm(db);
    SourceData s;
    s.name = "HDD-001";
    s.type = SourceType::Volume;
    auto source_id = sm.insert(s);
    ASSERT_TRUE(is_ok(source_id));

    EntryManager em(db);

    // Build: Software/Windows.iso/sources/install.wim
    EntryData dir1;
    dir1.source_id = get_ok(source_id);
    dir1.name = "Software";
    dir1.type = EntryType::Directory;
    auto dir1_id = em.insert(dir1);
    ASSERT_TRUE(is_ok(dir1_id));

    EntryData iso;
    iso.source_id = get_ok(source_id);
    iso.parent_id = get_ok(dir1_id);
    iso.name = "Windows.iso";
    iso.type = EntryType::File;
    iso.size = 4LL * 1024 * 1024 * 1024;
    auto iso_id = em.insert(iso);
    ASSERT_TRUE(is_ok(iso_id));

    EntryData dir2;
    dir2.source_id = get_ok(source_id);
    dir2.parent_id = get_ok(iso_id);
    dir2.name = "sources";
    dir2.type = EntryType::Directory;
    auto dir2_id = em.insert(dir2);
    ASSERT_TRUE(is_ok(dir2_id));

    EntryData wim;
    wim.source_id = get_ok(source_id);
    wim.parent_id = get_ok(dir2_id);
    wim.name = "install.wim";
    wim.type = EntryType::File;
    wim.size = 3LL * 1024 * 1024 * 1024;
    auto wim_id = em.insert(wim);
    ASSERT_TRUE(is_ok(wim_id));

    // Build path: parent_id chain
    auto built_path = em.build_path(get_ok(wim_id));
    ASSERT_TRUE(is_ok(built_path));
    EXPECT_EQ(get_ok(built_path), "Software/Windows.iso/sources/install.wim");

    // Children query
    auto children = em.get_children(get_ok(dir1_id));
    ASSERT_TRUE(is_ok(children));
    ASSERT_EQ(get_ok(children).size(), 1);
    EXPECT_EQ(get_ok(children)[0].name, "Windows.iso");

    // Count
    auto count = em.count();
    ASSERT_TRUE(is_ok(count));
    EXPECT_EQ(get_ok(count), 4);

    db.close();
    std::filesystem::remove(path);
    std::filesystem::remove(path + "-wal");
    std::filesystem::remove(path + "-shm");
}

TEST(EntryManagerTest, UnicodeNames) {
    std::string path = temp_db_path("offcat_test_unicode.db");
    std::filesystem::remove(path);

    Database db;
    ASSERT_TRUE(is_ok(db.create(path)));

    SourceManager sm(db);
    SourceData s;
    s.name = "测试盘";
    s.type = SourceType::Directory;
    auto source_id = sm.insert(s);
    ASSERT_TRUE(is_ok(source_id));

    EntryManager em(db);
    EntryData e;
    e.source_id = get_ok(source_id);
    e.name = "中文目录/日本語/😀emoji";
    e.type = EntryType::Directory;

    auto id = em.insert(e);
    ASSERT_TRUE(is_ok(id));

    auto fetched = em.get_by_id(get_ok(id));
    ASSERT_TRUE(is_ok(fetched));
    EXPECT_EQ(get_ok(fetched).name, "中文目录/日本語/😀emoji");

    db.close();
    std::filesystem::remove(path);
    std::filesystem::remove(path + "-wal");
    std::filesystem::remove(path + "-shm");
}

// ── Batch insert performance ────────────────────────────────────────

TEST(EntryManagerTest, BatchInsert10k) {
    std::string path = temp_db_path("offcat_test_batch.db");
    std::filesystem::remove(path);

    Database db;
    ASSERT_TRUE(is_ok(db.create(path)));

    SourceManager sm(db);
    SourceData s;
    s.name = "BATCH";
    s.type = SourceType::Directory;
    auto source_id = sm.insert(s);
    ASSERT_TRUE(is_ok(source_id));

    EntryManager em(db);
    Transaction txn(db);

    std::vector<EntryData> batch;
    batch.reserve(1000);
    for (int i = 0; i < 10000; i++) {
        EntryData e;
        e.source_id = get_ok(source_id);
        e.name = "file_" + std::to_string(i) + ".bin";
        e.type = EntryType::File;
        e.size = i * 100;
        batch.push_back(e);

        if (batch.size() == 1000) {
            auto result = em.insert_batch(batch);
            ASSERT_TRUE(is_ok(result));
            batch.clear();
        }
    }
    if (!batch.empty()) {
        ASSERT_TRUE(is_ok(em.insert_batch(batch)));
    }
    ASSERT_TRUE(is_ok(txn.commit()));

    auto count = em.count();
    ASSERT_TRUE(is_ok(count));
    EXPECT_EQ(get_ok(count), 10000);

    db.close();
    std::filesystem::remove(path);
    std::filesystem::remove(path + "-wal");
    std::filesystem::remove(path + "-shm");
}

// ── ChecksumManager ─────────────────────────────────────────────────

TEST(ChecksumManagerTest, InsertAndGet) {
    std::string path = temp_db_path("offcat_test_csmgr.db");
    std::filesystem::remove(path);

    Database db;
    ASSERT_TRUE(is_ok(db.create(path)));

    SourceManager sm(db);
    SourceData s;
    s.name = "SRC";
    s.type = SourceType::Directory;
    auto source_id = sm.insert(s);
    ASSERT_TRUE(is_ok(source_id));

    EntryManager em(db);
    EntryData e;
    e.source_id = get_ok(source_id);
    e.name = "a.bin";
    e.type = EntryType::File;
    auto entry_id = em.insert(e);
    ASSERT_TRUE(is_ok(entry_id));

    ChecksumManager cm(db);
    ChecksumData c;
    c.entry_id = get_ok(entry_id);
    c.algorithm = ChecksumAlgorithm::CRC32;
    c.value = {0xCB, 0xF4, 0x39, 0x26};
    c.calculated_at = 12345;
    ASSERT_TRUE(is_ok(cm.insert(c)));

    auto fetched = cm.get(get_ok(entry_id), ChecksumAlgorithm::CRC32);
    ASSERT_TRUE(is_ok(fetched));
    EXPECT_EQ(get_ok(fetched).value.size(), 4u);
    EXPECT_EQ(get_ok(fetched).value[0], 0xCB);
    EXPECT_EQ(get_ok(fetched).calculated_at, 12345);

    db.close();
    std::filesystem::remove(path);
    std::filesystem::remove(path + "-wal");
    std::filesystem::remove(path + "-shm");
}

// ── ScanManager ─────────────────────────────────────────────────────

TEST(ScanManagerTest, InsertAndFinish) {
    std::string path = temp_db_path("offcat_test_scanmgr.db");
    std::filesystem::remove(path);

    Database db;
    ASSERT_TRUE(is_ok(db.create(path)));

    SourceManager sm(db);
    SourceData s;
    s.name = "SRC";
    s.type = SourceType::Directory;
    auto source_id = sm.insert(s);
    ASSERT_TRUE(is_ok(source_id));

    ScanManager scan_mgr(db);
    ScanData scan;
    scan.source_id = get_ok(source_id);
    scan.started_at = 1000;
    scan.scanner_version = "0.1.0";
    scan.options = "{\"checksum\":[\"crc32\"],\"containers\":false}";
    scan.status = ScanStatus::InProgress;

    auto scan_id = scan_mgr.insert(scan);
    ASSERT_TRUE(is_ok(scan_id));

    ASSERT_TRUE(is_ok(scan_mgr.finish(get_ok(scan_id), ScanStatus::Completed)));

    auto fetched = scan_mgr.get_by_id(get_ok(scan_id));
    ASSERT_TRUE(is_ok(fetched));
    EXPECT_EQ(get_ok(fetched).status, ScanStatus::Completed);
    EXPECT_GT(get_ok(fetched).finished_at, 0);

    db.close();
    std::filesystem::remove(path);
    std::filesystem::remove(path + "-wal");
    std::filesystem::remove(path + "-shm");
}

TEST(DatabaseTest, OptimizeFts) {
    std::string path = temp_db_path("offcat_test_ftsopt.db");
    std::filesystem::remove(path);

    Database db;
    ASSERT_TRUE(is_ok(db.create(path)));

    // Seed a source, one entry and its FTS row.
    ASSERT_TRUE(is_ok(db.execute(
        "INSERT INTO source (id, name, type) VALUES (1, 'SRC', 'Directory');"
        "INSERT INTO entry (id, source_id, parent_id, name, type) VALUES"
        " (1, 1, NULL, 'alpha.txt', 1), (2, 1, 1, 'beta.txt', 1);"
        "INSERT INTO entry_fts(rowid, name, path, source_name) VALUES"
        " (1, 'alpha.txt', 'alpha.txt', 'SRC'),"
        " (2, 'beta.txt', 'alpha.txt/beta.txt', 'SRC');")));

    // optimize must succeed and keep all index rows.
    EntryManager em(db);
    ASSERT_TRUE(is_ok(em.optimize_fts()));
    {
        Statement stmt(db, "SELECT COUNT(*) FROM entry_fts");
        ASSERT_TRUE(stmt.is_valid());
        ASSERT_TRUE(stmt.step());
        EXPECT_EQ(stmt.column_int64(0), 2);
    }

    db.close();
    std::filesystem::remove(path);
    std::filesystem::remove(path + "-wal");
    std::filesystem::remove(path + "-shm");
}

TEST(DatabaseTest, MigratesContentlessFts) {
    std::string path = temp_db_path("offcat_test_ftsmig.db");
    std::filesystem::remove(path);

    Database db;
    ASSERT_TRUE(is_ok(db.create(path)));

    // Simulate an old catalog: contentless FTS index with existing rows.
    ASSERT_TRUE(is_ok(db.execute(
        "DROP TABLE entry_fts;"
        "CREATE VIRTUAL TABLE entry_fts USING fts5("
        " name, path, source_name, content='', tokenize='unicode61');"
        "INSERT INTO source (id, name, type) VALUES (1, 'SRC', 'Directory');"
        "INSERT INTO entry (id, source_id, parent_id, name, type) VALUES"
        " (1, 1, NULL, 'root.txt', 1), (2, 1, 1, 'sub.txt', 1);"
        "INSERT INTO entry_fts(rowid, name, path, source_name) VALUES"
        " (1, 'root.txt', 'root.txt', 'SRC'),"
        " (2, 'sub.txt', 'root.txt/sub.txt', 'SRC');")));

    // Re-initialize: the contentless index must be replaced and rebuilt
    // from the entry table.
    auto init = db.initialize_schema();
    ASSERT_TRUE(is_ok(init)) << get_err(init).message;

    // The new index is a regular fts5 table, so rowid DELETE works.
    ASSERT_TRUE(is_ok(db.execute("DELETE FROM entry_fts WHERE rowid = 1;")));

    // Both entries were rebuilt; one remains after the delete.
    {
        Statement stmt(db, "SELECT COUNT(*) FROM entry_fts");
        ASSERT_TRUE(stmt.is_valid());
        ASSERT_TRUE(stmt.step());
        EXPECT_EQ(stmt.column_int64(0), 1);
    }

    db.close();
    std::filesystem::remove(path);
    std::filesystem::remove(path + "-wal");
    std::filesystem::remove(path + "-shm");
}
