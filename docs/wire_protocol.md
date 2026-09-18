# UDP Wire Protocol (version 1)

Every UDP datagram sent by `ipc-relay-bridge` consists of a fixed 44-byte
transport header followed by a fragment of the original ZeroMQ message
payload. The protocol is **little-endian only**: every multi-byte field is
serialized least-significant byte first, and receivers must not apply any
byte-order conversion (BRG-040, BRG-041, BRG-042). Headers are serialized
field-by-field; no compiler-dependent C structures are placed on the wire
(BRG-053).

## Header layout

| Offset | Size | Type | Field             | Description |
|-------:|-----:|------|-------------------|-------------|
| 0      | 4    | u32  | `magic`           | `0x42554D5A`. On the wire the bytes are `5A 4D 55 42` (ASCII `ZMUB`). Receivers discard datagrams whose magic does not match (BRG-043). |
| 4      | 2    | u16  | `version`         | Protocol version, currently `1`. Receivers discard datagrams with an unknown version (BRG-044). |
| 6      | 2    | u16  | `flags`           | Bit field, see below. |
| 8      | 4    | u32  | `source_id`       | Identifier of the ZeroMQ source, as configured in the bridge's `[source] id` (BRG-045). Unique per configured endpoint. |
| 12     | 4    | u32  | `message_length`  | Total length in bytes of the original ZeroMQ message payload (BRG-050). |
| 16     | 8    | u64  | `sequence`        | Per-source sequence number, starting at 0 when the bridge starts and incremented by exactly one for every complete ZeroMQ message received from that source (BRG-046, BRG-047). All fragments of one message carry the same value (BRG-063). |
| 24     | 8    | u64  | `timestamp_ns`    | Nanoseconds since the Unix epoch (`CLOCK_REALTIME`) taken by the bridge when the message was received from ZeroMQ (BRG-048, BRG-049). Identical in all fragments of one message. |
| 32     | 4    | u32  | `fragment_offset` | Byte offset of this fragment's payload within the original message. `0` for unfragmented messages. |
| 36     | 2    | u16  | `fragment_index`  | 0-based index of this fragment (BRG-052). |
| 38     | 2    | u16  | `fragment_count`  | Total number of fragments for this message, at least `1` (BRG-052). |
| 40     | 2    | u16  | `fragment_length` | Number of payload bytes following the header in this datagram. |
| 42     | 2    | u16  | `reserved`        | Always `0`. Receivers ignore it. |
| 44     | n    | u8[] | `payload`         | `fragment_length` bytes of the original message, unmodified (BRG-035). |

Datagram length is always exactly `44 + fragment_length`.

### Flags

| Bit | Mask     | Name         | Meaning |
|----:|----------|--------------|---------|
| 0   | `0x0001` | `FRAGMENTED` | Set if and only if `fragment_count > 1`. |
| 1   | `0x0002` | `MULTIPART`  | The original ZeroMQ message consisted of more than one frame. The bridge concatenates all frames in order into one payload; frame boundaries are not transmitted (see [limitations](limitations.md)). |

All other bits are reserved and are `0`.

## Fragmentation

The bridge never relies on IP fragmentation (BRG-060). The maximum UDP
payload size (header + fragment) is set by the bridge option
`max_datagram_size`, default **1400 bytes**, which leaves room for IP/UDP
headers and VLAN tags inside a 1500-byte Ethernet MTU (BRG-061). The
fragment payload capacity is therefore `max_datagram_size - 44` (1356 bytes
by default).

A message of `message_length` bytes is split into
`ceil(message_length / capacity)` fragments (a zero-length message uses one
fragment with `fragment_length = 0`). Fragment `i` carries bytes
`[i * capacity, min((i + 1) * capacity, message_length))`, so
`fragment_offset = i * capacity`. Every fragment except the last carries
exactly `capacity` bytes. Fragments are sent in index order.

Because `fragment_count` and `fragment_index` are 16-bit, a message can span
at most 65535 fragments (about 88 MB at the default size). Larger messages
are dropped by the bridge and counted as `oversize`.

## Receiver validation rules

A receiver must reject a datagram (and count it as malformed, BRG-071,
BRG-113) when any of the following holds:

1. the datagram is shorter than 44 bytes (`truncated`);
2. `magic != 0x42554D5A` (`bad_magic`);
3. `version != 1` (`bad_version`);
4. `fragment_count == 0` (`bad_fragment_count`);
5. `fragment_index >= fragment_count` (`bad_fragment_index`);
6. datagram length `!= 44 + fragment_length` (`length_mismatch`);
7. `fragment_offset + fragment_length > message_length`, or for a
   single-fragment message `fragment_offset != 0` or
   `fragment_length != message_length`, or for the last fragment
   `fragment_offset + fragment_length != message_length`, or a non-final
   fragment with `fragment_length == 0` (`range_error`);
8. the `FRAGMENTED` flag disagrees with `fragment_count > 1`
   (`flag_mismatch`).

Fragments are associated with a message by the key
`(source_id, sequence)` (BRG-051). If a later fragment of the same key
carries a different `fragment_count` or `message_length` the message is
discarded (`fragment_inconsistent`). Fragments may arrive in any order and
duplicates are ignored. A message whose fragments have not all arrived
within the receiver's `reassembly_timeout_ms` is discarded and counted as
incomplete (BRG-065, BRG-066).

## Sequence gap detection

Receivers track the last `sequence` seen per `source_id`. Observing
`sequence > last + 1` indicates `sequence - last - 1` lost messages
(BRG-073). Sequence numbers restart at 0 when the bridge restarts; a
backwards jump of more than 1000 (or to 0) is treated as a restart.

## Example

A 5-byte message `hello` from source 7, sequence 42, timestamp
`0x18EE9E8B2D1C7C00`:

```
5A 4D 55 42   magic
01 00         version = 1
00 00         flags = 0
07 00 00 00   source_id = 7
05 00 00 00   message_length = 5
2A 00 00 00 00 00 00 00   sequence = 42
00 7C 1C 2D 8B 9E EE 18   timestamp_ns
00 00 00 00   fragment_offset = 0
00 00         fragment_index = 0
01 00         fragment_count = 1
05 00         fragment_length = 5
00 00         reserved
68 65 6C 6C 6F   "hello"
```

Python reference (`struct` format string): `"<IHHIIQQIHHHH"`, as used by
`tests/integration/test_receiver_faults.py`.
