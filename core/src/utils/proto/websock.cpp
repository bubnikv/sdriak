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

#include <cerrno>
#include <chrono>
#include <cstring>
#include <stdexcept>
#include <string_view>
#include <thread>

#include "../ascii.h"
#include "../base64.h"
#include "http.h"
#include "picohash.h"
#include <utils/flog.h>

namespace net::websock {

    // Inbound classification returned by getFrame(). These values never go on
    // the wire — they only tell maybeDecodeBuffer() what the decoded frame is.
    enum class WSClient::Inbound : int {
        Text,
        Binary,
        IncompleteText,     // first fragment of a TEXT message (FIN=0, opcode=1)
        IncompleteBinary,   // first fragment of a BINARY message (FIN=0, opcode=2)
        Continuation,       // mid-fragment (FIN=0, opcode=0)
        ContinuationFinal,  // final fragment (FIN=1, opcode=0)
        Ping,
        Close,
        Pong,
        Incomplete,         // not enough bytes buffered yet
        Error,              // protocol violation
    };

    namespace {

        // Outgoing wire bytes (FIN=1, RSV=000, opcode in low nibble).
        // RFC 6455 §5.2/§5.4. Fed directly to makeFrame() as buffer[0].
        constexpr uint8_t WIRE_TEXT   = 0x81;
        constexpr uint8_t WIRE_BINARY = 0x82;
        constexpr uint8_t WIRE_CLOSE  = 0x88;
        constexpr uint8_t WIRE_PONG   = 0x8A;

        std::string sha1Base64(const std::string& in) {
            _picohash_sha1_ctx_t ctx;
            _picohash_sha1_init(&ctx);
            _picohash_sha1_update(&ctx, in.data(), in.size());
            uint8_t digest[PICOHASH_SHA1_DIGEST_LENGTH];
            _picohash_sha1_final(&ctx, digest);
            return base64::encode(digest, PICOHASH_SHA1_DIGEST_LENGTH);
        }

        void closeRawSocket(::net::SockHandle_t sock) {
#ifdef _WIN32
            closesocket(sock);
#else
            close(sock);
#endif
        }

        ::net::SockHandle_t invalidSocketHandle() {
#ifdef _WIN32
            return INVALID_SOCKET;
#else
            return -1;
#endif
        }

        bool isValidSocketHandle(::net::SockHandle_t sock) {
#ifdef _WIN32
            return sock != INVALID_SOCKET;
#else
            return sock >= 0;
#endif
        }

        constexpr int HANDSHAKE_TIMEOUT_MS = 10000;

        int deadlineSliceMs(std::chrono::steady_clock::time_point deadline) {
            using namespace std::chrono;
            const auto remaining = duration_cast<milliseconds>(deadline - steady_clock::now()).count();
            if (remaining <= 0) { return 0; }
            return static_cast<int>(remaining < 100 ? remaining : 100);
        }

        class RawSocketGuard {
        public:
            explicit RawSocketGuard(::net::SockHandle_t sock) : sock(sock) {}
            ~RawSocketGuard() {
                if (isValidSocketHandle(sock)) {
                    closeRawSocket(sock);
                }
            }

            RawSocketGuard(const RawSocketGuard&) = delete;
            RawSocketGuard& operator=(const RawSocketGuard&) = delete;

            explicit operator bool() const {
                return isValidSocketHandle(sock);
            }

            ::net::SockHandle_t get() const {
                return sock;
            }

            void release() {
                sock = invalidSocketHandle();
            }

        private:
            ::net::SockHandle_t sock;
        };

    } // namespace

    WSClient::WSClient() : socket(), rd(), e1(rd()), uniform_dist(0, 255) {
        flog::info("WSClient instance: {}", (uint64_t)(uintptr_t)this);
    }

    WSClient::~WSClient() = default;

    void WSClient::emplaceSocket(::net::SockHandle_t sock, const ::net::Address& addr) {
        std::lock_guard<std::mutex> lock(socketMutex);
        socket.emplace(sock, &addr);
    }

    void WSClient::closeCurrentSocket(bool markStopped) {
        websocketReady = false;
        if (markStopped) {
            stopped = true;
        }

        std::lock_guard<std::mutex> lock(socketMutex);
        if (socket) {
            socket->close();
            socket.reset();
        }
    }

    int WSClient::recvSocket(uint8_t* data, size_t len, int timeout) {
        std::lock_guard<std::mutex> lock(socketMutex);
        if (!socket) {
            return -1;
        }

        if (stopped) {
            return 0;
        }

        int n = socket->recv(data, len, false, timeout);
        if (!socket->isOpen()) {
            stopped = true;
        }
        return n;
    }

    int WSClient::maybeDecodeBuffer(const std::vector<uint8_t>& data) {
        std::string buffer;
        if (data.empty()) {
            return 0;
        }
        buffer.resize(data.size() + 200, ' ');
        int outLen = 0;
        int skipSize = 0;
        Inbound kind = getFrame((unsigned char*)data.data(), (int)data.size(),
                                (unsigned char*)buffer.data(), (int)buffer.length(),
                                &outLen, &skipSize);
        auto appendFragment = [&]() {
            if (outLen > 1) {
                if (fragmentBuffer.size() + (size_t)(outLen - 1) > (size_t)MAX_FRAME_PAYLOAD) {
                    throw std::runtime_error("websock: fragmented message too large");
                }
                fragmentBuffer.append(buffer.data(), outLen - 1);
            }
        };
        switch (kind) {
        case Inbound::Text:
            if (fragmentOpcode != 0) {
                throw std::runtime_error("websock: TEXT frame received while fragment in progress");
            }
            // outLen == payload_length + 1 (trailing NUL); deliver every
            // valid message including 0/1/2-byte payloads.
            buffer.resize(outLen - 1);
            onTextMessage(buffer);
            break;
        case Inbound::Binary:
            if (fragmentOpcode != 0) {
                throw std::runtime_error("websock: BINARY frame received while fragment in progress");
            }
            buffer.resize(outLen - 1);
            onBinaryMessage(buffer);
            break;
        case Inbound::IncompleteText:
            if (fragmentOpcode != 0) {
                throw std::runtime_error("websock: TEXT fragment-start while fragment in progress");
            }
            fragmentOpcode = 1;
            appendFragment();
            break;
        case Inbound::IncompleteBinary:
            if (fragmentOpcode != 0) {
                throw std::runtime_error("websock: BINARY fragment-start while fragment in progress");
            }
            fragmentOpcode = 2;
            appendFragment();
            break;
        case Inbound::Continuation:
            if (fragmentOpcode == 0) {
                throw std::runtime_error("websock: CONTINUATION frame without fragment in progress");
            }
            appendFragment();
            break;
        case Inbound::ContinuationFinal:
            if (fragmentOpcode == 0) {
                throw std::runtime_error("websock: CONTINUATION-final frame without fragment in progress");
            }
            appendFragment();
            if (fragmentOpcode == 1) { onTextMessage(fragmentBuffer); }
            else if (fragmentOpcode == 2) { onBinaryMessage(fragmentBuffer); }
            fragmentOpcode = 0;
            fragmentBuffer.clear();
            break;
        case Inbound::Incomplete:
            return 0;
        case Inbound::Error:
            throw std::runtime_error("websock: invalid frame");
        case Inbound::Ping:
            // outLen = payload_length + 1 (getFrame appends a NUL terminator),
            // so outLen-1 is the actual ping payload size to echo.
            sendPong((const uint8_t*)buffer.data(), (size_t)(outLen - 1));
            break;
        case Inbound::Close:
            sendClose();
            stopped = true;
            break;
        case Inbound::Pong:
            // RFC 6455 §5.5.4: unsolicited Pongs are spec-allowed; this
            // client never sends Pings, so any Pong is ignored.
            break;
        }
        return skipSize;
    }

    // Short-write retry loop. Caller MUST hold sendMutex.
    void WSClient::sendAllRawLocked(const uint8_t* data, size_t len) {
        if (stopped) {
            throw std::runtime_error("websock: send aborted, client stopped");
        }
        size_t sent = 0;
        while (sent < len) {
            if (stopped) {
                throw std::runtime_error("websock: send aborted, client stopped");
            }

            int n = 0;
            bool open = true;
            {
                std::lock_guard<std::mutex> lock(socketMutex);
                if (!socket) {
                    throw std::runtime_error("websock: send on closed socket");
                }
                if (stopped) {
                    throw std::runtime_error("websock: send aborted, client stopped");
                }
                n = socket->send(data + sent, len - sent);
                open = socket->isOpen();
                if (!open) {
                    stopped = true;
                }
            }
            if (n > 0) {
                sent += (size_t)n;
                continue;
            }
            if (!open) {
                throw std::runtime_error("websock: send failed, socket closed");
            }
            // Non-blocking socket reported WOULD_BLOCK; brief backoff and retry.
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    // Used for the upgrade-request bytes during the handshake. No frame
    // wrapping, no mask, doesn't touch the random engine.
    void WSClient::sendAllRaw(const uint8_t* data, size_t len) {
        std::lock_guard<std::mutex> lock(sendMutex);
        sendAllRawLocked(data, len);
    }

    // Build a websocket frame and write it. The mutex MUST cover makeFrame()
    // because makeFrame() mutates the shared random engine while generating
    // mask bytes — without this the four sender entry points would race.
    void WSClient::sendFrame(uint8_t firstByte, const uint8_t* payload, size_t len) {
        std::lock_guard<std::mutex> lock(sendMutex);
        if (!websocketReady) {
            throw std::runtime_error("websock: send before websocket handshake completed");
        }
        std::string buffer;
        buffer.resize(len + 200, ' ');
        int frameLen = makeFrame(firstByte, (unsigned char*)payload, (int)len,
                                 (unsigned char*)buffer.data(), buffer.size());
        sendAllRawLocked((const uint8_t*)buffer.data(), (size_t)frameLen);
    }

    // RFC 6455 §5.5.3: a Pong sent in response to a Ping MUST carry the
    // same Application data as the Ping. getFrame() rejects control frames
    // with payload >125 bytes, so `len` is always small here.
    void WSClient::sendPong(const uint8_t* payload, size_t len) {
        sendFrame(WIRE_PONG, payload, len);
    }

    void WSClient::sendClose() {
        sendFrame(WIRE_CLOSE, nullptr, 0);
    }

    void WSClient::sendString(const std::string& str) {
        sendFrame(WIRE_TEXT, (const uint8_t*)str.data(), str.size());
//        flog::info("<= {}", str);
    }

    void WSClient::sendBinary(const std::vector<uint8_t>& data) {
        sendFrame(WIRE_BINARY, data.data(), data.size());
    }

    // PARTS were taken from https://github.com/katzarsky/WebSocket
    int WSClient::makeFrame(uint8_t firstByte, unsigned char* msg, int msg_length,
                            unsigned char* buffer, int buffer_size) {
        int pos = 0;
        int size = msg_length;
        buffer[pos++] = firstByte; // FIN/RSV/opcode — see WIRE_* constants

        if (size <= 125) {
            buffer[pos++] = size; // set mask bit
        }
        else if (size <= 65535) {
            buffer[pos++] = 126; //16 bit length follows

            buffer[pos++] = (size >> 8) & 0xFF; // leftmost first
            buffer[pos++] = size & 0xFF;
        }
        else { // >2^16-1 (65535)
            buffer[pos++] = 127; //64 bit length follows

            // write 8 bytes length (significant first)

            // since msg_length is int it can be no longer than 4 bytes = 2^32-1
            // padd zeroes for the first 4 bytes
            for (int i = 3; i >= 0; i--) {
                buffer[pos++] = 0;
            }
            // write the actual 32bit msg_length in the next 4 bytes
            for (int i = 3; i >= 0; i--) {
                buffer[pos++] = ((size >> 8 * i) & 0xFF);
            }
        }
        // RFC 6455 §5.1: a client MUST mask every frame it sends, including
        // control frames such as PONG.
        buffer[1] |= 0x80;
        auto maskIndex = pos;
        buffer[pos++] = uniform_dist(e1);
        buffer[pos++] = uniform_dist(e1);
        buffer[pos++] = uniform_dist(e1);
        buffer[pos++] = uniform_dist(e1);
        if (size > 0) {
            memcpy((void*)(buffer + pos), msg, size);
        }
        for (int q = 0; q < size; q++) {
            buffer[pos + q] ^= buffer[maskIndex + (q % 4)];
        }
        return (size + pos);
    }

    WSClient::Inbound WSClient::getFrame(unsigned char* in_buffer, int in_length,
                                         unsigned char* out_buffer, int out_size,
                                         int* out_length, int* skipSize) {
        //printf("getTextFrame()\n");
        if (in_length < 2) return Inbound::Incomplete;

        unsigned char msg_opcode = in_buffer[0] & 0x0F;
        unsigned char msg_fin = (in_buffer[0] >> 7) & 0x01;
        unsigned char msg_masked = (in_buffer[1] >> 7) & 0x01;
        if (in_buffer[0] & 0x70) {
            return Inbound::Error;
        }
        if (msg_masked) {
            return Inbound::Error;
        }

        // *** message decoding

        uint64_t payload_length = 0;
        int pos = 2;
        int length_field = in_buffer[1] & 0x7F;

        //printf("IN:"); for(int i=0; i<20; i++) printf("%02x ",buffer[i]); printf("\n");

        if (length_field <= 125) {
            payload_length = length_field;
        }
        else if (length_field == 126) { //msglen is 16bit!
            if (in_length < pos + 2) {
                return Inbound::Incomplete;
            }
            payload_length = (
                ((uint64_t)in_buffer[pos] << 8) |
                ((uint64_t)in_buffer[pos + 1])
            );
            if (payload_length < 126) {
                return Inbound::Error;
            }
            pos += 2;
        }
        else if (length_field == 127) { //msglen is 64bit!
            if (in_length < pos + 8) {
                return Inbound::Incomplete;
            }
            if (in_buffer[pos] & 0x80) {
                return Inbound::Error;
            }
            for (int i = 0; i < 8; i++) {
                payload_length = (payload_length << 8) | in_buffer[pos + i];
            }
            if (payload_length < 65536) {
                return Inbound::Error;
            }
            pos += 8;
        }

        //printf("PAYLOAD_LEN: %08x\n", payload_length);
        if (payload_length > (uint64_t)MAX_FRAME_PAYLOAD) {
            flog::error("ERROR: websocket payload is too large: {}", payload_length);
            return Inbound::Error;
        }

        if (payload_length > (uint64_t)(in_length - pos)) {
            return Inbound::Incomplete;
        }

        if (payload_length > (uint64_t)(out_size - 1)) {
            flog::error("ERROR: output buffer is too small for the payload");
            return Inbound::Error;
        }

        const int payloadSize = (int)payload_length;
        memcpy((void*)out_buffer, (void*)(in_buffer + pos), payloadSize);
        out_buffer[payloadSize] = 0;
        *out_length = payloadSize + 1;
        *skipSize = payloadSize + pos;

        //printf("TEXT: %s\n", out_buffer);

        if (msg_opcode >= 0x8) {
            if (!msg_fin || payload_length > 125) { return Inbound::Error; }
            if (msg_opcode == 0x8 && payload_length == 1) { return Inbound::Error; }
        }

        switch (msg_opcode) {
        case 0x0: return msg_fin ? Inbound::ContinuationFinal : Inbound::Continuation;
        case 0x1: return msg_fin ? Inbound::Text : Inbound::IncompleteText;
        case 0x2: return msg_fin ? Inbound::Binary : Inbound::IncompleteBinary;
        case 0x8: return Inbound::Close;
        case 0x9: return Inbound::Ping;
        case 0xA: return Inbound::Pong;
        default:  return Inbound::Error;
        }
    }

    ::net::SockHandle_t WSClient::connectSocket(const ::net::Address& addr,
                                                std::chrono::steady_clock::time_point deadline) {
        ::net::SockHandle_t sock = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
#ifdef _WIN32
        if (sock == INVALID_SOCKET) {
#else
        if (sock < 0) {
#endif
            throw std::runtime_error("Could not create socket");
        }

#ifdef _WIN32
        u_long enabled = 1;
        ioctlsocket(sock, FIONBIO, &enabled);
#else
        fcntl(sock, F_SETFL, O_NONBLOCK);
#endif

        int result = ::connect(sock, (sockaddr*)&addr.addr, sizeof(sockaddr_in));
        if (result) {
#ifdef _WIN32
            int err = WSAGetLastError();
            if (err != WSAEWOULDBLOCK && err != WSAEINPROGRESS) {
#else
            if (errno != EINPROGRESS) {
#endif
                closeRawSocket(sock);
                throw std::runtime_error("Could not connect");
            }
        }

        while (!stopped) {
            const int timeoutMs = deadlineSliceMs(deadline);
            if (timeoutMs <= 0) {
                closeRawSocket(sock);
                throw std::runtime_error("websock: connect timed out");
            }

            fd_set writeSet;
            fd_set errorSet;
            FD_ZERO(&writeSet);
            FD_ZERO(&errorSet);
            FD_SET(sock, &writeSet);
            FD_SET(sock, &errorSet);

            timeval tv;
            tv.tv_sec = timeoutMs / 1000;
            tv.tv_usec = (timeoutMs % 1000) * 1000;
#ifdef _WIN32
            int ready = select(0, NULL, &writeSet, &errorSet, &tv);
#else
            int ready = select(sock + 1, NULL, &writeSet, &errorSet, &tv);
#endif
            if (ready < 0) {
                closeRawSocket(sock);
                throw std::runtime_error("Could not connect");
            }
            if (ready == 0) {
                continue;
            }

            int connectError = 0;
#ifdef _WIN32
            int connectErrorLen = sizeof(connectError);
#else
            socklen_t connectErrorLen = sizeof(connectError);
#endif
            if (getsockopt(sock, SOL_SOCKET, SO_ERROR, (char*)&connectError, &connectErrorLen) || connectError) {
                closeRawSocket(sock);
                throw std::runtime_error("Could not connect");
            }
            return sock;
        }

        closeRawSocket(sock);
        return invalidSocketHandle();
    }

    std::string WSClient::generateSecKey() {
        uint8_t keyBytes[16];
        for (int i = 0; i < 16; i++) { keyBytes[i] = (uint8_t)uniform_dist(e1); }
        return base64::encode(keyBytes, 16);
    }

    std::string WSClient::buildUpgradeRequest(const std::string& host, int port,
                                              const std::string& path, const std::string& secKey) {
        // RFC 7230 §5.4: omit the port from Host/Origin when it matches the scheme default,
        // otherwise reverse proxies (e.g. *.proxy.kiwisdr.com) fail to match server_name.
        const std::string hostHeader = net::http::hostHeaderFor({ host, port, path });
        std::string req = "GET " + path + " HTTP/1.1\r\n"
            "Accept-Language: en-US,en\r\n"
            "Cache-Control: no-cache\r\n"
            "Connection: Upgrade\r\n"
            "Cookie: ident=\r\n"
            "Pragma: no-cache\r\n"
            "Sec-GPC: 1\r\n"
            // permessage-deflate (RFC 7692) is intentionally NOT advertised: getFrame()
            // ignores RSV1 and there is no inflate path, so accepting the extension
            // would silently feed compressed bytes to the message callbacks.
            "Sec-WebSocket-Version: 13\r\n"
            "Upgrade: websocket\r\n"
            "User-Agent: Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/108.0.0.0 Safari/537.36\r\n";
        req += "Sec-WebSocket-Key: " + secKey + "\r\n";
        req += "Host: " + hostHeader + "\r\n";
        req += "Origin: http://" + hostHeader + "\r\n";
        req += "\r\n";
        return req;
    }

    bool WSClient::readUpgradeResponse(std::vector<uint8_t>& buf, size_t& recvd,
                                       size_t& headerEnd,
                                       std::chrono::steady_clock::time_point deadline) {
        // Accumulate the upgrade response until we see the \r\n\r\n header terminator.
        // The response can be split across multiple TCP segments (e.g. direct KiwiSDRs
        // send the 101 in fragments), so a single recv is not enough.
        recvd = 0;
        while (!stopped) {
            if (recvd + 1 >= buf.size()) {
                closeCurrentSocket(true);
                throw std::runtime_error("websock: upgrade response headers too large");
            }
            const int timeoutMs = deadlineSliceMs(deadline);
            if (timeoutMs <= 0) {
                closeCurrentSocket(true);
                throw std::runtime_error("websock: upgrade response timed out");
            }
            int n = recvSocket(buf.data() + recvd, buf.size() - 1 - recvd, timeoutMs);
            if (n < 0) {
                closeCurrentSocket(true);
                throw std::runtime_error("websock: upgrade-response recv failed");
            }
            if (n == 0) { continue; } // timeout — keep waiting unless stopped
            recvd += n;
            const auto pos = std::string_view((const char*)buf.data(), recvd).find("\r\n\r\n");
            if (pos != std::string_view::npos) {
                headerEnd = pos;
                return true;
            }
        }
        return false;
    }

    void WSClient::validateUpgradeHeaders(const net::http::ResponseHeader& header) {
        const std::string upgradeHdr = net::http::getHeaderValue(header, "Upgrade");
        if (!ascii::equalsIgnoreCase(upgradeHdr, "websocket")) {
            closeCurrentSocket(true);
            throw std::runtime_error("websock: handshake failed: missing or wrong Upgrade header (got '" + upgradeHdr + "')");
        }
        const std::string accept = net::http::getHeaderValue(header, "Sec-WebSocket-Accept");
        const std::string expected = sha1Base64(secKey + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11");
        if (accept != expected) {
            closeCurrentSocket(true);
            throw std::runtime_error("websock: handshake failed: bad Sec-WebSocket-Accept (got '" + accept + "', expected '" + expected + "')");
        }
    }

    std::optional<std::vector<uint8_t>> WSClient::performHandshake(
            const std::string& host, int port, const std::string& path) {
        constexpr int MAX_REDIRECTS = 5;
        int redirectsLeft = MAX_REDIRECTS;
        std::string currentHost = host;
        int currentPort = port;
        std::string currentPath = path;

        while (true) {
            const auto deadline = std::chrono::steady_clock::now() +
                                  std::chrono::milliseconds(HANDSHAKE_TIMEOUT_MS);
            ::net::Address addr(currentHost, currentPort);
            RawSocketGuard sock(connectSocket(addr, deadline));
            if (!sock) { return std::nullopt; }
            emplaceSocket(sock.get(), addr);
            sock.release();
            if (stopped) {
                closeCurrentSocket(false);
                return std::nullopt;
            }
            flog::info("WSClient socket connected to {}:{}", currentHost, currentPort);

            secKey = generateSecKey();
            fragmentOpcode = 0;
            fragmentBuffer.clear();

            const std::string req = buildUpgradeRequest(currentHost, currentPort, currentPath, secKey);
            sendAllRaw((const uint8_t*)req.data(), req.size());
            flog::info("sent {} bytes of upgrade request", (int64_t)req.size());

            std::vector<uint8_t> buf(100000);
            size_t recvd = 0;
            size_t headerEnd = 0;
            if (!readUpgradeResponse(buf, recvd, headerEnd, deadline)) {
                closeCurrentSocket(false);
                return std::nullopt;
            }
            flog::info("recvd: {}", recvd);

            const std::string headerData((const char*)buf.data(), headerEnd + 2);
            net::http::ResponseHeader header;
            try { header.deserialize(headerData); }
            catch (...) {
                closeCurrentSocket(true);
                throw std::runtime_error("websock: upgrade failed: malformed HTTP response");
            }

            const int status = static_cast<int>(header.getStatusCode());
            if (status == 101) {
                validateUpgradeHeaders(header);
                return std::vector<uint8_t>(buf.begin() + headerEnd + 4, buf.begin() + recvd);
            }

            if (net::http::isRedirectStatus(status) && redirectsLeft > 0) {
                auto parsed = net::http::resolveRedirectLocation({ currentHost, currentPort, currentPath }, header);
                if (!parsed) {
                    closeCurrentSocket(true);
                    throw std::runtime_error("websock: cannot parse redirect Location: " +
                                             net::http::getHeaderValue(header, "Location"));
                }
                closeCurrentSocket(false);
                std::string redirectPath = parsed->path;
                if (redirectPath != currentPath && ascii::equalsIgnoreCase(redirectPath, currentPath)) {
                    flog::info("websock: preserving redirect path case: {} -> {}", redirectPath, currentPath);
                    redirectPath = currentPath;
                }
                flog::info("websock: following redirect ({} left) to {}:{}{}", redirectsLeft, parsed->host, parsed->port, redirectPath);
                currentHost = parsed->host;
                currentPort = parsed->port;
                currentPath = redirectPath;
                redirectsLeft--;
                continue;
            }

            const auto firstCr = headerData.find("\r\n");
            const std::string statusLine = firstCr == std::string::npos
                ? headerData : headerData.substr(0, firstCr);
            closeCurrentSocket(true);
            throw std::runtime_error("websock: upgrade failed: " + statusLine);
        }
    }

    void WSClient::runReceiveLoop(std::vector<uint8_t> initial) {
        std::vector<uint8_t> buf(100000);
        std::vector<uint8_t> data = std::move(initial);
        while (!stopped) {
            const int len0 = maybeDecodeBuffer(data);
            if (len0 > 0) {
                data.erase(data.begin(), data.begin() + len0);
                continue;
            }
            const int recvd = recvSocket(buf.data(), buf.size(), 100);
            if (stopped) { return; }
            if (recvd < 0) { return; } // peer closed or recv error
            // Tick fires on recv timeouts too, so time-based keepalives keep
            // flowing while the downlink is stalled (jittery mobile links);
            // without them the KiwiSDR server drops the session as inactive.
            onReceiveLoopTick();
            if (recvd == 0) { continue; }
            if (data.size() + (size_t)recvd > MAX_FRAME_PAYLOAD + 16) {
                throw std::runtime_error("websock: frame too large");
            }
            data.insert(data.end(), buf.begin(), buf.begin() + recvd);
        }
    }

    void WSClient::connectAndReceiveLoop(const std::string& host, int port, const std::string& path) {
        flog::info("WSClient connectAndReceiveLoop: inst={}", (uint64_t)(uintptr_t)this);
        websocketReady = false;

        auto residual = performHandshake(host, port, path);
        if (!residual) {
            // Stopped or connect failed; performHandshake already cleaned up.
            onDisconnected();
            return;
        }

        websocketReady = true;
        try {
            // onConnected typically sends the session-setup commands; if the
            // socket dies during them, onDisconnected must still fire so the
            // owner can unwind its "connected" state.
            onConnected();
            runReceiveLoop(std::move(*residual));
        }
        catch (...) {
            closeCurrentSocket(true);
            onDisconnected();
            throw;
        }
        closeCurrentSocket(true);
        onDisconnected();
    }

    void WSClient::stopSocket() {
        // Set the flag and let the receive loop close the socket on its way
        // out. Socket method calls are serialized by socketMutex, so no
        // thread closes while another is inside send/recv.
        stopped = true;
    }

    void WSClient::reset() {
        stopped = false;
    }

}
