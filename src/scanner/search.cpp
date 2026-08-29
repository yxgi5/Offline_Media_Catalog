#include "scanner/search.h"
#include "core/logger.h"
#include <cctype>

namespace offcat {

namespace {

// FTS5 unicode61 tokenizes text into terms; runs of CJK characters
// form a single term (unlike a per-character split). Build a MATCH
// expression from a user query by splitting on non-alphanumeric
// characters and adding a prefix wildcard to every term.

// Decode one UTF-8 sequence; advances *pos past it. Returns 0xFFFD on
// malformed input.
uint32_t decode_utf8_cp(const std::string& s, size_t* pos) {
    const unsigned char* p =
        reinterpret_cast<const unsigned char*>(s.data()) + *pos;
    size_t remaining = s.size() - *pos;
    uint32_t cp = 0;
    size_t need = 0;
    if (remaining >= 1 && p[0] < 0x80) {
        cp = p[0]; need = 1;
    } else if (remaining >= 2 && (p[0] & 0xE0) == 0xC0) {
        cp = p[0] & 0x1F; need = 2;
    } else if (remaining >= 3 && (p[0] & 0xF0) == 0xE0) {
        cp = p[0] & 0x0F; need = 3;
    } else if (remaining >= 4 && (p[0] & 0xF8) == 0xF0) {
        cp = p[0] & 0x07; need = 4;
    } else {
        *pos += 1;
        return 0xFFFD;
    }
    for (size_t i = 1; i < need; i++) {
        if ((p[i] & 0xC0) != 0x80) { *pos += 1; return 0xFFFD; }
        cp = (cp << 6) | (p[i] & 0x3F);
    }
    *pos += need;
    return cp;
}

// Append a codepoint as UTF-8
void append_utf8_cp(uint32_t cp, std::string& out) {
    if (cp < 0x80) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

// Build an FTS5 MATCH expression from a user query: every term gets a
// prefix wildcard; CJK runs are kept whole (unicode61 keeps them as a
// single term).
std::string build_fts_query(const std::string& query) {
    std::string result;
    std::string word;
    auto flush = [&result, &word]() {
        if (!word.empty()) {
            if (!result.empty()) result += " ";
            result += word;
            result += '*';
            word.clear();
        }
    };

    size_t pos = 0;
    while (pos < query.size()) {
        uint32_t cp = decode_utf8_cp(query, &pos);
        if (cp >= 0x80) {
            // Non-ASCII (including CJK): part of the current term
            append_utf8_cp(cp, word);
        } else if (std::isalnum(static_cast<unsigned char>(cp))) {
            word.push_back(static_cast<char>(cp));
        } else {
            flush();  // punctuation/space: term separator
        }
    }
    flush();
    return result;
}

} // namespace

SearchEngine::SearchEngine(Database& db)
    : db_(db), entry_mgr_(db), source_mgr_(db) {}

Result<std::vector<SearchResult>> SearchEngine::search(const std::string& query,
                                                         int limit) {
    // FTS5 search across name, path, and source_name
    Statement stmt(db_,
        "SELECT rowid, name, path, source_name, "
        "rank "
        "FROM entry_fts "
        "WHERE entry_fts MATCH ? "
        "ORDER BY rank "
        "LIMIT ?");
    if (!stmt.is_valid()) return Error{1, "Failed to prepare FTS query"};

    stmt.bind_text(1, build_fts_query(query));
    stmt.bind_int(2, limit);

    std::vector<SearchResult> results;
    while (stmt.step()) {
        int64_t entry_id = stmt.column_int64(0);
        std::string source_name = stmt.column_text(3);

        auto sr_result = build_search_result(entry_id, source_name);
        if (is_ok(sr_result)) {
            results.push_back(get_ok(sr_result));
        }
    }
    return results;
}

Result<std::vector<SearchResult>> SearchEngine::search_by_name(
    const std::string& query, int limit) {
    Statement stmt(db_,
        "SELECT rowid, name, source_name "
        "FROM entry_fts "
        "WHERE name MATCH ? "
        "ORDER BY rank "
        "LIMIT ?");
    if (!stmt.is_valid()) return Error{1, "Failed to prepare name search"};

    stmt.bind_text(1, build_fts_query(query));
    stmt.bind_int(2, limit);

    std::vector<SearchResult> results;
    while (stmt.step()) {
        int64_t entry_id = stmt.column_int64(0);
        std::string source_name = stmt.column_text(2);

        auto sr_result = build_search_result(entry_id, source_name);
        if (is_ok(sr_result)) {
            results.push_back(get_ok(sr_result));
        }
    }
    return results;
}

Result<std::vector<SearchResult>> SearchEngine::search_by_path(
    const std::string& query, int limit) {
    Statement stmt(db_,
        "SELECT rowid, path, source_name "
        "FROM entry_fts "
        "WHERE path MATCH ? "
        "ORDER BY rank "
        "LIMIT ?");
    if (!stmt.is_valid()) return Error{1, "Failed to prepare path search"};

    stmt.bind_text(1, build_fts_query(query));
    stmt.bind_int(2, limit);

    std::vector<SearchResult> results;
    while (stmt.step()) {
        int64_t entry_id = stmt.column_int64(0);
        std::string source_name = stmt.column_text(2);

        auto sr_result = build_search_result(entry_id, source_name);
        if (is_ok(sr_result)) {
            results.push_back(get_ok(sr_result));
        }
    }
    return results;
}

Result<SearchResult> SearchEngine::build_search_result(
    int64_t entry_id, const std::string& source_name) {
    auto entry_result = entry_mgr_.get_by_id(entry_id);
    if (is_err(entry_result)) {
        return Error{1, "Entry not found for search result"};
    }
    const auto& entry = get_ok(entry_result);

    SearchResult sr;
    sr.entry_id = entry_id;
    sr.entry_name = entry.name;
    sr.type = entry.type;
    sr.size = entry.size;
    sr.is_virtual = entry.is_virtual;

    // FTS contentless tables (content='') do not store column values,
    // so source_name from the FTS row is empty; fetch it from the
    // source table instead.
    std::string effective_source_name = source_name;
    if (effective_source_name.empty()) {
        auto src_result = source_mgr_.get_by_id(entry.source_id);
        if (is_ok(src_result)) {
            effective_source_name = get_ok(src_result).name;
        }
    }
    sr.source_name = effective_source_name;

    // Build full path
    auto path_result = entry_mgr_.build_path(entry_id);
    sr.full_path = is_ok(path_result) ? get_ok(path_result) : entry.name;

    // Check if inside a container
    auto container_result = entry_mgr_.get_by_id(entry.parent_id);
    if (is_ok(container_result)) {
        Statement stmt(db_, "SELECT type FROM container WHERE entry_id=?");
        if (stmt.is_valid()) {
            stmt.bind_int64(1, entry.parent_id);
            if (stmt.step()) {
                sr.container_type = stmt.column_text(0);
            }
        }
    }

    return sr;
}

} // namespace offcat
