/*
 * Adapted from the SDRPP distribution (https://github.com/qrp73/SDRPP).
 * Copyright (c) 2025 qrp73.
 * Originally ported from SDRPlusPlusBrown (https://github.com/sannysanoff/SDRPlusPlusBrown).
 * Modified for SDRIAK, 2026.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, version 3.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */
#include "websock.h"

#include <algorithm>
#include <chrono>
#include <stdexcept>

#include <curl/curl.h>

#ifndef _WIN32
#include <cerrno>
#include <poll.h>
#endif

#include "../ascii.h"
#include <utils/curl_init.h>
#include <utils/flog.h>

namespace net::websock {

    struct WSClient::Connection {
        CURL* curl = nullptr;
        curl_slist* headers = nullptr;
        char errbuf[CURL_ERROR_SIZE] = {};

        Connection() = default;
        ~Connection() {
            if (curl) { curl_easy_cleanup(curl); }
            if (headers) { curl_slist_free_all(headers); }
        }
        Connection(const Connection&) = delete;
        Connection& operator=(const Connection&) = delete;

        std::string errorText(CURLcode rc) const {
            return errbuf[0] ? std::string(errbuf) : std::string(curl_easy_strerror(rc));
        }
    };

    struct WSClient::Assembly {
        bool active = false;
        bool binary = false;
        std::string data;
    };

    struct WSClient::Message {
        bool binary;
        std::string data;
    };

    namespace {

        using Clock = std::chrono::steady_clock;

        constexpr int    kHandshakeTimeoutMs = 10000;
        constexpr int    kPollSliceMs        = 100;
        constexpr int    kSendTimeoutMs      = 5000;
        constexpr int    kSendWaitSliceMs    = 10;
        constexpr size_t kRecvChunk          = 64 * 1024;
        // Bytes one drainLocked() call reads before it hands the messages to
        // the callbacks and ticks, so a flooding peer cannot starve them.
        constexpr size_t kDrainBudget        = 256 * 1024;

        // Waits until sock is readable (or writable). Returns >0 when ready,
        // 0 on timeout, <0 on error.
        int waitSocket(curl_socket_t sock, bool forWrite, int timeoutMs) {
            if (sock == CURL_SOCKET_BAD) { return -1; }
#ifdef _WIN32
            fd_set set;
            FD_ZERO(&set);
            FD_SET(sock, &set);
            timeval tv;
            tv.tv_sec = timeoutMs / 1000;
            tv.tv_usec = (timeoutMs % 1000) * 1000;
            return select(0, forWrite ? nullptr : &set, forWrite ? &set : nullptr, nullptr, &tv);
#else
            pollfd p{};
            p.fd = sock;
            p.events = forWrite ? POLLOUT : POLLIN;
            const int r = poll(&p, 1, timeoutMs);
            return (r < 0 && errno == EINTR) ? 0 : r;
#endif
        }

        // http:// -> ws://, https:// -> wss://; anything else unchanged.
        std::string toWsScheme(const std::string& url) {
            if (ascii::startsWithIgnoreCase(url, "https://")) { return "wss://" + url.substr(8); }
            if (ascii::startsWithIgnoreCase(url, "http://"))  { return "ws://" + url.substr(7); }
            return url;
        }

        class UrlHandle {
        public:
            explicit UrlHandle(const std::string& url) : m_url(curl_url()) {
                m_ok = m_url && curl_url_set(m_url, CURLUPART_URL, url.c_str(), CURLU_NON_SUPPORT_SCHEME) == CURLUE_OK;
            }
            ~UrlHandle() { curl_url_cleanup(m_url); }
            UrlHandle(const UrlHandle&) = delete;
            UrlHandle& operator=(const UrlHandle&) = delete;

            bool ok() const { return m_ok; }

            std::string get(CURLUPart part, unsigned int flags = 0) const {
                char* value = nullptr;
                if (!m_ok || curl_url_get(m_url, part, &value, flags) != CURLUE_OK || !value) { return {}; }
                std::string out(value);
                curl_free(value);
                return out;
            }

            bool set(CURLUPart part, const std::string& value, unsigned int flags = 0) {
                return m_ok && curl_url_set(m_url, part, value.c_str(), flags) == CURLUE_OK;
            }

        private:
            CURLU* m_url;
            bool m_ok = false;
        };

        // Origin for the upgrade request: the http(s) form of the URL's
        // host, with the port only when it is not the scheme default.
        std::string originFor(const std::string& wsUrl) {
            UrlHandle u(wsUrl);
            const bool tls = ascii::equalsIgnoreCase(u.get(CURLUPART_SCHEME), "wss");
            std::string origin = (tls ? "https://" : "http://") + u.get(CURLUPART_HOST);
            const std::string port = u.get(CURLUPART_PORT, CURLU_NO_DEFAULT_PORT);
            if (!port.empty() && port != (tls ? "443" : "80")) { origin += ":" + port; }
            return origin;
        }

        // The ws(s) URL to retry after a redirect from currentUrl to the
        // Location value (absolute or relative). Keeps currentUrl's path when
        // the two differ only in case: some servers lower-case it, and the
        // KiwiSDR stream path (/kiwi/<ts>/SND) is case-sensitive.
        std::string redirectTarget(const std::string& currentUrl, const std::string& location) {
            UrlHandle cur(currentUrl);
            UrlHandle nxt(currentUrl);
            // Setting a URL on a handle that holds one resolves it relative
            // to the old one.
            if (!cur.ok() || !nxt.set(CURLUPART_URL, location, CURLU_NON_SUPPORT_SCHEME)) {
                return toWsScheme(location);
            }
            std::string next = toWsScheme(nxt.get(CURLUPART_URL));
            const std::string curPath = cur.get(CURLUPART_PATH);
            const std::string nextPath = nxt.get(CURLUPART_PATH);
            const bool nextIsRoot = (nextPath.empty() || nextPath == "/") && nxt.get(CURLUPART_QUERY).empty();
            if (nextIsRoot && !curPath.empty() && curPath != "/" && nxt.set(CURLUPART_PATH, curPath)) {
                // A redirect of the endpoint to the site root is a "use
                // https" redirect, not a new endpoint: hra.remoteqth.com
                // answers ws://host:8073/kiwi/<ts>/SND with
                // 302 -> https://host:8073/. Keep the endpoint path and query.
                const std::string curQuery = cur.get(CURLUPART_QUERY);
                if (!curQuery.empty()) { nxt.set(CURLUPART_QUERY, curQuery); }
                flog::info("websock: redirect to the site root, keeping endpoint {}", curPath);
                const std::string rebuilt = nxt.get(CURLUPART_URL);
                if (!rebuilt.empty()) { next = toWsScheme(rebuilt); }
            }
            else if (nextPath != curPath && ascii::equalsIgnoreCase(nextPath, curPath) && nxt.set(CURLUPART_PATH, curPath)) {
                flog::info("websock: preserving redirect path case: {} -> {}", nextPath, curPath);
                const std::string rebuilt = nxt.get(CURLUPART_URL);
                if (!rebuilt.empty()) { next = toWsScheme(rebuilt); }
            }
            return next;
        }

        struct HandshakeProgress {
            const std::atomic<bool>* stopped;
            Clock::time_point deadline;
            bool timedOut = false;
        };

        // libcurl progress callback during the upgrade: aborts the transfer
        // on stopSocket() or when the handshake deadline passes.
        int handshakeProgress(void* clientp, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
            auto* progress = static_cast<HandshakeProgress*>(clientp);
            if (progress->stopped->load()) { return 1; }
            if (Clock::now() >= progress->deadline) {
                progress->timedOut = true;
                return 1;
            }
            return 0;
        }

    } // namespace

    WSClient::WSClient() {
        flog::info("WSClient instance: {}", (uint64_t)(uintptr_t)this);
    }

    WSClient::~WSClient() = default;

    std::string WSClient::effectiveUrl() const {
        std::lock_guard<std::mutex> lock(m_urlMtx);
        return m_effectiveUrl;
    }

    std::unique_ptr<WSClient::Connection> WSClient::openConnection(std::string url) {
        for (int redirectsLeft = kMaxRedirects;; --redirectsLeft) {
            auto conn = std::make_unique<Connection>();
            conn->curl = curl::make_easy(url);
            if (!conn->curl) { throw std::runtime_error("websock: curl_easy_init failed"); }
            CURL* h = conn->curl;

            // permessage-deflate (RFC 7692) is not offered: libcurl does not
            // implement it, so the server sends plain frames.
            const std::string headers[] = {
                "Origin: " + originFor(url),
                "Cookie: ident=",
                "Cache-Control: no-cache",
                "Pragma: no-cache",
                "Accept-Language: en-US,en",
            };
            for (const std::string& header : headers) {
                curl_slist* list = curl_slist_append(conn->headers, header.c_str());
                if (!list) { throw std::runtime_error("websock: out of memory"); }
                conn->headers = list;
            }

            HandshakeProgress progress{ &m_stopped, Clock::now() + std::chrono::milliseconds(kHandshakeTimeoutMs) };
            curl_easy_setopt(h, CURLOPT_URL, url.c_str());
            curl_easy_setopt(h, CURLOPT_CONNECT_ONLY, 2L); // WebSocket upgrade, then hand over
            // libcurl does not follow a redirect on the upgrade; the loop
            // below does, mapping the scheme and keeping the path's case.
            curl_easy_setopt(h, CURLOPT_FOLLOWLOCATION, 0L);
            // Through an HTTP proxy, a ws:// upgrade forwarded as a plain
            // request rarely survives; tunnel with CONNECT as for wss://.
            curl_easy_setopt(h, CURLOPT_HTTPPROXYTUNNEL, 1L);
            curl_easy_setopt(h, CURLOPT_CONNECTTIMEOUT_MS, (long)kHandshakeTimeoutMs);
            curl_easy_setopt(h, CURLOPT_NOSIGNAL, 1L);
            curl_easy_setopt(h, CURLOPT_HTTPHEADER, conn->headers);
            curl_easy_setopt(h, CURLOPT_ERRORBUFFER, conn->errbuf);
            curl_easy_setopt(h, CURLOPT_XFERINFOFUNCTION, handshakeProgress);
            curl_easy_setopt(h, CURLOPT_XFERINFODATA, &progress);
            curl_easy_setopt(h, CURLOPT_NOPROGRESS, 0L);

            flog::info("websock: connecting to {}", url);
            const CURLcode rc = curl_easy_perform(h);

            // progress lives on this stack frame.
            curl_easy_setopt(h, CURLOPT_NOPROGRESS, 1L);
            curl_easy_setopt(h, CURLOPT_XFERINFODATA, nullptr);

            long status = 0;
            curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &status);

            if (rc == CURLE_OK) {
                char* effective = nullptr;
                curl_easy_getinfo(h, CURLINFO_EFFECTIVE_URL, &effective);
                {
                    std::lock_guard<std::mutex> lock(m_urlMtx);
                    m_effectiveUrl = effective ? effective : url;
                }
                flog::info("websock: connected to {} (HTTP {})", effectiveUrl(), status);
                return conn;
            }
            if (m_stopped) { return nullptr; }
            if (progress.timedOut) { throw std::runtime_error("websock: upgrade timed out: " + url); }

            if (status >= 300 && status < 400) {
                // libcurl refuses a non-101 answer to the upgrade before it
                // resolves a redirect (CURLINFO_REDIRECT_URL stays empty,
                // FOLLOWLOCATION would not follow), so read Location here.
                curl_header* header = nullptr;
                const std::string location =
                    (curl_easy_header(h, "Location", 0, CURLH_HEADER, -1, &header) == CURLHE_OK && header && header->value)
                    ? std::string(header->value) : std::string();
                if (location.empty()) {
                    throw std::runtime_error("websock: HTTP " + std::to_string(status) + " redirect without Location: " + url);
                }
                if (redirectsLeft <= 0) {
                    throw std::runtime_error("websock: too many redirects, last to " + location);
                }
                const std::string next = redirectTarget(url, location);
                flog::info("websock: following redirect ({} left) to {}", redirectsLeft, next);
                url = next;
                continue;
            }
            if (status != 0) {
                throw std::runtime_error("websock: upgrade failed: HTTP " + std::to_string(status));
            }
            throw std::runtime_error("websock: connect failed: " + conn->errorText(rc));
        }
    }

    WSClient::Drain WSClient::drainLocked(std::vector<char>& buf, Assembly& assembly, std::vector<Message>& out) {
        size_t budget = kDrainBudget;
        while (budget > 0) {
            size_t n = 0;
            const curl_ws_frame* meta = nullptr;
            m_conn->errbuf[0] = 0;
            const CURLcode rc = curl_ws_recv(m_conn->curl, buf.data(), buf.size(), &n, &meta);
            if (rc == CURLE_AGAIN) { return Drain::Idle; }
            if (rc == CURLE_GOT_NOTHING) {
                flog::info("websock: peer closed the connection");
                return Drain::Closed;
            }
            if (rc == CURLE_RECV_ERROR) {
                flog::warn("websock: connection lost: {}", m_conn->errorText(rc));
                return Drain::Closed;
            }
            if (rc != CURLE_OK) {
                throw std::runtime_error("websock: receive failed: " + m_conn->errorText(rc));
            }
            // Count a nominal header too, so a run of empty frames ends.
            budget -= std::min(budget, n + 16);
            if (!meta) { continue; }

            if (meta->flags & CURLWS_CLOSE) {
                if (meta->bytesleft == 0) {
                    // Answer the close; the connection is released either way.
                    try { sendFrameLocked(nullptr, 0, CURLWS_CLOSE); }
                    catch (const std::exception& e) { flog::warn("websock: close reply failed: {}", e.what()); }
                    return Drain::Closed;
                }
                continue;
            }
            // libcurl answers PINGs itself; PONGs are unsolicited (no PINGs
            // are sent). Neither interrupts a fragmented message.
            if (meta->flags & (CURLWS_PING | CURLWS_PONG)) { continue; }

            // Data frame, or a part of one. The first fragment carries the
            // type; CURLWS_CONT marks every fragment but the last.
            if (!assembly.active) {
                assembly.active = true;
                assembly.binary = (meta->flags & CURLWS_BINARY) != 0;
                assembly.data.clear();
            }
            if (assembly.data.size() + n > kMaxMessageSize) {
                throw std::runtime_error("websock: message too large");
            }
            assembly.data.append(buf.data(), n);
            if (meta->bytesleft == 0 && !(meta->flags & CURLWS_CONT)) {
                out.push_back(Message{ assembly.binary, std::move(assembly.data) });
                assembly = Assembly{};
            }
        }
        return Drain::More;
    }

    void WSClient::runReceiveLoop() {
        curl_socket_t sock = CURL_SOCKET_BAD;
        {
            std::lock_guard<std::mutex> lock(m_curlMtx);
            curl_easy_getinfo(m_conn->curl, CURLINFO_ACTIVESOCKET, &sock);
        }
        if (sock == CURL_SOCKET_BAD) { throw std::runtime_error("websock: no active socket after upgrade"); }

        std::vector<char> buf(kRecvChunk);
        Assembly assembly;
        std::vector<Message> ready;
        while (!m_stopped) {
            ready.clear();
            Drain result;
            {
                std::lock_guard<std::mutex> lock(m_curlMtx);
                result = drainLocked(buf, assembly, ready);
            }
            // Callbacks run outside the mutex so they may send.
            for (const Message& msg : ready) {
                if (msg.binary) { onBinaryMessage(msg.data); }
                else { onTextMessage(msg.data); }
            }
            if (result == Drain::Closed || m_stopped) { return; }
            // libcurl reads ahead (TLS records, frames after the 101), so the
            // drain comes first and the wait only once it reports Idle.
            if (result == Drain::Idle) { waitSocket(sock, false, kPollSliceMs); }
            // Tick fires on poll timeouts too, so time-based keepalives keep
            // flowing while the downlink is stalled (jittery mobile links);
            // without them the KiwiSDR server drops the session as inactive.
            onReceiveLoopTick();
        }
    }

    void WSClient::sendFrameLocked(const void* data, size_t len, unsigned int flags) {
        curl_socket_t sock = CURL_SOCKET_BAD;
        curl_easy_getinfo(m_conn->curl, CURLINFO_ACTIVESOCKET, &sock);
        const auto deadline = Clock::now() + std::chrono::milliseconds(kSendTimeoutMs);
        const char* p = static_cast<const char*>(data);
        for (;;) {
            size_t sent = 0;
            m_conn->errbuf[0] = 0;
            const CURLcode rc = curl_ws_send(m_conn->curl, p, len, &sent, 0, flags);
            sent = std::min(sent, len);
            p += sent;
            len -= sent;
            if (rc == CURLE_OK) {
                if (len == 0) { return; }
                continue;
            }
            if (rc != CURLE_AGAIN) {
                throw std::runtime_error("websock: send failed: " + m_conn->errorText(rc));
            }
            if (m_stopped) { throw std::runtime_error("websock: send aborted, client stopped"); }
            if (Clock::now() >= deadline) { throw std::runtime_error("websock: send timed out"); }
            waitSocket(sock, true, kSendWaitSliceMs);
        }
    }

    void WSClient::sendFrame(const void* data, size_t len, unsigned int flags) {
        std::lock_guard<std::mutex> lock(m_curlMtx);
        if (!m_conn) { throw std::runtime_error("websock: send while not connected"); }
        if (m_stopped) { throw std::runtime_error("websock: send aborted, client stopped"); }
        sendFrameLocked(data, len, flags);
    }

    void WSClient::sendString(const std::string& str) {
        sendFrame(str.data(), str.size(), CURLWS_TEXT);
    }

    void WSClient::sendBinary(const std::vector<uint8_t>& data) {
        sendFrame(data.data(), data.size(), CURLWS_BINARY);
    }

    void WSClient::closeConnection() {
        m_stopped = true;
        std::unique_ptr<Connection> conn;
        {
            std::lock_guard<std::mutex> lock(m_curlMtx);
            conn = std::move(m_conn);
        }
        // conn is released here, on the receive-loop thread; senders see
        // m_conn empty under the mutex and never touch the handle again.
    }

    void WSClient::connectAndReceiveLoop(const std::string& host, int port, const std::string& path) {
        const bool bracket = host.find(':') != std::string::npos && host.front() != '[';
        const std::string hostPart = bracket ? "[" + host + "]" : host;
        const std::string pathPart = (path.empty() || path.front() != '/') ? "/" + path : path;
        connectAndReceiveLoop("ws://" + hostPart + ":" + std::to_string(port) + pathPart);
    }

    void WSClient::connectAndReceiveLoop(const std::string& url) {
        flog::info("WSClient connectAndReceiveLoop: inst={}", (uint64_t)(uintptr_t)this);
        {
            std::lock_guard<std::mutex> lock(m_urlMtx);
            m_effectiveUrl.clear();
        }

        std::unique_ptr<Connection> conn;
        try { conn = openConnection(toWsScheme(url)); }
        catch (...) {
            m_stopped = true;
            throw;
        }
        if (!conn) {
            // Stopped during the handshake.
            m_stopped = true;
            onDisconnected();
            return;
        }
        {
            std::lock_guard<std::mutex> lock(m_curlMtx);
            m_conn = std::move(conn);
        }

        try {
            // onConnected typically sends the session-setup commands; if the
            // connection dies during them, onDisconnected must still fire so
            // the owner can unwind its "connected" state.
            onConnected();
            runReceiveLoop();
        }
        catch (...) {
            closeConnection();
            onDisconnected();
            throw;
        }
        closeConnection();
        onDisconnected();
    }

    void WSClient::stopSocket() {
        // Set the flag and let the receive loop release the connection on
        // its way out; libcurl calls are serialized by m_curlMtx.
        m_stopped = true;
    }

    void WSClient::reset() {
        m_stopped = false;
    }

}
