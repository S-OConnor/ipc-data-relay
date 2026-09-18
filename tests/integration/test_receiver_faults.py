#!/usr/bin/env python3
"""Receiver fault-injection test (BRG-134, BRG-136, BRG-113, BRG-065/066).

Builds raw UDP datagrams with an independent Python implementation of the
wire protocol (docs/wire_protocol.md) and checks the receiver's statistics
and capture file for: sequence gaps, out-of-order fragments, missing
fragments (reassembly timeout), duplicate fragments, inconsistent fragments
and several malformed packets.
"""
import json
import os
import random
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import time

WIRE = struct.Struct("<IHHIIQQIHHHH")
assert WIRE.size == 44
MAGIC = 0x42554D5A
VERSION = 1
FLAG_FRAGMENTED = 1

TIMEOUT_MS = 200


def datagram(source, seq, payload, chunk=1356, ts=1234567890123456789, mutate=None):
    """Returns the list of datagrams for one message, like the bridge builds them."""
    count = max(1, (len(payload) + chunk - 1) // chunk)
    out = []
    for i in range(count):
        part = payload[i * chunk:(i + 1) * chunk]
        flags = FLAG_FRAGMENTED if count > 1 else 0
        fields = dict(magic=MAGIC, version=VERSION, flags=flags, source=source, mlen=len(payload), seq=seq,
                      ts=ts, off=i * chunk, idx=i, count=count, flen=len(part), reserved=0)
        if mutate:
            mutate(i, fields)
        hdr = WIRE.pack(fields["magic"], fields["version"], fields["flags"], fields["source"], fields["mlen"],
                        fields["seq"], fields["ts"], fields["off"], fields["idx"], fields["count"],
                        fields["flen"], fields["reserved"])
        out.append(hdr + part)
    return out


def main():
    receiver = os.environ["RECEIVER"]
    inspect = os.environ["INSPECT"]
    work = tempfile.mkdtemp(prefix="ipcrelay-faults.")
    group = "239.255.%d.%d" % (os.getpid() % 200 + 1, random.randint(1, 200))
    port = random.randint(30000, 49999)
    cap = os.path.join(work, "capture.cap")
    log = open(os.path.join(work, "receiver.log"), "wb")
    proc = subprocess.Popen([
        receiver, "--set", "multicast_group=%s" % group, "--set", "multicast_port=%d" % port,
        "--set", "multicast_interface=127.0.0.1", "--set", "capture_file=%s" % cap,
        "--set", "stats_endpoint=", "--set", "command_endpoint=",
        "--set", "reassembly_timeout_ms=%d" % TIMEOUT_MS, "--set", "log_level=debug"],
        stdout=log, stderr=subprocess.STDOUT)
    try:
        deadline = time.time() + 10
        while time.time() < deadline:
            with open(log.name, "rb") as f:
                if b"receiver running" in f.read():
                    break
            time.sleep(0.05)
        else:
            raise SystemExit("receiver did not start:\n" + open(log.name).read())

        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM, socket.IPPROTO_UDP)
        s.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_IF, socket.inet_aton("127.0.0.1"))
        s.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_TTL, 0)
        s.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_LOOP, 1)
        dst = (group, port)

        def send(dgrams):
            for d in dgrams:
                s.sendto(d, dst)
                time.sleep(0.002)

        big = bytes(random.getrandbits(8) for _ in range(4000))  # 3 fragments

        # Source 1: in order
        for seq in (0, 1, 2):
            send(datagram(1, seq, b"msg%d" % seq))
        # gap: 3 and 4 missing
        send(datagram(1, 5, b"after gap"))
        # fragmented, delivered out of order
        frags = datagram(1, 6, big)
        assert len(frags) == 3
        send([frags[2], frags[0], frags[1]])
        # fragmented with the middle fragment missing -> incomplete after timeout
        frags8 = datagram(1, 7, big)
        send([frags8[0], frags8[2]])
        # fragmented with a duplicated fragment
        frags9 = datagram(1, 8, big)
        send([frags9[0], frags9[0], frags9[1], frags9[2]])
        # inconsistent fragment_count between fragments of the same message
        frags10 = datagram(1, 9, big)
        bad10 = datagram(1, 9, big, mutate=lambda i, f: f.update(count=5, mlen=len(big)))
        send([frags10[0], bad10[1]])
        # Source 2: independent sequence space, no gap expected
        send(datagram(2, 100, b"two-a"))
        send(datagram(2, 101, b"two-b"))
        # Malformed packets
        good = datagram(1, 10, b"good")[0]
        send([b"\x00" * 44])                                 # bad magic
        send([good[:20]])                                    # truncated
        bad_version = datagram(1, 10, b"good", mutate=lambda i, f: f.update(version=7))[0]
        send([bad_version])
        send([good + b"extra"])                              # length mismatch
        bad_index = datagram(1, 10, b"good", mutate=lambda i, f: f.update(idx=3))[0]
        send([bad_index])                                    # fragment_index >= count
        bad_range = datagram(1, 10, b"good", mutate=lambda i, f: f.update(mlen=99))[0]
        send([bad_range])                                    # single fragment not covering message
        # Late, in-order message after the faults
        send(datagram(1, 10, b"final"))

        time.sleep((TIMEOUT_MS * 3) / 1000.0)
        proc.send_signal(signal.SIGTERM)
        rc = proc.wait(timeout=10)
        log.close()
        text = open(log.name).read()
        if rc != 0:
            raise SystemExit("receiver exit code %d\n%s" % (rc, text))
        line = [l for l in text.splitlines() if "final stats: " in l][-1]
        st = json.loads(line.split("final stats: ", 1)[1])

        def check(cond, what):
            if not cond:
                print(json.dumps(st, indent=2))
                print(text)
                raise SystemExit("CHECK FAILED: " + what)

        s1 = st["sources"]["1"]
        s2 = st["sources"]["2"]
        # seq 0,1,2,5,6,8,10 complete for source 1
        check(s1["messages"] == 7, "source 1 messages == 7, got %d" % s1["messages"])
        check(s1["sequence_gaps"] == 1, "source 1 sequence gap events == 1")
        check(s1["dropped_messages"] == 2, "source 1 dropped == 2 (seq 3,4)")
        check(s1["incomplete_fragments"] == 1, "source 1 incomplete == 1 (seq 7)")
        check(s1["duplicate_fragments"] == 1, "source 1 duplicates == 1 (seq 8)")
        check(s1["malformed_fragments"] >= 1, "source 1 inconsistent fragment counted")
        check(s2["messages"] == 2 and s2["sequence_gaps"] == 0, "source 2 independent, no gaps")
        check(st["messages_received"] == 9, "total messages == 9, got %d" % st["messages_received"])
        check(st["incomplete_fragments"] == 1, "global incomplete == 1")
        reasons = st["malformed_by_reason"]
        for r in ("bad_magic", "truncated", "bad_version", "length_mismatch", "bad_fragment_index", "range_error",
                  "fragment_inconsistent"):
            check(reasons.get(r, 0) >= 1, "malformed reason %s counted (%s)" % (r, reasons))
        check(st["malformed_packets"] == sum(reasons.values()), "malformed total consistent")
        check(st["recording"]["records_written"] == 9, "9 records written")
        check(st["recording"]["write_errors"] == 0, "no write errors")

        # Verify the capture: reassembled payload for seq 7 and 9 must equal the original.
        summary = json.loads(subprocess.check_output([sys.executable, inspect, "--json", cap]).decode())
        check(summary["records"] == 9, "capture has 9 records")
        check(summary["sources"]["1"]["sequence_gaps"] >= 1, "capture inspector sees the gap")
        with open(cap, "rb") as f:
            f.seek(32)
            seen = {}
            while True:
                h = f.read(32)
                if not h:
                    break
                _m, sid, seq, ts, plen, flags, _r = struct.unpack("<IIQQIHH", h)
                seen[(sid, seq)] = (f.read(plen), ts, flags)
        check(seen[(1, 6)][0] == big and seen[(1, 6)][2] == FLAG_FRAGMENTED, "seq 6 reassembled correctly")
        check(seen[(1, 8)][0] == big, "seq 8 reassembled correctly despite duplicate")
        check(seen[(1, 5)][0] == b"after gap" and seen[(1, 5)][1] == 1234567890123456789, "timestamp preserved")
        check((1, 7) not in seen and (1, 9) not in seen, "incomplete/inconsistent messages not recorded")
        check(seen[(2, 101)][0] == b"two-b", "source 2 recorded")
        print("receiver fault injection OK")
        return 0
    finally:
        if proc.poll() is None:
            proc.kill()
        if os.environ.get("KEEP_WORK") != "1":
            import shutil
            shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
