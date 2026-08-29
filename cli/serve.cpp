// offcat serve - read-only web viewer for a catalog database.
//
// A tiny single-threaded-per-connection HTTP server (no external
// dependencies) that exposes the catalog through a small JSON API and
// serves an embedded single-page tree UI.  The database is opened
// strictly read-only; scanning data can never be modified here.

#include "core/types.h"
#include "core/logger.h"
#include "database/database.h"
#include "catalog/catalog.h"
#include "scanner/search.h"

#include <atomic>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace offcat {
namespace {

// ── Small HTTP helpers ──────────────────────────────────────────────

std::string url_decode(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '%' && i + 2 < s.size()) {
            auto hex = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                return -1;
            };
            int hi = hex(s[i + 1]), lo = hex(s[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out.push_back(static_cast<char>((hi << 4) | lo));
                i += 2;
                continue;
            }
        } else if (s[i] == '+') {
            out.push_back(' ');
            continue;
        }
        out.push_back(s[i]);
    }
    return out;
}

std::string json_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (unsigned char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out += static_cast<char>(c);
                }
        }
    }
    return out;
}

std::string format_time(int64_t t) {
    if (t <= 0) return "";
    std::time_t tt = static_cast<std::time_t>(t);
    std::tm tmv;
#ifdef _WIN32
    localtime_s(&tmv, &tt);
#else
    localtime_r(&tt, &tmv);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tmv);
    return buf;
}

std::string to_hex(const std::vector<uint8_t>& v) {
    static const char* digits = "0123456789abcdef";
    std::string out;
    out.reserve(v.size() * 2);
    for (uint8_t b : v) {
        out.push_back(digits[b >> 4]);
        out.push_back(digits[b & 0xF]);
    }
    return out;
}

// ── Request/response plumbing ───────────────────────────────────────

struct HttpRequest {
    std::string path;       // path only, without query
    std::string query;      // raw query string (may be empty)
    std::string header;     // full request (first line) for diagnostics
};

bool read_request(int sock, HttpRequest& req) {
    std::string data;
    char buf[2048];
    for (;;) {
        int n = static_cast<int>(recv(sock, buf, sizeof(buf), 0));
        if (n <= 0) return false;
        data.append(buf, static_cast<size_t>(n));
        // Headers end at the first blank line
        auto pos = data.find("\r\n\r\n");
        if (pos != std::string::npos) break;
        if (data.size() > 65536) return false;
    }
    req.header = data.substr(0, data.find("\r\n"));
    // First line: METHOD SP PATH?QUERY SP VERSION
    size_t sp1 = req.header.find(' ');
    if (sp1 == std::string::npos) return false;
    std::string method = req.header.substr(0, sp1);
    if (method != "GET") return false;
    size_t sp2 = req.header.find(' ', sp1 + 1);
    if (sp2 == std::string::npos) sp2 = req.header.size();
    std::string target = req.header.substr(sp1 + 1, sp2 - sp1 - 1);
    size_t qpos = target.find('?');
    if (qpos == std::string::npos) {
        req.path = target;
    } else {
        req.path = target.substr(0, qpos);
        req.query = target.substr(qpos + 1);
    }
    return true;
}

// Extract a decoded query parameter value.
bool query_param(const std::string& query, const std::string& key,
                 std::string& out) {
    size_t pos = 0;
    while (pos <= query.size()) {
        size_t amp = query.find('&', pos);
        if (amp == std::string::npos) amp = query.size();
        std::string pair = query.substr(pos, amp - pos);
        size_t eq = pair.find('=');
        if (eq != std::string::npos && pair.substr(0, eq) == key) {
            out = url_decode(pair.substr(eq + 1));
            return true;
        }
        pos = amp + 1;
    }
    return false;
}

void send_response(int sock, const std::string& content_type,
                   const std::string& body) {
    std::ostringstream resp;
    resp << "HTTP/1.1 200 OK\r\n"
         << "Content-Type: " << content_type << "; charset=utf-8\r\n"
         << "Content-Length: " << body.size() << "\r\n"
         << "Connection: close\r\n"
         << "Cache-Control: no-store\r\n"
         << "\r\n"
         << body;
    std::string out = resp.str();
    send(sock, out.data(), static_cast<int>(out.size()), 0);
}

void send_404(int sock, const std::string& what) {
    send_response(sock, "text/plain", "404 Not Found: " + what);
}

// ── JSON API ────────────────────────────────────────────────────────

class Viewer {
public:
    Viewer(Database& db) : db_(db), sources_(db), entries_(db),
                           containers_(db), checksums_(db), search_(db) {}

    std::string handle(const HttpRequest& req) {
        if (req.path == "/" || req.path == "/index.html") {
            return frontend_html();
        }
        if (req.path == "/api/stats") return api_stats();
        if (req.path == "/api/sources") return api_sources();
        if (req.path == "/api/tree") return api_tree(req.query);
        if (req.path == "/api/entry") return api_entry(req.query);
        if (req.path == "/api/search") return api_search(req.query);
        return "";
    }

    bool is_api(const std::string& path) const {
        return path.rfind("/api/", 0) == 0;
    }

private:
    Database& db_;
    SourceManager sources_;
    EntryManager entries_;
    ContainerManager containers_;
    ChecksumManager checksums_;
    SearchEngine search_;

    static std::string type_name(EntryType t) {
        return entry_type_to_string(t);
    }

    std::string entry_json(const EntryData& e) {
        bool is_container = false;
        auto c = containers_.is_container(e.id);
        if (is_ok(c)) is_container = get_ok(c);

        std::ostringstream j;
        j << "{\"id\":" << e.id
          << ",\"source_id\":" << e.source_id
          << ",\"parent_id\":" << e.parent_id
          << ",\"name\":\"" << json_escape(e.name) << "\""
          << ",\"type\":\"" << type_name(e.type) << "\""
          << ",\"size\":" << e.size
          << ",\"mtime\":" << e.mtime
          << ",\"is_virtual\":" << (e.is_virtual ? "true" : "false")
          << ",\"is_container\":" << (is_container ? "true" : "false")
          << ",\"is_dir\":" << (e.type == EntryType::Directory ? "true" : "false")
          << "}";
        return j.str();
    }

    std::string api_stats() {
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

    std::string api_sources() {
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

    std::string api_tree(const std::string& query) {
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

    std::string api_entry(const std::string& query) {
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

    std::string api_search(const std::string& query) {
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
              << ",\"type\":\"" << type_name(r.type) << "\""
              << ",\"size\":" << r.size
              << ",\"is_virtual\":" << (r.is_virtual ? "true" : "false")
              << ",\"container_type\":\"" << json_escape(r.container_type) << "\"}";
        }
        j << "]";
        return j.str();
    }

    // ── Embedded frontend (single page, no external assets) ─────────

    static const std::string& frontend_html() {
        static const std::string html = R"HTML(<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Offcat Catalog Viewer</title>
<style>
  :root { --bg:#f5f6f8; --panel:#fff; --line:#e2e4e8; --text:#24292f;
          --muted:#6a737d; --accent:#2f6fdb; --vtag:#9a6bff; --ctag:#2f9e6e; }
  * { box-sizing:border-box; }
  body { margin:0; font-family:"Segoe UI",system-ui,sans-serif; background:var(--bg);
         color:var(--text); height:100vh; display:flex; flex-direction:column; }
  header { display:flex; align-items:center; gap:12px; padding:10px 16px;
           background:var(--panel); border-bottom:1px solid var(--line); }
  header h1 { font-size:16px; margin:0 8px 0 0; white-space:nowrap; }
  #stats { color:var(--muted); font-size:12px; white-space:nowrap; }
  #search { flex:1; max-width:520px; padding:6px 10px; border:1px solid var(--line);
            border-radius:6px; font-size:14px; }
  main { flex:1; display:flex; min-height:0; }
  #sidebar { width:340px; min-width:220px; background:var(--panel);
             border-right:1px solid var(--line); overflow:auto; padding:8px; }
  #content { flex:1; overflow:auto; padding:16px; }
  ul.tree { list-style:none; margin:0; padding-left:16px; }
  ul.tree.root { padding-left:0; }
  .node { cursor:pointer; padding:2px 6px; border-radius:4px; white-space:nowrap;
          display:flex; align-items:center; gap:4px; }
  .node:hover { background:#eef1f5; }
  .node.selected { background:#dbe7fb; }
  .twist { width:14px; display:inline-block; color:var(--muted); font-size:10px; }
  .icon { width:16px; text-align:center; }
  .f-dir { color:#d08a00; } .f-file { color:#8b949e; } .f-sym { color:#b3478f; }
  .tag { font-size:10px; padding:0 5px; border-radius:8px; margin-left:4px; }
  .tag.v { background:#efe9ff; color:var(--vtag); }
  .tag.c { background:#e3f5ec; color:var(--ctag); }
  .tag.s { background:#fdeef4; color:var(--muted); }
  table.detail { border-collapse:collapse; width:100%; max-width:760px; }
  table.detail td { padding:5px 10px; border-bottom:1px solid var(--line);
                    vertical-align:top; }
  table.detail td:first-child { color:var(--muted); width:110px; white-space:nowrap; }
  code { background:#f1f3f5; padding:1px 6px; border-radius:4px;
         font-family:Consolas,monospace; font-size:12px; word-break:break-all; }
  .res { padding:8px 10px; border-bottom:1px solid var(--line); cursor:pointer; }
  .res:hover { background:#f0f3f7; }
  .res .p { color:var(--muted); font-size:12px; }
  .res .n { font-weight:600; }
  .err { color:#c62828; padding:12px; }
  #srcsel { padding:6px; border:1px solid var(--line); border-radius:6px;
            font-size:13px; max-width:180px; }
</style>
</head>
<body>
<header>
  <h1>📚 Offcat</h1>
  <select id="srcsel"><option value="">全部 Source</option></select>
  <span id="stats"></span>
  <input id="search" placeholder="FTS5 搜索文件名/路径（如 install.wim、readme）" autocomplete="off">
</header>
<main>
  <div id="sidebar"><ul class="tree root" id="tree"></ul></div>
  <div id="content"><p style="color:var(--muted)">选择左侧条目查看详情，或输入关键词搜索。</p></div>
</main>
<script>
"use strict";
const treeEl = document.getElementById("tree");
const contentEl = document.getElementById("content");
const srcsel = document.getElementById("srcsel");
const searchInput = document.getElementById("search");
const statsEl = document.getElementById("stats");
let sources = [];
let loaded = new Set();   // entry ids whose children are loaded
let selected = null;

async function getJSON(url) {
  const r = await fetch(url);
  if (!r.ok) throw new Error("HTTP " + r.status);
  return r.json();
}

function fmtSize(n) {
  if (n <= 0) return "";
  const u = ["B","KB","MB","GB","TB"];
  let i = 0, v = n;
  while (v >= 1024 && i < u.length - 1) { v /= 1024; i++; }
  return v.toFixed(v >= 100 ? 0 : 1) + " " + u[i];
}

function iconFor(type, isVirtual) {
  const m = {directory:"📁", file:"📄", symlink:"🔗", other:"❔"};
  return m[type] || "❔";
}

function nodeLabel(e) {
  let s = iconFor(e.type, e.is_virtual) + " " + e.name;
  if (e.is_virtual) s += '<span class="tag v">virtual</span>';
  if (e.is_container) s += '<span class="tag c">' + e.container_type + "</span>";
  return s;
}

function mkNode(e) {
  const li = document.createElement("li");
  const div = document.createElement("div");
  div.className = "node";
  div.innerHTML = '<span class="twist">' + (e.is_dir ? "▸" : "") + "</span>" +
                  '<span class="icon">' + iconFor(e.type, e.is_virtual) + "</span>" +
                  '<span class="f-' + e.type + '">' + escapeHtml(e.name) + "</span>" +
                  (e.is_virtual ? '<span class="tag v">virtual</span>' : "") +
                  (e.is_container ? '<span class="tag c">iso</span>' : "");
  div.addEventListener("click", async (ev) => {
    ev.stopPropagation();
    selectNode(div, e);
    if (e.is_dir && !loaded.has(e.id)) {
      loaded.add(e.id);
      const ul = document.createElement("ul");
      ul.className = "tree";
      li.appendChild(ul);
      try {
        const kids = await getJSON("/api/tree?parent_id=" + e.id);
        for (const k of kids) ul.appendChild(mkNode(k));
        div.querySelector(".twist").textContent = kids.length ? "▾" : "";
      } catch (err) { ul.innerHTML = '<li class="err">加载失败</li>'; }
    } else if (e.is_dir) {
      const twist = div.querySelector(".twist");
      const ul = li.querySelector("ul");
      if (ul) {
        ul.style.display = ul.style.display === "none" ? "" : "none";
        twist.textContent = ul.style.display === "none" ? "▸" : "▾";
      }
    }
  });
  li.appendChild(div);
  return li;
}

function selectNode(div, e) {
  if (selected) selected.classList.remove("selected");
  selected = div;
  selected.classList.add("selected");
  if (e.id < 0) {
    // Source node (negative id sentinel)
    const s = sources.find(x => x.id === -e.id);
    if (s) showSource(s);
    return;
  }
  showEntry(e.id);
}

function showSource(s) {
  contentEl.innerHTML =
    '<table class="detail">' +
    row("名称", escapeHtml(s.name)) +
    row("类型", s.type) +
    row("扫描路径", '<code>' + escapeHtml(s.path) + "</code>") +
    row("条目数", s.entries) +
    "</table>";
}

function escapeHtml(s) {
  return s.replace(/&/g,"&amp;").replace(/</g,"&lt;").replace(/>/g,"&gt;");
}

function row(label, val) {
  return "<tr><td>" + label + "</td><td>" + (val === "" ? '<span style="color:var(--muted)">—</span>' : val) + "</td></tr>";
}

async function showEntry(id) {
  contentEl.innerHTML = "<p>加载中…</p>";
  try {
    const e = await getJSON("/api/entry?id=" + id);
    const tags = (e.is_virtual ? '<span class="tag v">virtual</span> ' : "") +
                 (e.container ? '<span class="tag c">' + escapeHtml(e.container.type) + "</span> " : "");
    let cs = "";
    if (e.checksums && e.checksums.length) {
      cs = e.checksums.map(c =>
        "<div><code>" + c.algorithm + "</code> " + c.value +
        " <span style='color:var(--muted);font-size:11px'>(" + new Date(c.calculated_at*1000).toLocaleString() + ")</span></div>"
      ).join("");
    } else {
      cs = '<span style="color:var(--muted)">未计算</span>';
    }
    const table =
      '<table class="detail">' +
      row("名称", escapeHtml(e.name) + " " + tags) +
      row("类型", e.type) +
      row("完整路径", '<code>' + escapeHtml(e.path) + "</code>") +
      row("大小", fmtSize(e.size)) +
      row("修改时间", e.mtime ? new Date(e.mtime*1000).toLocaleString() : "") +
      row("创建时间", e.ctime ? new Date(e.ctime*1000).toLocaleString() : "") +
      row("校验码", cs) +
      (e.mode ? row("权限 mode", "0" + e.mode.toString(8)) : "") +
      (e.attributes ? row("属性 attributes", e.attributes) : "") +
      (e.is_virtual ? row("来源", "容器内虚拟条目") : "") +
      "</table>";
    contentEl.innerHTML = table;
  } catch (err) {
    contentEl.innerHTML = '<p class="err">加载失败: ' + escapeHtml(String(err)) + "</p>";
  }
}

async function loadSources() {
  try {
    const [st, srcs] = await Promise.all([
      getJSON("/api/stats"), getJSON("/api/sources")
    ]);
    statsEl.textContent = st.sources + " Source · " + st.entries + " Entries · " +
                          st.containers + " Containers";
    sources = srcs;
    srcsel.innerHTML = '<option value="">全部 Source</option>' +
      srcs.map(s => '<option value="' + s.id + '">' + escapeHtml(s.name) + " (" + s.entries + ")</option>").join("");
    treeEl.innerHTML = "";
    loaded.clear();
    for (const s of srcs) {
      const li = document.createElement("li");
      const div = document.createElement("div");
      div.className = "node";
      div.innerHTML = '<span class="twist">▸</span>' +
                      '<span class="icon">💾</span>' +
                      '<span class="f-dir">' + escapeHtml(s.name) + '</span>' +
                      '<span class="tag c">' + escapeHtml(s.type) + "</span>";
      div.addEventListener("click", async () => {
        selectNode(div, {id: -s.id});
        loaded.add(-s.id);
        const ul = document.createElement("ul");
        ul.className = "tree";
        li.appendChild(ul);
        const kids = await getJSON("/api/tree?parent_id=0&source_id=" + s.id);
        for (const k of kids) ul.appendChild(mkNode(k));
        div.querySelector(".twist").textContent = kids.length ? "▾" : "";
      });
      li.appendChild(div);
      treeEl.appendChild(li);
    }
  } catch (err) {
    statsEl.textContent = "加载失败: " + err;
  }
}

let searchTimer = null;
searchInput.addEventListener("input", () => {
  clearTimeout(searchTimer);
  searchTimer = setTimeout(runSearch, 250);
});
searchInput.addEventListener("keydown", (e) => {
  if (e.key === "Enter") { clearTimeout(searchTimer); runSearch(); }
});

async function runSearch() {
  const q = searchInput.value.trim();
  if (!q) { contentEl.innerHTML = '<p style="color:var(--muted)">输入关键词开始搜索。</p>'; return; }
  contentEl.innerHTML = "<p>搜索中…</p>";
  try {
    const res = await getJSON("/api/search?q=" + encodeURIComponent(q) + "&limit=200");
    if (res.error) { contentEl.innerHTML = '<p class="err">' + escapeHtml(res.error) + "</p>"; return; }
    if (!res.length) { contentEl.innerHTML = '<p style="color:var(--muted)">无结果。</p>'; return; }
    contentEl.innerHTML = '<p style="color:var(--muted)">' + res.length + " 条结果：</p>" +
      res.map(r =>
        '<div class="res" onclick="showEntry(' + r.entry_id + ')">' +
        '<span class="n">' + escapeHtml(r.entry_name) + "</span> " +
        (r.is_virtual ? '<span class="tag v">virtual</span>' : "") +
        (r.container_type ? '<span class="tag c">' + escapeHtml(r.container_type) + "</span>" : "") +
        '<div class="p">' + escapeHtml(r.source_name) + " / " + escapeHtml(r.full_path) + "</div>" +
        "</div>").join("");
  } catch (err) {
    contentEl.innerHTML = '<p class="err">搜索失败: ' + escapeHtml(String(err)) + "</p>";
  }
}

loadSources();
</script>
</body>
</html>
)HTML";
        return html;
    }
};

// ── Server loop ─────────────────────────────────────────────────────

void handle_client(int sock, Viewer& viewer) {
    HttpRequest req;
    if (!read_request(sock, req)) {
#ifdef _WIN32
        closesocket(sock);
#else
        close(sock);
#endif
        return;
    }
    std::string body = viewer.handle(req);
    if (body.empty()) {
        send_404(sock, req.path);
    } else {
        std::string ct = viewer.is_api(req.path) ? "application/json" : "text/html";
        send_response(sock, ct, body);
    }
#ifdef _WIN32
    closesocket(sock);
#else
    close(sock);
#endif
}

int run_server(const std::string& db_path, int port) {
    Database db;
    auto open_result = db.open_readonly(db_path);
    if (is_err(open_result)) {
        LOG_ERROR(get_err(open_result).message);
        return 1;
    }

    Viewer viewer(db);

#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        LOG_ERROR("WSAStartup failed");
        return 1;
    }
#endif

    int listener = static_cast<int>(socket(AF_INET, SOCK_STREAM, 0));
    if (listener < 0) {
        LOG_ERROR("socket() failed");
        return 1;
    }

    int opt = 1;
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR,
               reinterpret_cast<const char*>(&opt), sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(static_cast<uint16_t>(port));

    if (bind(listener, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        LOG_ERROR("bind() failed: port " + std::to_string(port) + " in use?");
#ifdef _WIN32
        closesocket(listener);
        WSACleanup();
#else
        close(listener);
#endif
        return 1;
    }
    listen(listener, 16);

    std::cout << "Serving catalog (read-only): " << db_path << "\n"
              << "  http://127.0.0.1:" << port << "\n"
              << "  Press Ctrl+C to stop.\n";

    std::atomic<bool> stop{false};
    (void)stop;
    for (;;) {
        sockaddr_in client{};
#ifdef _WIN32
        int len = sizeof(client);
#else
        socklen_t len = sizeof(client);
#endif
        int c = static_cast<int>(
            accept(listener, reinterpret_cast<sockaddr*>(&client), &len));
        if (c < 0) {
            if (errno == EINTR) continue;
            break;
        }
        std::thread([c, &viewer]() { handle_client(c, viewer); }).detach();
    }

#ifdef _WIN32
    closesocket(listener);
    WSACleanup();
#else
    close(listener);
#endif
    return 0;
}

} // namespace

// ── CLI entry (called from main.cpp) ────────────────────────────────

int cmd_serve(const std::vector<std::string>& args) {
    int port = 8080;
    std::string db_path;
    for (size_t i = 0; i < args.size(); i++) {
        if (args[i] == "--port" && i + 1 < args.size()) {
            port = std::stoi(args[++i]);
        } else if (db_path.empty()) {
            db_path = args[i];
        }
    }
    if (db_path.empty()) {
        std::cerr << "Error: offcat serve requires <catalog.db>\n"
                  << "Usage: offcat serve [--port <N>] <catalog.db>\n";
        return 1;
    }
    return run_server(db_path, port);
}

} // namespace offcat
