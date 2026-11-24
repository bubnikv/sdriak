// net::proxy::connectSocket() (the utils/net.h socket, sdriak_server_source)
// through the in-process authenticating proxy: an echo through the CONNECT
// tunnel, a wrong password (407), noProxy going direct, a silent proxy timing
// out. The proxy lives in proxy_socket_tests_proxy.cpp: utils/net.h and
// utils/networking.h cannot share a translation unit.
#include <utils/net.h>
#include <utils/proxy.h>
#include <utils/proxy_socket.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

namespace proxy_socket_tests {
    void* startProxy(const char* user, const char* password, uint16_t firstPort, std::string& url);
    int proxyTunnels(void* p);
    int proxyRejected(void* p);
    void stopProxy(void* p);
}

namespace {
    using Clock = std::chrono::steady_clock;
    using namespace proxy_socket_tests;

    void require(bool condition, const std::string& message) {
        if (!condition) { throw std::runtime_error(message); }
    }

    struct Proxy {
        void* handle;
        std::string url;
        Proxy(const char* user, const char* password) { handle = startProxy(user, password, 24600, url); }
        ~Proxy() { stopProxy(handle); }
        int tunnels() const { return proxyTunnels(handle); }
        int rejected() const { return proxyRejected(handle); }
    };

    struct ProxyScope {
        net::proxy::Config saved;
        explicit ProxyScope(const net::proxy::Config& c) : saved(net::proxy::config()) { net::proxy::setConfig(c); }
        ~ProxyScope() { net::proxy::setConfig(saved); }
    };

    net::proxy::Config manual(const std::string& url, const std::string& user = "",
                              const std::string& password = "", const std::string& noProxy = "") {
        net::proxy::Config c;
        c.mode = net::proxy::Mode::Manual;
        c.url = url;
        c.user = user;
        c.password = password;
        c.noProxy = noProxy;
        return c;
    }

    // utils/net.h echo server; silent: reads but never answers.
    class Echo {
    public:
        explicit Echo(bool silent = false) : m_silent(silent) {
            for (m_port = 24620; m_port < 24640; m_port++) {
                try {
                    m_listener = net::listen("127.0.0.1", m_port);
                    if (m_listener) { break; }
                }
                catch (const std::exception&) {}
            }
            if (!m_listener) { throw std::runtime_error("echo: no free port"); }
            m_thread = std::thread([this] {
                while (!m_stop) {
                    std::shared_ptr<net::Socket> s;
                    try { s = m_listener->accept(nullptr, 100); }
                    catch (...) { return; }
                    if (!s) { continue; }
                    uint8_t b[4096];
                    while (!m_stop) {
                        const int n = s->recv(b, sizeof(b), false, 100);
                        if (n < 0 || !s->isOpen()) { break; }
                        if (n > 0 && !m_silent) { s->send(b, (size_t)n); }
                    }
                    s->close();
                }
            });
        }
        ~Echo() {
            m_stop = true;
            m_thread.join();
            m_listener->stop();
        }
        int port() const { return m_port; }

    private:
        bool m_silent;
        int m_port = 0;
        std::shared_ptr<net::Listener> m_listener;
        std::thread m_thread;
        std::atomic<bool> m_stop{false};
    };

    std::string echoOnce(const std::shared_ptr<net::Socket>& s, const std::string& msg) {
        require(s->send(reinterpret_cast<const uint8_t*>(msg.data()), msg.size()) == (int)msg.size(), "echo send");
        std::string got(msg.size(), '\0');
        require(s->recv(reinterpret_cast<uint8_t*>(got.data()), got.size(), true, 3000) == (int)got.size(), "echo recv");
        return got;
    }

    void tunnel() {
        Echo echo;
        Proxy proxy("alice", "secret");
        ProxyScope scope(manual(proxy.url, "alice", "secret"));
        auto s = net::proxy::connectSocket("127.0.0.1", echo.port(), 3000);
        require(echoOnce(s, "sdriak hello") == "sdriak hello", "tunnelled echo differs");
        s->close();
        require(proxy.tunnels() == 1 && proxy.rejected() == 0, "expected one authenticated CONNECT");
    }

    void wrongPassword() {
        Echo echo;
        Proxy proxy("alice", "secret");
        ProxyScope scope(manual(proxy.url, "alice", "wrong"));
        bool threw = false;
        try { net::proxy::connectSocket("127.0.0.1", echo.port(), 3000); }
        catch (const std::exception& e) { threw = std::string(e.what()).find("407") != std::string::npos; }
        require(threw, "wrong password must throw with the 407 status");
    }

    void bypass() {
        Echo echo;
        Proxy proxy("", "");
        ProxyScope scope(manual(proxy.url, "", "", "127.0.0.1"));
        auto s = net::proxy::connectSocket("127.0.0.1", echo.port(), 3000);
        require(echoOnce(s, "direct") == "direct", "bypassed echo differs");
        s->close();
        require(proxy.tunnels() == 0, "a noProxy host was tunnelled");
    }

    void timeout() {
        Echo silentProxy(true);
        ProxyScope scope(manual("http://127.0.0.1:" + std::to_string(silentProxy.port())));
        const auto t0 = Clock::now();
        bool threw = false;
        try { net::proxy::connectSocket("example.invalid", 5259, 500); }
        catch (const std::exception& e) { threw = std::string(e.what()).find("no answer") != std::string::npos; }
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count();
        require(threw, "silent proxy did not time out");
        require(ms < 2000, "CONNECT timeout took " + std::to_string(ms) + " ms");
    }
}

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::thread([] {
        std::this_thread::sleep_for(std::chrono::seconds(60));
        std::printf("FAIL (hang): whole test run exceeded 60 s\n");
        std::_Exit(2);
    }).detach();
    const std::pair<const char*, void (*)()> tests[] = {
        { "tunnel through an authenticating proxy", tunnel },
        { "wrong proxy password", wrongPassword },
        { "noProxy goes direct", bypass },
        { "silent proxy times out", timeout },
    };
    int failures = 0;
    for (const auto& [name, fn] : tests) {
        try {
            fn();
            std::printf("PASS %s\n", name);
        }
        catch (const std::exception& e) {
            std::printf("FAIL %s: %s\n", name, e.what());
            failures++;
        }
    }
    return failures == 0 ? 0 : 1;
}
