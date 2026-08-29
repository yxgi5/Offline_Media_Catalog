#pragma once

#include "core/types.h"
#include "database/database.h"
#include "catalog/catalog.h"
#include <string>
#include <vector>
#include <memory>

namespace offcat {

// ── Search result ───────────────────────────────────────────────────

struct SearchResult {
    int64_t entry_id = 0;
    std::string source_name;
    std::string entry_name;
    std::string full_path;
    EntryType type = EntryType::File;
    int64_t size = 0;
    bool is_virtual = false;
    std::string container_type;  // Empty if not in a container
};

// ── Search engine ───────────────────────────────────────────────────

class SearchEngine {
public:
    explicit SearchEngine(Database& db);

    // Full-text search across name and path
    Result<std::vector<SearchResult>> search(const std::string& query,
                                              int limit = 100);

    // Search by filename only
    Result<std::vector<SearchResult>> search_by_name(const std::string& query,
                                                      int limit = 100);

    // Search by path
    Result<std::vector<SearchResult>> search_by_path(const std::string& query,
                                                      int limit = 100);

private:
    Database& db_;
    EntryManager entry_mgr_;
    SourceManager source_mgr_;

    // Convert a raw FTS result into a SearchResult
    Result<SearchResult> build_search_result(int64_t entry_id,
                                              const std::string& source_name);
};

} // namespace offcat
