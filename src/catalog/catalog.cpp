#include "catalog/catalog.h"
#include "core/logger.h"
#include <chrono>

namespace offcat {

// ── SourceManager ───────────────────────────────────────────────────

SourceManager::SourceManager(Database& db) : db_(db) {}

Result<int64_t> SourceManager::insert(const SourceData& source) {
    Statement stmt(db_,
        "INSERT INTO source (name, type, source_path, label, serial, filesystem, "
        "size, created_at, cataloged_at) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)");
    if (!stmt.is_valid()) return Error{1, "Failed to prepare source insert"};

    stmt.bind_text(1, source.name);
    stmt.bind_text(2, source_type_to_string(source.type));
    if (source.source_path.empty()) stmt.bind_null(3);
    else stmt.bind_text(3, source.source_path);
    if (source.label.empty()) stmt.bind_null(4);
    else stmt.bind_text(4, source.label);
    if (source.serial.empty()) stmt.bind_null(5);
    else stmt.bind_text(5, source.serial);
    if (source.filesystem.empty()) stmt.bind_null(6);
    else stmt.bind_text(6, source.filesystem);
    stmt.bind_int64(7, source.size);
    if (source.created_at) stmt.bind_int64(8, source.created_at);
    else stmt.bind_null(8);
    stmt.bind_int64(9, source.cataloged_at);

    if (!stmt.step_done()) return Error{1, "Failed to insert source"};
    return static_cast<int64_t>(sqlite3_last_insert_rowid(db_.handle()));
}

Result<bool> SourceManager::update(const SourceData& source) {
    Statement stmt(db_,
        "UPDATE source SET name=?, type=?, source_path=?, label=?, serial=?, "
        "filesystem=?, size=?, created_at=?, cataloged_at=? WHERE id=?");
    if (!stmt.is_valid()) return Error{1, "Failed to prepare source update"};

    stmt.bind_text(1, source.name);
    stmt.bind_text(2, source_type_to_string(source.type));
    if (source.source_path.empty()) stmt.bind_null(3);
    else stmt.bind_text(3, source.source_path);
    if (source.label.empty()) stmt.bind_null(4);
    else stmt.bind_text(4, source.label);
    if (source.serial.empty()) stmt.bind_null(5);
    else stmt.bind_text(5, source.serial);
    if (source.filesystem.empty()) stmt.bind_null(6);
    else stmt.bind_text(6, source.filesystem);
    stmt.bind_int64(7, source.size);
    if (source.created_at) stmt.bind_int64(8, source.created_at);
    else stmt.bind_null(8);
    stmt.bind_int64(9, source.cataloged_at);
    stmt.bind_int64(10, source.id);

    if (!stmt.step_done()) return Error{1, "Failed to update source"};
    return true;
}

Result<SourceData> SourceManager::get_by_id(int64_t id) {
    Statement stmt(db_,
        "SELECT id, name, type, source_path, label, serial, filesystem, "
        "size, created_at, cataloged_at FROM source WHERE id=?");
    if (!stmt.is_valid()) return Error{1, "Failed to prepare source query"};

    stmt.bind_int64(1, id);
    if (!stmt.step()) return Error{1, "Source not found"};

    SourceData s;
    s.id = stmt.column_int64(0);
    s.name = stmt.column_text(1);
    auto st = source_type_from_string(stmt.column_text(2));
    s.type = st.value_or(SourceType::Other);
    s.source_path = stmt.column_is_null(3) ? "" : stmt.column_text(3);
    s.label = stmt.column_is_null(4) ? "" : stmt.column_text(4);
    s.serial = stmt.column_is_null(5) ? "" : stmt.column_text(5);
    s.filesystem = stmt.column_is_null(6) ? "" : stmt.column_text(6);
    s.size = stmt.column_is_null(7) ? 0 : stmt.column_int64(7);
    s.created_at = stmt.column_is_null(8) ? 0 : stmt.column_int64(8);
    s.cataloged_at = stmt.column_is_null(9) ? 0 : stmt.column_int64(9);
    return s;
}

Result<std::vector<SourceData>> SourceManager::get_all() {
    Statement stmt(db_,
        "SELECT id, name, type, source_path, label, serial, filesystem, "
        "size, created_at, cataloged_at FROM source ORDER BY id");
    if (!stmt.is_valid()) return Error{1, "Failed to prepare source query"};

    std::vector<SourceData> sources;
    while (stmt.step()) {
        SourceData s;
        s.id = stmt.column_int64(0);
        s.name = stmt.column_text(1);
        auto st = source_type_from_string(stmt.column_text(2));
        s.type = st.value_or(SourceType::Other);
        s.source_path = stmt.column_is_null(3) ? "" : stmt.column_text(3);
        s.label = stmt.column_is_null(4) ? "" : stmt.column_text(4);
        s.serial = stmt.column_is_null(5) ? "" : stmt.column_text(5);
        s.filesystem = stmt.column_is_null(6) ? "" : stmt.column_text(6);
        s.size = stmt.column_is_null(7) ? 0 : stmt.column_int64(7);
        s.created_at = stmt.column_is_null(8) ? 0 : stmt.column_int64(8);
        s.cataloged_at = stmt.column_is_null(9) ? 0 : stmt.column_int64(9);
        sources.push_back(std::move(s));
    }
    return sources;
}

Result<int64_t> SourceManager::count() {
    Statement stmt(db_, "SELECT COUNT(*) FROM source");
    if (!stmt.is_valid()) return Error{1, "Failed to count sources"};
    if (!stmt.step()) return Error{1, "Failed to get count"};
    return stmt.column_int64(0);
}

Result<int64_t> SourceManager::find_by_path(const std::string& source_path) {
    // Match both the exact path and the trailing-separator-stripped
    // variant so "S:\dir" and "S:\dir\" hit the same source.
    std::string trimmed = source_path;
    while (trimmed.size() > 1 &&
           (trimmed.back() == '/' || trimmed.back() == '\\')) {
        trimmed.pop_back();
    }
    Statement stmt(db_,
        "SELECT id FROM source WHERE source_path = ? OR source_path = ? LIMIT 1");
    if (!stmt.is_valid()) return Error{1, "Failed to prepare source lookup"};
    stmt.bind_text(1, source_path);
    stmt.bind_text(2, trimmed);
    if (!stmt.step()) return 0;  // no previous scan of this path
    return stmt.column_int64(0);
}

Result<bool> SourceManager::remove_tree(int64_t source_id) {
    // Runs inside the caller's transaction.  Deferred FK checks allow the
    // self-referencing entry.parent_id tree to be deleted in any order.
    if (is_err(db_.execute("PRAGMA defer_foreign_keys = ON;"))) {
        return Error{1, "Failed to enable deferred foreign keys"};
    }
    const char* sqls[] = {
        "DELETE FROM checksum WHERE entry_id IN "
            "(SELECT id FROM entry WHERE source_id = ?)",
        "DELETE FROM container WHERE entry_id IN "
            "(SELECT id FROM entry WHERE source_id = ?)",
        // Contentless FTS rows are not cascaded; drop them manually.
        "DELETE FROM entry_fts WHERE rowid IN "
            "(SELECT id FROM entry WHERE source_id = ?)",
        "DELETE FROM entry WHERE source_id = ?",
        "DELETE FROM scan WHERE source_id = ?",
        "DELETE FROM source WHERE id = ?",
    };
    for (const char* sql : sqls) {
        Statement stmt(db_, sql);
        if (!stmt.is_valid()) {
            return Error{1, "Failed to prepare source cleanup"};
        }
        stmt.bind_int64(1, source_id);
        if (!stmt.step_done()) {
            return Error{1, "Failed to remove source data"};
        }
    }
    return true;
}

// ── EntryManager ────────────────────────────────────────────────────

EntryManager::EntryManager(Database& db) : db_(db) {}

Result<int64_t> EntryManager::insert(const EntryData& entry) {
    Statement stmt(db_,
        "INSERT INTO entry (source_id, parent_id, name, type, size, mtime, "
        "ctime, atime, birthtime, mode, attributes, is_virtual) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)");
    if (!stmt.is_valid()) return Error{1, "Failed to prepare entry insert"};

    stmt.bind_int64(1, entry.source_id);
    if (entry.parent_id) stmt.bind_int64(2, entry.parent_id);
    else stmt.bind_null(2);
    stmt.bind_text(3, entry.name);
    stmt.bind_int(4, static_cast<int>(entry.type));
    stmt.bind_int64(5, entry.size);
    if (entry.mtime) stmt.bind_int64(6, entry.mtime); else stmt.bind_null(6);
    if (entry.ctime) stmt.bind_int64(7, entry.ctime); else stmt.bind_null(7);
    if (entry.atime) stmt.bind_int64(8, entry.atime); else stmt.bind_null(8);
    if (entry.birthtime) stmt.bind_int64(9, entry.birthtime); else stmt.bind_null(9);
    if (entry.mode) stmt.bind_int64(10, entry.mode); else stmt.bind_null(10);
    stmt.bind_int64(11, entry.attributes);
    stmt.bind_int(12, entry.is_virtual ? 1 : 0);

    if (!stmt.step_done()) return Error{1, "Failed to insert entry"};
    return static_cast<int64_t>(sqlite3_last_insert_rowid(db_.handle()));
}

Result<int64_t> EntryManager::insert_batch(const std::vector<EntryData>& entries) {
    Statement stmt(db_,
        "INSERT INTO entry (source_id, parent_id, name, type, size, mtime, "
        "ctime, atime, birthtime, mode, attributes, is_virtual) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)");
    if (!stmt.is_valid()) return Error{1, "Failed to prepare batch insert"};

    int64_t last_id = 0;
    for (const auto& entry : entries) {
        stmt.reset();
        stmt.bind_int64(1, entry.source_id);
        if (entry.parent_id) stmt.bind_int64(2, entry.parent_id);
        else stmt.bind_null(2);
        stmt.bind_text(3, entry.name);
        stmt.bind_int(4, static_cast<int>(entry.type));
        stmt.bind_int64(5, entry.size);
        if (entry.mtime) stmt.bind_int64(6, entry.mtime); else stmt.bind_null(6);
        if (entry.ctime) stmt.bind_int64(7, entry.ctime); else stmt.bind_null(7);
        if (entry.atime) stmt.bind_int64(8, entry.atime); else stmt.bind_null(8);
        if (entry.birthtime) stmt.bind_int64(9, entry.birthtime); else stmt.bind_null(9);
        if (entry.mode) stmt.bind_int64(10, entry.mode); else stmt.bind_null(10);
        stmt.bind_int64(11, entry.attributes);
        stmt.bind_int(12, entry.is_virtual ? 1 : 0);

        if (!stmt.step_done()) {
            return Error{1, "Failed to insert entry in batch: " + entry.name};
        }
        last_id = sqlite3_last_insert_rowid(db_.handle());
    }
    return last_id;
}

Result<EntryData> EntryManager::get_by_id(int64_t id) {
    Statement stmt(db_,
        "SELECT id, source_id, parent_id, name, type, size, mtime, ctime, "
        "atime, birthtime, mode, attributes, is_virtual FROM entry WHERE id=?");
    if (!stmt.is_valid()) return Error{1, "Failed to prepare entry query"};

    stmt.bind_int64(1, id);
    if (!stmt.step()) return Error{1, "Entry not found"};

    EntryData e;
    e.id = stmt.column_int64(0);
    e.source_id = stmt.column_int64(1);
    e.parent_id = stmt.column_is_null(2) ? 0 : stmt.column_int64(2);
    e.name = stmt.column_text(3);
    e.type = static_cast<EntryType>(stmt.column_int(4));
    e.size = stmt.column_is_null(5) ? 0 : stmt.column_int64(5);
    e.mtime = stmt.column_is_null(6) ? 0 : stmt.column_int64(6);
    e.ctime = stmt.column_is_null(7) ? 0 : stmt.column_int64(7);
    e.atime = stmt.column_is_null(8) ? 0 : stmt.column_int64(8);
    e.birthtime = stmt.column_is_null(9) ? 0 : stmt.column_int64(9);
    e.mode = stmt.column_is_null(10) ? 0 : stmt.column_int64(10);
    e.attributes = stmt.column_is_null(11) ? 0 : stmt.column_int64(11);
    e.is_virtual = stmt.column_int(12) != 0;
    return e;
}

Result<std::vector<EntryData>> EntryManager::get_children(int64_t parent_id) {
    Statement stmt(db_,
        "SELECT id, source_id, parent_id, name, type, size, mtime, ctime, "
        "atime, birthtime, mode, attributes, is_virtual FROM entry "
        "WHERE parent_id=? ORDER BY type DESC, name");
    if (!stmt.is_valid()) return Error{1, "Failed to prepare children query"};

    stmt.bind_int64(1, parent_id);
    std::vector<EntryData> entries;
    while (stmt.step()) {
        EntryData e;
        e.id = stmt.column_int64(0);
        e.source_id = stmt.column_int64(1);
        e.parent_id = stmt.column_is_null(2) ? 0 : stmt.column_int64(2);
        e.name = stmt.column_text(3);
        e.type = static_cast<EntryType>(stmt.column_int(4));
        e.size = stmt.column_is_null(5) ? 0 : stmt.column_int64(5);
        e.mtime = stmt.column_is_null(6) ? 0 : stmt.column_int64(6);
        e.ctime = stmt.column_is_null(7) ? 0 : stmt.column_int64(7);
        e.atime = stmt.column_is_null(8) ? 0 : stmt.column_int64(8);
        e.birthtime = stmt.column_is_null(9) ? 0 : stmt.column_int64(9);
        e.mode = stmt.column_is_null(10) ? 0 : stmt.column_int64(10);
        e.attributes = stmt.column_is_null(11) ? 0 : stmt.column_int64(11);
        e.is_virtual = stmt.column_int(12) != 0;
        entries.push_back(std::move(e));
    }
    return entries;
}

Result<std::vector<EntryData>> EntryManager::get_by_source(int64_t source_id) {
    Statement stmt(db_,
        "SELECT id, source_id, parent_id, name, type, size, mtime, ctime, "
        "atime, birthtime, mode, attributes, is_virtual FROM entry "
        "WHERE source_id=? ORDER BY parent_id, type DESC, name");
    if (!stmt.is_valid()) return Error{1, "Failed to prepare source entries query"};

    stmt.bind_int64(1, source_id);
    std::vector<EntryData> entries;
    while (stmt.step()) {
        EntryData e;
        e.id = stmt.column_int64(0);
        e.source_id = stmt.column_int64(1);
        e.parent_id = stmt.column_is_null(2) ? 0 : stmt.column_int64(2);
        e.name = stmt.column_text(3);
        e.type = static_cast<EntryType>(stmt.column_int(4));
        e.size = stmt.column_is_null(5) ? 0 : stmt.column_int64(5);
        e.mtime = stmt.column_is_null(6) ? 0 : stmt.column_int64(6);
        e.ctime = stmt.column_is_null(7) ? 0 : stmt.column_int64(7);
        e.atime = stmt.column_is_null(8) ? 0 : stmt.column_int64(8);
        e.birthtime = stmt.column_is_null(9) ? 0 : stmt.column_int64(9);
        e.mode = stmt.column_is_null(10) ? 0 : stmt.column_int64(10);
        e.attributes = stmt.column_is_null(11) ? 0 : stmt.column_int64(11);
        e.is_virtual = stmt.column_int(12) != 0;
        entries.push_back(std::move(e));
    }
    return entries;
}

Result<int64_t> EntryManager::count() {
    Statement stmt(db_, "SELECT COUNT(*) FROM entry");
    if (!stmt.is_valid()) return Error{1, "Failed to count entries"};
    if (!stmt.step()) return Error{1, "Failed to get count"};
    return stmt.column_int64(0);
}

Result<int64_t> EntryManager::count_by_source(int64_t source_id) {
    Statement stmt(db_, "SELECT COUNT(*) FROM entry WHERE source_id=?");
    if (!stmt.is_valid()) return Error{1, "Failed to count entries"};
    stmt.bind_int64(1, source_id);
    if (!stmt.step()) return Error{1, "Failed to get count"};
    return stmt.column_int64(0);
}

Result<std::string> EntryManager::build_path(int64_t entry_id) {
    std::vector<std::string> parts;
    int64_t current_id = entry_id;

    while (current_id != 0) {
        Statement stmt(db_,
            "SELECT id, name, parent_id FROM entry WHERE id=?");
        if (!stmt.is_valid()) return Error{1, "Failed to build path"};
        stmt.bind_int64(1, current_id);
        if (!stmt.step()) break;

        parts.push_back(stmt.column_text(1));
        current_id = stmt.column_is_null(2) ? 0 : stmt.column_int64(2);
    }

    std::string path;
    for (auto it = parts.rbegin(); it != parts.rend(); ++it) {
        if (!path.empty()) path += "/";
        path += *it;
    }
    return path;
}

Result<bool> EntryManager::insert_fts(int64_t entry_id, const std::string& name,
                                       const std::string& path,
                                       const std::string& source_name) {
    Statement stmt(db_,
        "INSERT INTO entry_fts(rowid, name, path, source_name) VALUES(?, ?, ?, ?)");
    if (!stmt.is_valid()) return Error{1, "Failed to prepare FTS insert"};

    stmt.bind_int64(1, entry_id);
    stmt.bind_text(2, name);
    stmt.bind_text(3, path);
    stmt.bind_text(4, source_name);

    if (!stmt.step_done()) return Error{1, "Failed to insert FTS entry"};
    return true;
}

// ── ContainerManager ────────────────────────────────────────────────

ContainerManager::ContainerManager(Database& db) : db_(db) {}

Result<int64_t> ContainerManager::insert(const ContainerData& container) {
    Statement stmt(db_,
        "INSERT INTO container (entry_id, type, provider, version) VALUES (?, ?, ?, ?)");
    if (!stmt.is_valid()) return Error{1, "Failed to prepare container insert"};

    stmt.bind_int64(1, container.entry_id);
    stmt.bind_text(2, container.type);
    if (container.provider.empty()) stmt.bind_null(3);
    else stmt.bind_text(3, container.provider);
    if (container.version.empty()) stmt.bind_null(4);
    else stmt.bind_text(4, container.version);

    if (!stmt.step_done()) return Error{1, "Failed to insert container"};
    return static_cast<int64_t>(sqlite3_last_insert_rowid(db_.handle()));
}

Result<ContainerData> ContainerManager::get_by_entry_id(int64_t entry_id) {
    Statement stmt(db_,
        "SELECT id, entry_id, type, provider, version FROM container WHERE entry_id=?");
    if (!stmt.is_valid()) return Error{1, "Failed to prepare container query"};

    stmt.bind_int64(1, entry_id);
    if (!stmt.step()) return Error{1, "Container not found"};

    ContainerData c;
    c.id = stmt.column_int64(0);
    c.entry_id = stmt.column_int64(1);
    c.type = stmt.column_text(2);
    c.provider = stmt.column_is_null(3) ? "" : stmt.column_text(3);
    c.version = stmt.column_is_null(4) ? "" : stmt.column_text(4);
    return c;
}

Result<std::vector<ContainerData>> ContainerManager::get_all() {
    Statement stmt(db_,
        "SELECT id, entry_id, type, provider, version FROM container ORDER BY id");
    if (!stmt.is_valid()) return Error{1, "Failed to prepare container query"};

    std::vector<ContainerData> containers;
    while (stmt.step()) {
        ContainerData c;
        c.id = stmt.column_int64(0);
        c.entry_id = stmt.column_int64(1);
        c.type = stmt.column_text(2);
        c.provider = stmt.column_is_null(3) ? "" : stmt.column_text(3);
        c.version = stmt.column_is_null(4) ? "" : stmt.column_text(4);
        containers.push_back(std::move(c));
    }
    return containers;
}

Result<bool> ContainerManager::is_container(int64_t entry_id) {
    Statement stmt(db_, "SELECT COUNT(*) FROM container WHERE entry_id=?");
    if (!stmt.is_valid()) return Error{1, "Failed to check container"};
    stmt.bind_int64(1, entry_id);
    if (!stmt.step()) return Error{1, "Failed to check container"};
    return stmt.column_int64(0) > 0;
}

// ── ChecksumManager ─────────────────────────────────────────────────

ChecksumManager::ChecksumManager(Database& db) : db_(db) {}

Result<bool> ChecksumManager::insert(const ChecksumData& checksum) {
    Statement stmt(db_,
        "INSERT OR REPLACE INTO checksum (entry_id, algorithm, value, calculated_at) "
        "VALUES (?, ?, ?, ?)");
    if (!stmt.is_valid()) return Error{1, "Failed to prepare checksum insert"};

    stmt.bind_int64(1, checksum.entry_id);
    stmt.bind_text(2, checksum_algorithm_to_string(checksum.algorithm));
    stmt.bind_blob(3, checksum.value.data(), static_cast<int>(checksum.value.size()));
    stmt.bind_int64(4, checksum.calculated_at);

    if (!stmt.step_done()) return Error{1, "Failed to insert checksum"};
    return true;
}

Result<ChecksumData> ChecksumManager::get(int64_t entry_id, ChecksumAlgorithm algorithm) {
    Statement stmt(db_,
        "SELECT entry_id, algorithm, value, calculated_at FROM checksum "
        "WHERE entry_id=? AND algorithm=?");
    if (!stmt.is_valid()) return Error{1, "Failed to prepare checksum query"};

    stmt.bind_int64(1, entry_id);
    stmt.bind_text(2, checksum_algorithm_to_string(algorithm));
    if (!stmt.step()) return Error{1, "Checksum not found"};

    ChecksumData c;
    c.entry_id = stmt.column_int64(0);
    auto algo = checksum_algorithm_from_string(stmt.column_text(1));
    c.algorithm = algo.value_or(ChecksumAlgorithm::SHA256);
    const void* blob = stmt.column_blob(2);
    int blob_size = stmt.column_bytes(2);
    if (blob && blob_size > 0) {
        c.value.assign(static_cast<const uint8_t*>(blob),
                       static_cast<const uint8_t*>(blob) + blob_size);
    }
    c.calculated_at = stmt.column_int64(3);
    return c;
}

Result<std::vector<ChecksumData>> ChecksumManager::get_all_for_entry(int64_t entry_id) {
    Statement stmt(db_,
        "SELECT entry_id, algorithm, value, calculated_at FROM checksum WHERE entry_id=?");
    if (!stmt.is_valid()) return Error{1, "Failed to prepare checksum query"};

    stmt.bind_int64(1, entry_id);
    std::vector<ChecksumData> checksums;
    while (stmt.step()) {
        ChecksumData c;
        c.entry_id = stmt.column_int64(0);
        auto algo = checksum_algorithm_from_string(stmt.column_text(1));
        c.algorithm = algo.value_or(ChecksumAlgorithm::SHA256);
        const void* blob = stmt.column_blob(2);
        int blob_size = stmt.column_bytes(2);
        if (blob && blob_size > 0) {
            c.value.assign(static_cast<const uint8_t*>(blob),
                           static_cast<const uint8_t*>(blob) + blob_size);
        }
        c.calculated_at = stmt.column_int64(3);
        checksums.push_back(std::move(c));
    }
    return checksums;
}

// ── ScanManager ─────────────────────────────────────────────────────

ScanManager::ScanManager(Database& db) : db_(db) {}

Result<int64_t> ScanManager::insert(const ScanData& scan) {
    Statement stmt(db_,
        "INSERT INTO scan (source_id, started_at, finished_at, scanner_version, "
        "options, status) VALUES (?, ?, ?, ?, ?, ?)");
    if (!stmt.is_valid()) return Error{1, "Failed to prepare scan insert"};

    stmt.bind_int64(1, scan.source_id);
    stmt.bind_int64(2, scan.started_at);
    if (scan.finished_at) stmt.bind_int64(3, scan.finished_at);
    else stmt.bind_null(3);
    if (scan.scanner_version.empty()) stmt.bind_null(4);
    else stmt.bind_text(4, scan.scanner_version);
    if (scan.options.empty()) stmt.bind_null(5);
    else stmt.bind_text(5, scan.options);
    stmt.bind_int(6, static_cast<int>(scan.status));

    if (!stmt.step_done()) return Error{1, "Failed to insert scan"};
    return static_cast<int64_t>(sqlite3_last_insert_rowid(db_.handle()));
}

Result<bool> ScanManager::update_status(int64_t scan_id, ScanStatus status) {
    Statement stmt(db_, "UPDATE scan SET status=? WHERE id=?");
    if (!stmt.is_valid()) return Error{1, "Failed to prepare scan update"};
    stmt.bind_int(1, static_cast<int>(status));
    stmt.bind_int64(2, scan_id);
    if (!stmt.step_done()) return Error{1, "Failed to update scan status"};
    return true;
}

Result<bool> ScanManager::finish(int64_t scan_id, ScanStatus status) {
    auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    Statement stmt(db_,
        "UPDATE scan SET status=?, finished_at=? WHERE id=?");
    if (!stmt.is_valid()) return Error{1, "Failed to prepare scan finish"};
    stmt.bind_int(1, static_cast<int>(status));
    stmt.bind_int64(2, static_cast<int64_t>(now));
    stmt.bind_int64(3, scan_id);
    if (!stmt.step_done()) return Error{1, "Failed to finish scan"};
    return true;
}

Result<ScanData> ScanManager::get_by_id(int64_t id) {
    Statement stmt(db_,
        "SELECT id, source_id, started_at, finished_at, scanner_version, "
        "options, status FROM scan WHERE id=?");
    if (!stmt.is_valid()) return Error{1, "Failed to prepare scan query"};

    stmt.bind_int64(1, id);
    if (!stmt.step()) return Error{1, "Scan not found"};

    ScanData s;
    s.id = stmt.column_int64(0);
    s.source_id = stmt.column_int64(1);
    s.started_at = stmt.column_int64(2);
    s.finished_at = stmt.column_is_null(3) ? 0 : stmt.column_int64(3);
    s.scanner_version = stmt.column_is_null(4) ? "" : stmt.column_text(4);
    s.options = stmt.column_is_null(5) ? "" : stmt.column_text(5);
    s.status = static_cast<ScanStatus>(stmt.column_int(6));
    return s;
}

Result<std::vector<ScanData>> ScanManager::get_by_source(int64_t source_id) {
    Statement stmt(db_,
        "SELECT id, source_id, started_at, finished_at, scanner_version, "
        "options, status FROM scan WHERE source_id=? ORDER BY started_at DESC");
    if (!stmt.is_valid()) return Error{1, "Failed to prepare scan query"};

    stmt.bind_int64(1, source_id);
    std::vector<ScanData> scans;
    while (stmt.step()) {
        ScanData s;
        s.id = stmt.column_int64(0);
        s.source_id = stmt.column_int64(1);
        s.started_at = stmt.column_int64(2);
        s.finished_at = stmt.column_is_null(3) ? 0 : stmt.column_int64(3);
        s.scanner_version = stmt.column_is_null(4) ? "" : stmt.column_text(4);
        s.options = stmt.column_is_null(5) ? "" : stmt.column_text(5);
        s.status = static_cast<ScanStatus>(stmt.column_int(6));
        scans.push_back(std::move(s));
    }
    return scans;
}

} // namespace offcat
