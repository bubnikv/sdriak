// net::websock::WSClient against an in-process WebSocket server on loopback:
// echo of text, binary and a 300 kB message; a fragmented message with a PING
// between fragments (libcurl must answer it) and the close handshake;
// redirects (absolute and relative, path case kept, a redirect to the site
// root keeps the endpoint, the redirect cap); a
// refused upgrade and a refused connect; stop during the handshake; ten
// stop/start cycles on one client; sends from a second thread while frames
// stream in; a peer that drops TCP without a close frame.
// KiwiSDRClient: SND packet parsing and refusal wording, then a fake Kiwi on
// loopback (/VER, the krec path, the setup order, the server's 20250 rate in
// SET AR OK and the passband, flagged and little-endian packets, a seq gap,
// ADC overload) and a fake Kiwi that answers too_busy.
//
// `websock_tests --live <url> [--kiwi]` instead opens one real connection
// (for wss:// checks); "{ts}" in the url is replaced by the time in ms.
// `websock_tests --kiwi-client host:port` runs KiwiSDRClient's whole connect
// path once: the /VER scheme probe, then the websocket, until IQ arrives;
// with --via-test-proxy after it, through the in-process authenticating proxy.
// `websock_tests --kiwi-waterfall host:port [kHz]` runs one session with the
// server waterfall: W/F first, then SND; zoom 0, then zoom 8 around the
// tuned frequency (default 10 000 kHz); prints the setup and each zoom's
// frames.
#include <utils/base64.h>
#include <utils/networking.h>
#include <utils/proto/picohash.h>
#include <utils/proto/kiwisdr.h>
#include <utils/proto/kiwisdr_wf.h>
#include <utils/proto/websock.h>
#include <utils/proxy.h>

#include "test_http_proxy.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {
    using Clock = std::chrono::steady_clock;
    using net::websock::WSClient;

    void require(bool condition, const std::string& message) {
        if (!condition) { throw std::runtime_error(message); }
    }

    [[noreturn]] void hang(const char* what) {
        std::fprintf(stderr, "FAIL (hang): %s\n", what);
        std::fflush(stderr);
        std::_Exit(2);
    }

    long long msSince(Clock::time_point t) {
        return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t).count();
    }

    // Ports below the Linux (32768+) and Windows (49152+) ephemeral ranges;
    // a different block from networking_tests so the two can run in parallel.
    net::Listener listenOnTestPort(uint16_t& port) {
        for (port = 24460; port < 24480; port++) {
            try { return net::listen("127.0.0.1", port); }
            catch (const std::exception&) {}
        }
        throw std::runtime_error("could not reserve a loopback test port");
    }

    // ---- minimal RFC 6455 server side ----

    struct Peer {
        net::Conn conn;
        std::string buf;

        bool fill() {
            uint8_t tmp[16384];
            const int n = conn->read(sizeof(tmp), tmp, false);
            if (n <= 0) { return false; }
            buf.append(reinterpret_cast<const char*>(tmp), (size_t)n);
            return true;
        }
        bool need(size_t n) {
            while (buf.size() < n) {
                if (!fill()) { return false; }
            }
            return true;
        }
        std::string take(size_t n) {
            std::string out = buf.substr(0, n);
            buf.erase(0, n);
            return out;
        }
        void send(const std::string& s) {
            conn->write((int)s.size(), reinterpret_cast<uint8_t*>(const_cast<char*>(s.data())));
        }
        // Blocks until the client closes the connection.
        void drainUntilClosed() {
            while (fill()) { buf.clear(); }
        }
    };

    struct Request {
        bool ok = false;
        std::string path;
        std::string key;
    };

    Request readRequest(Peer& p) {
        size_t end;
        while ((end = p.buf.find("\r\n\r\n")) == std::string::npos) {
            if (!p.fill()) { return {}; }
        }
        const std::string head = p.take(end + 4);
        Request r;
        const size_t sp1 = head.find(' ');
        const size_t sp2 = head.find(' ', sp1 + 1);
        if (sp1 == std::string::npos || sp2 == std::string::npos) { return {}; }
        r.path = head.substr(sp1 + 1, sp2 - sp1 - 1);
        std::string lower = head;
        std::transform(lower.begin(), lower.end(), lower.begin(),
                       [](unsigned char ch) { return (char)std::tolower(ch); });
        const std::string name = "sec-websocket-key:";
        const size_t k = lower.find(name);
        if (k != std::string::npos) {
            const size_t e = head.find("\r\n", k);
            std::string v = head.substr(k + name.size(), e - k - name.size());
            v.erase(0, v.find_first_not_of(' '));
            v.erase(v.find_last_not_of(' ') + 1);
            r.key = v;
        }
        r.ok = true;
        return r;
    }

    void accept101(Peer& p, const Request& r) {
        const std::string in = r.key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
        _picohash_sha1_ctx_t ctx;
        _picohash_sha1_init(&ctx);
        _picohash_sha1_update(&ctx, in.data(), in.size());
        uint8_t digest[PICOHASH_SHA1_DIGEST_LENGTH];
        _picohash_sha1_final(&ctx, digest);
        p.send("HTTP/1.1 101 Switching Protocols\r\n"
               "Upgrade: websocket\r\n"
               "Connection: Upgrade\r\n"
               "Sec-WebSocket-Accept: " + base64::encode(digest, sizeof(digest)) + "\r\n\r\n");
    }

    void respond(Peer& p, const std::string& statusLine, const std::string& extraHeaders = "") {
        p.send("HTTP/1.1 " + statusLine + "\r\n" + extraHeaders +
               "Content-Length: 0\r\nConnection: close\r\n\r\n");
    }

    // Server-to-client frame (unmasked).
    std::string frame(int opcode, const std::string& payload, bool fin = true) {
        std::string out;
        out.push_back((char)((fin ? 0x80 : 0x00) | (opcode & 0x0f)));
        const size_t n = payload.size();
        if (n <= 125) {
            out.push_back((char)n);
        }
        else if (n <= 0xffff) {
            out.push_back((char)126);
            out.push_back((char)(n >> 8));
            out.push_back((char)(n & 0xff));
        }
        else {
            out.push_back((char)127);
            for (int i = 7; i >= 0; i--) { out.push_back((char)((uint64_t)n >> (8 * i))); }
        }
        return out + payload;
    }

    struct Frame {
        int opcode = -1; // -1: connection closed
        bool fin = false;
        bool masked = false;
        std::string payload;
    };

    Frame readFrame(Peer& p) {
        if (!p.need(2)) { return {}; }
        const uint8_t b0 = (uint8_t)p.buf[0];
        const uint8_t b1 = (uint8_t)p.buf[1];
        uint64_t len = b1 & 0x7f;
        size_t pos = 2;
        if (len == 126) {
            if (!p.need(4)) { return {}; }
            len = ((uint64_t)(uint8_t)p.buf[2] << 8) | (uint8_t)p.buf[3];
            pos = 4;
        }
        else if (len == 127) {
            if (!p.need(10)) { return {}; }
            len = 0;
            for (int i = 0; i < 8; i++) { len = (len << 8) | (uint8_t)p.buf[2 + i]; }
            pos = 10;
        }
        const bool masked = (b1 & 0x80) != 0;
        if (!p.need(pos + (masked ? 4 : 0) + (size_t)len)) { return {}; }
        p.take(pos);
        const std::string mask = masked ? p.take(4) : std::string();
        Frame f;
        f.opcode = b0 & 0x0f;
        f.fin = (b0 & 0x80) != 0;
        f.masked = masked;
        f.payload = p.take((size_t)len);
        if (masked) {
            for (size_t i = 0; i < f.payload.size(); i++) { f.payload[i] ^= mask[i % 4]; }
        }
        return f;
    }

    // Accepts connections one after another and hands each to the handler.
    class Server {
    public:
        using Handler = std::function<void(Peer&, const Request&, int index)>;

        explicit Server(Handler handler) : m_handler(std::move(handler)) {
            m_listener = listenOnTestPort(m_port);
            m_thread = std::thread([this] { run(); });
        }

        ~Server() {
            m_stop = true;
            // Wake a blocked accept() with a throwaway connection.
            try {
                auto wake = net::connect("127.0.0.1", m_port, 1000);
                if (wake) { wake->close(); }
            }
            catch (...) {}
            m_listener->close();
            m_thread.join();
        }

        std::string url(const std::string& path) const {
            return "ws://127.0.0.1:" + std::to_string(m_port) + path;
        }
        uint16_t port() const { return m_port; }

        std::vector<std::string> paths() const {
            std::lock_guard<std::mutex> lock(m_mtx);
            return m_paths;
        }

        // Waits until `count` connections have been fully handled.
        bool waitHandled(int count, int ms) {
            std::unique_lock<std::mutex> lock(m_mtx);
            return m_cv.wait_for(lock, std::chrono::milliseconds(ms), [&] { return m_handled >= count; });
        }

        int handled() const {
            std::lock_guard<std::mutex> lock(m_mtx);
            return m_handled;
        }

        // Set by handlers when a client frame was not masked (RFC 6455 §5.1).
        std::atomic<bool> unmaskedFrame{false};

    private:
        void run() {
            for (int index = 0;;) {
                net::Conn conn;
                try { conn = m_listener->accept(); }
                catch (...) { return; }
                if (m_stop || !conn) { return; }
                Peer peer{ std::move(conn) };
                const Request req = readRequest(peer);
                if (req.ok) {
                    {
                        std::lock_guard<std::mutex> lock(m_mtx);
                        m_paths.push_back(req.path);
                    }
                    try { m_handler(peer, req, index); }
                    catch (...) {}
                    index++;
                }
                // A handler may take the connection to serve it on a thread.
                if (peer.conn) { peer.conn->close(); }
                if (req.ok) {
                    std::lock_guard<std::mutex> lock(m_mtx);
                    m_handled++;
                    m_cv.notify_all();
                }
            }
        }

        Handler m_handler;
        net::Listener m_listener;
        uint16_t m_port = 0;
        std::thread m_thread;
        std::atomic<bool> m_stop{false};
        mutable std::mutex m_mtx;
        std::condition_variable m_cv;
        std::vector<std::string> m_paths;
        int m_handled = 0;
    };

    // Echoes data frames until the client closes; answers a close frame.
    Server::Handler echoHandler(Server*& self) {
        return [&self](Peer& p, const Request& r, int) {
            accept101(p, r);
            for (;;) {
                Frame f = readFrame(p);
                if (f.opcode < 0) { return; }
                if (!f.masked && self) { self->unmaskedFrame = true; }
                if (f.opcode == 8) {
                    p.send(frame(8, f.payload));
                    return;
                }
                if (f.opcode == 1 || f.opcode == 2) { p.send(frame(f.opcode, f.payload)); }
            }
        };
    }

    // ---- client side ----

    struct Message {
        bool binary;
        std::string data;
        bool operator==(const Message& o) const { return binary == o.binary && data == o.data; }
    };

    struct Collector {
        std::mutex mtx;
        std::condition_variable cv;
        std::vector<Message> msgs;
        std::atomic<int> connected{0};
        std::atomic<int> disconnected{0};

        void attach(WSClient& ws) {
            ws.onTextMessage = [this](const std::string& s) { push(false, s); };
            ws.onBinaryMessage = [this](const std::string& s) { push(true, s); };
            ws.onConnected = [this] { markConnected(); };
            ws.onDisconnected = [this] {
                std::lock_guard<std::mutex> lock(mtx);
                disconnected++;
                cv.notify_all();
            };
        }
        void markConnected() {
            std::lock_guard<std::mutex> lock(mtx);
            connected++;
            cv.notify_all();
        }
        void push(bool binary, const std::string& s) {
            std::lock_guard<std::mutex> lock(mtx);
            msgs.push_back({ binary, s });
            cv.notify_all();
        }
        size_t count() {
            std::lock_guard<std::mutex> lock(mtx);
            return msgs.size();
        }
        // pred runs under mtx.
        bool waitFor(const std::function<bool()>& pred, int ms) {
            std::unique_lock<std::mutex> lock(mtx);
            return cv.wait_for(lock, std::chrono::milliseconds(ms), pred);
        }
    };

    class ClientRun {
    public:
        ClientRun() = default;
        ClientRun(const ClientRun&) = delete;
        ClientRun& operator=(const ClientRun&) = delete;
        // A failed check can leave before join(); a joinable std::thread
        // would call std::terminate here.
        ~ClientRun() {
            if (m_thread.joinable()) {
                m_ws->stopSocket();
                join(5000, "client thread still running after a failed check");
            }
        }

        void start(WSClient& ws, const std::string& url) {
            m_ws = &ws;
            m_done = false;
            m_error = nullptr;
            m_thread = std::thread([this, &ws, url] {
                try { ws.connectAndReceiveLoop(url); }
                catch (...) { m_error = std::current_exception(); }
                m_done = true;
            });
        }
        // Joins, or ends the process if the loop does not return in time:
        // a hang is the failure the deadline exists to catch.
        void join(int ms, const char* what) {
            const auto deadline = Clock::now() + std::chrono::milliseconds(ms);
            while (!m_done && Clock::now() < deadline) {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            if (!m_done) { hang(what); }
            m_thread.join();
        }
        bool done() const { return m_done; }
        bool failed() const { return m_error != nullptr; }
        std::string errorText() const {
            if (!m_error) { return {}; }
            try { std::rethrow_exception(m_error); }
            catch (const std::exception& e) { return e.what(); }
            catch (...) { return "unknown exception"; }
        }

    private:
        WSClient* m_ws = nullptr;
        std::thread m_thread;
        std::atomic<bool> m_done{false};
        std::exception_ptr m_error;
    };

    void requireNoError(const ClientRun& run, const std::string& test) {
        require(!run.failed(), test + ": unexpected exception: " + run.errorText());
    }

    // ---- tests ----

    void echoTextBinaryLarge() {
        Server* self = nullptr;
        Server srv(echoHandler(self));
        self = &srv;
        WSClient ws;
        Collector c;
        c.attach(ws);
        std::string big(300000, '\0');
        for (size_t i = 0; i < big.size(); i++) { big[i] = (char)(i * 7 + 3); }
        ws.onConnected = [&] {
            ws.sendString("hello");
            ws.sendBinary({ 0, 1, 2, 255 });
            ws.sendBinary(std::vector<uint8_t>(big.begin(), big.end()));
            c.markConnected();
        };
        ClientRun run;
        run.start(ws, srv.url("/echo"));
        require(c.waitFor([&] { return c.msgs.size() >= 3; }, 5000), "echo: not all messages came back");
        require(ws.effectiveUrl() == srv.url("/echo"), "echo: effectiveUrl is " + ws.effectiveUrl());
        ws.stopSocket();
        run.join(2000, "echo: loop did not stop");
        requireNoError(run, "echo");
        require(c.msgs[0] == Message{ false, "hello" }, "echo: text message differs");
        require(c.msgs[1] == Message{ true, std::string("\x00\x01\x02\xff", 4) }, "echo: binary message differs");
        require(c.msgs[2] == Message{ true, big }, "echo: 300 kB message differs");
        require(c.connected == 1 && c.disconnected == 1, "echo: connect/disconnect callbacks");
        require(ws.isStopped(), "echo: not stopped after the loop");
        srv.waitHandled(1, 2000);
        require(!srv.unmaskedFrame, "echo: client sent an unmasked frame");
    }

    void fragmentsPingAndClose() {
        std::mutex mtx;
        Frame pong, closeReply;
        Server srv([&](Peer& p, const Request& r, int) {
            accept101(p, r);
            p.send(frame(1, "frag-", false));
            p.send(frame(9, "pp"));            // control frame between fragments
            p.send(frame(0, "ment", false));
            p.send(frame(0, "ed", true));
            Frame f = readFrame(p);
            p.send(frame(2, std::string("\x10\x20", 2)));
            p.send(frame(8, std::string("\x03\xe8", 2) + "bye"));
            Frame g = readFrame(p);
            std::lock_guard<std::mutex> lock(mtx);
            pong = f;
            closeReply = g;
        });
        WSClient ws;
        Collector c;
        c.attach(ws);
        ClientRun run;
        run.start(ws, srv.url("/frag"));
        run.join(5000, "fragments: loop did not end on the server's close");
        requireNoError(run, "fragments");
        require(srv.waitHandled(1, 2000), "fragments: server handler did not finish");
        require(c.msgs.size() == 2, "fragments: expected 2 messages, got " + std::to_string(c.msgs.size()));
        require(c.msgs[0] == Message{ false, "frag-mented" }, "fragments: reassembled text is '" + c.msgs[0].data + "'");
        require(c.msgs[1] == Message{ true, std::string("\x10\x20", 2) }, "fragments: binary after fragments");
        std::lock_guard<std::mutex> lock(mtx);
        require(pong.opcode == 0xA && pong.payload == "pp", "fragments: no PONG with the PING payload");
        require(closeReply.opcode == 0x8, "fragments: no close reply");
        require(c.disconnected == 1 && ws.isStopped(), "fragments: disconnect state");
    }

    void redirectsKeepPathCase() {
        uint16_t port = 0;
        Server srv([&](Peer& p, const Request& r, int index) {
            if (r.path == "/Kiwi/1/SND" && index == 0) {
                // Same path, lower-cased, absolute, http scheme.
                respond(p, "302 Found", "Location: http://127.0.0.1:" + std::to_string(port) + "/kiwi/1/snd\r\n");
            }
            else if (r.path == "/Kiwi/1/SND") {
                respond(p, "301 Moved Permanently", "Location: /next\r\n"); // relative
            }
            else if (r.path == "/next") {
                accept101(p, r);
                p.send(frame(1, "landed"));
                p.drainUntilClosed();
            }
            else {
                respond(p, "404 Not Found");
            }
        });
        port = srv.port();
        WSClient ws;
        Collector c;
        c.attach(ws);
        ClientRun run;
        run.start(ws, srv.url("/Kiwi/1/SND"));
        require(c.waitFor([&] { return !c.msgs.empty(); }, 5000), "redirect: no message after redirects");
        const std::string eff = ws.effectiveUrl();
        ws.stopSocket();
        run.join(2000, "redirect: loop did not stop");
        requireNoError(run, "redirect");
        const auto paths = srv.paths();
        require(paths.size() == 3 && paths[0] == "/Kiwi/1/SND" && paths[1] == "/Kiwi/1/SND" && paths[2] == "/next",
                "redirect: request paths not /Kiwi/1/SND, /Kiwi/1/SND, /next");
        require(eff == srv.url("/next"), "redirect: effectiveUrl is " + eff);
        require(c.msgs[0] == Message{ false, "landed" }, "redirect: message differs");
    }

    void redirectToRootKeepsEndpoint() {
        uint16_t port = 0;
        Server srv([&](Peer& p, const Request& r, int index) {
            if (index == 0) {
                // hra.remoteqth.com style: endpoint -> https site root.
                respond(p, "302 Found", "Location: http://127.0.0.1:" + std::to_string(port) + "/\r\n");
            }
            else if (r.path == "/kiwi/7/SND?x=1") {
                accept101(p, r);
                p.send(frame(1, "kept"));
                p.drainUntilClosed();
            }
            else {
                respond(p, "200 OK"); // the site root page
            }
        });
        port = srv.port();
        WSClient ws;
        Collector c;
        c.attach(ws);
        ClientRun run;
        run.start(ws, srv.url("/kiwi/7/SND?x=1"));
        require(c.waitFor([&] { return !c.msgs.empty(); }, 5000), "root redirect: no message; error '" + run.errorText() + "'");
        ws.stopSocket();
        run.join(2000, "root redirect: loop did not stop");
        requireNoError(run, "root redirect");
        const auto paths = srv.paths();
        require(paths.size() == 2 && paths[1] == "/kiwi/7/SND?x=1", "root redirect: second request was not the endpoint");
        require(ws.effectiveUrl() == srv.url("/kiwi/7/SND?x=1"), "root redirect: effectiveUrl is " + ws.effectiveUrl());
    }

    void redirectCap() {
        Server srv([](Peer& p, const Request&, int) {
            respond(p, "302 Found", "Location: /loop\r\n");
        });
        WSClient ws;
        Collector c;
        c.attach(ws);
        ClientRun run;
        run.start(ws, srv.url("/start"));
        run.join(10000, "redirect cap: loop did not end");
        require(run.failed() && run.errorText().find("too many redirects") != std::string::npos,
                "redirect cap: expected 'too many redirects', got '" + run.errorText() + "'");
        srv.waitHandled(WSClient::kMaxRedirects + 1, 2000);
        require(srv.handled() == WSClient::kMaxRedirects + 1,
                "redirect cap: " + std::to_string(srv.handled()) + " requests");
        require(c.connected == 0 && c.disconnected == 0, "redirect cap: callbacks fired");
        require(ws.isStopped(), "redirect cap: not stopped");
    }

    void refusedUpgrade() {
        Server srv([](Peer& p, const Request&, int) { respond(p, "404 Not Found"); });
        WSClient ws;
        Collector c;
        c.attach(ws);
        ClientRun run;
        run.start(ws, srv.url("/nope"));
        run.join(5000, "refused upgrade: loop did not end");
        require(run.failed() && run.errorText().find("HTTP 404") != std::string::npos,
                "refused upgrade: expected 'HTTP 404', got '" + run.errorText() + "'");
        require(c.disconnected == 0 && ws.isStopped(), "refused upgrade: state");
    }

    void refusedConnect() {
        uint16_t port = 0;
        {
            auto l = listenOnTestPort(port);
            l->close();
        }
        WSClient ws;
        ClientRun run;
        const auto t0 = Clock::now();
        run.start(ws, "ws://127.0.0.1:" + std::to_string(port) + "/x");
        run.join(8000, "refused connect: loop did not end");
        require(run.failed() && run.errorText().find("connect failed") != std::string::npos,
                "refused connect: expected 'connect failed', got '" + run.errorText() + "'");
        require(msSince(t0) < 5000, "refused connect: took " + std::to_string(msSince(t0)) + " ms");
    }

    void stopDuringHandshake() {
        Server srv([](Peer& p, const Request&, int) { p.drainUntilClosed(); }); // never answers
        WSClient ws;
        Collector c;
        c.attach(ws);
        ClientRun run;
        run.start(ws, srv.url("/silent"));
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        const auto t0 = Clock::now();
        ws.stopSocket();
        run.join(4000, "stop during handshake: loop did not return");
        const long long ms = msSince(t0);
        requireNoError(run, "stop during handshake");
        require(ms < 2500, "stop during handshake: took " + std::to_string(ms) + " ms");
        require(c.connected == 0 && c.disconnected == 1, "stop during handshake: callbacks");
        std::printf("  stop during handshake: %lld ms\n", ms);
    }

    void stopStartTenTimes() {
        Server* self = nullptr;
        Server srv(echoHandler(self));
        self = &srv;
        WSClient ws;
        long long worst = 0;
        for (int i = 0; i < 10; i++) {
            Collector c;
            c.attach(ws);
            ws.reset();
            ClientRun run;
            run.start(ws, srv.url("/cycle"));
            require(c.waitFor([&] { return c.connected.load() == 1; }, 3000), "stop/start: no connect in cycle " + std::to_string(i));
            for (int k = 0; k < 5; k++) { ws.sendString("m" + std::to_string(k)); }
            require(c.waitFor([&] { return c.msgs.size() >= 5; }, 3000), "stop/start: echoes missing in cycle " + std::to_string(i));
            const auto t0 = Clock::now();
            ws.stopSocket();
            run.join(2000, "stop/start: loop did not stop");
            worst = std::max(worst, msSince(t0));
            requireNoError(run, "stop/start cycle " + std::to_string(i));
            require(c.disconnected == 1, "stop/start: onDisconnected count");
            bool threw = false;
            try { ws.sendString("late"); }
            catch (const std::exception&) { threw = true; }
            require(threw, "stop/start: send after the loop ended did not throw");
        }
        require(srv.waitHandled(10, 3000), "stop/start: server saw fewer than 10 sessions");
        std::printf("  stop/start x10: worst stop %lld ms\n", worst);
    }

    void sendWhileStreaming() {
        constexpr int kServerFrames = 3000;
        constexpr int kClientTexts = 500;
        std::atomic<int> textsInOrder{0};
        std::atomic<bool> outOfOrder{false};
        Server srv([&](Peer& p, const Request& r, int) {
            accept101(p, r);
            std::thread writer([&] {
                const std::string payload(1000, 'b');
                for (int i = 0; i < kServerFrames; i++) { p.send(frame(2, payload)); }
            });
            for (;;) {
                Frame f = readFrame(p);
                if (f.opcode < 0) { break; }
                if (f.opcode == 1) {
                    const int n = textsInOrder.load();
                    if (f.payload != "n" + std::to_string(n)) { outOfOrder = true; }
                    textsInOrder = n + 1;
                }
            }
            writer.join();
        });
        WSClient ws;
        Collector c;
        c.attach(ws);
        ClientRun run;
        run.start(ws, srv.url("/stream"));
        require(c.waitFor([&] { return c.connected.load() == 1; }, 3000), "stream: no connect");
        for (int i = 0; i < kClientTexts; i++) { ws.sendString("n" + std::to_string(i)); }
        require(c.waitFor([&] { return c.msgs.size() >= (size_t)kServerFrames; }, 10000),
                "stream: received " + std::to_string(c.count()) + " of " + std::to_string(kServerFrames) + " frames");
        const auto deadline = Clock::now() + std::chrono::seconds(5);
        while (textsInOrder < kClientTexts && Clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        ws.stopSocket();
        run.join(2000, "stream: loop did not stop");
        requireNoError(run, "stream");
        require(textsInOrder == kClientTexts, "stream: server got " + std::to_string(textsInOrder.load()) + " texts");
        require(!outOfOrder, "stream: texts arrived out of order or corrupted");
    }

    struct ProxyScope {
        net::proxy::Config saved;
        explicit ProxyScope(const net::proxy::Config& c) : saved(net::proxy::config()) { net::proxy::setConfig(c); }
        ~ProxyScope() { net::proxy::setConfig(saved); }
    };

    net::proxy::Config manualProxy(const std::string& url, const std::string& user, const std::string& password) {
        net::proxy::Config c;
        c.mode = net::proxy::Mode::Manual;
        c.url = url;
        c.user = user;
        c.password = password;
        return c;
    }

    // ws:// through an authenticating HTTP proxy goes as a CONNECT tunnel.
    void throughProxy() {
        Server* self = nullptr;
        Server srv(echoHandler(self));
        self = &srv;
        test_proxy::HttpProxy proxy("alice", "secret", 24520);
        ProxyScope scope(manualProxy(proxy.url(), "alice", "secret"));
        WSClient ws;
        Collector c;
        c.attach(ws);
        ws.onConnected = [&] {
            ws.sendString("via proxy");
            c.markConnected();
        };
        ClientRun run;
        run.start(ws, srv.url("/echo"));
        require(c.waitFor([&] { return !c.msgs.empty(); }, 5000), "proxy: no echo; error '" + run.errorText() + "'");
        ws.stopSocket();
        run.join(2000, "proxy: loop did not stop");
        requireNoError(run, "proxy");
        require(c.msgs[0] == Message{ false, "via proxy" }, "proxy: echo differs");
        require(proxy.tunnels() == 1 && proxy.forwarded() == 0,
                "proxy: expected one CONNECT, got " + std::to_string(proxy.tunnels()) + " tunnels, " +
                std::to_string(proxy.forwarded()) + " forwarded");
    }

    void peerDropsTcp() {
        Server srv([](Peer& p, const Request& r, int) {
            accept101(p, r);
            p.send(frame(1, "x"));
            // return: the connection closes with no close frame
        });
        WSClient ws;
        Collector c;
        c.attach(ws);
        ClientRun run;
        run.start(ws, srv.url("/drop"));
        run.join(5000, "peer drop: loop did not end");
        requireNoError(run, "peer drop");
        require(c.msgs.size() == 1 && c.msgs[0] == Message{ false, "x" }, "peer drop: message before the drop");
        require(c.disconnected == 1 && ws.isStopped(), "peer drop: state");
    }

    void sendBeforeConnectThrows() {
        WSClient ws;
        bool threw = false;
        try { ws.sendString("early"); }
        catch (const std::exception&) { threw = true; }
        require(threw, "send before connect did not throw");
    }

    int live(std::string url, bool kiwi) {
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        const size_t pos = url.find("{ts}");
        if (pos != std::string::npos) { url.replace(pos, 4, std::to_string(ms)); }
        WSClient ws;
        Collector c;
        c.attach(ws);
        ws.onConnected = [&] {
            ws.sendString(kiwi ? "SET auth t=kiwi p=#" : "websock_tests live");
            c.markConnected();
        };
        ClientRun run;
        const auto t0 = Clock::now();
        run.start(ws, url);
        const auto waitEnd = Clock::now() + std::chrono::seconds(15);
        while (Clock::now() < waitEnd && !run.done() &&
               !c.waitFor([&] { return !c.msgs.empty() || c.disconnected.load() > 0; }, 50)) {}
        const long long elapsed = msSince(t0);
        ws.stopSocket();
        run.join(5000, "live: loop did not stop");
        std::printf("url:       %s\neffective: %s\nconnected: %d  messages: %zu  first after %lld ms\n",
                    url.c_str(), ws.effectiveUrl().c_str(), c.connected.load(), c.msgs.size(), elapsed);
        if (!c.msgs.empty()) {
            const Message& m = c.msgs[0];
            std::string shown = m.data.substr(0, 100);
            for (char& ch : shown) { if ((unsigned char)ch < 0x20 || (unsigned char)ch > 0x7e) { ch = '.'; } }
            std::printf("first:     %s %zu bytes: %s\n", m.binary ? "binary" : "text", m.data.size(), shown.c_str());
        }
        if (run.failed()) { std::printf("error:     %s\n", run.errorText().c_str()); }
        return (!run.failed() && !c.msgs.empty()) ? 0 : 1;
    }

    // ---- KiwiSDRClient ----

    void putLE32(std::string& m, uint32_t v) {
        for (int i = 0; i < 4; i++) { m.push_back((char)(v >> (8 * i))); }
    }

    // A SND message: IQ (flag 0x08) or real, samples I = k, Q = -k.
    std::string sndPacket(uint8_t flags, uint32_t seq, uint16_t smeter, int count) {
        std::string m = "SND";
        m.push_back((char)flags);
        putLE32(m, seq);
        m.push_back((char)(smeter >> 8));
        m.push_back((char)(smeter & 0xff));
        const bool iq = (flags & KiwiSDRClient::kFlagIq) != 0;
        const bool le = (flags & KiwiSDRClient::kFlagLittleEndian) != 0;
        if (iq) {
            m.push_back((char)7);
            m.push_back((char)0);
            putLE32(m, 1000 + seq);
            putLE32(m, 500);
        }
        auto put16 = [&](int16_t v) {
            const uint16_t u = (uint16_t)v;
            if (le) { m.push_back((char)(u & 0xff)); m.push_back((char)(u >> 8)); }
            else { m.push_back((char)(u >> 8)); m.push_back((char)(u & 0xff)); }
        };
        for (int k = 0; k < count; k++) {
            put16((int16_t)k);
            if (iq) { put16((int16_t)-k); }
        }
        return m;
    }

    bool approxEqual(float a, float b) { return std::fabs(a - b) < 1e-6f; }

    void kiwiParseSnd() {
        KiwiSDRClient::SndPacket p;
        std::vector<dsp::complex_t> buf;
        require(KiwiSDRClient::parseSndPacket(sndPacket(0x0d, 7, 700, 512), p, buf), "IQ packet not parsed");
        require(p.iq && p.newFreq && !p.adcOverload && p.seq == 7 && p.count == 512, "IQ header: count " + std::to_string(p.count));
        require(approxEqual(p.rssiDbm, -57.0f) && p.lastGpsSolution == 7 && p.gpsSec == 1007 && p.gpsNsec == 500, "IQ S-meter / GPS fields");
        require(approxEqual(p.samples[5].re, 5 / 32767.0f) && approxEqual(p.samples[5].im, -5 / 32767.0f), "IQ big-endian samples");
        require(KiwiSDRClient::parseSndPacket(sndPacket(0x8a, 8, 0, 240), p, buf), "little-endian packet not parsed");
        require(p.count == 240 && p.adcOverload && !p.newFreq, "240-sample packet with overload");
        require(approxEqual(p.samples[239].re, 239 / 32767.0f) && approxEqual(p.samples[239].im, -239 / 32767.0f), "IQ little-endian samples");
        require(KiwiSDRClient::parseSndPacket(sndPacket(0x00, 9, 0, 512), p, buf), "real packet not parsed");
        require(!p.iq && p.count == 512 && approxEqual(p.samples[3].re, 3 / 32767.0f) && p.samples[3].im == 0.0f, "real samples");
        require(!KiwiSDRClient::parseSndPacket(sndPacket(0x18, 1, 0, 16), p, buf), "compressed packet accepted");
        require(!KiwiSDRClient::parseSndPacket(std::string("SND\x08\x01", 5), p, buf), "short packet accepted");
        require(!KiwiSDRClient::parseSndPacket(std::string("MSG audio_rate=12000"), p, buf), "MSG parsed as SND");
    }

    void kiwiRefusalText() {
        require(KiwiSDRClient::refusalText("too_busy", "4") == "server busy: all 4 channels are in use", "too_busy");
        require(KiwiSDRClient::refusalText("badp", "0").empty(), "badp=0 is not a refusal");
        require(KiwiSDRClient::refusalText("badp", "1").rfind("password required", 0) == 0, "badp=1");
        require(KiwiSDRClient::refusalText("badp", "9") == "server refused the connection (badp=9)", "unknown badp");
        require(!KiwiSDRClient::refusalText("monitor", "").empty() && !KiwiSDRClient::refusalText("down", "").empty(), "monitor / down");
        require(KiwiSDRClient::refusalText("audio_rate", "12000").empty(), "audio_rate is not a refusal");
        require(KiwiSDRClient::refusalText("ip_limit", "120,10.0.0.1") == "time limit for this address reached (120 min)", "ip_limit");
    }

    // A fake Kiwi: connection 0 is GET /VER, connection 1 the SND websocket,
    // handed to session(). Commands the client sends are collected.
    struct FakeKiwi {
        std::mutex mtx;
        std::vector<std::string> commands;
        std::function<void(Peer&, FakeKiwi&, const std::string& path)> session;
        // The W/F websocket, served on a thread of its own: the server takes
        // one connection at a time, and SND connects while W/F is open.
        std::function<void(Peer&, FakeKiwi&)> wfSession;
        std::vector<std::thread> wfThreads;

        ~FakeKiwi() {
            for (auto& t : wfThreads) { t.join(); }
        }

        // Reads client frames until one starts with prefix; false on close.
        bool waitCommand(Peer& p, const std::string& prefix) {
            for (;;) {
                Frame f = readFrame(p);
                if (f.opcode < 0) { return false; }
                if (f.opcode != 1 && f.opcode != 2) { continue; }
                {
                    std::lock_guard<std::mutex> lock(mtx);
                    commands.push_back(f.payload);
                }
                if (f.payload.rfind(prefix, 0) == 0) { return true; }
            }
        }
        void drain(Peer& p) {
            for (;;) {
                Frame f = readFrame(p);
                if (f.opcode < 0 || f.opcode == 8) { return; }
                std::lock_guard<std::mutex> lock(mtx);
                commands.push_back(f.payload);
            }
        }
        bool sent(const std::string& command) {
            std::lock_guard<std::mutex> lock(mtx);
            return std::find(commands.begin(), commands.end(), command) != commands.end();
        }
        int indexOf(const std::string& prefix) {
            std::lock_guard<std::mutex> lock(mtx);
            for (size_t i = 0; i < commands.size(); i++) {
                if (commands[i].rfind(prefix, 0) == 0) { return (int)i; }
            }
            return -1;
        }

        Server::Handler handler() {
            return [this](Peer& p, const Request& r, int index) {
                if (index == 0) {
                    const std::string body = "{\"maj\":1,\"min\":902,\"ts\":4611686213435808288}";
                    p.send("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " +
                           std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body);
                    return;
                }
                accept101(p, r);
                const std::string wf = "/W/F";
                if (wfSession && r.path.size() >= wf.size() && r.path.compare(r.path.size() - wf.size(), wf.size(), wf) == 0) {
                    auto owned = std::make_shared<Peer>(std::move(p));
                    wfThreads.emplace_back([this, owned] {
                        wfSession(*owned, *this);
                        owned->conn->close();
                    });
                    return;
                }
                session(p, *this, r.path);
            };
        }
    };

    void kiwiFakeSession() {
        FakeKiwi kiwi;
        kiwi.session = [](Peer& p, FakeKiwi& k, const std::string&) {
            if (!k.waitCommand(p, "SET compression=0")) { return; }
            p.send(frame(2, "MSG audio_init=0 audio_rate=20250"));
            if (!k.waitCommand(p, "SET AR OK")) { return; }
            p.send(frame(2, "MSG sample_rate=20250.880000 bandwidth=30000000"));
            if (!k.waitCommand(p, "SET mod=iq")) { return; }
            p.send(frame(2, sndPacket(0x0d, 0, 700, 512)));  // first packet: IQ + NEW_FREQ + LPF
            p.send(frame(2, sndPacket(0x08, 1, 700, 512)));
            p.send(frame(2, sndPacket(0x0a, 3, 700, 512)));  // seq 2 lost; ADC overload
            k.drain(p);
        };
        Server srv(kiwi.handler());
        KiwiSDRClient client;
        std::atomic<int> samples{0};
        std::atomic<int> packets{0};
        std::atomic<int> newFreqPackets{0};
        std::atomic<bool> failed{false};
        client.init("127.0.0.1:" + std::to_string(srv.port()));
        client.onConnected = [&] { client.tune(14074000.0, KiwiSDRClient::TUNE_IQ); };
        client.onError = [&](const std::string&) { failed = true; };
        client.onPacket = [&](const KiwiSDRClient::SndPacket& p) {
            packets++;
            if (p.newFreq) { newFreqPackets++; }
        };
        client.onSamples = [&](const dsp::complex_t*, int n) { samples += n; };
        client.start();
        const auto t0 = Clock::now();
        while (msSince(t0) < 5000 && samples.load() < 3 * 512 && !failed) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        const bool wasConnected = client.connected.load();
        const int gaps = (int)client.seqGaps();
        const bool overload = client.adcOverloadRecent();
        const auto flavour = client.flavour();
        const double rate = client.sampleRate();
        const int audio = client.audioRate();
        client.stop();

        require(!failed, "fake kiwi: onError fired");
        require(wasConnected, "fake kiwi: never connected");
        const auto paths = srv.paths();
        require(paths.size() >= 2 && paths[0] == "/VER" && paths[1] == "/4611686213435808288/SND",
                "fake kiwi: paths " + (paths.size() >= 2 ? paths[0] + " " + paths[1] : std::string("missing")));
        require(samples.load() == 3 * 512 && packets.load() == 3, "fake kiwi: samples " + std::to_string(samples.load()));
        require(newFreqPackets.load() == 1, "fake kiwi: NEW_FREQ packets " + std::to_string(newFreqPackets.load()));
        require(gaps == 1, "fake kiwi: seq gaps " + std::to_string(gaps));
        require(overload, "fake kiwi: ADC overload not reported");
        require(flavour == KiwiSDRClient::Flavour::KiwiSDR, "fake kiwi: flavour");
        require(audio == 20250 && std::fabs(rate - 20250.88) < 1e-6, "fake kiwi: rates " + std::to_string(audio) + " " + std::to_string(rate));
        require(kiwi.sent("SET AR OK in=20250 out=48000"), "fake kiwi: SET AR OK did not use the server's rate");
        require(kiwi.sent("SET mod=iq low_cut=-10025 high_cut=10025 freq=14074.000"), "fake kiwi: passband not from the rate");
        const int auth = kiwi.indexOf("SET auth"), ar = kiwi.indexOf("SET AR OK"), agc = kiwi.indexOf("SET agc="),
                  mod = kiwi.indexOf("SET mod=iq");
        require(auth >= 0 && auth < ar && ar < agc && agc < mod, "fake kiwi: setup order auth < AR OK < agc < mod");
    }

    void kiwiFakeTooBusy() {
        FakeKiwi kiwi;
        kiwi.session = [](Peer& p, FakeKiwi& k, const std::string&) {
            if (!k.waitCommand(p, "SET compression=0")) { return; }
            p.send(frame(2, "MSG too_busy=4"));
            k.drain(p);
        };
        Server srv(kiwi.handler());
        KiwiSDRClient client;
        std::mutex mtx;
        std::string error;
        std::atomic<bool> disconnected{false};
        client.init("127.0.0.1:" + std::to_string(srv.port()));
        client.onError = [&](const std::string& e) {
            std::lock_guard<std::mutex> lock(mtx);
            error = e;
        };
        client.onDisconnected = [&] { disconnected = true; };
        client.start();
        const auto t0 = Clock::now();
        while (msSince(t0) < 5000 && !disconnected) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        const std::string status = client.getConnectionStatus();
        client.stop();
        require(disconnected, "too_busy: the client did not close");
        {
            std::lock_guard<std::mutex> lock(mtx);
            require(error == "server busy: all 4 channels are in use", "too_busy: onError '" + error + "'");
        }
        require(client.getConnectionStatus() == "Refused: server busy: all 4 channels are in use",
                "too_busy: status after stop '" + client.getConnectionStatus() + "' (before: '" + status + "')");
        require(!client.connected, "too_busy: connected");
    }

    // A Kiwi that ignores the krec path: the client goes to /kiwi/ without
    // reporting a disconnect in between.
    void kiwiFakeSilentKrec() {
        FakeKiwi kiwi;
        kiwi.session = [](Peer& p, FakeKiwi& k, const std::string& path) {
            if (path.rfind("/kiwi/", 0) != 0) {
                k.drain(p);
                return;
            }
            if (!k.waitCommand(p, "SET compression=0")) { return; }
            p.send(frame(2, "MSG audio_init=0 audio_rate=12000"));
            if (!k.waitCommand(p, "SET AR OK")) { return; }
            p.send(frame(2, "MSG sample_rate=11998.902088"));
            if (!k.waitCommand(p, "SET mod=iq")) { return; }
            p.send(frame(2, sndPacket(0x0d, 0, 700, 512)));
            k.drain(p);
        };
        Server srv(kiwi.handler());
        KiwiSDRClient client;
        std::atomic<int> samples{0};
        std::atomic<int> disconnects{0};
        client.init("127.0.0.1:" + std::to_string(srv.port()));
        client.onConnected = [&] { client.tune(7074000.0, KiwiSDRClient::TUNE_IQ); };
        client.onDisconnected = [&] { disconnects++; };
        client.onSamples = [&](const dsp::complex_t*, int n) { samples += n; };
        client.start();
        const auto t0 = Clock::now();
        while (msSince(t0) < 8000 && samples.load() < 512) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        const int disconnectsWhileUp = disconnects.load();
        client.stop();
        const auto paths = srv.paths();
        require(paths.size() >= 3 && paths[1] == "/4611686213435808288/SND" && paths[2] == "/kiwi/4611686213435808288/SND",
                "silent krec: paths " + std::to_string(paths.size()));
        require(samples.load() == 512, "silent krec: samples " + std::to_string(samples.load()));
        require(disconnectsWhileUp == 0, "silent krec: onDisconnected fired before the fallback streamed");
        require(kiwi.sent("SET auth t=kiwi p=#"), "silent krec: web UI auth form on /kiwi/");
    }

    // ---- KiwiWaterfallClient ----

    // A W/F message: x_bin, zoom, seq, then 1 024 bins of one value.
    std::string wfPacket(uint32_t xBin, uint32_t zoom, uint32_t seq, uint8_t value) {
        std::string m = "W/F";
        m.push_back('\0');
        putLE32(m, xBin);
        putLE32(m, zoom);
        putLE32(m, seq);
        m.append((size_t)KiwiWaterfallClient::kBins, (char)value);
        return m;
    }

    void kiwiParseWf() {
        KiwiWaterfallClient::Frame f;
        const std::string msg = wfPacket(82399, 10 | KiwiWaterfallClient::kFlagCompressed, 77, 200);
        require(KiwiWaterfallClient::parseFrame(msg, f), "W/F frame not parsed");
        require(f.xBin == 82399 && f.zoom == 10 && f.seq == 77 && f.count == 1024, "W/F header");
        require((f.flags & KiwiWaterfallClient::kFlagCompressed) != 0, "W/F compressed flag");
        require(!KiwiWaterfallClient::parseFrame(std::string("W/F\0\1\2", 6), f), "short W/F accepted");
        require(!KiwiWaterfallClient::parseFrame(sndPacket(0x08, 1, 0, 16), f), "SND parsed as W/F");

        KiwiWaterfallClient::Setup setup;  // 30 MHz, zoom_max 14, wf_cal -13
        // research 8.3: zoom 10, x_bin 82 399 is 147.34 kHz.
        require(std::fabs(KiwiWaterfallClient::frameStartHz(setup, 82399) - 147340.0) < 10.0, "W/F start");
        require(KiwiWaterfallClient::zoomSpanHz(setup, 0) == 30e6 && KiwiWaterfallClient::zoomSpanHz(setup, 3) == 3.75e6, "W/F span");
        require(approxEqual(KiwiWaterfallClient::binDbm(setup, 200), -68.0f), "W/F dBm");
        setup.bandwidthHz = 32e6;
        require(KiwiWaterfallClient::zoomSpanHz(setup, 1) == 16e6, "W/F span from the reported bandwidth");
    }

    // SND as on a real Kiwi, short: rates, then one IQ packet per tune.
    void fakeSndSession(Peer& p, FakeKiwi& k) {
        if (!k.waitCommand(p, "SET compression=0")) { return; }
        p.send(frame(2, "MSG audio_init=0 audio_rate=12000"));
        if (!k.waitCommand(p, "SET AR OK")) { return; }
        p.send(frame(2, "MSG sample_rate=11998.902088 bandwidth=30000000"));
        if (!k.waitCommand(p, "SET mod=iq")) { return; }
        p.send(frame(2, sndPacket(0x0d, 0, 700, 512)));
        k.drain(p);
    }

    // W/F opens first, gets its setup and the view commands, delivers
    // frames; SND follows on the same ts.
    void kiwiFakeWaterfall() {
        FakeKiwi kiwi;
        kiwi.wfSession = [](Peer& p, FakeKiwi& k) {
            if (!k.waitCommand(p, "SERVER DE CLIENT sdriak W/F")) { return; }
            p.send(frame(2, "MSG center_freq=15000000 bandwidth=30000000 adc_clk_nom=66666600"));
            p.send(frame(2, "MSG wf_fft_size=1024 wf_fps=23 wf_fps_max=23 zoom_max=14 zoom_cap=11 rx_chans=4 wf_chans=4 wf_chans_real=4 wf_cal=-13 wf_setup"));
            if (!k.waitCommand(p, "SET zoom=")) { return; }
            for (uint32_t i = 0; i < 3; i++) { p.send(frame(2, wfPacket(82399, 10, 5 + i, 200))); }
            k.drain(p);
        };
        kiwi.session = [](Peer& p, FakeKiwi& k, const std::string&) { fakeSndSession(p, k); };
        Server srv(kiwi.handler());

        KiwiWaterfallClient wf;
        std::atomic<int> frames{0};
        std::atomic<bool> frameOk{true};
        wf.onFrame = [&](const KiwiWaterfallClient::Frame& f) {
            if (f.zoom != 10 || f.xBin != 82399 || f.count != 1024 || f.bins[0] != 200) { frameOk = false; }
            frames++;
        };
        KiwiSDRClient client;
        std::atomic<int> samples{0};
        client.init("127.0.0.1:" + std::to_string(srv.port()));
        client.waterfall = &wf;
        client.waterfallSpeed = 3;
        client.waterfallCfKhz = 14074.0;
        client.onConnected = [&] { client.tune(14074000.0, KiwiSDRClient::TUNE_IQ); };
        client.onSamples = [&](const dsp::complex_t*, int n) { samples += n; };
        client.start();
        const auto t0 = Clock::now();
        while (msSince(t0) < 8000 && (samples.load() < 512 || frames.load() < 3)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        const bool available = wf.available();
        const auto setup = wf.setup();
        client.stop();

        const auto paths = srv.paths();
        require(paths.size() >= 3 && paths[1] == "/4611686213435808288/W/F" && paths[2] == "/4611686213435808288/SND",
                "waterfall: paths " + (paths.size() >= 3 ? paths[1] + " " + paths[2] : std::string("missing")));
        require(available, "waterfall: not available");
        require(frames.load() == 3 && frameOk, "waterfall: frames " + std::to_string(frames.load()));
        require(samples.load() == 512, "waterfall: SND samples " + std::to_string(samples.load()));
        require(setup.zoomMax == 14 && setup.zoomCap == 11 && setup.wfChans == 4 && setup.wfCalDb == -13.0 && setup.bandwidthHz == 30e6,
                "waterfall: setup fields");
        require(kiwi.sent("SET auth t=kiwi p=") && kiwi.sent("SET wf_speed=3") && kiwi.sent("SET interp=13") &&
                kiwi.sent("SET wf_comp=0") && kiwi.sent("SET zoom=0 cf=14074.000"), "waterfall: setup commands");
        require(!wf.available(), "waterfall: still available after stop");
    }

    // No waterfall channel: the session goes on with SND alone.
    void kiwiFakeWaterfallBusy() {
        FakeKiwi kiwi;
        kiwi.wfSession = [](Peer& p, FakeKiwi& k) {
            if (!k.waitCommand(p, "SERVER DE CLIENT sdriak W/F")) { return; }
            p.send(frame(2, "MSG too_busy=4"));
            k.drain(p);
        };
        kiwi.session = [](Peer& p, FakeKiwi& k, const std::string&) { fakeSndSession(p, k); };
        Server srv(kiwi.handler());

        KiwiWaterfallClient wf;
        KiwiSDRClient client;
        std::atomic<int> samples{0};
        std::atomic<bool> failed{false};
        client.init("127.0.0.1:" + std::to_string(srv.port()));
        client.waterfall = &wf;
        client.onConnected = [&] { client.tune(14074000.0, KiwiSDRClient::TUNE_IQ); };
        client.onError = [&](const std::string&) { failed = true; };
        client.onSamples = [&](const dsp::complex_t*, int n) { samples += n; };
        client.start();
        const auto t0 = Clock::now();
        while (msSince(t0) < 8000 && samples.load() < 512) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        const std::string reason = wf.unavailableReason();
        client.stop();
        require(samples.load() == 512 && !failed, "waterfall busy: SND did not carry on");
        require(!wf.available(), "waterfall busy: available");
        require(reason == "server busy: all 4 channels are in use", "waterfall busy: reason '" + reason + "'");
    }

    int kiwiClient(const std::string& hostPort) {
        KiwiSDRClient client;
        std::atomic<int> samples{0};
        std::atomic<bool> connected{false};
        std::atomic<bool> failed{false};
        client.onConnected = [&] { connected = true; };
        client.onError = [&](const std::string&) { failed = true; };
        client.onSamples = [&](const dsp::complex_t*, int n) { samples += n; };
        client.init(hostPort);
        const auto t0 = Clock::now();
        client.start();
        client.tune(14074000.0, KiwiSDRClient::TUNE_IQ);
        bool tuned = false;
        while (msSince(t0) < 15000 && samples.load() < 12000 && !failed) {
            if (connected && !tuned) {
                client.tune(14074000.0, KiwiSDRClient::TUNE_IQ);
                tuned = true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        const long long elapsed = msSince(t0);
        const std::string status = client.getConnectionStatus();
        const std::string eff = client.wsClient.effectiveUrl();
        const char* flavour = client.flavour() == KiwiSDRClient::Flavour::KiwiSDR          ? "KiwiSDR"
                              : client.flavour() == KiwiSDRClient::Flavour::KiwiCompatible ? "Kiwi-compatible"
                                                                                           : "unknown";
        std::printf("host:      %s\neffective: %s\nconnected: %d  samples: %d  after %lld ms\nstatus:    %s\n"
                    "server:    %s, audio_rate %d, sample_rate %.6f, seq gaps %llu, RSSI %.1f dBm\n",
                    hostPort.c_str(), eff.c_str(), (int)connected.load(), samples.load(), elapsed, status.c_str(),
                    flavour, client.audioRate(), client.sampleRate(), (unsigned long long)client.seqGaps(), client.rssiDbm());
        client.stop();
        return (connected && samples.load() > 0) ? 0 : 1;
    }
}

namespace {
    int kiwiWaterfall(const std::string& hostPort, double tuneKhz) {
        struct ZoomStats {
            int frames = 0;
            uint32_t xBin = 0;
            int peakBin = 0;
            float peakDbm = -999.0f;
            double meanDbm = 0.0;
        };
        std::mutex mtx;
        std::map<int, ZoomStats> stats;
        KiwiWaterfallClient wf;
        wf.onFrame = [&](const KiwiWaterfallClient::Frame& f) {
            const auto setup = wf.setup();
            std::lock_guard<std::mutex> lock(mtx);
            ZoomStats& z = stats[f.zoom];
            z.frames++;
            z.xBin = f.xBin;
            double sum = 0.0;
            for (int i = 0; i < f.count; i++) {
                const float dbm = KiwiWaterfallClient::binDbm(setup, f.bins[i]);
                sum += dbm;
                if (dbm > z.peakDbm) { z.peakDbm = dbm; z.peakBin = i; }
            }
            z.meanDbm = sum / f.count;
        };
        KiwiSDRClient client;
        std::atomic<int> samples{0};
        std::atomic<bool> connected{false};
        client.init(hostPort);
        client.waterfall = &wf;
        client.waterfallCfKhz = tuneKhz;
        client.onConnected = [&] { connected = true; };
        client.onSamples = [&](const dsp::complex_t*, int n) { samples += n; };
        client.start();
        const auto t0 = Clock::now();
        bool tuned = false, zoomed = false;
        while (msSince(t0) < 14000) {
            if (connected && !tuned) {
                client.tune(tuneKhz * 1e3, KiwiSDRClient::TUNE_IQ);
                tuned = true;
            }
            if (tuned && !zoomed && msSince(t0) > 6000) {
                wf.setView(8, tuneKhz - client.frequencyOffsetHz() / 1e3);
                zoomed = true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        const auto setup = wf.setup();
        std::printf("host:      %s\nwaterfall: %s%s\n", hostPort.c_str(), wf.available() ? "available" : "unavailable ",
                    wf.unavailableReason().c_str());
        std::printf("setup:     zoom_max %d zoom_cap %d wf_chans %d wf_cal %.1f bandwidth %.0f\n",
                    setup.zoomMax, setup.zoomCap, setup.wfChans, setup.wfCalDb, setup.bandwidthHz);
        std::printf("snd:       connected %d, %d samples, RSSI %.1f dBm, freq_offset %lld Hz\n", (int)connected.load(),
                    samples.load(), client.rssiDbm(), (long long)client.frequencyOffsetHz());
        {
            std::lock_guard<std::mutex> lock(mtx);
            for (const auto& [zoom, z] : stats) {
                const double start = KiwiWaterfallClient::frameStartHz(setup, z.xBin);
                const double span = KiwiWaterfallClient::zoomSpanHz(setup, zoom);
                std::printf("zoom %2d:   %d frames, start %.3f kHz span %.3f kHz, mean %.1f dBm, peak %.1f dBm at %.3f kHz\n",
                            zoom, z.frames, start / 1e3, span / 1e3, z.meanDbm, z.peakDbm,
                            (start + (z.peakBin + 0.5) * span / KiwiWaterfallClient::kBins) / 1e3);
            }
        }
        const bool ok = connected && samples.load() > 0 && !stats.empty();
        client.stop();
        return ok ? 0 : 1;
    }
}

int main(int argc, char** argv) {
    // Unbuffered, so the log up to a crash survives.
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc >= 3 && std::string(argv[1]) == "--kiwi-client") {
        if (argc >= 4 && std::string(argv[3]) == "--via-test-proxy") {
            test_proxy::HttpProxy proxy("alice", "secret", 24540);
            ProxyScope scope(manualProxy(proxy.url(), "alice", "secret"));
            const int rc = kiwiClient(argv[2]);
            std::printf("test proxy: %d tunnels, %d forwarded, %d answered 407\n",
                        proxy.tunnels(), proxy.forwarded(), proxy.rejected());
            for (const auto& t : proxy.targets()) { std::printf("  %s\n", t.c_str()); }
            return (rc == 0 && proxy.tunnels() > 0) ? 0 : 1;
        }
        return kiwiClient(argv[2]);
    }
    if (argc >= 3 && std::string(argv[1]) == "--kiwi-waterfall") {
        return kiwiWaterfall(argv[2], argc >= 4 ? std::atof(argv[3]) : 10000.0);
    }
    if (argc >= 3 && std::string(argv[1]) == "--live") {
        return live(argv[2], argc >= 4 && std::string(argv[3]) == "--kiwi");
    }

    // Offline cases talk to loopback only: start without any proxy, whatever
    // this machine's settings are.
    net::proxy::Config direct;
    direct.mode = net::proxy::Mode::None;
    net::proxy::setConfig(direct);

    std::thread([] {
        std::this_thread::sleep_for(std::chrono::seconds(120));
        hang("whole test run exceeded 120 s");
    }).detach();

    const std::pair<const char*, void (*)()> tests[] = {
        { "send before connect", sendBeforeConnectThrows },
        { "echo text/binary/300 kB", echoTextBinaryLarge },
        { "fragments, ping, close", fragmentsPingAndClose },
        { "redirects keep path case", redirectsKeepPathCase },
        { "redirect to site root", redirectToRootKeepsEndpoint },
        { "redirect cap", redirectCap },
        { "refused upgrade", refusedUpgrade },
        { "refused connect", refusedConnect },
        { "stop during handshake", stopDuringHandshake },
        { "stop/start x10", stopStartTenTimes },
        { "send while streaming", sendWhileStreaming },
        { "peer drops TCP", peerDropsTcp },
        { "through an authenticating proxy", throughProxy },
        { "kiwi: SND packet parsing", kiwiParseSnd },
        { "kiwi: refusal text", kiwiRefusalText },
        { "kiwi: fake server session", kiwiFakeSession },
        { "kiwi: fake server too_busy", kiwiFakeTooBusy },
        { "kiwi: silent krec path falls back to /kiwi/", kiwiFakeSilentKrec },
        { "kiwi: parse W/F frame", kiwiParseWf },
        { "kiwi: fake server waterfall", kiwiFakeWaterfall },
        { "kiwi: fake server waterfall busy", kiwiFakeWaterfallBusy },
    };
    int failures = 0;
    for (const auto& [name, fn] : tests) {
        const auto t0 = Clock::now();
        try {
            fn();
            std::printf("PASS %s (%lld ms)\n", name, msSince(t0));
        }
        catch (const std::exception& e) {
            std::printf("FAIL %s: %s\n", name, e.what());
            failures++;
        }
        std::fflush(stdout);
    }
    return failures == 0 ? 0 : 1;
}
