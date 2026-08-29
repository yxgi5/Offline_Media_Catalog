#include "iso_provider.h"
#include "iso9660/iso9660_parser.h"
#include "joliet/joliet_parser.h"
#include "udf/udf_parser.h"
#include "udf/udf_unicode.h"
#include "core/logger.h"
#include "catalog/catalog.h"

#include <cctype>
#include <algorithm>
#include <filesystem>

namespace offcat {

namespace {

// Rock Ridge relocates deep directories (paths with more than 8 name
// components) into a relocation directory at the root (rr_moved or
// .rr_moved) and leaves a single-byte placeholder (0x02-0x09) in the
// original location.  With Rock Ridge parsed, placeholders are resolved
// back to their real directories (see resolve_placeholder); entries
// that still cannot be resolved are skipped rather than catalogued as
// garbage.
bool valid_iso_name(const std::string& name) {
    if (name.empty()) return false;
    for (unsigned char c : name) {
        if (c < 0x20) return false;
    }
    return true;
}

// Contents of the Rock Ridge relocation directory.
struct RrRelocation {
    bool present = false;
    std::vector<IsoEntry> dirs;  // real directory entries, in order
};

// Map a placeholder entry to its real directory inside the relocation
// directory.  Writers differ: some name the moved directories with the
// digit string, others rely on positional mapping (kernel isofs maps
// placeholder N to record N of the relocation directory data, where
// records 0/1 are "."/"..", so the real index is N-2).
const IsoEntry* resolve_placeholder(const IsoEntry& ph,
                                    const RrRelocation& rr) {
    if (!rr.present) return nullptr;

    // Strategy 1: relocation dir contains a directory named after the
    // placeholder digit ("2".."9").
    std::string digit = std::to_string(ph.rr_placeholder);
    for (const auto& d : rr.dirs) {
        if (d.name == digit) return &d;
    }

    // Strategy 2: positional mapping (entries already exclude "."/"..").
    size_t index = static_cast<size_t>(ph.rr_placeholder - 2);
    if (index < rr.dirs.size()) return &rr.dirs[index];

    return nullptr;
}

// Recursively expand a directory, writing virtual entries.  `reader`
// enumerates a directory extent (ISO9660 or Joliet).  Rock Ridge
// placeholders are resolved through `rr` before insertion.
using DirReader = std::function<bool(int64_t, int64_t,
                                     std::vector<IsoEntry>&)>;
void walk_iso_directory(const std::vector<IsoEntry>& entries,
                        const RrRelocation& rr,
                        VirtualTreeWriter& writer, int64_t parent_id,
                        int depth, int max_depth, int& count,
                        const DirReader& reader) {
    for (const auto& orig : entries) {
        IsoEntry e = orig;

        // Deep-directory placeholder: substitute the real entry from
        // the relocation directory when resolvable.
        if (e.is_rr_placeholder) {
            const IsoEntry* real = resolve_placeholder(e, rr);
            if (!real) {
                LOG_VERBOSE("ISO: skipping unresolvable rr_moved placeholder " +
                            e.name);
                continue;
            }
            e = *real;
        }

        if (!valid_iso_name(e.name)) {
            LOG_VERBOSE("ISO: skipping entry with invalid name (len=" +
                        std::to_string(e.name.size()) + ")");
            continue;
        }

        EntryData ed;
        ed.parent_id = parent_id;
        ed.name = e.name;
        ed.type = e.is_symlink ? EntryType::Symlink
                 : (e.is_directory ? EntryType::Directory : EntryType::File);
        ed.size = e.size;
        ed.mtime = e.mtime;
        ed.mode = e.mode;

        auto result = writer.add_entry(ed);
        if (is_err(result)) {
            LOG_WARN("ISO: failed to insert entry: " + e.name);
            continue;
        }
        count++;
        int64_t entry_id = get_ok(result);

        if (e.is_directory && depth < max_depth) {
            std::vector<IsoEntry> children;
            if (reader(e.extent, e.size, children)) {
                walk_iso_directory(children, rr, writer, entry_id,
                                   depth + 1, max_depth, count, reader);
            } else {
                LOG_WARN("ISO: failed to read directory: " + e.name);
            }
        }
    }
}

// Recursively expand a UDF directory.
void walk_udf_directory(const std::vector<UdfEntry>& entries,
                        VirtualTreeWriter& writer, int64_t parent_id,
                        int depth, int max_depth, int& count,
                        UdfParser& udf) {
    for (const auto& e : entries) {
        if (!valid_iso_name(e.name)) {
            LOG_VERBOSE("UDF: skipping entry with invalid name (len=" +
                        std::to_string(e.name.size()) + ")");
            continue;
        }

        EntryData ed;
        ed.parent_id = parent_id;
        ed.name = e.name;
        ed.type = e.is_directory ? EntryType::Directory : EntryType::File;
        ed.size = e.size;
        ed.mtime = e.mtime;

        // genisoimage images write a constant 2048 in the FID ICB
        // extent length; the real size lives in the file entry.
        if (!e.is_directory) {
            int64_t extent = 0, file_size = 0, file_mtime = 0;
            bool file_is_dir = false;
            std::vector<UdfLongAd> ads;
            if (udf.read_file_entry(e.extent_location, e.partition_ref,
                                    extent, file_size, file_is_dir,
                                    file_mtime, ads) &&
                file_size > 0) {
                ed.size = file_size;
            }
        }

        auto result = writer.add_entry(ed);
        if (is_err(result)) {
            LOG_WARN("UDF: failed to insert entry: " + e.name);
            continue;
        }
        count++;
        int64_t entry_id = get_ok(result);

        if (e.is_directory && depth < max_depth) {
            UdfLongAd icb;
            icb.extent_length = static_cast<uint32_t>(e.size);
            icb.location = static_cast<uint32_t>(e.extent_location);
            icb.partition_ref = e.partition_ref;
            std::vector<UdfEntry> children;
            if (udf.read_directory(icb, children, depth)) {
                walk_udf_directory(children, writer, entry_id,
                                   depth + 1, max_depth, count, udf);
            } else {
                LOG_WARN("UDF: failed to read directory: " + e.name);
            }
        }
    }
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

    // Construct physical path from source_path + relative.  A source
    // that IS a container file (single-file scan) carries the full file
    // path in source_path; only directory sources need the relative
    // path appended.  Judge by the path itself rather than the declared
    // type: callers may register an ISO source whose source_path is a
    // directory holding the image.
    std::string filepath;
    std::error_code fs_ec;
    if (!source.source_path.empty() &&
        std::filesystem::is_directory(source.source_path, fs_ec)) {
        filepath = source.source_path;
        if (filepath.back() != '/' && filepath.back() != '\\') {
            filepath += "/";
        }
        filepath += rel_path;
    } else if (!source.source_path.empty()) {
        filepath = source.source_path;
    } else {
        filepath = rel_path;
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

    int inserted = 0;
    int max_depth = options.max_depth > 0 ? options.max_depth : 1;
    walk_udf_directory(root_entries, writer, container_entry_id, 1,
                       max_depth, inserted, udf);
    LOG_VERBOSE("UDF inserted: " + std::to_string(inserted) + " entries");
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

        DirReader reader = [&joliet](int64_t extent, int64_t size,
                                     std::vector<IsoEntry>& out) {
            return joliet.read_directory(extent, size, out);
        };
        RrRelocation no_rr;  // Joliet trees carry no Rock Ridge
        int inserted = 0;
        int max_depth = options.max_depth > 0 ? options.max_depth : 1;
        walk_iso_directory(root_entries, no_rr, writer, container_entry_id,
                           1, max_depth, inserted, reader);
        LOG_VERBOSE("Joliet inserted: " + std::to_string(inserted) + " entries");
        return true;
    }

    // Fallback: plain ISO9660, optionally with Rock Ridge
    Iso9660Parser iso(filepath);
    if (!iso.open()) return false;

    std::vector<IsoEntry> root_entries;
    if (!iso.read_root_directory(root_entries)) {
        LOG_WARN("ISO9660: failed to read root directory of " + filepath);
        return false;
    }

    // Locate the Rock Ridge relocation directory (rr_moved or .rr_moved)
    RrRelocation rr;
    for (const auto& e : root_entries) {
        if (e.is_directory && (e.name == "rr_moved" || e.name == ".rr_moved")) {
            rr.present = true;
            if (!iso.read_directory(e.extent, e.size, rr.dirs)) {
                LOG_WARN("ISO9660: failed to read rr_moved directory");
            }
            break;
        }
    }

    LOG_VERBOSE("ISO9660 entry count: " + std::to_string(root_entries.size()));
    if (rr.present) {
        LOG_VERBOSE("ISO9660: Rock Ridge relocation directory with " +
                    std::to_string(rr.dirs.size()) + " entries");
    }

    DirReader reader = [&iso](int64_t extent, int64_t size,
                              std::vector<IsoEntry>& out) {
        return iso.read_directory(extent, size, out);
    };
    int inserted = 0;
    int max_depth = options.max_depth > 0 ? options.max_depth : 1;
    walk_iso_directory(root_entries, rr, writer, container_entry_id,
                       1, max_depth, inserted, reader);
    LOG_VERBOSE("ISO9660 inserted: " + std::to_string(inserted) + " entries");
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
