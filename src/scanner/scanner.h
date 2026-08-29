#pragma once

#include "core/types.h"
#include "database/database.h"
#include "catalog/catalog.h"
#include <atomic>
#include <functional>
#include <filesystem>

namespace offcat {

// ── Cancellation ────────────────────────────────────────────────────

class CancellationManager {
public:
    void request_cancel() { cancelled_.store(true); }
    bool is_cancelled() const { return cancelled_.load(); }
    void reset() { cancelled_.store(false); }

private:
    std::atomic<bool> cancelled_{false};
};

// ── Scanner ─────────────────────────────────────────────────────────

class Scanner {
public:
    Scanner(Database& db, CancellationManager& cancel);

    // Main scan entry point
    Result<int64_t> scan_source(const std::string& path,
                                 const ScanOptions& options);

    // Statistics from last scan
    int64_t files_scanned() const { return files_scanned_; }
    int64_t directories_scanned() const { return dirs_scanned_; }
    int64_t errors_count() const { return errors_; }
    int64_t total_size() const { return total_size_; }

private:
    Database& db_;
    CancellationManager& cancel_;
    SourceManager source_mgr_;
    EntryManager entry_mgr_;
    ContainerManager container_mgr_;
    ChecksumManager checksum_mgr_;
    ScanManager scan_mgr_;

    int64_t files_scanned_ = 0;
    int64_t dirs_scanned_ = 0;
    int64_t errors_ = 0;
    int64_t total_size_ = 0;
    int64_t batch_counter_ = 0;

    static constexpr int BATCH_SIZE = 1000;

    // Internal scan methods
    Result<int64_t> scan_directory(int64_t source_id, int64_t parent_id,
                                    const std::filesystem::path& dir_path,
                                    const ScanOptions& options);
    Result<int64_t> scan_file_entry(int64_t source_id, int64_t parent_id,
                                     const std::filesystem::path& file_path,
                                     const ScanOptions& options);

    // Compute checksums for a file
    Result<bool> compute_checksums(int64_t entry_id,
                                    const std::filesystem::path& file_path,
                                    const ScanOptions& options);

    // Determine source type from path
    SourceType detect_source_type(const std::string& path);

    // Build source name from path
    std::string build_source_name(const std::string& path);
};

} // namespace offcat
