#pragma once

#include <cstdint>
#include <string>
#include <variant>
#include <optional>
#include <vector>

namespace offcat {

// ── Error handling ──────────────────────────────────────────────────

struct Error {
    int code = 0;
    std::string message;
};

template <typename T>
using Result = std::variant<T, Error>;

template <typename T>
bool is_ok(const Result<T>& r) { return std::holds_alternative<T>(r); }

template <typename T>
bool is_err(const Result<T>& r) { return std::holds_alternative<Error>(r); }

template <typename T>
const T& get_ok(const Result<T>& r) { return std::get<T>(r); }

template <typename T>
const Error& get_err(const Result<T>& r) { return std::get<Error>(r); }

inline Result<bool> ok() { return true; }
inline Result<bool> err(int code, const std::string& msg) {
    return Error{code, msg};
}

// ── Source type ─────────────────────────────────────────────────────

enum class SourceType {
    PhysicalDisk,
    Volume,
    Directory,
    File,
    ISO,
    Other
};

std::string source_type_to_string(SourceType t);
std::optional<SourceType> source_type_from_string(const std::string& s);

// ── Entry type ──────────────────────────────────────────────────────

enum class EntryType : int {
    File      = 1,
    Directory = 2,
    Symlink   = 3,
    Other     = 4
};

std::string entry_type_to_string(EntryType t);

// ── Scan status ─────────────────────────────────────────────────────

enum class ScanStatus : int {
    InProgress = 0,
    Completed  = 1,
    Cancelled  = 2,
    Failed     = 3
};

std::string scan_status_to_string(ScanStatus s);

// ── Checksum algorithm ──────────────────────────────────────────────

enum class ChecksumAlgorithm {
    SHA256,
    MD5,
    CRC32
};

std::string checksum_algorithm_to_string(ChecksumAlgorithm a);
std::optional<ChecksumAlgorithm> checksum_algorithm_from_string(const std::string& s);

// ── Data structures ─────────────────────────────────────────────────

struct SourceData {
    int64_t id = 0;
    std::string name;
    SourceType type = SourceType::Other;
    std::string source_path;
    std::string label;
    std::string serial;
    std::string filesystem;
    int64_t size = 0;
    int64_t created_at = 0;
    int64_t cataloged_at = 0;
};

struct EntryData {
    int64_t id = 0;
    int64_t source_id = 0;
    int64_t parent_id = 0;   // 0 = root
    std::string name;
    EntryType type = EntryType::File;
    int64_t size = 0;
    int64_t mtime = 0;
    int64_t ctime = 0;
    int64_t atime = 0;
    int64_t birthtime = 0;
    int64_t mode = 0;
    int64_t attributes = 0;
    bool is_virtual = false;
};

struct ContainerData {
    int64_t id = 0;
    int64_t entry_id = 0;
    std::string type;
    std::string provider;
    std::string version;
};

struct ChecksumData {
    int64_t entry_id = 0;
    ChecksumAlgorithm algorithm = ChecksumAlgorithm::SHA256;
    std::vector<uint8_t> value;
    int64_t calculated_at = 0;
};

struct ScanData {
    int64_t id = 0;
    int64_t source_id = 0;
    int64_t started_at = 0;
    int64_t finished_at = 0;
    std::string scanner_version;
    std::string options;  // JSON
    ScanStatus status = ScanStatus::InProgress;
};

// ── Scan options ────────────────────────────────────────────────────

struct ScanOptions {
    bool compute_checksum = false;
    std::vector<ChecksumAlgorithm> checksum_algorithms;
    // 0 = discover containers only, no expansion (default)
    int max_container_depth = 0;
    bool show_progress = true;  // Live progress output during the scan
};

// ── Container options ───────────────────────────────────────────────

struct ContainerOptions {
    int max_depth = 1;
    int current_depth = 0;
    int max_entries = 1000000;
    int64_t max_virtual_size = 0;
    int max_scan_time_seconds = 0;
};

} // namespace offcat
