// Minimal HTTP plumbing for the read-only catalog viewer.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace offcat {

struct HttpRequest {
    std::string path;   // path only, without query
    std::string query;  // raw query string (may be empty)
};

std::string url_decode(const std::string& s);
std::string json_escape(const std::string& s);
std::string to_hex(const std::vector<uint8_t>& v);

bool read_request(int sock, HttpRequest& req);
bool query_param(const std::string& query, const std::string& key,
                 std::string& out);
void send_response(int sock, const std::string& content_type,
                   const std::string& body);
void send_404(int sock, const std::string& what);

}  // namespace offcat
