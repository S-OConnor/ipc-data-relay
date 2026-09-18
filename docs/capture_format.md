# Binary Capture File Format (version 1)

`ipc-relay-receiver` records every successfully received or reassembled
message into **one** binary capture file that contains messages from all
sources (BRG-080, BRG-081). The format is **little-endian only** (BRG-083)
and is designed for deterministic offline parsing by C, C++ or Python tools
(BRG-090). `tools/capture_inspect.py` is a standard-library-only reference
parser.

```
+-------------------+
| File header (32B) |
+-------------------+
| Record header 32B |  record 0
| payload           |
+-------------------+
| Record header 32B |  record 1
| payload           |
+-------------------+
| ...               |
```

## File header (32 bytes)

| Offset | Size | Type  | Field           | Description |
|-------:|-----:|-------|-----------------|-------------|
| 0      | 4    | u32   | `magic`         | `0x50434D5A`; bytes `5A 4D 43 50` (ASCII `ZMCP`) (BRG-084). |
| 4      | 2    | u16   | `version`       | Capture format version, `1` (BRG-084). |
| 6      | 2    | u16   | `header_length` | Length of this header, `32`. Parsers must skip `header_length` bytes so future versions can extend the header. |
| 8      | 8    | u64   | `created_ns`    | `CLOCK_REALTIME` nanoseconds since the Unix epoch when the file was created. |
| 16     | 2    | u16   | `wire_version`  | UDP wire protocol version the receiver implemented (`1`). |
| 18     | 2    | u16   | `reserved`      | `0`. |
| 20     | 12   | u8[]  | `reserved`      | `0`. |

Python `struct` format: `"<IHHQHH12s"`.

## Record header (32 bytes) followed by the payload

| Offset | Size | Type | Field            | Description |
|-------:|-----:|------|------------------|-------------|
| 0      | 4    | u32  | `record_magic`   | `0x52434D5A`; bytes `5A 4D 43 52` (ASCII `ZMCR`). Allows validation and resynchronisation. |
| 4      | 4    | u32  | `source_id`      | Source identifier from the wire header (BRG-086). |
| 8      | 8    | u64  | `sequence`       | Per-source sequence number from the wire header (BRG-087). |
| 16     | 8    | u64  | `timestamp_ns`   | Bridge receive timestamp from the wire header, nanoseconds since epoch (BRG-088). |
| 24     | 4    | u32  | `payload_length` | Number of payload bytes that follow (BRG-089). |
| 28     | 2    | u16  | `flags`          | Wire header flags of the message (`0x0001` was fragmented, `0x0002` was a multipart ZeroMQ message). |
| 30     | 2    | u16  | `reserved`       | `0`. |
| 32     | n    | u8[] | `payload`        | The complete, reassembled, unmodified ZeroMQ message payload (BRG-085). |

Python `struct` format: `"<IIQQIHH"`.

Records appear in the order the receiver completed them. Because UDP may
reorder datagrams and fragmented messages complete when their last fragment
arrives, records of one source are *usually* but not necessarily in
sequence order; consumers that need strict ordering should sort by
`(source_id, sequence)`. Duplicate delivery of an unfragmented datagram
results in two records with the same `(source_id, sequence)`; the receiver
counts these as `out_of_order` and post-processing may de-duplicate on that
key.

## Integrity

* The file header is written when the file is opened; the receiver flushes
  its buffer every `capture_flush_interval_ms` and flushes and `fsync`s on
  shutdown or on the `flush` command (BRG-092).
* A file that ends mid-record (for example after a power loss) is detected by
  parsers as a truncated record; all preceding records remain valid.
* Any open, write, flush or close error is logged, counted in the
  `recording.write_errors` statistic and disables recording (BRG-093).

## Reading the file

```python
import struct
FILE = struct.Struct("<IHHQHH12s")
REC = struct.Struct("<IIQQIHH")
with open("capture.cap", "rb") as f:
    magic, version, hlen, created_ns, wire_version, _, _ = FILE.unpack(f.read(FILE.size))
    assert magic == 0x50434D5A and version == 1
    f.seek(hlen)
    while True:
        raw = f.read(REC.size)
        if not raw:
            break
        rmagic, source_id, sequence, ts_ns, length, flags, _ = REC.unpack(raw)
        assert rmagic == 0x52434D5A
        payload = f.read(length)
        ...
```

C/C++ readers should read the header fields byte-by-byte (or `memcpy` into
fixed-width integers on a little-endian host) rather than casting the file
bytes to a packed struct. `include/ipcrelay/capture_reader.hpp` is a C++
example.

## Validation utility

```
tools/capture_inspect.py capture.cap                     # per-source summary
tools/capture_inspect.py --list capture.cap              # one line per record
tools/capture_inspect.py --json capture.cap              # machine-readable
tools/capture_inspect.py --verify-testpub --strict capture.cap
```

The tool exits non-zero on any structural error, on any failed
`--expect-*` check, on testpub pattern mismatches with `--verify-testpub`,
and on sequence gaps with `--strict` (BRG-137).
