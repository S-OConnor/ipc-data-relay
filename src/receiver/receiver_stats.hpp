// Receiver runtime statistics (BRG-074, BRG-075B, BRG-122).
#pragma once

#include <cstdint>
#include <map>
#include <string>

namespace ipcrelay {

struct ReceiverSourceStats {
    uint64_t datagrams = 0;
    uint64_t messages = 0;
    uint64_t payload_bytes = 0;
    uint64_t gap_events = 0;
    uint64_t missing = 0;        // dropped/skipped sequence numbers
    uint64_t out_of_order = 0;
    uint64_t incomplete = 0;     // fragmented messages discarded (timeout/eviction)
    uint64_t duplicates = 0;
    uint64_t malformed = 0;      // fragments inconsistent with their message
    uint64_t records_written = 0;
    uint64_t last_sequence = 0;
    uint64_t last_timestamp_ns = 0;
};

struct ReceiverStats {
    uint64_t start_monotonic_ns = 0;
    uint64_t start_realtime_ns = 0;
    uint64_t datagrams = 0;          // packets accepted (valid header)
    uint64_t datagram_bytes = 0;
    uint64_t messages = 0;           // complete messages
    uint64_t payload_bytes = 0;
    uint64_t malformed = 0;          // rejected datagrams
    std::map<std::string, uint64_t> malformed_by_reason;
    uint64_t incomplete = 0;
    uint64_t duplicates = 0;
    uint64_t oversize = 0;           // datagrams larger than the receive buffer
    uint64_t recv_errors = 0;
    uint64_t kernel_drops = 0;       // SO_RXQ_OVFL
    uint64_t records_written = 0;
    uint64_t record_bytes = 0;
    uint64_t write_errors = 0;
    uint64_t records_skipped = 0;    // messages not recorded because recording was off/failed
    uint64_t commands = 0;
    uint64_t stats_published = 0;
    uint64_t stats_publish_failures = 0;
    uint64_t reassembly_pending = 0;
    bool recording_enabled = false;
    bool capture_open = false;
    bool capture_failed = false;
    std::string capture_error;
    std::string capture_file;
    std::map<uint32_t, ReceiverSourceStats> sources;
};

// Serializes the statistics as a single JSON object (documented in
// docs/architecture.md).
std::string stats_to_json(const ReceiverStats& s, uint64_t now_monotonic_ns, uint64_t now_realtime_ns);

}  // namespace ipcrelay
