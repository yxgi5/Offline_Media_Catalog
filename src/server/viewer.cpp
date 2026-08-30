#include "server/viewer.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "server/http.h"
#include "web_resources.h"

namespace offcat {

namespace {

std::string entry_type_name(EntryType t) {
    return entry_type_to_string(t);
}

// Map a static asset path to a MIME type.
std::string static_content_type(const std::string& path) {
    auto ends_with = [&path](const char* suffix) {
        size_t n = std::char_traits<char>::length(suffix);
        return path.size() >= n &&
               path.compare(path.size() - n, n, suffix) == 0;
    };
    if (ends_with(".html")) return "text/html";
    if (ends_with(".css")) return "text/css";
    if (ends_with(".js")) return "text/javascript";
    if (ends_with(".mjs")) return "text/javascript";
    if (ends_with(".json")) return "application/json";
    if (ends_with(".png")) return "image/png";
    if (ends_with(".jpg") || ends_with(".jpeg")) return "image/jpeg";
    if (ends_with(".svg")) return "image/svg+xml";
    if (ends_with(".ico")) return "image/x-icon";
    if (ends_with(".woff2")) return "font/woff2";
    if (ends_with(".txt")) return "text/plain";
    return "application/octet-stream";
}

}  // namespace

Viewer::Viewer(Database& db, const std::string& web_root)
    : db_(db), web_root_(web_root), sources_(db), entries_(db),
      containers_(db), checksums_(db), search_(db) {}

std::string Viewer::handle(const HttpRequest& req) {
    if (req.path == "/" || req.path == "/index.html") {
        return serve_index();
    }
    if (req.path == "/api/stats") return api_stats();
    if (req.path == "/api/sources") return api_sources();
    if (req.path == "/api/tree") return api_tree(req.query);
    if (req.path == "/api/entry") return api_entry(req.query);
    if (req.path == "/api/search") return api_search(req.query);
    // Any other path may be a static asset of the external frontend.
    return serve_static(req.path);
}

bool Viewer::is_api(const std::string& path) const {
    return path.rfind("/api/", 0) == 0;
}

std::string Viewer::content_type_for(const std::string& path) const {
    // The index page has no .html suffix in its URL.
    if (path == "/" || path == "/index.html") return "text/html";
    return static_content_type(path);
}

std::string Viewer::entry_json(const EntryData& e) {
    bool is_container = false;
    auto c = containers_.is_container(e.id);
    if (is_ok(c)) is_container = get_ok(c);

    std::ostringstream j;
    j << "{\"id\":" << e.id
      << ",\"source_id\":" << e.source_id
      << ",\"parent_id\":" << e.parent_id
      << ",\"name\":\"" << json_escape(e.name) << "\""
      << ",\"type\":\"" << entry_type_name(e.type) << "\""
      << ",\"size\":" << e.size
      << ",\"mtime\":" << e.mtime
      << ",\"is_virtual\":" << (e.is_virtual ? "true" : "false")
      << ",\"is_container\":" << (is_container ? "true" : "false")
      << ",\"is_dir\":" << (e.type == EntryType::Directory ? "true" : "false")
      << "}";
    return j.str();
}

std::string Viewer::api_stats() {
    int64_t n_sources = 0, n_entries = 0, n_containers = 0;
    if (auto r = sources_.count(); is_ok(r)) n_sources = get_ok(r);
    if (auto r = entries_.count(); is_ok(r)) n_entries = get_ok(r);
    if (auto r = containers_.get_all(); is_ok(r)) n_containers = get_ok(r).size();
    std::ostringstream j;
    j << "{\"sources\":" << n_sources
      << ",\"entries\":" << n_entries
      << ",\"containers\":" << n_containers << "}";
    return j.str();
}

std::string Viewer::api_sources() {
    auto result = sources_.get_all();
    if (is_err(result)) return "[]";
    std::ostringstream j;
    j << "[";
    bool first = true;
    for (const auto& s : get_ok(result)) {
        int64_t count = 0;
        if (auto r = entries_.count_by_source(s.id); is_ok(r)) {
            count = get_ok(r);
        }
        if (!first) j << ",";
        first = false;
        j << "{\"id\":" << s.id
          << ",\"name\":\"" << json_escape(s.name) << "\""
          << ",\"type\":\"" << source_type_to_string(s.type) << "\""
          << ",\"path\":\"" << json_escape(s.source_path) << "\""
          << ",\"entries\":" << count << "}";
    }
    j << "]";
    return j.str();
}

std::string Viewer::api_tree(const std::string& query) {
    std::string parent_s, source_s;
    query_param(query, "parent_id", parent_s);
    query_param(query, "source_id", source_s);
    int64_t parent_id = parent_s.empty() ? 0 : std::stoll(parent_s);
    int64_t source_id = source_s.empty() ? -1 : std::stoll(source_s);

    std::vector<EntryData> children;
    if (parent_id == 0) {
        // Top level of one source: all root entries
        if (auto r = entries_.get_by_source(source_id); is_ok(r)) {
            for (const auto& e : get_ok(r)) {
                if (e.parent_id == 0) children.push_back(e);
            }
        }
    } else {
        if (auto r = entries_.get_children(parent_id); is_ok(r)) {
            children = get_ok(r);
        }
    }

    std::sort(children.begin(), children.end(),
              [](const EntryData& a, const EntryData& b) {
                  bool da = a.type == EntryType::Directory;
                  bool db = b.type == EntryType::Directory;
                  if (da != db) return da;
                  return a.name < b.name;
              });

    std::ostringstream j;
    j << "[";
    bool first = true;
    for (const auto& e : children) {
        if (!first) j << ",";
        first = false;
        j << entry_json(e);
    }
    j << "]";
    return j.str();
}

std::string Viewer::api_entry(const std::string& query) {
    std::string id_s;
    query_param(query, "id", id_s);
    if (id_s.empty()) return "{}";
    int64_t id = std::stoll(id_s);

    auto result = entries_.get_by_id(id);
    if (is_err(result)) return "{}";
    const EntryData& e = get_ok(result);

    auto path_result = entries_.build_path(id);
    std::string path = is_ok(path_result) ? get_ok(path_result) : "";

    std::ostringstream j;
    std::string base = entry_json(e);
    base.pop_back();  // drop closing brace; fields are appended below
    j << base;
    j << ",\"path\":\"" << json_escape(path) << "\"";
    j << ",\"ctime\":" << e.ctime
      << ",\"atime\":" << e.atime
      << ",\"birthtime\":" << e.birthtime
      << ",\"mode\":" << e.mode
      << ",\"attributes\":" << e.attributes;

    // Container info (if this entry is a container)
    if (auto c = containers_.get_by_entry_id(e.id); is_ok(c)) {
        const auto& cd = get_ok(c);
        j << ",\"container\":{\"type\":\"" << json_escape(cd.type)
          << "\",\"provider\":\"" << json_escape(cd.provider) << "\"}";
    } else {
        j << ",\"container\":null";
    }

    // Checksums
    j << ",\"checksums\":[";
    if (auto cs = checksums_.get_all_for_entry(e.id); is_ok(cs)) {
        bool first = true;
        for (const auto& c : get_ok(cs)) {
            if (!first) j << ",";
            first = false;
            j << "{\"algorithm\":\"" << checksum_algorithm_to_string(c.algorithm)
              << "\",\"value\":\"" << to_hex(c.value)
              << "\",\"calculated_at\":" << c.calculated_at << "}";
        }
    }
    j << "]}";

    return j.str();
}

std::string Viewer::api_search(const std::string& query) {
    std::string q, limit_s;
    query_param(query, "q", q);
    query_param(query, "limit", limit_s);
    if (q.empty()) return "[]";
    int limit = limit_s.empty() ? 200 : std::stoi(limit_s);

    auto result = search_.search(q, limit);
    if (is_err(result)) {
        std::ostringstream j;
        j << "{\"error\":\"" << json_escape(get_err(result).message) << "\"}";
        return j.str();
    }
    std::ostringstream j;
    j << "[";
    bool first = true;
    for (const auto& r : get_ok(result)) {
        if (!first) j << ",";
        first = false;
        j << "{\"entry_id\":" << r.entry_id
          << ",\"entry_name\":\"" << json_escape(r.entry_name) << "\""
          << ",\"full_path\":\"" << json_escape(r.full_path) << "\""
          << ",\"source_name\":\"" << json_escape(r.source_name) << "\""
          << ",\"type\":\"" << entry_type_name(r.type) << "\""
          << ",\"size\":" << r.size
          << ",\"is_virtual\":" << (r.is_virtual ? "true" : "false")
          << ",\"container_type\":\"" << json_escape(r.container_type) << "\"}";
    }
    j << "]";
    return j.str();
}

std::string Viewer::serve_index() {
    std::string external = serve_static("index.html");
    if (!external.empty()) return external;
    return std::string(resources::kWebIndexHtml);
}

std::string Viewer::serve_static(const std::string& path) {
    if (web_root_.empty()) return "";
    // Normalize: strip leading slashes, reject parent traversal.
    std::string rel = path;
    while (!rel.empty() && rel.front() == '/') rel.erase(rel.begin());
    if (rel.empty()) return "";
    size_t pos = 0;
    while (pos <= rel.size()) {
        size_t slash = rel.find('/', pos);
        std::string seg = rel.substr(
            pos, slash == std::string::npos ? std::string::npos : slash - pos);
        if (seg == "..") return "";
        if (slash == std::string::npos) break;
        pos = slash + 1;
    }

    std::error_code ec;
    std::filesystem::path full = std::filesystem::path(web_root_) / rel;
    if (!std::filesystem::is_regular_file(full, ec)) return "";
    std::ifstream in(full, std::ios::binary);
    if (!in) return "";
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

}  // namespace offcat
