// offcat serve - read-only web viewer for a catalog database.
//
// Thin CLI wrapper: parses command line options and delegates to the
// HTTP server in src/server.  The frontend is replaceable via
// --web-root (any static site implementing the JSON API contract in
// docs/web-api.md); without it the embedded frontend is served.

#include <iostream>
#include <string>
#include <vector>

#include "server/server.h"

namespace offcat {

int cmd_serve(const std::vector<std::string>& args) {
    int port = 8080;
    std::string web_root;
    std::string db_path;
    for (size_t i = 0; i < args.size(); i++) {
        if (args[i] == "--port" && i + 1 < args.size()) {
            port = std::stoi(args[++i]);
        } else if (args[i] == "--web-root" && i + 1 < args.size()) {
            web_root = args[++i];
        } else if (db_path.empty()) {
            db_path = args[i];
        }
    }
    if (db_path.empty()) {
        std::cerr << "Error: offcat serve requires <catalog.db>\n"
                  << "Usage: offcat serve [--port <N>] [--web-root <dir>] <catalog.db>\n";
        return 1;
    }
    return run_server(db_path, port, web_root);
}

}  // namespace offcat
