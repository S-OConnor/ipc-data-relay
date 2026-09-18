#!/usr/bin/env python3
"""Validate and inspect ipc-relay-receiver binary capture files.

This tool is intentionally independent of the C++ sources: it implements the
capture format from docs/capture_format.md using only the Python standard
library, which demonstrates that the format is parseable by a third party
(BRG-090, BRG-091, BRG-137).

Usage examples:
    capture_inspect.py capture.cap                 # summary
    capture_inspect.py --list capture.cap          # one line per record
    capture_inspect.py --json capture.cap          # machine-readable summary
    capture_inspect.py --verify-testpub capture.cap
    capture_inspect.py --expect-messages 3000 --expect-sources 3 capture.cap
    capture_inspect.py --self-test                 # exercises the parser

Exit status is 0 when the file is well-formed and all requested checks pass,
1 otherwise.
"""
import argparse
import io
import json
import os
import struct
import sys
import tempfile

FILE_MAGIC = 0x50434D5A      # "ZMCP" as bytes 5A 4D 43 50
RECORD_MAGIC = 0x52434D5A    # "ZMCR" as bytes 5A 4D 43 52
FORMAT_VERSION = 1
FILE_HEADER = struct.Struct("<IHHQHH12s")   # magic, version, header_length, created_ns, wire_version, reserved, reserved
RECORD_HEADER = struct.Struct("<IIQQIHH")    # magic, source_id, sequence, timestamp_ns, payload_length, flags, reserved
assert FILE_HEADER.size == 32
assert RECORD_HEADER.size == 32

FLAG_FRAGMENTED = 0x0001
FLAG_MULTIPART = 0x0002

TESTPUB_MAGIC = 0x54534554   # "TEST"
TESTPUB_HEADER = struct.Struct("<IIQI")


class CaptureError(Exception):
    pass


def read_file_header(f):
    raw = f.read(FILE_HEADER.size)
    if len(raw) < FILE_HEADER.size:
        raise CaptureError("file shorter than the 32-byte file header")
    magic, version, header_length, created_ns, wire_version, _r1, _r2 = FILE_HEADER.unpack(raw)
    if magic != FILE_MAGIC:
        raise CaptureError("bad file magic 0x%08X (expected 0x%08X)" % (magic, FILE_MAGIC))
    if version != FORMAT_VERSION:
        raise CaptureError("unsupported capture format version %d" % version)
    if header_length < FILE_HEADER.size:
        raise CaptureError("invalid header_length %d" % header_length)
    if header_length > FILE_HEADER.size:
        f.seek(header_length - FILE_HEADER.size, io.SEEK_CUR)
    return {"version": version, "header_length": header_length,
            "created_ns": created_ns, "wire_version": wire_version}


def iter_records(f):
    """Yields (offset, header_dict, payload_bytes). Raises CaptureError on corruption."""
    while True:
        offset = f.tell()
        raw = f.read(RECORD_HEADER.size)
        if not raw:
            return
        if len(raw) < RECORD_HEADER.size:
            raise CaptureError("truncated record header at offset %d" % offset)
        magic, source_id, sequence, timestamp_ns, payload_length, flags, _reserved = RECORD_HEADER.unpack(raw)
        if magic != RECORD_MAGIC:
            raise CaptureError("bad record magic 0x%08X at offset %d" % (magic, offset))
        payload = f.read(payload_length)
        if len(payload) < payload_length:
            raise CaptureError("truncated payload at offset %d (expected %d bytes, got %d)"
                               % (offset + RECORD_HEADER.size, payload_length, len(payload)))
        yield offset, {"source_id": source_id, "sequence": sequence, "timestamp_ns": timestamp_ns,
                       "payload_length": payload_length, "flags": flags}, payload


def verify_testpub_payload(payload, topic_prefix_len=None):
    """Checks a payload against the ipc-relay-testpub pattern. Returns (ok, detail)."""
    # The topic prefix length is unknown in general; locate the TEST magic.
    if topic_prefix_len is None:
        idx = payload.find(struct.pack("<I", TESTPUB_MAGIC))
        if idx < 0:
            return False, "testpub magic not found"
    else:
        idx = topic_prefix_len
    if len(payload) < idx + TESTPUB_HEADER.size:
        return False, "payload too short for testpub header"
    magic, pub_index, msg_index, total_len = TESTPUB_HEADER.unpack_from(payload, idx)
    if magic != TESTPUB_MAGIC:
        return False, "bad testpub magic"
    if total_len != len(payload):
        return False, "testpub length field %d != payload length %d" % (total_len, len(payload))
    body = payload[idx:]
    for i in range(TESTPUB_HEADER.size, len(body)):
        if body[i] != (msg_index + i) & 0xFF:
            return False, "filler mismatch at byte %d of message %d" % (i, msg_index)
    return True, (pub_index, msg_index)


def inspect(path, args):
    problems = []
    summary = {"file": path, "records": 0, "payload_bytes": 0, "sources": {}}
    with open(path, "rb") as f:
        summary["header"] = read_file_header(f)
        testpub_last = {}
        for offset, hdr, payload in iter_records(f):
            sid = hdr["source_id"]
            s = summary["sources"].setdefault(sid, {
                "messages": 0, "payload_bytes": 0, "first_sequence": hdr["sequence"],
                "last_sequence": hdr["sequence"], "sequence_gaps": 0, "missing": 0,
                "out_of_order": 0, "fragmented": 0, "multipart": 0, "min_payload": hdr["payload_length"],
                "max_payload": hdr["payload_length"], "first_timestamp_ns": hdr["timestamp_ns"],
                "last_timestamp_ns": hdr["timestamp_ns"], "testpub_errors": 0})
            if s["messages"] > 0:
                expected = s["last_sequence"] + 1
                if hdr["sequence"] > expected:
                    s["sequence_gaps"] += 1
                    s["missing"] += hdr["sequence"] - expected
                elif hdr["sequence"] < expected:
                    s["out_of_order"] += 1
            s["messages"] += 1
            s["payload_bytes"] += hdr["payload_length"]
            s["last_sequence"] = hdr["sequence"]
            s["last_timestamp_ns"] = hdr["timestamp_ns"]
            s["min_payload"] = min(s["min_payload"], hdr["payload_length"])
            s["max_payload"] = max(s["max_payload"], hdr["payload_length"])
            if hdr["flags"] & FLAG_FRAGMENTED:
                s["fragmented"] += 1
            if hdr["flags"] & FLAG_MULTIPART:
                s["multipart"] += 1
            summary["records"] += 1
            summary["payload_bytes"] += hdr["payload_length"]
            if args.list:
                print("%10d  src=%-6d seq=%-10d ts=%d len=%-7d flags=0x%04x  %s" % (
                    offset, sid, hdr["sequence"], hdr["timestamp_ns"], hdr["payload_length"], hdr["flags"],
                    payload[:16].hex()))
            if args.verify_testpub:
                ok, detail = verify_testpub_payload(payload)
                if not ok:
                    s["testpub_errors"] += 1
                    if s["testpub_errors"] <= 5:
                        problems.append("source %d seq %d: %s" % (sid, hdr["sequence"], detail))
                else:
                    pub_index, msg_index = detail
                    last = testpub_last.get(sid)
                    if last is not None and msg_index != last + 1 and msg_index > last:
                        # testpub message index gap should match the sequence gap
                        pass
                    testpub_last[sid] = msg_index

    for sid, s in summary["sources"].items():
        if s["sequence_gaps"] and args.strict:
            problems.append("source %d: %d sequence gap(s), %d message(s) missing" % (sid, s["sequence_gaps"], s["missing"]))
        if s["testpub_errors"]:
            problems.append("source %d: %d record(s) failed testpub verification" % (sid, s["testpub_errors"]))
    if args.expect_sources is not None and len(summary["sources"]) != args.expect_sources:
        problems.append("expected %d source(s), found %d" % (args.expect_sources, len(summary["sources"])))
    if args.expect_messages is not None and summary["records"] != args.expect_messages:
        problems.append("expected %d record(s), found %d" % (args.expect_messages, summary["records"]))
    if args.expect_per_source is not None:
        for sid, s in summary["sources"].items():
            if s["messages"] != args.expect_per_source:
                problems.append("source %d: expected %d record(s), found %d" % (sid, args.expect_per_source, s["messages"]))
    return summary, problems


def print_summary(summary):
    h = summary["header"]
    print("file:            %s" % summary["file"])
    print("format version:  %d (wire version %d)" % (h["version"], h["wire_version"]))
    print("created:         %d ns since epoch" % h["created_ns"])
    print("records:         %d" % summary["records"])
    print("payload bytes:   %d" % summary["payload_bytes"])
    print("sources:         %d" % len(summary["sources"]))
    for sid in sorted(summary["sources"]):
        s = summary["sources"][sid]
        span = (s["last_timestamp_ns"] - s["first_timestamp_ns"]) / 1e9
        print("  source %-6d messages=%-8d bytes=%-10d seq=%d..%d gaps=%d missing=%d ooo=%d "
              "fragmented=%d multipart=%d payload=%d..%d span=%.3fs" % (
                  sid, s["messages"], s["payload_bytes"], s["first_sequence"], s["last_sequence"],
                  s["sequence_gaps"], s["missing"], s["out_of_order"], s["fragmented"], s["multipart"],
                  s["min_payload"], s["max_payload"], span))


def self_test():
    """Builds a synthetic capture in memory and checks the parser on it."""
    def make_record(sid, seq, ts, flags, payload):
        return RECORD_HEADER.pack(RECORD_MAGIC, sid, seq, ts, len(payload), flags, 0) + payload

    data = FILE_HEADER.pack(FILE_MAGIC, FORMAT_VERSION, 32, 123456789, 1, 0, b"\0" * 12)
    data += make_record(1, 0, 10, 0, b"hello")
    data += make_record(2, 5, 11, FLAG_FRAGMENTED, bytes(range(200)) * 10)
    data += make_record(1, 1, 12, 0, b"")
    data += make_record(1, 3, 13, 0, b"gap")
    with tempfile.NamedTemporaryFile(delete=False, suffix=".cap") as tf:
        tf.write(data)
        path = tf.name
    try:
        ns = argparse.Namespace(list=False, verify_testpub=False, strict=False, expect_sources=2,
                                expect_messages=4, expect_per_source=None)
        summary, problems = inspect(path, ns)
        assert not problems, problems
        assert summary["records"] == 4
        assert summary["sources"][1]["sequence_gaps"] == 1 and summary["sources"][1]["missing"] == 1
        assert summary["sources"][2]["fragmented"] == 1
        assert summary["sources"][2]["payload_bytes"] == 2000
        # truncated payload must be detected
        with open(path, "wb") as f:
            f.write(data[:-2])
        try:
            inspect(path, ns)
            raise AssertionError("truncation not detected")
        except CaptureError as e:
            assert "truncated" in str(e), e
        # bad magic must be detected
        with open(path, "wb") as f:
            f.write(b"XXXX" + data[4:])
        try:
            inspect(path, ns)
            raise AssertionError("bad magic not detected")
        except CaptureError as e:
            assert "magic" in str(e), e
        # testpub pattern check
        idx = 7
        body = TESTPUB_HEADER.pack(TESTPUB_MAGIC, 0, idx, 40 + 3) + bytes(((idx + i) & 0xFF) for i in range(20, 40))
        ok, detail = verify_testpub_payload(b"TOP" + body)
        assert ok and detail == (0, idx), detail
        bad = bytearray(b"TOP" + body)
        bad[-1] ^= 0xFF
        ok, detail = verify_testpub_payload(bytes(bad))
        assert not ok, detail
    finally:
        os.unlink(path)
    print("capture_inspect self-test OK")
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("path", nargs="?", help="capture file")
    ap.add_argument("--list", action="store_true", help="print one line per record")
    ap.add_argument("--json", action="store_true", help="print the summary as JSON")
    ap.add_argument("--verify-testpub", action="store_true",
                    help="verify payloads were generated by ipc-relay-testpub")
    ap.add_argument("--strict", action="store_true", help="treat sequence gaps as failures")
    ap.add_argument("--expect-sources", type=int, help="fail unless exactly N sources are present")
    ap.add_argument("--expect-messages", type=int, help="fail unless exactly N records are present")
    ap.add_argument("--expect-per-source", type=int, help="fail unless every source has exactly N records")
    ap.add_argument("--self-test", action="store_true", help="run the built-in parser self-test")
    args = ap.parse_args()
    if args.self_test:
        return self_test()
    if not args.path:
        ap.error("path is required")
    try:
        summary, problems = inspect(args.path, args)
    except CaptureError as e:
        print("ERROR: %s: %s" % (args.path, e), file=sys.stderr)
        return 1
    except OSError as e:
        print("ERROR: %s" % e, file=sys.stderr)
        return 1
    if args.json:
        out = dict(summary)
        out["sources"] = {str(k): v for k, v in summary["sources"].items()}
        out["problems"] = problems
        print(json.dumps(out, indent=2))
    else:
        print_summary(summary)
        for p in problems:
            print("PROBLEM: %s" % p)
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
