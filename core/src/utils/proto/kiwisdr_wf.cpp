#include "kiwisdr_wf.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <locale>
#include <sstream>

#include "utils/flog.h"
#include "utils/proto/kiwisdr.h"
#include "utils/url.h"

namespace {
    uint32_t readLE32(const uint8_t* b) {
        return (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
    }

    double parseDouble(const std::string& value, double def) {
        std::istringstream iss(value);
        iss.imbue(std::locale::classic());
        double v = 0.0;
        iss >> v;
        return iss ? v : def;
    }

    constexpr auto kKeepaliveInterval = std::chrono::seconds(4);
}

bool KiwiWaterfallClient::parseFrame(const std::string& msg, Frame& frame) {
    constexpr size_t kHeader = 16;
    if (msg.size() < kHeader || msg.compare(0, 3, "W/F") != 0) { return false; }
    const auto* b = reinterpret_cast<const uint8_t*>(msg.data());
    frame = Frame();
    frame.xBin = readLE32(b + 4);
    const uint32_t zoomWord = readLE32(b + 8);
    frame.zoom = (int)(zoomWord & 0xFFFF);
    frame.flags = zoomWord & 0xFFFF0000u;
    frame.seq = readLE32(b + 12);
    frame.bins = b + kHeader;
    frame.count = (int)(msg.size() - kHeader);
    return true;
}

double KiwiWaterfallClient::frameStartHz(const Setup& setup, uint32_t xBin) {
    return (double)xBin * setup.bandwidthHz / ((double)kBins * std::ldexp(1.0, setup.zoomMax));
}

double KiwiWaterfallClient::zoomSpanHz(const Setup& setup, int zoom) {
    return setup.bandwidthHz / std::ldexp(1.0, zoom);
}

KiwiWaterfallClient::KiwiWaterfallClient() {
    m_ws.onConnected = [this] {
        m_lastPing = Clock::now();
        // No password: "p=" on the krec path, "p=#" on the web UI's.
        trySend(m_webUiPath.load() ? "SET auth t=kiwi p=#" : "SET auth t=kiwi p=");
        trySend("SERVER DE CLIENT sdriak W/F");
        trySend("SET ident_user=sdriak");
    };
    m_ws.onBinaryMessage = [this](const std::string& msg) { onMessage(msg); };
    m_ws.onReceiveLoopTick = [this] {
        const auto now = Clock::now();
        if (now - m_lastPing > kKeepaliveInterval) {
            trySend("SET keepalive");
            m_lastPing = now;
        }
    };
    m_ws.onDisconnected = [this] { fail("waterfall connection closed"); };
}

KiwiWaterfallClient::~KiwiWaterfallClient() {
    stop();
}

void KiwiWaterfallClient::start(const std::string& url, int speed, int zoom, double cfKhz, bool webUiPath) {
    std::lock_guard<std::mutex> life(m_lifeMtx);
    stopLocked();
    {
        std::lock_guard<std::mutex> lck(m_mtx);
        m_setup = Setup();
        m_reason.clear();
    }
    m_keyValues.clear();
    m_ready = false;
    m_failed = false;
    m_anyMessage = false;
    m_webUiPath = webUiPath;
    m_speed = std::clamp(speed, kMinSpeed, kMaxSpeed);
    m_zoom = zoom;
    m_cfKhz = cfKhz;
    m_running = true;
    m_ws.reset();
    m_thread = std::thread([this, url] {
        try {
            m_ws.connectAndReceiveLoop(url);
            fail("waterfall connection closed");
        }
        catch (const std::exception& e) {
            flog::warn("KiwiWaterfallClient: {}", e.what());
            fail(std::string("waterfall connection failed: ") + e.what());
        }
        catch (...) {
            fail("waterfall connection failed");
        }
    });
}

void KiwiWaterfallClient::stop() {
    std::lock_guard<std::mutex> life(m_lifeMtx);
    stopLocked();
}

void KiwiWaterfallClient::stopLocked() {
    m_running = false;
    m_ws.stopSocket();
    if (m_thread.joinable() && m_thread.get_id() != std::this_thread::get_id()) {
        m_thread.join();
    }
    m_ready = false;
    m_readyCv.notify_all();
}

bool KiwiWaterfallClient::waitReady(std::chrono::milliseconds timeout, const std::function<bool()>& cancel) {
    const auto deadline = Clock::now() + timeout;
    std::unique_lock<std::mutex> lck(m_mtx);
    while (!m_ready.load() && !m_failed.load() && Clock::now() < deadline) {
        if (cancel && cancel()) { return false; }
        m_readyCv.wait_for(lck, std::chrono::milliseconds(50));
    }
    if (!m_ready.load() && !m_failed.load() && m_reason.empty()) {
        m_reason = "no waterfall setup from the server";
    }
    return m_ready.load() && !m_failed.load();
}

std::string KiwiWaterfallClient::unavailableReason() const {
    std::lock_guard<std::mutex> lck(m_mtx);
    return m_reason;
}

KiwiWaterfallClient::Setup KiwiWaterfallClient::setup() const {
    std::lock_guard<std::mutex> lck(m_mtx);
    return m_setup;
}

void KiwiWaterfallClient::setView(int zoom, double cfKhz) {
    m_zoom = zoom;
    m_cfKhz = cfKhz;
    if (!available()) { return; }
    char buf[96];
    std::snprintf(buf, sizeof buf, "SET zoom=%d cf=%.3f", zoom, cfKhz);
    m_ws.queueString(buf, "view");
}

void KiwiWaterfallClient::setSpeed(int speed) {
    m_speed = std::clamp(speed, kMinSpeed, kMaxSpeed);
    if (!available()) { return; }
    m_ws.queueString("SET wf_speed=" + std::to_string(m_speed.load()), "speed");
}

bool KiwiWaterfallClient::trySend(const std::string& cmd) {
    try {
        m_ws.sendString(cmd);
        return true;
    }
    catch (const std::exception& e) {
        flog::warn("KiwiWaterfallClient: dropped command '{}': {}", cmd, e.what());
        return false;
    }
}

void KiwiWaterfallClient::onMessage(const std::string& msg) {
    m_anyMessage = true;
    if (msg.size() < 3) { return; }
    if (msg.compare(0, 3, "W/F") == 0) {
        if (!m_ready.load()) { return; }
        Frame frame;
        if (!parseFrame(msg, frame) || (frame.flags & kFlagCompressed)) { return; }
        onFrame(frame);
        return;
    }
    if (msg.compare(0, 3, "MSG") != 0) { return; }
    std::istringstream iss(msg);
    std::string token;
    iss >> token;
    while (iss >> token) {
        const auto eq = token.find('=');
        if (eq == std::string::npos) {
            onKeyValue(token, std::string());
            continue;
        }
        onKeyValue(token.substr(0, eq), url::decode(token.substr(eq + 1)));
    }
}

void KiwiWaterfallClient::onKeyValue(const std::string& key, const std::string& value) {
    if (key == "wf_setup") {
        becomeReady();
        return;
    }
    // Setup values repeat in later messages; zoom and start echo every change.
    auto it = m_keyValues.find(key);
    if (it != m_keyValues.end() && it->second == value) { return; }
    m_keyValues[key] = value;

    const std::string refused = KiwiSDRClient::refusalText(key, value);
    if (!refused.empty()) {
        fail(refused);
        m_ws.stopSocket();
        return;
    }

    std::lock_guard<std::mutex> lck(m_mtx);
    if (key == "zoom_max") { m_setup.zoomMax = std::atoi(value.c_str()); m_setup.zoomCap = std::min(m_setup.zoomCap, m_setup.zoomMax); }
    else if (key == "zoom_cap") { m_setup.zoomCap = std::atoi(value.c_str()); }
    else if (key == "wf_chans") { m_setup.wfChans = std::atoi(value.c_str()); }
    else if (key == "wf_cal") { m_setup.wfCalDb = parseDouble(value, m_setup.wfCalDb); }
    else if (key == "bandwidth") { m_setup.bandwidthHz = parseDouble(value, m_setup.bandwidthHz); }
    else if (key == "wf_fps") { m_setup.fps = std::atoi(value.c_str()); }
}

void KiwiWaterfallClient::becomeReady() {
    if (m_ready.load() || m_failed.load()) { return; }
    Setup s = setup();
    if (s.wfChans == 0) {
        fail("the server has no waterfall channels");
        m_ws.stopSocket();
        return;
    }
    if (s.zoomCap > s.zoomMax || s.zoomCap < 0) { s.zoomCap = s.zoomMax; }
    {
        std::lock_guard<std::mutex> lck(m_mtx);
        m_setup.zoomCap = s.zoomCap;
    }
    trySend("SET wf_speed=" + std::to_string(m_speed.load()));
    trySend("SET interp=13");
    trySend("SET wf_comp=0");
    // As kiwiclient and the web UI send it; the range does not change the
    // bin encoding (research section 3.2).
    trySend("SET maxdb=-10 mindb=-110");
    char buf[96];
    std::snprintf(buf, sizeof buf, "SET zoom=%d cf=%.3f", std::clamp(m_zoom.load(), 0, s.zoomCap), m_cfKhz.load());
    trySend(buf);
    flog::info("KiwiWaterfallClient: zoom_max {}, zoom_cap {}, wf_chans {}, wf_cal {}, bandwidth {}",
               s.zoomMax, s.zoomCap, s.wfChans, s.wfCalDb, s.bandwidthHz);
    m_ready = true;
    m_readyCv.notify_all();
}

void KiwiWaterfallClient::fail(const std::string& reason) {
    {
        std::lock_guard<std::mutex> lck(m_mtx);
        if (m_reason.empty() && m_running.load()) { m_reason = reason; }
    }
    m_failed = true;
    m_readyCv.notify_all();
}
