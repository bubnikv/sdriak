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
#pragma once
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <random>
#include <string>
#include <vector>

#include "../net.h"

namespace net::http { class ResponseHeader; }

namespace net::websock {

    struct WSClient {
        WSClient();
        ~WSClient();
        WSClient(const WSClient&) = delete;
        WSClient& operator=(const WSClient&) = delete;

        // RFC 6455 client handshake followed by the receive loop. Drives the
        // on*Message / onConnected / onDisconnected / onReceiveLoopTick
        // callbacks until stopSocket() is called or the peer closes. Throws on
        // protocol error; the caller is expected to surface that to the user.
        void connectAndReceiveLoop(const std::string& host, int port, const std::string& path);

        // Both sends are thread-safe (serialized by an internal mutex) and
        // perform short-write retry until the full frame is on the wire.
        void sendString(const std::string& str);
        void sendBinary(const std::vector<uint8_t>& data);

        // Signals the receive loop to exit. Safe to call from any thread,
        // including from inside a callback. The loop notices the stop on its
        // next 100 ms recv-timeout tick, then closes the socket under the
        // same mutex used by send/recv.
        void stopSocket();

        // Clear the stopped flag so connectAndReceiveLoop() can be called
        // again on this instance after a previous stopSocket() / disconnect.
        void reset();

        // True iff the receive loop has been asked to stop or has terminated.
        bool isStopped() const { return stopped.load(); }

        std::function<void(const std::string&)> onTextMessage   = [](auto){};
        std::function<void(const std::string&)> onBinaryMessage = [](auto){};
        std::function<void()> onConnected     = []{};
        std::function<void()> onDisconnected  = []{};
        // Invoked on every receive-loop iteration, including the 100 ms recv
        // timeouts while the downlink is stalled. Suitable for time-based
        // keepalives that must keep flowing even when no data arrives.
        std::function<void()> onReceiveLoopTick = []{};

    private:
        // Classification of an inbound frame returned by getFrame(). These
        // values are sentinels only; they never appear on the wire. Outgoing
        // wire bytes live as WIRE_* constants in the .cpp.
        enum class Inbound : int;

        static constexpr int64_t MAX_FRAME_PAYLOAD = 1024 * 1024;

        // socketMutex guards both socket lifetime and net::Socket method
        // calls because net::Socket keeps non-atomic state internally.
        std::optional<::net::Socket> socket;
        std::mutex socketMutex;

        std::string path;
        std::random_device rd;
        std::default_random_engine e1;
        std::uniform_int_distribution<int> uniform_dist;
        std::mutex sendMutex;

        std::string secKey;
        std::string fragmentBuffer;
        int fragmentOpcode = 0;
        std::atomic<bool> stopped{false};
        std::atomic<bool> websocketReady{false};

        int maybeDecodeBuffer(const std::vector<uint8_t>& data);

        int makeFrame(uint8_t firstByte, unsigned char* msg, int msg_length,
                      unsigned char* buffer, int buffer_size);
        Inbound getFrame(unsigned char* in_buffer, int in_length,
                         unsigned char* out_buffer, int out_size,
                         int* out_length, int* skipSize);

        ::net::SockHandle_t connectSocket(const ::net::Address& addr,
                                          std::chrono::steady_clock::time_point deadline);
        void emplaceSocket(::net::SockHandle_t sock, const ::net::Address& addr);
        void closeCurrentSocket(bool markStopped);
        int recvSocket(uint8_t* data, size_t len, int timeout);

        // Handshake helpers — see websock.cpp for the full flow.
        // performHandshake returns the residual bytes received after the
        // 101 response headers, or std::nullopt if stopped / connect failed.
        // It follows redirects internally and throws on hard failure.
        std::optional<std::vector<uint8_t>> performHandshake(
            const std::string& host, int port, const std::string& path);
        // Read upgrade-response bytes until "\r\n\r\n". Returns false if
        // stopped before completion; throws on size cap / recv error.
        // On success, headerEnd is the byte offset of the "\r\n\r\n".
        bool readUpgradeResponse(std::vector<uint8_t>& buf, size_t& recvd,
                                 size_t& headerEnd,
                                 std::chrono::steady_clock::time_point deadline);
        void validateUpgradeHeaders(const net::http::ResponseHeader& header);
        std::string generateSecKey();
        static std::string buildUpgradeRequest(const std::string& host, int port,
                                               const std::string& path, const std::string& secKey);
        // Receive-loop body. Throws on protocol errors; returns normally on
        // stopped / peer-close.
        void runReceiveLoop(std::vector<uint8_t> initial);
        // Handshake path: takes sendMutex, then writes raw bytes (no frame wrapping).
        void sendAllRaw(const uint8_t* data, size_t len);
        // Caller must hold sendMutex. Pure short-write retry loop.
        void sendAllRawLocked(const uint8_t* data, size_t len);
        // Builds a websocket frame and writes it under sendMutex. The mutex
        // covers makeFrame() too, since makeFrame mutates the shared random
        // engine while generating mask bytes.
        void sendFrame(uint8_t firstByte, const uint8_t* payload, size_t len);
        void sendPong(const uint8_t* payload, size_t len);
        void sendClose();
    };

}
