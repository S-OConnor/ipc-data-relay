#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "ipcrelay/config.hpp"
#include "ipcrelay/log.hpp"

namespace ipcrelay {

struct ReceiverConfig {
    // UDP multicast input
    std::string multicast_group_text;
    uint32_t multicast_group = 0;
    uint16_t multicast_port = 0;
    std::string multicast_interface_text;
    uint32_t multicast_interface = 0;
    int receive_buffer_bytes = 4 * 1024 * 1024;
    int max_datagrams_per_poll = 64;

    // Binary capture output
    std::string capture_file;
    bool record_on_start = true;
    int capture_buffer_bytes = 1024 * 1024;
    int capture_flush_interval_ms = 1000;
    bool capture_sync_on_flush = false;

    // Reassembly
    int reassembly_timeout_ms = 500;
    int reassembly_max_pending = 256;
    uint32_t reassembly_max_message_bytes = 16u * 1024u * 1024u;

    // ZeroMQ TCP statistics publisher and command subscriber
    std::string stats_endpoint = "tcp://*:5556";  // "" disables
    std::string stats_topic = "stats";
    int stats_interval_ms = 1000;
    bool stats_print = false;
    std::string command_endpoint = "tcp://127.0.0.1:5557";  // "" disables
    bool command_bind = false;      // false: connect to a controller's PUB; true: bind
    std::string command_topic;      // subscription filter, "" = all

    LogLevel log_level = LogLevel::Info;
};

bool build_receiver_config(const ConfigFile& file, ReceiverConfig& out, std::vector<std::string>& errors);
std::string describe_receiver_config(const ReceiverConfig& cfg);

}  // namespace ipcrelay
