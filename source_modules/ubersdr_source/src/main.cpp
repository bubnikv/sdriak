// UberSDR source: wide IQ from an UberSDR receiver (madpsy/ka9q_ubersdr)
// over its native API. Protocol facts: doc/research/ubersdr.md. The plan
// this follows: doc/todo/network-sources/ubersdr/.
#include <imgui.h>
#include <module.h>
#include <config.h>
#include <core.h>
#include <gui/gui.h>
#include <gui/smgui.h>
#include <gui/style.h>
#include <signal_path/signal_path.h>
#include <dsp/buffer/prebuffer.h>
#include <signal_path/spectrum_provider.h>
#include <signal_path/iq_window_policy.h>
#include <utils/flog.h>
#include <gui/remote/browser.h>
#include <gui/remote/recent.h>

#include "ubersdr_client.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

SDRPP_MOD_INFO{
    /* Name:            */ "ubersdr_source",
    /* Description:     */ "UberSDR source module for SDRIAK",
    /* Author:          */ "SDRIAK",
    /* Version:         */ 0, 1, 0,
    /* Max instances    */ 1
};

ConfigManager config;

namespace {
    // The primary IQ modes, narrowest first, with their nominal rates.
    struct IqMode {
        const char* key;
        const char* label;
        int rate;
    };
    constexpr IqMode kIqModes[] = {
        { "iq48", "48 kHz", 48000 },
        { "iq96", "96 kHz", 96000 },
        { "iq192", "192 kHz", 192000 },
        { "iq384", "384 kHz", 384000 },
    };
    constexpr int kIqModeCount = (int)(sizeof(kIqModes) / sizeof(kIqModes[0]));

    int modeIndex(const std::string& key) {
        for (int i = 0; i < kIqModeCount; i++) {
            if (key == kIqModes[i].key) { return i; }
        }
        return 0;
    }

    // "host:port", "host" or a URL, to scheme://host[:port] with no path.
    // Without a scheme: https, which 74 of 77 instances use.
    std::string normaliseBaseUrl(std::string s) {
        while (!s.empty() && (s.back() == '/' || s.back() == ' ')) { s.pop_back(); }
        while (!s.empty() && s.front() == ' ') { s.erase(s.begin()); }
        if (s.find("://") == std::string::npos) { s = "https://" + s; }
        const auto scheme = s.find("://");
        const auto path = s.find('/', scheme + 3);
        if (path != std::string::npos) { s.erase(path); }
        return s;
    }
}

class UberSDRSourceModule : public ModuleManager::Instance {
public:
    static constexpr int BUFFER_MS_MIN = 0;
    static constexpr int BUFFER_MS_MAX = 1000;
    static constexpr size_t RECENT_MAX = 8;
    // Link-rate guard: the arrival rate over each window against the
    // header rate; two windows below the threshold flag the link as slow.
    static constexpr auto kGuardWindow = std::chrono::seconds(5);
    static constexpr double kGuardThreshold = 0.98;

    UberSDRSourceModule(std::string name) : client(ubersdr::UberSDRClient::newSessionId()) {
        this->name = name;
        // Not served headless: a client should reach the receiver directly.
        if (core::args["server"].b()) { return; }

        {
            auto configAccess = config.read();
            std::string url, password, mode;
            if (configAccess.tryGet("url", url)) { std::snprintf(urlBuf, sizeof urlBuf, "%s", url.c_str()); }
            if (configAccess.tryGet("password", password)) { std::snprintf(passwordBuf, sizeof passwordBuf, "%s", password.c_str()); }
            if (configAccess.tryGet("iqMode", mode)) { iqModeId = modeIndex(mode); }
            int ms = bufferMs.load();
            configAccess.tryGet("bufferMs", ms);
            bufferMs = std::clamp(ms, BUFFER_MS_MIN, BUFFER_MS_MAX);
            configAccess.tryGet("serverWaterfall", serverWaterfall);
            json recentJson = configAccess.value("recent", json::array());
            if (recentJson.is_array()) {
                for (const auto& r : recentJson) {
                    if (r.is_string() && recent.size() < RECENT_MAX) { recent.push_back(r.get<std::string>()); }
                }
            }
        }

        chain.init(&rawStream, &stream);
        chain.setSampleRate(kIqModes[iqModeId].rate);
        chain.setPrebufferMsec(bufferMs.load(), false, false);

        client.onIQ = [this](const ubersdr::UberSDRClient::PacketInfo& info, const dsp::complex_t* samples, int count) {
            onIQ(info, samples, count);
        };
        client.onStatus = [this](const nlohmann::json& j) { onPrimaryStatus(j); };
        client.onError = [this](const std::string& e) { setStatus("Error: " + e); };
        spectrum.onFrame = [this](const ubersdr::UberSpectrumClient::Frame& f) { onSpectrumFrame(f); };
        spectrum.onConfig = [this](const ubersdr::UberSpectrumClient::Config& c) {
            configBinBandwidth = c.binBandwidth;
            configSpan = c.binCount * c.binBandwidth;
            if (defaultSpan.load() <= 0.0) { defaultSpan = c.totalBandwidth > 0.0 ? c.totalBandwidth : c.binCount * c.binBandwidth; }
        };
        spectrum.onError = [this](const std::string& e, int status) {
            if (status == 429) { rateLimitedTicks = std::chrono::steady_clock::now().time_since_epoch().count(); }
            flog::warn("UberSDRSourceModule: spectrum: {} ({})", e, status);
        };
        provider.module = this;
        client.onConnected = [this] {
            packetsSinceEcho = -1;
            lastEchoW = sigpath::vfoManager.committedIQWindowOffset();
            connected = true;
            setStatus("Connected");
        };
        client.onDisconnected = [this] {
            connected = false;
            if (running.load()) {
                running = false;
                // As kiwisdr_source does: the play button follows the session.
                gui::mainWindow.setPlayState(false);
            }
        };

        handler.ctx = this;
        handler.selectHandler = menuSelected;
        handler.deselectHandler = menuDeselected;
        handler.menuHandler = menuHandler;
        handler.startHandler = start;
        handler.stopHandler = stop;
        handler.tuneHandler = tune;
        handler.uiTickHandler = uiTick;
        handler.stream = &stream;
        sigpath::sourceManager.registerSource("UberSDR", &handler);

        remote::RemoteHandler remoteHandler;
        remoteHandler.kind = remote::RemoteKind::UberSDR;
        remoteHandler.sourceName = "UberSDR";
        remoteHandler.probe = probe;
        remoteHandler.connect = [this](const remote::RemoteEntry& entry) { connectRemote(entry); };
        sigpath::remoteRegistry.registerHandler(remoteHandler);

        vfoDeleteHandler.handler = onVfoDelete;
        vfoDeleteHandler.ctx = this;
        sigpath::vfoManager.onVfoDelete.bindHandler(&vfoDeleteHandler);
    }

    ~UberSDRSourceModule() {
        if (core::args["server"].b()) { return; }
        sigpath::remoteRegistry.unregisterHandler(remote::RemoteKind::UberSDR);
        stop(this);
        sigpath::vfoManager.onVfoDelete.unbindHandler(&vfoDeleteHandler);
        sigpath::sourceManager.unregisterSource("UberSDR");
    }

    void postInit() {}
    void enable() { enabled = true; }
    void disable() { enabled = false; }
    bool isEnabled() { return enabled; }

private:
    // The remote browser's Test: POST /connection, then the first iq48
    // packet on /ws, then close. Worker thread.
    static remote::ProbeResult probe(const remote::RemoteEntry& entry, const std::atomic<bool>& cancel) {
        using Clock = std::chrono::steady_clock;
        remote::ProbeResult result;
        const std::string base = normaliseBaseUrl(entry.target.url());
        ubersdr::UberSDRClient client(ubersdr::UberSDRClient::newSessionId());
        const auto t0 = Clock::now();
        const auto admission = client.admit(base, std::string(), [&cancel] { return cancel.load(); });
        if (cancel) {
            result.reason = "cancelled";
            return result;
        }
        if (!admission.allowed) {
            result.reason = admission.reason.empty() ? "not admitted (HTTP " + std::to_string(admission.httpStatus) + ")" : admission.reason;
            return result;
        }
        result.rttMs = (int)std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count();
        std::string modes;
        for (const auto& m : admission.iqModes) { modes += (modes.empty() ? "" : ", ") + m; }
        result.negotiated.emplace_back("IQ modes", modes.empty() ? "none" : modes);
        if (admission.maxSessionS > 0) { result.negotiated.emplace_back("Session limit", std::to_string(admission.maxSessionS / 60) + " min"); }
        result.negotiated.emplace_back("VFOs", std::to_string(admission.vfos));

        std::atomic<int> rate{ 0 };
        std::atomic<bool> closed{ false };
        client.onIQ = [&rate](const ubersdr::UberSDRClient::PacketInfo& info, const dsp::complex_t*, int count) {
            if (count > 0 && rate.load() == 0) { rate = info.sampleRate > 0 ? info.sampleRate : 1; }
        };
        client.onDisconnected = [&closed] { closed = true; };
        const double freq = entry.ranges.empty() ? 14074000.0 : (double)(entry.ranges.front().first + entry.ranges.front().second) / 2.0;
        client.start(base, "iq48", freq);
        const auto wsStart = Clock::now();
        while (!cancel && rate.load() == 0 && !closed && Clock::now() - wsStart < std::chrono::seconds(5)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        client.stop();
        if (cancel) { result.reason = "cancelled"; }
        else if (rate.load() > 0) {
            result.ok = true;
            result.negotiated.emplace_back("IQ rate", std::to_string(rate.load()) + " S/s");
        }
        else { result.reason = closed ? "admitted, but the IQ stream closed" : "admitted, but no IQ within 5 s"; }
        return result;
    }

    // The remote browser's Connect: the browser has selected this source.
    void connectRemote(const remote::RemoteEntry& entry) {
        const std::string url = normaliseBaseUrl(entry.target.url());
        std::snprintf(urlBuf, sizeof urlBuf, "%s", url.c_str());
        config.edit().set("url", url);
        gui::mainWindow.setPlayState(true);
    }

    void setStatus(const std::string& s) {
        std::lock_guard lck(statusMtx);
        status = s;
    }
    std::string getStatus() {
        std::lock_guard lck(statusMtx);
        return status;
    }
    // The mode the primary streams; the admission worker and the UI set it.
    void setActiveMode(const std::string& m) {
        std::lock_guard lck(statusMtx);
        activeMode = m;
    }
    std::string getActiveMode() {
        std::lock_guard lck(statusMtx);
        return activeMode;
    }

    static void menuSelected(void* ctx) {
        UberSDRSourceModule* _this = (UberSDRSourceModule*)ctx;
        _this->appliedRate = kIqModes[_this->iqModeId].rate;
        core::setInputSampleRate(_this->appliedRate);
        flog::info("UberSDRSourceModule '{0}': Menu Select!", _this->name);
    }

    static void menuDeselected(void* ctx) {
        UberSDRSourceModule* _this = (UberSDRSourceModule*)ctx;
        // The UI tick stops with the selection: withdraw the provider and
        // close the secondaries here, also on the way to the destructor
        // (unregisterSource deselects first).
        _this->unregisterProvider();
        flog::info("UberSDRSourceModule '{0}': Menu Deselect!", _this->name);
    }

    // UI thread, once per frame while selected: the network thread only
    // stores what the server reported.
    static void uiTick(void* ctx) {
        UberSDRSourceModule* _this = (UberSDRSourceModule*)ctx;
        const int rate = _this->headerRate.load();
        if (rate > 0 && rate != _this->appliedRate) {
            flog::info("UberSDRSourceModule '{0}': stream rate {1}", _this->name, rate);
            _this->appliedRate = rate;
            core::setInputSampleRate(rate);
            _this->applyBufferMode(true);
            _this->placedWidth = -1.0;
        }
        _this->serverWaterfallTick();
        if (_this->providerRegistered) {
            _this->placeIQWindow();
            _this->manageSecondaries();
        }
        _this->sendPrimaryTune(true);
        _this->commitStaleTunes();
    }

    // UI thread: commit window moves whose echo never came, with 40 ms of
    // mute.
    void commitStaleTunes() {
        const auto now = Clock::now();
        double w = 0.0;
        bool stale;
        {
            std::lock_guard lck(pendingMtx);
            stale = takeStaleTune(pendingTunes, now, w);
        }
        if (stale && windowLive.load() && std::fabs(w - sigpath::vfoManager.committedIQWindowOffset()) >= 0.5) {
            sigpath::vfoManager.muteAllFor((int)(0.04 * appliedRate));
            sigpath::vfoManager.commitIQWindowOffset(w);
        }
        for (auto& sec : secondaries) {
            bool secStale;
            {
                std::lock_guard lck(sec->mtx);
                secStale = takeStaleTune(sec->pending, now, w);
            }
            if (secStale && std::fabs(w - sec->committedW.load()) >= 0.5) {
                sigpath::vfoManager.muteVFOFor(sec->vfo, (int)(0.04 * sec->appliedRate));
                sec->committedW = w;
                sigpath::vfoManager.commitVFOWindowOffset(sec->vfo, w);
            }
        }
    }

    // Server waterfall ------------------------------------------------------

    struct Provider : public SpectrumProvider {
        UberSDRSourceModule* module = nullptr;
        std::string name() const override { return "UberSDR"; }
        int binCount() const override { return 1024; }
        std::vector<double> zoomSpans() const override { return module->zoomLadder; }
        void requestSpan(double spanHz) override { module->wantBinBandwidth = spanHz / 1024.0; }
        bool available() const override { return module->spectrumAlive(); }
        std::string unavailableReason() const override { return "no spectrum frames from the server"; }
        // Front-end dBFS.
        std::pair<float, float> defaultLevels() const override { return { -130.0f, -60.0f }; }
    };

    using Clock = std::chrono::steady_clock;
    static constexpr auto kFrameTimeout = std::chrono::seconds(3);
    static constexpr auto kViewInterval = std::chrono::milliseconds(200);  // at most 5 per second
    static constexpr auto kRateLimitBackoff = std::chrono::seconds(2);
    static constexpr double kMinBinBandwidth = 0.5;

    // The IQ the primary carries usefully: 90 % of its rate.
    double iqWindowWidth() const { return 0.9 * (double)appliedRate; }

    bool spectrumAlive() const {
        if (!running.load() || !spectrum.connected()) { return false; }
        const auto now = Clock::now();
        const auto last = Clock::time_point(Clock::duration(lastFrameTicks.load()));
        return now - last < kFrameTimeout || now - registeredAt < kFrameTimeout + std::chrono::seconds(2);
    }

    // Spans from the default view down to 1 024 bins of 0.5 Hz, in steps of
    // sqrt(2); the zoom is continuous on the server.
    void buildLadder(double widest) {
        zoomLadder.clear();
        for (double s = widest; s >= 1024.0 * kMinBinBandwidth * 0.999; s /= std::sqrt(2.0)) { zoomLadder.push_back(s); }
    }

    void serverWaterfallTick() {
        uiTuningOffset = sigpath::sourceManager.getTuningOffset();
        uiCentre = gui::waterfall.getCenterFrequency();
        uiInvertIQ = sigpath::iqFrontEnd.getInvertIQ();

        // Core takes the provider away when it reports unavailable, and has
        // cleared the IQ window and display span by then. It is not
        // registered again until the next session.
        if (providerRegistered && sigpath::iqFrontEnd.spectrumProvider() != &provider) {
            closeAllSecondaries();
            providerRegistered = false;
            frameSink = false;
            windowLive = false;
            intendedW = 0.0;
            appliedSpan = -1.0;
            fellBack = true;
            sendPrimaryTune(false);
        }
        if (!(serverWaterfall && running.load() && connected.load())) {
            unregisterProvider();
            return;
        }
        const auto now = Clock::now();
        if (!providerRegistered) {
            if (fellBack || !spectrum.connected() || configSpan.load() <= 0.0 || sigpath::iqFrontEnd.spectrumProvider() != nullptr) { return; }
            buildLadder(defaultSpan.load() > 0.0 ? defaultSpan.load() : configSpan.load());
            wantBinBandwidth = configBinBandwidth.load();
            sentBinBandwidth = configBinBandwidth.load();
            sentCentre = lastTune;
            appliedSpan = -1.0;
            registeredAt = now;
            lastFrameTicks = 0;
            sigpath::iqFrontEnd.setSpectrumProvider(&provider);
            providerRegistered = true;
            frameSink = true;
            intendedW = 0.0;
            placedWidth = iqWindowWidth();
            windowLive = true;
            sigpath::vfoManager.setIQWindow(0.0, placedWidth);
        }

        // The span the server confirmed.
        const double span = configSpan.load();
        if (span > 0.0 && std::fabs(span - appliedSpan) > 0.5) {
            appliedSpan = span;
            core::setDisplaySpan(span);
        }

        // View: zoom when the bin width changes, pan when only the centre
        // does; at most 5 per second, and quiet for a while after a 429.
        const double bw = std::max(kMinBinBandwidth, wantBinBandwidth.load());
        const double centre = lastTune;
        const bool zoomChange = std::fabs(bw - sentBinBandwidth) > 1e-6 * bw;
        const bool panChange = std::fabs(centre - sentCentre) >= 1.0;
        const auto limited = Clock::time_point(Clock::duration(rateLimitedTicks.load()));
        if ((zoomChange || panChange) && now - lastViewSend >= kViewInterval && now - limited >= kRateLimitBackoff) {
            if (zoomChange) { spectrum.zoom(centre, bw); }
            else { spectrum.pan(centre, bw); }
            sentBinBandwidth = bw;
            sentCentre = centre;
            lastViewSend = now;
        }
    }

    // UI thread.
    void unregisterProvider() {
        if (!providerRegistered) { return; }
        closeAllSecondaries();
        providerRegistered = false;
        frameSink = false;
        if (sigpath::iqFrontEnd.spectrumProvider() == &provider) {
            sigpath::iqFrontEnd.setSpectrumProvider(nullptr);
        }
        core::clearDisplaySpan();
        windowLive = false;
        intendedW = 0.0;
        sigpath::vfoManager.clearIQWindow();
        appliedSpan = -1.0;
        // Back to the display centre.
        sendPrimaryTune(false);
    }

    // Network thread (IQ): the link index of each packet's first sample by
    // its header timestamp, for spectrum presentation.
    struct TimeMark {
        uint64_t ns = 0;
        uint64_t index = 0;
    };
    void recordTime(uint64_t ns, uint64_t index) {
        if (ns == 0) { return; }
        std::lock_guard lck(timeMtx);
        timeRing[timeHead] = TimeMark{ ns, index };
        timeHead = (timeHead + 1) % timeRing.size();
        timeCount = std::min(timeCount + 1, timeRing.size());
    }

    // When the IQ sample captured at ns leaves the link: interpolated between
    // recorded packets, extrapolated at the stream rate past the newest.
    Clock::time_point playoutAt(uint64_t ns) {
        TimeMark lo{}, hi{};
        bool haveLo = false, haveHi = false;
        {
            std::lock_guard lck(timeMtx);
            for (size_t i = 0; i < timeCount; i++) {
                const TimeMark& m = timeRing[i];
                if (m.ns <= ns && (!haveLo || m.ns > lo.ns)) { lo = m; haveLo = true; }
                if (m.ns >= ns && (!haveHi || m.ns < hi.ns)) { hi = m; haveHi = true; }
            }
        }
        const double rate = headerRate.load() > 0 ? (double)headerRate.load() : (double)appliedRate;
        double index;
        if (haveLo && haveHi && hi.ns > lo.ns) {
            index = (double)lo.index + (double)(hi.index - lo.index) * (double)(ns - lo.ns) / (double)(hi.ns - lo.ns);
        }
        else if (haveLo) {
            index = (double)lo.index + (double)(ns - lo.ns) * 1e-9 * rate;
        }
        else if (haveHi) {
            index = std::max(0.0, (double)hi.index - (double)(hi.ns - ns) * 1e-9 * rate);
        }
        else {
            return Clock::now();
        }
        return chain.playoutTime((uint64_t)std::llround(index));
    }

    // Network thread (spectrum): one frame to core, in the display domain.
    void onSpectrumFrame(const ubersdr::UberSpectrumClient::Frame& f) {
        lastFrameTicks = Clock::now().time_since_epoch().count();
        const double bw = configBinBandwidth.load();
        if (!frameSink.load() || f.count <= 0 || bw <= 0.0) { return; }
        specFrame.bins.assign(f.binsDb, f.binsDb + f.count);
        const double span = bw * f.count;
        // Bin k is centred at centre + (k - N/2) * bw.
        double start = f.centreHz - ((double)(f.count / 2) + 0.5) * bw + uiTuningOffset.load();
        if (uiInvertIQ.load()) {
            std::reverse(specFrame.bins.begin(), specFrame.bins.end());
            start = 2.0 * uiCentre.load() - (start + span);
        }
        specFrame.startHz = start;
        specFrame.spanHz = span;
        specFrame.presentAt = playoutAt(f.timestampNanos);
        sigpath::iqFrontEnd.pushSpectrum(std::move(specFrame));
    }

    // IQ window ---------------------------------------------------------------
    //
    // While the server waterfall is the display, the primary IQ is a window
    // at C + W placed over the VFOs (iq_window_policy.h); W is display Hz.
    // A tune is answered by a status echo in the IQ socket, and the switch
    // lands in the server's block timing two packets after it (ubersdr.md
    // section 4.2), where W is committed.

    static constexpr auto kRetuneInterval = std::chrono::milliseconds(125);  // at most 8 per second
    static constexpr double kMinHz = 10e3;
    static constexpr double kMaxHz = 30e6;
    static constexpr int kSecondaryRate = 12000;
    static constexpr size_t kMaxPendingTunes = 16;

    static constexpr auto kEchoTimeout = std::chrono::seconds(2);

    struct PendingTune {
        double freq = 0.0;  // as sent, whole Hz
        double w = 0.0;
        Clock::time_point sentAt{};
    };

    // Tunes never echoed (the server may clamp a frequency, or drop the
    // message): after kEchoTimeout, the newest such W, to commit at once.
    static bool takeStaleTune(std::deque<PendingTune>& pending, Clock::time_point now, double& w) {
        bool stale = false;
        while (!pending.empty() && now - pending.front().sentAt > kEchoTimeout) {
            w = pending.front().w;
            pending.pop_front();
            stale = true;
        }
        return stale;
    }

    double hardwareOffset(double w) const { return sigpath::iqFrontEnd.getInvertIQ() ? -w : w; }

    // UI thread, while the provider is registered.
    void placeIQWindow() {
        IQWindowInput in;
        const std::string& selected = gui::waterfall.selectedVFO;
        auto add = [&in](const std::string& name, const ImGui::WaterfallVFO* v) {
            if (!sigpath::vfoManager.isVFORouted(name)) { in.passbands.emplace_back(v->lowerOffset, v->upperOffset); }
        };
        if (auto sel = gui::waterfall.vfos.find(selected); sel != gui::waterfall.vfos.end()) { add(sel->first, sel->second); }
        for (auto const& [name, v] : gui::waterfall.vfos) {
            if (name != selected) { add(name, v); }
        }
        in.currentOffset = intendedW;
        in.width = iqWindowWidth();
        in.margin = 0.1 * in.width;
        // The window inside the receiver's range (0-30 MHz, 10 kHz kept off DC).
        const double lo = kMinHz + in.width / 2.0 - lastTune;
        const double hi = kMaxHz - in.width / 2.0 - lastTune;
        const bool invert = sigpath::iqFrontEnd.getInvertIQ();
        in.minOffset = invert ? -hi : lo;
        in.maxOffset = invert ? -lo : hi;
        const double w = chooseIQWindow(in);
        if (std::fabs(w - intendedW) >= 1.0 || in.width != placedWidth) {
            {
                std::lock_guard tl(tuneMtx);
                intendedW = w;
            }
            placedWidth = in.width;
            sigpath::vfoManager.setIQWindow(w, in.width);
        }
    }

    // Tune the primary to C + W. From tune() at once (any thread: rigctl
    // tunes too), from the UI tick when the window moved (at most 8 per
    // second).
    void sendPrimaryTune(bool rateLimited) {
        if (!running.load() || !connected.load()) { return; }
        std::lock_guard tl(tuneMtx);
        const double w = providerRegistered ? intendedW : 0.0;
        const double target = std::round(lastTune + hardwareOffset(w));
        if (sentTuneValid && std::fabs(target - sentTuneFreq) < 0.5) { return; }
        const auto now = Clock::now();
        if (rateLimited && now - lastTuneSend < kRetuneInterval) { return; }
        {
            std::lock_guard lck(pendingMtx);
            pendingTunes.push_back(PendingTune{ target, w, now });
            while (pendingTunes.size() > kMaxPendingTunes) { pendingTunes.pop_front(); }
        }
        client.tune(target, getActiveMode());
        sentTuneFreq = target;
        sentTuneValid = true;
        lastTuneSend = now;
    }

    // Network thread (IQ): a status message. The echo of a pending tune
    // starts the commit count; older pending tunes were superseded.
    void onPrimaryStatus(const nlohmann::json& j) {
        if (j.contains("sampleRate") && j["sampleRate"].is_number()) { headerRate = j["sampleRate"].get<int>(); }
        auto it = j.find("frequency");
        if (it == j.end() || !it->is_number()) { return; }
        const double f = it->get<double>();
        std::lock_guard lck(pendingMtx);
        for (size_t i = 0; i < pendingTunes.size(); i++) {
            if (std::fabs(pendingTunes[i].freq - f) < 1.0) {
                echoW = pendingTunes[i].w;
                packetsSinceEcho = 0;
                pendingTunes.erase(pendingTunes.begin(), pendingTunes.begin() + (std::ptrdiff_t)i + 1);
                return;
            }
        }
    }

    // Network thread (IQ), before a packet enters the link: mute from the
    // first packet after the echo through the fifth when W moves (the
    // switch fell 2 packets after the echo in 17 of 20 retunes, 3-5 in the
    // rest), and commit W 7 samples before the end of the second.
    void echoProgress(uint64_t first, int count) {
        if (packetsSinceEcho < 0) { return; }
        packetsSinceEcho++;
        if (packetsSinceEcho == 1 && std::fabs(echoW - lastEchoW) >= 0.5) {
            const int mute = 5 * std::max(count, 360);
            chain.scheduleMarker(first, [this, mute] {
                if (windowLive.load()) { sigpath::vfoManager.muteAllFor(mute); }
            });
        }
        else if (packetsSinceEcho == 2) {
            const double w = echoW;
            chain.scheduleMarker(first + (uint64_t)std::max(0, count - 7), [this, w] {
                if (windowLive.load()) { sigpath::vfoManager.commitIQWindowOffset(w); }
            });
            lastEchoW = w;
            packetsSinceEcho = -1;
        }
    }

    // Secondary VFOs ------------------------------------------------------------
    //
    // A VFO outside the primary window gets a stream of its own while the
    // receiver has secondaries to give (admission "vfos" - 1): /ws?vfo=N in
    // plain "iq", 12 kHz, on the primary's session id. Its RxVFO is fed from
    // that stream (VFOManager::routeVFOInput) through a PrebufferedLink with
    // the primary's target, so the audio stays in step.

    struct Secondary {
        explicit Secondary(const std::string& sessionId) : client(sessionId) {}
        int slot = 1;            // vfo=N
        std::string vfo;
        ubersdr::UberSDRClient client;
        dsp::stream<dsp::complex_t> raw;
        dsp::stream<dsp::complex_t> out;
        dsp::buffer::PrebufferedLink<dsp::complex_t> link;
        std::atomic<bool> failed{false};
        std::atomic<bool> invert{false};
        std::atomic<int> headerRate{0};
        std::atomic<double> committedW{0.0};
        std::mutex mtx;          // error, pending
        std::string error;
        std::deque<PendingTune> pending;
        // UI thread.
        double intendedW = 0.0;
        double sentFreq = 0.0;
        Clock::time_point lastSend{};
        int appliedRate = kSecondaryRate;
        // Network thread: the last two packets' link positions.
        uint64_t pushed = 0;
        uint64_t prevFirst = 0;
        int prevCount = 0;
        uint64_t lastFirst = 0;
        int lastCount = 0;
    };

    double secondaryWidth(int rate = kSecondaryRate) const { return 0.9 * (double)rate; }

    Secondary* findSecondary(const std::string& vfo) {
        for (auto& s : secondaries) {
            if (s->vfo == vfo) { return s.get(); }
        }
        return nullptr;
    }

    // UI thread: the lowest vfo=N neither in use nor refused, or 0.
    int freeSlot() {
        int vfos;
        {
            std::lock_guard lck(admissionMtx);
            vfos = admission.vfos;
        }
        for (int n = 1; n < vfos; n++) {
            if (slotRefusals.count(n)) { continue; }
            bool used = false;
            for (const auto& s : secondaries) { used = used || s->slot == n; }
            if (!used) { return n; }
        }
        return 0;
    }

    // UI thread.
    void openSecondary(int slot, const std::string& vfo, double w) {
        auto s = std::make_unique<Secondary>(client.sessionId());
        Secondary* p = s.get();
        p->slot = slot;
        p->vfo = vfo;
        p->intendedW = w;
        p->committedW = w;
        p->invert = sigpath::iqFrontEnd.getInvertIQ();
        p->link.init(&p->raw, &p->out);
        p->link.setSampleRate(kSecondaryRate);
        p->link.setPrebufferMsec(bufferMs.load(), false, false);
        p->client.onIQ = [this, p](const ubersdr::UberSDRClient::PacketInfo& info, const dsp::complex_t* samples, int count) {
            onSecondaryIQ(*p, info, samples, count);
        };
        p->client.onStatus = [this, p](const nlohmann::json& j) { onSecondaryStatus(*p, j); };
        p->client.onError = [p](const std::string& e) {
            std::lock_guard lck(p->mtx);
            if (p->error.empty()) { p->error = e; }
            p->failed = true;
        };
        p->client.onDisconnected = [p] {
            {
                std::lock_guard lck(p->mtx);
                if (p->error.empty()) { p->error = "closed by the server"; }
            }
            p->failed = true;
        };
        p->link.start();
        if (!sigpath::vfoManager.routeVFOInput(vfo, &p->out, kSecondaryRate, w, secondaryWidth())) {
            p->link.stop();
            return;
        }
        p->sentFreq = std::round(lastTune + hardwareOffset(w));
        p->lastSend = Clock::now();
        p->client.start(baseUrl, "iq", p->sentFreq, passwordBuf, slot);
        flog::info("UberSDRSourceModule '{0}': VFO '{1}' on secondary {2}", name, vfo, slot);
        secondaries.push_back(std::move(s));
    }

    // UI thread: route the VFO back, then stop the stream.
    void closeSecondary(Secondary& s) {
        if (sigpath::vfoManager.vfoExists(s.vfo)) { sigpath::vfoManager.routeVFOInput(s.vfo, nullptr, 0.0); }
        // Release a network thread blocked in raw.swap() before joining it.
        s.raw.stopWriter();
        s.client.stop();
        s.link.stop();
        s.raw.clearWriteStop();
    }

    void closeAllSecondaries() {
        for (auto& s : secondaries) { closeSecondary(*s); }
        secondaries.clear();
    }

    static void onVfoDelete(VFOManager::VFO* vfo, void* ctx) {
        UberSDRSourceModule* _this = (UberSDRSourceModule*)ctx;
        const std::string vfoName = vfo->getName();
        auto& list = _this->secondaries;
        for (auto it = list.begin(); it != list.end(); ++it) {
            if ((*it)->vfo == vfoName) {
                _this->closeSecondary(**it);
                list.erase(it);
                return;
            }
        }
    }

    // UI thread, while the provider is registered: give out-of-window VFOs
    // a secondary, keep each secondary's window over its VFO, and return a
    // VFO to the primary once it is well inside the primary window.
    void manageSecondaries() {
        for (auto it = secondaries.begin(); it != secondaries.end();) {
            Secondary& s = **it;
            if (!s.failed.load()) {
                ++it;
                continue;
            }
            std::string reason;
            {
                std::lock_guard lck(s.mtx);
                reason = s.error;
            }
            flog::warn("UberSDRSourceModule '{0}': secondary {1} for '{2}': {3}", name, s.slot, s.vfo, reason);
            slotRefusals[s.slot] = reason;
            closeSecondary(s);
            it = secondaries.erase(it);
        }

        const double pw = iqWindowWidth();
        const double pLo = intendedW - pw / 2.0;
        const double pHi = intendedW + pw / 2.0;
        const double pMargin = 0.1 * pw;
        const bool invert = sigpath::iqFrontEnd.getInvertIQ();
        const auto now = Clock::now();
        std::vector<std::string> release;
        for (auto const& [vfoName, v] : gui::waterfall.vfos) {
            const double lo = v->lowerOffset, hi = v->upperOffset;
            Secondary* s = findSecondary(vfoName);
            if (!s) {
                const bool outside = lo < pLo || hi > pHi;
                if (outside && hi - lo <= 0.9 * secondaryWidth()) {
                    const int slot = freeSlot();
                    if (slot > 0) { openSecondary(slot, vfoName, (lo + hi) / 2.0); }
                }
                continue;
            }
            const double sw = secondaryWidth(s->appliedRate);
            if ((lo >= pLo + pMargin && hi <= pHi - pMargin) || hi - lo > sw) {
                release.push_back(vfoName);
                continue;
            }
            s->invert = invert;
            // The stream's rate as the server reports it.
            const int rate = s->headerRate.load();
            if (rate > 0 && rate != s->appliedRate) {
                s->appliedRate = rate;
                s->link.setSampleRate(rate);
                sigpath::vfoManager.routeVFOInput(vfoName, &s->out, rate, s->committedW.load(), secondaryWidth(rate));
            }
            // Recentre when the passband nears the stream's edge.
            const double margin = 0.1 * sw;
            if (lo < s->intendedW - sw / 2.0 + margin || hi > s->intendedW + sw / 2.0 - margin) {
                s->intendedW = (lo + hi) / 2.0;
                sigpath::vfoManager.setVFOWindow(vfoName, s->intendedW, sw);
            }
            const double target = std::round(lastTune + hardwareOffset(s->intendedW));
            if (std::fabs(target - s->sentFreq) >= 0.5 && now - s->lastSend >= kRetuneInterval && s->client.connected()) {
                {
                    std::lock_guard lck(s->mtx);
                    s->pending.push_back(PendingTune{ target, s->intendedW, now });
                    while (s->pending.size() > kMaxPendingTunes) { s->pending.pop_front(); }
                }
                s->client.tune(target, "iq");
                s->sentFreq = target;
                s->lastSend = now;
            }
        }
        for (const auto& vfoName : release) {
            for (auto it = secondaries.begin(); it != secondaries.end(); ++it) {
                if ((*it)->vfo == vfoName) {
                    closeSecondary(**it);
                    secondaries.erase(it);
                    break;
                }
            }
        }
    }

    // Network thread (secondary): into its link, conjugated when the IQ is
    // inverted (a routed VFO bypasses the front end's pre-processing).
    void onSecondaryIQ(Secondary& s, const ubersdr::UberSDRClient::PacketInfo& info, const dsp::complex_t* samples, int count) {
        if (info.sampleRate > 0) { s.headerRate = info.sampleRate; }
        s.prevFirst = s.lastFirst;
        s.prevCount = s.lastCount;
        s.lastFirst = s.pushed;
        s.lastCount = count;
        const bool invert = s.invert.load();
        for (int done = 0; done < count;) {
            const int n = std::min(count - done, s.raw.capacity());
            if (invert) {
                for (int i = 0; i < n; i++) { s.raw.writeBuf[i] = dsp::complex_t{ samples[done + i].re, -samples[done + i].im }; }
            }
            else {
                memcpy(s.raw.writeBuf, samples + done, (size_t)n * sizeof(dsp::complex_t));
            }
            if (!s.raw.swap(n)) { return; }
            s.pushed += (uint64_t)n;
            done += n;
        }
    }

    // Network thread (secondary): the echo arrives with the switch packet or
    // one packet after it, and the switch sits 7 samples before that
    // packet's end (ubersdr.md section 4.2). Commit at the earlier
    // candidate and mute across both.
    void onSecondaryStatus(Secondary& s, const nlohmann::json& j) {
        auto it = j.find("frequency");
        if (it == j.end() || !it->is_number()) { return; }
        const double f = it->get<double>();
        double w = 0.0;
        {
            std::lock_guard lck(s.mtx);
            size_t i = 0;
            while (i < s.pending.size() && std::fabs(s.pending[i].freq - f) >= 1.0) { i++; }
            if (i == s.pending.size()) { return; }
            w = s.pending[i].w;
            s.pending.erase(s.pending.begin(), s.pending.begin() + (std::ptrdiff_t)i + 1);
        }
        const bool havePrev = s.prevCount > 0;
        const uint64_t switchEnd = havePrev ? s.prevFirst + (uint64_t)s.prevCount : s.lastFirst + (uint64_t)s.lastCount;
        const uint64_t muteStart = switchEnd > 20 ? switchEnd - 20 : 0;
        const int mute = (havePrev ? s.lastCount : 0) + 40;
        const uint64_t commitAt = switchEnd > 7 ? switchEnd - 7 : 0;
        const std::string vfo = s.vfo;
        Secondary* p = &s;
        s.link.scheduleMarker(muteStart, [vfo, mute] { sigpath::vfoManager.muteVFOFor(vfo, mute); });
        s.link.scheduleMarker(commitAt, [vfo, w, p] {
            p->committedW = w;
            sigpath::vfoManager.commitVFOWindowOffset(vfo, w);
        });
    }

    static void start(void* ctx) {
        UberSDRSourceModule* _this = (UberSDRSourceModule*)ctx;
        if (_this->running.exchange(true)) { return; }
        if (_this->worker.joinable()) { _this->worker.join(); }
        // UI thread. A stop() on a network thread leaves the provider and
        // the secondaries to the UI tick; one may not have run since.
        _this->unregisterProvider();
        _this->appliedRate = kIqModes[_this->iqModeId].rate;
        core::setInputSampleRate(_this->appliedRate);
        _this->headerRate = 0;
        _this->linkSlow = false;
        _this->configSpan = 0.0;
        _this->configBinBandwidth = 0.0;
        _this->defaultSpan = 0.0;
        {
            std::lock_guard lck(_this->bufferModeMtx);
            _this->rawStream.clearWriteStop();
            _this->applyBufferModeLocked(true);
            // A block left unread by the last session would shift this
            // session's input indices (PrebufferedLink::discardUnreadInput).
            _this->chain.discardUnreadInput();
            _this->chain.start();
        }
        {
            std::lock_guard lck(_this->timeMtx);
            _this->timeCount = 0;
            _this->timeHead = 0;
        }
        // The link has consumed everything pushed so far.
        _this->linkIndexBase = _this->chain.samplesIn();
        _this->samplesPushed = 0;
        {
            std::lock_guard lck(_this->pendingMtx);
            _this->pendingTunes.clear();
        }
        _this->intendedW = 0.0;
        _this->sentTuneFreq = std::round(_this->lastTune);
        _this->sentTuneValid = true;
        _this->slotRefusals.clear();
        _this->fellBack = false;
        _this->baseUrl = normaliseBaseUrl(_this->urlBuf);
        _this->rememberServer(_this->urlBuf);
        const std::string password = _this->passwordBuf;
        const std::string mode = kIqModes[_this->iqModeId].key;
        const double freq = _this->lastTune;
        _this->setStatus("Connecting...");
        // Admission is an HTTPS request: off the UI thread.
        _this->worker = std::thread([_this, password, mode, freq] {
            auto a = _this->client.admit(_this->baseUrl, password, [_this] { return !_this->running.load(); });
            {
                std::lock_guard lck(_this->admissionMtx);
                _this->admission = a;
                _this->admittedAt = std::chrono::steady_clock::now();
            }
            if (!_this->running.load()) { return; }
            if (!a.allowed) {
                _this->setStatus("Refused: " + a.reason);
                _this->running = false;
                gui::mainWindow.setPlayState(false);
                return;
            }
            // The chosen width when allowed, else the widest allowed below it.
            std::string useMode = mode;
            if (!a.iqModes.empty() && std::find(a.iqModes.begin(), a.iqModes.end(), mode) == a.iqModes.end()) {
                useMode = a.iqModes.front();
                for (int i = modeIndex(mode); i >= 0; i--) {
                    if (std::find(a.iqModes.begin(), a.iqModes.end(), kIqModes[i].key) != a.iqModes.end()) {
                        useMode = kIqModes[i].key;
                        break;
                    }
                }
            }
            _this->setActiveMode(useMode);
            _this->setStatus("Admitted, opening IQ (" + useMode + ")");
            _this->client.start(_this->baseUrl, useMode, freq, password);
            if (_this->serverWaterfall) {
                // The server's default (shared, 30 MHz) view first.
                _this->spectrum.start(_this->baseUrl, _this->client.sessionId(), 0.0, 0.0, password);
            }
        });
        flog::info("UberSDRSourceModule '{0}': Start!", _this->name);
    }

    static void stop(void* ctx) {
        UberSDRSourceModule* _this = (UberSDRSourceModule*)ctx;
        const bool wasRunning = _this->running.exchange(false);
        if (!wasRunning && !_this->client.running() && !_this->worker.joinable()) { return; }
        // May run on the primary's network thread (a server close) or on the
        // admission worker (a refusal), both through setPlayState(false), so
        // no GUI work here: the secondaries, routed VFOs and the provider are
        // UI-thread state, released by unregisterProvider() from the UI tick
        // (running is false now), from menuDeselected() or from start().
        // Release a writer blocked in rawStream.swap() before joining the
        // network thread that is that writer.
        _this->rawStream.stopWriter();
        // On the worker itself, start() joins it.
        if (_this->worker.joinable() && _this->worker.get_id() != std::this_thread::get_id()) { _this->worker.join(); }
        _this->spectrum.stop();
        _this->client.stop();
        {
            std::lock_guard lck(_this->bufferModeMtx);
            _this->chain.stop();
            _this->rawStream.clearWriteStop();
        }
        _this->connected = false;
        _this->setStatus("Stopped");
        flog::info("UberSDRSourceModule '{0}': Stop!", _this->name);
    }

    static void tune(double freq, void* ctx) {
        UberSDRSourceModule* _this = (UberSDRSourceModule*)ctx;
        _this->lastTune = freq;
        _this->sendPrimaryTune(false);
    }

    // Network thread: one decoded IQ packet into the link.
    void onIQ(const ubersdr::UberSDRClient::PacketInfo& info, const dsp::complex_t* samples, int count) {
        if (info.sampleRate > 0) { headerRate = info.sampleRate; }
        basebandPower = info.basebandPower;
        const uint64_t first = linkIndexBase.load() + samplesPushed;
        recordTime(info.timestampNanos, first);
        echoProgress(first, count);
        for (int done = 0; done < count;) {
            const int n = std::min(count - done, rawStream.capacity());
            memcpy(rawStream.writeBuf, samples + done, (size_t)n * sizeof(dsp::complex_t));
            if (!rawStream.swap(n)) { return; }
            samplesPushed += (uint64_t)n;
            done += n;
        }
        linkGuard(count);
    }

    // Network thread: compare the arrival rate with the header rate.
    void linkGuard(int count) {
        const auto now = std::chrono::steady_clock::now();
        if (guardStart == std::chrono::steady_clock::time_point{}) {
            guardStart = now;
            guardSamples = 0;
            return;
        }
        guardSamples += (uint64_t)count;
        const auto elapsed = now - guardStart;
        if (elapsed < kGuardWindow) { return; }
        const double seconds = std::chrono::duration<double>(elapsed).count();
        const double ratio = headerRate.load() > 0 ? (double)guardSamples / seconds / (double)headerRate.load() : 1.0;
        arrivalRatio = (float)ratio;
        slowWindows = (ratio < kGuardThreshold) ? slowWindows + 1 : 0;
        if (slowWindows >= 2) { linkSlow = true; }
        guardStart = now;
        guardSamples = 0;
    }

    void applyBufferMode(bool resetBuffer) {
        std::lock_guard lck(bufferModeMtx);
        applyBufferModeLocked(resetBuffer);
    }

    // As in kiwisdr_source: a live switch between prebuffer and bypass
    // re-plumbs the link while the network thread may sit in swap().
    void applyBufferModeLocked(bool resetBuffer) {
        chain.setSampleRate(appliedRate);
        const bool live = running.load();
        const bool replumb = live && (bufferMs.load() > 0) != chain.prebuffering();
        if (replumb) { rawStream.stopWriter(); }
        chain.setPrebufferMsec(bufferMs.load(), resetBuffer, live);
        if (replumb) { rawStream.clearWriteStop(); }
    }

    void rememberServer(const std::string& url) {
        if (url.empty()) { return; }
        recent.erase(std::remove(recent.begin(), recent.end(), url), recent.end());
        recent.insert(recent.begin(), url);
        if (recent.size() > RECENT_MAX) { recent.resize(RECENT_MAX); }
        auto configAccess = config.edit();
        configAccess.set("url", url);
        configAccess.set("recent", recent);
        if (auto target = remote::parseTarget(normaliseBaseUrl(url), "https")) {
            target->path.clear();
            auto entry = remote::manualEntry(remote::RemoteKind::UberSDR, *target);
            entry.tlsRequired = target->scheme == "https";
            remote::recordRecent(entry);
        }
    }

    // Reconnect the primary at another width: the server replaces the old
    // socket with the new one.
    void switchMode(int id) {
        iqModeId = id;
        config.edit().set("iqMode", std::string(kIqModes[id].key));
        if (!running.load() || !connected.load()) { return; }
        setActiveMode(kIqModes[id].key);
        linkSlow = false;
        slowWindows = 0;
        guardStart = {};
        {
            std::lock_guard lck(pendingMtx);
            pendingTunes.clear();
        }
        const double w = providerRegistered ? sigpath::vfoManager.committedIQWindowOffset() : 0.0;
        double freq;
        {
            std::lock_guard tl(tuneMtx);
            intendedW = w;
            sentTuneFreq = std::round(lastTune + hardwareOffset(w));
            sentTuneValid = true;
            freq = sentTuneFreq;
        }
        client.start(baseUrl, kIqModes[id].key, freq, passwordBuf);
    }

    static void menuHandler(void* ctx) {
        UberSDRSourceModule* _this = (UberSDRSourceModule*)ctx;
        const bool playing = _this->running.load();

        // The shared remote browser, UberSDR only. Its Connect stops the
        // radio first, so it stays enabled while playing.
        if (ImGui::Button(("Browse...##_ubersdr_browse_" + _this->name).c_str())) {
            remote::openBrowser(remote::RemoteKind::UberSDR);
        }
        ImGui::BeginDisabled(playing);
        ImGui::LeftLabel("Server");
        ImGui::FillWidth();
        if (ImGui::InputText(("##_ubersdr_url_" + _this->name).c_str(), _this->urlBuf, sizeof _this->urlBuf)) {
            config.edit().set("url", std::string(_this->urlBuf));
        }
        if (!_this->recent.empty()) {
            ImGui::SetNextItemWidth(-1);
            if (ImGui::BeginCombo(("##_ubersdr_recent_" + _this->name).c_str(), "Recent...")) {
                for (const auto& r : _this->recent) {
                    if (ImGui::Selectable(r.c_str(), false)) {
                        std::snprintf(_this->urlBuf, sizeof _this->urlBuf, "%s", r.c_str());
                        config.edit().set("url", r);
                    }
                }
                ImGui::EndCombo();
            }
        }
        ImGui::LeftLabel("Password");
        ImGui::FillWidth();
        if (ImGui::InputText(("##_ubersdr_password_" + _this->name).c_str(), _this->passwordBuf, sizeof _this->passwordBuf,
                             ImGuiInputTextFlags_Password)) {
            config.edit().set("password", std::string(_this->passwordBuf));
        }
        if (ImGui::IsItemHovered()) { ImGui::SetTooltip("Stored in the module config as plain text"); }
        ImGui::EndDisabled();

        // IQ width: the admitted modes once known, all of them before.
        std::vector<std::string> allowed;
        {
            std::lock_guard lck(_this->admissionMtx);
            allowed = _this->admission.iqModes;
        }
        ImGui::LeftLabel("IQ width");
        ImGui::FillWidth();
        if (ImGui::BeginCombo(("##_ubersdr_iqmode_" + _this->name).c_str(), kIqModes[_this->iqModeId].label)) {
            for (int i = 0; i < kIqModeCount; i++) {
                const bool ok = allowed.empty() || std::find(allowed.begin(), allowed.end(), kIqModes[i].key) != allowed.end();
                if (ImGui::Selectable(kIqModes[i].label, i == _this->iqModeId, ok ? 0 : ImGuiSelectableFlags_Disabled)) {
                    _this->switchMode(i);
                }
            }
            ImGui::EndCombo();
        }

        ImGui::BeginDisabled(playing);
        if (ImGui::Checkbox(("Server waterfall##_ubersdr_wf_" + _this->name).c_str(), &_this->serverWaterfall)) {
            config.edit().set("serverWaterfall", _this->serverWaterfall);
        }
        ImGui::EndDisabled();

        int ms = _this->bufferMs.load();
        ImGui::LeftLabel("Buffer (ms)");
        ImGui::FillWidth();
        if (ImGui::SliderInt(("##_ubersdr_buffer_" + _this->name).c_str(), &ms, BUFFER_MS_MIN, BUFFER_MS_MAX)) {
            _this->bufferMs = ms;
            _this->applyBufferMode(true);
            config.edit().set("bufferMs", ms);
        }

        ImGui::TextWrapped("Status: %s", _this->getStatus().c_str());
        if (playing && _this->connected.load()) {
            ubersdr::UberSDRClient::Admission a;
            std::chrono::steady_clock::time_point at;
            {
                std::lock_guard lck(_this->admissionMtx);
                a = _this->admission;
                at = _this->admittedAt;
            }
            ImGui::Text("%s at %d S/s, arrival %.0f %%, power %.1f dBFS", _this->getActiveMode().c_str(), _this->headerRate.load(),
                        100.0f * _this->arrivalRatio.load(), _this->basebandPower.load());
            if (a.maxSessionS > 0) {
                const int left = a.maxSessionS - (int)std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - at).count();
                ImGui::Text("Session time left: %d min", std::max(0, left) / 60);
            }
            for (const auto& sec : _this->secondaries) {
                ImGui::Text("%s: own 12 kHz stream (VFO %d)", sec->vfo.c_str(), sec->slot);
            }
            for (const auto& [slot, reason] : _this->slotRefusals) {
                ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.2f, 1.0f), "VFO %d refused: %s", slot, reason.c_str());
            }
            if (_this->linkSlow.load() && _this->iqModeId > 0) {
                ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.2f, 1.0f), "Link too slow for %s", kIqModes[_this->iqModeId].label);
                if (ImGui::Button(("Use " + std::string(kIqModes[_this->iqModeId - 1].label) + "##_ubersdr_down").c_str())) {
                    _this->switchMode(_this->iqModeId - 1);
                }
            }
        }
    }

    std::string name;
    bool enabled = true;

    ubersdr::UberSDRClient client;
    ubersdr::UberSpectrumClient spectrum;
    std::thread worker;          // admission, then client.start()
    std::string baseUrl;         // set before the worker starts
    std::string activeMode = "iq48";   // statusMtx

    char urlBuf[512] = "";
    char passwordBuf[256] = "";
    int iqModeId = 0;
    bool serverWaterfall = true; // UberSDR 3
    std::vector<std::string> recent;

    std::atomic<bool> running{false};
    std::atomic<bool> connected{false};
    std::atomic<int> bufferMs{150};
    std::atomic<int> headerRate{0};
    std::atomic<float> basebandPower{-999.0f};
    std::atomic<float> arrivalRatio{1.0f};
    std::atomic<bool> linkSlow{false};
    double lastTune = 7100000.0;
    int appliedRate = 48000;     // UI thread

    std::mutex statusMtx;
    std::string status = "Not connected";
    std::mutex admissionMtx;
    ubersdr::UberSDRClient::Admission admission;
    std::chrono::steady_clock::time_point admittedAt{};

    // Network thread.
    uint64_t samplesPushed = 0;
    std::chrono::steady_clock::time_point guardStart{};
    uint64_t guardSamples = 0;
    int slowWindows = 0;

    dsp::stream<dsp::complex_t> rawStream;
    dsp::stream<dsp::complex_t> stream;
    dsp::buffer::PrebufferedLink<dsp::complex_t> chain;
    std::mutex bufferModeMtx;
    SourceManager::SourceHandler handler;

    // Server waterfall, UI thread.
    Provider provider;
    // Written on the UI thread; the tune path reads it on any thread.
    std::atomic<bool> providerRegistered{false};
    bool fellBack = false;       // core fell back this session
    std::vector<double> zoomLadder;
    double sentBinBandwidth = 0.0;
    double sentCentre = 0.0;
    double appliedSpan = -1.0;
    Clock::time_point lastViewSend{};
    Clock::time_point registeredAt{};
    // Written by one thread, read by another.
    std::atomic<double> wantBinBandwidth{0.0};
    std::atomic<double> configBinBandwidth{0.0};
    std::atomic<double> configSpan{0.0};
    std::atomic<double> defaultSpan{0.0};
    std::atomic<int64_t> rateLimitedTicks{0};
    std::atomic<int64_t> lastFrameTicks{0};
    std::atomic<bool> frameSink{false};
    std::atomic<double> uiTuningOffset{0.0};
    std::atomic<double> uiCentre{0.0};
    std::atomic<bool> uiInvertIQ{false};
    std::atomic<uint64_t> linkIndexBase{0};
    // IQ packet timestamps against link indices.
    std::mutex timeMtx;
    std::array<TimeMark, 512> timeRing{};
    size_t timeHead = 0;
    size_t timeCount = 0;
    // Spectrum network thread.
    SpectrumFrame specFrame;

    // IQ window. UI thread, and tuneMtx for what sendPrimaryTune() touches.
    std::mutex tuneMtx;
    double intendedW = 0.0;
    double placedWidth = -1.0;
    double sentTuneFreq = 0.0;
    bool sentTuneValid = false;
    Clock::time_point lastTuneSend{};
    // Markers commit only while the window is set.
    std::atomic<bool> windowLive{false};
    // Tunes sent and not yet echoed.
    std::mutex pendingMtx;
    std::deque<PendingTune> pendingTunes;
    // IQ network thread: packets since the echo (-1: none in progress).
    int packetsSinceEcho = -1;
    double echoW = 0.0;
    double lastEchoW = 0.0;

    // Secondary VFOs, UI thread.
    std::vector<std::unique_ptr<Secondary>> secondaries;
    std::map<int, std::string> slotRefusals;  // vfo=N -> the server's reason
    EventHandler<VFOManager::VFO*> vfoDeleteHandler;
};

MOD_EXPORT void _INIT_() {
    json def = json({});
    def["url"] = "";
    def["password"] = "";
    def["iqMode"] = "iq48";
    def["bufferMs"] = 150;
    def["serverWaterfall"] = true;
    def["recent"] = json::array();
    config.setPath(core::args["root"].s() + "/ubersdr_source_config.json");
    config.load(def);
    config.enableAutoSave();
}

MOD_EXPORT ModuleManager::Instance* _CREATE_INSTANCE_(std::string name) {
    return new UberSDRSourceModule(name);
}

MOD_EXPORT void _DELETE_INSTANCE_(ModuleManager::Instance* instance) {
    delete (UberSDRSourceModule*)instance;
}

MOD_EXPORT void _END_() {
    config.shutdown();
}
