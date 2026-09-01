#include "server/http.h"

#include <cstdio>
#include <sstream>

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
    std::string header = data.substr(0, data.find("\r\n"));
    // First line: METHOD SP PATH?QUERY SP VERSION
    size_t sp1 = header.find(' ');
    if (sp1 == std::string::npos) return false;
    std::string method = header.substr(0, sp1);
    if (method != "GET") return false;
    size_t sp2 = header.find(' ', sp1 + 1);
    if (sp2 == std::string::npos) sp2 = header.size();
    std::string target = header.substr(sp1 + 1, sp2 - sp1 - 1);
    size_t qpos = target.find('?');
    if (qpos == std::string::npos) {
        req.path = target;
    } else {
        req.path = target.substr(0, qpos);
        req.query = target.substr(qpos + 1);
    }

    // Parse the remaining header lines: cap the line count and pick up
    // the Host header (used by the loopback check in handle_client).
    req.host.clear();
    size_t line_start = header.size() + 2;  // after the request line
    size_t nlines = 0;
    while (line_start < data.size()) {
        size_t le = data.find("\r\n", line_start);
        if (le == std::string::npos) le = data.size();
        std::string line = data.substr(line_start, le - line_start);
        if (line.empty()) break;  // blank line ends the headers
        if (++nlines > 32) return false;
        if (line.size() > 5 && line.compare(0, 5, "Host:") == 0) {
            req.host = line.substr(5);
            size_t b = req.host.find_first_not_of(" \t");
            size_t e = req.host.find_last_not_of(" \t");
            if (b == std::string::npos) {
                req.host.clear();
            } else {
                req.host = req.host.substr(b, e - b + 1);
            }
        }
        line_start = le + 2;
    }
    return true;
}

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

void send_response(int sock, int status, const std::string& content_type,
                   const std::string& body) {
    std::string status_line;
    switch (status) {
        case 200: status_line = "200 OK"; break;
        case 400: status_line = "400 Bad Request"; break;
        case 404: status_line = "404 Not Found"; break;
        case 500: status_line = "500 Internal Server Error"; break;
        default:  status_line = std::to_string(status) + " Status"; break;
    }
    std::ostringstream resp;
    resp << "HTTP/1.1 " << status_line << "\r\n"
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
    send_response(sock, 404, "text/plain", "404 Not Found: " + what);
}

bool host_allowed(const std::string& host) {
    // Strip an optional :port.  A colon is a port separator only when
    // it follows the closing ']' (bracketed IPv6 like [::1]:8080) or
    // when the string has exactly one colon (host:port).  A bare IPv6
    // like ::1 has several colons and no brackets, so it is matched
    // as-is below.
    std::string h = host;
    size_t colon_count = 0;
    for (char c : h) {
        if (c == ':') colon_count++;
    }
    size_t close_bracket = h.find(']');
    size_t colon = h.rfind(':');
    if (colon != std::string::npos) {
        bool bracketed = !h.empty() && h.front() == '[';
        bool port_sep = bracketed
            ? (close_bracket != std::string::npos && colon > close_bracket)
            : (colon_count == 1);
        if (port_sep) h = h.substr(0, colon);
    }
    if (h.size() >= 2 && h.front() == '[' && h.back() == ']') {
        h = h.substr(1, h.size() - 2);
    }
    return h == "127.0.0.1" || h == "localhost" || h == "::1";
}

}  // namespace offcat
