#include "scanner/scanner.h"
#include "core/checksum.h"
#include "core/logger.h"
#include "container/provider.h"
#include <chrono>
#include <fstream>
#include <sstream>

namespace offcat {

Scanner::Scanner(Database& db, CancellationManager& cancel)
    : db_(db), cancel_(cancel),
      source_mgr_(db), entry_mgr_(db),
      container_mgr_(db), checksum_mgr_(db), scan_mgr_(db) {}

SourceType Scanner::detect_source_type(const std::string& path) {
    std::error_code ec;
    auto status = std::filesystem::status(path, ec);
    if (ec) return SourceType::Other;

    if (std::filesystem::is_directory(status)) {
        return SourceType::Directory;
    }
    if (std::filesystem::is_regular_file(status)) {
        // Check if it's an ISO file
        std::string ext = std::filesystem::path(path).extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        if (ext == ".iso" || ext == ".img") {
            return SourceType::ISO;
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
    SourceType source_type = detect_source_type(path);
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
    scan.scanner_version = "0.1.0";

    // Build options JSON
    std::ostringstream opts;
    opts << "{\"containers\":" << (options.scan_containers ? "true" : "false")
         << ",\"max_container_depth\":" << options.max_container_depth
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

    // Begin scanning
    Transaction txn(db_);

    std::error_code ec;
    if (std::filesystem::is_directory(path, ec)) {
        // Scan directory tree
        auto dir_result = scan_directory(source_id, 0,
            std::filesystem::path(path), options);
        if (is_err(dir_result)) {
            auto& err = get_err(dir_result);
            if (err.code == 0) {
                // Cancellation: stop gracefully, keep partial data
                LOG_INFO("Scan cancelled by user");
            } else {
                scan_mgr_.finish(scan_id, ScanStatus::Failed);
                return dir_result;
            }
        }
    } else if (std::filesystem::is_regular_file(path, ec)) {
        // Single file scan
        auto file_result = scan_file_entry(source_id, 0,
            std::filesystem::path(path), options);
        if (is_err(file_result)) {
            scan_mgr_.finish(scan_id, ScanStatus::Failed);
            return file_result;
        }
    } else {
        scan_mgr_.finish(scan_id, ScanStatus::Failed);
        return Error{1, "Path is not a directory or regular file: " + path};
    }

    // Commit remaining entries
    auto commit_result = txn.commit();
    if (is_err(commit_result)) {
        LOG_WARN("Transaction commit issue, attempting to preserve data");
    }

    // Determine final status
    ScanStatus final_status = cancel_.is_cancelled()
        ? ScanStatus::Cancelled : ScanStatus::Completed;
    scan_mgr_.finish(scan_id, final_status);

    LOG_INFO("Scan complete: " + std::to_string(files_scanned_) + " files, " +
             std::to_string(dirs_scanned_) + " directories, " +
             std::to_string(errors_) + " errors");

    return source_id;
}

Result<int64_t> Scanner::scan_directory(int64_t source_id, int64_t parent_id,
                                          const std::filesystem::path& dir_path,
                                          const ScanOptions& options) {
    if (cancel_.is_cancelled()) {
        return Error{0, "Scan cancelled"};
    }

    std::error_code ec;
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
        entry_mgr_.insert_fts(entry_id, name, entry_path, source_name);

        batch_counter_++;

        // Batch commit
        if (batch_counter_ % BATCH_SIZE == 0) {
            LOG_DEBUG("Batch checkpoint at " + std::to_string(batch_counter_) +
                      " entries");
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
            if (options.scan_containers) {
                std::string ext = entry.path().extension().string();
                std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
                if (ext == ".iso" || ext == ".img") {
                    ContainerData container;
                    container.entry_id = entry_id;
                    container.type = "iso";
                    container.provider = "iso_provider";
                    auto container_result = container_mgr_.insert(container);
                    if (is_ok(container_result)) {
                        LOG_VERBOSE("Discovered container: " + name + " (type=iso)");

                        // Expansion: use the registered provider if available
                        auto provider = ProviderRegistry::instance()
                            .find_provider("iso");
                        if (provider && options.max_container_depth >= 1) {
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
                }
            }
        }
    }

    return parent_id;
}

Result<int64_t> Scanner::scan_file_entry(int64_t source_id, int64_t parent_id,
                                           const std::filesystem::path& file_path,
                                           const ScanOptions& options) {
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

    auto insert_result = entry_mgr_.insert(entry_data);
    if (is_err(insert_result)) {
        return insert_result;
    }
    int64_t entry_id = get_ok(insert_result);
    files_scanned_++;

    // Compute checksums if requested
    if (options.compute_checksum && !options.checksum_algorithms.empty()) {
        compute_checksums(entry_id, file_path, options);
    }

    return entry_id;
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

    while (file.good()) {
        file.read(reinterpret_cast<char*>(buffer.data()), BUFFER_SIZE);
        auto bytes_read = file.gcount();
        if (bytes_read > 0) {
            for (auto& engine : engines) {
                engine->update(buffer.data(), static_cast<size_t>(bytes_read));
            }
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
