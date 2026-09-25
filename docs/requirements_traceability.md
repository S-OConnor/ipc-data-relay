# Requirements Traceability

Maps every requirement in
[zmq_ipc_udp_multicast_shall_requirements.csv](zmq_ipc_udp_multicast_shall_requirements.csv)
to its implementation and verification.

| ID | Implementation | Verification |
|----|----------------|--------------|
| BRG-001 | C++17 throughout (`src/`, `include/`, `tools/`) | build |
| BRG-002 | POSIX/Linux only; `docs/yocto.md`, `packaging/yocto/` | CI build on Debian |
| BRG-003 | `ipc-relay-bridge`, `ipc-relay-receiver` | e2e tests |
| BRG-004 | `CMakeLists.txt`, `cmake/FindZeroMQ.cmake` | CI `build` job |
| BRG-005 | Only libzmq + libc/libstdc++ linked; tests use an in-tree framework; tools use Python stdlib | `ldd` |
| BRG-010, BRG-011, BRG-013 | `Bridge::init` creates one `ZMQ_SUB` per `[source]`; `bridge_config.cpp` | `e2e_multi_source_pipeline` (3 sources) |
| BRG-012 | SUB sockets only, connect-only; `docs/architecture.md` | `zmq_pubsub_multiple_subscribers_receive_everything`, `e2e_multi_source_pipeline` (independent subscriber) |
| BRG-014 | `Bridge::Source::socket` | unit `bridge_config_valid`; code |
| BRG-015, BRG-016 | `Bridge::poll_once` uses `zmq_poll` from one thread | code; e2e |
| BRG-017, BRG-018 | `ZMQ_DONTWAIT` receive, `max_messages_per_poll` round-robin (`Bridge::service_source`) | e2e stress run |
| BRG-019 | `[source] id` validated unique; placed in every header | unit `bridge_config_invalid_reports_all_problems`; e2e |
| BRG-020 | `filter` / `subscribe_all` per source | unit `bridge_config_valid`; e2e (source 3 filter `TLM`) |
| BRG-030..BRG-033 | `UdpMulticastSender::open`, `multicast_*` options | e2e (loopback); config tests |
| BRG-034 | Raw `sendmsg` datagrams; receiver test uses Python sockets | `receiver_fault_injection` |
| BRG-035 | Payload copied verbatim (scatter/gather) | `capture_inspect.py --verify-testpub` in e2e |
| BRG-036, BRG-040..BRG-053 | `include/ipcrelay/common/wire_protocol.hpp`, `src/common/wire_protocol.cpp`, `byteorder.hpp`; `docs/wire_protocol.md` | unit `wire_*`; `receiver_fault_injection` (independent encoder) |
| BRG-047 | `Bridge::transmit` increments per-source `next_sequence` once per message | e2e `--strict` (no gaps) |
| BRG-048, BRG-049 | `now_realtime_ns()` in header | unit `wire_roundtrip_single_fragment`; fault test timestamp check |
| BRG-060..BRG-063 | `max_datagram_size` (default 1400), `Bridge::transmit` fragmentation | unit `wire_fragment_count`; e2e (fragmented counts) |
| BRG-064..BRG-066 | `Reassembler`, `reassembly_timeout_ms`, `reassembly_max_pending` | unit `reassembler_*`; `receiver_fault_injection` |
| BRG-070 | `UdpMulticastReceiver::open` | e2e |
| BRG-071, BRG-113 | `wire::parse_datagram`, `Receiver::handle_datagram` | unit `wire_rejects_malformed`; `receiver_fault_injection` |
| BRG-072, BRG-073 | `SequenceTracker` keyed by `source_id` | unit `sequence_*`; `receiver_fault_injection` |
| BRG-074, BRG-075 | `ReceiverStats`, `stats_print` | `e2e_runtime_control`, fault test |
| BRG-075A..BRG-075C | `Receiver::publish_stats` on `ZMQ_PUB` tcp with `ZMQ_DONTWAIT`, JSON in `receiver_stats.cpp` | `e2e_runtime_control` (`ipc-relay-ctl monitor`), `ctl_web_backend` (`ipc-relay-ctl serve`) |
| BRG-076..BRG-079 | `Receiver::handle_command`, `set_recording`; `ZMQ_SUB` tcp command channel | `e2e_runtime_control`, `ctl_web_backend` (record on/off from the web protocol) |
| BRG-080..BRG-089 | `capture_format.hpp/.cpp`, `capture_writer.cpp`; `docs/capture_format.md` | unit `capture_*`; e2e; fault test |
| BRG-090, BRG-091 | `docs/capture_format.md`; `tools/capture_inspect.py` (stdlib only) | `capture_inspect_selftest`; e2e |
| BRG-092 | `Receiver::run` flush + fsync + close on shutdown | e2e (records count after SIGTERM) |
| BRG-093 | `CaptureWriter::fail`, `write_errors` statistic | unit `capture_writer_reports_open_error` |
| BRG-100..BRG-102 | Config files and `--set`/`--source` overrides | unit `config_*`; e2e configs |
| BRG-103 | `build_bridge_config` / `build_receiver_config` collect all problems; `--check` | unit `*_invalid*`; CI `config-examples` |
| BRG-110 | `signal_handler.cpp`; orderly shutdown in `run()` | e2e (SIGTERM exit code 0) |
| BRG-111 | Per-source error handling in `Bridge::service_source`; per-datagram rejection in receiver | fault test (source 2 unaffected) |
| BRG-112 | `recv_errors`, `send_errors`, `send_timeouts` counters and logs | code |
| BRG-114 | Bounded reassembly map, fixed buffers, `ZMQ_SNDHWM` on stats | unit `reassembler_bounded_pending` |
| BRG-115 | `log_level`, `LOG_DEBUG` gated at runtime | code |
| BRG-120 | `sendmsg` scatter/gather, zero-copy single-fragment path | code; unit `reassembler_single_fragment_fast_path` |
| BRG-121 | Single poll loop, batching; 120k msgs / 300 MB in 1.6 s on loopback | stress run (`PER_SOURCE=40000 RATE=0`) |
| BRG-122 | Bridge and receiver counters incl. `kernel_drops` (`SO_RXQ_OVFL`), `batch_limit_hits` | e2e output |
| BRG-130 | `ipc-relay-testpub` | e2e |
| BRG-131 | — | `zmq_pubsub_multiple_subscribers_receive_everything`; `e2e_multi_source_pipeline` |
| BRG-132, BRG-133 | — | `e2e_multi_source_pipeline` |
| BRG-134 | — | unit `sequence_*`; `receiver_fault_injection` |
| BRG-135 | — | unit `reassembler_*`; e2e (fragmented messages) |
| BRG-136 | — | unit `reassembler_out_of_order_and_duplicates`, `reassembler_missing_fragment_times_out`; `receiver_fault_injection` |
| BRG-137 | `tools/capture_inspect.py` | `capture_inspect_selftest`; e2e |
| BRG-140 | `README.md` | review |
| BRG-141 | `docs/architecture.md` | review |
| BRG-142 | `docs/wire_protocol.md` | review; fault test encoder derived from it |
| BRG-143 | `docs/capture_format.md` | review; `capture_inspect.py` derived from it |
| BRG-144 | `examples/bridge-three-publishers.conf`, `examples/bridge.conf` | CI `config-examples` |
| BRG-145 | `docs/limitations.md` | review |
| BRG-146 | `docs/yocto.md`, `packaging/` | review |
