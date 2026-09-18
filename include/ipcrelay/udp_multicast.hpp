// Thin RAII wrappers around POSIX UDP multicast sockets (BRG-030..BRG-034).
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <sys/uio.h>

namespace ipcrelay {

struct MulticastSenderConfig {
    uint32_t group_host_order = 0;
    uint16_t port = 0;
    uint32_t interface_host_order = 0;  // 0 = default
    int ttl = 1;
    bool loopback = true;
    int send_buffer_bytes = 0;          // 0 = leave kernel default
    int send_timeout_ms = 100;          // bounded blocking on a full socket buffer
};

class UdpMulticastSender {
public:
    UdpMulticastSender() = default;
    ~UdpMulticastSender();
    UdpMulticastSender(const UdpMulticastSender&) = delete;
    UdpMulticastSender& operator=(const UdpMulticastSender&) = delete;

    bool open(const MulticastSenderConfig& cfg, std::string& error);
    void close();
    int fd() const { return fd_; }

    // Sends one datagram made of the given iovecs (scatter/gather, so the
    // payload is not copied into a contiguous buffer, BRG-120).
    // Returns bytes sent, or -1 with errno set.
    long send(const struct iovec* iov, int iovcnt);

private:
    int fd_ = -1;
};

struct MulticastReceiverConfig {
    uint32_t group_host_order = 0;
    uint16_t port = 0;
    uint32_t interface_host_order = 0;  // 0 = default
    int receive_buffer_bytes = 0;       // 0 = leave kernel default
};

class UdpMulticastReceiver {
public:
    UdpMulticastReceiver() = default;
    ~UdpMulticastReceiver();
    UdpMulticastReceiver(const UdpMulticastReceiver&) = delete;
    UdpMulticastReceiver& operator=(const UdpMulticastReceiver&) = delete;

    bool open(const MulticastReceiverConfig& cfg, std::string& error);
    void close();
    int fd() const { return fd_; }

    // Non-blocking receive of one datagram. Returns the datagram length, 0 if
    // nothing was available (EAGAIN), or -1 with errno set. kernel_drops is
    // updated with the running SO_RXQ_OVFL counter reported by the kernel.
    long receive(uint8_t* buf, std::size_t len, uint32_t& kernel_drops);

    int actual_receive_buffer() const { return actual_rcvbuf_; }

private:
    int fd_ = -1;
    int actual_rcvbuf_ = 0;
};

}  // namespace ipcrelay
