SUMMARY = "ZeroMQ IPC to UDP multicast bridge and capture receiver"
DESCRIPTION = "Passively subscribes to ZeroMQ IPC publishers and republishes \
their messages over UDP multicast with a compact little-endian header; a \
companion receiver reassembles, monitors and records the stream to a binary \
capture file."
HOMEPAGE = "https://example.com/ipc-data-relay"
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://LICENSE;md5=REPLACE_WITH_ACTUAL_MD5"

SRC_URI = "git://git.example.com/ipc-data-relay.git;protocol=https;branch=main"
SRCREV = "${AUTOREV}"
S = "${WORKDIR}/git"

DEPENDS = "zeromq"

inherit cmake systemd useradd

EXTRA_OECMAKE = " \
    -DCMAKE_BUILD_TYPE=Release \
    -DIPCRELAY_BUILD_TESTS=OFF \
    -DIPCRELAY_BUILD_TOOLS=ON \
"

USERADD_PACKAGES = "${PN}"
USERADD_PARAM:${PN} = "--system --no-create-home --shell /sbin/nologin --user-group ipcrelay"

SYSTEMD_PACKAGES = "${PN}"
SYSTEMD_SERVICE:${PN} = "ipc-relay-bridge.service ipc-relay-receiver.service"
SYSTEMD_AUTO_ENABLE:${PN} = "disable"

do_install:append() {
    install -d ${D}${systemd_system_unitdir}
    install -m 0644 ${S}/packaging/systemd/ipc-relay-bridge.service ${D}${systemd_system_unitdir}/
    install -m 0644 ${S}/packaging/systemd/ipc-relay-receiver.service ${D}${systemd_system_unitdir}/
    install -d ${D}${docdir}/${PN}
    install -m 0644 ${S}/docs/*.md ${S}/README.md ${D}${docdir}/${PN}/
}

PACKAGES =+ "${PN}-tools ${PN}-examples"

FILES:${PN} = " \
    ${bindir}/ipc-relay-bridge \
    ${bindir}/ipc-relay-receiver \
    ${systemd_system_unitdir}/*.service \
"
FILES:${PN}-tools = " \
    ${bindir}/ipc-relay-testpub \
    ${bindir}/ipc-relay-ctl \
    ${bindir}/ipc-relay-capture-inspect \
"
FILES:${PN}-examples = "${sysconfdir}/ipc-data-relay"
CONFFILES:${PN}-examples = " \
    ${sysconfdir}/ipc-data-relay/bridge.conf \
    ${sysconfdir}/ipc-data-relay/receiver.conf \
    ${sysconfdir}/ipc-data-relay/bridge-three-publishers.conf \
"

RDEPENDS:${PN} += "${PN}-examples"
RDEPENDS:${PN}-tools += "python3-core"
