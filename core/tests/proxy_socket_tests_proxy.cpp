// The test proxy (utils/networking.h) behind a plain interface, so
// proxy_socket_tests.cpp can use utils/net.h in its own translation unit.
#include "test_http_proxy.h"

#include <cstdint>
#include <string>

namespace proxy_socket_tests {
    void* startProxy(const char* user, const char* password, uint16_t firstPort, std::string& url) {
        auto* p = new test_proxy::HttpProxy(user, password, firstPort);
        url = p->url();
        return p;
    }
    int proxyTunnels(void* p) { return static_cast<test_proxy::HttpProxy*>(p)->tunnels(); }
    int proxyRejected(void* p) { return static_cast<test_proxy::HttpProxy*>(p)->rejected(); }
    void stopProxy(void* p) { delete static_cast<test_proxy::HttpProxy*>(p); }
}
