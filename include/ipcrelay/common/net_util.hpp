#pragma once

#include <cstdint>
#include <string>

namespace ipcrelay {

// Resolves an interface specification to an IPv4 address in host byte order.
// Accepts a dotted-quad address ("192.168.1.10"), an interface name
// ("eth0"), or "" / "any" / "0.0.0.0" for the default interface.
bool resolve_interface_ipv4(const std::string& spec, uint32_t& addr_host_order, std::string& error);

std::string ipv4_to_string(uint32_t addr_host_order);

}  // namespace ipcrelay
