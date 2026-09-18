# Assumptions, Limitations and Trade-offs

## Assumptions

* **Platform**: Linux with POSIX sockets and libzmq 4.x. Linux-specific
  features used: `SO_RXQ_OVFL` (receive-drop counter, optional),
  `SOCK_CLOEXEC`/`SOCK_NONBLOCK`, `MSG_NOSIGNAL`, `CLOCK_MONOTONIC`.
* **ZeroMQ pattern**: every input is a PUB socket that the bridge connects
  to as a SUB. The bridge does not support PUSH/PULL, PAIR or XPUB/XSUB
  inputs.
* **Endianness**: the wire protocol and capture file are little-endian by
  definition; serialization is explicit so the code is correct on any host,
  but no big-endian variant of the formats exists or is planned.
* **Clock**: `timestamp_ns` is the bridge's `CLOCK_REALTIME` at reception
  from ZeroMQ. It reflects the bridge host's clock (and its NTP/PTP
  discipline), not the original publisher's send time. Timestamps are only
  comparable across sources if they are read by the same bridge.
* **Source identifiers** are assigned by configuration and must be unique
  per bridge. If several bridges feed the same multicast group, their
  configurations must use disjoint identifiers; the receiver keys all
  state on `source_id` alone.
* **IPv4 only**. Multicast groups must lie in 224.0.0.0/4.

## Expected packet-loss behaviour

UDP multicast is unreliable by design. The bridge never retransmits and the
receiver never requests retransmission; loss is **detected and counted**,
not corrected (BRG-145). Where loss occurs and how it is observed:

| Location | Cause | Visible as |
|----------|-------|------------|
| Publisher → bridge | The bridge is a normal SUB: if it falls behind and the SUB's `ZMQ_RCVHWM` (`zmq_recv_hwm`) is exceeded, the *publisher* silently drops messages for that subscriber only. Other subscribers are unaffected. | Sequence numbers are assigned after reception, so this loss is *not* visible as a gap. It appears as a lower message count than the publisher's own count. Raise `zmq_recv_hwm` or reduce load. |
| Bridge send | UDP socket buffer full for longer than `send_timeout_ms`, or a network error. | Bridge `send_timeouts` / `send_errors`; receiver sees a sequence gap. |
| Network | Switch/NIC drops, Wi-Fi, IGMP snooping misconfiguration. | Receiver sequence gaps (`dropped_messages`) and, for fragmented messages, `incomplete_fragments`. |
| Receiver kernel | Socket receive buffer overflow while the receiver is busy (for example flushing to a slow disk). | `kernel_drops` (from `SO_RXQ_OVFL`) plus sequence gaps. Raise `receive_buffer_bytes` and `net.core.rmem_max`. |
| Receiver reassembly | Any single fragment of a fragmented message lost. | `incomplete_fragments`; the whole message is discarded. |

A single lost datagram loses one unfragmented message or one entire
fragmented message. The probability of losing a message therefore grows
linearly with its fragment count: at a datagram loss rate *p* a message of
*n* fragments survives with probability (1 − *p*)^*n*. Keep large messages
rare, or raise `max_datagram_size` on networks with jumbo frames.

Multicast delivery may also **reorder** datagrams. Fragments of one message
are reassembled correctly in any order. Whole messages arriving out of order
are recorded in arrival order and counted as `out_of_order`; consumers that
require strict order should sort the capture by `(source_id, sequence)`.

## Performance trade-offs

* **Single-threaded design**: both applications use one thread and
  `zmq_poll`. This avoids locking and satisfies BRG-016, and one core
  comfortably forwards >100k messages/s on loopback in the test
  environment. If a single core becomes the bottleneck, run several bridge
  instances with disjoint source sets and (optionally) different multicast
  groups.
* **Fairness vs. latency**: `max_messages_per_poll` bounds how many
  messages one source may send per poll round. Small values give the most
  even service under overload; larger values reduce per-message overhead.
* **Datagram size**: the default 1400 bytes avoids IP fragmentation on
  standard Ethernet. Larger values reduce header overhead and fragment
  count but require every link to carry the larger MTU; IP fragmentation
  (which the bridge otherwise avoids) makes loss far more likely.
* **Bounded blocking on send**: the bridge prefers to block briefly
  (`send_timeout_ms`) rather than drop when the UDP send buffer is full.
  During that time no source is serviced; ZeroMQ queues absorb the pause up
  to the receive high-water mark. Set `send_timeout_ms = 0` for fully
  blocking sends or a small value to prefer dropping.
* **Capture buffering**: records are written through a 1 MB user-space
  buffer and flushed every second by default; `capture_sync_on_flush = true`
  adds an `fsync` per flush at the cost of throughput on slow flash media.
  Writes happen on the receive thread. With a very slow storage device the
  receive buffer may overflow; `kernel_drops` will show it.
* **Receiver memory** is bounded by `reassembly_max_pending ×
  reassembly_max_message_bytes` in the worst case, plus the socket and file
  buffers. There is no per-source state limit other than the number of
  distinct source identifiers seen (BRG-114).

## Functional limitations

* **Multipart ZeroMQ messages** are concatenated into one payload; frame
  boundaries are not preserved (the `MULTIPART` flag marks such messages).
  Publishers that need frame boundaries preserved should encode them in the
  payload themselves.
* **Message size** is limited to 65535 fragments (≈88 MB at the default
  datagram size) by the 16-bit fragment fields, and by the receiver's
  `reassembly_max_message_bytes` (16 MB default).
* **Duplicate datagrams** (which real networks rarely produce) are ignored
  for fragments of a pending message but produce duplicate records for
  unfragmented messages; they are counted as `out_of_order`.
* **Sequence numbers restart** at 0 when the bridge restarts. The receiver
  resynchronises on a backwards jump larger than 1000 or to 0 and counts one
  `out_of_order` event instead of a giant gap.
* **No authentication or encryption** on the multicast stream or the TCP
  control/statistics channels. Deploy on trusted networks or restrict the
  command endpoint to localhost (the default).
* **One capture file per receiver run**; there is no automatic rotation.
  Stop and restart the receiver, or run several receivers, to start a new
  file.
