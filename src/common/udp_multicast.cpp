#include "ipcrelay/common/udp_multicast.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

namespace ipcrelay {

// Note: the sockaddr/ip_mreq fields below are defined by the BSD socket API and
// therefore use htonl()/htons(). They are not application wire-protocol fields.

static std::string errno_string(const char* what) {
    return std::string(what) + ": " + std::strerror(errno);
}

UdpMulticastSender::~UdpMulticastSender() { close(); }

void UdpMulticastSender::close() {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

bool UdpMulticastSender::open(const MulticastSenderConfig& cfg, std::string& error) {
    close();
    fd_ = ::socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, IPPROTO_UDP);
    if (fd_ < 0) {
        error = errno_string("socket");
        return false;
    }
    struct in_addr ifaddr;
    ifaddr.s_addr = htonl(cfg.interface_host_order);
    if (cfg.interface_host_order != 0 &&
        setsockopt(fd_, IPPROTO_IP, IP_MULTICAST_IF, &ifaddr, sizeof ifaddr) != 0) {
        error = errno_string("setsockopt(IP_MULTICAST_IF)");
        close();
        return false;
    }
    unsigned char ttl = static_cast<unsigned char>(cfg.ttl);
    if (setsockopt(fd_, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof ttl) != 0) {
        error = errno_string("setsockopt(IP_MULTICAST_TTL)");
        close();
        return false;
    }
    unsigned char loop = cfg.loopback ? 1 : 0;
    if (setsockopt(fd_, IPPROTO_IP, IP_MULTICAST_LOOP, &loop, sizeof loop) != 0) {
        error = errno_string("setsockopt(IP_MULTICAST_LOOP)");
        close();
        return false;
    }
    if (cfg.send_buffer_bytes > 0) {
        int sz = cfg.send_buffer_bytes;
        if (setsockopt(fd_, SOL_SOCKET, SO_SNDBUF, &sz, sizeof sz) != 0) {
            error = errno_string("setsockopt(SO_SNDBUF)");
            close();
            return false;
        }
    }
    if (cfg.send_timeout_ms > 0) {
        struct timeval tv;
        tv.tv_sec = cfg.send_timeout_ms / 1000;
        tv.tv_usec = (cfg.send_timeout_ms % 1000) * 1000;
        if (setsockopt(fd_, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv) != 0) {
            error = errno_string("setsockopt(SO_SNDTIMEO)");
            close();
            return false;
        }
    }
    struct sockaddr_in dst {};
    dst.sin_family = AF_INET;
    dst.sin_port = htons(cfg.port);
    dst.sin_addr.s_addr = htonl(cfg.group_host_order);
    // connect() fixes the destination so send() can be used with sendmsg
    // without a per-call address and lets the kernel report ICMP errors.
    if (::connect(fd_, reinterpret_cast<struct sockaddr*>(&dst), sizeof dst) != 0) {
        error = errno_string("connect");
        close();
        return false;
    }
    return true;
}

long UdpMulticastSender::send(const struct iovec* iov, int iovcnt) {
    struct msghdr msg {};
    msg.msg_iov = const_cast<struct iovec*>(iov);
    msg.msg_iovlen = static_cast<std::size_t>(iovcnt);
    for (;;) {
        ssize_t n = ::sendmsg(fd_, &msg, MSG_NOSIGNAL);
        if (n < 0 && errno == EINTR) continue;
        return static_cast<long>(n);
    }
}

UdpMulticastReceiver::~UdpMulticastReceiver() { close(); }

void UdpMulticastReceiver::close() {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

bool UdpMulticastReceiver::open(const MulticastReceiverConfig& cfg, std::string& error) {
    close();
    fd_ = ::socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, IPPROTO_UDP);
    if (fd_ < 0) {
        error = errno_string("socket");
        return false;
    }
    int one = 1;
    if (setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one) != 0) {
        error = errno_string("setsockopt(SO_REUSEADDR)");
        close();
        return false;
    }
    if (cfg.receive_buffer_bytes > 0) {
        int sz = cfg.receive_buffer_bytes;
        if (setsockopt(fd_, SOL_SOCKET, SO_RCVBUF, &sz, sizeof sz) != 0) {
            error = errno_string("setsockopt(SO_RCVBUF)");
            close();
            return false;
        }
    }
    socklen_t optlen = sizeof actual_rcvbuf_;
    getsockopt(fd_, SOL_SOCKET, SO_RCVBUF, &actual_rcvbuf_, &optlen);

    // Ask the kernel to report receive-queue overflow so drops are visible
    // in the statistics (BRG-122). Failure is not fatal.
    setsockopt(fd_, SOL_SOCKET, SO_RXQ_OVFL, &one, sizeof one);

    // Bind to the group address so this socket only sees datagrams for the
    // group, not all unicast traffic to the port.
    struct sockaddr_in local {};
    local.sin_family = AF_INET;
    local.sin_port = htons(cfg.port);
    local.sin_addr.s_addr = htonl(cfg.group_host_order);
    if (::bind(fd_, reinterpret_cast<struct sockaddr*>(&local), sizeof local) != 0) {
        error = errno_string("bind");
        close();
        return false;
    }
    struct ip_mreq mreq {};
    mreq.imr_multiaddr.s_addr = htonl(cfg.group_host_order);
    mreq.imr_interface.s_addr = htonl(cfg.interface_host_order);
    if (setsockopt(fd_, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof mreq) != 0) {
        error = errno_string("setsockopt(IP_ADD_MEMBERSHIP)");
        close();
        return false;
    }
    return true;
}

long UdpMulticastReceiver::receive(uint8_t* buf, std::size_t len, uint32_t& kernel_drops) {
    struct iovec iov;
    iov.iov_base = buf;
    iov.iov_len = len;
    alignas(struct cmsghdr) char control[64];
    struct msghdr msg {};
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = control;
    msg.msg_controllen = sizeof control;
    for (;;) {
        ssize_t n = ::recvmsg(fd_, &msg, MSG_DONTWAIT | MSG_TRUNC);
        if (n < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
            return -1;
        }
        for (struct cmsghdr* c = CMSG_FIRSTHDR(&msg); c; c = CMSG_NXTHDR(&msg, c)) {
            if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SO_RXQ_OVFL) {
                uint32_t v;
                std::memcpy(&v, CMSG_DATA(c), sizeof v);
                kernel_drops = v;
            }
        }
        // With MSG_TRUNC, n is the full datagram length even if it exceeded
        // the buffer; callers compare against len to detect oversize input.
        return static_cast<long>(n);
    }
}

}  // namespace ipcrelay
