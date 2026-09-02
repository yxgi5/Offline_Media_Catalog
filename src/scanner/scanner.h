#pragma once

#include "core/types.h"
#include "database/database.h"
#include "catalog/catalog.h"
#include <atomic>
#include <chrono>
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

    // Progress reporting state
    bool show_progress_ = false;
    bool tty_ = false;
    bool progress_printed_ = false;
    std::chrono::steady_clock::time_point scan_start_;
    std::chrono::steady_clock::time_point last_progress_;

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

    // Drop the shadow source tree (batched scan leftovers) in its own
    // small transaction; used on failure and on cancelled replacement
    // scans so the previous catalog data stays untouched.
    Result<bool> drop_shadow(int64_t source_id);

    // Remove sources left behind by scans that died mid-flight (crash,
    // SIGKILL, power loss).  The batched transaction design trades crash
    // atomicity for a bounded WAL; this recovery runs at the start of
    // each scan and deletes any tree whose scan row is still InProgress.
    Result<bool> recover_orphaned_scans();

    // Discover and expand ISO containers (shared by directory and single-file scans)
    void expand_container_if_needed(int64_t entry_id,
                                    const std::filesystem::path& file_path,
                                    const ScanOptions& options);

    // Progress output helpers
    void detect_tty();
    void report_progress(const std::filesystem::path& path);
    void report_hashing(const std::filesystem::path& path, int64_t bytes);
    void progress_line(const std::string& text);
    void clear_progress();

    // Determine source type from path
    SourceType detect_source_type(const std::string& path,
                                  const ScanOptions& options);

    // Build source name from path
    std::string build_source_name(const std::string& path);
};

} // namespace offcat
