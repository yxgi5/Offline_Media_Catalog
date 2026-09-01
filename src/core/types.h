#pragma once

#include <cstdint>
#include <string>
#include <variant>
#include <optional>
#include <utility>
#include <vector>

namespace offcat {

// ── Error handling ──────────────────────────────────────────────────

struct Error {
    int code = 0;
    std::string message;
};

// Result<T> carries either a value or an Error.  The class carries
// [[nodiscard]] so every dropped error return becomes a compile-time
// warning instead of silent failure.
template <typename T>
class [[nodiscard]] Result {
public:
    Result(const T& value) : v_(value) {}
    Result(T&& value) : v_(std::move(value)) {}
    Result(const Error& error) : v_(error) {}
    Result(Error&& error) : v_(std::move(error)) {}

    const T& value() const { return std::get<T>(v_); }
    const Error& error() const { return std::get<Error>(v_); }
    bool has_value() const { return std::holds_alternative<T>(v_); }

private:
    std::variant<T, Error> v_;
};

template <typename T>
bool is_ok(const Result<T>& r) { return r.has_value(); }

template <typename T>
bool is_err(const Result<T>& r) { return !r.has_value(); }

template <typename T>
const T& get_ok(const Result<T>& r) { return r.value(); }

template <typename T>
const Error& get_err(const Result<T>& r) { return r.error(); }

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
    // Guardrails for one container expansion; each limit stops the
    // walk (a warning is logged, inserted entries are kept).
    // 0 disables the limit for max_virtual_size / max_scan_time_seconds.
    int max_entries = 1000000;       // cap on virtual entries inserted
    int64_t max_virtual_size = 0;    // cap on accumulated entry size (0 = off)
    int max_scan_time_seconds = 0;   // wall-clock cap per container (0 = off)
};

} // namespace offcat
