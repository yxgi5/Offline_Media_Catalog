#include "database/database.h"
#include "core/logger.h"
#include <cstring>

namespace offcat {

// ── Database ────────────────────────────────────────────────────────

Database::Database() = default;

Database::~Database() {
    close();
}

Result<bool> Database::open(const std::string& path) {
    if (db_) {
        close();
    }
    int rc = sqlite3_open(path.c_str(), &db_);
    if (rc != SQLITE_OK) {
        std::string msg = sqlite3_errmsg(db_);
        sqlite3_close(db_);
        db_ = nullptr;
        return Error{rc, "Failed to open database: " + msg};
    }

    // Enable WAL mode
    auto wal_result = enable_wal();
    if (is_err(wal_result)) {
        LOG_WARN("Failed to enable WAL mode");
    }

    // Enable foreign keys
    execute("PRAGMA foreign_keys = ON;");

    return true;
}

Result<bool> Database::create(const std::string& path) {
    auto result = open(path);
    if (is_err(result)) {
        return result;
    }
    return initialize_schema();
}

Result<bool> Database::open_readonly(const std::string& path) {
    if (db_) {
        close();
    }
    int rc = sqlite3_open_v2(path.c_str(), &db_, SQLITE_OPEN_READONLY, nullptr);
    if (rc != SQLITE_OK) {
        std::string msg = db_ ? sqlite3_errmsg(db_) : "unknown";
        sqlite3_close(db_);
        db_ = nullptr;
        return Error{rc, "Failed to open database read-only: " + msg};
    }
    // WAL databases need the shared-memory file even for readers;
    // query_only guards against accidental writes via raw handles.
    execute("PRAGMA query_only = ON;");
    return true;
}

void Database::close() {
    if (db_) {
        sqlite3_close_v2(db_);
        db_ = nullptr;
    }
}

bool Database::is_open() const {
    return db_ != nullptr;
}

sqlite3* Database::handle() {
    return db_;
}

Result<bool> Database::initialize_schema() {
    const char* schema = R"SQL(
        CREATE TABLE IF NOT EXISTS source (
            id              INTEGER PRIMARY KEY,
            name            TEXT NOT NULL,
            type            TEXT NOT NULL,
            source_path     TEXT,
            label           TEXT,
            serial          TEXT,
            filesystem      TEXT,
            size            INTEGER,
            created_at      INTEGER,
            cataloged_at    INTEGER
        );

        CREATE TABLE IF NOT EXISTS entry (
            id              INTEGER PRIMARY KEY,
            source_id       INTEGER NOT NULL,
            parent_id       INTEGER,
            name            TEXT NOT NULL,
            type            INTEGER NOT NULL,
            size            INTEGER,
            mtime           INTEGER,
            ctime           INTEGER,
            atime           INTEGER,
            birthtime       INTEGER,
            mode            INTEGER,
            attributes      INTEGER DEFAULT 0,
            is_virtual      INTEGER DEFAULT 0,
            FOREIGN KEY(source_id) REFERENCES source(id),
            FOREIGN KEY(parent_id) REFERENCES entry(id)
        );

        CREATE TABLE IF NOT EXISTS container (
            id              INTEGER PRIMARY KEY,
            entry_id        INTEGER NOT NULL,
            type            TEXT NOT NULL,
            provider        TEXT,
            version         TEXT,
            FOREIGN KEY(entry_id) REFERENCES entry(id)
        );

        CREATE TABLE IF NOT EXISTS checksum (
            entry_id        INTEGER NOT NULL,
            algorithm       TEXT NOT NULL,
            value           BLOB NOT NULL,
            calculated_at   INTEGER NOT NULL,
            PRIMARY KEY(entry_id, algorithm),
            FOREIGN KEY(entry_id) REFERENCES entry(id)
        );

        CREATE TABLE IF NOT EXISTS scan (
            id              INTEGER PRIMARY KEY,
            source_id       INTEGER NOT NULL,
            started_at      INTEGER NOT NULL,
            finished_at     INTEGER,
            scanner_version TEXT,
            options         TEXT,
            status          INTEGER,
            FOREIGN KEY(source_id) REFERENCES source(id)
        );

        CREATE INDEX IF NOT EXISTS idx_entry_source ON entry(source_id);
        CREATE INDEX IF NOT EXISTS idx_entry_parent ON entry(parent_id);
        CREATE INDEX IF NOT EXISTS idx_entry_source_parent ON entry(source_id, parent_id);
        CREATE INDEX IF NOT EXISTS idx_container_entry ON container(entry_id);
        CREATE INDEX IF NOT EXISTS idx_checksum_entry ON checksum(entry_id);
        CREATE INDEX IF NOT EXISTS idx_scan_source ON scan(source_id);
    )SQL";

    char* errmsg = nullptr;
    int rc = sqlite3_exec(db_, schema, nullptr, nullptr, &errmsg);
    if (rc != SQLITE_OK) {
        std::string msg = errmsg ? errmsg : "unknown error";
        sqlite3_free(errmsg);
        return Error{rc, "Failed to create schema: " + msg};
    }

    // Create FTS5 virtual table (separate because it uses different syntax)
    const char* fts_sql = R"SQL(
        CREATE VIRTUAL TABLE IF NOT EXISTS entry_fts USING fts5(
            name,
            path,
            source_name,
            content='',
            tokenize='unicode61'
        );
    )SQL";

    rc = sqlite3_exec(db_, fts_sql, nullptr, nullptr, &errmsg);
    if (rc != SQLITE_OK) {
        std::string msg = errmsg ? errmsg : "unknown error";
        sqlite3_free(errmsg);
        return Error{rc, "Failed to create FTS5 table: " + msg};
    }

    // Schema version tracking
    const char* version_sql = R"SQL(
        CREATE TABLE IF NOT EXISTS schema_version (
            version INTEGER NOT NULL
        );
        INSERT OR IGNORE INTO schema_version(rowid, version) VALUES(1, 1);
    )SQL";

    rc = sqlite3_exec(db_, version_sql, nullptr, nullptr, &errmsg);
    if (rc != SQLITE_OK) {
        std::string msg = errmsg ? errmsg : "unknown error";
        sqlite3_free(errmsg);
        // Non-fatal: version table is supplementary
        LOG_WARN("Failed to set schema version: " + msg);
    }

    LOG_VERBOSE("Database schema initialized");
    return true;
}

Result<bool> Database::begin_transaction() {
    return execute("BEGIN TRANSACTION;");
}

Result<bool> Database::commit_transaction() {
    return execute("COMMIT;");
}

Result<bool> Database::rollback_transaction() {
    return execute("ROLLBACK;");
}

Result<bool> Database::execute(const std::string& sql) {
    char* errmsg = nullptr;
    int rc = sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &errmsg);
    if (rc != SQLITE_OK) {
        std::string msg = errmsg ? errmsg : "unknown error";
        sqlite3_free(errmsg);
        return Error{rc, msg};
    }
    return true;
}

Result<bool> Database::enable_wal() {
    return execute("PRAGMA journal_mode = WAL;");
}

Result<bool> Database::set_journal_mode(const std::string& mode) {
    return execute("PRAGMA journal_mode = " + mode + ";");
}

// ── Transaction ─────────────────────────────────────────────────────

Transaction::Transaction(Database& db) : db_(db) {
    auto result = db_.begin_transaction();
    if (is_ok(result)) {
        active_ = true;
    } else {
        LOG_ERROR("Failed to begin transaction: " + get_err(result).message);
    }
}

Transaction::~Transaction() {
    if (active_ && !committed_) {
        auto result = db_.rollback_transaction();
        if (is_err(result)) {
            LOG_ERROR("Failed to rollback transaction: " + get_err(result).message);
        }
    }
}

Result<bool> Transaction::commit() {
    if (!active_) {
        return Error{1, "Transaction not active"};
    }
    auto result = db_.commit_transaction();
    if (is_ok(result)) {
        active_ = false;
        committed_ = true;
    }
    return result;
}

Result<bool> Transaction::rollback() {
    if (!active_) {
        return Error{1, "Transaction not active"};
    }
    auto result = db_.rollback_transaction();
    if (is_ok(result)) {
        active_ = false;
    }
    return result;
}

bool Transaction::is_active() const {
    return active_;
}

// ── Statement ───────────────────────────────────────────────────────

Statement::Statement(Database& db, const std::string& sql) {
    int rc = sqlite3_prepare_v2(db.handle(), sql.c_str(),
                                 static_cast<int>(sql.size()), &stmt_, nullptr);
    if (rc != SQLITE_OK) {
        LOG_ERROR("Failed to prepare statement: " + std::string(sql) +
                  " error: " + sqlite3_errmsg(db.handle()));
        stmt_ = nullptr;
    }
}

Statement::~Statement() {
    if (stmt_) {
        sqlite3_finalize(stmt_);
    }
}

bool Statement::is_valid() const {
    return stmt_ != nullptr;
}

void Statement::bind_int(int index, int64_t value) {
    sqlite3_bind_int64(stmt_, index, value);
}

void Statement::bind_int64(int index, int64_t value) {
    sqlite3_bind_int64(stmt_, index, value);
}

void Statement::bind_text(int index, const std::string& value) {
    sqlite3_bind_text(stmt_, index, value.c_str(),
                      static_cast<int>(value.size()), SQLITE_TRANSIENT);
}

void Statement::bind_blob(int index, const void* data, int size) {
    sqlite3_bind_blob(stmt_, index, data, size, SQLITE_TRANSIENT);
}

void Statement::bind_null(int index) {
    sqlite3_bind_null(stmt_, index);
}

bool Statement::step() {
    int rc = sqlite3_step(stmt_);
    return rc == SQLITE_ROW;
}

bool Statement::step_done() {
    int rc = sqlite3_step(stmt_);
    return rc == SQLITE_DONE;
}

int64_t Statement::column_int64(int index) const {
    return sqlite3_column_int64(stmt_, index);
}

int Statement::column_int(int index) const {
    return sqlite3_column_int(stmt_, index);
}

std::string Statement::column_text(int index) const {
    const unsigned char* text = sqlite3_column_text(stmt_, index);
    if (!text) return "";
    int len = sqlite3_column_bytes(stmt_, index);
    return std::string(reinterpret_cast<const char*>(text), len);
}

const void* Statement::column_blob(int index) const {
    return sqlite3_column_blob(stmt_, index);
}

int Statement::column_bytes(int index) const {
    return sqlite3_column_bytes(stmt_, index);
}

bool Statement::column_is_null(int index) const {
    return sqlite3_column_type(stmt_, index) == SQLITE_NULL;
}

void Statement::reset() {
    if (stmt_) {
        sqlite3_reset(stmt_);
        sqlite3_clear_bindings(stmt_);
    }
}

} // namespace offcat
