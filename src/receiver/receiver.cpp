#include "receiver.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>

#include "ipcrelay/config.hpp"
#include "ipcrelay/log.hpp"
#include "ipcrelay/signal_handler.hpp"
#include "ipcrelay/time_util.hpp"
#include "ipcrelay/wire_protocol.hpp"
#include "ipcrelay/zmq_util.hpp"

namespace ipcrelay {

namespace {
Reassembler::Config reassembler_config(const ReceiverConfig& cfg) {
    Reassembler::Config r;
    r.max_pending = static_cast<std::size_t>(cfg.reassembly_max_pending);
    r.timeout_ns = static_cast<uint64_t>(cfg.reassembly_timeout_ms) * kNsPerMs;
    r.max_message_length = cfg.reassembly_max_message_bytes;
    return r;
}
}  // namespace

Receiver::Receiver(const ReceiverConfig& cfg) : cfg_(cfg), reassembler_(reassembler_config(cfg)) {
    // Receive buffer large enough for any UDP datagram; oversize datagrams are
    // detected via MSG_TRUNC and counted.
    buffer_.resize(wire::kMaxUdpPayload + 1);
    stats_.capture_file = cfg.capture_file;
}

Receiver::~Receiver() {
    if (writer_.is_open()) {
        if (!writer_.close()) LOG_ERROR("capture close failed: %s", writer_.last_error().c_str());
    }
    if (stats_pub_) zmq_close(stats_pub_);
    if (command_sub_) zmq_close(command_sub_);
    udp_.close();
    if (ctx_) zmq_ctx_term(ctx_);
}

bool Receiver::init(std::string& error) {
    stats_.start_monotonic_ns = now_monotonic_ns();
    stats_.start_realtime_ns = now_realtime_ns();

    MulticastReceiverConfig ucfg;
    ucfg.group_host_order = cfg_.multicast_group;
    ucfg.port = cfg_.multicast_port;
    ucfg.interface_host_order = cfg_.multicast_interface;
    ucfg.receive_buffer_bytes = cfg_.receive_buffer_bytes;
    if (!udp_.open(ucfg, error)) {
        error = "UDP multicast receiver: " + error;
        return false;
    }
    if (cfg_.receive_buffer_bytes > 0 && udp_.actual_receive_buffer() < cfg_.receive_buffer_bytes) {
        LOG_WARN("kernel limited SO_RCVBUF to %d bytes (requested %d); raise net.core.rmem_max to avoid receive drops",
                 udp_.actual_receive_buffer(), cfg_.receive_buffer_bytes);
    }

    ctx_ = zmq_ctx_new();
    if (!ctx_) {
        error = "zmq_ctx_new: " + zmq_error_string(errno);
        return false;
    }
    int linger = 0;
    if (!cfg_.stats_endpoint.empty()) {
        stats_pub_ = zmq_socket(ctx_, ZMQ_PUB);
        if (!stats_pub_) {
            error = "zmq_socket(PUB): " + zmq_error_string(errno);
            return false;
        }
        int hwm = 8;  // never let a slow monitor buffer unboundedly
        zmq_setsockopt(stats_pub_, ZMQ_SNDHWM, &hwm, sizeof hwm);
        zmq_setsockopt(stats_pub_, ZMQ_LINGER, &linger, sizeof linger);
        if (zmq_bind(stats_pub_, cfg_.stats_endpoint.c_str()) != 0) {
            error = "zmq_bind('" + cfg_.stats_endpoint + "') for statistics: " + zmq_error_string(errno);
            return false;
        }
        LOG_INFO("statistics PUB bound to %s (topic '%s', every %d ms)", cfg_.stats_endpoint.c_str(),
                 cfg_.stats_topic.c_str(), cfg_.stats_interval_ms);
    }
    if (!cfg_.command_endpoint.empty()) {
        command_sub_ = zmq_socket(ctx_, ZMQ_SUB);
        if (!command_sub_) {
            error = "zmq_socket(SUB): " + zmq_error_string(errno);
            return false;
        }
        zmq_setsockopt(command_sub_, ZMQ_LINGER, &linger, sizeof linger);
        zmq_setsockopt(command_sub_, ZMQ_SUBSCRIBE, cfg_.command_topic.data(), cfg_.command_topic.size());
        int rc = cfg_.command_bind ? zmq_bind(command_sub_, cfg_.command_endpoint.c_str())
                                   : zmq_connect(command_sub_, cfg_.command_endpoint.c_str());
        if (rc != 0) {
            error = std::string(cfg_.command_bind ? "zmq_bind('" : "zmq_connect('") + cfg_.command_endpoint +
                    "') for commands: " + zmq_error_string(errno);
            return false;
        }
        LOG_INFO("command SUB %s %s", cfg_.command_bind ? "bound to" : "connected to", cfg_.command_endpoint.c_str());
    }

    if (cfg_.record_on_start) {
        if (!set_recording(true)) {
            error = "capture file: " + writer_.last_error();
            return false;
        }
    } else {
        LOG_INFO("recording disabled at start; capture file '%s' will be created on 'record on'", cfg_.capture_file.c_str());
    }

    const uint64_t now = now_monotonic_ns();
    next_stats_ns_ = now + static_cast<uint64_t>(cfg_.stats_interval_ms) * kNsPerMs;
    next_sweep_ns_ = now + std::max<uint64_t>(10, static_cast<uint64_t>(cfg_.reassembly_timeout_ms) / 4) * kNsPerMs;
    next_flush_ns_ = now + static_cast<uint64_t>(cfg_.capture_flush_interval_ms) * kNsPerMs;
    return true;
}

bool Receiver::set_recording(bool enabled) {
    if (enabled) {
        if (writer_.failed()) {
            LOG_ERROR("cannot enable recording: capture file failed earlier (%s); restart to recover", writer_.last_error().c_str());
            return false;
        }
        if (!writer_.is_open()) {
            if (!writer_.open(cfg_.capture_file, static_cast<std::size_t>(cfg_.capture_buffer_bytes), wire::kVersion)) {
                LOG_ERROR("capture file open failed: %s", writer_.last_error().c_str());
                refresh_capture_stats();
                return false;
            }
            LOG_INFO("capture file '%s' opened", cfg_.capture_file.c_str());
        }
        if (!recording_) LOG_INFO("recording ENABLED");
        recording_ = true;
    } else {
        if (recording_) {
            LOG_INFO("recording DISABLED (file stays open; reception and statistics continue)");
            if (writer_.is_open() && !writer_.flush(true)) {
                LOG_ERROR("capture flush failed: %s", writer_.last_error().c_str());
                ++stats_.write_errors;
            }
        }
        recording_ = false;
    }
    refresh_capture_stats();
    return true;
}

void Receiver::refresh_capture_stats() {
    stats_.recording_enabled = recording_;
    stats_.capture_open = writer_.is_open();
    stats_.capture_failed = writer_.failed();
    stats_.capture_error = writer_.last_error();
    stats_.records_written = writer_.records_written();
    stats_.record_bytes = writer_.bytes_written();
    stats_.reassembly_pending = reassembler_.pending();
}

bool Receiver::handle_command(const std::string& raw) {
    ++stats_.commands;
    std::string cmd = to_lower(trim(raw));
    std::replace(cmd.begin(), cmd.end(), '_', ' ');
    std::replace(cmd.begin(), cmd.end(), '=', ' ');
    std::replace(cmd.begin(), cmd.end(), ':', ' ');
    // Collapse repeated spaces.
    std::string norm;
    for (char c : cmd) {
        if (c == ' ' && !norm.empty() && norm.back() == ' ') continue;
        norm.push_back(c);
    }
    LOG_INFO("command received: '%s'", norm.c_str());
    if (norm == "record on" || norm == "record start" || norm == "record enable" || norm == "record true" || norm == "record 1") {
        return set_recording(true);
    }
    if (norm == "record off" || norm == "record stop" || norm == "record disable" || norm == "record false" || norm == "record 0") {
        return set_recording(false);
    }
    if (norm == "record toggle") return set_recording(!recording_);
    if (norm == "flush") {
        if (writer_.is_open() && !writer_.flush(true)) {
            LOG_ERROR("capture flush failed: %s", writer_.last_error().c_str());
            ++stats_.write_errors;
            refresh_capture_stats();
            return false;
        }
        return true;
    }
    if (norm == "stats" || norm == "status") {
        LOG_INFO("stats: %s", stats_json().c_str());
        return true;
    }
    LOG_WARN("unknown command '%s' (expected: record on|off|toggle, flush, stats)", norm.c_str());
    return false;
}

void Receiver::note_malformed(const char* reason, uint32_t source_id, bool have_source) {
    ++stats_.malformed;
    ++stats_.malformed_by_reason[reason];
    if (have_source) ++stats_.sources[source_id].malformed;
    if (malformed_log_budget_ > 0) {
        --malformed_log_budget_;
        LOG_WARN("malformed packet rejected: %s%s", reason,
                 malformed_log_budget_ == 0 ? " (further malformed packets are only counted)" : "");
    } else {
        LOG_DEBUG("malformed packet rejected: %s", reason);
    }
}

void Receiver::handle_datagram(const uint8_t* data, std::size_t len) {
    wire::Header hdr;
    const uint8_t* payload = nullptr;
    const wire::ParseError perr = wire::parse_datagram(data, len, hdr, payload);
    if (perr != wire::ParseError::None) {
        // Only attribute to a source when the header was at least readable.
        const bool have_source = perr != wire::ParseError::Truncated && perr != wire::ParseError::BadMagic &&
                                 perr != wire::ParseError::BadVersion;
        note_malformed(wire::to_string(perr), hdr.source_id, have_source);
        return;
    }
    ++stats_.datagrams;
    stats_.datagram_bytes += len;
    ReceiverSourceStats& ss = stats_.sources[hdr.source_id];
    ++ss.datagrams;

    CompletedMessage msg;
    bool new_message = false;
    const Reassembler::Result r = reassembler_.add(hdr, payload, now_monotonic_ns(), msg, new_message);
    if (new_message) {
        uint64_t missing = 0;
        switch (sequences_.observe(hdr.source_id, hdr.sequence, missing)) {
            case SequenceTracker::Outcome::Gap:
                ++ss.gap_events;
                ss.missing += missing;
                LOG_DEBUG("source %u: sequence gap, %llu message(s) missing before %llu", hdr.source_id,
                          static_cast<unsigned long long>(missing), static_cast<unsigned long long>(hdr.sequence));
                break;
            case SequenceTracker::Outcome::OutOfOrder:
                ++ss.out_of_order;
                break;
            default:
                break;
        }
    }
    switch (r) {
        case Reassembler::Result::Complete:
            on_complete(msg);
            break;
        case Reassembler::Result::Incomplete:
            break;
        case Reassembler::Result::Duplicate:
            ++stats_.duplicates;
            ++ss.duplicates;
            break;
        case Reassembler::Result::Malformed:
            note_malformed("fragment_inconsistent", hdr.source_id, true);
            break;
    }
}

void Receiver::on_complete(const CompletedMessage& m) {
    ++stats_.messages;
    stats_.payload_bytes += m.length;
    ReceiverSourceStats& ss = stats_.sources[m.source_id];
    ++ss.messages;
    ss.payload_bytes += m.length;
    ss.last_sequence = m.sequence;
    ss.last_timestamp_ns = m.timestamp_ns;

    if (!recording_ || !writer_.is_open() || writer_.failed()) {
        ++stats_.records_skipped;
        return;
    }
    capture::RecordHeader rh;
    rh.source_id = m.source_id;
    rh.sequence = m.sequence;
    rh.timestamp_ns = m.timestamp_ns;
    rh.flags = m.flags;
    if (!writer_.write_record(rh, m.data, m.length)) {
        ++stats_.write_errors;
        LOG_ERROR("capture write failed: %s; recording disabled", writer_.last_error().c_str());
        recording_ = false;
        refresh_capture_stats();
        return;
    }
    ++ss.records_written;
}

void Receiver::drain_udp() {
    int budget = cfg_.max_datagrams_per_poll;
    while (budget-- > 0) {
        long n = udp_.receive(buffer_.data(), buffer_.size(), kernel_drops_raw_);
        if (n == 0) break;
        if (n < 0) {
            ++stats_.recv_errors;
            LOG_ERROR("UDP receive failed: %s", std::strerror(errno));
            break;
        }
        if (static_cast<std::size_t>(n) > buffer_.size()) {
            ++stats_.oversize;
            note_malformed("oversize", 0, false);
            continue;
        }
        handle_datagram(buffer_.data(), static_cast<std::size_t>(n));
    }
    stats_.kernel_drops = kernel_drops_raw_;
}

void Receiver::drain_commands() {
    for (int i = 0; i < 64; ++i) {
        int rc = zmq_recv_multipart_nowait(command_sub_, frames_);
        if (rc <= 0) {
            if (rc < 0) LOG_ERROR("command receive failed: %s", zmq_error_string(errno).c_str());
            break;
        }
        // Accept either [command] or [topic][command].
        const std::vector<uint8_t>& f = frames_.size() >= 2 ? frames_[1] : frames_[0];
        handle_command(std::string(f.begin(), f.end()));
    }
}

std::string Receiver::stats_json() const {
    ReceiverStats s = stats_;
    s.recording_enabled = recording_;
    s.capture_open = writer_.is_open();
    s.capture_failed = writer_.failed();
    s.capture_error = writer_.last_error();
    s.records_written = writer_.records_written();
    s.record_bytes = writer_.bytes_written();
    s.reassembly_pending = reassembler_.pending();
    return stats_to_json(s, now_monotonic_ns(), now_realtime_ns());
}

void Receiver::publish_stats() {
    const std::string json = stats_json();
    if (cfg_.stats_print) LOG_INFO("stats: %s", json.c_str());
    if (stats_pub_) {
        // ZMQ_DONTWAIT: if no subscriber is keeping up the message is dropped
        // rather than blocking reception (BRG-075C).
        if (zmq_send_frames(stats_pub_, {cfg_.stats_topic, json}, true)) ++stats_.stats_published;
        else ++stats_.stats_publish_failures;
    }
}

void Receiver::sweep_reassembly() {
    reassembler_.expire(now_monotonic_ns(), [this](const ExpiredMessage& e) {
        ++stats_.incomplete;
        ++stats_.sources[e.source_id].incomplete;
        LOG_DEBUG("source %u seq %llu: incomplete (%u/%u fragments), discarded", e.source_id,
                  static_cast<unsigned long long>(e.sequence), e.fragments_received, e.fragment_count);
    });
}

bool Receiver::poll_once(long timeout_ms) {
    zmq_pollitem_t items[2];
    int n = 0;
    items[n].socket = nullptr;
    items[n].fd = udp_.fd();
    items[n].events = ZMQ_POLLIN;
    items[n].revents = 0;
    ++n;
    if (command_sub_) {
        items[n].socket = command_sub_;
        items[n].fd = 0;
        items[n].events = ZMQ_POLLIN;
        items[n].revents = 0;
        ++n;
    }
    int rc = zmq_poll(items, n, timeout_ms);
    if (rc < 0) {
        if (errno == EINTR) return true;
        LOG_ERROR("zmq_poll failed: %s", zmq_error_string(errno).c_str());
        return false;
    }
    if (rc > 0) {
        if (items[0].revents & ZMQ_POLLIN) drain_udp();
        if (command_sub_ && (items[1].revents & ZMQ_POLLIN)) drain_commands();
    }
    const uint64_t now = now_monotonic_ns();
    if (now >= next_sweep_ns_) {
        sweep_reassembly();
        next_sweep_ns_ = now + std::max<uint64_t>(10, static_cast<uint64_t>(cfg_.reassembly_timeout_ms) / 4) * kNsPerMs;
    }
    if (cfg_.capture_flush_interval_ms > 0 && now >= next_flush_ns_) {
        if (writer_.is_open() && !writer_.failed() && !writer_.flush(cfg_.capture_sync_on_flush)) {
            ++stats_.write_errors;
            LOG_ERROR("capture flush failed: %s", writer_.last_error().c_str());
        }
        next_flush_ns_ = now + static_cast<uint64_t>(cfg_.capture_flush_interval_ms) * kNsPerMs;
    }
    if (now >= next_stats_ns_) {
        publish_stats();
        next_stats_ns_ = now + static_cast<uint64_t>(cfg_.stats_interval_ms) * kNsPerMs;
    }
    return true;
}

int Receiver::run() {
    LOG_INFO("receiver running: %s", describe_receiver_config(cfg_).c_str());
    while (!shutdown_requested()) {
        const uint64_t now = now_monotonic_ns();
        uint64_t next = std::min(next_stats_ns_, next_sweep_ns_);
        if (cfg_.capture_flush_interval_ms > 0) next = std::min(next, next_flush_ns_);
        long timeout_ms = next > now ? static_cast<long>((next - now) / kNsPerMs + 1) : 0;
        if (!poll_once(timeout_ms)) {
            LOG_ERROR("fatal poll error; shutting down");
            break;
        }
    }
    LOG_INFO("shutdown requested (signal %d)", shutdown_signal());
    sweep_reassembly();
    int code = 0;
    if (writer_.is_open()) {
        if (writer_.close()) {
            LOG_INFO("capture file '%s' closed: %llu records, %llu bytes", cfg_.capture_file.c_str(),
                     static_cast<unsigned long long>(writer_.records_written()),
                     static_cast<unsigned long long>(writer_.bytes_written()));
        } else {
            LOG_ERROR("capture file close failed: %s", writer_.last_error().c_str());
            ++stats_.write_errors;
            code = 1;
        }
    }
    LOG_INFO("final stats: %s", stats_json().c_str());
    return code;
}

}  // namespace ipcrelay
