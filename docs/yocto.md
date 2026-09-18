# Embedded Linux / Yocto Integration

The project has no dependencies beyond a C++17 toolchain, CMake ≥ 3.16,
libzmq and (for the tools/tests only) Python 3, which makes it
straightforward to package for a Yocto-based image (BRG-002, BRG-146).

## Recipe

A ready-to-adapt recipe is in
[`packaging/yocto/ipc-data-relay_git.bb`](../packaging/yocto/ipc-data-relay_git.bb).
Key points:

```bitbake
inherit cmake systemd
DEPENDS = "zeromq"
RDEPENDS:${PN}-tools = "python3-core"
EXTRA_OECMAKE = "-DIPCRELAY_BUILD_TESTS=OFF -DIPCRELAY_BUILD_TOOLS=ON"
```

* `zeromq` is provided by `meta-oe` (`recipes-connectivity/zeromq`).
* `find_package(ZeroMQ)` uses `pkg-config` first, which the Yocto `cmake`
  class configures for the sysroot; no cross-compilation-specific CMake
  changes are needed.
* Tests are disabled in the recipe (`IPCRELAY_BUILD_TESTS=OFF`) because they
  execute binaries; run them on the development host or in a QEMU image
  with `ptest` if desired.
* The recipe splits packages: `ipc-data-relay` (bridge + receiver),
  `ipc-data-relay-tools` (test publisher, control tool, capture inspector)
  and `ipc-data-relay-examples`.

Place the recipe in your layer (for example
`meta-yourproduct/recipes-connectivity/ipc-data-relay/`), point `SRC_URI`
at your git server and set `SRCREV`, then add `ipc-data-relay` to
`IMAGE_INSTALL`.

## systemd services

[`packaging/systemd/`](../packaging/systemd/) contains
`ipc-relay-bridge.service` and `ipc-relay-receiver.service`. They read
`/etc/ipc-data-relay/bridge.conf` and `/etc/ipc-data-relay/receiver.conf`
(installed from `examples/` by `cmake --install` and by the recipe) and run
as a dedicated unprivileged user. Adapt:

* `multicast_interface` to the board's network interface name;
* `[source] endpoint` paths to the IPC sockets of the on-target
  publishers (typically under `/run`), and make sure the service user can
  read them (same group, or `SupplementaryGroups=`);
* `capture_file` to a writable location on persistent storage with enough
  space; a `tmpfiles.d` snippet or `StateDirectory=` creates it.

`ipc-relay-bridge.service` uses `After=`/`Wants=` on `network-online.target`
so the multicast interface exists when the bridge opens its socket. The
bridge keeps trying to connect to IPC endpoints that are not up yet
(`reconnect_ivl_ms`), so ordering against the publisher services is not
required, but `After=` them to avoid the initial reconnect delay.

## Kernel and network settings

* Ensure the multicast interface has `MULTICAST` set and, on a switched
  network with IGMP snooping, that a querier exists; otherwise switches may
  not forward the group. `ip maddr show dev eth0` shows the joined groups on
  the receiver.
* Raise `net.core.rmem_max` (for example to 8 MB) on the receiver so
  `receive_buffer_bytes` is honoured; the receiver logs a warning when the
  kernel clamps the value. The systemd unit sets no sysctl itself; add a
  `sysctl.d` fragment to the image.
* For the bridge, `net.core.wmem_max` limits `send_buffer_bytes` the same
  way.
* If receivers are on the same host as the bridge, leave
  `multicast_loopback = true`; set `multicast_ttl` to the number of router
  hops needed (usually 1).

## Cross-compiling outside Yocto

The project builds with any CMake toolchain file:

```
cmake -S . -B build-arm -DCMAKE_TOOLCHAIN_FILE=toolchain.cmake \
      -DCMAKE_PREFIX_PATH=/path/to/sysroot/usr -DIPCRELAY_BUILD_TESTS=OFF
cmake --build build-arm
```

`FindZeroMQ.cmake` honours `PKG_CONFIG_PATH`/`PKG_CONFIG_SYSROOT_DIR` and
`CMAKE_PREFIX_PATH`.

## Resource footprint

Both binaries are a few hundred kB, link only libzmq, libstdc++ and libc,
use one thread each and allocate at startup (socket buffers, receive
buffer, capture buffer) plus bounded reassembly state; see
[limitations.md](limitations.md) for the bounds.
