# Architecture and Data Flow

## Overview

```
  ZeroMQ PUB (app A) --ipc--+
                            |   +------------------+   UDP multicast    +--------------------+
  ZeroMQ PUB (app B) --ipc--+-->| ipc-relay-bridge |==================>| ipc-relay-receiver |--> capture.cap
                            |   |  N x ZMQ_SUB     |  239.x.x.x:port   |  reassembly, stats |
  ZeroMQ PUB (app C) --ipc--+   |  1 x UDP socket  |  (1 stream)       |  ZMQ TCP PUB stats |--> monitors
                                +------------------+                    |  ZMQ TCP SUB cmds  |<-- ipc-relay-ctl
  (other subscribers keep       ^ zmq_poll over all sources             +--------------------+
   receiving as before)                                                   ^ stats   | cmds
                                                                    +--------------------+  HTTP + WebSocket
                                                                    | ipc-relay-ctl serve|<=================> browser
                                                                    +--------------------+  (JSON)
```

The system has two applications (BRG-003) plus two tools:

| Binary               | Role |
|----------------------|------|
| `ipc-relay-bridge`   | Subscribes passively to N ZeroMQ IPC PUB endpoints and republishes each message over one UDP multicast stream with a compact binary header. |
| `ipc-relay-receiver` | Joins the multicast group, validates and reassembles datagrams, detects loss, records messages into one binary capture file, publishes statistics and accepts runtime commands over ZeroMQ TCP. |
| `ipc-relay-testpub`  | Test publisher that generates verifiable messages on one or more IPC endpoints. |
| `ipc-relay-ctl`      | Web control page for the receiver: a backend that relays statistics and commands between the receiver's ZeroMQ channels and browsers over a WebSocket. Also has one-shot `send`/`monitor` modes. |
| `capture_inspect.py` | Standalone capture file validator. |

## Bridge

### Sockets

* One `ZMQ_SUB` socket per configured source (BRG-011, BRG-014), connected
  to the publisher's `ipc://` endpoint. The bridge never binds and never
  uses PULL/REQ/PAIR patterns, so it is one more subscriber among any
  others: ZeroMQ PUB delivers every message to every connected subscriber
  independently, and the bridge cannot consume messages meant for another
  subscriber (BRG-012). The end-to-end test runs an independent subscriber
  next to the bridge to demonstrate this (BRG-131).
* Subscription filters are configurable per source; no `filter` (or an
  empty one) subscribes to everything (BRG-020).
* One UDP socket, `connect()`ed to the multicast group/port, with
  `IP_MULTICAST_IF`, `IP_MULTICAST_TTL`, `IP_MULTICAST_LOOP`, `SO_SNDBUF`
  and `SO_SNDTIMEO` set from configuration (BRG-031..BRG-033, BRG-101).

### Main loop

A single thread runs `zmq_poll()` over all SUB sockets (BRG-015, BRG-016).
For every readable socket it drains at most `max_messages_per_poll`
messages with `ZMQ_DONTWAIT` and then moves on to the next socket
(round-robin), so a high-rate source cannot starve the others and no
individual source can block the loop (BRG-017, BRG-018). If a source still
has data, `zmq_poll` reports it again immediately on the next iteration; the
`batch_limit_hits` counter shows how often this happens.

Each ZeroMQ message is stamped with `CLOCK_REALTIME` and the source's next
sequence number, then fragmented as described in
[wire_protocol.md](wire_protocol.md). Single-frame messages are transmitted
straight from the ZeroMQ message buffer using `sendmsg()` scatter/gather
(header iovec + payload iovec), so the payload is never copied in user space
(BRG-120). Multipart messages are concatenated once into a scratch buffer.

UDP sends block for at most `send_timeout_ms` when the socket buffer is
full; a timeout or error is counted (`send_timeouts`, `send_errors`),
logged (rate-limited) and the message is dropped, but the loop continues
(BRG-111, BRG-112). ZeroMQ receive errors on one source are counted and
logged without affecting other sources.

### Statistics

Every `stats_interval_ms` the bridge logs global and per-source counters:
messages, bytes, datagrams, fragmented, multipart, receive errors, send
errors/timeouts, oversize drops and batch-limit hits (BRG-122). Final
counters are logged on shutdown.

## Receiver

### Sockets

* One UDP socket bound to `group:port` with `SO_REUSEADDR`, `SO_RCVBUF`,
  `IP_ADD_MEMBERSHIP` on the configured interface and `SO_RXQ_OVFL` so that
  kernel receive-queue drops are visible in the statistics (BRG-070,
  BRG-122).
* Optional `ZMQ_PUB` bound to `stats_endpoint` (TCP) that publishes a JSON
  statistics message every `stats_interval_ms` (BRG-075A). The socket has a
  small send high-water mark and sends with `ZMQ_DONTWAIT`, so a slow or
  absent monitor never blocks reception (BRG-075C).
* Optional `ZMQ_SUB` on `command_endpoint` (TCP) for runtime commands
  (BRG-077). By default it *connects* to a controller's PUB socket (which is
  what `ipc-relay-ctl send` binds); set `command_bind = true` to have the
  receiver bind instead.

### Main loop

A single thread runs `zmq_poll()` over the UDP file descriptor and the
command socket with a timeout derived from the next statistics, reassembly
sweep or capture flush deadline. Datagrams are drained in batches of
`max_datagrams_per_poll` using `recvmsg(MSG_DONTWAIT | MSG_TRUNC)`.

Each datagram goes through:

1. **Validation** (`wire::parse_datagram`): magic, version, lengths and
   fragment consistency. Failures are counted per reason under
   `malformed_by_reason` and the datagram is dropped (BRG-071, BRG-113).
2. **Sequence tracking** per `source_id` on the first fragment of each
   `(source_id, sequence)`; gaps, missing counts and out-of-order arrivals
   are counted per source (BRG-072, BRG-073).
3. **Reassembly** (`Reassembler`): unfragmented messages take a zero-copy
   fast path. Fragments are stored in a map keyed by `(source_id,
   sequence)` bounded by `reassembly_max_pending` entries and
   `reassembly_max_message_bytes` per entry; the oldest entry is evicted
   when the bound is reached and a periodic sweep discards entries older
   than `reassembly_timeout_ms`, counting them as incomplete (BRG-064..066,
   BRG-114).
4. **Recording**: complete messages are appended to the capture file
   through a buffered writer when recording is enabled (BRG-080..BRG-089).
   Write errors disable recording and are reported; reception and
   statistics continue (BRG-093).

### Runtime control

Commands are plain text in the last frame of a ZeroMQ message (optionally
preceded by a topic frame). Case and `_`/`=`/`:` separators are ignored.

| Command                    | Effect |
|----------------------------|--------|
| `record on` / `record off` | Enable/disable writing records (BRG-076, BRG-078, BRG-079). The file stays open while disabled; the first `record on` creates it if `record_on_start = false`. |
| `record toggle`            | Flip the recording state. |
| `flush`                    | Flush and `fsync` the capture file. |
| `stats`                    | Log the current statistics JSON immediately. |

### Statistics message

Published as a two-frame ZeroMQ message `[stats_topic][json]`. Fields
(BRG-075B):

```json
{
  "type": "ipc-relay-receiver-stats", "version": 1,
  "timestamp_ns": 1789746853514171829, "uptime_s": 12.345,
  "packets_received": 0, "packet_bytes": 0,
  "messages_received": 0, "payload_bytes": 0,
  "malformed_packets": 0, "malformed_by_reason": {"bad_magic": 0},
  "incomplete_fragments": 0, "duplicate_fragments": 0,
  "oversize_packets": 0, "receive_errors": 0, "kernel_drops": 0,
  "reassembly_pending": 0,
  "recording": {"enabled": true, "file": "...", "open": true, "failed": false, "error": "",
                "records_written": 0, "bytes_written": 0, "write_errors": 0, "records_skipped": 0},
  "commands_received": 0, "stats_published": 0, "stats_publish_failures": 0,
  "sources": {
    "1": {"packets": 0, "messages": 0, "payload_bytes": 0, "sequence_gaps": 0,
          "dropped_messages": 0, "out_of_order": 0, "incomplete_fragments": 0,
          "duplicate_fragments": 0, "malformed_fragments": 0, "records_written": 0,
          "last_sequence": 0, "last_timestamp_ns": 0}
  }
}
```

`kernel_drops` is the running `SO_RXQ_OVFL` value: datagrams the kernel
discarded because the socket receive buffer was full (receive-side
overrun). `dropped_messages` per source is the number of sequence numbers
never seen. The same JSON is logged as `final stats:` on shutdown.

## Control tool web interface

`ipc-relay-ctl serve` is a single-threaded backend (one `zmq_poll()` loop
over the ZeroMQ sockets, the HTTP listening socket and every browser
connection):

* A `ZMQ_SUB` connects to the receiver's `stats_endpoint`. The last valid
  statistics JSON object is kept. Messages that are not a JSON object are
  ignored.
* A `ZMQ_PUB` stays open for the life of the process. It binds
  `--command-endpoint` by default, or connects with `--connect` when the
  receiver binds. A persistent socket avoids the PUB/SUB slow-joiner
  delay of one-shot `send`.
* A small HTTP/1.1 server serves the frontend (`/`, `/app.js`,
  `/style.css`, compiled in from `tools/ctl/frontend/`), `GET /api/stats`
  and the WebSocket endpoint `/ws` (RFC 6455, text frames only). Upgrades
  whose `Origin` does not match `Host` are rejected, so another web page
  in the operator's browser cannot drive the receiver. There is no
  authentication.
* Every `--update-ms` the latest snapshot goes to every WebSocket client.
  A client with more than 1 MiB queued is skipped until it catches up.
  After a command, the next statistics message is pushed straight away so
  the page shows the new recording state without waiting a full interval.

Messages on `/ws` are JSON text frames.

Backend to browser:

```json
{"type": "hello", "protocol_version": 1, "tool_version": "1.0.0",
 "update_interval_ms": 1000, "stale_after_ms": 3000,
 "stats_endpoint": "tcp://127.0.0.1:5556", "command_endpoint": "tcp://127.0.0.1:5557"}

{"type": "stats", "update": 42, "server_time_ns": 1789746853514171829,
 "receiver_online": true, "stats_age_ms": 180, "stats_received": 97,
 "stats": { ...receiver statistics message, see above, or null before the first one... }}

{"type": "command_result", "id": 1, "command": "record on", "ok": true, "error": ""}
```

Browser to backend:

```json
{"type": "record", "enabled": true, "id": 1}
```

`hello` and one `stats` message are sent on connect, then `stats` at the
update rate. `receiver_online` is false when no statistics arrived for
`stale_after_ms`. `command_result.ok` means the backend published the
command. PUB/SUB has no acknowledgement, so the page treats the recording
state in the following `stats` messages as confirmation. `id` is echoed
back.

## Shutdown

Both applications install `SIGINT`/`SIGTERM` handlers (without
`SA_RESTART`) that set a flag; the poll loops notice it within one poll
timeout, log final statistics, and the receiver flushes, `fsync`s and closes
the capture file (BRG-092, BRG-110).

## Source layout

```
include/ipcrelay/     public headers of the common library
src/common/           wire protocol, config parser, UDP, ZeroMQ helpers, logging
src/bridge/           ipc-relay-bridge (IPC subscribers -> UDP multicast sender)
tools/receiver/       ipc-relay-receiver
tools/receiver/lib/   capture format, reassembly, sequence tracking, JSON (receiver-only)
tools/testpub/        ipc-relay-testpub
tools/ctl/            ipc-relay-ctl (CLI modes in main.cpp)
tools/ctl/backend/    web backend: HTTP, WebSocket, JSON parser, control server
tools/ctl/frontend/   browser frontend (embedded into the binary at build time)
tools/                capture_inspect.py
tests/unit/           unit tests (self-contained framework)
tests/integration/    end-to-end shell/Python tests (run by ctest)
docker/               build/runtime images and compose file for the full pipeline
examples/             example configurations
packaging/            systemd units and Yocto recipe
docs/                 this documentation
```
