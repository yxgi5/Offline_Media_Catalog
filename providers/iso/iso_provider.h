#pragma once

#include "container/provider.h"
#include <string>

namespace offcat {

// ── ISO Container Provider ──────────────────────────────────────────
//
// Supports ISO9660 / Joliet / UDF.
// Filesystem selection: UDF > Joliet > ISO9660.
// Parsing is done directly on the file (no mounting).

class IsoProvider : public ContainerProvider {
public:
    std::string type() const override { return "iso"; }

    // Detect ISO files by magic
    bool probe(const std::string& filepath) override;

    // Scan container contents into the database as virtual entries
    bool scan(int64_t container_entry_id,
              Database& db,
              const ContainerOptions& options) override;

private:
    // Walk a directory tree, writing virtual entries
    bool walk_directory(int64_t parent_id, Database& db,
                        int64_t source_id,
                        const ContainerOptions& options);

    // Walks using UDF parser
    bool scan_udf(const std::string& filepath, int64_t source_id,
                  int64_t container_entry_id, Database& db,
                  const ContainerOptions& options);

    // Walks using ISO9660/Joliet parser
    bool scan_iso9660(const std::string& filepath, int64_t source_id,
                      int64_t container_entry_id, Database& db,
                      const ContainerOptions& options);

    // Expand a container image extracted from inside a parent image
    void expand_nested(const std::string& filepath, int64_t container_entry_id,
                       int64_t source_id, Database& db,
                       const ContainerOptions& options, int next_depth);
};

// Register ISO provider with the global registry
void register_iso_provider();

} // namespace offcat
