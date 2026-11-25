#include "owrx_client.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>

#include <utils/flog.h>

namespace owrx {
    namespace {
        constexpr uint8_t kTypeFft = 1;
        constexpr uint8_t kTypeHdStereo = 6;

        double num(const nlohmann::json& j, const char* key, double def) {
            auto it = j.find(key);
            return (it != j.end() && it->is_number()) ? it->get<double>() : def;
        }
        std::string str(const nlohmann::json& j, const char* key) {
            auto it = j.find(key);
            if (it == j.end()) { return std::string(); }
            if (it->is_string()) { return it->get<std::string>(); }
            if (it->is_number_integer()) { return std::to_string(it->get<long long>()); }
            return std::string();
        }

        // What a failed connect means to a user.
        std::string explainFailure(const std::string& what) {
            if (what.find("HTTP 403") != std::string::npos) { return "this receiver blocks non-browser clients (HTTP 403)"; }
            if (what.find("HTTP 404") != std::string::npos) { return "no OpenWebRX receiver socket at this address (HTTP 404)"; }
            return what;
        }
    }

    OpenWebRxClient::OpenWebRxClient() {
        m_ws.onConnected = [this] {
            m_haveComplete = false;
            m_lastCentre = 0.0;
            m_lastRate = 0.0;
            m_iqDecoder.reset();
            m_handshaken = false;
            m_connected = true;
            trySend("SERVER DE CLIENT client=sdriak type=receiver");
        };
        m_ws.onTextMessage = [this](const std::string& msg) { handleText(msg); };
        m_ws.onBinaryMessage = [this](const std::string& msg) {
            handleBinary(reinterpret_cast<const uint8_t*>(msg.data()), msg.size());
        };
        m_ws.onDisconnected = [this] {
            const bool wasHandshaken = m_handshaken.load();
            m_connected = false;
            if (!wasHandshaken && m_running.load()) { onError("the server closed the connection before the handshake"); }
            onDisconnected();
        };
    }

    OpenWebRxClient::~OpenWebRxClient() {
        stop();
    }

    std::string OpenWebRxClient::socketUrl(const std::string& receiverUrl) {
        std::string s = receiverUrl;
        while (!s.empty() && (s.back() == ' ' || s.back() == '/')) { s.pop_back(); }
        while (!s.empty() && s.front() == ' ') { s.erase(s.begin()); }
        // A receiver page URL may carry a fragment or query (#freq=...).
        if (auto cut = s.find_first_of("#?"); cut != std::string::npos) { s.erase(cut); }
        while (!s.empty() && s.back() == '/') { s.pop_back(); }
        if (s.find("://") == std::string::npos) { s = "http://" + s; }
        if (s.rfind("https://", 0) == 0) { s = "wss://" + s.substr(8); }
        else if (s.rfind("http://", 0) == 0) { s = "ws://" + s.substr(7); }
        // Already pointing at the socket.
        if (s.size() >= 3 && s.compare(s.size() - 3, 3, "/ws") == 0) { return s + "/"; }
        return s + "/ws/";
    }

    bool OpenWebRxClient::versionSupportsIq(const std::string& version) {
        int v[3] = { 0, 0, 0 };
        size_t i = (!version.empty() && (version[0] == 'v' || version[0] == 'V')) ? 1 : 0;
        for (int part = 0; part < 3; part++) {
            if (i >= version.size() || !std::isdigit((unsigned char)version[i])) { break; }
            while (i < version.size() && std::isdigit((unsigned char)version[i])) {
                v[part] = v[part] * 10 + (version[i] - '0');
                i++;
            }
            if (i < version.size() && version[i] == '.') { i++; }
            else { break; }
        }
        for (int part = 0; part < 3; part++) {
            if (v[part] != kMinVersion[part]) { return v[part] > kMinVersion[part]; }
        }
        return true;
    }

    void OpenWebRxClient::start(const std::string& receiverUrl) {
        std::lock_guard<std::mutex> life(m_lifeMtx);
        m_running = false;
        m_ws.stopSocket();
        if (m_thread.joinable() && m_thread.get_id() != std::this_thread::get_id()) { m_thread.join(); }
        {
            std::lock_guard lck(m_stateMtx);
            m_version.clear();
            m_config = Config();
            m_profiles.clear();
            m_modes.clear();
            m_details = Details();
        }
        m_clients = 0;
        const std::string url = socketUrl(receiverUrl);
        m_running = true;
        m_ws.reset();
        m_thread = std::thread([this, url] {
            try {
                m_ws.connectAndReceiveLoop(url);
            }
            catch (const std::exception& e) {
                flog::warn("OpenWebRxClient: {}", e.what());
                onError(explainFailure(e.what()));
            }
            catch (...) {
                onError("connection failed");
            }
            m_connected = false;
            m_running = false;
        });
    }

    void OpenWebRxClient::stop() {
        std::lock_guard<std::mutex> life(m_lifeMtx);
        m_running = false;
        m_ws.stopSocket();
        if (m_thread.joinable() && m_thread.get_id() != std::this_thread::get_id()) { m_thread.join(); }
        m_connected = false;
    }

    bool OpenWebRxClient::trySend(const std::string& msg) {
        try {
            m_ws.sendString(msg);
            return true;
        }
        catch (const std::exception& e) {
            flog::warn("OpenWebRxClient: dropped '{}': {}", msg, e.what());
            return false;
        }
    }

    void OpenWebRxClient::sendDsp(const nlohmann::json& params) {
        nlohmann::json j;
        j["type"] = "dspcontrol";
        j["params"] = params;
        trySend(j.dump());
    }

    void OpenWebRxClient::startIq(double offsetHz) {
        if (!m_connected.load()) { return; }
        trySend("{\"type\":\"dspcontrol\",\"action\":\"start\"}");
        nlohmann::json p;
        p["mod"] = "iq";
        p["offset_freq"] = (int64_t)std::llround(offsetHz);
        p["low_cut"] = -kIqCut;
        p["high_cut"] = kIqCut;
        sendDsp(p);
    }

    void OpenWebRxClient::setOffset(double offsetHz) {
        if (!m_connected.load()) { return; }
        nlohmann::json p;
        p["offset_freq"] = (int64_t)std::llround(offsetHz);
        sendDsp(p);
    }

    void OpenWebRxClient::selectProfile(const std::string& profileKey) {
        if (!m_connected.load()) { return; }
        nlohmann::json j;
        j["type"] = "selectprofile";
        j["params"]["profile"] = profileKey;
        trySend(j.dump());
    }

    std::string OpenWebRxClient::serverVersion() const {
        std::lock_guard lck(m_stateMtx);
        return m_version;
    }
    OpenWebRxClient::Config OpenWebRxClient::config() const {
        std::lock_guard lck(m_stateMtx);
        return m_config;
    }
    std::vector<OpenWebRxClient::Profile> OpenWebRxClient::profiles() const {
        std::lock_guard lck(m_stateMtx);
        return m_profiles;
    }
    std::vector<std::string> OpenWebRxClient::modes() const {
        std::lock_guard lck(m_stateMtx);
        return m_modes;
    }
    bool OpenWebRxClient::hasIqMode() const {
        std::lock_guard lck(m_stateMtx);
        for (const auto& m : m_modes) {
            if (m == "iq") { return true; }
        }
        return false;
    }
    OpenWebRxClient::Details OpenWebRxClient::details() const {
        std::lock_guard lck(m_stateMtx);
        return m_details;
    }

    void OpenWebRxClient::handleText(const std::string& msg) {
        // "CLIENT DE SERVER server=openwebrx version=v1.2.131"
        if (msg.rfind("CLIENT DE SERVER", 0) == 0) {
            std::string version;
            if (auto at = msg.find("version="); at != std::string::npos) {
                const auto end = msg.find(' ', at);
                version = msg.substr(at + 8, end == std::string::npos ? std::string::npos : end - at - 8);
            }
            if (msg.find("server=openwebrx") == std::string::npos) {
                onError("not an OpenWebRX receiver");
                m_ws.stopSocket();
                return;
            }
            {
                std::lock_guard lck(m_stateMtx);
                m_version = version;
            }
            m_handshaken = true;
            nlohmann::json j;
            j["type"] = "connectionproperties";
            j["params"]["output_rate"] = 12000;
            j["params"]["hd_output_rate"] = kIqRate;
            trySend(j.dump());
            onHandshake();
            return;
        }
        auto j = nlohmann::json::parse(msg, nullptr, false);
        if (j.is_discarded() || !j.is_object()) { return; }
        const std::string type = j.value("type", std::string());
        // The one message without a "value": sent before the server closes
        // ("Too many clients", "Client address banned").
        if (type == "backoff") {
            onError("the receiver refused the connection: " + str(j, "reason"));
            return;
        }
        auto valueIt = j.find("value");
        if (valueIt == j.end()) { return; }
        const nlohmann::json& v = *valueIt;

        if (type == "config" && v.is_object()) {
            Config c;
            {
                std::lock_guard lck(m_stateMtx);
                Config& m = m_config;
                m.centerFreq = num(v, "center_freq", m.centerFreq);
                m.sampRate = num(v, "samp_rate", m.sampRate);
                m.fftSize = (int)num(v, "fft_size", m.fftSize);
                if (v.contains("audio_compression")) { m.audioCompression = str(v, "audio_compression"); }
                if (v.contains("fft_compression")) { m.fftCompression = str(v, "fft_compression"); }
                if (auto it = v.find("waterfall_levels"); it != v.end() && it->is_object()) {
                    m.levelMin = (float)num(*it, "min", m.levelMin);
                    m.levelMax = (float)num(*it, "max", m.levelMax);
                    m.haveLevels = true;
                }
                if (v.contains("sdr_id")) { m.sdrId = str(v, "sdr_id"); }
                if (v.contains("profile_id")) { m.profileId = str(v, "profile_id"); }
                if (v.contains("start_mod")) { m.startMod = str(v, "start_mod"); }
                m.startOffsetFreq = num(v, "start_offset_freq", m.startOffsetFreq);
                c = m;
            }
            if (!c.complete()) { return; }
            bool changed = false;
            if (m_haveComplete) {
                changed = c.centerFreq != m_lastCentre || c.sampRate != m_lastRate;
            }
            m_haveComplete = true;
            m_lastCentre = c.centerFreq;
            m_lastRate = c.sampRate;
            onConfig(c, changed);
        }
        else if (type == "profiles" && v.is_array()) {
            std::vector<Profile> list;
            for (const auto& p : v) {
                if (!p.is_object()) { continue; }
                Profile pr{ str(p, "id"), str(p, "name") };
                if (!pr.id.empty()) { list.push_back(std::move(pr)); }
            }
            std::lock_guard lck(m_stateMtx);
            m_profiles = std::move(list);
        }
        else if (type == "modes" && v.is_array()) {
            std::vector<std::string> list;
            for (const auto& m : v) {
                if (m.is_object()) { list.push_back(str(m, "modulation")); }
                else if (m.is_string()) { list.push_back(m.get<std::string>()); }
            }
            std::lock_guard lck(m_stateMtx);
            m_modes = std::move(list);
        }
        else if (type == "receiver_details" && v.is_object()) {
            std::lock_guard lck(m_stateMtx);
            m_details.name = str(v, "receiver_name");
            m_details.location = str(v, "receiver_location");
            m_details.sessionTimeoutS = (int)num(v, "session_timeout", 0.0);
        }
        else if (type == "clients" && v.is_number()) {
            m_clients = v.get<int>();
        }
        else if (type == "sdr_error" || type == "demodulator_error") {
            const std::string text = v.is_string() ? v.get<std::string>() : v.dump();
            flog::warn("OpenWebRxClient: {}: {}", type, text);
            onError(text);
        }
        else if (type == "log_message" && v.is_string()) {
            flog::info("OpenWebRxClient: server: {}", v.get<std::string>());
        }
    }

    void OpenWebRxClient::handleBinary(const uint8_t* data, size_t len) {
        if (len < 2) { return; }
        const uint8_t type = data[0];
        const uint8_t* body = data + 1;
        const size_t bodyLen = len - 1;
        if (type == kTypeFft) {
            Config c;
            {
                std::lock_guard lck(m_stateMtx);
                c = m_config;
            }
            if (c.fftSize <= 0) { return; }
            // The first message after connecting may hold a backlog of rows.
            if (c.fftCompression == "adpcm") {
                const size_t row = FftAdpcm::rowBytes(c.fftSize);
                for (size_t at = 0; at + row <= bodyLen; at += row) {
                    FftAdpcm::decodeRow(body + at, row, c.fftSize, m_row);
                    if ((int)m_row.size() == c.fftSize) { onFftRow(m_row.data(), c.fftSize); }
                }
            }
            else {
                const size_t row = (size_t)c.fftSize * 4;
                for (size_t at = 0; at + row <= bodyLen; at += row) {
                    m_row.resize((size_t)c.fftSize);
                    std::memcpy(m_row.data(), body + at, row);
                    onFftRow(m_row.data(), c.fftSize);
                }
            }
        }
        else if (type == kTypeHdStereo) {
            bool adpcm;
            {
                std::lock_guard lck(m_stateMtx);
                adpcm = m_config.audioCompression == "adpcm";
            }
            int count;
            if (adpcm) {
                m_chA.clear();
                m_chB.clear();
                m_iqDecoder.decode(body, bodyLen, m_chA, m_chB);
                count = (int)std::min(m_chA.size(), m_chB.size());
                m_samples.resize((size_t)count);
                for (int i = 0; i < count; i++) {
                    m_samples[(size_t)i] = dsp::complex_t{ (float)m_chA[(size_t)i] / 32768.0f, (float)m_chB[(size_t)i] / 32768.0f };
                }
            }
            else {
                count = (int)(bodyLen / 4);
                m_samples.resize((size_t)count);
                for (int i = 0; i < count; i++) {
                    const uint8_t* p = body + 4 * (size_t)i;
                    const int16_t re = (int16_t)(p[0] | (p[1] << 8));
                    const int16_t im = (int16_t)(p[2] | (p[3] << 8));
                    m_samples[(size_t)i] = dsp::complex_t{ (float)re / 32768.0f, (float)im / 32768.0f };
                }
            }
            if (count > 0) { onIQ(m_samples.data(), count); }
        }
    }
}
