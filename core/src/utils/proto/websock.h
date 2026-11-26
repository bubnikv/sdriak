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
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace net::websock {

    // RFC 6455 client on libcurl's WebSocket API (curl_ws_*). TLS, trust
    // store and user agent come from curl::make_easy(). libcurl is linked
    // privately into the core, so this header does not include curl.h.
    struct WSClient {
        WSClient();
        ~WSClient();
        WSClient(const WSClient&) = delete;
        WSClient& operator=(const WSClient&) = delete;

        // Connects to ws://host:port/path; see the URL overload.
        void connectAndReceiveLoop(const std::string& host, int port, const std::string& path);

        // WebSocket upgrade followed by the receive loop, both on the calling
        // thread. url is ws://, wss://, http:// (treated as ws) or https://
        // (treated as wss). Follows up to kMaxRedirects redirects, mapping
        // http to ws and https to wss and keeping the path's case. Drives the
        // on*Message / onConnected / onDisconnected / onReceiveLoopTick
        // callbacks until stopSocket() is called or the peer closes. Throws
        // on connect, upgrade or protocol failure; the caller is expected to
        // surface that to the user. Returns normally when stopped or closed.
        void connectAndReceiveLoop(const std::string& url);

        // Final URL after redirects. Set before onConnected fires; empty
        // before the first successful upgrade of a connectAndReceiveLoop().
        std::string effectiveUrl() const;

        // Both sends are thread-safe (serialized with the receive loop by an
        // internal mutex) and return once the whole message is handed to
        // libcurl. They throw std::runtime_error when not connected, stopped,
        // or the send fails.
        void sendString(const std::string& str);
        void sendBinary(const std::vector<uint8_t>& data);
        // Enqueue control messages for the connection thread. A key replaces
        // only the immediately preceding command of that kind, preserving
        // order across setup commands and other control messages.
        bool queueString(std::string message, std::string key = {});

        // Signals the receive loop to exit. Safe to call from any thread,
        // including from inside a callback. The receive loop notices within
        // one 100 ms poll slice (the handshake within libcurl's progress
        // callback period, about 1 s) and releases the connection on its
        // own thread.
        void stopSocket();

        // Clear the stopped flag so connectAndReceiveLoop() can be called
        // again on this instance after a previous stopSocket() / disconnect.
        void reset();

        // True iff the receive loop has been asked to stop or has terminated.
        bool isStopped() const { return m_stopped.load(); }

        std::function<void(const std::string&)> onTextMessage   = [](auto){};
        std::function<void(const std::string&)> onBinaryMessage = [](auto){};
        std::function<void()> onConnected     = []{};
        std::function<void()> onDisconnected  = []{};
        // Invoked on every receive-loop iteration, including the 100 ms poll
        // timeouts while the downlink is stalled. Suitable for time-based
        // keepalives that must keep flowing even when no data arrives.
        std::function<void()> onReceiveLoopTick = []{};

        static constexpr int kMaxRedirects = 5;

    private:
        // libcurl easy handle and its request headers; defined in websock.cpp.
        struct Connection;
        // A message being reassembled from fragments, and a complete one.
        struct Assembly;
        struct Message;
        enum class Drain { Idle, More, Closed };

        static constexpr size_t kMaxMessageSize = 1024 * 1024;

        // Guards m_conn: every libcurl call on the connected handle (recv,
        // send, getinfo) and its release happen under this mutex.
        std::mutex m_curlMtx;
        std::unique_ptr<Connection> m_conn;

        mutable std::mutex m_urlMtx;
        std::string m_effectiveUrl;

        std::atomic<bool> m_stopped{false};
        struct Command {
            std::string message;
            std::string key;
        };
        std::mutex m_commandMtx;
        std::deque<Command> m_commands;

        // Upgrade with redirects. Returns nullptr if stopped during the
        // handshake; throws on failure.
        std::unique_ptr<Connection> openConnection(std::string url);
        // Receive-loop body. Throws on protocol errors; returns normally on
        // stopped / peer close.
        void runReceiveLoop();
        // Caller holds m_curlMtx. Reads frames until libcurl has nothing
        // buffered (Idle), a per-call byte budget is used up (More) or the
        // connection ends (Closed). Complete messages go to out.
        Drain drainLocked(std::vector<char>& buf, Assembly& assembly, std::vector<Message>& out);
        void sendFrame(const void* data, size_t len, unsigned int flags);
        // Caller holds m_curlMtx and m_conn is set.
        void sendFrameLocked(const void* data, size_t len, unsigned int flags);
        // Sets the stopped flag and releases the handle under m_curlMtx.
        void closeConnection();
    };

}
