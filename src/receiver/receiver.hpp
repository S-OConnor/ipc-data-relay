// UDP multicast receiver / binary capture application core (BRG-070..BRG-093).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <zmq.h>

#include "ipcrelay/capture_writer.hpp"
#include "ipcrelay/reassembler.hpp"
#include "ipcrelay/sequence_tracker.hpp"
#include "ipcrelay/udp_multicast.hpp"
#include "receiver_config.hpp"
#include "receiver_stats.hpp"

namespace ipcrelay {

class Receiver {
public:
    explicit Receiver(const ReceiverConfig& cfg);
    ~Receiver();
    Receiver(const Receiver&) = delete;
    Receiver& operator=(const Receiver&) = delete;

    bool init(std::string& error);
    int run();

    // One poll iteration (exposed for tests). Returns false on fatal error.
    bool poll_once(long timeout_ms);

    // Processes a single raw datagram (exposed for tests).
    void handle_datagram(const uint8_t* data, std::size_t len);

    // Applies a textual runtime command; returns false if unknown.
    bool handle_command(const std::string& command);

    bool set_recording(bool enabled);
    bool recording() const { return recording_; }

    const ReceiverStats& stats() const { return stats_; }
    std::string stats_json() const;

private:
    void drain_udp();
    void drain_commands();
    void publish_stats();
    void sweep_reassembly();
    void on_complete(const CompletedMessage& m);
    void note_malformed(const char* reason, uint32_t source_id, bool have_source);
    void refresh_capture_stats();

    ReceiverConfig cfg_;
    void* ctx_ = nullptr;
    void* stats_pub_ = nullptr;
    void* command_sub_ = nullptr;
    UdpMulticastReceiver udp_;
    std::vector<uint8_t> buffer_;
    std::vector<std::vector<uint8_t>> frames_;
    Reassembler reassembler_;
    SequenceTracker sequences_;
    capture::CaptureWriter writer_;
    bool recording_ = false;
    ReceiverStats stats_;
    uint32_t kernel_drops_raw_ = 0;
    uint64_t next_stats_ns_ = 0;
    uint64_t next_sweep_ns_ = 0;
    uint64_t next_flush_ns_ = 0;
    uint64_t malformed_log_budget_ = 20;
};

}  // namespace ipcrelay
