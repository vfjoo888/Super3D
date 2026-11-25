#pragma once

#include <cstdint>
#include <string>

namespace nx::net {

// True when Nextendo / online networking is enabled (via NX_ONLINE_MODE=1 or settings).
bool online_enabled();

// Returns the original Nintendo hostname last resolved to this IPv4 address (network byte order),
// so TLS SNI can still be sent when the game connects by resolved IP.
std::string lookup_sni_host(uint32_t ipv4_be);

// Applies Nextendo DNS redirect rules to a hostname (empty when no redirect applies).
std::string redirect_target_host(const std::string& host, bool& is_nat);

// Returns the underlying native socket descriptor (SOCKET on Windows, int fd on POSIX) for a virtual nn::socket fd.
intptr_t get_host_socket(int fd);

}  // namespace nx::net
