#include "ipcrelay/bridge/bridge.hpp"

#include <cerrno>
#include <cstring>
#include <sys/uio.h>

#include "ipcrelay/common/log.hpp"
#include "ipcrelay/common/net_util.hpp"
#include "ipcrelay/common/signal_handler.hpp"
#include "ipcrelay/common/time_util.hpp"
#include "ipcrelay/common/wire_protocol.hpp"
#include "ipcrelay/common/zmq_util.hpp"

namespace ipcrelay {

Bridge::Bridge(const BridgeConfig& cfg) : cfg_(cfg) {}

Bridge::~Bridge() {
    for (auto& s : sources_) {
        if (s.socket) zmq_close(s.socket);
    }
    sender_.close();
    if (ctx_) zmq_ctx_term(ctx_);
}

bool Bridge::init(std::string& error) {
    MulticastSenderConfig scfg;
    scfg.group_host_order = cfg_.multicast_group;
    scfg.port = cfg_.multicast_port;
    scfg.interface_host_order = cfg_.multicast_interface;
    scfg.ttl = cfg_.multicast_ttl;
    scfg.loopback = cfg_.multicast_loopback;
    scfg.send_buffer_bytes = cfg_.send_buffer_bytes;
    scfg.send_timeout_ms = cfg_.send_timeout_ms;
    if (!sender_.open(scfg, error)) {
        error = "UDP multicast sender: " + error;
        return false;
    }

    ctx_ = zmq_ctx_new();
    if (!ctx_) {
        error = "zmq_ctx_new: " + zmq_error_string(errno);
        return false;
    }
    zmq_ctx_set(ctx_, ZMQ_IO_THREADS, cfg_.zmq_io_threads);

    sources_.clear();
    poll_items_.clear();
    source_stats_.assign(cfg_.sources.size(), BridgeSourceStats{});
    for (const auto& sc : cfg_.sources) {
        Source s;
        s.cfg = sc;
        // One SUB socket per source (BRG-014). SUB sockets never consume
        // messages destined for other subscribers: a PUB socket copies each
        // message to every connected subscriber (BRG-012).
        s.socket = zmq_socket(ctx_, ZMQ_SUB);
        if (!s.socket) {
            error = "zmq_socket(SUB) for source " + std::to_string(sc.id) + ": " + zmq_error_string(errno);
            return false;
        }
        int hwm = sc.recv_hwm > 0 ? sc.recv_hwm : cfg_.zmq_recv_hwm;
        zmq_setsockopt(s.socket, ZMQ_RCVHWM, &hwm, sizeof hwm);
        int linger = 0;
        zmq_setsockopt(s.socket, ZMQ_LINGER, &linger, sizeof linger);
        int ivl = cfg_.reconnect_ivl_ms;
        zmq_setsockopt(s.socket, ZMQ_RECONNECT_IVL, &ivl, sizeof ivl);
        if (sc.filters.empty()) {
            if (zmq_setsockopt(s.socket, ZMQ_SUBSCRIBE, "", 0) != 0) {
                error = "zmq_setsockopt(SUBSCRIBE) for source " + std::to_string(sc.id) + ": " + zmq_error_string(errno);
                return false;
            }
        } else {
            for (const auto& f : sc.filters) {
                if (zmq_setsockopt(s.socket, ZMQ_SUBSCRIBE, f.data(), f.size()) != 0) {
                    error = "zmq_setsockopt(SUBSCRIBE '" + f + "') for source " + std::to_string(sc.id) + ": " + zmq_error_string(errno);
                    return false;
                }
            }
        }
        if (zmq_connect(s.socket, sc.endpoint.c_str()) != 0) {
            error = "zmq_connect('" + sc.endpoint + "') for source " + std::to_string(sc.id) + ": " + zmq_error_string(errno);
            return false;
        }
        zmq_pollitem_t item{};
        item.socket = s.socket;
        item.events = ZMQ_POLLIN;
        poll_items_.push_back(item);
        sources_.push_back(s);
        LOG_INFO("source %u%s: connected SUB to %s (%zu filter(s), hwm=%d)", sc.id,
                 sc.name.empty() ? "" : (" '" + sc.name + "'").c_str(), sc.endpoint.c_str(), sc.filters.size(), hwm);
    }
    last_stats_ns_ = now_monotonic_ns();
    return true;
}

bool Bridge::poll_once(long timeout_ms) {
    int rc = zmq_poll(poll_items_.data(), static_cast<int>(poll_items_.size()), timeout_ms);
    ++stats_.poll_iterations;
    if (rc < 0) {
        if (errno == EINTR) return true;
        ++stats_.poll_errors;
        LOG_ERROR("zmq_poll failed: %s", zmq_error_string(errno).c_str());
        return errno == EAGAIN;  // anything else is fatal (ETERM, EFAULT)
    }
    if (rc == 0) return true;
    // Round-robin over every readable source, draining a bounded number of
    // messages from each so a high-rate source cannot starve the others
    // (BRG-017, BRG-018).
    for (std::size_t i = 0; i < poll_items_.size(); ++i) {
        if (poll_items_[i].revents & ZMQ_POLLIN) service_source(i);
    }
    return true;
}

void Bridge::service_source(std::size_t index) {
    Source& src = sources_[index];
    BridgeSourceStats& st = source_stats_[index];
    int budget = cfg_.max_messages_per_poll;
    while (budget-- > 0) {
        zmq_msg_t msg;
        zmq_msg_init(&msg);
        int rc = zmq_msg_recv(&msg, src.socket, ZMQ_DONTWAIT);
        if (rc < 0) {
            int err = errno;
            zmq_msg_close(&msg);
            if (err == EAGAIN) return;
            if (err == EINTR) continue;
            ++st.recv_errors;
            LOG_ERROR("source %u: zmq_msg_recv failed: %s", src.cfg.id, zmq_error_string(err).c_str());
            return;  // BRG-111: give up on this source for now, others continue
        }
        const uint64_t ts = now_realtime_ns();
        int more = zmq_msg_more(&msg);
        if (!more) {
            // Single-frame message: transmit directly from the zmq buffer
            // without copying (BRG-120).
            transmit(index, static_cast<const uint8_t*>(zmq_msg_data(&msg)), zmq_msg_size(&msg), 0, ts);
            zmq_msg_close(&msg);
        } else {
            // Multipart message: concatenate all frames into one payload.
            concat_.assign(static_cast<const uint8_t*>(zmq_msg_data(&msg)),
                           static_cast<const uint8_t*>(zmq_msg_data(&msg)) + zmq_msg_size(&msg));
            zmq_msg_close(&msg);
            bool ok = true;
            while (more) {
                zmq_msg_t part;
                zmq_msg_init(&part);
                // Remaining frames of a multipart message are always
                // available atomically once the first frame arrived.
                rc = zmq_msg_recv(&part, src.socket, ZMQ_DONTWAIT);
                if (rc < 0) {
                    int err = errno;
                    zmq_msg_close(&part);
                    if (err == EINTR) continue;
                    ++st.recv_errors;
                    LOG_ERROR("source %u: zmq_msg_recv (multipart) failed: %s", src.cfg.id, zmq_error_string(err).c_str());
                    ok = false;
                    break;
                }
                const auto* d = static_cast<const uint8_t*>(zmq_msg_data(&part));
                concat_.insert(concat_.end(), d, d + zmq_msg_size(&part));
                more = zmq_msg_more(&part);
                zmq_msg_close(&part);
            }
            if (!ok) return;
            ++st.multipart;
            transmit(index, concat_.data(), concat_.size(), wire::kFlagMultipart, ts);
        }
    }
    // Budget exhausted while the socket may still be readable: zmq_poll will
    // report it again on the next iteration. Count it as a backlog indicator.
    ++stats_.max_batch_hits;
}

bool Bridge::transmit(std::size_t index, const uint8_t* data, std::size_t len, uint16_t flags, uint64_t timestamp_ns) {
    Source& src = sources_[index];
    BridgeSourceStats& st = source_stats_[index];
    ++st.messages;
    st.bytes += len;
    const uint64_t sequence = st.next_sequence++;  // BRG-047: one per complete message

    const std::size_t chunk = cfg_.fragment_payload();
    const std::size_t count = wire::fragment_count_for(len, chunk);
    if (len > wire::kMaxMessageLength || count > 0xFFFFu) {
        ++st.oversize;
        LOG_WARN("source %u: message of %zu bytes needs %zu fragments; exceeds protocol limits, dropped",
                 src.cfg.id, len, count);
        return false;
    }
    if (count > 1) {
        ++st.fragmented;
        flags |= wire::kFlagFragmented;
    }
    LOG_DEBUG("source %u seq %llu: %zu bytes in %zu datagram(s)", src.cfg.id,
              static_cast<unsigned long long>(sequence), len, count);

    uint8_t hdr_bytes[wire::kHeaderSize];
    wire::Header h;
    h.flags = flags;
    h.source_id = src.cfg.id;
    h.message_length = static_cast<uint32_t>(len);
    h.sequence = sequence;
    h.timestamp_ns = timestamp_ns;
    h.fragment_count = static_cast<uint16_t>(count);

    std::size_t offset = 0;
    for (std::size_t i = 0; i < count; ++i) {
        const std::size_t n = (len - offset) < chunk ? (len - offset) : chunk;
        h.fragment_index = static_cast<uint16_t>(i);
        h.fragment_offset = static_cast<uint32_t>(offset);
        h.fragment_length = static_cast<uint16_t>(n);
        wire::encode_header(h, hdr_bytes);

        struct iovec iov[2];
        iov[0].iov_base = hdr_bytes;
        iov[0].iov_len = wire::kHeaderSize;
        iov[1].iov_base = const_cast<uint8_t*>(data + offset);
        iov[1].iov_len = n;
        long sent = sender_.send(iov, n > 0 ? 2 : 1);
        if (sent < 0) {
            const int err = errno;
            if (err == EAGAIN || err == EWOULDBLOCK) {
                ++st.send_timeouts;
                ++stats_.send_timeouts;
            } else {
                ++st.send_errors;
                ++stats_.send_errors;
            }
            if (send_error_log_budget_ > 0) {
                --send_error_log_budget_;
                LOG_ERROR("source %u seq %llu: UDP send failed (%s)%s", src.cfg.id,
                          static_cast<unsigned long long>(sequence), std::strerror(err),
                          send_error_log_budget_ == 0 ? "; further send errors are only counted" : "");
            }
            return false;  // remaining fragments are pointless without this one
        }
        ++st.datagrams;
        ++stats_.datagrams_sent;
        stats_.bytes_sent += static_cast<uint64_t>(sent);
        offset += n;
    }
    return true;
}

void Bridge::log_stats() const {
    LOG_INFO("stats: datagrams=%llu bytes=%llu send_errors=%llu send_timeouts=%llu batch_limit_hits=%llu polls=%llu",
             static_cast<unsigned long long>(stats_.datagrams_sent), static_cast<unsigned long long>(stats_.bytes_sent),
             static_cast<unsigned long long>(stats_.send_errors), static_cast<unsigned long long>(stats_.send_timeouts),
             static_cast<unsigned long long>(stats_.max_batch_hits), static_cast<unsigned long long>(stats_.poll_iterations));
    for (std::size_t i = 0; i < sources_.size(); ++i) {
        const auto& st = source_stats_[i];
        LOG_INFO("  source %u: messages=%llu bytes=%llu datagrams=%llu fragmented=%llu multipart=%llu recv_errors=%llu send_errors=%llu send_timeouts=%llu oversize=%llu",
                 sources_[i].cfg.id, static_cast<unsigned long long>(st.messages), static_cast<unsigned long long>(st.bytes),
                 static_cast<unsigned long long>(st.datagrams), static_cast<unsigned long long>(st.fragmented),
                 static_cast<unsigned long long>(st.multipart), static_cast<unsigned long long>(st.recv_errors),
                 static_cast<unsigned long long>(st.send_errors), static_cast<unsigned long long>(st.send_timeouts),
                 static_cast<unsigned long long>(st.oversize));
    }
}

int Bridge::run() {
    LOG_INFO("bridge running: %s", describe_bridge_config(cfg_).c_str());
    const uint64_t stats_interval_ns = static_cast<uint64_t>(cfg_.stats_interval_ms) * kNsPerMs;
    while (!shutdown_requested()) {
        long timeout_ms = 1000;
        if (stats_interval_ns > 0) {
            const uint64_t now = now_monotonic_ns();
            const uint64_t due = last_stats_ns_ + stats_interval_ns;
            if (now >= due) {
                log_stats();
                last_stats_ns_ = now;
                timeout_ms = cfg_.stats_interval_ms;
            } else {
                uint64_t remaining_ms = (due - now) / kNsPerMs + 1;
                timeout_ms = remaining_ms < 1000 ? static_cast<long>(remaining_ms) : 1000;
            }
        }
        if (!poll_once(timeout_ms)) {
            LOG_ERROR("fatal poll error; shutting down");
            log_stats();
            return 1;
        }
    }
    LOG_INFO("shutdown requested (signal %d); final statistics:", shutdown_signal());
    log_stats();
    return 0;
}

}  // namespace ipcrelay
