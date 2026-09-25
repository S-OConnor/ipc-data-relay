#include "receiver_config.hpp"

#include <set>
#include <sstream>

#include "ipcrelay/common/net_util.hpp"
#include "ipcrelay/common/wire_protocol.hpp"

namespace ipcrelay {

namespace {

struct Reader {
    const ConfigSection& sec;
    // cppcheck-suppress uninitMemberVarNoCtor ; aggregate always brace-initialised
    std::vector<std::string>& errors;

    std::string where(const ConfigEntry* e) const {
        return e && e->line > 0 ? "(line " + std::to_string(e->line) + ") " : "";
    }
    void str(const char* key, std::string& out) {
        if (auto v = sec.get(key)) out = *v;
    }
    void i32(const char* key, int& out, int64_t lo, int64_t hi) {
        const ConfigEntry* e = sec.find(key);
        if (!e) return;
        int64_t v = 0;
        std::string err;
        if (!parse_i64(e->value, v, err)) { errors.push_back(where(e) + key + ": " + err); return; }
        if (v < lo || v > hi) {
            errors.push_back(where(e) + key + ": value " + std::to_string(v) + " outside [" + std::to_string(lo) + ", " + std::to_string(hi) + "]");
            return;
        }
        out = static_cast<int>(v);
    }
    void u32(const char* key, uint32_t& out, uint64_t lo, uint64_t hi) {
        const ConfigEntry* e = sec.find(key);
        if (!e) return;
        uint64_t v = 0;
        std::string err;
        if (!parse_u64(e->value, v, err)) { errors.push_back(where(e) + key + ": " + err); return; }
        if (v < lo || v > hi) {
            errors.push_back(where(e) + key + ": value " + std::to_string(v) + " outside [" + std::to_string(lo) + ", " + std::to_string(hi) + "]");
            return;
        }
        out = static_cast<uint32_t>(v);
    }
    void boolean(const char* key, bool& out) {
        const ConfigEntry* e = sec.find(key);
        if (!e) return;
        std::string err;
        if (!parse_bool(e->value, out, err)) errors.push_back(where(e) + key + ": " + err);
    }
};

const std::set<std::string> kKnownKeys = {
    "multicast_group", "multicast_port", "multicast_interface", "receive_buffer_bytes", "max_datagrams_per_poll",
    "capture_file", "record_on_start", "capture_buffer_bytes", "capture_flush_interval_ms", "capture_sync_on_flush",
    "reassembly_timeout_ms", "reassembly_max_pending", "reassembly_max_message_bytes",
    "stats_endpoint", "stats_topic", "stats_interval_ms", "stats_print",
    "command_endpoint", "command_bind", "command_topic", "log_level"};

bool valid_tcp_endpoint(const std::string& ep) {
    return ep.rfind("tcp://", 0) == 0 && ep.size() > 6 && ep.find(':', 6) != std::string::npos;
}

}  // namespace

bool build_receiver_config(const ConfigFile& file, ReceiverConfig& cfg, std::vector<std::string>& errors) {
    cfg = ReceiverConfig{};
    errors.clear();
    const ConfigSection& g = file.global();
    Reader r{g, errors};
    for (const auto& e : g.entries) {
        if (!kKnownKeys.count(e.key)) errors.push_back("unknown option '" + e.key + "' (line " + std::to_string(e.line) + ")");
    }
    for (const auto& s : file.sections) {
        if (!s.name.empty()) errors.push_back("unknown section [" + s.name + "] (line " + std::to_string(s.line) + ")");
    }

    r.str("multicast_group", cfg.multicast_group_text);
    if (cfg.multicast_group_text.empty()) {
        errors.push_back("multicast_group is required");
    } else {
        std::string err;
        if (!parse_ipv4(cfg.multicast_group_text, cfg.multicast_group, err)) errors.push_back("multicast_group: " + err);
        else if (!is_multicast_ipv4(cfg.multicast_group)) errors.push_back("multicast_group '" + cfg.multicast_group_text + "' is not a multicast address (224.0.0.0/4)");
    }
    if (const ConfigEntry* e = g.find("multicast_port")) {
        std::string err;
        if (!parse_u16(e->value, cfg.multicast_port, err)) errors.push_back("multicast_port: " + err);
        else if (cfg.multicast_port == 0) errors.push_back("multicast_port must be 1..65535");
    } else {
        errors.push_back("multicast_port is required");
    }
    r.str("multicast_interface", cfg.multicast_interface_text);
    {
        std::string err;
        if (!resolve_interface_ipv4(cfg.multicast_interface_text, cfg.multicast_interface, err)) errors.push_back("multicast_interface: " + err);
    }
    r.i32("receive_buffer_bytes", cfg.receive_buffer_bytes, 0, 1 << 30);
    r.i32("max_datagrams_per_poll", cfg.max_datagrams_per_poll, 1, 100000);

    r.str("capture_file", cfg.capture_file);
    r.boolean("record_on_start", cfg.record_on_start);
    if (cfg.capture_file.empty()) errors.push_back("capture_file is required");
    r.i32("capture_buffer_bytes", cfg.capture_buffer_bytes, 0, 1 << 30);
    r.i32("capture_flush_interval_ms", cfg.capture_flush_interval_ms, 0, 3600000);
    r.boolean("capture_sync_on_flush", cfg.capture_sync_on_flush);

    r.i32("reassembly_timeout_ms", cfg.reassembly_timeout_ms, 1, 3600000);
    r.i32("reassembly_max_pending", cfg.reassembly_max_pending, 1, 1000000);
    r.u32("reassembly_max_message_bytes", cfg.reassembly_max_message_bytes, 1, wire::kMaxMessageLength);

    r.str("stats_endpoint", cfg.stats_endpoint);
    if (!cfg.stats_endpoint.empty() && !valid_tcp_endpoint(cfg.stats_endpoint))
        errors.push_back("stats_endpoint '" + cfg.stats_endpoint + "' must be tcp://ADDRESS:PORT (or empty to disable)");
    r.str("stats_topic", cfg.stats_topic);
    r.i32("stats_interval_ms", cfg.stats_interval_ms, 10, 3600000);
    r.boolean("stats_print", cfg.stats_print);
    r.str("command_endpoint", cfg.command_endpoint);
    if (!cfg.command_endpoint.empty() && !valid_tcp_endpoint(cfg.command_endpoint))
        errors.push_back("command_endpoint '" + cfg.command_endpoint + "' must be tcp://ADDRESS:PORT (or empty to disable)");
    r.boolean("command_bind", cfg.command_bind);
    r.str("command_topic", cfg.command_topic);
    if (!cfg.stats_endpoint.empty() && cfg.stats_endpoint == cfg.command_endpoint && cfg.command_bind)
        errors.push_back("stats_endpoint and command_endpoint cannot both bind the same address");
    if (const ConfigEntry* e = g.find("log_level")) {
        if (!parse_log_level(e->value, cfg.log_level)) errors.push_back("log_level: unknown level '" + e->value + "'");
    }
    return errors.empty();
}

std::string describe_receiver_config(const ReceiverConfig& cfg) {
    std::ostringstream o;
    o << "multicast " << cfg.multicast_group_text << ":" << cfg.multicast_port
      << " iface=" << (cfg.multicast_interface ? ipv4_to_string(cfg.multicast_interface) : "default")
      << " rcvbuf=" << cfg.receive_buffer_bytes
      << "; capture=" << cfg.capture_file << " record_on_start=" << (cfg.record_on_start ? "yes" : "no")
      << " flush_ms=" << cfg.capture_flush_interval_ms
      << "; reassembly timeout_ms=" << cfg.reassembly_timeout_ms << " max_pending=" << cfg.reassembly_max_pending
      << "; stats=" << (cfg.stats_endpoint.empty() ? "disabled" : cfg.stats_endpoint) << " every " << cfg.stats_interval_ms << "ms"
      << "; commands=" << (cfg.command_endpoint.empty() ? "disabled" : cfg.command_endpoint + (cfg.command_bind ? " (bind)" : " (connect)"));
    return o.str();
}

}  // namespace ipcrelay
