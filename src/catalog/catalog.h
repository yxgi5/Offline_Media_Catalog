#pragma once

#include "database/database.h"
#include "core/types.h"
#include <vector>

namespace offcat {

class SourceManager {
public:
    explicit SourceManager(Database& db);

    Result<int64_t> insert(const SourceData& source);
    Result<bool> update(const SourceData& source);
    Result<SourceData> get_by_id(int64_t id);
    Result<std::vector<SourceData>> get_all();
    Result<int64_t> count();

private:
    Database& db_;
};

class EntryManager {
public:
    explicit EntryManager(Database& db);

    Result<int64_t> insert(const EntryData& entry);
    // Batch insert for performance
    Result<int64_t> insert_batch(const std::vector<EntryData>& entries);
    Result<EntryData> get_by_id(int64_t id);
    Result<std::vector<EntryData>> get_children(int64_t parent_id);
    Result<std::vector<EntryData>> get_by_source(int64_t source_id);
    Result<int64_t> count();
    Result<int64_t> count_by_source(int64_t source_id);

    // Build full path from parent_id chain
    Result<std::string> build_path(int64_t entry_id);

    // FTS index management
    Result<bool> insert_fts(int64_t entry_id, const std::string& name,
                            const std::string& path, const std::string& source_name);

private:
    Database& db_;
};

class ContainerManager {
public:
    explicit ContainerManager(Database& db);

    Result<int64_t> insert(const ContainerData& container);
    Result<ContainerData> get_by_entry_id(int64_t entry_id);
    Result<std::vector<ContainerData>> get_all();
    Result<bool> is_container(int64_t entry_id);

private:
    Database& db_;
};

class ChecksumManager {
public:
    explicit ChecksumManager(Database& db);

    Result<bool> insert(const ChecksumData& checksum);
    Result<ChecksumData> get(int64_t entry_id, ChecksumAlgorithm algorithm);
    Result<std::vector<ChecksumData>> get_all_for_entry(int64_t entry_id);

private:
    Database& db_;
};

class ScanManager {
public:
    explicit ScanManager(Database& db);

    Result<int64_t> insert(const ScanData& scan);
    Result<bool> update_status(int64_t scan_id, ScanStatus status);
    Result<bool> finish(int64_t scan_id, ScanStatus status);
    Result<ScanData> get_by_id(int64_t id);
    Result<std::vector<ScanData>> get_by_source(int64_t source_id);

private:
    Database& db_;
};

} // namespace offcat
