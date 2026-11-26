#pragma once
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <dsp/types.h>
#include <json.hpp>
#include <utils/proto/websock.h>

#include "owrx_adpcm.h"

namespace owrx {
    // Client for the OpenWebRX receiver socket (/ws/): handshake, DSP control
    // in the "iq" mode of OpenWebRX+ 1.2.129 and later, the profile's FFT and
    // the profile list. Protocol facts from the OpenWebRX+ source (AGPL-3.0,
    // no code copied) and live sessions: doc/research/openwebrx.md.
    class OpenWebRxClient {
    public:
        // The merged `config` messages: the parts this client uses.
        struct Config {
            double centerFreq = 0.0;
            double sampRate = 0.0;
            int fftSize = 0;
            std::string audioCompression;  // "none" or "adpcm"
            std::string fftCompression;
            float levelMin = -88.0f, levelMax = -20.0f;
            bool haveLevels = false;
            std::string sdrId, profileId;
            std::string startMod;
            double startOffsetFreq = 0.0;

            // centre, rate and FFT size known: enough to start.
            bool complete() const { return centerFreq > 0.0 && sampRate > 0.0 && fftSize > 0; }
            std::string profileKey() const { return sdrId + "|" + profileId; }
        };
        struct Profile {
            std::string id;  // "<sdr>|<profile>"
            std::string name;
        };
        struct Details {
            std::string name, location;
            int sessionTimeoutS = 0;
        };

        // The IQ mode's own rate: hd_output_rate is set to it, so the server
        // does not resample.
        static constexpr int kIqRate = 48000;
        // The cuts the IQ is always sent with: without them the IQ keeps the
        // profile's start-mode filter.
        static constexpr int kIqCut = 24000;
        static constexpr int kMinVersion[3] = { 1, 2, 129 };

        OpenWebRxClient();
        ~OpenWebRxClient();
        OpenWebRxClient(const OpenWebRxClient&) = delete;
        OpenWebRxClient& operator=(const OpenWebRxClient&) = delete;

        // "host", "host:port" or a URL, to ws[s]://host[:port][/path]/ws/.
        // Without a scheme: http.
        static std::string socketUrl(const std::string& receiverUrl);
        // "v1.2.131" or "1.3.0-dev": true when at least kMinVersion.
        static bool versionSupportsIq(const std::string& version);

        // Opens the socket on a thread of its own and handshakes. IQ starts
        // only with startIq().
        void start(const std::string& receiverUrl);
        // Close and join. Any thread except the client's own; idempotent.
        void stop();
        bool running() const { return m_running.load(); }
        bool connected() const { return m_connected.load(); }

        // Any thread; dropped while not connected. offsetHz is from the
        // profile centre, in hardware Hz.
        void startIq(double offsetHz);
        void setOffset(double offsetHz);
        void selectProfile(const std::string& profileKey);

        // Any thread: copies of what the server reported.
        std::string serverVersion() const;
        Config config() const;
        std::vector<Profile> profiles() const;
        std::vector<std::string> modes() const;
        bool hasIqMode() const;
        bool hasModeList() const { return m_hasModeList.load(); }
        Details details() const;
        int clients() const { return m_clients.load(); }

        // Network thread. onIQ gets complex samples scaled to +-1.
        std::function<void()> onHandshake = [] {};
        // Every merged config; profileChanged when the centre or rate moved
        // after the first complete config (another listener's profile change).
        std::function<void(const Config&, bool profileChanged)> onConfig = [](const Config&, bool) {};
        std::function<void(const dsp::complex_t*, int)> onIQ = [](const dsp::complex_t*, int) {};
        // One FFT row, fftSize bins in display order, dB as sent.
        std::function<void(const float*, int)> onFftRow = [](const float*, int) {};
        std::function<void(const std::string&)> onError = [](const std::string&) {};
        std::function<void()> onDisconnected = [] {};

        // Network thread (public for tests): one message as received.
        void handleText(const std::string& msg);
        void handleBinary(const uint8_t* data, size_t len);

    private:
        bool trySend(const std::string& msg);
        void sendDsp(const nlohmann::json& params, const char* key = "");

        net::websock::WSClient m_ws;
        std::mutex m_lifeMtx;   // start() / stop()
        std::thread m_thread;
        std::atomic<bool> m_running{false};
        std::atomic<bool> m_hasModeList{false};
        std::atomic<bool> m_connected{false};
        std::atomic<bool> m_handshaken{false};

        mutable std::mutex m_stateMtx;  // what the server reported
        std::string m_version;
        Config m_config;
        std::vector<Profile> m_profiles;
        std::vector<std::string> m_modes;
        Details m_details;
        std::atomic<int> m_clients{0};

        // Network thread.
        bool m_haveComplete = false;
        double m_lastCentre = 0.0, m_lastRate = 0.0;
        StereoSyncAdpcm m_iqDecoder;
        std::vector<int16_t> m_chA, m_chB;
        std::vector<dsp::complex_t> m_samples;
        std::vector<float> m_row;
    };
}
