# ipc-data-relay

A ZeroMQ IPC-to-UDP-multicast bridge for Linux, with a companion multicast
receiver/capture tool and supporting test utilities. Written in C++17 with
CMake and no dependencies beyond POSIX and libzmq.

```
ZeroMQ PUB (ipc://) x N  -->  ipc-relay-bridge  ==UDP multicast==>  ipc-relay-receiver  -->  capture.cap
                                                                        |  stats (ZMQ TCP PUB)
                                                                        |  commands (ZMQ TCP SUB)
```

The main application is **`ipc-relay-bridge`** (in [src/](src/)). Everything
under [tools/](tools/) exists to feed it, receive its output, control the
receiver or validate what was recorded:

| Component | Location | Purpose |
|-----------|----------|---------|
| [`ipc-relay-bridge`](#ipc-relay-bridge-main-application) | [src/bridge/](src/bridge/) | Subscribes to IPC publishers and republishes over UDP multicast |
| [`ipc-relay-receiver`](#ipc-relay-receiver) | [tools/receiver/](tools/receiver/) | Joins the multicast group, reassembles, monitors and records to a capture file |
| [`ipc-relay-testpub`](#ipc-relay-testpub) | [tools/testpub/](tools/testpub/) | Test publisher that generates deterministic, verifiable messages |
| [`ipc-relay-ctl`](#ipc-relay-ctl) | [tools/ctl/](tools/ctl/) | Web control page for the receiver (live counts, start/stop recording), plus one-shot command/monitor modes |
| [`capture_inspect.py`](#capture_inspectpy) | [tools/capture_inspect.py](tools/capture_inspect.py) | Standalone capture-file validator (Python standard library only) |
| [`ipc-relay-capture-to-csv`](#ipc-relay-capture-to-csv) | [tools/capture_to_csv/](tools/capture_to_csv/) | Decodes a capture file into one CSV per message type |

Documentation:

| Document | Content |
|----------|---------|
| [docs/architecture.md](docs/architecture.md) | Components, data flow, main loops, statistics message, runtime commands |
| [docs/wire_protocol.md](docs/wire_protocol.md) | UDP transport header, field by field |
| [docs/capture_format.md](docs/capture_format.md) | Binary capture file format, field by field, with parsing examples |
| [docs/limitations.md](docs/limitations.md) | Assumptions, limitations, packet-loss behaviour, performance trade-offs |
| [docs/yocto.md](docs/yocto.md) | Yocto recipe, systemd units, embedded deployment guidance |
| [docs/requirements_traceability.md](docs/requirements_traceability.md) | Requirement → implementation → test matrix |
| [docs/zmq_ipc_udp_multicast_shall_requirements.csv](docs/zmq_ipc_udp_multicast_shall_requirements.csv) | The requirements this project implements |

## Building

Requirements: Linux, a C++17 compiler (GCC ≥ 8 or Clang ≥ 7), CMake ≥ 3.16,
libzmq ≥ 4.1 with headers, and Python 3 for the tools and tests.

```sh
# Debian/Ubuntu
sudo apt install build-essential cmake pkg-config libzmq3-dev python3
# Fedora
sudo dnf install gcc-c++ cmake pkgconf zeromq-devel python3
# Homebrew (Linux or macOS-style prefix); FindZeroMQ.cmake looks in the brew prefix automatically
brew install zeromq

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
sudo cmake --install build            # optional: installs to /usr/local
```

All executables are written to the top of the build directory
(`build/ipc-relay-bridge`, `build/ipc-relay-receiver`, ...).

CMake options:

| Option | Default | Meaning |
|--------|---------|---------|
| `IPCRELAY_BUILD_TESTS` | `ON` | Build unit tests and register the integration tests with CTest |
| `IPCRELAY_BUILD_TOOLS` | `ON` | Build `ipc-relay-testpub` and `ipc-relay-ctl` |
| `IPCRELAY_WARNINGS_AS_ERRORS` | `OFF` | `-Werror` |

`ipc-relay-bridge` and `ipc-relay-receiver` are always built. If libzmq is
installed in a non-standard prefix, pass `-DCMAKE_PREFIX_PATH=/prefix` or set
`PKG_CONFIG_PATH`.

## ipc-relay-bridge (main application)

The bridge passively subscribes to any number of ZeroMQ IPC publishers (one
`ZMQ_SUB` per source, all polled from a single thread) and republishes every
message over one UDP multicast stream. Each datagram carries a 44-byte
little-endian header with source id, per-source sequence number, nanosecond
timestamp and fragmentation metadata ([docs/wire_protocol.md](docs/wire_protocol.md)).
Messages larger than one datagram are fragmented at the application level;
IP fragmentation is never relied on. The bridge only subscribes, so other
subscribers on the same IPC endpoints still receive every message.

### Usage

```
ipc-relay-bridge [options]

  -c, --config FILE        Configuration file (see examples/bridge.conf)
  -s, --source ID=ENDPOINT[,FILTER]
                           Add a ZeroMQ source (repeatable). Without FILTER all
                           messages are forwarded.
  -o, --set KEY=VALUE      Override a global option, e.g. multicast_port=6000.
                           source.KEY=VALUE applies to the last [source].
  -l, --log-level LEVEL    error | warn | info | debug
      --check              Validate the configuration and exit
  -V, --version            Print version and exit
  -h, --help               Show this help
```

`multicast_group`, `multicast_port` and at least one source are required.
They can come from a file, the command line, or both:

```sh
# From a configuration file
ipc-relay-bridge --config examples/bridge.conf

# Entirely from the command line
ipc-relay-bridge --set multicast_group=239.192.10.1 --set multicast_port=5100 \
    --source 1=ipc:///tmp/nav.sock --source 2=ipc:///tmp/log.sock,ERR

# Validate a configuration without starting (reports every problem, exit status 2 on error)
ipc-relay-bridge --config examples/bridge.conf --check
```

### Configuration

The configuration file is INI-style (`key = value`, `#` comments). Examples:

* [examples/bridge.conf](examples/bridge.conf): two IPC sources, one with a
  subscription filter.
* [examples/bridge-three-publishers.conf](examples/bridge-three-publishers.conf):
  three IPC publishers feeding one multicast stream.

Global options: `multicast_group`, `multicast_port`, `multicast_interface`,
`multicast_ttl`, `multicast_loopback`, `max_datagram_size`,
`send_buffer_bytes`, `send_timeout_ms`, `zmq_recv_hwm`,
`max_messages_per_poll`, `reconnect_ivl_ms`, `stats_interval_ms`,
`log_level`.

Each `[source]` block takes `id`, `endpoint`, optional `name`, `recv_hwm`
and any number of `filter` lines (none, or an empty one, subscribes to all
messages).

### Logging and shutdown

Logging goes to stderr at the configured level; `debug` prints one line per
message and should be off in production. The bridge logs its counters every
`stats_interval_ms` (default 5000, `0` disables). On `SIGINT`/`SIGTERM` it
stops within one poll timeout and logs a final statistics summary.

## Tools

### ipc-relay-receiver

[tools/receiver/](tools/receiver/). Joins the multicast group, validates and
reassembles datagrams, detects per-source sequence gaps and keeps detailed
statistics. It publishes the statistics as JSON over ZeroMQ TCP, accepts
runtime commands over ZeroMQ TCP, and writes every message to a single binary
capture file ([docs/capture_format.md](docs/capture_format.md)).

```
ipc-relay-receiver [options]

  -c, --config FILE        Configuration file (see examples/receiver.conf)
  -o, --set KEY=VALUE      Override an option, e.g. capture_file=/tmp/out.cap
  -l, --log-level LEVEL    error | warn | info | debug
      --check              Validate the configuration and exit
  -V, --version            Print version and exit
  -h, --help               Show this help
```

`multicast_group`, `multicast_port` and `capture_file` are required:

```sh
ipc-relay-receiver --config examples/receiver.conf
ipc-relay-receiver --set multicast_group=239.192.10.1 --set multicast_port=5100 \
    --set multicast_interface=127.0.0.1 --set capture_file=/tmp/demo.cap
```

Options (see [examples/receiver.conf](examples/receiver.conf) for comments
and defaults): `multicast_group`, `multicast_port`, `multicast_interface`,
`receive_buffer_bytes`, `max_datagrams_per_poll`, `capture_file`,
`record_on_start`, `capture_buffer_bytes`, `capture_flush_interval_ms`,
`capture_sync_on_flush`, `reassembly_timeout_ms`, `reassembly_max_pending`,
`reassembly_max_message_bytes`, `stats_endpoint`, `stats_topic`,
`stats_interval_ms`, `stats_print`, `command_endpoint`, `command_bind`,
`command_topic`, `log_level`.

By default statistics are published on `tcp://*:5556` (topic `stats`, once
per second) and the receiver connects to a command publisher at
`tcp://127.0.0.1:5557`. Set either endpoint to `""` to disable it. Setting
`stats_print = true` also logs each statistics message. On
`SIGINT`/`SIGTERM` the receiver flushes, `fsync`s and closes the capture
file and logs final statistics.

### ipc-relay-testpub

[tools/testpub/](tools/testpub/). A ZeroMQ PUB publisher that stands in for
real applications. It binds one or more endpoints and sends simulated
telemetry whose framing `capture_inspect.py --verify-testpub` can check end to
end. Each endpoint carries one data type, by its position on the command line
(the 4th endpoint starts again at board health):

| Endpoint | Data type | Structure | Contents |
|----------|-----------|-----------|----------|
| 1st | 1 | `BoardHealth` (80 bytes) | Voltage and current of six power rails, four temperatures, alarm bits |
| 2nd | 2 | `ModeStatus` (32 bytes) | System mode (boot/standby/operational/maintenance/fault), status flags, fault code, uptime, heartbeat |
| 3rd | 3 | `PtpStats` (56 bytes) | Port and servo state, offset from master, mean path delay, frequency adjustment, grandmaster identity and class |

Each payload holds a magic number, the publisher index, a per-endpoint
message index, the length, the data type and length, the data structure and
a filler pattern up to the chosen size (see the header comment in
[tools/testpub/main.cpp](tools/testpub/main.cpp)). The structures are plain C
records in [tools/testpub/telemetry.hpp](tools/testpub/telemetry.hpp),
encoded little-endian without padding.

```
ipc-relay-testpub --endpoint EP [--endpoint EP ...] [options]

  -e, --endpoint EP      ZeroMQ PUB bind endpoint, e.g. ipc:///tmp/src1.sock (repeatable)
  -n, --count N          Messages per endpoint (0 = until SIGINT/SIGTERM)   [1000]
  -r, --rate HZ          Messages per second per endpoint (0 = unthrottled) [1000]
  -s, --size BYTES       Minimum payload size (excluding topic); raised to
                         fit the header and the channel's data structure  [64]
  -S, --size-max BYTES   Maximum payload size; random in [size, size-max]   [=size]
  -t, --topic PREFIX     Topic prefix prepended to every message            [""]
  -m, --multipart        Send the topic as a separate first frame
      --settle-ms MS     Delay after bind before publishing (slow joiner)   [500]
      --linger-ms MS     Delay after the last message before exiting        [500]
      --seed N           Random seed for sizes                              [1]
  -q, --quiet            Do not print progress
```

```sh
# 5000 messages of 100..3000 bytes on each of two endpoints, 500 msg/s each
ipc-relay-testpub -e ipc:///tmp/nav.sock -e ipc:///tmp/imu.sock \
    --count 5000 --rate 500 --size 100 --size-max 3000
```

`--size-max` above the bridge's `max_datagram_size` exercises fragmentation.
Use `--topic` to match a subscription filter on the bridge.

### ipc-relay-ctl

[tools/ctl/](tools/ctl/). Controls a running receiver and shows its
statistics. It has a backend ([tools/ctl/backend/](tools/ctl/backend/), C++)
and a browser frontend ([tools/ctl/frontend/](tools/ctl/frontend/), plain
HTML/JS/CSS compiled into the binary). They exchange JSON over a WebSocket.

```
ipc-relay-ctl [serve] [--http HOST:PORT] [--update-ms MS] [--stale-ms MS]
                      [--stats-endpoint EP] [--stats-topic T] [--command-endpoint EP] [--connect]
                      [--command-topic T] [--web-root DIR] [--log-level LEVEL]
ipc-relay-ctl send    [--endpoint tcp://127.0.0.1:5557] [--connect] [--topic T] [--settle-ms MS] COMMAND...
ipc-relay-ctl monitor [--endpoint tcp://127.0.0.1:5556] [--topic stats] [--count N]
```

* `serve` (the default mode) runs until SIGINT/SIGTERM. Open
  `http://HOST:PORT/` (default `127.0.0.1:8083`; use `--http 0.0.0.0:8083`
  for remote browsers). The page has a Start/Stop recording button at the
  top and lists the receiver's counters and per-source counts below it. The
  backend pushes the latest receiver statistics to every open page every
  `--update-ms` milliseconds (default 1000, 50..60000; fixed at startup).
  The receiver shows as offline when no statistics arrived for `--stale-ms`
  (default 3000). The backend keeps one command PUB socket open: it binds
  `--command-endpoint` (default `tcp://127.0.0.1:5557`, the receiver's
  default), or connects to it with `--connect` when the receiver has
  `command_bind = true`. `GET /api/stats` returns the same JSON snapshot as
  the page gets. `--web-root tools/ctl/frontend` serves the frontend files
  from disk, so frontend edits need no rebuild. The page has no
  authentication, so only expose it on trusted networks. The message
  protocol is in [docs/architecture.md](docs/architecture.md#control-tool-web-interface).
* `send` publishes a command to the receiver. Commands: `record on`,
  `record off`, `record toggle`, `flush`, `stats`. By default `send` binds
  the endpoint and the receiver connects to it. If the receiver has
  `command_bind = true`, add `--connect`.
* `monitor` connects to the receiver's statistics endpoint and prints each
  JSON statistics message on its own line until Ctrl-C (or `--count N`
  messages).

```sh
ipc-relay-ctl --update-ms 500                  # web page on http://127.0.0.1:8083/
ipc-relay-ctl send record off
ipc-relay-ctl send record on
ipc-relay-ctl monitor --endpoint tcp://127.0.0.1:5556 --count 5
```

### capture_inspect.py

[tools/capture_inspect.py](tools/capture_inspect.py) (installed as
`ipc-relay-capture-inspect`). Validates and summarises a capture file. It
uses only the Python standard library and does not share code with the C++
sources, which shows the capture format can be parsed by a third party.

```sh
python3 tools/capture_inspect.py capture.cap                   # per-source summary
python3 tools/capture_inspect.py --list capture.cap            # one line per record
python3 tools/capture_inspect.py --json capture.cap            # machine-readable summary
python3 tools/capture_inspect.py --verify-testpub --strict capture.cap
python3 tools/capture_inspect.py --expect-sources 3 --expect-messages 3000 capture.cap
python3 tools/capture_inspect.py --self-test                   # exercises the parser
```

| Option | Check |
|--------|-------|
| `--verify-testpub` | Payloads match the `ipc-relay-testpub` pattern |
| `--strict` | Sequence gaps count as failures |
| `--expect-sources N` | Exactly N sources present |
| `--expect-messages N` | Exactly N records present |
| `--expect-per-source N` | Every source has exactly N records |

Exit status is 0 when the file is well formed and all requested checks pass,
1 otherwise.

### ipc-relay-capture-to-csv

[tools/capture_to_csv/](tools/capture_to_csv/). Decodes the
`ipc-relay-testpub` telemetry in a capture file and writes one CSV per
message type. It reads the capture with the receiver's `CaptureReader` and
decodes payloads with the structures in
[tools/testpub/telemetry.hpp](tools/testpub/telemetry.hpp).

```sh
ipc-relay-capture-to-csv capture.cap           # writes capture_csv/*.csv
ipc-relay-capture-to-csv -o out/ capture.cap   # writes out/*.csv
```

| File | Contents |
|------|----------|
| `board_health.csv` | Rail voltages and currents, temperatures, alarm flags |
| `mode_status.csv` | System mode, previous mode, status flags, fault code |
| `ptp_stats.csv` | PTP offset, path delay, frequency adjustment, port/servo state |
| `unknown.csv` | Payloads that are not testpub telemetry, as hex |

A file is only written when the capture contains that type. Every row starts
with `source_id`, `sequence`, `timestamp_ns`, `flags`, `publisher_index` and
`message_index`. Enumerations are written as names (`operational`, `slave`)
and bit fields as the integer plus a `|`-separated list of set flags. Rows
are in capture order. The exit status is 1 if the capture is malformed or
truncated; the CSVs then contain every record before the error.

## Running everything together

A complete single-host pipeline on loopback, using
[examples/bridge-three-publishers.conf](examples/bridge-three-publishers.conf)
(group `239.192.10.1:5100` on `127.0.0.1`, sources on `/tmp/nav.sock`,
`/tmp/imu.sock` and `/tmp/log.sock`). Run each step in its own terminal from
the repository root after building:

```sh
# 1. Receiver: join 239.192.10.1:5100 on loopback and record to /tmp/demo.cap
./build/ipc-relay-receiver --set multicast_group=239.192.10.1 --set multicast_port=5100 \
    --set multicast_interface=127.0.0.1 --set capture_file=/tmp/demo.cap

# 2. Bridge: subscribe to the three IPC sources and send to the same group
./build/ipc-relay-bridge --config examples/bridge-three-publishers.conf

# 3. Publishers: the test publisher stands in for the real applications
./build/ipc-relay-testpub -e ipc:///tmp/nav.sock -e ipc:///tmp/imu.sock -e ipc:///tmp/log.sock \
    --count 0 --rate 500 --size 100 --size-max 3000

# 4. Watch live statistics (one JSON line per second) and pause/resume recording
./build/ipc-relay-ctl monitor --endpoint tcp://127.0.0.1:5556
./build/ipc-relay-ctl send record off
./build/ipc-relay-ctl send record on

# 5. Stop the publisher, bridge and receiver with Ctrl-C, then inspect the capture
python3 tools/capture_inspect.py /tmp/demo.cap
python3 tools/capture_inspect.py --verify-testpub --strict /tmp/demo.cap
```

Things to expect:

* The order of steps 1–3 does not matter. ZeroMQ reconnects automatically,
  but messages published before the bridge has connected are not captured.
* `examples/bridge-three-publishers.conf` only forwards `ERR`- and
  `WARN`-prefixed messages from the third source (`/tmp/log.sock`). Without
  `--topic ERR` on the test publisher, that source is filtered out and the
  capture contains two sources.
* Pausing recording with `record off` leaves a gap in the capture, so
  `--strict` reports sequence gaps for that run. Leave out `--strict`, or
  skip the `record off`/`record on` step, to get a clean verification.
* To use a real network instead of loopback, set `multicast_interface` on
  both sides to the interface name or address and raise `multicast_ttl` on
  the bridge if the receiver is more than one hop away. See
  [docs/limitations.md](docs/limitations.md) and
  [docs/yocto.md](docs/yocto.md) for buffer sizing and kernel settings.

## Testing

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DIPCRELAY_WARNINGS_AS_ERRORS=ON
cmake --build build -j"$(nproc)"
ctest --test-dir build --output-on-failure
```

| Test | What it covers |
|------|----------------|
| `unit_tests` | Little-endian serialization, wire header layout and every malformed-packet rule, fragmentation math, reassembly (in-order, out-of-order, duplicates, timeouts, bounded memory, inconsistent fragments), sequence-gap detection, capture writer/reader, configuration parsing and validation, JSON writer, and an in-process ZeroMQ test showing two subscribers each receive every message |
| `e2e_multi_source_pipeline` | `ipc-relay-testpub` on three IPC endpoints → bridge → multicast on loopback → receiver → capture file, with sizes up to 5000 bytes so most messages are fragmented. An independent subscriber on one endpoint must receive every message (the bridge does not steal messages). The capture is verified with `capture_inspect.py --verify-testpub --strict` |
| `e2e_runtime_control` | `record on` / `record off` over the ZeroMQ TCP command channel, statistics over the ZeroMQ TCP stats channel |
| `receiver_fault_injection` | Raw datagrams built by an independent Python implementation of the wire protocol: sequence gaps, out-of-order and duplicate fragments, missing fragments (reassembly timeout), inconsistent fragments and each malformed-packet class; checks the statistics and the capture contents |
| `capture_inspect_selftest` | The capture validator against a synthetic file, including truncation and bad-magic detection |

The integration tests pick random multicast groups/ports so they can run in
parallel (`ctest -j`). They use `127.0.0.1` as the multicast interface;
set `MCAST_IFACE=<address>` to use another one. A larger unthrottled run:
`PER_SOURCE=40000 RATE=0 bash tests/integration/test_e2e.sh` with the
`BRIDGE`, `RECEIVER`, `TESTPUB`, `SUB_COUNTER`, `CTL` and `INSPECT`
environment variables pointing at the built binaries (see
`tests/CMakeLists.txt`).

### Continuous integration

[`.gitlab-ci.yml`](.gitlab-ci.yml) defines `build`, `unit-tests`,
`integration-tests`, `config-examples`, `static-analysis` (cppcheck,
shellcheck) and `package` jobs on `debian:bookworm`. Run it locally with
[gitlab-ci-local](https://github.com/firecow/gitlab-ci-local):

```sh
scripts/ci-local.sh --list           # show jobs
scripts/ci-local.sh                  # whole pipeline
scripts/ci-local.sh integration-tests
```

`scripts/ci-local.sh` is a thin wrapper around `gitlab-ci-local` that
selects docker or rootless podman and works around an rsync 3.5.0
regression (rsync issue #1083) that breaks gitlab-ci-local's file sync on
recent Fedora hosts. Plain `gitlab-ci-local` works on hosts without that
rsync version.

## Containers

[docker/](docker/) has one build image and four runtime images, plus a
compose file that runs the whole pipeline with the test publisher as the
simulated data source:

| File | Image | Contents |
|------|-------|----------|
| [docker/Dockerfile.build](docker/Dockerfile.build) | `ipc-relay-build` | Debian toolchain + libzmq headers; builds every product and installs stripped binaries into `/opt/ipc-relay` (`--build-arg RUN_TESTS=true` also runs CTest) |
| [docker/Dockerfile.relay](docker/Dockerfile.relay) | `ipc-relay-relay` | `ipc-relay-bridge` on `debian:bookworm-slim` + `libzmq5` |
| [docker/Dockerfile.receiver](docker/Dockerfile.receiver) | `ipc-relay-receiver` | `ipc-relay-receiver` and `ipc-relay-ctl` on `debian:bookworm-slim` + `libzmq5` |
| [docker/Dockerfile.testpub](docker/Dockerfile.testpub) | `ipc-relay-testpub` | `ipc-relay-testpub` on `debian:bookworm-slim` + `libzmq5` |
| [docker/Dockerfile.ctl](docker/Dockerfile.ctl) | `ipc-relay-ctl` | Web control page: `ipc-relay-ctl serve` (backend + embedded frontend) on port 8083 |

The runtime images copy their binaries from `ipc-relay-build:latest`
(override with `--build-arg BUILD_IMAGE=...`). All builds use the repository
root as the context.

```sh
docker compose -f docker/compose.yaml up --build             # build all five images and run
xdg-open http://localhost:8083/                              # live counts, start/stop recording
docker compose -f docker/compose.yaml exec receiver ipc-relay-ctl monitor
docker compose -f docker/compose.yaml exec receiver ipc-relay-ctl send --connect record off
docker compose -f docker/compose.yaml exec receiver ipc-relay-ctl send --connect record on
docker compose -f docker/compose.yaml down                   # receiver flushes and closes the capture
docker compose -f docker/compose.yaml run --rm inspect       # capture_inspect.py --verify-testpub
docker compose -f docker/compose.yaml run --rm inspect --list /captures/capture.cap
```

`podman compose` works the same way with rootless podman. In the compose
setup:

* `testpub` and `relay` share the `ipc-sockets` volume (`/run/ipc-relay`)
  for the ZeroMQ IPC endpoints. The publisher has no network.
* `relay` and `receiver` exchange UDP multicast on the `relaynet` bridge
  network through each container's `eth0`.
* `receiver` writes `/var/lib/ipc-relay/capture.cap` to the `captures`
  volume and publishes statistics on host port 5556, so a host
  `ipc-relay-ctl monitor` also works. The file is overwritten each time the
  receiver starts. The receiver binds its command endpoint (port 5557) on
  `relaynet` only, so the one-shot `send` needs `--connect`.
* `ctl` runs the web control page on host port 8083. It connects to the
  receiver's statistics (5556) and command (5557) endpoints over `relaynet`.
* Configurations are in [docker/config/](docker/config/). They are built
  into the images and also mounted from the repository, so edits only need
  a restart.
* Environment overrides: `MCAST_GROUP`, `MCAST_PORT`, `STATS_PORT`,
  `CTL_HTTP_PORT` (default 8083), `CTL_UPDATE_MS` (default 1000),
  `TESTPUB_COUNT` (0 = run until stopped), `TESTPUB_RATE`, `TESTPUB_SIZE`,
  `TESTPUB_SIZE_MAX`, `RUN_TESTS`.

## Deployment

See [docs/yocto.md](docs/yocto.md) for the Yocto recipe
([packaging/yocto](packaging/yocto)), systemd units
([packaging/systemd](packaging/systemd)) and kernel/network settings for
embedded targets.

## License

MIT, see [LICENSE](LICENSE).
