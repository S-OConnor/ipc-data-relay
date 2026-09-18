#include "ipcrelay/net_util.hpp"

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <cstring>

#include "ipcrelay/config.hpp"

namespace ipcrelay {

std::string ipv4_to_string(uint32_t addr_host_order) {
    struct in_addr a;
    a.s_addr = htonl(addr_host_order);
    char buf[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &a, buf, sizeof buf);
    return buf;
}

bool resolve_interface_ipv4(const std::string& spec, uint32_t& addr_host_order, std::string& error) {
    std::string s = trim(spec);
    if (s.empty() || s == "any" || s == "0.0.0.0" || s == "default") {
        addr_host_order = 0;
        return true;
    }
    std::string ignored;
    if (parse_ipv4(s, addr_host_order, ignored)) return true;

    struct ifaddrs* ifs = nullptr;
    if (getifaddrs(&ifs) != 0) {
        error = "getifaddrs failed: " + std::string(std::strerror(errno));
        return false;
    }
    bool found = false;
    bool name_seen = false;
    for (struct ifaddrs* p = ifs; p; p = p->ifa_next) {
        if (!p->ifa_name || s != p->ifa_name) continue;
        name_seen = true;
        if (!p->ifa_addr || p->ifa_addr->sa_family != AF_INET) continue;
        auto* sin = reinterpret_cast<struct sockaddr_in*>(p->ifa_addr);
        addr_host_order = ntohl(sin->sin_addr.s_addr);
        found = true;
        break;
    }
    freeifaddrs(ifs);
    if (!found) {
        error = name_seen ? "interface '" + s + "' has no IPv4 address"
                          : "'" + s + "' is neither an IPv4 address nor a known interface name";
        return false;
    }
    return true;
}

}  // namespace ipcrelay
