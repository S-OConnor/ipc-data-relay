#include "http.hpp"

#include "ipcrelay/config.hpp"

namespace ipcrelay::ctl {

const std::string& HttpRequest::header(const std::string& name) const {
    static const std::string kEmpty;
    for (const auto& h : headers) {
        if (h.first == name) return h.second;
    }
    return kEmpty;
}

bool HttpRequest::has_header(const std::string& name) const {
    for (const auto& h : headers) {
        if (h.first == name) return true;
    }
    return false;
}

HttpParse parse_http_request(const std::string& buf, std::size_t max_header_bytes, HttpRequest& req,
                             std::size_t& consumed, std::string& error) {
    const std::size_t end = buf.find("\r\n\r\n");
    if (end == std::string::npos) {
        if (buf.size() > max_header_bytes) {
            error = "request header too large";
            return HttpParse::Error;
        }
        return HttpParse::NeedMore;
    }
    if (end + 4 > max_header_bytes) {
        error = "request header too large";
        return HttpParse::Error;
    }

    req = HttpRequest{};
    std::size_t line_end = buf.find("\r\n");
    const std::string request_line = buf.substr(0, line_end);
    const std::size_t sp1 = request_line.find(' ');
    const std::size_t sp2 = sp1 == std::string::npos ? std::string::npos : request_line.find(' ', sp1 + 1);
    if (sp1 == std::string::npos || sp2 == std::string::npos || request_line.find(' ', sp2 + 1) != std::string::npos) {
        error = "malformed request line";
        return HttpParse::Error;
    }
    req.method = request_line.substr(0, sp1);
    req.target = request_line.substr(sp1 + 1, sp2 - sp1 - 1);
    req.version = request_line.substr(sp2 + 1);
    if (req.method.empty() || req.target.empty() || req.target[0] != '/' || req.version.rfind("HTTP/1.", 0) != 0) {
        error = "malformed request line";
        return HttpParse::Error;
    }
    req.path = req.target.substr(0, req.target.find('?'));

    std::size_t pos = line_end + 2;
    while (pos < end + 2) {
        line_end = buf.find("\r\n", pos);
        const std::string line = buf.substr(pos, line_end - pos);
        pos = line_end + 2;
        const std::size_t colon = line.find(':');
        if (colon == std::string::npos || colon == 0 || line[0] == ' ' || line[0] == '\t') {
            error = "malformed header line";
            return HttpParse::Error;
        }
        req.headers.emplace_back(to_lower(line.substr(0, colon)), trim(line.substr(colon + 1)));
    }
    consumed = end + 4;
    return HttpParse::Request;
}

bool header_has_token(const std::string& value, const std::string& token) {
    const std::string want = to_lower(token);
    std::size_t start = 0;
    while (start <= value.size()) {
        std::size_t comma = value.find(',', start);
        if (comma == std::string::npos) comma = value.size();
        if (to_lower(trim(value.substr(start, comma - start))) == want) return true;
        start = comma + 1;
    }
    return false;
}

const char* http_reason(int status) {
    switch (status) {
        case 101: return "Switching Protocols";
        case 200: return "OK";
        case 400: return "Bad Request";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 426: return "Upgrade Required";
        case 431: return "Request Header Fields Too Large";
        case 500: return "Internal Server Error";
        case 503: return "Service Unavailable";
        default: return "Unknown";
    }
}

std::string http_response(int status, const std::string& content_type, const std::string& body, bool head_only,
                          const std::vector<std::pair<std::string, std::string>>& extra_headers) {
    std::string out = "HTTP/1.1 " + std::to_string(status) + " " + http_reason(status) + "\r\n";
    if (!content_type.empty()) out += "Content-Type: " + content_type + "\r\n";
    out += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    out += "Cache-Control: no-store\r\n";
    out += "X-Content-Type-Options: nosniff\r\n";
    for (const auto& h : extra_headers) out += h.first + ": " + h.second + "\r\n";
    out += "Connection: close\r\n\r\n";
    if (!head_only) out += body;
    return out;
}

}  // namespace ipcrelay::ctl
