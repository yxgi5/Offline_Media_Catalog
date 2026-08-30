#include "core/logger.h"
#include "core/types.h"
#include "core/checksum.h"
#include "database/database.h"
#include "catalog/catalog.h"
#include "scanner/scanner.h"
#include "scanner/search.h"
#include "container/provider.h"

#include <iostream>
#include <string>
#include <vector>
#include <cstring>
#include <csignal>
#include <chrono>
#include <sstream>
#include <iomanip>

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#endif

// ISO provider registration (defined in providers/iso/iso_provider.cpp)
namespace offcat {
void register_iso_provider();
}

// Read-only web viewer (defined in cli/serve.cpp)
namespace offcat {
int cmd_serve(const std::vector<std::string>& args);
}

using namespace offcat;

static CancellationManager g_cancel;

static void signal_handler(int /*signum*/) {
    g_cancel.request_cancel();
    Logger::instance().warning("Cancellation requested (Ctrl+C). Finishing safely...");
}

// ── Helpers ─────────────────────────────────────────────────────────

static void print_usage(const char* prog) {
    std::cout <<
        "offcat - Offline Media Catalog\n"
        "Usage:\n"
        "  " << prog << " create <catalog.db>                    Create a new empty catalog\n"
        "  " << prog << " scan [options] <path> <catalog.db>     Scan a source into the catalog\n"
        "  " << prog << " search <catalog.db> <query>            Search filenames/paths\n"
        "  " << prog << " info <catalog.db>                      Show catalog statistics\n"
        "  " << prog << " serve [--port <N>] [--web-root <dir>] <catalog.db>  Read-only web viewer (default port 8080)\n"
        "\n"
        "Scan options:\n"
        "  --containers      Scan and expand ISO containers\n"
        "  --depth <N>       Container nesting depth (0 = do not expand;\n"
                "                    1 = expand top-level containers with full\n"
                "                    contents; 2+ also expands containers nested\n"
                "                    inside them; default 1)\n"
        "  --checksum <spec> Checksums to compute: comma-separated list of\n"
        "                    sha256|md5|crc32, or all/none; repeatable\n"
        "                    (bare --checksum = all; none disables)\n"
        "  --progress        Show live scan progress (default on)\n"
        "  --no-progress     Disable progress output\n"
        "  --verbose         Verbose output\n"
        "  --debug           Debug output\n"
        "  --quiet           Minimal output\n";
}

static void add_algorithm(ScanOptions& options, ChecksumAlgorithm algo) {
    for (auto existing : options.checksum_algorithms) {
        if (existing == algo) return;  // deduplicate
    }
    options.checksum_algorithms.push_back(algo);
    options.compute_checksum = true;
}

// Apply one --checksum spec (already comma-split tokens).  Returns
// false on an unknown algorithm name.
static bool apply_checksum_spec(const std::vector<std::string>& tokens,
                                ScanOptions& options) {
    for (const auto& t : tokens) {
        if (t == "none") {
            options.checksum_algorithms.clear();
            options.compute_checksum = false;
        } else if (t == "all") {
            add_algorithm(options, ChecksumAlgorithm::SHA256);
            add_algorithm(options, ChecksumAlgorithm::MD5);
            add_algorithm(options, ChecksumAlgorithm::CRC32);
        } else if (t == "sha256") {
            add_algorithm(options, ChecksumAlgorithm::SHA256);
        } else if (t == "md5") {
            add_algorithm(options, ChecksumAlgorithm::MD5);
        } else if (t == "crc32") {
            add_algorithm(options, ChecksumAlgorithm::CRC32);
        } else {
            return false;
        }
    }
    return true;
}

// Validate a spec (already comma-split tokens) without touching
// options, so a partially valid list cannot be half-applied.
static bool is_valid_spec(const std::vector<std::string>& tokens) {
    for (const auto& t : tokens) {
        if (t != "none" && t != "all" && t != "sha256" &&
            t != "md5" && t != "crc32") {
            return false;
        }
    }
    return true;
}

// Split "sha256,crc32" into tokens, trimming whitespace around each.
static std::vector<std::string> split_csv(const std::string& s) {
    std::vector<std::string> out;
    size_t start = 0;
    while (start <= s.size()) {
        size_t comma = s.find(',', start);
        std::string token = comma == std::string::npos
            ? s.substr(start)
            : s.substr(start, comma - start);
        while (!token.empty() && (token.front() == ' ' || token.front() == '\t')) {
            token.erase(token.begin());
        }
        while (!token.empty() && (token.back() == ' ' || token.back() == '\t')) {
            token.pop_back();
        }
        out.push_back(token);
        if (comma == std::string::npos) break;
        start = comma + 1;
    }
    return out;
}

static bool parse_checksum_flags(const std::vector<std::string>& args,
                                 ScanOptions& options,
                                 int& idx) {
    for (; idx < static_cast<int>(args.size()); idx++) {
        const std::string& arg = args[idx];
        if (arg == "--checksum") {
            // --checksum [spec]: consume the next argument only when it
            // parses as a valid spec; a bare --checksum keeps the legacy
            // meaning (all algorithms).  Specs are repeatable and the
            // results are merged (deduplicated).
            bool consumed = false;
            if (idx + 1 < static_cast<int>(args.size())) {
                auto tokens = split_csv(args[idx + 1]);
                if (is_valid_spec(tokens)) {
                    apply_checksum_spec(tokens, options);
                    idx++;
                    consumed = true;
                } else if (args[idx + 1].find(',') != std::string::npos) {
                    // A comma list with an unknown algorithm name is
                    // clearly a bad spec, not a path.
                    std::cerr << "Error: invalid --checksum spec: "
                              << args[idx + 1] << "\n";
                    return false;
                }
            }
            if (!consumed) {
                apply_checksum_spec({"all"}, options);
            }
        } else if (arg == "--containers") {
            options.scan_containers = true;
        } else if (arg == "--depth") {
            if (idx + 1 >= static_cast<int>(args.size())) {
                std::cerr << "Error: --depth requires a value\n";
                return false;
            }
            try {
                options.max_container_depth = std::stoi(args[++idx]);
            } catch (...) {
                std::cerr << "Error: invalid --depth value\n";
                return false;
            }
        } else if (arg == "--verbose") {
            Logger::instance().set_level(LogLevel::Verbose);
        } else if (arg == "--debug") {
            Logger::instance().set_level(LogLevel::Debug);
        } else if (arg == "--quiet") {
            Logger::instance().set_level(LogLevel::Quiet);
            options.show_progress = false;
        } else if (arg == "--progress") {
            options.show_progress = true;
        } else if (arg == "--no-progress") {
            options.show_progress = false;
        } else {
            // Non-flag argument: path
            return true;
        }
    }
    return true;
}

// ── Commands ────────────────────────────────────────────────────────

static int cmd_create(const std::vector<std::string>& args) {
    if (args.empty()) {
        std::cerr << "Error: offcat create requires a database path\n";
        return 1;
    }

    Database db;
    auto result = db.create(args[0]);
    if (is_err(result)) {
        std::cerr << "Error: " << get_err(result).message << "\n";
        return 1;
    }

    LOG_INFO("Created catalog: " + args[0]);
    return 0;
}

static int cmd_scan(const std::vector<std::string>& args) {
    ScanOptions options;
    int idx = 0;
    if (!parse_checksum_flags(args, options, idx)) return 1;

    // Remaining: <path> <catalog.db>
    std::vector<std::string> positional;
    for (; idx < static_cast<int>(args.size()); idx++) {
        positional.push_back(args[idx]);
    }

    if (positional.size() < 2) {
        std::cerr << "Error: offcat scan requires <path> and <catalog.db>\n";
        return 1;
    }

    const std::string& path = positional[0];
    const std::string& db_path = positional[1];

    // Register providers
    register_iso_provider();

    Database db;
    auto open_result = db.open(db_path);
    if (is_err(open_result)) {
        std::cerr << "Error: " << get_err(open_result).message << "\n";
        return 1;
    }

    // Ensure schema exists
    db.initialize_schema();

    // Set up Ctrl+C handling
    std::signal(SIGINT, signal_handler);
#ifdef SIGBREAK
    std::signal(SIGBREAK, signal_handler);
#endif

    auto started = std::chrono::steady_clock::now();

    Scanner scanner(db, g_cancel);
    auto result = scanner.scan_source(path, options);
    if (is_err(result)) {
        auto& err = get_err(result);
        if (err.code == 0) {
            LOG_INFO("Scan cancelled.");
            return 130;
        }
        std::cerr << "Error: " << err.message << "\n";
        return 1;
    }

    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - started).count();

    // Summary
    std::ostringstream summary;
    summary << "Source ID: " << get_ok(result) << "\n"
            << "Files: " << scanner.files_scanned() << "\n"
            << "Directories: " << scanner.directories_scanned() << "\n"
            << "Total size: " << scanner.total_size() << " bytes\n"
            << "Errors: " << scanner.errors_count() << "\n"
            << "Elapsed: " << elapsed << "s";
    LOG_INFO(summary.str());

    return g_cancel.is_cancelled() ? 130 : 0;
}

static int cmd_search(const std::vector<std::string>& args) {
    if (args.size() < 2) {
        std::cerr << "Error: offcat search requires <catalog.db> and <query>\n";
        return 1;
    }

    Database db;
    auto open_result = db.open(args[0]);
    if (is_err(open_result)) {
        std::cerr << "Error: " << get_err(open_result).message << "\n";
        return 1;
    }
    db.initialize_schema();

    SearchEngine engine(db);
    auto results = engine.search(args[1], 200);
    if (is_err(results)) {
        std::cerr << "Error: " << get_err(results).message << "\n";
        return 1;
    }

    const auto& found = get_ok(results);
    if (found.empty()) {
        LOG_INFO("No results for \"" + args[1] + "\"");
        return 0;
    }

    for (const auto& r : found) {
        std::cout << r.source_name << "\n";
        std::cout << "  " << r.full_path;
        if (!r.container_type.empty()) {
            std::cout << "  [container: " << r.container_type << "]";
        }
        if (r.is_virtual) {
            std::cout << "  [virtual]";
        }
        std::cout << "\n";
    }

    return 0;
}

static int cmd_info(const std::vector<std::string>& args) {
    if (args.empty()) {
        std::cerr << "Error: offcat info requires a database path\n";
        return 1;
    }

    Database db;
    auto open_result = db.open(args[0]);
    if (is_err(open_result)) {
        std::cerr << "Error: " << get_err(open_result).message << "\n";
        return 1;
    }
    db.initialize_schema();

    SourceManager source_mgr(db);
    EntryManager entry_mgr(db);
    ContainerManager container_mgr(db);
    ScanManager scan_mgr(db);

    auto sources = source_mgr.get_all();
    auto entries = entry_mgr.count();
    auto containers = container_mgr.get_all();

    if (is_err(sources) || is_err(entries)) {
        std::cerr << "Error: failed to read catalog statistics\n";
        return 1;
    }

    std::cout << "Catalog: " << args[0] << "\n";
    std::cout << "Sources: " << get_ok(sources).size() << "\n";
    std::cout << "Entries: " << get_ok(entries) << "\n";
    std::cout << "Containers: " << get_ok(containers).size() << "\n";
    std::cout << "\nSources:\n";
    for (const auto& s : get_ok(sources)) {
        auto count = entry_mgr.count_by_source(s.id);
        std::cout << "  [" << source_type_to_string(s.type) << "] " << s.name
                  << " (" << (is_ok(count) ? std::to_string(get_ok(count)) : "?")
                  << " entries)";
        if (!s.source_path.empty()) {
            std::cout << " <- " << s.source_path;
        }
        std::cout << "\n";
    }

    return 0;
}

// ── Main ────────────────────────────────────────────────────────────

// On Windows, PowerShell/cmd pass arguments encoded in the ANSI code
// page (e.g. GBK), but the codebase assumes UTF-8 paths.  Rebuild argv
// from GetCommandLineW so non-ASCII arguments arrive as UTF-8 regardless
// of the console code page; std::filesystem (and our own UTF-8 helpers)
// then decode them correctly.
#ifdef _WIN32
static std::vector<std::string> utf8_argv() {
    int argc = 0;
    LPWSTR* wargv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::vector<std::string> result;
    if (!wargv) return result;
    for (int i = 0; i < argc; i++) {
        int len = WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1,
                                      nullptr, 0, nullptr, nullptr);
        std::string s;
        if (len > 1) {
            s.resize(len - 1);
            WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, s.data(), len,
                                nullptr, nullptr);
        }
        result.push_back(s);
    }
    LocalFree(wargv);
    return result;
}
#endif

int main(int argc, char* argv[]) {
    std::vector<std::string> args_all;
#ifdef _WIN32
    args_all = utf8_argv();
    if (args_all.empty()) args_all.assign(argv, argv + argc);
#else
    args_all.assign(argv, argv + argc);
#endif
    if (args_all.size() < 2) {
        print_usage(args_all.empty() ? argv[0] : args_all[0].c_str());
        return 1;
    }

    const std::string& command = args_all[1];
    std::vector<std::string> args(args_all.begin() + 2, args_all.end());

    if (command == "create") {
        return cmd_create(args);
    } else if (command == "scan") {
        return cmd_scan(args);
    } else if (command == "search") {
        return cmd_search(args);
    } else if (command == "info") {
        return cmd_info(args);
    } else if (command == "serve") {
        return cmd_serve(args);
    } else if (command == "help" || command == "--help" || command == "-h") {
        print_usage(argv[0]);
        return 0;
    }

    std::cerr << "Unknown command: " << command << "\n";
    print_usage(argv[0]);
    return 1;
}
