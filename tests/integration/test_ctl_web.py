#!/usr/bin/env python3
"""ipc-relay-ctl web backend end-to-end test.

Runs a receiver and "ipc-relay-ctl serve", then acts as the browser frontend
with a small standard-library WebSocket client:
  - the embedded frontend and /api/stats are served over HTTP
  - hello + stats snapshots arrive over /ws at the configured --update-ms rate
  - counts track datagrams injected into the receiver's multicast group
  - {"type":"record","enabled":...} starts and stops recording on the receiver
  - bad requests, cross-origin upgrades and pings are handled
  - SIGTERM stops the backend cleanly
"""
import base64
import hashlib
import json
import os
import random
import shutil
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import time

WIRE = struct.Struct("<IHHIIQQIHHHH")
MAGIC = 0x42554D5A
UPDATE_MS = 250


def datagram(source, seq, payload):
    """Single-fragment datagram (docs/wire_protocol.md)."""
    return WIRE.pack(MAGIC, 1, 0, source, len(payload), seq, 1000 + seq, 0, 0, 1, len(payload), 0) + payload


class WebSocket:
    def __init__(self, port, origin=None, path="/ws"):
        self.sock = socket.create_connection(("127.0.0.1", port), timeout=5)
        key = base64.b64encode(os.urandom(16)).decode()
        lines = ["GET %s HTTP/1.1" % path, "Host: 127.0.0.1:%d" % port, "Upgrade: websocket",
                 "Connection: keep-alive, Upgrade", "Sec-WebSocket-Key: " + key, "Sec-WebSocket-Version: 13"]
        if origin:
            lines.append("Origin: " + origin)
        self.sock.sendall(("\r\n".join(lines) + "\r\n\r\n").encode())
        buf = b""
        while b"\r\n\r\n" not in buf:
            chunk = self.sock.recv(4096)
            if not chunk:
                break
            buf += chunk
        head, _, self.buf = buf.partition(b"\r\n\r\n")
        self.head = head.decode("latin-1")
        self.status = int(self.head.split()[1]) if self.head else 0
        expect = base64.b64encode(hashlib.sha1((key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11").encode()).digest())
        self.accept_ok = ("Sec-WebSocket-Accept: " + expect.decode()) in self.head

    def _read(self, n):
        while len(self.buf) < n:
            chunk = self.sock.recv(65536)
            if not chunk:
                raise EOFError("connection closed")
            self.buf += chunk
        out, self.buf = self.buf[:n], self.buf[n:]
        return out

    def recv_frame(self):
        b0, b1 = self._read(2)
        assert b1 & 0x80 == 0, "server frames must not be masked"
        n = b1 & 0x7F
        if n == 126:
            n = struct.unpack(">H", self._read(2))[0]
        elif n == 127:
            n = struct.unpack(">Q", self._read(8))[0]
        return b0 & 0x0F, self._read(n)

    def recv_json(self):
        while True:
            op, payload = self.recv_frame()
            if op == 0x1:
                return json.loads(payload.decode())
            if op == 0x8:
                raise EOFError("close frame received")

    def send_frame(self, opcode, payload):
        mask = os.urandom(4)
        n = len(payload)
        hdr = bytes([0x80 | opcode])
        if n < 126:
            hdr += bytes([0x80 | n])
        else:
            hdr += bytes([0x80 | 126]) + struct.pack(">H", n)
        self.sock.sendall(hdr + mask + bytes(b ^ mask[i % 4] for i, b in enumerate(payload)))

    def send_json(self, obj):
        self.send_frame(0x1, json.dumps(obj).encode())

    def close(self):
        self.sock.close()


def http_get(port, path, method="GET"):
    s = socket.create_connection(("127.0.0.1", port), timeout=5)
    s.sendall(("%s %s HTTP/1.1\r\nHost: 127.0.0.1:%d\r\n\r\n" % (method, path, port)).encode())
    data = b""
    while True:
        chunk = s.recv(65536)
        if not chunk:
            break
        data += chunk
    s.close()
    head, _, body = data.partition(b"\r\n\r\n")
    head = head.decode("latin-1")
    return int(head.split()[1]), head, body


def wait_for(path, text, timeout=10):
    deadline = time.time() + timeout
    while time.time() < deadline:
        with open(path, "rb") as f:
            if text.encode() in f.read():
                return True
        time.sleep(0.05)
    return False


def main():
    receiver, ctl = os.environ["RECEIVER"], os.environ["CTL"]
    work = tempfile.mkdtemp(prefix="ipcrelay-ctlweb.")
    group = "239.255.%d.%d" % (os.getpid() % 200 + 1, random.randint(1, 200))
    mport = random.randint(30000, 49999)
    stats_port = random.randint(20000, 28000)
    cmd_port, http_port = stats_port + 1, stats_port + 2
    cap = os.path.join(work, "capture.cap")
    rlog_path, clog_path = os.path.join(work, "receiver.log"), os.path.join(work, "ctl.log")
    rlog, clog = open(rlog_path, "wb"), open(clog_path, "wb")
    procs = []

    def check(cond, what):
        if not cond:
            for p in (rlog_path, clog_path):
                print("--- %s ---\n%s" % (os.path.basename(p), open(p, errors="replace").read()))
            raise SystemExit("CHECK FAILED: " + what)

    try:
        rproc = subprocess.Popen([
            receiver, "--set", "multicast_group=%s" % group, "--set", "multicast_port=%d" % mport,
            "--set", "multicast_interface=127.0.0.1", "--set", "capture_file=%s" % cap,
            "--set", "record_on_start=false", "--set", "stats_endpoint=tcp://127.0.0.1:%d" % stats_port,
            "--set", "stats_interval_ms=200", "--set", "command_endpoint=tcp://127.0.0.1:%d" % cmd_port,
            "--set", "command_bind=false"], stdout=rlog, stderr=subprocess.STDOUT)
        procs.append(rproc)
        check(wait_for(rlog_path, "receiver running"), "receiver started")

        cproc = subprocess.Popen([
            ctl, "serve", "--http", "127.0.0.1:%d" % http_port, "--update-ms", str(UPDATE_MS),
            "--stats-endpoint", "tcp://127.0.0.1:%d" % stats_port,
            "--command-endpoint", "tcp://127.0.0.1:%d" % cmd_port], stdout=clog, stderr=subprocess.STDOUT)
        procs.append(cproc)
        check(wait_for(clog_path, "control UI listening"), "ctl serve started")

        # --- HTTP: embedded frontend and JSON snapshot -----------------------
        st, head, body = http_get(http_port, "/")
        check(st == 200 and b"<title>IPC Relay Control</title>" in body, "index page served")
        check("text/html" in head, "index content type")
        st, head, body = http_get(http_port, "/app.js")
        check(st == 200 and "javascript" in head and b"WebSocket" in body, "app.js served")
        check(http_get(http_port, "/style.css")[0] == 200, "style.css served")
        check(http_get(http_port, "/../etc/passwd")[0] == 404, "unknown path is 404")
        check(http_get(http_port, "/", "POST")[0] == 405, "POST rejected")
        st, _, body = http_get(http_port, "/api/stats")
        check(st == 200 and json.loads(body)["type"] == "stats", "/api/stats returns a snapshot")

        # --- WebSocket: hello, then snapshots --------------------------------
        ws = WebSocket(http_port, origin="http://127.0.0.1:%d" % http_port)
        check(ws.status == 101 and ws.accept_ok, "WebSocket upgrade accepted (%s)" % ws.head)
        hello = ws.recv_json()
        check(hello["type"] == "hello" and hello["update_interval_ms"] == UPDATE_MS, "hello message: %s" % hello)

        results = []  # command_result messages, collected while waiting for snapshots

        def wait_stats(pred, what, timeout=5.0):
            deadline = time.time() + timeout
            env = None
            while time.time() < deadline:
                env = ws.recv_json()
                if env["type"] == "command_result":
                    results.append(env)
                elif env["stats"] is not None and pred(env):
                    return env
            check(False, "%s (last: %s)" % (what, json.dumps(env)[:2000]))

        wait_stats(lambda e: e["receiver_online"], "receiver reported online")

        # Continuous updates at the configured rate.
        t0, n = time.time(), 0
        while time.time() - t0 < 2.0:
            if ws.recv_json()["type"] == "stats":
                n += 1
        expected = 2000 // UPDATE_MS
        check(expected - 2 <= n <= expected + 3, "about %d updates in 2 s, got %d" % (expected, n))

        # --- Counts follow received traffic ----------------------------------
        tx = socket.socket(socket.AF_INET, socket.SOCK_DGRAM, socket.IPPROTO_UDP)
        tx.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_IF, socket.inet_aton("127.0.0.1"))
        tx.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_TTL, 0)
        tx.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_LOOP, 1)

        def send_messages(first, count):
            for seq in range(first, first + count):
                tx.sendto(datagram(5, seq, b"message %d" % seq), (group, mport))
                time.sleep(0.001)

        send_messages(0, 50)
        wait_stats(lambda e: e["stats"]["messages_received"] == 50 and e["stats"]["sources"].get("5", {}).get(
            "messages") == 50, "50 messages counted")

        # --- Start / stop recording from the "frontend" ----------------------
        ws.send_json({"type": "record", "enabled": True, "id": 1})
        wait_stats(lambda e: e["stats"]["recording"]["enabled"], "recording enabled via WebSocket")
        send_messages(50, 30)
        env = wait_stats(lambda e: e["stats"]["recording"]["records_written"] == 30, "30 records written")
        check(env["stats"]["recording"]["records_skipped"] == 50, "first 50 messages were not recorded")

        ws.send_json({"type": "record", "enabled": False, "id": "two"})
        wait_stats(lambda e: not e["stats"]["recording"]["enabled"], "recording disabled via WebSocket")
        check(wait_for(clog_path, "'record on' sent") and wait_for(clog_path, "'record off' sent"), "commands logged")

        # command_result messages for both requests plus an invalid one.
        ws.send_json({"type": "reboot"})
        deadline = time.time() + 5
        while len(results) < 3 and time.time() < deadline:
            m = ws.recv_json()
            if m["type"] == "command_result":
                results.append(m)
        check(len(results) == 3, "three command results, got %s" % results)
        check(results[0]["ok"] and results[0]["id"] == 1 and results[0]["command"] == "record on", str(results[0]))
        check(results[1]["ok"] and results[1]["id"] == "two" and results[1]["command"] == "record off", str(results[1]))
        check(not results[2]["ok"] and "unknown message type" in results[2]["error"], str(results[2]))

        # Ping is answered with a pong carrying the same payload.
        ws.send_frame(0x9, b"hi")
        while True:
            op, payload = ws.recv_frame()
            if op == 0xA:
                check(payload == b"hi", "pong payload")
                break
        ws.send_frame(0x8, struct.pack(">H", 1000))
        ws.close()

        # Invalid JSON closes nothing but is reported.
        ws2 = WebSocket(http_port)
        check(ws2.status == 101, "upgrade without Origin (non-browser client) accepted")
        ws2.send_frame(0x1, b"{not json")
        while True:
            m = ws2.recv_json()
            if m["type"] == "command_result":
                check(not m["ok"] and "invalid JSON" in m["error"], str(m))
                break
        ws2.close()

        bad = WebSocket(http_port, origin="http://evil.example")
        check(bad.status == 403, "cross-origin upgrade rejected, got %d" % bad.status)
        bad.close()

        # --- Shutdown --------------------------------------------------------
        cproc.send_signal(signal.SIGTERM)
        check(cproc.wait(timeout=5) == 0, "ctl serve exits 0 on SIGTERM")
        rproc.send_signal(signal.SIGTERM)
        check(rproc.wait(timeout=10) == 0, "receiver exits 0")
        print("ctl web backend OK: %d updates in 2 s at %d ms" % (n, UPDATE_MS))
        return 0
    finally:
        for p in procs:
            if p.poll() is None:
                p.kill()
        rlog.close()
        clog.close()
        if os.environ.get("KEEP_WORK") != "1":
            shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
