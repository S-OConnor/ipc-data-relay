// Minimal INI-style configuration file parser (no external dependencies).
//
//   # comment
//   key = value                 (global section, before any [section])
//   [section]
//   key = value
//
// Sections may repeat; each occurrence is kept in order, which is how the
// bridge expresses N ZeroMQ sources with repeated [source] blocks.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace ipcrelay {

struct ConfigEntry {
    std::string key;
    std::string value;
    int line = 0;
};

struct ConfigSection {
    std::string name;  // "" for the global section
    int line = 0;
    std::vector<ConfigEntry> entries;

    const ConfigEntry* find(const std::string& key) const;
    std::optional<std::string> get(const std::string& key) const;
};

struct ConfigFile {
    std::vector<ConfigSection> sections;  // sections[0] is always the global section

    ConfigSection& global() { return sections.front(); }
    const ConfigSection& global() const { return sections.front(); }
    std::vector<const ConfigSection*> named(const std::string& name) const;
};

// Parses text. Returns false and sets error on syntax errors.
bool parse_config_text(const std::string& text, ConfigFile& out, std::string& error);
bool load_config_file(const std::string& path, ConfigFile& out, std::string& error);

// Applies "key=value" or "section.key=value" overrides (from the command
// line). A "section.key" override applies to the LAST section of that name.
bool apply_override(ConfigFile& cfg, const std::string& assignment, std::string& error);

// Value parsing helpers. All return false (with error set) on bad input.
bool parse_bool(const std::string& s, bool& out, std::string& error);
bool parse_i64(const std::string& s, int64_t& out, std::string& error);
bool parse_u64(const std::string& s, uint64_t& out, std::string& error);
bool parse_u32(const std::string& s, uint32_t& out, std::string& error);
bool parse_u16(const std::string& s, uint16_t& out, std::string& error);
bool parse_ipv4(const std::string& s, uint32_t& out_host_order, std::string& error);
bool is_multicast_ipv4(uint32_t addr_host_order);

std::string trim(const std::string& s);
std::string to_lower(std::string s);

}  // namespace ipcrelay
