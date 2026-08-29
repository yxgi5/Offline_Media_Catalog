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

// ISO provider registration (defined in providers/iso/iso_provider.cpp)
namespace offcat {
void register_iso_provider();
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
        "\n"
        "Scan options:\n"
        "  --containers      Scan and expand ISO containers\n"
        "  --depth <N>       Max container depth (default 1)\n"
        "  --sha256          Compute SHA-256 checksums\n"
        "  --md5             Compute MD5 checksums\n"
        "  --crc32           Compute CRC32 checksums\n"
        "  --checksum        Compute all checksums (sha256+md5+crc32)\n"
        "  --verbose         Verbose output\n"
        "  --debug           Debug output\n"
        "  --quiet           Minimal output\n";
}

static bool parse_checksum_flags(const std::vector<std::string>& args,
                                 ScanOptions& options,
                                 int& idx) {
    for (; idx < static_cast<int>(args.size()); idx++) {
        const std::string& arg = args[idx];
        if (arg == "--sha256") {
            options.compute_checksum = true;
            options.checksum_algorithms.push_back(ChecksumAlgorithm::SHA256);
        } else if (arg == "--md5") {
            options.compute_checksum = true;
            options.checksum_algorithms.push_back(ChecksumAlgorithm::MD5);
        } else if (arg == "--crc32") {
            options.compute_checksum = true;
            options.checksum_algorithms.push_back(ChecksumAlgorithm::CRC32);
        } else if (arg == "--checksum") {
            options.compute_checksum = true;
            options.checksum_algorithms.push_back(ChecksumAlgorithm::SHA256);
            options.checksum_algorithms.push_back(ChecksumAlgorithm::MD5);
            options.checksum_algorithms.push_back(ChecksumAlgorithm::CRC32);
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

int main(int argc, char* argv[]) {
    if (argc < 2) {
        print_usage(argv[0]);
        return 1;
    }

    std::string command = argv[1];
    std::vector<std::string> args;
    for (int i = 2; i < argc; i++) {
        args.push_back(argv[i]);
    }

    if (command == "create") {
        return cmd_create(args);
    } else if (command == "scan") {
        return cmd_scan(args);
    } else if (command == "search") {
        return cmd_search(args);
    } else if (command == "info") {
        return cmd_info(args);
    } else if (command == "help" || command == "--help" || command == "-h") {
        print_usage(argv[0]);
        return 0;
    }

    std::cerr << "Unknown command: " << command << "\n";
    print_usage(argv[0]);
    return 1;
}
