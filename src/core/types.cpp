#include "core/types.h"

namespace offcat {

std::string source_type_to_string(SourceType t) {
    switch (t) {
        case SourceType::PhysicalDisk: return "physical_disk";
        case SourceType::Volume:       return "volume";
        case SourceType::Directory:    return "directory";
        case SourceType::File:         return "file";
        case SourceType::ISO:          return "iso";
        case SourceType::Other:        return "other";
    }
    return "other";
}

std::optional<SourceType> source_type_from_string(const std::string& s) {
    if (s == "physical_disk") return SourceType::PhysicalDisk;
    if (s == "volume")        return SourceType::Volume;
    if (s == "directory")     return SourceType::Directory;
    if (s == "file")          return SourceType::File;
    if (s == "iso")           return SourceType::ISO;
    if (s == "other")         return SourceType::Other;
    return std::nullopt;
}

std::string entry_type_to_string(EntryType t) {
    switch (t) {
        case EntryType::File:      return "file";
        case EntryType::Directory: return "directory";
        case EntryType::Symlink:   return "symlink";
        case EntryType::Other:     return "other";
    }
    return "other";
}

std::string scan_status_to_string(ScanStatus s) {
    switch (s) {
        case ScanStatus::InProgress: return "in_progress";
        case ScanStatus::Completed:  return "completed";
        case ScanStatus::Cancelled:  return "cancelled";
        case ScanStatus::Failed:     return "failed";
    }
    return "unknown";
}

std::string checksum_algorithm_to_string(ChecksumAlgorithm a) {
    switch (a) {
        case ChecksumAlgorithm::SHA256: return "sha256";
        case ChecksumAlgorithm::MD5:    return "md5";
        case ChecksumAlgorithm::CRC32:  return "crc32";
    }
    return "unknown";
}

std::optional<ChecksumAlgorithm> checksum_algorithm_from_string(const std::string& s) {
    if (s == "sha256") return ChecksumAlgorithm::SHA256;
    if (s == "md5")    return ChecksumAlgorithm::MD5;
    if (s == "crc32")  return ChecksumAlgorithm::CRC32;
    return std::nullopt;
}

} // namespace offcat
