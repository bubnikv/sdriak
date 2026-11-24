// net::http::curlRequest / curlGet against an in-process HTTP server on
// loopback: GET, POST with a JSON body and Content-Type, another verb, a
// relative redirect (effectiveUrl and the origin a client derives from it),
// a non-200 status returned rather than thrown, the body cap, and
// shouldCancel ending a stalled response. Plus urlOrigin / urlPath.
// Proxy: settings parsing, GET/POST through an authenticating test proxy, a
// wrong password (407), noProxy, http_proxy honoured by system mode and
// ignored by none. Raw TCP (net::proxy::connect): an authenticated CONNECT
// tunnel, 407, noProxy, SOCKS refused, a silent proxy timing out. PAC: the
// target a script is asked about, and on Windows a script served on loopback
// evaluated through WinHTTP.
// `curl_http_tests --directory [--via-test-proxy]` fetches the real KiwiSDR
// directory once, optionally through the in-process authenticating proxy.
#include <gui/brown/kiwisdr_directory.h>
#include <utils/networking.h>
#include <utils/proto/curl_http.h>
#include <utils/proxy.h>

#include "test_http_proxy.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

namespace {
    using Clock = std::chrono::steady_clock;

    void require(bool condition, const std::string& message) {
        if (!condition) { throw std::runtime_error(message); }
    }

    long long msSince(Clock::time_point t) {
        return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t).count();
    }

    // A port block of its own, below the ephemeral ranges.
    net::Listener listenOnTestPort(uint16_t& port) {
        for (port = 24500; port < 24520; port++) {
            try { return net::listen("127.0.0.1", port); }
            catch (const std::exception&) {}
        }
        throw std::runtime_error("could not reserve a loopback test port");
    }

    struct Request {
        bool ok = false;
        std::string method;
        std::string path;
        std::string contentType;
        std::string body;
    };

    std::string lower(std::string s) {
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::tolower(c); });
        return s;
    }

    std::string headerValue(const std::string& head, const std::string& name) {
        const std::string l = lower(head);
        const size_t k = l.find("\r\n" + lower(name) + ":");
        if (k == std::string::npos) { return {}; }
        const size_t start = k + 3 + name.size();
        const size_t end = head.find("\r\n", start);
        std::string v = head.substr(start, end - start);
        v.erase(0, v.find_first_not_of(' '));
        return v;
    }

    Request readRequest(net::Conn& conn) {
        std::string buf;
        uint8_t tmp[4096];
        size_t end;
        while ((end = buf.find("\r\n\r\n")) == std::string::npos) {
            const int n = conn->read(sizeof(tmp), tmp, false);
            if (n <= 0) { return {}; }
            buf.append(reinterpret_cast<const char*>(tmp), (size_t)n);
        }
        Request r;
        const std::string head = buf.substr(0, end + 2);
        const size_t sp1 = head.find(' ');
        const size_t sp2 = head.find(' ', sp1 + 1);
        r.method = head.substr(0, sp1);
        r.path = head.substr(sp1 + 1, sp2 - sp1 - 1);
        r.contentType = headerValue(head, "Content-Type");
        const std::string len = headerValue(head, "Content-Length");
        const size_t want = len.empty() ? 0 : (size_t)std::stoul(len);
        r.body = buf.substr(end + 4);
        while (r.body.size() < want) {
            const int n = conn->read(sizeof(tmp), tmp, false);
            if (n <= 0) { return {}; }
            r.body.append(reinterpret_cast<const char*>(tmp), (size_t)n);
        }
        r.ok = true;
        return r;
    }

    void send(net::Conn& conn, const std::string& s) {
        conn->write((int)s.size(), reinterpret_cast<uint8_t*>(const_cast<char*>(s.data())));
    }

    void respond(net::Conn& conn, const std::string& status, const std::string& body,
                 const std::string& extraHeaders = "") {
        send(conn, "HTTP/1.1 " + status + "\r\n" + extraHeaders +
                   "Content-Length: " + std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body);
    }

    // Serves connections one after another; the handler sees each request.
    class Server {
    public:
        using Handler = std::function<void(net::Conn&, const Request&)>;

        explicit Server(Handler handler) : m_handler(std::move(handler)) {
            m_listener = listenOnTestPort(m_port);
            m_thread = std::thread([this] { run(); });
        }
        ~Server() {
            m_stop = true;
            try {
                auto wake = net::connect("127.0.0.1", m_port, 1000);
                if (wake) { wake->close(); }
            }
            catch (...) {}
            m_listener->close();
            m_thread.join();
        }
        std::string url(const std::string& path) const {
            return "http://127.0.0.1:" + std::to_string(m_port) + path;
        }

    private:
        void run() {
            for (;;) {
                net::Conn conn;
                try { conn = m_listener->accept(); }
                catch (...) { return; }
                if (m_stop || !conn) { return; }
                const Request req = readRequest(conn);
                if (req.ok) {
                    try { m_handler(conn, req); }
                    catch (...) {}
                }
                conn->close();
            }
        }

        Handler m_handler;
        net::Listener m_listener;
        uint16_t m_port = 0;
        std::thread m_thread;
        std::atomic<bool> m_stop{false};
    };

    // Echoes "METHOD path|content-type|body".
    void echo(net::Conn& c, const Request& r) {
        respond(c, "200 OK", r.method + " " + r.path + "|" + r.contentType + "|" + r.body);
    }

    void urlHelpers() {
        using net::http::urlOrigin;
        using net::http::urlPath;
        require(urlOrigin("https://hra.remoteqth.com:8073/") == "https://hra.remoteqth.com:8073", "origin with port");
        require(urlOrigin("http://h.example:80/x?y=1") == "http://h.example", "default http port dropped");
        require(urlOrigin("https://h.example:443") == "https://h.example", "default https port dropped");
        require(urlOrigin("http://[::1]:8073/VER") == "http://[::1]:8073", "IPv6 origin: " + urlOrigin("http://[::1]:8073/VER"));
        require(urlPath("https://h.example:1") == "/", "empty path is /");
        require(urlPath("http://h.example/VER?x") == "/VER", "path without query");
        require(urlOrigin("not a url").empty(), "garbage origin is empty");
    }

    void getAndPost() {
        Server srv(echo);
        auto get = net::http::curlGet(srv.url("/VER"));
        require(get.status == 200 && get.body == "GET /VER||", "GET: " + get.body);
        require(get.effectiveUrl == srv.url("/VER"), "GET effectiveUrl: " + get.effectiveUrl);

        net::http::CurlRequest post;
        post.method = "POST";
        post.url = srv.url("/connection");
        post.body = R"({"user_session_id":"abc"})";
        post.contentType = "application/json";
        auto pr = net::http::curlRequest(post);
        require(pr.status == 200 && pr.body == R"(POST /connection|application/json|{"user_session_id":"abc"})",
                "POST: " + pr.body);

        net::http::CurlRequest empty;
        empty.method = "POST";
        empty.url = srv.url("/empty");
        auto er = net::http::curlRequest(empty);
        // libcurl's default Content-Type for a POST without one is
        // application/x-www-form-urlencoded; only method and body matter here.
        require(er.body.rfind("POST /empty|", 0) == 0 && er.body.back() == '|', "empty POST: " + er.body);

        net::http::CurlRequest del;
        del.method = "DELETE";
        del.url = srv.url("/x");
        auto dr = net::http::curlRequest(del);
        require(dr.body == "DELETE /x||", "DELETE: " + dr.body);
    }

    void redirectAndStatus() {
        Server srv([](net::Conn& c, const Request& r) {
            if (r.path == "/VER") { respond(c, "302 Found", "", "Location: /\r\n"); }
            else if (r.path == "/") { respond(c, "200 OK", "<html>root</html>"); }
            else { respond(c, "404 Not Found", "nope"); }
        });
        auto res = net::http::curlGet(srv.url("/VER"));
        require(res.status == 200 && res.body == "<html>root</html>", "redirect: body " + res.body);
        require(net::http::urlPath(res.effectiveUrl) == "/", "redirect: effective path " + res.effectiveUrl);
        require(net::http::urlOrigin(res.effectiveUrl) == net::http::urlOrigin(srv.url("/")), "redirect: origin");

        auto missing = net::http::curlGet(srv.url("/missing"));
        require(missing.status == 404 && missing.body == "nope", "404 must be returned, not thrown");
    }

    void bodyCap() {
        Server srv([](net::Conn& c, const Request&) { respond(c, "200 OK", std::string(10000, 'x')); });
        net::http::CurlRequestOptions options;
        options.maxBody = 1000;
        bool threw = false;
        try { net::http::curlGet(srv.url("/big"), options); }
        catch (const std::exception& e) { threw = std::string(e.what()).find("maximum body size") != std::string::npos; }
        require(threw, "oversized body did not throw the size error");
    }

    void cancelStalled() {
        std::atomic<bool> release{false};
        Server srv([&](net::Conn& c, const Request&) {
            send(c, "HTTP/1.1 200 OK\r\nContent-Length: 100\r\n\r\npartial");
            while (!release) { std::this_thread::sleep_for(std::chrono::milliseconds(10)); }
        });
        std::atomic<bool> cancel{false};
        net::http::CurlRequestOptions options;
        options.timeoutMs = 20000;
        options.shouldCancel = [&] { return cancel.load(); };
        std::thread canceller([&] {
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            cancel = true;
        });
        const auto t0 = Clock::now();
        bool threw = false;
        try { net::http::curlGet(srv.url("/stall"), options); }
        catch (const std::exception&) { threw = true; }
        const long long ms = msSince(t0);
        canceller.join();
        release = true;
        require(threw, "cancelled transfer did not throw");
        require(ms < 2500, "cancel took " + std::to_string(ms) + " ms");
        std::printf("  cancel after 300 ms returned at %lld ms\n", ms);
    }
}

namespace {
    // Sets net::proxy for a scope and restores the previous setting.
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

    void setEnv(const char* name, const char* value) {
#ifdef _WIN32
        _putenv_s(name, value ? value : "");
#else
        if (value) { setenv(name, value, 1); }
        else { unsetenv(name); }
#endif
    }

    void proxyParsing() {
        using namespace net::proxy;
        require(normalizeUrl("proxy:3128") == "http://proxy:3128", "bare host:port");
        require(normalizeUrl("user:pw@h.example:8080") == "http://h.example:8080", "credentials stripped");
        require(normalizeUrl("socks5://[::1]:1080/") == "socks5://[::1]:1080", "IPv6 socks: " + normalizeUrl("socks5://[::1]:1080/"));
        require(normalizeUrl("h.example").empty() && normalizeUrl("h:0").empty() && normalizeUrl("[::1]").empty(),
                "no valid port must not parse");
        require(winInetProxyUrl("c.example:3") == "http://c.example:3", "WinINET plain");
        require(winInetProxyUrl("http=a.example:1;https=b.example:2") == "http://a.example:1", "WinINET http= first");
        require(winInetProxyUrl("https=b.example:2;socks=c.example:3") == "http://b.example:2", "WinINET https=");
        require(winInetProxyUrl("socks=c.example:3") == "socks4a://c.example:3", "WinINET socks=");
        const std::string np = winInetBypassToNoProxy("*.local; 192.168.*;<local>;10.1.*;host.lan;*foo*;*");
        require(np == ".local,192.168.0.0/16,localhost,127.0.0.1,::1,10.1.0.0/16,host.lan,*", "ProxyOverride: " + np);
        require(modeFromName(modeName(Mode::Manual)) == Mode::Manual && modeFromName("bogus") == Mode::System, "mode names");

        ProxyScope scope(manual("proxy.example:3128", "alice", "secret", "localhost"));
        const Effective e = effective();
        require(e.kind == Effective::Kind::Proxy && e.url == "http://proxy.example:3128" && e.user == "alice",
                "manual effective");
        require(e.description.find("secret") == std::string::npos, "description leaks the password");
        net::proxy::Config none;
        none.mode = Mode::None;
        net::proxy::setConfig(none);
        require(effective().kind == Effective::Kind::Direct, "none is direct");
    }

    void pacScript() {
        using net::proxy::detail::pacTarget;
        require(pacTarget("wss://User:pw@Hra.RemoteQTH.com:8073/kiwi/1/SND?x") == "https://hra.remoteqth.com:8073/",
                "wss target: " + pacTarget("wss://User:pw@Hra.RemoteQTH.com:8073/kiwi/1/SND?x"));
        require(pacTarget("ws://[::1]:8073") == "http://[::1]:8073/", "ws IPv6 target: " + pacTarget("ws://[::1]:8073"));
        require(pacTarget("http://h.example?q") == "http://h.example/", "query only: " + pacTarget("http://h.example?q"));
        require(pacTarget("").empty() && pacTarget("ftp://h/").empty() && pacTarget("h.example:80").empty(),
                "no target for other schemes");
#ifdef _WIN32
        const std::string script =
            "function FindProxyForURL(url, host) {\n"
            "  if (host == \"direct.example\") return \"DIRECT\";\n"
            "  if (shExpMatch(host, \"*.tunnel.example\")) return \"PROXY 10.1.2.3:3128; DIRECT\";\n"
            "  if (url.substring(0, 6) == \"https:\") return \"PROXY tls.example:8443; PROXY other.example:1\";\n"
            "  return \"PROXY p.example:8080\";\n"
            "}\n";
        Server srv([&script](net::Conn& c, const Request& r) {
            if (r.path == "/proxy.pac") { respond(c, "200 OK", script, "Content-Type: application/x-ns-proxy-autoconfig\r\n"); }
            else { respond(c, "404 Not Found", ""); }
        });
        auto ask = [&](const std::string& scriptUrl, const std::string& url, std::string& proxy) {
            std::string error;
            const bool ok = net::proxy::detail::evaluatePac(scriptUrl, false, pacTarget(url), proxy, error);
            if (!ok) { proxy = "error: " + error; }
            return ok;
        };
        std::string proxy;
        require(ask(srv.url("/proxy.pac"), "http://direct.example/x", proxy) && proxy.empty(), "DIRECT: " + proxy);
        require(ask(srv.url("/proxy.pac"), "ws://a.tunnel.example:8073/", proxy) && proxy == "http://10.1.2.3:3128",
                "first of a list: " + proxy);
        require(ask(srv.url("/proxy.pac"), "wss://k.example:8073/kiwi", proxy) && proxy == "http://tls.example:8443",
                "wss as https: " + proxy);
        require(ask(srv.url("/proxy.pac"), "http://k.example/", proxy) && proxy == "http://p.example:8080", "default: " + proxy);
        require(!ask(srv.url("/missing.pac"), "http://k.example/", proxy), "a missing script must fail: " + proxy);
#endif
    }

    void getAndPostThroughProxy() {
        Server srv(echo);
        test_proxy::HttpProxy proxy("alice", "secret", 24520);
        ProxyScope scope(manual(proxy.url(), "alice", "secret"));
        auto get = net::http::curlGet(srv.url("/VER"));
        require(get.status == 200 && get.body == "GET /VER||", "GET via proxy: " + std::to_string(get.status) + " " + get.body);
        net::http::CurlRequest post;
        post.method = "POST";
        post.url = srv.url("/connection");
        post.body = "{}";
        post.contentType = "application/json";
        auto pr = net::http::curlRequest(post);
        require(pr.body == "POST /connection|application/json|{}", "POST via proxy: " + pr.body);
        require(proxy.forwarded() >= 2, "requests did not go through the proxy: " + std::to_string(proxy.forwarded()));
    }

    void wrongProxyPassword() {
        Server srv(echo);
        test_proxy::HttpProxy proxy("alice", "secret", 24520);
        ProxyScope scope(manual(proxy.url(), "alice", "wrong"));
        auto res = net::http::curlGet(srv.url("/VER"));
        require(res.status == 407, "expected 407, got " + std::to_string(res.status));
        require(proxy.forwarded() == 0 && proxy.rejected() >= 1, "a rejected request reached the target");
    }

    void noProxyBypass() {
        Server srv(echo);
        test_proxy::HttpProxy proxy("", "", 24520);
        ProxyScope scope(manual(proxy.url(), "", "", "127.0.0.1"));
        auto res = net::http::curlGet(srv.url("/VER"));
        require(res.status == 200 && proxy.forwarded() == 0 && proxy.tunnels() == 0, "noProxy host went through the proxy");
    }

    void environmentAndNone() {
        Server srv(echo);
        test_proxy::HttpProxy proxy("", "", 24520);
        setEnv("http_proxy", proxy.url().c_str());
        setEnv("no_proxy", nullptr);
        setEnv("NO_PROXY", nullptr);
        net::proxy::Config none;
        none.mode = net::proxy::Mode::None;
        {
            ProxyScope scope(none);
            auto res = net::http::curlGet(srv.url("/VER"));
            require(res.status == 200 && proxy.forwarded() == 0, "mode none still used http_proxy");
        }
        net::proxy::Config system;
        system.mode = net::proxy::Mode::System;
        {
            ProxyScope scope(system);
            // With the URL: a PAC script or WPAD on this network answers first.
            const net::proxy::Effective e = net::proxy::effective(srv.url("/VER"));
            if (e.kind == net::proxy::Effective::Kind::Environment) {
                auto res = net::http::curlGet(srv.url("/VER"));
                require(res.status == 200 && proxy.forwarded() == 1, "mode system ignored http_proxy");
            }
            else {
                std::printf("  system mode resolves to '%s'; environment case skipped\n", e.description.c_str());
            }
        }
        setEnv("http_proxy", nullptr);
    }

    // A raw TCP echo server: reads, writes back; one connection at a time.
    class EchoServer {
    public:
        explicit EchoServer(bool silent = false) : m_silent(silent) {
            for (m_port = 24560; m_port < 24580; m_port++) {
                try {
                    m_listener = net::listen("127.0.0.1", m_port);
                    break;
                }
                catch (const std::exception&) {}
            }
            if (!m_listener) { throw std::runtime_error("echo: no free port"); }
            m_thread = std::thread([this] {
                for (;;) {
                    net::Conn c;
                    try { c = m_listener->accept(); }
                    catch (...) { return; }
                    if (m_stop || !c) { return; }
                    uint8_t b[4096];
                    for (;;) {
                        const int n = c->read(sizeof(b), b, false);
                        if (n <= 0) { break; }
                        if (!m_silent && !c->write(n, b)) { break; }
                    }
                }
            });
        }
        ~EchoServer() {
            m_stop = true;
            try {
                auto wake = net::connect("127.0.0.1", m_port, 1000);
                if (wake) { wake->close(); }
            }
            catch (...) {}
            m_listener->close();
            m_thread.join();
        }
        uint16_t port() const { return m_port; }

    private:
        bool m_silent;
        uint16_t m_port = 0;
        net::Listener m_listener;
        std::thread m_thread;
        std::atomic<bool> m_stop{false};
    };

    std::string echoOnce(net::Conn& c, const std::string& msg) {
        require(c->write((int)msg.size(), reinterpret_cast<uint8_t*>(const_cast<char*>(msg.data()))), "echo write");
        std::string got(msg.size(), '\0');
        require(c->read((int)got.size(), reinterpret_cast<uint8_t*>(got.data()), true) == (int)got.size(), "echo read");
        return got;
    }

    void noProxyMatching() {
        using net::proxy::noProxyMatches;
        require(noProxyMatches("*", "anything.example"), "*");
        require(noProxyMatches("example.com", "example.com") && noProxyMatches("example.com", "rx.example.com"), "domain");
        require(noProxyMatches(".example.com", "rx.example.com") && !noProxyMatches("example.com", "badexample.com"), "suffix");
        require(noProxyMatches(" localhost , 10.0.0.0/8", "10.20.30.40") && !noProxyMatches("10.0.0.0/8", "11.0.0.1"), "CIDR");
        require(noProxyMatches("192.168.1.5", "192.168.1.5") && !noProxyMatches("192.168.1.5", "192.168.1.50"), "exact IP");
        require(noProxyMatches("::1", "[::1]") && !noProxyMatches("", "localhost"), "IPv6 and empty");
    }

    void rawTcpThroughProxy() {
        EchoServer echoSrv;
        test_proxy::HttpProxy proxy("alice", "secret", 24520);
        {
            ProxyScope scope(manual(proxy.url(), "alice", "secret"));
            const auto route = net::proxy::tcpRoute("127.0.0.1");
            require(!route.direct && route.proxyHost == "127.0.0.1", "route: " + route.description + route.error);
            net::Conn c = net::proxy::connect("127.0.0.1", echoSrv.port(), 3000);
            require(echoOnce(c, "spyserver hello") == "spyserver hello", "tunnelled echo differs");
            c->close();
            require(proxy.tunnels() == 1 && proxy.rejected() == 0, "expected one authenticated CONNECT");
        }
        {
            ProxyScope scope(manual(proxy.url(), "alice", "wrong"));
            bool threw = false;
            try { net::proxy::connect("127.0.0.1", echoSrv.port(), 3000); }
            catch (const std::exception& e) { threw = std::string(e.what()).find("407") != std::string::npos; }
            require(threw, "wrong password must throw with the 407 status");
        }
        {
            ProxyScope scope(manual(proxy.url(), "", "", "127.0.0.0/8"));
            net::Conn c = net::proxy::connect("127.0.0.1", echoSrv.port(), 3000);
            require(echoOnce(c, "direct") == "direct", "bypassed echo differs");
            c->close();
            require(proxy.tunnels() == 1, "a noProxy host was tunnelled");
        }
        {
            ProxyScope scope(manual("socks5://127.0.0.1:1080"));
            bool threw = false;
            try { net::proxy::connect("127.0.0.1", echoSrv.port(), 1000); }
            catch (const std::exception& e) { threw = std::string(e.what()).find("socks5") != std::string::npos; }
            require(threw, "a SOCKS proxy must be refused for raw TCP");
        }
    }

    void rawTcpProxyTimeout() {
        EchoServer silentProxy(true); // accepts and reads, never answers
        ProxyScope scope(manual("http://127.0.0.1:" + std::to_string(silentProxy.port())));
        const auto t0 = Clock::now();
        bool threw = false;
        try { net::proxy::connect("example.invalid", 5555, 500); }
        catch (const std::exception& e) { threw = std::string(e.what()).find("no answer") != std::string::npos; }
        const long long ms = msSince(t0);
        require(threw, "silent proxy did not time out");
        require(ms < 2000, "CONNECT timeout took " + std::to_string(ms) + " ms");
    }

    // One real fetch of the KiwiSDR directory into an empty cache folder.
    int liveDirectory() {
        const auto dir = std::filesystem::temp_directory_path() /
            ("curl_http_tests_dir_" + std::to_string(Clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(dir);
        int rc = 1;
        {
            KiwiSDRDirectoryClient client(dir.string());
            const auto t0 = Clock::now();
            client.requestRefresh();
            while (client.isLoading() && msSince(t0) < 90000) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
            auto list = client.takeIfReady();
            std::printf("directory: %s after %lld ms, error '%s'\n",
                        list ? (std::to_string(list->size()) + " servers").c_str() : "no list",
                        msSince(t0), client.errorMessage().c_str());
            rc = (list && !list->empty()) ? 0 : 1;
        }
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
        return rc;
    }
}

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc >= 2 && std::string(argv[1]) == "--directory") {
        if (argc >= 3 && std::string(argv[2]) == "--via-test-proxy") {
            test_proxy::HttpProxy proxy("alice", "secret", 24540);
            ProxyScope scope(manual(proxy.url(), "alice", "secret"));
            const int rc = liveDirectory();
            std::printf("test proxy: %d forwarded, %d tunnels, %d answered 407\n",
                        proxy.forwarded(), proxy.tunnels(), proxy.rejected());
            return (rc == 0 && proxy.forwarded() + proxy.tunnels() > 0) ? 0 : 1;
        }
        return liveDirectory();
    }
    // Offline cases talk to loopback only: start without any proxy, whatever
    // this machine's settings are.
    net::proxy::Config direct;
    direct.mode = net::proxy::Mode::None;
    net::proxy::setConfig(direct);
    std::thread([] {
        std::this_thread::sleep_for(std::chrono::seconds(60));
        std::printf("FAIL (hang): whole test run exceeded 60 s\n");
        std::_Exit(2);
    }).detach();

    const std::pair<const char*, void (*)()> tests[] = {
        { "urlOrigin / urlPath", urlHelpers },
        { "GET, POST, other verb", getAndPost },
        { "redirect, non-200 status", redirectAndStatus },
        { "body cap", bodyCap },
        { "cancel a stalled response", cancelStalled },
        { "proxy settings parsing", proxyParsing },
        { "PAC script", pacScript },
        { "GET and POST through an authenticating proxy", getAndPostThroughProxy },
        { "wrong proxy password", wrongProxyPassword },
        { "noProxy bypass", noProxyBypass },
        { "environment proxy and mode none", environmentAndNone },
        { "noProxy matching", noProxyMatching },
        { "raw TCP through the proxy", rawTcpThroughProxy },
        { "raw TCP CONNECT timeout", rawTcpProxyTimeout },
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
