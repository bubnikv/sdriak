#include "ubersdr_client.h"

#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>

#include <utils/flog.h>
#include <utils/proto/curl_http.h>
#include <utils/url.h>
#include <utils/gzip.h>

namespace ubersdr {
    namespace {
        // Percent-encode a query value (RFC 3986 unreserved kept).
        std::string queryEscape(const std::string& v) {
            static const char* hex = "0123456789ABCDEF";
            std::string out;
            for (unsigned char c : v) {
                if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
                    out.push_back((char)c);
                }
                else {
                    out.push_back('%');
                    out.push_back(hex[c >> 4]);
                    out.push_back(hex[c & 15]);
                }
            }
            return out;
        }

        int jsonInt(const nlohmann::json& j, const char* key, int def) {
            auto it = j.find(key);
            return (it != j.end() && it->is_number()) ? it->get<int>() : def;
        }
    }

    UberSDRClient::UberSDRClient(std::string sessionId) : m_sessionId(std::move(sessionId)) {
        m_ws.onConnected = [this] {
            m_decoder.reset();
            m_streamIndex = 0;
            m_lastPing = std::chrono::steady_clock::now();
            m_connected = true;
            onConnected();
        };
        m_ws.onBinaryMessage = [this](const std::string& msg) { onBinary(msg); };
        m_ws.onTextMessage = [this](const std::string& msg) { onText(msg); };
        m_ws.onReceiveLoopTick = [this] {
            const auto now = std::chrono::steady_clock::now();
            if (m_connected.load() && now - m_lastPing > std::chrono::seconds(kPingIntervalS)) {
                trySend("{\"type\":\"ping\"}");
                m_lastPing = now;
            }
        };
        m_ws.onDisconnected = [this] {
            m_connected = false;
        };
    }

    UberSDRClient::~UberSDRClient() {
        stop();
    }

    std::string UberSDRClient::newSessionId() {
        std::random_device rd;
        std::mt19937_64 gen(((uint64_t)rd() << 32) ^ rd());
        uint64_t hi = gen(), lo = gen();
        hi = (hi & 0xFFFFFFFFFFFF0FFFull) | 0x0000000000004000ull;  // version 4
        lo = (lo & 0x3FFFFFFFFFFFFFFFull) | 0x8000000000000000ull;  // RFC 4122 variant
        char buf[37];
        std::snprintf(buf, sizeof buf, "%08x-%04x-%04x-%04x-%012llx",
                      (unsigned)(hi >> 32), (unsigned)((hi >> 16) & 0xFFFF), (unsigned)(hi & 0xFFFF),
                      (unsigned)(lo >> 48), (unsigned long long)(lo & 0xFFFFFFFFFFFFull));
        return buf;
    }

    UberSDRClient::Admission UberSDRClient::admit(const std::string& baseUrl, const std::string& password,
                                                  const std::function<bool()>& cancel) {
        Admission a;
        nlohmann::json body;
        body["user_session_id"] = m_sessionId;
        if (!password.empty()) { body["password"] = password; }
        net::http::CurlRequest req;
        req.method = "POST";
        req.url = baseUrl + "/connection";
        req.body = body.dump();
        req.contentType = "application/json";
        net::http::CurlRequestOptions options;
        options.connectTimeoutMs = 8000;
        options.timeoutMs = 10000;
        options.maxBody = 256 * 1024;
        options.headers["Accept"] = "application/json";
        options.shouldCancel = cancel;
        try {
            const auto resp = net::http::curlRequest(req, options);
            a.httpStatus = resp.status;
            auto j = nlohmann::json::parse(resp.body, nullptr, false);
            if (resp.status == 403) {
                a.reason = "wrong password";
                return a;
            }
            if (j.is_discarded() || !j.is_object()) {
                a.reason = "unexpected reply to /connection (HTTP " + std::to_string(resp.status) + ")";
                return a;
            }
            a.allowed = j.value("allowed", false);
            a.reason = j.value("reason", std::string());
            if (auto it = j.find("allowed_iq_modes"); it != j.end() && it->is_array()) {
                for (const auto& m : *it) {
                    if (m.is_string()) { a.iqModes.push_back(m.get<std::string>()); }
                }
            }
            a.maxSessionS = jsonInt(j, "max_session_time", jsonInt(j, "session_timeout", 0));
            a.dailyRemainingS = jsonInt(j, "daily_time_remaining_secs", -1);
            a.vfos = jsonInt(j, "vfos", 1);
            a.bypassed = j.value("bypassed", false);
            if (!a.allowed && a.reason.empty()) { a.reason = "the server refused the connection"; }
        }
        catch (const std::exception& e) {
            a.reason = e.what();
        }
        return a;
    }

    void UberSDRClient::start(const std::string& baseUrl, const std::string& mode, double freqHz,
                              const std::string& password, int vfo) {
        stop();

        char freq[32];
        std::snprintf(freq, sizeof freq, "%.0f", freqHz);
        std::string url = baseUrl + "/ws?frequency=" + freq + "&mode=" + queryEscape(mode) +
                          "&user_session_id=" + queryEscape(m_sessionId) + "&format=pcm-zstd&version=4";
        if (!password.empty()) { url += "&password=" + queryEscape(password); }
        if (vfo > 0) { url += "&vfo=" + std::to_string(vfo); }

        m_running = true;
        m_ws.reset();
        m_thread = std::thread([this, url] {
            try {
                m_ws.connectAndReceiveLoop(url);
            }
            catch (const std::exception& e) {
                flog::warn("UberSDRClient: {}", e.what());
                if (m_running.load()) { onError(e.what()); }
            }
            catch (...) {
                if (m_running.load()) { onError("connection failed"); }
            }
            m_connected = false;
            if (m_running.exchange(false)) { onDisconnected(); }
        });
    }

    void UberSDRClient::stop() {
        m_running = false;
        m_ws.stopSocket();
        if (m_thread.joinable()) { m_thread.join(); }
        m_connected = false;
    }

    bool UberSDRClient::trySend(const std::string& msg) {
        try {
            m_ws.sendString(msg);
            return true;
        }
        catch (const std::exception& e) {
            flog::warn("UberSDRClient: dropped '{}': {}", msg, e.what());
            return false;
        }
    }

    void UberSDRClient::tune(double freqHz, const std::string& mode) {
        if (!m_connected.load()) { return; }
        nlohmann::json j;
        j["type"] = "tune";
        j["frequency"] = (int64_t)std::llround(freqHz);
        j["mode"] = mode;
        m_ws.queueString(j.dump(), "tune");
    }

    void UberSDRClient::requestStatus() {
        if (!m_connected.load()) { return; }
        m_ws.queueString("{\"type\":\"get_status\"}", "status");
    }

    void UberSDRClient::onText(const std::string& msg) {
        auto j = nlohmann::json::parse(msg, nullptr, false);
        if (j.is_discarded() || !j.is_object()) { return; }
        const std::string type = j.value("type", std::string());
        if (type == "status") {
            onStatus(j);
        }
        else if (type == "error") {
            const std::string text = j.value("error", j.value("message", std::string("server error")));
            flog::warn("UberSDRClient: server error: {}", text);
            onError(text);
        }
    }

    void UberSDRClient::onBinary(const std::string& msg) {
        const auto* p = reinterpret_cast<const uint8_t*>(msg.data());
        if (PCMv4StreamDecoder::isZstdFrame(p, msg.size())) {
            onError("the server speaks an older audio protocol (before UberSDR 0.1.63)");
            m_ws.stopSocket();
            return;
        }
        if (!PCMv4StreamDecoder::isV4Frame(p, msg.size())) {
            onError("unsupported UberSDR audio wire format (requires PCM v4)");
            m_ws.stopSocket();
            return;
        }
        PCMv4Header h;
        std::string err;
        if (!m_decoder.decode(p, msg.size(), h, err)) {
            // The predictor state is lost with this packet; a new connection
            // starts a fresh decoder.
            onError("audio decoder: " + err);
            m_ws.stopSocket();
            return;
        }
        if (h.channels != 2) {
            onError("the server sent audio instead of two-channel IQ");
            m_ws.stopSocket();
            return;
        }
        if (h.sampleCount < 2) { return; }
        const int count = h.sampleCount / 2;
        const int16_t* s = m_decoder.samples();
        m_samples.resize((size_t)count);
        for (int i = 0; i < count; i++) {
            m_samples[(size_t)i] = dsp::complex_t{ (float)s[2 * i] / 32768.0f, (float)s[2 * i + 1] / 32768.0f };
        }
        PacketInfo info;
        info.timestampNanos = h.timestampNanos;
        info.sampleRate = h.sampleRate;
        info.basebandPower = h.basebandPower;
        info.noise = h.noise;
        info.streamIndex = m_streamIndex;
        m_streamIndex += (uint64_t)count;
        onIQ(info, m_samples.data(), count);
    }

    // ---- UberSpectrumClient ----

    namespace {
        uint16_t le16(const uint8_t* b) { return (uint16_t)(b[0] | (b[1] << 8)); }
        uint64_t le64(const uint8_t* b) {
            uint64_t v = 0;
            for (int i = 7; i >= 0; i--) { v = (v << 8) | b[i]; }
            return v;
        }
        constexpr size_t kSpecHeader = 24;
        constexpr uint8_t kFlagFull = 0x05;
        constexpr uint8_t kFlagDelta = 0x06;
    }

    UberSpectrumClient::UberSpectrumClient() {
        m_ws.onConnected = [this] {
            m_haveFull = false;
            m_haveSeq = false;
            m_lastPing = std::chrono::steady_clock::now();
            m_connected = true;
        };
        m_ws.onBinaryMessage = [this](const std::string& msg) { onBinary(msg); };
        m_ws.onTextMessage = [this](const std::string& msg) { onControl(msg); };
        m_ws.onReceiveLoopTick = [this] {
            const auto now = std::chrono::steady_clock::now();
            if (m_connected.load() && now - m_lastPing > std::chrono::seconds(kPingIntervalS)) {
                trySend("{\"type\":\"ping\"}");
                m_lastPing = now;
            }
        };
        m_ws.onDisconnected = [this] { m_connected = false; };
    }

    UberSpectrumClient::~UberSpectrumClient() {
        stop();
    }

    void UberSpectrumClient::start(const std::string& baseUrl, const std::string& sessionId, double freqHz,
                                   double binBandwidth, const std::string& password) {
        stop();
        char buf[64];
        std::string url = baseUrl + "/ws/user-spectrum?user_session_id=" + queryEscape(sessionId) + "&mode=binary8&version=2";
        if (freqHz > 0.0) {
            std::snprintf(buf, sizeof buf, "%.0f", freqHz);
            url += std::string("&frequency=") + buf;
        }
        if (binBandwidth > 0.0) {
            std::snprintf(buf, sizeof buf, "%.3f", binBandwidth);
            url += std::string("&bin_bandwidth=") + buf;
        }
        if (!password.empty()) { url += "&password=" + queryEscape(password); }
        m_running = true;
        m_ws.reset();
        m_thread = std::thread([this, url] {
            try {
                m_ws.connectAndReceiveLoop(url);
            }
            catch (const std::exception& e) {
                flog::warn("UberSpectrumClient: {}", e.what());
                onError(e.what(), 0);
            }
            catch (...) {
                onError("spectrum connection failed", 0);
            }
            m_connected = false;
            m_running = false;
        });
    }

    void UberSpectrumClient::stop() {
        m_running = false;
        m_ws.stopSocket();
        if (m_thread.joinable()) { m_thread.join(); }
        m_connected = false;
    }

    bool UberSpectrumClient::trySend(const std::string& msg) {
        try {
            m_ws.sendString(msg);
            return true;
        }
        catch (const std::exception& e) {
            flog::warn("UberSpectrumClient: dropped '{}': {}", msg, e.what());
            return false;
        }
    }

    void UberSpectrumClient::zoom(double freqHz, double binBandwidth) {
        if (!m_connected.load()) { return; }
        nlohmann::json j;
        j["type"] = "zoom";
        j["frequency"] = (int64_t)std::llround(freqHz);
        j["binBandwidth"] = binBandwidth;
        m_ws.queueString(j.dump(), "zoom");
    }

    void UberSpectrumClient::pan(double freqHz, double binBandwidth) {
        if (!m_connected.load()) { return; }
        nlohmann::json j;
        j["type"] = "pan";
        j["frequency"] = (int64_t)std::llround(freqHz);
        j["binBandwidth"] = binBandwidth;
        m_ws.queueString(j.dump(), "pan");
    }

    void UberSpectrumClient::onBinary(const std::string& msg) {
        const auto* p = reinterpret_cast<const uint8_t*>(msg.data());
        // Control replies are gzip-compressed JSON in binary frames.
        if (gzip::isGzip(p, msg.size())) {
            std::string text;
            if (gzip::gunzip(p, msg.size(), text, 1024 * 1024)) { onControl(text); }
            return;
        }
        Frame f;
        if (parseFrame(p, msg.size(), f)) { onFrame(f); }
    }

    void UberSpectrumClient::onControl(const std::string& text) {
        auto j = nlohmann::json::parse(text, nullptr, false);
        if (j.is_discarded() || !j.is_object()) { return; }
        const std::string type = j.value("type", std::string());
        auto num = [&j](const char* key, double def) {
            auto it = j.find(key);
            return (it != j.end() && it->is_number()) ? it->get<double>() : def;
        };
        if (type == "config" || type == "status") {
            Config c;
            c.centreHz = num("centerFreq", num("frequency", 0.0));
            c.binCount = (int)num("binCount", 0.0);
            c.binBandwidth = num("binBandwidth", 0.0);
            c.totalBandwidth = num("totalBandwidth", c.binCount * c.binBandwidth);
            if (c.binCount > 0 && c.binBandwidth > 0.0) { onConfig(c); }
        }
        else if (type == "error") {
            const int status = (int)num("status", 0.0);
            onError(j.value("error", j.value("message", std::string("spectrum error"))), status);
        }
    }

    bool UberSpectrumClient::parseFrame(const uint8_t* p, size_t len, Frame& f) {
        // "SPEC", version 2, flags, seq (LE16), timestamp ns (LE64), centre Hz (LE64).
        if (len < kSpecHeader || std::memcmp(p, "SPEC", 4) != 0 || p[4] != 2) { return false; }
        const uint8_t flags = p[5];
        const uint16_t seq = le16(p + 6);
        f.timestampNanos = le64(p + 8);
        f.centreHz = (double)le64(p + 16);
        f.seq = seq;
        // A lost frame breaks the delta chain until the next full frame.
        if (m_haveSeq && seq != (uint16_t)(m_lastSeq + 1)) { m_haveFull = false; }
        m_haveSeq = true;
        m_lastSeq = seq;
        const uint8_t* body = p + kSpecHeader;
        const size_t bodyLen = len - kSpecHeader;
        if (flags == kFlagFull) {
            // ref (int16 centi-dB), step (uint8 centi-dB), one code per bin.
            if (bodyLen < 3) { return false; }
            m_refCentiDb = (int16_t)le16(body);
            m_stepCentiDb = body[2];
            m_codes.assign(body + 3, body + bodyLen);
            m_haveFull = !m_codes.empty();
        }
        else if (flags == kFlagDelta) {
            // A bitmask of changed bins, then their codes, on the full
            // frame's scale.
            if (!m_haveFull) { return false; }
            const size_t n = m_codes.size();
            const size_t maskLen = (n + 7) / 8;
            if (bodyLen < maskLen) {
                m_haveFull = false;
                return false;
            }
            size_t next = maskLen;
            for (size_t k = 0; k < n; k++) {
                if (body[k >> 3] & (1u << (k & 7))) {
                    if (next >= bodyLen) {
                        m_haveFull = false;
                        return false;
                    }
                    m_codes[k] = body[next++];
                }
            }
        }
        else {
            return false;
        }
        // FFT order on the wire (DC first, negative frequencies in the upper
        // half): rotate by N/2.
        const size_t n = m_codes.size();
        m_db.resize(n);
        const size_t half = n / 2;
        for (size_t k = 0; k < n; k++) {
            const uint8_t code = m_codes[(k + half) % n];
            m_db[k] = (float)m_refCentiDb / 100.0f + (float)code * (float)m_stepCentiDb / 100.0f;
        }
        f.binsDb = m_db.data();
        f.count = (int)n;
        return true;
    }
}

