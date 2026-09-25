#include "ipcrelay/common/config.hpp"

#include <arpa/inet.h>
#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace ipcrelay {

std::string trim(const std::string& s) {
    std::size_t b = 0;
    std::size_t e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

std::string to_lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

const ConfigEntry* ConfigSection::find(const std::string& key) const {
    // Last assignment wins, so that command-line overrides appended later
    // take precedence over the file.
    for (auto it = entries.rbegin(); it != entries.rend(); ++it) {
        if (it->key == key) return &*it;
    }
    return nullptr;
}

std::optional<std::string> ConfigSection::get(const std::string& key) const {
    const ConfigEntry* e = find(key);
    if (!e) return std::nullopt;
    return e->value;
}

std::vector<const ConfigSection*> ConfigFile::named(const std::string& name) const {
    std::vector<const ConfigSection*> out;
    for (const auto& s : sections) {
        if (s.name == name) out.push_back(&s);
    }
    return out;
}

static std::string strip_comment(const std::string& line) {
    // '#' or ';' starts a comment unless inside double quotes.
    bool quoted = false;
    for (std::size_t i = 0; i < line.size(); ++i) {
        char c = line[i];
        if (c == '"') quoted = !quoted;
        if (!quoted && (c == '#' || c == ';')) return line.substr(0, i);
    }
    return line;
}

static std::string unquote(const std::string& v) {
    if (v.size() >= 2 && v.front() == '"' && v.back() == '"') return v.substr(1, v.size() - 2);
    return v;
}

bool parse_config_text(const std::string& text, ConfigFile& out, std::string& error) {
    out.sections.clear();
    out.sections.push_back(ConfigSection{});
    std::istringstream in(text);
    std::string line;
    int lineno = 0;
    while (std::getline(in, line)) {
        ++lineno;
        std::string s = trim(strip_comment(line));
        if (s.empty()) continue;
        if (s.front() == '[') {
            if (s.back() != ']') {
                error = "line " + std::to_string(lineno) + ": unterminated section header";
                return false;
            }
            ConfigSection sec;
            sec.name = trim(s.substr(1, s.size() - 2));
            sec.line = lineno;
            if (sec.name.empty()) {
                error = "line " + std::to_string(lineno) + ": empty section name";
                return false;
            }
            out.sections.push_back(std::move(sec));
            continue;
        }
        std::size_t eq = s.find('=');
        if (eq == std::string::npos) {
            error = "line " + std::to_string(lineno) + ": expected key = value";
            return false;
        }
        ConfigEntry e;
        e.key = trim(s.substr(0, eq));
        e.value = unquote(trim(s.substr(eq + 1)));
        e.line = lineno;
        if (e.key.empty()) {
            error = "line " + std::to_string(lineno) + ": empty key";
            return false;
        }
        out.sections.back().entries.push_back(std::move(e));
    }
    return true;
}

bool load_config_file(const std::string& path, ConfigFile& out, std::string& error) {
    std::ifstream f(path);
    if (!f) {
        error = "cannot open config file '" + path + "'";
        return false;
    }
    std::stringstream ss;
    ss << f.rdbuf();
    if (!parse_config_text(ss.str(), out, error)) {
        error = path + ": " + error;
        return false;
    }
    return true;
}

bool apply_override(ConfigFile& cfg, const std::string& assignment, std::string& error) {
    std::size_t eq = assignment.find('=');
    if (eq == std::string::npos) {
        error = "override '" + assignment + "' must be key=value";
        return false;
    }
    std::string key = trim(assignment.substr(0, eq));
    std::string value = unquote(trim(assignment.substr(eq + 1)));
    if (key.empty()) {
        error = "override '" + assignment + "' has an empty key";
        return false;
    }
    if (cfg.sections.empty()) cfg.sections.push_back(ConfigSection{});
    ConfigSection* target = &cfg.global();
    std::size_t dot = key.find('.');
    if (dot != std::string::npos) {
        std::string sec = key.substr(0, dot);
        key = key.substr(dot + 1);
        target = nullptr;
        for (auto it = cfg.sections.rbegin(); it != cfg.sections.rend(); ++it) {
            if (it->name == sec) {
                target = &*it;
                break;
            }
        }
        if (!target) {
            error = "override refers to section '" + sec + "' which does not exist";
            return false;
        }
    }
    target->entries.push_back(ConfigEntry{key, value, 0});
    return true;
}

bool parse_bool(const std::string& s, bool& out, std::string& error) {
    std::string v = to_lower(trim(s));
    if (v == "1" || v == "true" || v == "yes" || v == "on") { out = true; return true; }
    if (v == "0" || v == "false" || v == "no" || v == "off") { out = false; return true; }
    error = "'" + s + "' is not a boolean (use true/false)";
    return false;
}

bool parse_i64(const std::string& s, int64_t& out, std::string& error) {
    std::string v = trim(s);
    if (v.empty()) { error = "empty integer"; return false; }
    errno = 0;
    char* end = nullptr;
    long long r = std::strtoll(v.c_str(), &end, 0);
    if (errno != 0 || end == v.c_str() || *end != '\0') {
        error = "'" + s + "' is not an integer";
        return false;
    }
    out = r;
    return true;
}

bool parse_u64(const std::string& s, uint64_t& out, std::string& error) {
    std::string v = trim(s);
    if (v.empty() || v.front() == '-') { error = "'" + s + "' is not an unsigned integer"; return false; }
    errno = 0;
    char* end = nullptr;
    unsigned long long r = std::strtoull(v.c_str(), &end, 0);
    if (errno != 0 || end == v.c_str() || *end != '\0') {
        error = "'" + s + "' is not an unsigned integer";
        return false;
    }
    out = r;
    return true;
}

bool parse_u32(const std::string& s, uint32_t& out, std::string& error) {
    uint64_t v = 0;
    if (!parse_u64(s, v, error)) return false;
    if (v > 0xFFFFFFFFull) { error = "'" + s + "' exceeds 32 bits"; return false; }
    out = static_cast<uint32_t>(v);
    return true;
}

bool parse_u16(const std::string& s, uint16_t& out, std::string& error) {
    uint64_t v = 0;
    if (!parse_u64(s, v, error)) return false;
    if (v > 0xFFFFull) { error = "'" + s + "' exceeds 16 bits"; return false; }
    out = static_cast<uint16_t>(v);
    return true;
}

bool parse_ipv4(const std::string& s, uint32_t& out_host_order, std::string& error) {
    struct in_addr a;
    if (inet_pton(AF_INET, trim(s).c_str(), &a) != 1) {
        error = "'" + s + "' is not an IPv4 address";
        return false;
    }
    // sockaddr fields are defined by the socket API, so ntohl is the correct
    // accessor here; this is not an application wire-protocol field.
    out_host_order = ntohl(a.s_addr);
    return true;
}

bool is_multicast_ipv4(uint32_t addr_host_order) {
    return (addr_host_order & 0xF0000000u) == 0xE0000000u;
}

}  // namespace ipcrelay
