// OpenWebRX+ source: 48 kS/s IQ from an OpenWebRX+ receiver (1.2.129 and
// later, the "iq" mode), with the profile's FFT as the waterfall. Protocol
// facts: doc/research/openwebrx.md. The plan this follows:
// doc/todo/network-sources/openwebrx/.
//
// The receiver's band is its current profile: centre C and span S, fixed by
// the server and shared by every listener on that SDR. Within it the client
// moves only its demodulator offset W, over which the 48 kHz IQ is centred.
// Two ways to show it:
//   * server waterfall (SpectrumProvider registered): the display is the
//     whole profile around C, and the IQ is a 36 kHz window at C + W placed
//     over the VFOs (iq_window_policy.h);
//   * local FFT: the display is the IQ itself, centred at C + W, and tuning
//     moves W within the profile.
#include <imgui.h>
#include <module.h>
#include <config.h>
#include <core.h>
#include <gui/gui.h>
#include <gui/style.h>
#include <gui/widgets/popup_dialog.h>
#include <signal_path/signal_path.h>
#include <dsp/buffer/prebuffer.h>
#include <signal_path/spectrum_provider.h>
#include <signal_path/iq_window_policy.h>
#include <utils/flog.h>
#include <gui/remote/browser.h>
#include <gui/remote/recent.h>

#include "owrx_client.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

SDRPP_MOD_INFO{
    /* Name:            */ "openwebrx_source",
    /* Description:     */ "OpenWebRX+ source module for SDRIAK",
    /* Author:          */ "SDRIAK",
    /* Version:         */ 0, 1, 0,
    /* Max instances    */ 1
};

ConfigManager config;

class OpenWebRxSourceModule : public ModuleManager::Instance {
public:
    using Client = owrx::OpenWebRxClient;
    using Clock = std::chrono::steady_clock;

    static constexpr int BUFFER_MS_MIN = 0;
    static constexpr int BUFFER_MS_MAX = 1000;
    static constexpr size_t RECENT_MAX = 8;
    static constexpr int kRate = Client::kIqRate;
    // Link-rate guard: the arrival rate over each window against 48 kS/s;
    // two windows below the threshold flag the link as slow.
    static constexpr auto kGuardWindow = std::chrono::seconds(5);
    static constexpr double kGuardThreshold = 0.98;
    // The bot ban scores profile changes less than 10 s apart; one change
    // per 15 s never scores.
    static constexpr auto kProfileInterval = std::chrono::seconds(15);
    // Modes follow the config, 3.8 s later on one receiver; how long to
    // wait for them.
    static constexpr auto kModesWait = std::chrono::seconds(10);

    OpenWebRxSourceModule(std::string name) {
        this->name = name;
        // Not served headless: a client should reach the receiver directly.
        if (core::args["server"].b()) { return; }

        {
            auto configAccess = config.read();
            std::string url;
            if (configAccess.tryGet("url", url)) { std::snprintf(urlBuf, sizeof urlBuf, "%s", url.c_str()); }
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
        chain.setSampleRate(kRate);
        chain.setPrebufferMsec(bufferMs.load(), false, false);

        client.onHandshake = [this] {
            connected = true;
            connectedAt = Clock::now().time_since_epoch().count();
            setStatus("Connected, waiting for the receiver's configuration");
        };
        client.onConfig = [this](const Client::Config& c, bool changed) {
            if (!configComplete.load()) { configAt = Clock::now().time_since_epoch().count(); }
            configComplete = true;
            if (changed) { profileChanged = true; }
            (void)c;
        };
        client.onIQ = [this](const dsp::complex_t* samples, int count) { onIQ(samples, count); };
        client.onFftRow = [this](const float* bins, int count) { onFftRow(bins, count); };
        client.onError = [this](const std::string& e) {
            setError("Error: " + e);
            sessionEnded = true;
        };
        client.onDisconnected = [this] {
            connected = false;
            if (running.load()) {
                setError("Disconnected by the server");
                sessionEnded = true;
            }
        };
        provider.module = this;

        handler.ctx = this;
        handler.selectHandler = menuSelected;
        handler.deselectHandler = menuDeselected;
        handler.menuHandler = menuHandler;
        handler.startHandler = start;
        handler.stopHandler = stop;
        handler.tuneHandler = tune;
        handler.uiTickHandler = uiTick;
        handler.stream = &stream;
        sigpath::sourceManager.registerSource("OpenWebRX+", &handler);

        remote::RemoteHandler remoteHandler;
        remoteHandler.kind = remote::RemoteKind::OpenWebRX;
        remoteHandler.sourceName = "OpenWebRX+";
        remoteHandler.probe = probe;
        remoteHandler.connect = [this](const remote::RemoteEntry& entry) { connectRemote(entry); };
        sigpath::remoteRegistry.registerHandler(remoteHandler);
    }

    ~OpenWebRxSourceModule() {
        if (core::args["server"].b()) { return; }
        sigpath::remoteRegistry.unregisterHandler(remote::RemoteKind::OpenWebRX);
        stop(this);
        sigpath::sourceManager.unregisterSource("OpenWebRX+");
    }

    void postInit() {}
    void enable() { enabled = true; }
    void disable() { enabled = false; }
    bool isEnabled() { return enabled; }

private:
    // The remote browser's Test: request IQ and wait for decoded samples.
    static remote::ProbeResult probe(const remote::RemoteEntry& entry, const std::atomic<bool>& cancel) {
        using Clock = std::chrono::steady_clock;
        remote::ProbeResult result;
        owrx::OpenWebRxClient client;
        std::atomic<bool> handshake{ false };
        std::atomic<bool> configured{ false };
        std::atomic<bool> closed{ false };
        std::atomic<bool> gotIq{ false };
        std::atomic<bool> requested{ false };
        std::atomic<int> handshakeMs{ -1 };
        std::mutex errMtx;
        std::string error;
        const auto t0 = Clock::now();
        client.onHandshake = [&] {
            handshake = true;
            handshakeMs = (int)std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count();
        };
        client.onConfig = [&](const owrx::OpenWebRxClient::Config& c, bool) {
            if (c.complete()) { configured = true; }
        };
        client.onError = [&](const std::string& e) {
            std::lock_guard lck(errMtx);
            if (error.empty()) { error = e; }
        };
        client.onDisconnected = [&] { closed = true; };
        client.onIQ = [&](const dsp::complex_t*, int count) {
            if (requested && count > 0) { gotIq = true; }
        };
        client.start(entry.target.url());
        auto configSeenAt = Clock::time_point{};
        while (!cancel && !closed && !gotIq && Clock::now() - t0 < std::chrono::seconds(6)) {
            if (configured) {
                const auto now = Clock::now();
                if (configSeenAt == Clock::time_point{}) { configSeenAt = now; }
                if (client.hasModeList() && !client.hasIqMode()) { break; }
                if (!requested && (client.hasIqMode() || now - configSeenAt >= std::chrono::seconds(1))) {
                    requested = true;
                    client.startIq(0);
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        const std::string version = client.serverVersion();
        const bool excluded = client.hasModeList() && !client.hasIqMode();
        const auto cfg = client.config();
        const size_t profiles = client.profiles().size();
        const int clients = client.clients();
        client.stop();

        if (handshakeMs >= 0) { result.rttMs = handshakeMs.load(); }
        if (!version.empty()) { result.negotiated.emplace_back("Version", version); }
        if (cfg.complete()) {
            result.negotiated.emplace_back("Profile", remote::formatRangeMHz((uint64_t)std::max(0.0, cfg.centerFreq - cfg.sampRate / 2.0),
                                                                             (uint64_t)(cfg.centerFreq + cfg.sampRate / 2.0)));
        }
        if (profiles) { result.negotiated.emplace_back("Profiles", std::to_string(profiles)); }
        if (clients > 0) { result.negotiated.emplace_back("Clients", std::to_string(clients)); }
        if (cancel) { result.reason = "cancelled"; }
        else if (excluded) { result.reason = "the receiver's mode list does not include IQ"; }
        else if (gotIq) { result.ok = true; result.negotiated.emplace_back("IQ rate", std::to_string(kRate) + " S/s"); }
        else {
            std::lock_guard lck(errMtx);
            if (!error.empty()) { result.reason = error; }
            else if (requested) { result.reason = "no decoded IQ received after requesting IQ"; }
            else { result.reason = handshake ? "handshake, but no config within 6 s" : "could not connect"; }
        }
        return result;
    }

    // The remote browser's Connect: the browser has selected this source.
    void connectRemote(const remote::RemoteEntry& entry) {
        const std::string url = entry.target.url();
        std::snprintf(urlBuf, sizeof urlBuf, "%s", url.c_str());
        config.edit().set("url", url);
        gui::mainWindow.setPlayState(true);
    }

    void setStatus(const std::string& s) {
        std::lock_guard lck(statusMtx);
        if (!statusSticky) { status = s; }
    }
    // A status that stop() keeps.
    void setError(const std::string& s) {
        std::lock_guard lck(statusMtx);
        if (statusSticky) { return; }
        status = s;
        statusSticky = true;
    }
    std::string getStatus() {
        std::lock_guard lck(statusMtx);
        return status;
    }

    static Clock::time_point fromTicks(int64_t t) { return Clock::time_point(Clock::duration(t)); }

    static void menuSelected(void* ctx) {
        OpenWebRxSourceModule* _this = (OpenWebRxSourceModule*)ctx;
        core::setInputSampleRate(kRate);
        _this->selected = true;
        if (_this->profileApplied) { _this->applyTuningLimits(); }
        flog::info("OpenWebRxSourceModule '{0}': Menu Select!", _this->name);
    }

    static void menuDeselected(void* ctx) {
        OpenWebRxSourceModule* _this = (OpenWebRxSourceModule*)ctx;
        _this->selected = false;
        _this->unregisterProvider();
        sigpath::sourceManager.clearTuningLimits();
        gui::waterfall.centerFrequencyLocked = false;
        flog::info("OpenWebRxSourceModule '{0}': Menu Deselect!", _this->name);
    }

    // The profile ---------------------------------------------------------------

    double profileCentre() const { return profile.centerFreq; }
    double profileSpan() const { return profile.sampRate; }
    // Largest |W| that keeps the 48 kHz IQ inside the profile.
    double maxOffset() const { return std::max(0.0, profileSpan() / 2.0 - (double)Client::kIqCut); }

    void applyTuningLimits() {
        sigpath::sourceManager.setTuningLimits(profileCentre() - profileSpan() / 2.0, profileCentre() + profileSpan() / 2.0);
    }

    // UI thread: the first complete config of a session, or a profile change.
    void applyProfile(bool first) {
        profile = client.config();
        profileApplied = true;
        profileSpanNet = profileSpan();
        applyTuningLimits();
        uiDisplayCentre = profileCentre() - sigpath::sourceManager.getTuningOffset();
        ++moveGeneration;
        {
            std::lock_guard lck(movesMtx);
            moves.clear();
        }
        // The first IQ goes where the last tune pointed, when that is inside
        // the profile; a profile change restarts at W = 0.
        double w = 0.0;
        if (first && !providerRegistered) {
            const double want = requestedHw.load() - profileCentre();
            if (std::fabs(want) <= profileSpan() / 2.0) { w = std::clamp(want, -maxOffset(), maxOffset()); }
        }
        sentOffsetHw = w;
        intendedOffsetHw = w;
        intendedW = 0.0;
        if (providerRegistered) {
            core::setDisplaySpan(profileSpan());
            sigpath::vfoManager.setIQWindow(0.0, kWindowWidth);
            sigpath::vfoManager.commitIQWindowOffset(0.0);
            lastFrameTicks = Clock::now().time_since_epoch().count();
        }
        if (!first) {
            // The band moved under the VFOs: keep their offsets, clamp them,
            // and start the waterfall afresh.
            moveCentre(expectedCentreHw(), false);
            gui::waterfall.resetHistory();
            sigpath::vfoManager.muteAllFor(kRate / 4);
        }
        lastOffsetSend = Clock::now();
        if (first) { iqRequestedAt = Clock::now(); }
        iqRequested = true;
        client.startIq(sentOffsetHw);
        iqStarted = true;
    }

    // Hardware Hz the waterfall centre stands for. Local FFT: where the IQ
    // is headed; the mute covers the IQ until it gets there.
    double expectedCentreHw() const { return providerRegistered ? profileCentre() : profileCentre() + intendedOffsetHw; }

    // UI thread: put the waterfall centre at hw. keepAbsolute keeps each
    // VFO on its frequency (a correction of a tune the source refused);
    // otherwise VFOs keep their offsets (the band itself moved).
    void moveCentre(double hw, bool keepAbsolute) {
        const double display = hw - sigpath::sourceManager.getTuningOffset();
        const double old = gui::waterfall.getCenterFrequency();
        if (std::fabs(display - old) < 0.5) { return; }
        // A jump past the span (the first profile of a session) keeps the
        // offsets: absolute frequencies would all clamp to one edge.
        if (std::fabs(display - old) > gui::waterfall.getBandwidth()) { keepAbsolute = false; }
        std::vector<std::pair<std::string, double>> vfos;
        if (keepAbsolute) {
            for (auto const& [vfoName, v] : gui::waterfall.vfos) { vfos.emplace_back(vfoName, old + v->centerOffset); }
        }
        gui::waterfall.setCenterFrequency(display);
        for (const auto& [vfoName, f] : vfos) { sigpath::vfoManager.setCenterOffset(vfoName, f - display); }
        gui::mainWindow.clampVFOsToSpan();
        // Tells the frequency listeners (rigctl, bookmarks); comes back to
        // tune() as the centre the source already has.
        sigpath::sourceManager.tune(display);
    }

    // The profile combo asks before changing the band for everyone.
    void requestProfile(const std::string& key) {
        const auto now = Clock::now();
        if (lastProfileChange != Clock::time_point{} && now - lastProfileChange < kProfileInterval) {
            const int wait = (int)std::chrono::duration_cast<std::chrono::seconds>(kProfileInterval - (now - lastProfileChange)).count() + 1;
            profileNotice = "Wait " + std::to_string(wait) + " s before the next profile change";
            return;
        }
        profileNotice.clear();
        pendingProfile = key;
        profileDialog.request();
    }

    // UI thread ------------------------------------------------------------------

    static void uiTick(void* ctx) {
        OpenWebRxSourceModule* _this = (OpenWebRxSourceModule*)ctx;
        _this->tick();
    }

    void tick() {
        uiInvertIQ = sigpath::iqFrontEnd.getInvertIQ();
        if (sessionEnded.exchange(false) && running.load()) {
            gui::mainWindow.setPlayState(false);
            return;
        }
        if (running && !configComplete.load() && Clock::now() - openedAt > std::chrono::seconds(15)) {
            setError("Timed out waiting for the receiver's configuration");
            gui::mainWindow.setPlayState(false);
            return;
        }
        if (!running.load() || !connected.load()) {
            unregisterProvider();
            return;
        }
        if (client.hasModeList() && !client.hasIqMode()) {
            setError("Refused: this receiver does not offer the IQ mode");
            gui::mainWindow.setPlayState(false);
            return;
        }
        if (!iqStarted) {
            if (!configComplete.load()) { return; }
            if (!client.hasModeList() && Clock::now() - fromTicks(configAt.load()) < kModesWait) { return; }
            applyProfile(true);
            setStatus("Waiting for decoded IQ");
        }
        if (!gotIq.load()) {
            if (Clock::now() - iqRequestedAt > std::chrono::seconds(6)) {
                setError("No decoded IQ received after requesting IQ");
                gui::mainWindow.setPlayState(false);
            }
            return;
        }
        if (!iqConfirmed) {
            iqConfirmed = true;
            setStatus(profile.audioCompression == "adpcm" ? "Streaming IQ (4-bit ADPCM)" : "Streaming IQ");
        }
        if (profileChanged.exchange(false)) {
            applyProfile(false);
            const double c = profileCentre();
            setStatus("The band changed to " + std::to_string((int64_t)std::llround(c / 1e3)) + " kHz (profile " + profile.profileId + ")");
        }
        uiDisplayCentre = profileCentre() - sigpath::sourceManager.getTuningOffset();
        serverWaterfallTick();
        if (providerRegistered) { placeIQWindow(); }
        else { followTune(); }
        sendOffset();
        // A tune the source could not follow leaves the waterfall elsewhere.
        moveCentre(expectedCentreHw(), true);
    }

    // Local FFT: tuning moves W within the profile.
    void followTune() {
        const double want = requestedHw.load() - profileCentre();
        const double w = std::clamp(want, -maxOffset(), maxOffset());
        if (std::fabs(want) > profileSpan() / 2.0) { tuneNoticeUntil = Clock::now() + std::chrono::seconds(3); }
        if (std::fabs(w - intendedOffsetHw) < 0.5) { return; }
        intendedOffsetHw = w;
        muteUntilSwitch(kMoveInterval);
    }

    // Local FFT: the display moves at once, while the prebuffer still holds
    // IQ from the old centre. Mute it and the IQ arriving until the switch,
    // `wait` (the send still due) included.
    void muteUntilSwitch(Clock::duration wait) {
        const double s = std::chrono::duration<double>(wait + kMuteTo).count();
        sigpath::vfoManager.muteAllFor((int)chain.bufferedSamples() + (int)(kRate * s));
    }

    // Send W when it moved, at most 4 per second.
    void sendOffset() {
        const double target = providerRegistered ? hardwareOffset(intendedW) : intendedOffsetHw;
        if (std::fabs(target - sentOffsetHw) < 0.5) { return; }
        const auto now = Clock::now();
        if (now - lastOffsetSend < kMoveInterval) { return; }
        if (providerRegistered) {
            std::lock_guard lck(movesMtx);
            moves.push_back(Move{ intendedW, now, false });
            while (moves.size() > kMaxMoves) { moves.pop_front(); }
        }
        else {
            muteUntilSwitch(Clock::duration::zero());
        }
        client.setOffset(target);
        sentOffsetHw = target;
        lastOffsetSend = now;
    }

    // Server waterfall ------------------------------------------------------------

    struct Provider : public SpectrumProvider {
        OpenWebRxSourceModule* module = nullptr;
        std::string name() const override { return "OpenWebRX+"; }
        int binCount() const override { return std::clamp(module->profile.fftSize, 256, 8192); }
        // One span per profile: core's view zoom magnifies the frame.
        std::vector<double> zoomSpans() const override { return { module->profileSpan() }; }
        void requestSpan(double) override {}
        bool available() const override { return module->spectrumAlive(); }
        std::string unavailableReason() const override { return "no spectrum rows from the receiver"; }
        // Log power plus the server's fixed -70 dB; the profile's own levels.
        std::pair<float, float> defaultLevels() const override {
            if (module->profile.haveLevels) { return { module->profile.levelMin, module->profile.levelMax }; }
            return { -88.0f, -20.0f };
        }
    };

    static constexpr auto kFrameTimeout = std::chrono::seconds(3);

    bool spectrumAlive() const {
        if (!running.load() || !connected.load()) { return false; }
        const auto now = Clock::now();
        return now - fromTicks(lastFrameTicks.load()) < kFrameTimeout;
    }

    void serverWaterfallTick() {
        // Core takes the provider away when it reports unavailable, and has
        // cleared the IQ window and display span by then. It is not
        // registered again until the next session.
        if (providerRegistered && sigpath::iqFrontEnd.spectrumProvider() != &provider) {
            dropWindow();
            fellBack = true;
        }
        if (!serverWaterfall) {
            unregisterProvider();
            return;
        }
        if (providerRegistered || fellBack || sigpath::iqFrontEnd.spectrumProvider() != nullptr) { return; }
        // W stays where the IQ is: the window starts over the current IQ.
        intendedW = displayOffset(sentOffsetHw);
        lastFrameTicks = Clock::now().time_since_epoch().count();
        sigpath::iqFrontEnd.setSpectrumProvider(&provider);
        providerRegistered = true;
        frameSink = true;
        core::setDisplaySpan(profileSpan());
        sigpath::vfoManager.setIQWindow(intendedW, kWindowWidth);
        sigpath::vfoManager.commitIQWindowOffset(intendedW);
        windowLive = true;
        gui::waterfall.centerFrequencyLocked = true;
        // The display is the whole profile from here on.
        moveCentre(profileCentre(), true);
    }

    // UI thread: back to the local FFT, the display centred on the IQ.
    void dropWindow() {
        providerRegistered = false;
        frameSink = false;
        windowLive = false;
        ++moveGeneration;
        {
            std::lock_guard lck(movesMtx);
            moves.clear();
        }
        // The IQ stays where it is: the local display follows it.
        intendedOffsetHw = sentOffsetHw;
        requestedHw = profileCentre() + sentOffsetHw;
        gui::waterfall.centerFrequencyLocked = false;
    }

    void unregisterProvider() {
        if (!providerRegistered) { return; }
        const bool ours = sigpath::iqFrontEnd.spectrumProvider() == &provider;
        dropWindow();
        if (ours) { sigpath::iqFrontEnd.setSpectrumProvider(nullptr); }
        core::clearDisplaySpan();
        sigpath::vfoManager.clearIQWindow();
        if (running.load() && connected.load()) { moveCentre(expectedCentreHw(), true); }
    }

    // Network thread: one FFT row, already in display order.
    void onFftRow(const float* bins, int count) {
        lastFrameTicks = Clock::now().time_since_epoch().count();
        if (!frameSink.load() || count <= 0) { return; }
        const double span = profileSpanNet.load();
        if (span <= 0.0) { return; }
        specFrame.bins.assign(bins, bins + count);
        if (uiInvertIQ.load()) { std::reverse(specFrame.bins.begin(), specFrame.bins.end()); }
        specFrame.startHz = uiDisplayCentre.load() - span / 2.0;
        specFrame.spanHz = span;
        // No frame timestamps: present at arrival plus the link's delay, so
        // the row lines up with the IQ that arrived with it.
        specFrame.presentAt = chain.playoutTime(linkIndexBase.load() + samplesPushed.load());
        sigpath::iqFrontEnd.pushSpectrum(std::move(specFrame));
    }

    // IQ window ---------------------------------------------------------------------
    //
    // W (display Hz) moves with a dspcontrol offset_freq, which the server
    // does not answer. The switch lands in a message arriving 167-246 ms
    // after the command (openwebrx.md section 5): W is committed at the first
    // message arriving kCommitAfter after the send, and every non-routed VFO
    // is muted across the messages arriving between kMuteFrom and kMuteTo.

    static constexpr double kWindowWidth = 36000.0;  // flat to +-16 kHz, -1 to -2 dB at +-20 kHz
    static constexpr auto kMoveInterval = std::chrono::milliseconds(250);
    static constexpr auto kCommitAfter = std::chrono::milliseconds(200);
    static constexpr auto kMuteFrom = std::chrono::milliseconds(120);
    static constexpr auto kMuteTo = std::chrono::milliseconds(320);
    static constexpr size_t kMaxMoves = 16;

    struct Move {
        double w = 0.0;  // display Hz
        Clock::time_point sentAt{};
        bool committed = false;
    };

    double hardwareOffset(double w) const { return uiInvertIQ.load() ? -w : w; }
    double displayOffset(double hw) const { return uiInvertIQ.load() ? -hw : hw; }

    void placeIQWindow() {
        IQWindowInput in;
        const std::string& selectedVfo = gui::waterfall.selectedVFO;
        auto add = [&in](const std::string& vfoName, const ImGui::WaterfallVFO* v) {
            if (!sigpath::vfoManager.isVFORouted(vfoName)) { in.passbands.emplace_back(v->lowerOffset, v->upperOffset); }
        };
        if (auto sel = gui::waterfall.vfos.find(selectedVfo); sel != gui::waterfall.vfos.end()) { add(sel->first, sel->second); }
        for (auto const& [vfoName, v] : gui::waterfall.vfos) {
            if (vfoName != selectedVfo) { add(vfoName, v); }
        }
        in.currentOffset = intendedW;
        in.width = kWindowWidth;
        in.margin = 0.1 * kWindowWidth;
        const double lim = std::max(0.0, profileSpan() / 2.0 - kWindowWidth / 2.0);
        in.minOffset = -lim;
        in.maxOffset = lim;
        const double w = chooseIQWindow(in);
        if (std::fabs(w - intendedW) >= 1.0) {
            intendedW = w;
            sigpath::vfoManager.setIQWindow(w, kWindowWidth);
        }
    }

    // Network thread, before a message enters the link.
    void moveProgress(const Clock::time_point now, uint64_t first, int count) {
        bool mute = false;
        std::vector<double> commits;
        {
            std::lock_guard lck(movesMtx);
            for (auto& m : moves) {
                const auto age = now - m.sentAt;
                if (age >= kMuteFrom && age <= kMuteTo) { mute = true; }
                if (!m.committed && age >= kCommitAfter) {
                    commits.push_back(m.w);
                    m.committed = true;
                }
            }
            while (!moves.empty() && now - moves.front().sentAt > kMuteTo && moves.front().committed) { moves.pop_front(); }
        }
        const uint64_t gen = moveGeneration.load();
        if (mute) {
            const int n = count + kRate / 100;
            chain.scheduleMarker(first, [n] { sigpath::vfoManager.muteAllFor(n); });
        }
        for (double w : commits) {
            chain.scheduleMarker(first, [this, w, gen] {
                if (windowLive.load() && moveGeneration.load() == gen) { sigpath::vfoManager.commitIQWindowOffset(w); }
            });
        }
    }

    // Session ---------------------------------------------------------------------

    static void start(void* ctx) {
        OpenWebRxSourceModule* _this = (OpenWebRxSourceModule*)ctx;
        if (_this->running.exchange(true)) { return; }
        _this->sessionEnded = false;
        _this->iqRequested = false;
        _this->gotIq = false;
        _this->iqConfirmed = false;
        core::setInputSampleRate(kRate);
        {
            std::lock_guard lck(_this->statusMtx);
            _this->statusSticky = false;
        }
        _this->linkSlow = false;
        _this->slowWindows = 0;
        _this->guardStart = {};
        _this->configComplete = false;
        _this->profileChanged = false;
        _this->iqStarted = false;
        _this->fellBack = false;
        _this->profileApplied = false;
        _this->sentOffsetHw = 0.0;
        _this->intendedOffsetHw = 0.0;
        _this->intendedW = 0.0;
        _this->profileNotice.clear();
        _this->tuneNoticeUntil = {};
        {
            std::lock_guard lck(_this->bufferModeMtx);
            _this->rawStream.clearWriteStop();
            _this->chain.setPrebufferMsec(_this->bufferMs.load(), true, false);
            // A block left unread by the last session would shift this
            // session's input indices (PrebufferedLink::discardUnreadInput).
            _this->chain.discardUnreadInput();
            _this->linkIndexBase = _this->chain.samplesIn();
            _this->samplesPushed = 0;
            _this->chain.start();
        }
        {
            std::lock_guard lck(_this->movesMtx);
            _this->moves.clear();
        }
        _this->rememberServer(_this->urlBuf);
        _this->setStatus("Connecting...");
        _this->openedAt = Clock::now();
        _this->client.start(_this->urlBuf);
        flog::info("OpenWebRxSourceModule '{0}': Start!", _this->name);
    }

    static void stop(void* ctx) {
        OpenWebRxSourceModule* _this = (OpenWebRxSourceModule*)ctx;
        const bool wasRunning = _this->running.exchange(false);
        if (!wasRunning && !_this->client.running()) { return; }
        _this->frameSink = false;
        _this->windowLive = false;
        // Release a writer blocked in rawStream.swap() before joining the
        // network thread that is that writer.
        _this->rawStream.stopWriter();
        _this->client.stop();
        _this->unregisterProvider();
        {
            std::lock_guard lck(_this->bufferModeMtx);
            _this->chain.stop();
            _this->rawStream.clearWriteStop();
        }
        _this->connected = false;
        _this->iqStarted = false;
        {
            std::lock_guard lck(_this->statusMtx);
            if (!_this->statusSticky) { _this->status = "Stopped"; }
        }
        flog::info("OpenWebRxSourceModule '{0}': Stop!", _this->name);
    }

    // Any thread (rigctl tunes too): recorded, applied by the UI tick.
    static void tune(double freq, void* ctx) {
        OpenWebRxSourceModule* _this = (OpenWebRxSourceModule*)ctx;
        _this->requestedHw = freq;
    }

    // Network thread: one decoded IQ message into the link.
    void onIQ(const dsp::complex_t* samples, int count) {
        if (!iqRequested || count <= 0) { return; }
        gotIq = true;
        const uint64_t first = linkIndexBase.load() + samplesPushed.load();
        moveProgress(Clock::now(), first, count);
        for (int done = 0; done < count;) {
            const int n = std::min(count - done, rawStream.capacity());
            memcpy(rawStream.writeBuf, samples + done, (size_t)n * sizeof(dsp::complex_t));
            if (!rawStream.swap(n)) { return; }
            samplesPushed += (uint64_t)n;
            done += n;
        }
        linkGuard(count);
    }

    // Network thread: compare the arrival rate with 48 kS/s.
    void linkGuard(int count) {
        const auto now = Clock::now();
        if (guardStart == Clock::time_point{}) {
            guardStart = now;
            guardSamples = 0;
            return;
        }
        guardSamples += (uint64_t)count;
        const auto elapsed = now - guardStart;
        if (elapsed < kGuardWindow) { return; }
        const double seconds = std::chrono::duration<double>(elapsed).count();
        const double ratio = (double)guardSamples / seconds / (double)kRate;
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
        chain.setSampleRate(kRate);
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
        {
            auto configAccess = config.edit();
            configAccess.set("url", url);
            configAccess.set("recent", recent);
        }
        if (auto target = remote::parseTarget(url)) {
            auto entry = remote::manualEntry(remote::RemoteKind::OpenWebRX, *target);
            entry.tlsRequired = target->scheme == "https";
            remote::recordRecent(entry);
        }
    }

    static void menuHandler(void* ctx) {
        OpenWebRxSourceModule* _this = (OpenWebRxSourceModule*)ctx;
        const bool playing = _this->running.load();
        const ImVec4 warn(1.0f, 0.75f, 0.2f, 1.0f);

        // The shared remote browser, OpenWebRX+ only. Its Connect stops the
        // radio first, so it stays enabled while playing.
        if (ImGui::Button(("Browse...##_owrx_browse_" + _this->name).c_str())) {
            remote::openBrowser(remote::RemoteKind::OpenWebRX);
        }
        ImGui::BeginDisabled(playing);
        ImGui::LeftLabel("Receiver");
        ImGui::FillWidth();
        if (ImGui::InputText(("##_owrx_url_" + _this->name).c_str(), _this->urlBuf, sizeof _this->urlBuf)) {
            config.edit().set("url", std::string(_this->urlBuf));
        }
        if (ImGui::IsItemHovered()) { ImGui::SetTooltip("The receiver's page, e.g. http://host:8073/"); }
        if (!_this->recent.empty()) {
            ImGui::SetNextItemWidth(-1);
            if (ImGui::BeginCombo(("##_owrx_recent_" + _this->name).c_str(), "Recent...")) {
                for (const auto& r : _this->recent) {
                    if (ImGui::Selectable(r.c_str(), false)) {
                        std::snprintf(_this->urlBuf, sizeof _this->urlBuf, "%s", r.c_str());
                        config.edit().set("url", r);
                    }
                }
                ImGui::EndCombo();
            }
        }
        if (ImGui::Checkbox(("Server waterfall##_owrx_wf_" + _this->name).c_str(), &_this->serverWaterfall)) {
            config.edit().set("serverWaterfall", _this->serverWaterfall);
        }
        ImGui::EndDisabled();

        // Profiles: the receiver's bands, shared with every listener.
        const bool live = playing && _this->iqStarted;
        if (live) {
            const auto profiles = _this->client.profiles();
            const std::string current = _this->profile.profileKey();
            std::string currentName = current;
            for (const auto& p : profiles) {
                if (p.id == current) { currentName = p.name; }
            }
            ImGui::LeftLabel("Profile");
            ImGui::FillWidth();
            if (ImGui::BeginCombo(("##_owrx_profile_" + _this->name).c_str(), currentName.c_str())) {
                for (const auto& p : profiles) {
                    if (ImGui::Selectable(p.name.c_str(), p.id == current) && p.id != current) { _this->requestProfile(p.id); }
                }
                ImGui::EndCombo();
            }
            if (!_this->profileNotice.empty()) { ImGui::TextColored(warn, "%s", _this->profileNotice.c_str()); }
        }
        if (_this->profileDialog.begin(("Change profile##_owrx_profile_dlg_" + _this->name).c_str(), ImGuiWindowFlags_AlwaysAutoResize)) {
            const int others = std::max(0, _this->client.clients() - 1);
            ImGui::TextUnformatted("Changing the profile retunes the receiver's SDR");
            ImGui::Text("for every listener on it (%d other%s connected now).", others, others == 1 ? "" : "s");
            ImGui::TextUnformatted("Change it anyway?");
            if (_this->profileDialog.applyButton("Change")) {
                _this->client.selectProfile(_this->pendingProfile);
                _this->lastProfileChange = Clock::now();
                _this->profileDialog.close();
            }
            ImGui::SameLine();
            if (_this->profileDialog.cancelButton()) { _this->profileDialog.close(); }
            _this->profileDialog.end();
        }

        int ms = _this->bufferMs.load();
        ImGui::LeftLabel("Buffer (ms)");
        ImGui::FillWidth();
        if (ImGui::SliderInt(("##_owrx_buffer_" + _this->name).c_str(), &ms, BUFFER_MS_MIN, BUFFER_MS_MAX)) {
            _this->bufferMs = ms;
            _this->applyBufferMode(true);
            config.edit().set("bufferMs", ms);
        }

        ImGui::TextWrapped("Status: %s", _this->getStatus().c_str());
        if (!live) { return; }
        const auto d = _this->client.details();
        if (!d.name.empty()) { ImGui::TextWrapped("%s%s%s", d.name.c_str(), d.location.empty() ? "" : ", ", d.location.c_str()); }
        ImGui::Text("%s %s, %.0f kHz wide around %.3f MHz", _this->client.serverVersion().c_str(), _this->profile.sdrId.c_str(),
                    _this->profileSpan() / 1e3, _this->profileCentre() / 1e6);
        ImGui::Text("IQ arrival %.1f %%, %d listener%s", 100.0f * _this->arrivalRatio.load(), _this->client.clients(),
                    _this->client.clients() == 1 ? "" : "s");
        if (_this->profile.audioCompression == "adpcm") {
            ImGui::TextColored(warn, "IQ is 4-bit ADPCM compressed (lossy) on this receiver");
        }
        if (d.sessionTimeoutS > 0) {
            const auto since = Clock::now() - fromTicks(_this->connectedAt.load());
            const int left = d.sessionTimeoutS - (int)std::chrono::duration_cast<std::chrono::seconds>(since).count();
            ImGui::Text("Session time left: %d min", std::max(0, left) / 60);
        }
        if (Clock::now() < _this->tuneNoticeUntil && !_this->providerRegistered) {
            ImGui::TextColored(warn, "That frequency is outside the receiver's current profile");
        }
        if (_this->fellBack) { ImGui::TextColored(warn, "Server waterfall unavailable; local FFT of the IQ"); }
        if (_this->linkSlow.load()) { ImGui::TextColored(warn, "Link too slow for 48 kHz IQ"); }
    }

    std::string name;
    bool enabled = true;
    bool iqConfirmed = false;
    Clock::time_point iqRequestedAt{};
    Clock::time_point openedAt{};
    std::atomic<bool> iqRequested{false};
    std::atomic<bool> gotIq{false};
    std::atomic<bool> sessionEnded{false};
    bool selected = false;

    Client client;
    char urlBuf[512] = "";
    bool serverWaterfall = true;
    std::vector<std::string> recent;

    std::atomic<bool> running{false};
    std::atomic<bool> connected{false};
    std::atomic<bool> configComplete{false};
    std::atomic<bool> profileChanged{false};
    std::atomic<int64_t> configAt{0};
    std::atomic<int64_t> connectedAt{0};
    std::atomic<int> bufferMs{150};
    std::atomic<float> arrivalRatio{1.0f};
    std::atomic<bool> linkSlow{false};
    std::atomic<double> requestedHw{7100000.0};  // the last tune() request

    std::mutex statusMtx;
    std::string status = "Not connected";
    bool statusSticky = false;  // an error or refusal stop() keeps

    // UI thread.
    std::atomic<bool> iqStarted{false};  // stop() may clear it from the network thread
    bool profileApplied = false;
    Client::Config profile;
    double sentOffsetHw = 0.0;       // the W the server was last sent, hardware Hz
    double intendedOffsetHw = 0.0;   // local FFT: the W tuning asks for
    Clock::time_point lastOffsetSend{};
    Clock::time_point tuneNoticeUntil{};
    std::string profileNotice;
    std::string pendingProfile;
    PopupDialog profileDialog;
    Clock::time_point lastProfileChange{};  // kept across sessions

    // Network thread.
    std::atomic<uint64_t> samplesPushed{0};
    Clock::time_point guardStart{};
    uint64_t guardSamples = 0;
    int slowWindows = 0;
    SpectrumFrame specFrame;

    dsp::stream<dsp::complex_t> rawStream{8192};
    dsp::stream<dsp::complex_t> stream{8192};
    dsp::buffer::PrebufferedLink<dsp::complex_t> chain;
    std::mutex bufferModeMtx;
    SourceManager::SourceHandler handler;

    // Server waterfall and IQ window, UI thread.
    Provider provider;
    bool providerRegistered = false;
    bool fellBack = false;  // core fell back this session
    double intendedW = 0.0;  // display Hz
    // Written by one thread, read by another.
    std::atomic<int64_t> lastFrameTicks{0};
    std::atomic<bool> frameSink{false};
    std::atomic<bool> windowLive{false};
    std::atomic<double> uiDisplayCentre{0.0};
    std::atomic<double> profileSpanNet{0.0};
    std::atomic<bool> uiInvertIQ{false};
    std::atomic<uint64_t> linkIndexBase{0};
    std::atomic<uint64_t> moveGeneration{0};
    // Offset changes sent and not yet past their mute.
    std::mutex movesMtx;
    std::deque<Move> moves;
};

MOD_EXPORT void _INIT_() {
    json def = json({});
    def["url"] = "";
    def["bufferMs"] = 150;
    def["serverWaterfall"] = true;
    def["recent"] = json::array();
    config.setPath(core::args["root"].s() + "/openwebrx_source_config.json");
    config.load(def);
    config.enableAutoSave();
}

MOD_EXPORT ModuleManager::Instance* _CREATE_INSTANCE_(std::string name) {
    return new OpenWebRxSourceModule(name);
}

MOD_EXPORT void _DELETE_INSTANCE_(ModuleManager::Instance* instance) {
    delete (OpenWebRxSourceModule*)instance;
}

MOD_EXPORT void _END_() {
    config.shutdown();
}
