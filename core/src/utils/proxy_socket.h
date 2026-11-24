#pragma once
// net::proxy::connect() for the utils/net.h socket library (sdriak_server_
// source). Separate from proxy.h because utils/net.h and utils/networking.h
// both define net::Socket and cannot share a translation unit.
#include <memory>
#include <string>

#include <utils/net.h>

namespace net::proxy {
    // TCP to host:port along tcpRoute(host), like net::proxy::connect().
    // Throws std::runtime_error like net::connect().
    std::shared_ptr<net::Socket> connectSocket(const std::string& host, int port, int timeoutMs);
}
