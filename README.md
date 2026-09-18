# ipc-data-relay

A ZeroMQ IPC-to-UDP-multicast bridge and a companion multicast
receiver/capture application for Linux, written in C++17 with CMake and no
dependencies beyond POSIX and libzmq.

```
ZeroMQ PUB (ipc://) x N  -->  ipc-relay-bridge  ==UDP multicast==>  ipc-relay-receiver  -->  capture.cap
                                                                        |  stats (ZMQ TCP PUB)
                                                                        |  commands (ZMQ TCP SUB)
```

* **`ipc-relay-bridge`** passively subscribes (one `ZMQ_SUB` per source,
  polled from a single thread) to any number of IPC publishers and
  republishes every message over one multicast stream, prefixing a 44-byte
  little-endian header with source id, sequence number, nanosecond
  timestamp and fragmentation metadata. Large messages are fragmented at
  the application level; IP fragmentation is never relied on.
* **`ipc-relay-receiver`** joins the group, validates and reassembles
  datagrams, detects per-source sequence gaps, keeps detailed statistics,
  publishes them as JSON over ZeroMQ TCP, accepts `record on/off` commands
  over ZeroMQ TCP, and writes all messages to a single documented binary
  capture file.
* **`ipc-relay-testpub`**, **`ipc-relay-ctl`** and
  **`tools/capture_inspect.py`** support testing, runtime control and
  offline validation.

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

CMake options:

| Option | Default | Meaning |
|--------|---------|---------|
| `IPCRELAY_BUILD_TESTS` | `ON` | Build unit tests and register the integration tests with CTest |
| `IPCRELAY_BUILD_TOOLS` | `ON` | Build `ipc-relay-testpub` and `ipc-relay-ctl` |
| `IPCRELAY_WARNINGS_AS_ERRORS` | `OFF` | `-Werror` |

If libzmq is installed in a non-standard prefix, pass
`-DCMAKE_PREFIX_PATH=/prefix` or set `PKG_CONFIG_PATH`.

## Configuration

Both applications read an INI-style file (`key = value`, `#` comments) and
accept `--set key=value` overrides on the command line; the bridge also
accepts `--source ID=ENDPOINT[,FILTER]` to add sources without a file.
`--check` validates the configuration, reports every problem found and
exits. Complete, commented examples:

* [examples/bridge.conf](examples/bridge.conf): two IPC sources, one with a
  subscription filter.
* [examples/bridge-three-publishers.conf](examples/bridge-three-publishers.conf):
  three IPC publishers feeding one multicast stream.
* [examples/receiver.conf](examples/receiver.conf): receiver with statistics
  and command channels.

Bridge options (global section): `multicast_group`, `multicast_port`,
`multicast_interface`, `multicast_ttl`, `multicast_loopback`,
`max_datagram_size`, `send_buffer_bytes`, `send_timeout_ms`,
`zmq_recv_hwm`, `max_messages_per_poll`, `reconnect_ivl_ms`,
`stats_interval_ms`, `log_level`. Each `[source]` block takes `id`,
`endpoint`, optional `name`, `recv_hwm` and any number of `filter` lines
(none, or an empty one, subscribes to all messages).

Receiver options: `multicast_group`, `multicast_port`,
`multicast_interface`, `receive_buffer_bytes`, `max_datagrams_per_poll`,
`capture_file`, `record_on_start`, `capture_buffer_bytes`,
`capture_flush_interval_ms`, `capture_sync_on_flush`,
`reassembly_timeout_ms`, `reassembly_max_pending`,
`reassembly_max_message_bytes`, `stats_endpoint`, `stats_topic`,
`stats_interval_ms`, `stats_print`, `command_endpoint`, `command_bind`,
`command_topic`, `log_level`.

## Running

A complete single-host demonstration on loopback:

```sh
# 1. Receiver: join 239.192.10.1:5100 on lo, record to /tmp/demo.cap
./build/ipc-relay-receiver --set multicast_group=239.192.10.1 --set multicast_port=5100 \
    --set multicast_interface=127.0.0.1 --set capture_file=/tmp/demo.cap

# 2. Bridge: three IPC sources -> the same group
./build/ipc-relay-bridge --config examples/bridge-three-publishers.conf

# 3. Publishers (the test publisher stands in for the real applications)
./build/ipc-relay-testpub -e ipc:///tmp/nav.sock -e ipc:///tmp/imu.sock -e ipc:///tmp/log.sock \
    --count 0 --rate 500 --size 100 --size-max 3000

# 4. Watch live statistics (JSON, one line per second) and control recording
./build/ipc-relay-ctl monitor --endpoint tcp://127.0.0.1:5556
./build/ipc-relay-ctl send record off
./build/ipc-relay-ctl send record on

# 5. Stop with Ctrl-C / SIGTERM (both apps shut down cleanly and print final statistics), then inspect
python3 tools/capture_inspect.py /tmp/demo.cap
python3 tools/capture_inspect.py --verify-testpub --strict /tmp/demo.cap
```

Note that `examples/bridge-three-publishers.conf` only forwards `ERR`- and
`WARN`-prefixed messages from the third source; use `--topic ERR` on the
test publisher to see them.

Logging goes to stderr with a configurable level (`log_level` or
`--log-level`); `debug` prints one line per message and should be off in
production. The bridge logs its counters every `stats_interval_ms`; the
receiver publishes its JSON statistics over ZeroMQ and can also log them
(`stats_print = true`). Both log a final statistics summary on shutdown.

Runtime commands understood by the receiver (`ipc-relay-ctl send ...`):
`record on`, `record off`, `record toggle`, `flush`, `stats`. By default
`ipc-relay-ctl send` binds `tcp://127.0.0.1:5557` and the receiver connects
to it; set `command_bind = true` in the receiver and use `--connect` on the
tool to reverse that.

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

## Deployment

See [docs/yocto.md](docs/yocto.md) for the Yocto recipe
([packaging/yocto](packaging/yocto)), systemd units
([packaging/systemd](packaging/systemd)) and kernel/network settings for
embedded targets.

## License

MIT, see [LICENSE](LICENSE).
