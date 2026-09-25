// Minimal HTTP/1.1 request parsing and response formatting for the control
// tool's embedded web server (static frontend + WebSocket upgrade). No I/O.
#pragma once

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace ipcrelay::ctl {

struct HttpRequest {
    std::string method;
    std::string target;   // request-target as sent (path + optional query)
    std::string path;     // target without the query string
    std::string version;  // e.g. "HTTP/1.1"
    std::vector<std::pair<std::string, std::string>> headers;  // names lower-cased

    // Value of the first header with this (lower-case) name, or "".
    const std::string& header(const std::string& name) const;
    bool has_header(const std::string& name) const;
};

enum class HttpParse { NeedMore, Request, Error };

// Parses the request line and headers at the start of buf (bodies are not
// supported; the server only accepts GET/HEAD). On Request, consumed is the
// length of the header block including the terminating blank line.
HttpParse parse_http_request(const std::string& buf, std::size_t max_header_bytes, HttpRequest& req,
                             std::size_t& consumed, std::string& error);

// True if a comma-separated header value contains token (case-insensitive),
// e.g. header_has_token("keep-alive, Upgrade", "upgrade").
bool header_has_token(const std::string& value, const std::string& token);

// Formats a complete response with Content-Length and Connection: close.
// head_only omits the body (for HEAD requests) but keeps its length.
std::string http_response(int status, const std::string& content_type, const std::string& body, bool head_only,
                          const std::vector<std::pair<std::string, std::string>>& extra_headers = {});

const char* http_reason(int status);

}  // namespace ipcrelay::ctl
