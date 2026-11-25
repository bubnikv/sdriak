#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>

#include "utils/proto/websock.h"

// KiwiSDR waterfall (W/F) client, the krec form of the protocol: a second
// websocket /<ts>/W/F with the SND connection's ts, which the server pairs
// with it on one receiver channel. Opened before SND, so it claims a
// waterfall-capable channel that SND then joins. Uncompressed frames only
// (wf_comp=0). Protocol facts from jks-prv/Beagle_SDR_GPS (rx_waterfall.cpp)
// and kiwiclient; see doc/research/kiwisdr-streams.md section 3.
class KiwiWaterfallClient {
public:
    using Clock = std::chrono::steady_clock;

    static constexpr int kBins = 1024;
    // wf_speed: 1, 5, 13 and 23 frames/s.
    static constexpr int kMinSpeed = 1;
    static constexpr int kMaxSpeed = 4;

    // The wf_setup message.
    struct Setup {
        int zoomMax = 14;
        int zoomCap = 14;           // deepest zoom served; zoom_max on servers that do not say
        int wfChans = -1;           // -1 not reported
        double wfCalDb = -13.0;     // the default before v1.550
        double bandwidthHz = 30e6;  // receiver span, 30 or 32 MHz
        int fps = 23;
    };

    // One frame. bins points into the message and is valid only during the
    // callback.
    struct Frame {
        uint32_t xBin = 0;          // start counter, HZperStart units
        int zoom = 0;
        uint32_t flags = 0;         // bits 16 and up of the zoom word
        uint32_t seq = 0;           // SND seq leaving when the line was computed
        const uint8_t* bins = nullptr;
        int count = 0;
    };

    static constexpr uint32_t kFlagCompressed = 0x10000;

    // "W/F", pad, x_bin (LE32), flags|zoom (LE32), seq (LE32), bins.
    static bool parseFrame(const std::string& msg, Frame& frame);

    // Display geometry of a frame: start of bin 0 and the span, baseband Hz.
    static double frameStartHz(const Setup& setup, uint32_t xBin);
    static double zoomSpanHz(const Setup& setup, int zoom);
    // dBm = byte - 255 + wf_cal.
    static float binDbm(const Setup& setup, uint8_t value) { return (float)value - 255.0f + (float)setup.wfCalDb; }

    KiwiWaterfallClient();
    ~KiwiWaterfallClient();
    KiwiWaterfallClient(const KiwiWaterfallClient&) = delete;
    KiwiWaterfallClient& operator=(const KiwiWaterfallClient&) = delete;

    // Connect to url (http(s)://host[:port]/<ts>/W/F, or the web UI's
    // /kiwi/<ts>/W/F with webUiPath) on a thread of its own. speed is
    // wf_speed; zoom and cfKhz the first view.
    void start(const std::string& url, int speed, int zoom, double cfKhz, bool webUiPath = false);
    // Close and join. Any thread except the client's own; idempotent.
    void stop();

    // Until wf_setup or a refusal arrives, the connection ends, or timeout
    // passes. True when the waterfall is usable. cancel aborts the wait.
    bool waitReady(std::chrono::milliseconds timeout, const std::function<bool()>& cancel);

    // The server sent anything at all on this connection.
    bool serverAnswered() const { return m_anyMessage.load(); }
    // wf_setup arrived with waterfall channels, and the connection is up.
    bool available() const { return m_ready.load() && !m_failed.load(); }
    // Why the waterfall is not available; empty while it is or before the
    // connection was tried.
    std::string unavailableReason() const;
    Setup setup() const;

    // Any thread; dropped while the connection is not set up.
    void setView(int zoom, double cfKhz);
    void setSpeed(int speed);

    // Receive thread: every uncompressed frame, after wf_setup.
    std::function<void(const Frame&)> onFrame = [](const Frame&) {};

private:
    void stopLocked();
    void onMessage(const std::string& msg);
    void onKeyValue(const std::string& key, const std::string& value);
    void becomeReady();
    void fail(const std::string& reason);
    bool trySend(const std::string& cmd);

    net::websock::WSClient m_ws;
    // Serialises start() and stop(): the session thread and the owner both
    // stop the client when a session ends.
    std::mutex m_lifeMtx;
    std::thread m_thread;
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_ready{false};
    std::atomic<bool> m_failed{false};
    std::atomic<bool> m_anyMessage{false};
    std::atomic<bool> m_webUiPath{false};

    mutable std::mutex m_mtx;   // m_setup, m_reason; m_readyCv waits on it
    std::condition_variable m_readyCv;
    Setup m_setup;
    std::string m_reason;

    std::atomic<int> m_speed{kMaxSpeed};
    std::atomic<int> m_zoom{0};
    std::atomic<double> m_cfKhz{15000.0};

    // Receive thread only.
    std::map<std::string, std::string> m_keyValues;
    Clock::time_point m_lastPing;
};
