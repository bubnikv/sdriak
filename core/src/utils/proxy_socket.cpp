#include "proxy_socket.h"

#include <chrono>
#include <stdexcept>

#include <utils/proxy.h>

namespace net::proxy {

    std::shared_ptr<net::Socket> connectSocket(const std::string& host, int port, int timeoutMs) {
        const TcpRoute route = tcpRoute(host);
        if (!route.error.empty()) { throw std::runtime_error("proxy: " + route.error); }
        if (route.direct) { return net::connect(host, port, timeoutMs); }

        const int budgetMs = timeoutMs < 0 ? 10000 : timeoutMs;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(budgetMs);
        auto remainingMs = [&deadline] {
            const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
            return left > 0 ? (int)left : 0;
        };
        std::shared_ptr<net::Socket> sock = net::connect(route.proxyHost, route.proxyPort, budgetMs);
        if (!sock) { throw std::runtime_error("proxy: could not connect to " + route.proxyHost); }

        const std::string authority = detail::authorityOf(host, port);
        const std::string request = detail::connectRequest(route, authority);
        if (sock->send(reinterpret_cast<const uint8_t*>(request.data()), request.size()) != (int)request.size()) {
            sock->close();
            throw std::runtime_error("proxy: sending CONNECT to " + route.proxyHost + " failed");
        }

        // Socket::recv() times out by itself. One byte at a time, so nothing
        // the server sends through the tunnel is consumed here.
        std::string head;
        bool timedOut = false;
        uint8_t ch = 0;
        while (head.size() < 8192 && head.find("\r\n\r\n") == std::string::npos) {
            const int left = remainingMs();
            if (left <= 0) {
                timedOut = true;
                break;
            }
            if (sock->recv(&ch, 1, false, left) == 1) {
                head.push_back((char)ch);
                continue;
            }
            // 0 is a timeout or a close; only the deadline tells them apart.
            timedOut = remainingMs() <= 0 && sock->isOpen();
            break;
        }
        if (head.find("\r\n\r\n") == std::string::npos) { sock->close(); }
        detail::checkConnectReply(head, timedOut, budgetMs, route, authority);
        return sock;
    }
}
