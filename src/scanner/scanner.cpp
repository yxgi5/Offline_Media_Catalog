// POSIX feature test macro: -std=c++17 implies __STRICT_ANSI__, which makes
// glibc/cygwin headers hide POSIX functions (isatty, fileno) behind
// _POSIX_C_SOURCE. Must be defined before any system header is included.
#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif

#include "scanner/scanner.h"
#include "core/checksum.h"
#include "core/logger.h"
#include "container/provider.h"
#include "platform/file_util.h"
#include <chrono>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <sstream>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>  // isatty()/fileno()
#endif

namespace offcat {

namespace {
// Content probing opens and reads each candidate file, so only files at
// least this large are checked when probe_containers is enabled; real
// disc images are far bigger than 1 MiB.
constexpr int64_t kProbeMinSize = 1 << 20;
}

Scanner::Scanner(Database& db, CancellationManager& cancel)
    : db_(db), cancel_(cancel),
      source_mgr_(db), entry_mgr_(db),
      container_mgr_(db), checksum_mgr_(db), scan_mgr_(db) {}

// Detect whether stdout is an interactive console: interactive terminals
// get an in-place progress line (carriage return), redirected output gets
// plain lines at a lower rate.
void Scanner::detect_tty() {
#ifdef _WIN32
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode = 0;
    // GetConsoleMode also succeeds on pipes (Win10 compatibility shim),
    // so require FILE_TYPE_CHAR to distinguish a real console from a
    // redirected/pipe stdout.
    if (h != INVALID_HANDLE_VALUE && GetFileType(h) == FILE_TYPE_CHAR &&
        GetConsoleMode(h, &mode)) {
        // Enable ANSI VT sequences so the progress line can be erased
        // whole-line (ESC[2K) instead of by byte count, which breaks
        // with multi-byte UTF-8 paths.
        SetConsoleMode(h, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
        tty_ = true;
    } else {
        tty_ = false;
    }
#else
    tty_ = isatty(fileno(stdout));
#endif
}

// Emit one progress line (throttled).  Interactive terminals redraw the
// same line with '\r'; redirected output appends plain lines every few
// seconds so logs stay readable.
void Scanner::progress_line(const std::string& text) {
    if (tty_) {
        // Clear the whole line first; byte-count based erasing leaves
        // stale fragments when the path contains multi-byte UTF-8.
        std::cout << "\r\x1b[2K" << text << std::flush;
        progress_printed_ = true;
    } else {
        std::cout << text << '\n' << std::flush;
    }
}

void Scanner::clear_progress() {
    if (tty_ && progress_printed_) {
        std::cout << "\r\x1b[2K" << std::flush;
        progress_printed_ = false;
    }
}

void Scanner::report_progress(const std::filesystem::path& path) {
    if (!show_progress_) return;
    auto now = std::chrono::steady_clock::now();
    auto interval = tty_ ? std::chrono::seconds(1)
                         : std::chrono::seconds(5);
    if (now - last_progress_ < interval) return;
    last_progress_ = now;
    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
        now - scan_start_).count();
    std::ostringstream oss;
    oss << "[" << elapsed << "s] " << path.string()
        << " - " << files_scanned_ << " files, " << dirs_scanned_ << " dirs";
    progress_line(oss.str());
}

void Scanner::report_hashing(const std::filesystem::path& path,
                             int64_t bytes) {
    if (!show_progress_) return;
    auto now = std::chrono::steady_clock::now();
    if (now - last_progress_ < std::chrono::seconds(1)) return;
    last_progress_ = now;
    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
        now - scan_start_).count();
    std::ostringstream oss;
    oss << "[" << elapsed << "s] hashing: " << path.string();
    if (bytes > 0) {
        oss << " (" << (bytes / (1024 * 1024)) << " MB)";
    }
    progress_line(oss.str());
}

SourceType Scanner::detect_source_type(const std::string& path,
                                        const ScanOptions& options) {
    std::error_code ec;
    auto status = std::filesystem::status(path, ec);
    if (ec) return SourceType::Other;

    if (std::filesystem::is_directory(status)) {
        return SourceType::Directory;
    }
    if (std::filesystem::is_regular_file(status)) {
        // Check if it's an ISO file
        std::string ext = std::filesystem::path(path).extension().string();
        // ::tolower on a bare char is UB for non-ASCII bytes; fold via
        // unsigned char first.
        std::transform(ext.begin(), ext.end(), ext.begin(),
                       [](unsigned char c) {
                           return static_cast<char>(std::tolower(c));
                       });
        if (ext == ".iso") {
            return SourceType::ISO;
        }
        // Optional content probe for renamed or extension-less images
        // (opt-in via --probe-containers).
        if (options.probe_containers) {
            std::error_code size_ec;
            auto size = std::filesystem::file_size(path, size_ec);
            if (!size_ec && size >= kProbeMinSize &&
                ProviderRegistry::instance().probe_file(path)) {
                return SourceType::ISO;
            }
        }
        return SourceType::File;
    }
    return SourceType::Other;
}

std::string Scanner::build_source_name(const std::string& path) {
    std::filesystem::path p(path);
    std::string name = p.filename().string();
    if (name.empty()) {
        // Root directory case (e.g., "D:\")
        name = p.string();
        // Remove trailing separator
        while (!name.empty() && (name.back() == '/' || name.back() == '\\')) {
            name.pop_back();
        }
        if (name.empty()) name = path;
    }
    return name;
}

Result<int64_t> Scanner::scan_source(const std::string& path,
                                      const ScanOptions& options) {
    files_scanned_ = 0;
    dirs_scanned_ = 0;
    errors_ = 0;
    total_size_ = 0;
    batch_counter_ = 0;

    // Detect source type
    SourceType source_type = detect_source_type(path, options);
    std::string source_name = build_source_name(path);

    // Validate before creating any records so a bad path leaves no
    // orphaned source row behind.
    std::error_code path_ec;
    bool is_dir = std::filesystem::is_directory(path, path_ec);
    bool is_file = !path_ec && std::filesystem::is_regular_file(path, path_ec);
    if (!is_dir && !is_file) {
        return Error{1, "Path is not a directory or regular file: " + path};
    }

    LOG_INFO("Scanning source: " + source_name + " (" + path + ")");

    // Remove sources left behind by scans that died mid-flight.  The
    // batched transaction design trades crash atomicity for a bounded
    // WAL (see recover_orphaned_scans): a killed scan can leave a shadow
    // tree plus its InProgress scan row behind, which would otherwise
    // confuse the find_by_path lookup below (no ORDER BY).
    auto recover_result = recover_orphaned_scans();
    if (is_err(recover_result)) {
        LOG_WARN("Orphan cleanup failed: " +
                 get_err(recover_result).message);
    }

    // Progress reporting setup (throttled path/stat output; see
    // report_progress / report_hashing)
    show_progress_ = options.show_progress;
    scan_start_ = std::chrono::steady_clock::now();
    last_progress_ = scan_start_;
    detect_tty();

    // Replacement semantics: a previous scan of the same path is removed
    // instead of accumulating duplicate sources/entries.  The new data is
    // written under a fresh shadow source id in batched transactions so
    // the WAL can be checkpointed during large scans; on success the old
    // tree is dropped in one small atomic transaction, on failure the
    // shadow tree is dropped instead and the previous data stays intact.
    bool replaced = false;
    int64_t old_source_id = 0;
    auto old_result = source_mgr_.find_by_path(path);
    if (is_err(old_result)) {
        return Error{1, "Failed to look up existing source"};
    }
    if (get_ok(old_result) > 0) {
        old_source_id = get_ok(old_result);
        replaced = true;
        LOG_INFO("Replacing previous scan data for: " + source_name);
    }

    // Create source record
    SourceData source;
    source.name = source_name;
    source.type = source_type;
    source.source_path = path;
    source.cataloged_at = std::chrono::system_clock::to_time_t(
        std::chrono::system_clock::now());

    // Try to get filesystem info for volumes/directories
    if (source_type == SourceType::Directory || source_type == SourceType::Volume) {
        std::error_code ec;
        auto space = std::filesystem::space(path, ec);
        if (!ec) {
            source.size = static_cast<int64_t>(space.capacity);
        }
    } else if (source_type == SourceType::File || source_type == SourceType::ISO) {
        std::error_code ec;
        source.size = static_cast<int64_t>(
            std::filesystem::file_size(path, ec));
    }

    auto source_result = source_mgr_.insert(source);
    if (is_err(source_result)) {
        return Error{1, "Failed to create source: " + get_err(source_result).message};
    }
    int64_t source_id = get_ok(source_result);

    // Create scan record
    ScanData scan;
    scan.source_id = source_id;
    scan.started_at = std::chrono::system_clock::to_time_t(
        std::chrono::system_clock::now());
    scan.scanner_version =
#ifdef OFFCAT_VERSION
        OFFCAT_VERSION;  // from git tag via CMake
#else
        "unknown";
#endif

    // Build options JSON
    std::ostringstream opts;
    opts << "{\"max_container_depth\":" << options.max_container_depth
         << ",\"checksum\":[";
    bool first = true;
    for (const auto& algo : options.checksum_algorithms) {
        if (!first) opts << ",";
        opts << "\"" << checksum_algorithm_to_string(algo) << "\"";
        first = false;
    }
    opts << "]}";
    scan.options = opts.str();
    scan.status = ScanStatus::InProgress;

    auto scan_result = scan_mgr_.insert(scan);
    if (is_err(scan_result)) {
        return Error{1, "Failed to create scan record"};
    }
    int64_t scan_id = get_ok(scan_result);

    // Scan phase: one transaction, committed in batches by
    // scan_directory so the WAL stays bounded (see Batch commit there).
    if (is_err(db_.execute("BEGIN;"))) {
        return Error{1, "Failed to begin scan transaction"};
    }

    std::error_code ec;
    if (std::filesystem::is_directory(path, ec)) {
        // Scan directory tree
        auto dir_result = scan_directory(source_id, 0,
            std::filesystem::path(path), options);
        if (is_err(dir_result)) {
            auto& err = get_err(dir_result);
            // Drop the uncommitted tail of the current batch.
            (void)db_.execute("ROLLBACK;");  // best effort; drop_shadow below is the real cleanup
            if (err.code == 0) {
                if (replaced) {
                    // Cancelled replacement scan: drop the shadow tree
                    // so the previous catalog data is preserved.
                    (void)drop_shadow(source_id);  // drop_shadow logs its own failures
                    clear_progress();
                    LOG_INFO("Scan cancelled, previous data preserved");
                    return Error{0, "Scan cancelled"};
                }
                // Cancellation: stop gracefully, keep the committed
                // partial data under the new source.
                clear_progress();
                LOG_INFO("Scan cancelled by user");
                auto fin_result = scan_mgr_.finish(scan_id, ScanStatus::Cancelled);
                if (is_err(fin_result)) {
                    LOG_WARN("Failed to mark scan cancelled: " +
                             get_err(fin_result).message);
                }
                return source_id;
            }
            // Failure: drop the shadow tree, the previous data (if any)
            // was never touched.
            (void)drop_shadow(source_id);  // drop_shadow logs its own failures
            return dir_result;
        }
    } else if (std::filesystem::is_regular_file(path, ec)) {
        // Single file scan
        auto file_result = scan_file_entry(source_id, 0,
            std::filesystem::path(path), options);
        if (is_err(file_result)) {
            (void)db_.execute("ROLLBACK;");  // best effort
            (void)drop_shadow(source_id);  // drop_shadow logs its own failures
            return file_result;
        }
    } else {
        (void)db_.execute("ROLLBACK;");  // best effort
        (void)drop_shadow(source_id);  // drop_shadow logs its own failures
        return Error{1, "Path is not a directory or regular file: " + path};
    }

    // Commit the remaining entries
    auto commit_result = db_.execute("COMMIT;");
    if (is_err(commit_result)) {
        LOG_WARN("Transaction commit issue, attempting to preserve data");
    }

    // Atomic switch: drop the old tree and close the scan record in one
    // small transaction, so the switch itself never bloats the WAL.
    std::string switch_error;
    {
        Transaction txn(db_);
        if (replaced) {
            auto rm_result = source_mgr_.remove_tree(old_source_id);
            if (is_err(rm_result)) {
                switch_error = "Failed to remove previous source data: " +
                               get_err(rm_result).message;
            }
        }
        if (switch_error.empty()) {
            ScanStatus final_status = cancel_.is_cancelled()
                ? ScanStatus::Cancelled : ScanStatus::Completed;
            auto fin_result = scan_mgr_.finish(scan_id, final_status);
            if (is_err(fin_result)) {
                LOG_WARN("Failed to close scan record: " +
                         get_err(fin_result).message);
            }
            auto switch_result = txn.commit();
            if (is_err(switch_result)) {
                switch_error = "Switch commit failed: " +
                               get_err(switch_result).message;
            }
        }
    }
    if (!switch_error.empty()) {
        // The switch rolled back, so the previous tree is still in place;
        // drop the committed shadow tree instead of leaving two trees in
        // the catalog.
        (void)drop_shadow(source_id);  // drop_shadow logs its own failures
        return Error{1, switch_error};
    }

    // Merge FTS segments so tombstones left by replaced scans do not
    // accumulate and the index stays compact across rescans.
    auto opt_result = entry_mgr_.optimize_fts();
    if (is_err(opt_result)) {
        LOG_WARN("FTS optimize failed: " + get_err(opt_result).message);
    }

    // Rebuild the database file so pages freed by the merge are
    // physically returned; keeps the file size stable across rescans.
    auto vacuum_result = db_.execute("VACUUM;");
    if (is_err(vacuum_result)) {
        LOG_WARN("VACUUM failed: " + get_err(vacuum_result).message);
    }

    clear_progress();

    LOG_INFO("Scan complete: " + std::to_string(files_scanned_) + " files, " +
             std::to_string(dirs_scanned_) + " directories, " +
             std::to_string(errors_) + " errors");

    return source_id;
}

Result<bool> Scanner::drop_shadow(int64_t source_id) {
    Transaction txn(db_);
    auto rm = source_mgr_.remove_tree(source_id);
    if (is_err(rm)) {
        LOG_WARN("Failed to drop shadow source data: " +
                 get_err(rm).message);
        return rm;
    }
    return txn.commit();
}

Result<bool> Scanner::recover_orphaned_scans() {
    // Any scan row still InProgress when a new scan starts belongs to a
    // dead process (the CLI runs one scan at a time and always finishes
    // its records), so its source tree is a crash leftover: drop it.
    // Collect the ids first: SQLite makes no guarantee for a cursor over
    // a table being modified mid-iteration, and BEGIN must not run while
    // the SELECT cursor still holds an implicit read transaction.
    std::vector<int64_t> orphans;
    {
        Statement stmt(db_, "SELECT source_id FROM scan WHERE status = ?");
        if (!stmt.is_valid()) {
            return Error{1, "Failed to prepare orphan scan lookup"};
        }
        stmt.bind_int(1, static_cast<int>(ScanStatus::InProgress));
        while (stmt.step()) {
            orphans.push_back(stmt.column_int64(0));
        }
    }
    if (orphans.empty()) return true;

    // One transaction for the whole cleanup: remove_tree's deferred
    // foreign-key checks need an active transaction.
    Transaction txn(db_);
    int64_t orphaned = 0;
    for (int64_t source_id : orphans) {
        auto rm = source_mgr_.remove_tree(source_id);
        if (is_err(rm)) {
            LOG_WARN("Failed to remove orphaned source data: " +
                     get_err(rm).message);
            continue;
        }
        orphaned++;
    }
    auto commit_result = txn.commit();
    if (is_err(commit_result)) {
        LOG_WARN("Failed to commit orphan cleanup: " +
                 get_err(commit_result).message);
        return commit_result;
    }
    if (orphaned > 0) {
        LOG_INFO("Removed " + std::to_string(orphaned) +
                 " orphaned source(s) from interrupted scans");
    }
    return true;
}

Result<int64_t> Scanner::scan_directory(int64_t source_id, int64_t parent_id,
                                          const std::filesystem::path& dir_path,
                                          const ScanOptions& options) {
    if (cancel_.is_cancelled()) {
        return Error{0, "Scan cancelled"};
    }

    std::error_code ec;
    LOG_VERBOSE("Scanning directory: " + dir_path.string());
    for (auto it = std::filesystem::directory_iterator(dir_path, ec);
         it != std::filesystem::directory_iterator(); it.increment(ec)) {

        if (ec) {
            LOG_WARN("Cannot read directory entry in " + dir_path.string() +
                     ": " + ec.message());
            errors_++;
            ec.clear();
            continue;
        }

        if (cancel_.is_cancelled()) {
            return Error{0, "Scan cancelled"};
        }

        const auto& entry = *it;
        std::string name = entry.path().filename().string();

        if (name.empty()) continue;

        report_progress(entry.path());

        EntryData entry_data;
        entry_data.source_id = source_id;
        entry_data.parent_id = parent_id;
        entry_data.name = name;

        // Read metadata
        std::error_code meta_ec;
        auto sym_status = entry.symlink_status(meta_ec);
        if (meta_ec) {
            LOG_WARN("Cannot read status for " + entry.path().string());
            errors_++;
            continue;
        }

        if (std::filesystem::is_symlink(sym_status)) {
            entry_data.type = EntryType::Symlink;
        } else if (std::filesystem::is_directory(sym_status)) {
            entry_data.type = EntryType::Directory;
        } else if (std::filesystem::is_regular_file(sym_status)) {
            entry_data.type = EntryType::File;
        } else {
            entry_data.type = EntryType::Other;
        }

        // Get detailed metadata
        auto file_status = entry.status(meta_ec);
        if (!meta_ec) {
            if (entry_data.type == EntryType::File) {
                entry_data.size = static_cast<int64_t>(entry.file_size(meta_ec));
                total_size_ += entry_data.size;
            }

            auto ftime = entry.last_write_time(meta_ec);
            if (!meta_ec) {
                // Convert file_time_type to time_t
                auto sctp = std::chrono::time_point_cast<
                    std::chrono::system_clock::duration>(
                    ftime - std::filesystem::file_time_type::clock::now() +
                    std::chrono::system_clock::now());
                entry_data.mtime = std::chrono::system_clock::to_time_t(sctp);
            }

            // Creation time is not exposed by std::filesystem; best-effort
            // platform lookup (NULL when unavailable).
            int64_t birthtime = 0;
            if (get_creation_time_utc(entry.path().string(), birthtime)) {
                entry_data.birthtime = birthtime;
            }

#ifdef __unix__
            entry_data.mode = static_cast<int64_t>(file_status.permissions());
#endif
#ifdef _WIN32
            if (file_status.permissions() != std::filesystem::perms::unknown) {
                entry_data.attributes = static_cast<int64_t>(
                    file_status.permissions());
            }
#endif
        }

        // Insert entry
        auto insert_result = entry_mgr_.insert(entry_data);
        if (is_err(insert_result)) {
            LOG_WARN("Failed to insert entry: " + name + " - " +
                     get_err(insert_result).message);
            errors_++;
            continue;
        }
        int64_t entry_id = get_ok(insert_result);

        // Update FTS index
        auto path_result = entry_mgr_.build_path(entry_id);
        std::string entry_path = is_ok(path_result) ? get_ok(path_result) : name;

        auto source_result = source_mgr_.get_by_id(source_id);
        std::string source_name = is_ok(source_result) ?
            get_ok(source_result).name : "";
        auto fts_result = entry_mgr_.insert_fts(entry_id, name, entry_path,
                                               source_name);
        if (is_err(fts_result)) {
            LOG_WARN("Failed to index entry: " + name);
        }

        batch_counter_++;

        // Batch commit: checkpoint every BATCH_SIZE entries so the WAL
        // is merged and cannot grow to the size of the whole scan.  On
        // failure the uncommitted tail is rolled back by scan_source;
        // committed batches belong to the shadow source and are dropped
        // together with it.
        if (batch_counter_ % BATCH_SIZE == 0) {
            auto cc = db_.execute("COMMIT;");
            if (is_err(cc)) {
                return Error{1, "Batch commit failed: " + get_err(cc).message};
            }
            auto bb = db_.execute("BEGIN;");
            if (is_err(bb)) {
                return Error{1, "Batch begin failed"};
            }
            LOG_DEBUG("Batch checkpoint at " +
                      std::to_string(batch_counter_) + " entries");
        }

        // Recurse into directories
        if (entry_data.type == EntryType::Directory) {
            dirs_scanned_++;
            auto sub_result = scan_directory(source_id, entry_id,
                entry.path(), options);
            if (is_err(sub_result)) {
                auto& err = get_err(sub_result);
                if (err.code == 0) {
                    // Cancellation
                    return sub_result;
                }
                LOG_WARN("Error scanning subdirectory: " + entry.path().string());
                errors_++;
            }
        } else if (entry_data.type == EntryType::File) {
            files_scanned_++;

            // Compute checksums if requested
            if (options.compute_checksum && !options.checksum_algorithms.empty()) {
                auto cs_result = compute_checksums(entry_id, entry.path(), options);
                if (is_err(cs_result)) {
                    LOG_WARN("Checksum failed for " + entry.path().string());
                    errors_++;
                }
            }

            // Container discovery and expansion
            expand_container_if_needed(entry_id, entry.path(), options);
        }
    }

    return parent_id;
}

Result<int64_t> Scanner::scan_file_entry(int64_t source_id, int64_t parent_id,
                                           const std::filesystem::path& file_path,
                                           const ScanOptions& options) {
    report_progress(file_path);

    EntryData entry_data;
    entry_data.source_id = source_id;
    entry_data.parent_id = parent_id;
    entry_data.name = file_path.filename().string();
    entry_data.type = EntryType::File;

    std::error_code ec;
    entry_data.size = static_cast<int64_t>(
        std::filesystem::file_size(file_path, ec));
    if (!ec) total_size_ += entry_data.size;

    auto ftime = std::filesystem::last_write_time(file_path, ec);
    if (!ec) {
        auto sctp = std::chrono::time_point_cast<
            std::chrono::system_clock::duration>(
            ftime - std::filesystem::file_time_type::clock::now() +
            std::chrono::system_clock::now());
        entry_data.mtime = std::chrono::system_clock::to_time_t(sctp);
    }

    // Best-effort creation time (NULL when unavailable).
    int64_t birthtime = 0;
    if (get_creation_time_utc(file_path.string(), birthtime)) {
        entry_data.birthtime = birthtime;
    }

    auto insert_result = entry_mgr_.insert(entry_data);
    if (is_err(insert_result)) {
        return insert_result;
    }
    int64_t entry_id = get_ok(insert_result);
    files_scanned_++;

    // Compute checksums if requested
    if (options.compute_checksum && !options.checksum_algorithms.empty()) {
        auto cs_result = compute_checksums(entry_id, file_path, options);
        if (is_err(cs_result)) {
            LOG_WARN("Checksum failed for " + file_path.string());
        }
    }

    // Container discovery and expansion
    expand_container_if_needed(entry_id, file_path, options);

    return entry_id;
}

void Scanner::expand_container_if_needed(
    int64_t entry_id, const std::filesystem::path& file_path,
    const ScanOptions& options) {
    // Discovery always runs (spec §16): ISO files are registered as
    // containers even when expansion is disabled, so `info` can show
    // them. Expansion below is gated by max_container_depth.
    // Discovery is extension-based by default; with probe_containers
    // enabled (--probe-containers), renamed or extension-less files are
    // also probed by content.
    std::string ext = file_path.extension().string();
    // ::tolower on a bare char is UB for non-ASCII bytes; fold via
    // unsigned char first.
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) {
                       return static_cast<char>(std::tolower(c));
                   });
    bool ext_match = (ext == ".iso");

    std::shared_ptr<ContainerProvider> provider;
    if (ext_match) {
        provider = ProviderRegistry::instance().find_provider("iso");
    } else if (options.probe_containers) {
        // Content probe for renamed or extension-less images
        // (.bin, .nrg, ...).  Only probe files large enough to be a
        // real disc image, so ordinary files do not pay an extra
        // open+read.
        std::error_code ec;
        auto size = std::filesystem::file_size(file_path, ec);
        if (!ec && size >= kProbeMinSize) {
            provider = ProviderRegistry::instance()
                           .probe_file(file_path.string());
        }
    }
    if (!provider) return;

    std::string name = file_path.filename().string();
    ContainerData container;
    container.entry_id = entry_id;
    container.type = "iso";
    container.provider = "iso_provider";
    auto container_result = container_mgr_.insert(container);
    if (is_err(container_result)) {
        LOG_WARN("Failed to register container: " + name);
        return;
    }

    LOG_VERBOSE("Discovered container: " + name + " (type=iso)");

    // Expansion: use the already-resolved provider
    if (options.max_container_depth >= 1) {
        ContainerOptions copt;
        copt.max_depth = options.max_container_depth;
        copt.current_depth = 1;
        if (provider->scan(entry_id, db_, copt)) {
            LOG_VERBOSE("Expanded container: " + name);
        } else {
            LOG_WARN("Container expansion failed: " + name);
            errors_++;
        }
    }
}

Result<bool> Scanner::compute_checksums(int64_t entry_id,
                                          const std::filesystem::path& file_path,
                                          const ScanOptions& options) {
    // Create checksum engines
    std::vector<std::unique_ptr<ChecksumEngine>> engines;
    for (const auto& algo : options.checksum_algorithms) {
        auto engine = create_checksum_engine(checksum_algorithm_to_string(algo));
        if (engine) {
            engines.push_back(std::move(engine));
        }
    }

    if (engines.empty()) return true;

    // Read file and compute
    std::ifstream file(file_path, std::ios::binary);
    if (!file.is_open()) {
        return Error{1, "Cannot open file for checksum: " + file_path.string()};
    }

    constexpr size_t BUFFER_SIZE = 65536;
    std::vector<uint8_t> buffer(BUFFER_SIZE);
    int64_t hashed = 0;

    while (file.good()) {
        file.read(reinterpret_cast<char*>(buffer.data()), BUFFER_SIZE);
        auto bytes_read = file.gcount();
        if (bytes_read > 0) {
            for (auto& engine : engines) {
                engine->update(buffer.data(), static_cast<size_t>(bytes_read));
            }
            hashed += bytes_read;
            report_hashing(file_path, hashed);
        }
    }

    // Store results
    auto now = std::chrono::system_clock::to_time_t(
        std::chrono::system_clock::now());

    for (auto& engine : engines) {
        auto digest = engine->finalize();
        ChecksumData cs;
        cs.entry_id = entry_id;
        auto algo = checksum_algorithm_from_string(engine->name());
        cs.algorithm = algo.value_or(ChecksumAlgorithm::SHA256);
        cs.value = digest;
        cs.calculated_at = now;

        auto result = checksum_mgr_.insert(cs);
        if (is_err(result)) {
            LOG_WARN("Failed to store checksum for entry " +
                     std::to_string(entry_id));
        }
    }

    LOG_DEBUG("Computed checksums for " + file_path.string());
    return true;
}

} // namespace offcat
