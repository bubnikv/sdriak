#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <dsp/types.h>
#include <json.hpp>
#include <utils/proto/websock.h>

#include "pcm_v4.hpp"

namespace ubersdr {
    // Client for UberSDR's native API (madpsy/ka9q_ubersdr, GPL-3.0): the
    // POST /connection admission and the /ws IQ socket in pcm_v4 format.
    // Protocol facts from the server source and from measurements on public
    // instances: doc/research/ubersdr.md sections 3 and 4.
    class UberSDRClient {
    public:
        // POST /connection reply.
        struct Admission {
            bool allowed = false;
            long httpStatus = 0;           // 403: wrong password
            std::string reason;            // why not allowed, or the transport error
            std::vector<std::string> iqModes;  // allowed_iq_modes, e.g. "iq48"
            int maxSessionS = 0;           // 0: unlimited
            int dailyRemainingS = -1;      // -1: unlimited
            int vfos = 1;                  // primary plus secondaries allowed
            bool bypassed = false;
        };

        // One IQ packet's header, as the server sent it.
        struct PacketInfo {
            uint64_t timestampNanos = 0;   // first sample, ns since the epoch; 0 on the first few packets
            int sampleRate = 0;
            float basebandPower = -999.0f; // front-end dBFS, -999 when not reported
            float noise = -999.0f;
            uint64_t streamIndex = 0;      // complex samples delivered before this packet
        };

        // sessionId: user_session_id, kept by the caller across reconnects
        // (a reconnect with the same id needs no new /connection).
        explicit UberSDRClient(std::string sessionId);
        ~UberSDRClient();
        UberSDRClient(const UberSDRClient&) = delete;
        UberSDRClient& operator=(const UberSDRClient&) = delete;

        // A random UUID v4.
        static std::string newSessionId();

        // Blocking. baseUrl is scheme://host[:port]. cancel aborts the
        // request.
        Admission admit(const std::string& baseUrl, const std::string& password,
                        const std::function<bool()>& cancel = {});

        // Open /ws on a thread of its own: mode iq48 / iq96 / iq192 / iq384 on
        // the primary (vfo 0), or a demodulated mode or "iq" (12 kHz) on a
        // secondary vfo = N. A second primary with the same session id
        // replaces the first on the server.
        void start(const std::string& baseUrl, const std::string& mode, double freqHz,
                   const std::string& password = {}, int vfo = 0);
        // Close and join. Any thread except the client's own; idempotent.
        void stop();
        bool running() const { return m_running.load(); }
        bool connected() const { return m_connected.load(); }
        const std::string& sessionId() const { return m_sessionId; }

        // Any thread; answered by a status echo. Dropped while not connected.
        void tune(double freqHz, const std::string& mode);
        void requestStatus();

        // Network thread. onIQ gets complex samples scaled to +-1.
        std::function<void()> onConnected = [] {};
        std::function<void(const PacketInfo&, const dsp::complex_t*, int)> onIQ = [](const PacketInfo&, const dsp::complex_t*, int) {};
        // "status" messages: frequency, mode, sampleRate, sessionId, info.
        std::function<void(const nlohmann::json&)> onStatus = [](const nlohmann::json&) {};
        // A server "error" (the server closes next) or a decoder or transport
        // failure.
        std::function<void(const std::string&)> onError = [](const std::string&) {};
        std::function<void()> onDisconnected = [] {};

        static constexpr int kPingIntervalS = 30;

    private:
        void onBinary(const std::string& msg);
        void onText(const std::string& msg);
        bool trySend(const std::string& msg);

        std::string m_sessionId;
        net::websock::WSClient m_ws;
        std::mutex m_lifeMtx;   // start() / stop()
        std::thread m_thread;
        std::atomic<bool> m_running{false};
        std::atomic<bool> m_connected{false};

        // Network thread only. One decoder per connection: every packet must
        // go through it, or the predictor loses sync.
        PCMv4StreamDecoder m_decoder;
        std::vector<dsp::complex_t> m_samples;
        uint64_t m_streamIndex = 0;
        std::chrono::steady_clock::time_point m_lastPing;
    };

    // The per-user spectrum, /ws/user-spectrum in v2 binary8 format, on the
    // IQ session's id. Frames state their own centre and timestamp; the span
    // comes from the last config reply (binCount x binBandwidth).
    class UberSpectrumClient {
    public:
        // One frame, rotated out of FFT order: bin k is centred at
        // centreHz + (k - count/2) * binBandwidth. Valid only during the
        // callback.
        struct Frame {
            const float* binsDb = nullptr;  // front-end dBFS
            int count = 0;
            uint64_t timestampNanos = 0;
            double centreHz = 0.0;
            uint16_t seq = 0;
        };
        struct Config {
            double centreHz = 0.0;
            int binCount = 0;
            double binBandwidth = 0.0;
            double totalBandwidth = 0.0;
        };

        UberSpectrumClient();
        ~UberSpectrumClient();
        UberSpectrumClient(const UberSpectrumClient&) = delete;
        UberSpectrumClient& operator=(const UberSpectrumClient&) = delete;

        // Opens the socket on a thread of its own; freqHz and binBandwidth
        // (0: the server's default view) set the first view.
        void start(const std::string& baseUrl, const std::string& sessionId, double freqHz, double binBandwidth,
                   const std::string& password = {});
        void stop();
        bool connected() const { return m_connected.load(); }

        // Any thread; dropped while not connected. The server clamps the view
        // to its range, and may not answer at all near 0 Hz: never wait on a
        // reply.
        void zoom(double freqHz, double binBandwidth);
        void pan(double freqHz, double binBandwidth);

        // Network thread.
        std::function<void(const Frame&)> onFrame = [](const Frame&) {};
        std::function<void(const Config&)> onConfig = [](const Config&) {};
        // status is the code the server attaches (429: rate limit).
        std::function<void(const std::string&, int status)> onError = [](const std::string&, int) {};

        // Network thread (public for tests): one binary message. Returns
        // false for anything that is not a usable v2 frame; frame.binsDb
        // points into the client's buffer.
        bool parseFrame(const uint8_t* data, size_t len, Frame& frame);

        static constexpr int kPingIntervalS = 30;

    private:
        void onBinary(const std::string& msg);
        void onControl(const std::string& json);
        bool trySend(const std::string& msg);

        net::websock::WSClient m_ws;
        std::mutex m_lifeMtx;
        std::thread m_thread;
        std::atomic<bool> m_running{false};
        std::atomic<bool> m_connected{false};

        // Network thread: the last full frame's codes and scale, which deltas
        // update; stale after a seq gap until the next full frame.
        std::vector<uint8_t> m_codes;
        int m_refCentiDb = 0;
        int m_stepCentiDb = 0;
        bool m_haveFull = false;
        bool m_haveSeq = false;
        uint16_t m_lastSeq = 0;
        std::vector<float> m_db;
        std::chrono::steady_clock::time_point m_lastPing;
    };
}
