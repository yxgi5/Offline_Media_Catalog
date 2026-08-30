// Read-only HTTP server for the catalog viewer.
//
// Binds loopback only and serves the catalog through a small JSON API
// plus an optional frontend asset directory (see docs/web-api.md).
// The database is opened strictly read-only.
#pragma once

#include <string>

namespace offcat {

// Run the server until the process is terminated.  Returns 0 on a
// clean bind/listen setup; 1 on fatal errors.
//   db_path:  catalog database to serve (read-only)
//   port:     TCP port on 127.0.0.1
//   web_root: optional frontend asset directory; when empty the
//             embedded index.html is served
int run_server(const std::string& db_path, int port,
               const std::string& web_root);

}  // namespace offcat
