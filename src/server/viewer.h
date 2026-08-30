// JSON API and frontend serving for the read-only catalog viewer.
//
// The frontend is replaceable: pass a web root directory (--web-root)
// and any static site implementing the JSON contract below can be
// served; otherwise the embedded index.html is used.  The contract is
// documented in docs/web-api.md.
#pragma once

#include <string>

#include "catalog/catalog.h"
#include "scanner/search.h"

namespace offcat {

struct HttpRequest;

class Viewer {
public:
    Viewer(Database& db, const std::string& web_root);

    // Dispatch one request; an empty body means "not found".
    std::string handle(const HttpRequest& req);
    bool is_api(const std::string& path) const;
    // Content type for a path that served a static asset.
    std::string content_type_for(const std::string& path) const;

private:
    Database& db_;
    std::string web_root_;
    SourceManager sources_;
    EntryManager entries_;
    ContainerManager containers_;
    ChecksumManager checksums_;
    SearchEngine search_;

    std::string entry_json(const EntryData& e);
    std::string api_stats();
    std::string api_sources();
    std::string api_tree(const std::string& query);
    std::string api_entry(const std::string& query);
    std::string api_search(const std::string& query);

    // Serve the frontend: --web-root files win, the embedded HTML is
    // the fallback so a single binary stays self-contained.
    std::string serve_index();
    // Read one file from web_root_ ("" when absent or unsafe).
    std::string serve_static(const std::string& path);
};

}  // namespace offcat
