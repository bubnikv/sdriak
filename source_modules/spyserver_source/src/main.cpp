#include <spyserver_client.h>
#include <imgui.h>
#include <utils/flog.h>
#include <module.h>
#include <gui/gui.h>
#include <signal_path/signal_path.h>
#include <core.h>
#include <gui/style.h>
#include <config.h>
#include <gui/widgets/stepped_slider.h>
#include <gui/smgui.h>
#include <utils/async_connector.h>
#include <dsp/buffer/prebuffer.h>
#include <signal_path/spectrum_provider.h>
#include <signal_path/iq_window_policy.h>
#include <gui/remote/browser.h>
#include <gui/remote/recent.h>
#include <atomic>
#include <thread>
#include <chrono>
#include <mutex>
#include <algorithm>
#include <cmath>
#include <stdexcept>


#define CONCAT(a, b) ((std::string(a) + b).c_str())

SDRPP_MOD_INFO{
    /* Name:            */ "spyserver_source",
    /* Description:     */ "SpyServer source module for SDRIAK",
    /* Author:          */ "Ryzerth",
    /* Version:         */ 0, 1, 0,
    /* Max instances    */ 1
};

const char* deviceTypesStr[] = {
    "Unknown",
    "Airspy One",
    "Airspy HF+",
    "RTL-SDR"
};

const char* streamFormatStr = "UInt8\0"
                              "Int16\0"
                              "Float32\0";

const SpyServerStreamFormat streamFormats[] = {
    SPYSERVER_STREAM_FORMAT_UINT8,
    SPYSERVER_STREAM_FORMAT_INT16,
    SPYSERVER_STREAM_FORMAT_FLOAT
};

const int streamFormatsBitCount[] = {
    8,
    16,
    32
};

// Full IQ streams the device's IQ at the chosen rate. VFO + server waterfall
// (SpyServer's FFT_IQ mode) streams a narrow IQ window plus the server's own
// spectrum of the whole device span. The session order follows jp's
// spyserver_vfo_source (SDRPlusPlus-jp); the protocol facts are in
// doc/research/spyserver-streams.md.
const char* modeStr = "Full IQ\0"
                      "VFO + server waterfall\0";
const char* modeKeys[] = { "iq", "vfo_fft" };

constexpr int FFT_DISPLAY_PIXELS = 2048;

ConfigManager config;

class SpyServerSourceModule : public ModuleManager::Instance {
public:
    SpyServerSourceModule(std::string name) {
        this->name = name;

        std::string host;
        {
            auto configAccess = config.read();
            configAccess.tryGet("hostname", host);
            configAccess.tryGet("port", port);
            configAccess.tryGet("viaProxy", viaProxy);
        }

        handler.ctx = this;
        handler.selectHandler = menuSelected;
        handler.deselectHandler = menuDeselected;
        handler.menuHandler = menuHandler;
        handler.startHandler = start;
        handler.stopHandler = stop;
        handler.tuneHandler = tune;
        handler.uiTickHandler = uiTick;
        handler.stream = &stream;
        provider.module = this;

        // The client writes rawStream; the link (bypass by default) feeds the
        // source stream and gives both modes stream-position markers.
        chain.init(&rawStream, &stream);
        chain.setPrebufferMsec(0, false, false);

        strcpy(hostname, host.c_str());

        sigpath::sourceManager.registerSource("SpyServer", &handler);

        // The remote browser runs in the GUI only.
        if (!core::args["server"].b()) {
            remote::RemoteHandler remoteHandler;
            remoteHandler.kind = remote::RemoteKind::SpyServer;
            remoteHandler.sourceName = "SpyServer";
            remoteHandler.probe = [this](const remote::RemoteEntry& entry, const std::atomic<bool>& cancel) {
                return probe(entry, cancel, viaProxy);
            };
            remoteHandler.connect = [this](const remote::RemoteEntry& entry) { connectRemote(entry); };
            sigpath::remoteRegistry.registerHandler(remoteHandler);
        }
    }

    ~SpyServerSourceModule() {
        if (!core::args["server"].b()) { sigpath::remoteRegistry.unregisterHandler(remote::RemoteKind::SpyServer); }
        stop(this);
        // Stop the connect worker before members it uses are destroyed.
        connector.shutdown();
        sigpath::sourceManager.unregisterSource("SpyServer");
    }

    void postInit() {}

    void enable() {
        enabled = true;
    }

    void disable() {
        enabled = false;
    }

    bool isEnabled() {
        return enabled;
    }

private:
    // The remote browser's Test: handshake until DEVICE_INFO and the first
    // CLIENT_SYNC, then close. canControl depends on who else is connected,
    // so it comes from here, not from the directory. Worker thread.
    static remote::ProbeResult probe(const remote::RemoteEntry& entry, const std::atomic<bool>& cancel, bool throughProxy) {
        using Clock = std::chrono::steady_clock;
        remote::ProbeResult result;
        dsp::stream<dsp::complex_t> sink;
        const auto t0 = Clock::now();
        auto client = spyserver::connect(entry.target.host, (uint16_t)entry.target.port, &sink, 3000, throughProxy);
        if (!client) {
            result.reason = "could not connect";
            return result;
        }
        if (cancel || !client->waitForDevInfo(3000)) {
            client->close();
            result.reason = cancel ? "cancelled" : "no device information";
            return result;
        }
        result.rttMs = (int)std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count();
        const auto syncStart = Clock::now();
        while (!cancel && client->syncCount() == 0 && Clock::now() - syncStart < std::chrono::seconds(2)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        const auto& info = client->devInfo;
        constexpr uint32_t DEVICE_TYPE_COUNT = sizeof(deviceTypesStr) / sizeof(deviceTypesStr[0]);
        result.negotiated.emplace_back("Device", info.DeviceType < DEVICE_TYPE_COUNT ? deviceTypesStr[info.DeviceType] : "unknown");
        result.negotiated.emplace_back("Max sample rate", std::to_string(info.MaximumSampleRate) + " S/s");
        if (client->syncCount() > 0) {
            result.negotiated.emplace_back("Control", client->canControl() ? "yes (you can tune the device)" : "no (another client controls it)");
        }
        client->close();
        result.ok = !cancel;
        if (cancel) { result.reason = "cancelled"; }
        return result;
    }

    // The remote browser's Connect: the browser has selected this source.
    void connectRemote(const remote::RemoteEntry& entry) {
        if (client && client->isOpen()) { client->close(); }
        std::snprintf(hostname, sizeof hostname, "%s", entry.target.host.c_str());
        port = entry.target.port;
        {
            auto configAccess = config.edit();
            configAccess.set("hostname", std::string(hostname));
            configAccess.set("port", port);
        }
        tryConnect();
    }

    std::string getBandwdithScaled(double bw) {
        char buf[1024];
        if (bw >= 1000000.0) {
            sprintf(buf, "%.1lfMHz", bw / 1000000.0);
        }
        else if (bw >= 1000.0) {
            sprintf(buf, "%.1lfKHz", bw / 1000.0);
        }
        else {
            sprintf(buf, "%.1lfHz", bw);
        }
        return std::string(buf);
    }

    static void menuSelected(void* ctx) {
        SpyServerSourceModule* _this = (SpyServerSourceModule*)ctx;
        _this->applySampleRate();
        gui::mainWindow.playButtonLocked = !(_this->client && _this->client->isOpen());
        flog::info("SpyServerSourceModule '{0}': Menu Select!", _this->name);
    }

    static void menuDeselected(void* ctx) {
        SpyServerSourceModule* _this = (SpyServerSourceModule*)ctx;
        _this->unregisterProvider();
        _this->connector.cancel();
        gui::mainWindow.playButtonLocked = false;
        flog::info("SpyServerSourceModule '{0}': Menu Deselect!", _this->name);
    }

    static void start(void* ctx) {
        SpyServerSourceModule* _this = (SpyServerSourceModule*)ctx;
        if (_this->running) { return; }
        
        // Try to connect if not already connected
        if (!_this->client) {
            _this->tryConnect();
            if (!_this->client) { return; }
        }

        const int stage = _this->iqStage();
        _this->chain.setSampleRate(_this->sampleRate);
        // A block left unread by the last session would shift this
        // session's input indices (PrebufferedLink::discardUnreadInput).
        _this->chain.discardUnreadInput();
        _this->chain.start();
        // The link has consumed everything this client published so far.
        _this->linkIndexBase = _this->chain.samplesIn() - _this->client->samplesPublished();
        {
            std::lock_guard lck(_this->iqTuneMtx);
            _this->intendedW = 0.0;
            _this->sentIqValid = false;
        }
        _this->client->setSetting(SPYSERVER_SETTING_IQ_FORMAT, streamFormats[_this->iqType]);
        _this->client->setSetting(SPYSERVER_SETTING_IQ_DECIMATION, stage);
        _this->sendIQTune(false);
        if (_this->vfoMode()) {
            // FFT on the same centre: in FFT_IQ mode the device follows the
            // FFT centre, and the IQ must stay inside the device's band.
            _this->client->setFftScale(_this->fftDbOffset, _this->fftDbRange);
            _this->client->setSetting(SPYSERVER_SETTING_FFT_FORMAT, SPYSERVER_STREAM_FORMAT_UINT8);
            _this->client->setSetting(SPYSERVER_SETTING_FFT_DB_OFFSET, _this->fftDbOffset);
            _this->client->setSetting(SPYSERVER_SETTING_FFT_DB_RANGE, _this->fftDbRange);
            _this->client->setSetting(SPYSERVER_SETTING_FFT_DISPLAY_PIXELS, FFT_DISPLAY_PIXELS);
            _this->client->setSetting(SPYSERVER_SETTING_FFT_FREQUENCY, _this->freq);
            _this->client->setSetting(SPYSERVER_SETTING_FFT_DECIMATION, _this->fftZoomId);
        }
        // Gains after the streaming mode (jp's order).
        _this->client->setSetting(SPYSERVER_SETTING_STREAMING_MODE, _this->vfoMode() ? SPYSERVER_STREAM_MODE_FFT_IQ : SPYSERVER_STREAM_MODE_IQ_ONLY);
        _this->client->setSetting(SPYSERVER_SETTING_GAIN, _this->gain);
        _this->applyDigitalGain(true);
        _this->client->startStream();

        _this->running = true;
        flog::info("SpyServerSourceModule '{0}': Start!", _this->name);
    }

    static void stop(void* ctx) {
        SpyServerSourceModule* _this = (SpyServerSourceModule*)ctx;
        if (!_this->running) { return; }

        _this->client->stopStream();
        _this->chain.stop();

        _this->running = false;
        flog::info("SpyServerSourceModule '{0}': Stop!", _this->name);
    }

    static void tune(double freq, void* ctx) {
        SpyServerSourceModule* _this = (SpyServerSourceModule*)ctx;
        _this->freq = freq;
        if (_this->running) {
            // C + W; in VFO+FFT mode the FFT centre (and with it the device)
            // follows from the UI tick, rate-limited.
            _this->sendIQTune(false);
            if (_this->servoActive()) { _this->applyDigitalGain(true); }
        }
        flog::info("SpyServerSourceModule '{0}': Tune: {1}!", _this->name, freq);
    }

    static void menuHandler(void* ctx) {
        SpyServerSourceModule* _this = (SpyServerSourceModule*)ctx;

        _this->pollConnection();

        bool connected = (_this->client && _this->client->isOpen());
        gui::mainWindow.playButtonLocked = !connected;

        const bool connecting = _this->connector.connecting();
        if (connected || connecting) { SmGui::BeginDisabled(); }
        if (SmGui::InputText(CONCAT("##_spyserver_srv_host_", _this->name), _this->hostname, 1023)) {
            config.edit().set("hostname", _this->hostname);
        }
        SmGui::SameLine();
        SmGui::FillWidth();
        if (SmGui::InputInt(CONCAT("##_spyserver_srv_port_", _this->name), &_this->port, 0, 0)) {
            config.edit().set("port", _this->port);
        }
        // CONNECT through the Network proxy; many proxies allow it only to 443.
        if (SmGui::Checkbox(CONCAT("Through the proxy##_spyserver_proxy_", _this->name), &_this->viaProxy)) {
            config.edit().set("viaProxy", _this->viaProxy);
        }
        if (connected || connecting) { SmGui::EndDisabled(); }

        if (_this->running) { SmGui::BeginDisabled(); }
        SmGui::FillWidth();
        SmGui::ForceSync();
        // Keep this value for the whole ImGui scope. tryConnect() changes the
        // atomic immediately; testing it again below would call EndDisabled()
        // without the matching BeginDisabled() in the frame where Connect was
        // clicked, corrupting ImGui's style stack.
        if (connecting) { SmGui::BeginDisabled(); }
        if (!connected && SmGui::Button("Connect##spyserver_source")) {
            _this->tryConnect();
        }
        else if (connected && SmGui::Button("Disconnect##spyserver_source")) {
            _this->client->close();
        }
        if (connecting) { SmGui::EndDisabled(); }
        if (_this->running) { SmGui::EndDisabled(); }
        // The browser is local UI; a headless server never draws it.
        if (!core::args["server"].b() && ImGui::Button("Browse...##_spyserver_browse")) {
            remote::openBrowser(remote::RemoteKind::SpyServer);
        }


        if (connected) {
            if (_this->running) { SmGui::BeginDisabled(); }
            // A headless server streams full IQ only: the server waterfall
            // needs the local GUI.
            if (!core::args["server"].b()) {
                SmGui::LeftLabel("Mode");
                SmGui::FillWidth();
                if (SmGui::Combo("##spyserver_source_mode", &_this->modeId, modeStr)) {
                    _this->applySampleRate();
                    config.edit().section("devices", _this->devRef).set("mode", std::string(modeKeys[_this->modeId]));
                }
            }
            if (_this->vfoMode()) {
                SmGui::LeftLabel("IQ bandwidth");
                SmGui::FillWidth();
                if (SmGui::Combo("##spyserver_source_vfo_sr", &_this->vfoIqRateId, _this->sampleRatesTxt.c_str())) {
                    _this->applySampleRate();
                    config.edit().section("devices", _this->devRef).set("vfoIqRateId", _this->vfoIqRateId);
                }
            }
            else {
                SmGui::LeftLabel("Samplerate");
                SmGui::FillWidth();
                if (SmGui::Combo("##spyserver_source_sr", &_this->srId, _this->sampleRatesTxt.c_str())) {
                    _this->applySampleRate();
                    config.edit().section("devices", _this->devRef).set("sampleRateId", _this->srId);
                }
            }
            if (_this->running) { SmGui::EndDisabled(); }

            SmGui::LeftLabel("Sample bit depth");
            SmGui::FillWidth();
            if (SmGui::Combo("##spyserver_source_type", &_this->iqType, streamFormatStr)) {
                _this->client->setSetting(SPYSERVER_SETTING_IQ_FORMAT, streamFormats[_this->iqType]);
                _this->applyDigitalGain(false);

                config.edit().section("devices", _this->devRef).set("sampleBitDepthId", _this->iqType);
            }

            if (_this->client->devInfo.MaximumGainIndex) {
                SmGui::FillWidth();
                if (SmGui::SliderInt("##spyserver_source_gain", (int*)&_this->gain, 0, _this->client->devInfo.MaximumGainIndex)) {
                    _this->client->setSetting(SPYSERVER_SETTING_GAIN, _this->gain);
                    _this->applyDigitalGain(false);
                    config.edit().section("devices", _this->devRef).set("gainId", _this->gain);
                }
            }

            // Digital gain: the formula value, or jp's servo on the IQ peak
            // (UInt8 and Int16 only; not on a headless server, which has no
            // UI tick to run it).
            if (!core::args["server"].b()) {
                bool& autoGain = _this->vfoMode() ? _this->digitalGainAuto : _this->digitalGainAutoIq;
                if (SmGui::Checkbox("Auto digital gain##spyserver_source_dgain_auto", &autoGain)) {
                    config.edit().section("devices", _this->devRef).set(_this->vfoMode() ? "digitalGainAuto" : "digitalGainAutoIq", autoGain);
                    _this->applyDigitalGain(true);
                }
                SmGui::SameLine();
                SmGui::TextF("%d dB", _this->sentDigitalGain);
            }

            if (_this->vfoMode()) {
                // The server's FFT scale: byte = 255 * (dB + offset + range) / range.
                // Applied live, though the server takes 1-3 s to follow.
                SmGui::LeftLabel("FFT dB offset");
                SmGui::FillWidth();
                if (SmGui::SliderInt("##spyserver_source_fft_offset", &_this->fftDbOffset, 0, SPYSERVER_MAX_FFT_DB_OFFSET)) {
                    _this->sendFftScale();
                    config.edit().section("devices", _this->devRef).set("fftDbOffset", _this->fftDbOffset);
                }
                SmGui::LeftLabel("FFT dB range");
                SmGui::FillWidth();
                if (SmGui::SliderInt("##spyserver_source_fft_range", &_this->fftDbRange, SPYSERVER_MIN_FFT_DB_RANGE, SPYSERVER_MAX_FFT_DB_RANGE)) {
                    _this->sendFftScale();
                    config.edit().section("devices", _this->devRef).set("fftDbRange", _this->fftDbRange);
                }
                SmGui::LeftLabel("Spectrum calibration");
                SmGui::FillWidth();
                if (SmGui::SliderInt("##spyserver_source_fft_cal", &_this->fftCalDb, -60, 60, SmGui::FMT_STR_INT_DB)) {
                    config.edit().section("devices", _this->devRef).set("fftCalDb", _this->fftCalDb);
                }
            }

            SmGui::Text("Status:");
            SmGui::SameLine();
            SmGui::TextColoredF(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "Connected (%s)", deviceTypesStr[_this->client->devInfo.DeviceType]);
            if (!_this->client->canControl()) {
                SmGui::TextColoredF(ImVec4(1.0f, 0.75f, 0.2f, 1.0f), "Shared: device centre fixed by another client");
            }
        }
        else {
            SmGui::Text("Status:");
            SmGui::SameLine();
            SmGui::Text(connecting ? "Connecting..." : "Not connected");
            if (!connecting && !_this->connector.lastError().empty()) {
                SmGui::TextColoredF(ImVec4(1.0f, 0.35f, 0.35f, 1.0f), "Connection failed: %s", _this->connector.lastError().c_str());
            }
        }
    }

    void tryConnect() {
        if (connector.connecting()) { return; }

        if (port <= 0 || port > 65535) {
            connector.setError("Port must be between 1 and 65535");
            return;
        }

        if (client) { client.reset(); }

        const std::string host = hostname;
        const uint16_t targetPort = port;
        const bool throughProxy = viaProxy;
        connector.begin([this, host, targetPort, throughProxy]() {
            auto newClient = spyserver::connect(host, targetPort, &rawStream, 3000, throughProxy);
            if (!newClient) {
                throw std::runtime_error("Could not connect to SpyServer");
            }
            if (!newClient->waitForDevInfo(3000)) {
                newClient->close();
                throw std::runtime_error("SpyServer didn't respond with device information");
            }
            return newClient;
        });
    }

    void pollConnection() {
        spyserver::SpyServerClient newClient;
        auto res = connector.poll(newClient);
        if (res == SpyConnector::Result::FAILED) {
            flog::error("Could not connect to spyserver {}", connector.lastError());
            return;
        }
        if (res != SpyConnector::Result::CONNECTED) { return; }

        try {
            constexpr uint32_t DEVICE_TYPE_COUNT = sizeof(deviceTypesStr) / sizeof(deviceTypesStr[0]);
            if (newClient->devInfo.DeviceType >= DEVICE_TYPE_COUNT) {
                throw std::runtime_error("SpyServer reported an unsupported device type");
            }
            if (newClient->devInfo.MinimumIQDecimation > newClient->devInfo.DecimationStageCount ||
                newClient->devInfo.DecimationStageCount >= 31 ||
                newClient->devInfo.MaximumSampleRate == 0) {
                throw std::runtime_error("SpyServer reported invalid sample-rate metadata");
            }

            client = std::move(newClient);
            client->onFft = [this](const spyserver::SpyServerClientClass::FftFrame& f) { onFftFrame(f); };
            char buf[1024];
            snprintf(buf, sizeof(buf), "%s [%08X]", deviceTypesStr[client->devInfo.DeviceType], client->devInfo.DeviceSerial);
            devRef = std::string(buf);
            if (!core::args["server"].b()) {
                remote::recordRecent(remote::manualEntry(remote::RemoteKind::SpyServer,
                                                         remote::RemoteTarget{ "tcp", hostname, port, "" },
                                                         deviceTypesStr[client->devInfo.DeviceType]));
            }

            {
                // The edit access unlocks on the way out of the scope, including
                // through an exception, so no hand-written unwind is needed.
                auto configAccess = config.edit();
                ConfigManager::EditSection dev = configAccess.section("devices", devRef);
                dev.ensure("sampleRateId", 0);
                dev.ensure("sampleBitDepthId", 1);
                dev.ensure("gainId", 0);
                dev.ensure("mode", std::string("iq"));
                dev.ensure("vfoIqRateId", defaultVfoRateId(client->devInfo));
                dev.ensure("fftZoomId", 0);
                dev.ensure("fftDbOffset", 0);
                dev.ensure("fftDbRange", 150);
                dev.ensure("fftCalDb", 0);
                dev.ensure("digitalGainAuto", true);
                dev.ensure("digitalGainAutoIq", false);
                dev.tryGet("digitalGainAuto", digitalGainAuto);
                dev.tryGet("digitalGainAutoIq", digitalGainAutoIq);
                dev.tryGet("sampleRateId", srId);
                dev.tryGet("sampleBitDepthId", iqType);
                dev.tryGet("gainId", gain);
                std::string mode = "iq";
                dev.tryGet("mode", mode);
                modeId = (mode == "vfo_fft" && !core::args["server"].b()) ? 1 : 0;
                dev.tryGet("vfoIqRateId", vfoIqRateId);
                dev.tryGet("fftZoomId", fftZoomId);
                dev.tryGet("fftDbOffset", fftDbOffset);
                dev.tryGet("fftDbRange", fftDbRange);
                dev.tryGet("fftCalDb", fftCalDb);
            }

            gain = std::clamp<int>(gain, 0, client->devInfo.MaximumGainIndex);

            // Refresh sample rates on the GUI thread, where the source state is used.
            sampleRates.clear();
            sampleRatesTxt.clear();
            for (int i = client->devInfo.MinimumIQDecimation; i <= client->devInfo.DecimationStageCount; i++) {
                double sr = (double)client->devInfo.MaximumSampleRate / ((double)(1U << i));
                sampleRates.push_back(sr);
                sampleRatesTxt += getBandwdithScaled(sr);
                sampleRatesTxt += '\0';
            }

            srId = std::clamp<int>(srId, 0, (int)sampleRates.size() - 1);
            vfoIqRateId = std::clamp<int>(vfoIqRateId, 0, (int)sampleRates.size() - 1);
            fftZoomId = std::clamp<int>(fftZoomId, 0, (int)client->devInfo.DecimationStageCount);
            fftDbOffset = std::clamp<int>(fftDbOffset, 0, SPYSERVER_MAX_FFT_DB_OFFSET);
            fftDbRange = std::clamp<int>(fftDbRange, SPYSERVER_MIN_FFT_DB_RANGE, SPYSERVER_MAX_FFT_DB_RANGE);
            iqType = std::clamp<int>(iqType, 0, (int)(sizeof(streamFormats) / sizeof(streamFormats[0])) - 1);

            applySampleRate();
            flog::info("Connected to server");
        }
        catch (const std::exception& e) {
            flog::error("Could not initialize SpyServer connection: {}", e.what());
            connector.setError(e.what());
            if (client) {
                client->close();
                client.reset();
            }
        }
    }

    bool vfoMode() const { return modeId == 1; }

    // Server waterfall ------------------------------------------------------

    // What core sees of the server's spectrum; all calls on the UI thread.
    struct Provider : public SpectrumProvider {
        SpyServerSourceModule* module = nullptr;
        std::string name() const override { return "SpyServer"; }
        int binCount() const override { return FFT_DISPLAY_PIXELS; }
        std::vector<double> zoomSpans() const override { return module->fftSpans; }
        void requestSpan(double spanHz) override { module->requestSpan(spanHz); }
        bool available() const override { return module->serverSpectrumAlive(); }
        std::string unavailableReason() const override {
            return module->client && module->client->isOpen() ? "no FFT frames from the server" : "connection closed";
        }
        // The server's dB at the default offset 0, range 150.
        std::pair<float, float> defaultLevels() const override { return { -110.0f, -40.0f }; }
    };

    using Clock = std::chrono::steady_clock;
    static constexpr auto kFrameTimeout = std::chrono::seconds(3);
    static constexpr auto kCommitTimeout = std::chrono::seconds(3);
    static constexpr auto kViewInterval = std::chrono::milliseconds(125);

    struct FftGeometry {
        double centre = 0.0;  // hardware Hz
        int stage = 0;
    };

    double fftSpan(int stage) const {
        if (!client) { return 0.0; }
        return (double)client->devInfo.MaximumBandwidth / (double)(1U << stage);
    }

    // Narrow IQ usable width: the IQ rate times the device's alias-free
    // fraction (MaximumBandwidth / MaximumSampleRate).
    double iqWindowWidth() const {
        if (!client || client->devInfo.MaximumSampleRate == 0) { return sampleRate; }
        return sampleRate * (double)client->devInfo.MaximumBandwidth / (double)client->devInfo.MaximumSampleRate;
    }

    // UI thread: the narrowest stage whose span still covers spanHz.
    void requestSpan(double spanHz) {
        int stage = 0;
        for (int i = 0; i < (int)fftSpans.size(); i++) {
            if (fftSpans[(size_t)i] >= spanHz * (1.0 - 1e-9)) { stage = i; }
        }
        wantStage = stage;
    }

    bool serverSpectrumAlive() const {
        if (!running || !client || !client->isOpen()) { return false; }
        const auto now = Clock::now();
        const auto last = Clock::time_point(Clock::duration(lastFftTicks.load()));
        // A grace period after registering: the first frames take a moment.
        return now - last < kFrameTimeout || now - registeredAt < kFrameTimeout + std::chrono::seconds(2);
    }

    static void uiTick(void* ctx) {
        SpyServerSourceModule* _this = (SpyServerSourceModule*)ctx;
        _this->serverWaterfallTick();
        if (_this->providerRegistered) { _this->placeIQWindow(); }
        if (_this->running && _this->client && _this->client->isOpen()) {
            _this->sendIQTune(true);
            _this->digitalGainServo();
        }
    }

    // IQ window ---------------------------------------------------------------
    //
    // In VFO+FFT mode the narrow IQ is a window at C + W inside the server's
    // spectrum, placed over the VFOs (iq_window_policy.h). W is display Hz.

    double hardwareOffset(double w) const { return uiInvertIQ.load() ? -w : w; }

    // UI thread, while the provider is registered.
    void placeIQWindow() {
        IQWindowInput in;
        const std::string& selected = gui::waterfall.selectedVFO;
        auto sel = gui::waterfall.vfos.find(selected);
        if (sel != gui::waterfall.vfos.end()) { in.passbands.emplace_back(sel->second->lowerOffset, sel->second->upperOffset); }
        for (auto const& [name, vfo] : gui::waterfall.vfos) {
            if (name != selected) { in.passbands.emplace_back(vfo->lowerOffset, vfo->upperOffset); }
        }
        in.currentOffset = intendedW.load();
        in.width = iqWindowWidth();
        in.margin = 0.1 * in.width;
        // Hardware limits: the window inside the device's alias-free band
        // around its centre (the FFT centre in this mode), intersected with
        // the server's IQ limits.
        SpyServerSyncCopy sy = latestSync();
        double devCentre;
        {
            std::lock_guard lck(geometryMtx);
            devCentre = committed.centre;
        }
        if (sy.valid && sy.sync.DeviceCenterFrequency) { devCentre = sy.sync.DeviceCenterFrequency; }
        const double half = std::max(0.0, ((double)client->devInfo.MaximumBandwidth - in.width) / 2.0);
        double lo = devCentre - half - freq;
        double hi = devCentre + half - freq;
        if (sy.valid && sy.sync.MaximumIQCenterFrequency > sy.sync.MinimumIQCenterFrequency) {
            lo = std::max(lo, (double)sy.sync.MinimumIQCenterFrequency - freq);
            hi = std::min(hi, (double)sy.sync.MaximumIQCenterFrequency - freq);
        }
        if (hi < lo) { hi = lo = 0.0; }
        in.minOffset = uiInvertIQ.load() ? -hi : lo;
        in.maxOffset = uiInvertIQ.load() ? -lo : hi;
        const double w = chooseIQWindow(in);
        if (std::fabs(w - intendedW.load()) >= 1.0) {
            intendedW = w;
            sigpath::vfoManager.setIQWindow(w, in.width);
        }
    }

    struct SpyServerSyncCopy {
        bool valid = false;
        SpyServerClientSync sync{};
    };
    SpyServerSyncCopy latestSync() const {
        SpyServerSyncCopy c;
        if (client) { c.valid = client->clientSync(c.sync); }
        return c;
    }

    // Send IQ_FREQUENCY = C + W (the server's IQ limits applied: it ignores
    // requests outside them). From tune() at once, from the UI tick when the
    // window moved (at most 8 per second). A send that moves the window
    // PINGs, and the PONG commits it (commitAt).
    void sendIQTune(bool rateLimited) {
        if (!client) { return; }
        std::lock_guard lck(iqTuneMtx);
        const double w = providerRegistered ? intendedW.load() : 0.0;
        const double hw = hardwareOffset(w);
        double target = freq + hw;
        SpyServerSyncCopy sy = latestSync();
        if (sy.valid && sy.sync.MaximumIQCenterFrequency > sy.sync.MinimumIQCenterFrequency) {
            target = std::clamp(target, (double)sy.sync.MinimumIQCenterFrequency, (double)sy.sync.MaximumIQCenterFrequency);
        }
        const auto now = Clock::now();
        if (sentIqValid && std::fabs(target - sentIqFreq) < 0.5) { return; }
        if (rateLimited && now - lastIqSend < kViewInterval) { return; }
        const bool movesWindow = sentIqValid && std::fabs(hw - sentHwW) >= 0.5;
        client->setSetting(SPYSERVER_SETTING_IQ_FREQUENCY, (uint32_t)std::llround(target));
        if (movesWindow) {
            client->ping([this, w](bool answered, uint64_t iqIndex) { commitAt(answered, iqIndex, w); });
        }
        sentIqFreq = target;
        sentHwW = hw;
        sentIqValid = true;
        lastIqSend = now;
    }

    // Network thread: the PONG of a window move. The IQ switches at its
    // stream position (Phase 0: within about 20 samples), so commit there
    // with a short mute. Without a PONG (rule C) commit now, with 40 ms.
    void commitAt(bool answered, uint64_t iqIndex, double w) {
        if (!running) { return; }
        const double sr = sampleRate;
        const uint64_t index = linkIndexBase + iqIndex;
        const uint64_t half = (uint64_t)std::llround((answered ? 0.003 : 0.020) * sr);
        // A marker fires before the whole output block holding its sample.
        const int block = std::max(client ? client->lastMessageSamples() : 0, (int)std::lround(0.010 * sr));
        const int muteSamples = (int)(2 * half) + block;
        chain.scheduleMarker(index > half ? index - half : 0, [muteSamples] { sigpath::vfoManager.muteAllFor(muteSamples); });
        chain.scheduleMarker(index, [w] { sigpath::vfoManager.commitIQWindowOffset(w); });
    }

    // Digital gain ------------------------------------------------------------
    //
    // jp's servo (spyserver_vfo_source, SDRPlusPlus-jp): fast attack, slow
    // release towards a -6 dBFS raw IQ peak, a stepped cut on hard clipping,
    // never above the formula ceiling.
    static constexpr float kServoTargetDb = -6.0f;
    static constexpr float kServoAlphaCut = 0.80f;
    static constexpr float kServoAlphaRaise = 0.30f;
    static constexpr double kServoRailFraction = 0.001;
    static constexpr auto kServoInterval = std::chrono::milliseconds(150);
    // jp's empirical baseline for an Airspy HF+ on UInt8, added only while
    // the servo runs (the servo then trims it down).
    static constexpr int kHfUInt8BaselineDb = 32;

    bool servoActive() const {
        if (core::args["server"].b()) { return false; }
        if (streamFormats[iqType] == SPYSERVER_STREAM_FORMAT_FLOAT) { return false; }
        return vfoMode() ? digitalGainAuto : digitalGainAutoIq;
    }

    int digitalGainCeiling() const {
        int g = client->computeDigitalGain(streamFormatsBitCount[iqType], gain, iqStage());
        if (servoActive() && client->devInfo.DeviceType == SPYSERVER_DEVICE_AIRSPY_HF &&
            streamFormats[iqType] == SPYSERVER_STREAM_FORMAT_UINT8) {
            g += kHfUInt8BaselineDb;
        }
        return std::max(0, g);
    }

    // The one place that sends IQ_DIGITAL_GAIN. reset (start, retune, Auto
    // switched on) snaps the servo to the ceiling; otherwise only the
    // ceiling moves. With the servo off it sends the formula value.
    void applyDigitalGain(bool reset) {
        if (!client) { return; }
        const int ceiling = digitalGainCeiling();
        if (servoActive()) {
            if (reset) { servoGain = (float)ceiling; }
            servoGain = std::clamp(servoGain, 0.0f, (float)ceiling);
            if (reset) { client->readIqMeter(); }
        }
        else {
            servoGain = (float)ceiling;
        }
        sendDigitalGain((int)std::lround(servoGain), reset);
    }

    void sendDigitalGain(int value, bool force) {
        if (!force && value == sentDigitalGain) { return; }
        sentDigitalGain = value;
        client->setSetting(SPYSERVER_SETTING_IQ_DIGITAL_GAIN, (uint32_t)value);
    }

    // UI tick: one adjustment per kServoInterval from the meter since the last.
    void digitalGainServo() {
        if (!servoActive()) { return; }
        const auto now = Clock::now();
        if (now - lastServoStep < kServoInterval) { return; }
        lastServoStep = now;
        const auto m = client->readIqMeter();
        if (m.values == 0) { return; }
        const int ceiling = digitalGainCeiling();
        const double railed = (double)m.railed / (double)m.values;
        if (railed > kServoRailFraction) {
            // 3 dB, up to 24 dB when a fifth of the samples sit on a rail.
            servoGain -= (float)(3.0 + 21.0 * std::min(1.0, railed / 0.2));
        }
        else if (m.peak > 0.0f) {
            const float err = kServoTargetDb - 20.0f * std::log10(m.peak);
            servoGain += (err < 0.0f ? kServoAlphaCut : kServoAlphaRaise) * err;
        }
        servoGain = std::clamp(servoGain, 0.0f, (float)ceiling);
        sendDigitalGain((int)std::lround(servoGain), false);
    }

    void serverWaterfallTick() {
        uiTuningOffset = sigpath::sourceManager.getTuningOffset();
        uiCentre = gui::waterfall.getCenterFrequency();
        uiInvertIQ = sigpath::iqFrontEnd.getInvertIQ();
        uiCalDb = (float)fftCalDb;

        // Core takes the provider away when it reports unavailable.
        if (providerRegistered && sigpath::iqFrontEnd.spectrumProvider() != &provider) {
            providerRegistered = false;
            frameSink = false;
        }
        if (!(vfoMode() && running && client && client->isOpen())) {
            unregisterProvider();
            return;
        }
        const auto now = Clock::now();
        if (!providerRegistered) {
            if (sigpath::iqFrontEnd.spectrumProvider() != nullptr) { return; }
            fftSpans.clear();
            for (int i = 0; i <= (int)client->devInfo.DecimationStageCount; i++) { fftSpans.push_back(fftSpan(i)); }
            // start() sent the view: the device's centre at fftZoomId.
            {
                std::lock_guard lck(geometryMtx);
                committed = FftGeometry{ freq, fftZoomId };
            }
            sent = committed;
            wantStage = fftZoomId;
            pendingActive = false;
            appliedStage = -1;
            registeredAt = now;
            lastFftTicks = 0;
            sigpath::iqFrontEnd.setSpectrumProvider(&provider);
            providerRegistered = true;
            frameSink = true;
            intendedW = 0.0;
            sigpath::vfoManager.setIQWindow(0.0, iqWindowWidth());
        }

        // A send is confirmed by the CLIENT_SYNC that follows FFT changes, or
        // after kCommitTimeout.
        if (pendingActive && (client->syncCount() > pendingSyncCount || now - pendingSentAt > kCommitTimeout)) {
            pendingActive = false;
            std::lock_guard lck(geometryMtx);
            committed = pending;
        }
        int stage;
        {
            std::lock_guard lck(geometryMtx);
            stage = committed.stage;
        }
        if (stage != appliedStage && stage < (int)fftSpans.size()) {
            appliedStage = stage;
            core::setDisplaySpan(fftSpans[(size_t)stage]);
        }

        // View: FFT stage and centre, at most 8 sends per second; a held-back
        // change goes out once the interval has passed.
        // Requests outside the server's FFT limits are ignored without a
        // reply (Phase 0), so clamp to the last CLIENT_SYNC's.
        FftGeometry want{ freq, wantStage };
        SpyServerClientSync sync{};
        if (client->clientSync(sync) && sync.MaximumFFTCenterFrequency > sync.MinimumFFTCenterFrequency) {
            want.centre = std::clamp(want.centre, (double)sync.MinimumFFTCenterFrequency, (double)sync.MaximumFFTCenterFrequency);
        }
        const bool changed = want.stage != sent.stage || std::fabs(want.centre - sent.centre) >= 0.5;
        if (changed && now - lastViewSend >= kViewInterval) {
            if (want.stage != sent.stage) { client->setSetting(SPYSERVER_SETTING_FFT_DECIMATION, want.stage); }
            if (std::fabs(want.centre - sent.centre) >= 0.5) { client->setSetting(SPYSERVER_SETTING_FFT_FREQUENCY, (uint32_t)std::llround(want.centre)); }
            pending = want;
            pendingActive = true;
            pendingSyncCount = client->syncCount();
            pendingSentAt = now;
            sent = want;
            lastViewSend = now;
            if (want.stage != fftZoomId) {
                fftZoomId = want.stage;
                config.edit().section("devices", devRef).set("fftZoomId", fftZoomId);
            }
        }
    }

    // UI thread.
    void unregisterProvider() {
        if (!providerRegistered) { return; }
        providerRegistered = false;
        frameSink = false;
        if (sigpath::iqFrontEnd.spectrumProvider() == &provider) {
            sigpath::iqFrontEnd.setSpectrumProvider(nullptr);
        }
        core::clearDisplaySpan();
        // W = 0 at once; the retune to C follows from the UI tick.
        sigpath::vfoManager.clearIQWindow();
        intendedW = 0.0;
        appliedStage = -1;
    }

    // Network thread: one server FFT line to core, in the display domain,
    // with the geometry committed when it arrived.
    void onFftFrame(const spyserver::SpyServerClientClass::FftFrame& f) {
        lastFftTicks = f.arrival.time_since_epoch().count();
        if (!frameSink.load() || f.count <= 0) { return; }
        FftGeometry g;
        {
            std::lock_guard lck(geometryMtx);
            g = committed;
        }
        const double span = fftSpan(g.stage);
        const float cal = uiCalDb.load();
        fftFrame.bins.resize((size_t)f.count);
        for (int i = 0; i < f.count; i++) { fftFrame.bins[(size_t)i] = f.bins[i] + cal; }
        // Bin n/2 is centred on the FFT centre: bin 0's lower edge is half a
        // bin below centre - span/2.
        double start = g.centre - span / 2.0 - span / (2.0 * f.count) + uiTuningOffset.load();
        if (uiInvertIQ.load()) {
            std::reverse(fftFrame.bins.begin(), fftFrame.bins.end());
            start = 2.0 * uiCentre.load() - (start + span);
        }
        fftFrame.startHz = start;
        fftFrame.spanHz = span;
        // No sync key: show it after the IQ now buffered ahead of the DSP.
        const double delay = sampleRate > 0.0 ? (double)chain.bufferedSamples() / sampleRate : 0.0;
        fftFrame.presentAt = f.arrival + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(delay));
        sigpath::iqFrontEnd.pushSpectrum(std::move(fftFrame));
    }

    // IQ decimation stage of the current mode.
    int iqStage() const {
        const int id = vfoMode() ? vfoIqRateId : srId;
        return id + (client ? (int)client->devInfo.MinimumIQDecimation : 0);
    }

    // The rate the DSP runs at: the mode's IQ rate. Stopped only (the combos
    // are disabled while running).
    void applySampleRate() {
        if (sampleRates.empty()) {
            // Not connected yet: the default rate, as before a device is known.
            core::setInputSampleRate(sampleRate);
            return;
        }
        const int id = std::clamp<int>(vfoMode() ? vfoIqRateId : srId, 0, (int)sampleRates.size() - 1);
        sampleRate = sampleRates[id];
        chain.setSampleRate(sampleRate);
        core::setInputSampleRate(sampleRate);
    }

    // First narrow-IQ choice for a device: the widest stage at or below
    // 192 kS/s, enough for any analogue mode while far below the device rate.
    static int defaultVfoRateId(const SpyServerDeviceInfo& dev) {
        int id = 0;
        for (int i = dev.MinimumIQDecimation; i <= (int)dev.DecimationStageCount; i++) {
            id = i - (int)dev.MinimumIQDecimation;
            if ((double)dev.MaximumSampleRate / (double)(1U << i) <= 192000.0) { break; }
        }
        return id;
    }

    void sendFftScale() {
        if (!client) { return; }
        client->setFftScale(fftDbOffset, fftDbRange);
        if (running) {
            client->setSetting(SPYSERVER_SETTING_FFT_DB_OFFSET, fftDbOffset);
            client->setSetting(SPYSERVER_SETTING_FFT_DB_RANGE, fftDbRange);
        }
    }

    std::string name;
    bool enabled = true;
    bool running = false;
    double sampleRate = 1000000;
    double freq;

    char hostname[1024];
    int port = 5555;
    bool viaProxy = false;
    int iqType = 0;

    int srId = 0;
    std::vector<double> sampleRates;
    std::string sampleRatesTxt;

    // Per-device settings of the VFO + server waterfall mode.
    int modeId = 0;              // index into modeKeys
    int vfoIqRateId = 0;         // narrow IQ stage, from MinimumIQDecimation
    int fftZoomId = 0;           // FFT stage
    int fftDbOffset = 0;
    int fftDbRange = 150;
    int fftCalDb = 0;            // our calibration on top of the server's scale

    // Server waterfall, UI thread.
    Provider provider;
    bool providerRegistered = false;
    std::vector<double> fftSpans;       // span per FFT stage, widest first
    int wantStage = 0;
    FftGeometry sent;                   // last view sent
    FftGeometry pending;                // sent, not yet confirmed
    bool pendingActive = false;
    uint64_t pendingSyncCount = 0;
    Clock::time_point pendingSentAt{};
    Clock::time_point lastViewSend{};
    Clock::time_point registeredAt{};
    int appliedStage = -1;              // stage whose span core shows
    // Written on the UI thread, read on the network thread.
    std::atomic<bool> frameSink{false};
    std::atomic<double> uiTuningOffset{0.0};
    std::atomic<double> uiCentre{0.0};
    std::atomic<bool> uiInvertIQ{false};
    std::atomic<float> uiCalDb{0.0f};
    std::mutex geometryMtx;
    FftGeometry committed;              // geometryMtx
    // Network thread.
    SpectrumFrame fftFrame;
    std::atomic<int64_t> lastFftTicks{0};

    // IQ window. intendedW: UI thread writes, the tune path reads.
    std::atomic<double> intendedW{0.0};
    // Last IQ_FREQUENCY sent; tune() may come from any thread.
    std::mutex iqTuneMtx;
    bool sentIqValid = false;
    double sentIqFreq = 0.0;
    double sentHwW = 0.0;
    Clock::time_point lastIqSend{};
    // The link's input index of the client's sample 0.
    std::atomic<uint64_t> linkIndexBase{0};

    // Digital gain, UI thread.
    bool digitalGainAuto = true;     // VFO+FFT mode
    bool digitalGainAutoIq = false;  // Full IQ mode
    float servoGain = 0.0f;
    int sentDigitalGain = 0;
    Clock::time_point lastServoStep{};

    uint32_t gain = 0;

    std::string devRef = "";

    dsp::stream<dsp::complex_t> rawStream;
    dsp::stream<dsp::complex_t> stream;
    dsp::buffer::PrebufferedLink<dsp::complex_t> chain;
    SourceManager::SourceHandler handler;

    spyserver::SpyServerClient client;
    // Declared after every member its factory uses (stream, client), so its
    // destructor joins the connect worker before those are destroyed.
    using SpyConnector = AsyncConnector<spyserver::SpyServerClient>;
    SpyConnector connector;
};

MOD_EXPORT void _INIT_() {
    json def = json({});
    def["hostname"] = "localhost";
    def["port"] = 5555;
    def["devices"] = json::object();
    config.setPath(core::args["root"].s() + "/spyserver_config.json");
    config.load(def);
    config.enableAutoSave();

    // Check config in case a user has a very old version
    {
        auto configAccess = config.edit();
        if (!configAccess.contains("hostname") || !configAccess.contains("port") || !configAccess.contains("devices")) {
            configAccess.reset(def);
        }
    }
}

MOD_EXPORT ModuleManager::Instance* _CREATE_INSTANCE_(std::string name) {
    return new SpyServerSourceModule(name);
}

MOD_EXPORT void _DELETE_INSTANCE_(ModuleManager::Instance* instance) {
    delete (SpyServerSourceModule*)instance;
}

MOD_EXPORT void _END_() {
    config.shutdown();
}
