// ZeroMQ IPC SUB -> UDP multicast bridge core (BRG-010..BRG-036).
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <zmq.h>

#include "ipcrelay/bridge/bridge_config.hpp"
#include "ipcrelay/common/udp_multicast.hpp"

namespace ipcrelay {

struct BridgeSourceStats {
    uint64_t messages = 0;       // complete ZeroMQ messages received
    uint64_t bytes = 0;          // payload bytes received
    uint64_t multipart = 0;      // messages that had more than one frame
    uint64_t datagrams = 0;      // UDP datagrams sent for this source
    uint64_t fragmented = 0;     // messages that needed >1 datagram
    uint64_t recv_errors = 0;    // zmq_msg_recv failures
    uint64_t send_errors = 0;    // sendmsg failures (excluding timeouts)
    uint64_t send_timeouts = 0;  // sendmsg EAGAIN/timeouts
    uint64_t oversize = 0;       // messages too large to fragment (dropped)
    uint64_t next_sequence = 0;
};

struct BridgeStats {
    uint64_t datagrams_sent = 0;
    uint64_t bytes_sent = 0;
    uint64_t send_errors = 0;
    uint64_t send_timeouts = 0;
    uint64_t poll_errors = 0;
    uint64_t poll_iterations = 0;
    uint64_t max_batch_hits = 0;  // times a source hit max_messages_per_poll (backlog indicator)
};

class Bridge {
public:
    explicit Bridge(const BridgeConfig& cfg);
    ~Bridge();
    Bridge(const Bridge&) = delete;
    Bridge& operator=(const Bridge&) = delete;

    // Opens sockets. Returns false and sets error on failure.
    bool init(std::string& error);

    // Runs the poll loop until shutdown is requested (or a fatal error).
    // Returns the process exit code.
    int run();

    // Processes at most one poll iteration with the given timeout; exposed
    // for tests. Returns false on a fatal error.
    bool poll_once(long timeout_ms);

    const BridgeStats& stats() const { return stats_; }
    const std::vector<BridgeSourceStats>& source_stats() const { return source_stats_; }
    void log_stats() const;

private:
    struct Source {
        BridgeSourceConfig cfg;
        void* socket = nullptr;
    };

    // Drains up to max_messages_per_poll messages from one source.
    void service_source(std::size_t index);
    // Fragments and transmits one message. Returns false if it was dropped.
    bool transmit(std::size_t index, const uint8_t* data, std::size_t len, uint16_t flags, uint64_t timestamp_ns);

    BridgeConfig cfg_;
    void* ctx_ = nullptr;
    std::vector<Source> sources_;
    std::vector<zmq_pollitem_t> poll_items_;
    std::vector<std::vector<uint8_t>> frames_;  // scratch for multipart receive
    std::vector<uint8_t> concat_;               // scratch for multipart concatenation
    UdpMulticastSender sender_;
    BridgeStats stats_;
    std::vector<BridgeSourceStats> source_stats_;
    uint64_t last_stats_ns_ = 0;
    uint64_t send_error_log_budget_ = 10;
};

}  // namespace ipcrelay
