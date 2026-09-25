#include "ipcrelay/bridge/bridge_config.hpp"

#include <set>
#include <sstream>

#include "ipcrelay/common/net_util.hpp"
#include "ipcrelay/common/wire_protocol.hpp"

namespace ipcrelay {

uint32_t BridgeConfig::fragment_payload() const {
    return max_datagram_size > wire::kHeaderSize ? max_datagram_size - static_cast<uint32_t>(wire::kHeaderSize) : 0;
}

namespace {

struct Reader {
    const ConfigSection& sec;
    // cppcheck-suppress uninitMemberVarNoCtor ; aggregate always brace-initialised
    std::vector<std::string>& errors;
    std::string prefix;

    std::string where(const ConfigEntry* e) const {
        return prefix + (e && e->line > 0 ? " (line " + std::to_string(e->line) + ")" : "");
    }

    void str(const char* key, std::string& out) {
        if (auto v = sec.get(key)) out = *v;
    }
    void i32(const char* key, int& out, int64_t lo, int64_t hi) {
        const ConfigEntry* e = sec.find(key);
        if (!e) return;
        int64_t v = 0;
        std::string err;
        if (!parse_i64(e->value, v, err)) {
            errors.push_back(where(e) + key + ": " + err);
            return;
        }
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
        if (!parse_u64(e->value, v, err)) {
            errors.push_back(where(e) + key + ": " + err);
            return;
        }
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

const std::set<std::string> kKnownGlobalKeys = {
    "multicast_group", "multicast_port", "multicast_interface", "multicast_ttl", "multicast_loopback",
    "max_datagram_size", "send_buffer_bytes", "send_timeout_ms", "zmq_io_threads", "zmq_recv_hwm",
    "max_messages_per_poll", "reconnect_ivl_ms", "stats_interval_ms", "log_level"};
const std::set<std::string> kKnownSourceKeys = {"id", "endpoint", "filter", "subscribe_all", "recv_hwm", "name"};

}  // namespace

bool build_bridge_config(const ConfigFile& file, BridgeConfig& cfg, std::vector<std::string>& errors) {
    cfg = BridgeConfig{};
    errors.clear();
    const ConfigSection& g = file.global();
    Reader r{g, errors, ""};

    for (const auto& e : g.entries) {
        if (!kKnownGlobalKeys.count(e.key)) errors.push_back("unknown option '" + e.key + "' (line " + std::to_string(e.line) + ")");
    }
    for (const auto& s : file.sections) {
        if (!s.name.empty() && s.name != "source") {
            errors.push_back("unknown section [" + s.name + "] (line " + std::to_string(s.line) + ")");
        }
    }

    r.str("multicast_group", cfg.multicast_group_text);
    if (cfg.multicast_group_text.empty()) {
        errors.push_back("multicast_group is required");
    } else {
        std::string err;
        if (!parse_ipv4(cfg.multicast_group_text, cfg.multicast_group, err)) {
            errors.push_back("multicast_group: " + err);
        } else if (!is_multicast_ipv4(cfg.multicast_group)) {
            errors.push_back("multicast_group '" + cfg.multicast_group_text + "' is not a multicast address (224.0.0.0/4)");
        }
    }
    {
        const ConfigEntry* e = g.find("multicast_port");
        if (!e) {
            errors.push_back("multicast_port is required");
        } else {
            std::string err;
            if (!parse_u16(e->value, cfg.multicast_port, err)) errors.push_back("multicast_port: " + err);
            else if (cfg.multicast_port == 0) errors.push_back("multicast_port must be 1..65535");
        }
    }
    r.str("multicast_interface", cfg.multicast_interface_text);
    {
        std::string err;
        if (!resolve_interface_ipv4(cfg.multicast_interface_text, cfg.multicast_interface, err)) {
            errors.push_back("multicast_interface: " + err);
        }
    }
    r.i32("multicast_ttl", cfg.multicast_ttl, 0, 255);
    r.boolean("multicast_loopback", cfg.multicast_loopback);
    r.u32("max_datagram_size", cfg.max_datagram_size, wire::kHeaderSize + 1, wire::kMaxUdpPayload);
    r.i32("send_buffer_bytes", cfg.send_buffer_bytes, 0, 1 << 30);
    r.i32("send_timeout_ms", cfg.send_timeout_ms, 0, 60000);
    r.i32("zmq_io_threads", cfg.zmq_io_threads, 1, 64);
    r.i32("zmq_recv_hwm", cfg.zmq_recv_hwm, 0, 10000000);
    r.i32("max_messages_per_poll", cfg.max_messages_per_poll, 1, 100000);
    r.i32("reconnect_ivl_ms", cfg.reconnect_ivl_ms, 1, 600000);
    r.i32("stats_interval_ms", cfg.stats_interval_ms, 0, 3600000);
    if (const ConfigEntry* e = g.find("log_level")) {
        if (!parse_log_level(e->value, cfg.log_level)) errors.push_back("log_level: unknown level '" + e->value + "'");
    }

    cfg.sources.clear();
    std::set<uint32_t> ids;
    std::set<std::string> endpoints;
    int index = 0;
    for (const ConfigSection* s : file.named("source")) {
        ++index;
        std::string prefix = "[source] #" + std::to_string(index) + " (line " + std::to_string(s->line) + "): ";
        BridgeSourceConfig src;
        for (const auto& e : s->entries) {
            if (!kKnownSourceKeys.count(e.key)) errors.push_back(prefix + "unknown option '" + e.key + "'");
        }
        const ConfigEntry* id = s->find("id");
        if (!id) {
            errors.push_back(prefix + "id is required");
        } else {
            std::string err;
            if (!parse_u32(id->value, src.id, err)) errors.push_back(prefix + "id: " + err);
            else if (!ids.insert(src.id).second) errors.push_back(prefix + "duplicate source id " + std::to_string(src.id));
        }
        if (auto ep = s->get("endpoint")) src.endpoint = trim(*ep);
        if (src.endpoint.empty()) {
            errors.push_back(prefix + "endpoint is required");
        } else {
            if (src.endpoint.find("://") == std::string::npos) errors.push_back(prefix + "endpoint '" + src.endpoint + "' must be a ZeroMQ endpoint such as ipc:///run/app.sock");
            if (!endpoints.insert(src.endpoint).second) errors.push_back(prefix + "duplicate endpoint '" + src.endpoint + "'");
        }
        bool subscribe_all = false;
        for (const auto& e : s->entries) {
            if (e.key == "filter") src.filters.push_back(e.value);
        }
        if (const ConfigEntry* e = s->find("subscribe_all")) {
            std::string err;
            if (!parse_bool(e->value, subscribe_all, err)) errors.push_back(prefix + "subscribe_all: " + err);
        }
        // An explicit empty filter or subscribe_all means "everything".
        bool any_empty = false;
        for (const auto& f : src.filters) any_empty = any_empty || f.empty();
        if (subscribe_all || any_empty) src.filters.clear();
        if (auto n = s->get("name")) src.name = *n;
        Reader sr{*s, errors, prefix};
        sr.i32("recv_hwm", src.recv_hwm, 0, 10000000);
        cfg.sources.push_back(std::move(src));
    }
    if (cfg.sources.empty()) errors.push_back("at least one [source] section is required");

    return errors.empty();
}

std::string describe_bridge_config(const BridgeConfig& cfg) {
    std::ostringstream o;
    o << "multicast " << cfg.multicast_group_text << ":" << cfg.multicast_port
      << " iface=" << (cfg.multicast_interface ? ipv4_to_string(cfg.multicast_interface) : "default")
      << " ttl=" << cfg.multicast_ttl << " loop=" << (cfg.multicast_loopback ? "on" : "off")
      << " max_datagram=" << cfg.max_datagram_size << " (payload/fragment=" << cfg.fragment_payload() << ")"
      << " sndbuf=" << cfg.send_buffer_bytes << " batch=" << cfg.max_messages_per_poll << "; sources:";
    for (const auto& s : cfg.sources) {
        o << " [" << s.id << (s.name.empty() ? "" : "/" + s.name) << " " << s.endpoint;
        if (s.filters.empty()) o << " filter=*";
        else for (const auto& f : s.filters) o << " filter='" << f << "'";
        o << "]";
    }
    return o.str();
}

}  // namespace ipcrelay
