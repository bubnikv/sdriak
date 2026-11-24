// net::websock::WSClient against an in-process WebSocket server on loopback:
// echo of text, binary and a 300 kB message; a fragmented message with a PING
// between fragments (libcurl must answer it) and the close handshake;
// redirects (absolute and relative, path case kept, a redirect to the site
// root keeps the endpoint, the redirect cap); a
// refused upgrade and a refused connect; stop during the handshake; ten
// stop/start cycles on one client; sends from a second thread while frames
// stream in; a peer that drops TCP without a close frame.
//
// `websock_tests --live <url> [--kiwi]` instead opens one real connection
// (for wss:// checks); "{ts}" in the url is replaced by the time in ms.
#include <utils/base64.h>
#include <utils/networking.h>
#include <utils/proto/picohash.h>
#include <utils/proto/websock.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <functional>
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
                peer.conn->close();
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
}

int main(int argc, char** argv) {
    // Unbuffered, so the log up to a crash survives.
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc >= 3 && std::string(argv[1]) == "--live") {
        return live(argv[2], argc >= 4 && std::string(argv[3]) == "--kiwi");
    }

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
