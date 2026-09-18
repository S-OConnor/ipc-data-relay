#include "receiver_stats.hpp"

#include "ipcrelay/json_writer.hpp"

namespace ipcrelay {

std::string stats_to_json(const ReceiverStats& s, uint64_t now_monotonic_ns, uint64_t now_realtime_ns) {
    JsonWriter j;
    j.begin_object();
    j.key("type").value("ipc-relay-receiver-stats");
    j.key("version").value(1);
    j.key("timestamp_ns").value(now_realtime_ns);
    j.key("uptime_s").value(static_cast<double>(now_monotonic_ns - s.start_monotonic_ns) / 1e9);
    j.key("packets_received").value(s.datagrams);
    j.key("packet_bytes").value(s.datagram_bytes);
    j.key("messages_received").value(s.messages);
    j.key("payload_bytes").value(s.payload_bytes);
    j.key("malformed_packets").value(s.malformed);
    j.key("malformed_by_reason").begin_object();
    for (const auto& kv : s.malformed_by_reason) j.key(kv.first).value(kv.second);
    j.end_object();
    j.key("incomplete_fragments").value(s.incomplete);
    j.key("duplicate_fragments").value(s.duplicates);
    j.key("oversize_packets").value(s.oversize);
    j.key("receive_errors").value(s.recv_errors);
    j.key("kernel_drops").value(s.kernel_drops);
    j.key("reassembly_pending").value(s.reassembly_pending);
    j.key("recording").begin_object();
    j.key("enabled").value(s.recording_enabled);
    j.key("file").value(s.capture_file);
    j.key("open").value(s.capture_open);
    j.key("failed").value(s.capture_failed);
    j.key("error").value(s.capture_error);
    j.key("records_written").value(s.records_written);
    j.key("bytes_written").value(s.record_bytes);
    j.key("write_errors").value(s.write_errors);
    j.key("records_skipped").value(s.records_skipped);
    j.end_object();
    j.key("commands_received").value(s.commands);
    j.key("stats_published").value(s.stats_published);
    j.key("stats_publish_failures").value(s.stats_publish_failures);
    j.key("sources").begin_object();
    for (const auto& kv : s.sources) {
        const ReceiverSourceStats& ss = kv.second;
        j.key(std::to_string(kv.first)).begin_object();
        j.key("packets").value(ss.datagrams);
        j.key("messages").value(ss.messages);
        j.key("payload_bytes").value(ss.payload_bytes);
        j.key("sequence_gaps").value(ss.gap_events);
        j.key("dropped_messages").value(ss.missing);
        j.key("out_of_order").value(ss.out_of_order);
        j.key("incomplete_fragments").value(ss.incomplete);
        j.key("duplicate_fragments").value(ss.duplicates);
        j.key("malformed_fragments").value(ss.malformed);
        j.key("records_written").value(ss.records_written);
        j.key("last_sequence").value(ss.last_sequence);
        j.key("last_timestamp_ns").value(ss.last_timestamp_ns);
        j.end_object();
    }
    j.end_object();
    j.end_object();
    return j.str();
}

}  // namespace ipcrelay
