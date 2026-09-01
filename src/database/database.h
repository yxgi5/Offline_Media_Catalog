#pragma once

#include <sqlite3.h>
#include <string>
#include <memory>
#include <functional>
#include "core/types.h"

namespace offcat {

class Database {
public:
    Database();
    ~Database();

    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;

    // Open or create a catalog database
    Result<bool> open(const std::string& path);
    Result<bool> create(const std::string& path);
    // Open strictly read-only (safe for viewers; never creates the file)
    Result<bool> open_readonly(const std::string& path);
    void close();

    bool is_open() const;
    sqlite3* handle();

    // Schema management
    Result<bool> initialize_schema();

    // Transaction helpers
    Result<bool> begin_transaction();
    Result<bool> commit_transaction();
    Result<bool> rollback_transaction();

    // Execute raw SQL (no results)
    Result<bool> execute(const std::string& sql);

    // PRAGMA helpers
    Result<bool> enable_wal();

private:
    sqlite3* db_ = nullptr;
};

// RAII Transaction guard
class Transaction {
public:
    explicit Transaction(Database& db);
    ~Transaction();

    Transaction(const Transaction&) = delete;
    Transaction& operator=(const Transaction&) = delete;

    Result<bool> commit();
    Result<bool> rollback();

    // If not committed, rolls back on destruction
    bool is_active() const;

private:
    Database& db_;
    bool active_ = false;
    bool committed_ = false;
};

// Prepared statement wrapper
class Statement {
public:
    Statement(Database& db, const std::string& sql);
    ~Statement();

    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;

    bool is_valid() const;

    // Bind parameters (1-based index)
    void bind_int(int index, int64_t value);
    void bind_int64(int index, int64_t value);
    void bind_text(int index, const std::string& value);
    void bind_blob(int index, const void* data, int size);
    void bind_null(int index);

    // Execute step
    bool step();       // Returns true if row available
    bool step_done();  // Returns true if SQLITE_DONE

    // Get column values (0-based index)
    int64_t    column_int64(int index) const;
    int        column_int(int index) const;
    std::string column_text(int index) const;
    const void* column_blob(int index) const;
    int        column_bytes(int index) const;
    bool       column_is_null(int index) const;

    void reset();

private:
    sqlite3_stmt* stmt_ = nullptr;
};

} // namespace offcat
