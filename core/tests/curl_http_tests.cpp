// net::http::curlRequest / curlGet against an in-process HTTP server on
// loopback: GET, POST with a JSON body and Content-Type, another verb, a
// relative redirect (effectiveUrl and the origin a client derives from it),
// a non-200 status returned rather than thrown, the body cap, and
// shouldCancel ending a stalled response. Plus urlOrigin / urlPath.
// `curl_http_tests --directory` fetches the real KiwiSDR directory once.
#include <gui/brown/kiwisdr_directory.h>
#include <utils/networking.h>
#include <utils/proto/curl_http.h>

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
        return liveDirectory();
    }
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
