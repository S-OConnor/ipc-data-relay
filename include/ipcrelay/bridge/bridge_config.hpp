#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "ipcrelay/common/config.hpp"
#include "ipcrelay/common/log.hpp"

namespace ipcrelay {

struct BridgeSourceConfig {
    uint32_t id = 0;
    std::string endpoint;
    std::vector<std::string> filters;  // empty vector => subscribe to everything
    int recv_hwm = 0;                  // 0 => use the global default
    std::string name;                  // optional human-readable label
};

struct BridgeConfig {
    // UDP multicast output
    std::string multicast_group_text;
    uint32_t multicast_group = 0;      // host order
    uint16_t multicast_port = 0;
    std::string multicast_interface_text;
    uint32_t multicast_interface = 0;  // host order, 0 = default
    int multicast_ttl = 1;
    bool multicast_loopback = true;
    uint32_t max_datagram_size = 1400;  // UDP payload bytes incl. transport header
    int send_buffer_bytes = 4 * 1024 * 1024;
    int send_timeout_ms = 100;

    // ZeroMQ input
    int zmq_io_threads = 1;
    int zmq_recv_hwm = 10000;
    int max_messages_per_poll = 32;    // fairness: max messages drained per source per poll
    int reconnect_ivl_ms = 100;

    // Runtime
    int stats_interval_ms = 5000;      // 0 disables periodic stats logging
    LogLevel log_level = LogLevel::Info;

    std::vector<BridgeSourceConfig> sources;

    uint32_t fragment_payload() const;  // bytes of payload per datagram
};

// Builds a BridgeConfig from a parsed ConfigFile. Returns false and fills
// errors (one per problem) if the configuration is invalid (BRG-103).
bool build_bridge_config(const ConfigFile& file, BridgeConfig& out, std::vector<std::string>& errors);

std::string describe_bridge_config(const BridgeConfig& cfg);

}  // namespace ipcrelay
