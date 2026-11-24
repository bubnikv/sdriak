#pragma once
// A small HTTP proxy for tests, on loopback: CONNECT tunnels (https, wss,
// and ws:// with CURLOPT_HTTPPROXYTUNNEL) and absolute-form requests
// ("GET http://host/path"), forwarded to the real target. With credentials
// set it answers 407 until Proxy-Authorization: Basic matches. Counts what
// it saw so tests can tell whether traffic went through it.
#include <utils/base64.h>
#include <utils/networking.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace test_proxy {

    class HttpProxy {
    public:
        // user empty: no authentication. firstPort: start of the port block.
        HttpProxy(std::string user, std::string password, uint16_t firstPort)
            : m_user(std::move(user)), m_password(std::move(password)) {
            for (m_port = firstPort; m_port < firstPort + 20; m_port++) {
                try {
                    m_listener = net::listen("127.0.0.1", m_port);
                    break;
                }
                catch (const std::exception&) {}
            }
            if (!m_listener) { throw std::runtime_error("test proxy: no free port"); }
            m_acceptor = std::thread([this] { acceptLoop(); });
        }

        ~HttpProxy() {
            m_stop = true;
            try {
                auto wake = net::connect("127.0.0.1", m_port, 1000);
                if (wake) { wake->close(); }
            }
            catch (...) {}
            m_listener->close();
            m_acceptor.join();
            std::vector<std::shared_ptr<Session>> sessions;
            {
                std::lock_guard<std::mutex> lock(m_mtx);
                sessions = m_sessions;
            }
            for (auto& s : sessions) { s->closeBoth(); }
            for (auto& s : sessions) {
                if (s->thread.joinable()) { s->thread.join(); }
            }
        }

        std::string url() const { return "http://127.0.0.1:" + std::to_string(m_port); }
        int tunnels() const { return m_tunnels; }        // CONNECTs accepted
        int forwarded() const { return m_forwarded; }    // absolute-form requests accepted
        int rejected() const { return m_rejected; }      // 407 answers
        std::vector<std::string> targets() const {
            std::lock_guard<std::mutex> lock(m_mtx);
            return m_targets;
        }

    private:
        struct Session {
            net::Conn client;
            net::Conn upstream;
            std::thread thread;
            std::mutex mtx;
            void closeBoth() {
                std::lock_guard<std::mutex> lock(mtx);
                if (client) { client->close(); }
                if (upstream) { upstream->close(); }
            }
        };

        static std::string lower(std::string s) {
            std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::tolower(c); });
            return s;
        }

        static std::string header(const std::string& head, const std::string& name) {
            const std::string l = lower(head);
            const size_t k = l.find("\r\n" + lower(name) + ":");
            if (k == std::string::npos) { return {}; }
            const size_t start = k + 3 + name.size();
            const size_t end = head.find("\r\n", start);
            std::string v = head.substr(start, end - start);
            v.erase(0, v.find_first_not_of(' '));
            return v;
        }

        static void send(net::Conn& c, const std::string& s) {
            c->write((int)s.size(), reinterpret_cast<uint8_t*>(const_cast<char*>(s.data())));
        }

        static bool splitAuthority(const std::string& a, std::string& host, int& port, int defPort) {
            std::string hp = a;
            port = defPort;
            size_t colon = hp.rfind(':');
            if (!hp.empty() && hp.front() == '[') {
                const size_t close = hp.find(']');
                if (close == std::string::npos) { return false; }
                host = hp.substr(1, close - 1);
                if (close + 1 < hp.size() && hp[close + 1] == ':') { port = std::atoi(hp.c_str() + close + 2); }
                return true;
            }
            if (colon != std::string::npos) {
                host = hp.substr(0, colon);
                port = std::atoi(hp.c_str() + colon + 1);
            }
            else {
                host = hp;
            }
            return !host.empty() && port > 0;
        }

        void acceptLoop() {
            for (;;) {
                net::Conn c;
                try { c = m_listener->accept(); }
                catch (...) { return; }
                if (m_stop || !c) { return; }
                auto s = std::make_shared<Session>();
                s->client = std::move(c);
                std::lock_guard<std::mutex> lock(m_mtx);
                m_sessions.push_back(s);
                s->thread = std::thread([this, s] { serve(*s); });
            }
        }

        void serve(Session& s) {
            std::string buf;
            uint8_t tmp[16384];
            for (;;) {
                size_t end;
                while ((end = buf.find("\r\n\r\n")) == std::string::npos) {
                    const int n = s.client->read(sizeof(tmp), tmp, false);
                    if (n <= 0) { return; }
                    buf.append(reinterpret_cast<const char*>(tmp), (size_t)n);
                }
                std::string head = buf.substr(0, end + 4);
                buf.erase(0, end + 4);

                if (!m_user.empty()) {
                    const std::string cred = m_user + ":" + m_password;
                    const std::string want = "Basic " + base64::encode(reinterpret_cast<const uint8_t*>(cred.data()), cred.size());
                    if (header(head, "Proxy-Authorization") != want) {
                        m_rejected++;
                        // Skip the rejected request's body before the retry.
                        const std::string len = header(head, "Content-Length");
                        size_t skip = len.empty() ? 0 : (size_t)std::stoul(len);
                        while (buf.size() < skip) {
                            const int n = s.client->read(sizeof(tmp), tmp, false);
                            if (n <= 0) { return; }
                            buf.append(reinterpret_cast<const char*>(tmp), (size_t)n);
                        }
                        buf.erase(0, skip);
                        // Keep the connection: libcurl answers the challenge on it.
                        send(s.client, "HTTP/1.1 407 Proxy Authentication Required\r\n"
                                       "Proxy-Authenticate: Basic realm=\"test\"\r\n"
                                       "Content-Length: 0\r\n\r\n");
                        continue;
                    }
                }

                const size_t sp1 = head.find(' ');
                const size_t sp2 = head.find(' ', sp1 + 1);
                const std::string method = head.substr(0, sp1);
                const std::string target = head.substr(sp1 + 1, sp2 - sp1 - 1);
                std::string host;
                int port = 0;
                std::string forwardHead;

                if (method == "CONNECT") {
                    if (!splitAuthority(target, host, port, 443)) { return; }
                }
                else {
                    // Absolute form: METHOD http://host[:port]/path HTTP/1.1
                    if (target.rfind("http://", 0) != 0) { return; }
                    const size_t pathAt = target.find('/', 7);
                    const std::string authority = target.substr(7, pathAt == std::string::npos ? std::string::npos : pathAt - 7);
                    if (!splitAuthority(authority, host, port, 80)) { return; }
                    const std::string path = pathAt == std::string::npos ? "/" : target.substr(pathAt);
                    forwardHead = method + " " + path + head.substr(sp2);
                    // Drop Proxy-* headers on the way.
                    std::string cleaned;
                    size_t pos = 0;
                    while (pos < forwardHead.size()) {
                        const size_t eol = forwardHead.find("\r\n", pos);
                        const std::string line = forwardHead.substr(pos, eol - pos + 2);
                        if (lower(line).rfind("proxy-", 0) != 0) { cleaned += line; }
                        pos = eol + 2;
                    }
                    forwardHead = cleaned;
                }

                net::Conn up;
                try { up = net::connect(host, port, 5000); }
                catch (...) {}
                if (!up) {
                    send(s.client, "HTTP/1.1 502 Bad Gateway\r\nContent-Length: 0\r\n\r\n");
                    return;
                }
                {
                    std::lock_guard<std::mutex> lock(m_mtx);
                    m_targets.push_back(method + " " + host + ":" + std::to_string(port));
                }
                {
                    std::lock_guard<std::mutex> lock(s.mtx);
                    s.upstream = std::move(up);
                }
                if (method == "CONNECT") {
                    m_tunnels++;
                    send(s.client, "HTTP/1.1 200 Connection established\r\n\r\n");
                }
                else {
                    m_forwarded++;
                    send(s.upstream, forwardHead);
                }
                if (!buf.empty()) { send(s.upstream, buf); }
                pump(s);
                return;
            }
        }

        // Copies both ways until either side closes, then closes both.
        void pump(Session& s) {
            std::thread down([&s] {
                uint8_t b[16384];
                for (;;) {
                    const int n = s.upstream->read(sizeof(b), b, false);
                    if (n <= 0 || !s.client->write(n, b)) { break; }
                }
                s.closeBoth();
            });
            uint8_t b[16384];
            for (;;) {
                const int n = s.client->read(sizeof(b), b, false);
                if (n <= 0 || !s.upstream->write(n, b)) { break; }
            }
            s.closeBoth();
            down.join();
        }

        std::string m_user;
        std::string m_password;
        uint16_t m_port = 0;
        net::Listener m_listener;
        std::thread m_acceptor;
        std::atomic<bool> m_stop{false};
        mutable std::mutex m_mtx;
        std::vector<std::shared_ptr<Session>> m_sessions;
        std::vector<std::string> m_targets;
        std::atomic<int> m_tunnels{0};
        std::atomic<int> m_forwarded{0};
        std::atomic<int> m_rejected{0};
    };

}
