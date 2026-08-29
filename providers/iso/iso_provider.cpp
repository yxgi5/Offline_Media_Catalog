#include "iso_provider.h"
#include "iso9660/iso9660_parser.h"
#include "joliet/joliet_parser.h"
#include "udf/udf_parser.h"
#include "udf/udf_unicode.h"
#include "core/logger.h"
#include "catalog/catalog.h"

#include <cctype>
#include <algorithm>

namespace offcat {

namespace {

// Rock Ridge extended attributes move deep directories into /rr_moved
// and leave a single-byte placeholder (0x00, 0x01, ...) in the original
// location.  We do not parse RR attributes, so names containing control
// characters are skipped rather than catalogued as garbage.
bool valid_iso_name(const std::string& name) {
    if (name.empty()) return false;
    for (unsigned char c : name) {
        if (c < 0x20) return false;
    }
    return true;
}

} // namespace

// ── Extension check ─────────────────────────────────────────────────

bool IsoProvider::has_iso_extension(const std::string& filepath) {
    auto dot = filepath.find_last_of('.');
    if (dot == std::string::npos) return false;
    std::string ext = filepath.substr(dot + 1);
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext == "iso" || ext == "img" || ext == "udf";
}

// ── Probe ───────────────────────────────────────────────────────────

bool IsoProvider::probe(const std::string& filepath) {
    // Quick extension check first
    if (!has_iso_extension(filepath)) return false;

    // Check ISO9660 magic at sector 16
    {
        UdfParser udf(filepath);
        if (udf.open()) return true;
    }

    {
        Iso9660Parser iso(filepath);
        if (iso.open()) return true;
    }

    return false;
}

// ── Scan ────────────────────────────────────────────────────────────

bool IsoProvider::scan(int64_t container_entry_id,
                       Database& db,
                       const ContainerOptions& options) {
    // Get the container entry to find the file path
    EntryManager entry_mgr(db);
    auto entry_result = entry_mgr.get_by_id(container_entry_id);
    if (is_err(entry_result)) {
        LOG_ERROR("ISO scan: container entry not found");
        return false;
    }

    const EntryData& entry = get_ok(entry_result);

    // Build the physical path: source_path + relative path
    SourceManager source_mgr(db);
    auto source_result = source_mgr.get_by_id(entry.source_id);
    if (is_err(source_result)) {
        LOG_ERROR("ISO scan: source not found");
        return false;
    }
    const SourceData& source = get_ok(source_result);

    auto path_result = entry_mgr.build_path(entry.id);
    std::string rel_path = is_ok(path_result) ? get_ok(path_result) : entry.name;

    // Construct physical path from source_path + relative
    std::string filepath;
    if (source.source_path.empty()) {
        filepath = rel_path;
    } else {
        filepath = source.source_path;
        if (filepath.back() != '/' && filepath.back() != '\\') {
            filepath += "/";
        }
        filepath += rel_path;
    }

    // Try UDF first, then ISO9660 (with Joliet).  Broken or non-standard
    // images may have a damaged UDF volume yet a readable ISO9660 tree
    // (e.g. hybrid discs), so fall back instead of giving up.
    UdfParser udf(filepath);
    if (udf.open()) {
        LOG_VERBOSE("Detected UDF: " + filepath);
        LOG_VERBOSE("Volume Identifier: " + udf.volume_identifier());
        LOG_VERBOSE("Filesystem: " + udf.filesystem_type());
        if (scan_udf(filepath, entry.source_id, container_entry_id, db, options)) {
            return true;
        }
        LOG_WARN("UDF parse failed for " + filepath +
                 ", falling back to ISO9660");
    }

    Iso9660Parser iso(filepath);
    if (iso.open()) {
        LOG_VERBOSE("Detected ISO9660: " + filepath);
        if (iso.volume_info().has_joliet) {
            LOG_VERBOSE("Joliet extension detected");
        }
        LOG_VERBOSE("Volume Identifier: " + iso.volume_info().volume_id);
        return scan_iso9660(filepath, entry.source_id, container_entry_id, db, options);
    }

    LOG_WARN("ISO scan: cannot parse file: " + filepath);
    return false;
}

// ── UDF walk ────────────────────────────────────────────────────────

bool IsoProvider::scan_udf(const std::string& filepath, int64_t source_id,
                           int64_t container_entry_id, Database& db,
                           const ContainerOptions& options) {
    UdfParser udf(filepath);
    if (!udf.open()) return false;

    std::vector<UdfEntry> root_entries;
    if (!udf.read_root_directory(root_entries)) {
        LOG_WARN("UDF: failed to read root directory of " + filepath);
        return false;
    }

    VirtualTreeWriter writer(db, source_id, container_entry_id);

    LOG_VERBOSE("UDF entry count: " + std::to_string(root_entries.size()));

    // Write virtual entries for the root directory
    for (const auto& e : root_entries) {
        if (!valid_iso_name(e.name)) {
            LOG_VERBOSE("UDF: skipping entry with invalid name (len=" +
                        std::to_string(e.name.size()) + ")");
            continue;
        }
        EntryData ed;
        ed.parent_id = container_entry_id;
        ed.name = e.name;
        ed.type = e.is_directory ? EntryType::Directory : EntryType::File;
        ed.size = e.size;
        ed.mtime = e.mtime;

        auto result = writer.add_entry(ed);
        if (is_err(result)) {
            LOG_WARN("UDF: failed to insert entry: " + e.name);
            continue;
        }
    }

    return true;
}

// ── ISO9660/Joliet walk ─────────────────────────────────────────────

bool IsoProvider::scan_iso9660(const std::string& filepath, int64_t source_id,
                               int64_t container_entry_id, Database& db,
                               const ContainerOptions& options) {
    VirtualTreeWriter writer(db, source_id, container_entry_id);

    // Prefer Joliet for names when present
    JolietParser joliet(filepath);
    if (joliet.open()) {
        std::vector<IsoEntry> root_entries;
        if (!joliet.read_root_directory(root_entries)) {
            LOG_WARN("Joliet: failed to read root directory of " + filepath);
            return false;
        }

        LOG_VERBOSE("Joliet entry count: " + std::to_string(root_entries.size()));

        for (const auto& e : root_entries) {
            if (!valid_iso_name(e.name)) {
                LOG_VERBOSE("Joliet: skipping entry with invalid name (len=" +
                            std::to_string(e.name.size()) + ")");
                continue;
            }
            EntryData ed;
            ed.parent_id = container_entry_id;
            ed.name = e.name;
            ed.type = e.is_directory ? EntryType::Directory : EntryType::File;
            ed.size = e.size;
            ed.mtime = e.mtime;

            auto result = writer.add_entry(ed);
            if (is_err(result)) {
                LOG_WARN("Joliet: failed to insert entry: " + e.name);
            }
        }
        return true;
    }

    // Fallback: plain ISO9660
    Iso9660Parser iso(filepath);
    if (!iso.open()) return false;

    std::vector<IsoEntry> root_entries;
    if (!iso.read_root_directory(root_entries)) {
        LOG_WARN("ISO9660: failed to read root directory of " + filepath);
        return false;
    }

    LOG_VERBOSE("ISO9660 entry count: " + std::to_string(root_entries.size()));

    for (const auto& e : root_entries) {
        if (!valid_iso_name(e.name)) {
            LOG_VERBOSE("ISO9660: skipping entry with invalid name (len=" +
                        std::to_string(e.name.size()) + ")");
            continue;
        }
        EntryData ed;
        ed.parent_id = container_entry_id;
        ed.name = e.name;
        ed.type = e.is_directory ? EntryType::Directory : EntryType::File;
        ed.size = e.size;
        ed.mtime = e.mtime;

        auto result = writer.add_entry(ed);
        if (is_err(result)) {
            LOG_WARN("ISO9660: failed to insert entry: " + e.name);
        }
    }

    return true;
}

bool IsoProvider::walk_directory(int64_t parent_id, Database& db,
                                 int64_t source_id,
                                 const ContainerOptions& options) {
    (void)parent_id;
    (void)db;
    (void)source_id;
    (void)options;
    // Full recursive walk is implemented in scan_udf / scan_iso9660
    return true;
}

// ── Registration ────────────────────────────────────────────────────

void register_iso_provider() {
    ProviderRegistry::instance().register_provider(
        std::make_shared<IsoProvider>());
    LOG_VERBOSE("ISO Provider registered");
}

} // namespace offcat
