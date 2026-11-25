/*
 * This file is based on code from project SDRPlusPlusBrown (https://github.com/sannysanoff/SDRPlusPlusBrown)
 * originally licensed under the GNU General Public License, version 3 (GPLv3).
 *
 * Modified and extended by qrp73, 2025 (https://github.com/qrp73/SDRPP).
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
#define IMGUI_DEFINE_MATH_OPERATORS
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2ipdef.h>
#include <ws2tcpip.h>
#endif

#include <imgui.h>
#include <utils/flog.h>
#include <module.h>
#include <gui/gui.h>
#include <gui/widgets/simple_widgets.h>
#include <signal_path/signal_path.h>
#include <core.h>
#include <config.h>
#include <dsp/buffer/prebuffer.h>
#include "utils/proto/kiwisdr.h"
#include "utils/proto/kiwisdr_wf.h"
#include <signal_path/spectrum_provider.h>
#include <signal_path/iq_window_policy.h>
#include "utils/url.h"
#include "gui/smgui.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <optional>
#include <utility>
#include <chrono>
#include <ctime>
#include <cstring>
#include <cctype>
#include <vector>
#include <fstream>
#include <atomic>
#include <mutex>
#include <gui/remote/browser.h>
#include <gui/remote/recent.h>
#include "kiwisdr_probe.h"


SDRPP_MOD_INFO{
    /* Name:            */ "kiwisdr_source",
    /* Description:     */ "KiwiSDR WebSDR source module for SDRIAK",
    /* Author:          */ "qrp73; san",
    /* Version:         */ 0, 1, 0,
    /* Max instances    */ 1
};


ConfigManager config;

static std::tm safeLocalTime(std::time_t t) {
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    return tm;
}

struct KiwiSDRSourceModule : public ModuleManager::Instance {
    static constexpr int BUFFER_MS_MIN = 0;
    static constexpr int BUFFER_MS_MAX = 2000;

    // Fallback tuning range for a plain (converter-less) KiwiSDR when the
    // server's served band is unknown (manually typed host, no directory
    // entry). KiwiSDR/KiwiSDR 2 are specified as 10 kHz - 30 MHz receivers,
    // which covers the overwhelming majority of servers.
    static constexpr uint64_t DEFAULT_MIN_FREQ = 10000ull;     // 10 kHz
    static constexpr uint64_t DEFAULT_MAX_FREQ = 30000000ull;  // 30 MHz

    // A served frequency range, Hz.
    struct FrequencyBand {
        uint64_t startHz = 0;
        uint64_t endHz = 0;
    };

    char kiwisdrHost[1024] = "";
    int kiwisdrPort = 8073;
    std::string kiwisdrLoc = "";
    // Served frequency range of the selected server, if known (from the
    // directory 'bands' field). Used to constrain the tuning UI.
    std::optional<FrequencyBand> selectedBand;
    // Last native [lo, hi] pushed to sourceManager from the live-refine path,
    // so menuHandler only re-applies the limit when the served range changes.
    std::optional<std::pair<uint64_t, uint64_t>> appliedServedRange;
    // Declared before kiwiSdrClient, which stops it from its destructor.
    KiwiWaterfallClient wfClient;
    KiwiSDRClient kiwiSdrClient;
    std::string root;

    static constexpr size_t RECENT_MAX = 8;

    // One remembered server in the most-recently-used list shown by the
    // "Recent..." combo. loc/band come from the directory entry when the
    // server was picked on the map; both may be empty for typed hosts.
    struct RecentServer {
        std::string host;
        int port = 8073;
        std::string loc;
        std::optional<FrequencyBand> band;
    };
    std::vector<RecentServer> recentServers; // front = most recent

    std::string kiwisdrHostPort() const {
        return std::string(kiwisdrHost) + ":" + std::to_string(kiwisdrPort);
    }

    KiwiSDRSourceModule(std::string name, const std::string &root) : kiwiSdrClient() {
        this->name = name;
        this->root = root;

        // Not served headless: re-streaming an already networked source adds
        // a pointless hop (a client should connect to the KiwiSDR directly),
        // and the disconnect handling relies on gui::mainWindow, which is
        // never driven on a server. Skip registration so the source doesn't
        // appear in the server's list. Also the remote browser is a custom canvas widget
        // that can't be serialized over the server protocol, so it has no counterpart
        // in the streamed (remote client) UI.
        if (core::args["server"].b()) { return; }

        json recent;
        {
            auto configAccess = config.read();
            std::string host;
            if (configAccess.tryGet("kiwisdr_host", host)) {
                std::strncpy(kiwisdrHost, host.c_str(), sizeof(kiwisdrHost) - 1);
                kiwisdrHost[sizeof(kiwisdrHost) - 1] = '\0';
            }
            configAccess.tryGet("kiwisdr_port", kiwisdrPort);
            configAccess.tryGet("kiwisdr_loc", kiwisdrLoc);
            FrequencyBand b;
            if (configAccess.tryGet("kiwisdr_band_start", b.startHz) &&
                configAccess.tryGet("kiwisdr_band_end", b.endHz) &&
                b.startHz <= b.endHz) {
                selectedBand = b;
            }
            int storedBufferMs = 0;
            if (configAccess.tryGet("kiwisdr_buffer_ms", storedBufferMs)) {
                bufferMs = std::clamp<int>(storedBufferMs, BUFFER_MS_MIN, BUFFER_MS_MAX);
            }
            serverWaterfall = configAccess.value("kiwisdr_server_waterfall", true);
            wfSpeed = std::clamp<int>(configAccess.value("kiwisdr_wf_speed", KiwiWaterfallClient::kMaxSpeed),
                                      KiwiWaterfallClient::kMinSpeed, KiwiWaterfallClient::kMaxSpeed);
            recent = configAccess.value("kiwisdr_recent", json());
        }
        // Parsed outside the access: loadRecent() walks the whole list and
        // builds a string per entry, none of which needs the config mutex.
        if (!recent.is_null()) { loadRecent(recent); }

        kiwiSdrClient.init(kiwisdrHostPort());
        chain.init(&rawStream, &stream);
        appliedRate = kiwiSdrClient.audioRate();
        chain.setSampleRate(appliedRate);
        chain.setPrebufferMsec(bufferMs.load(), false, false);
        kiwiSdrClient.onSamples = [this](const dsp::complex_t* samples, int count) {
            memcpy(rawStream.writeBuf, samples, count * sizeof(dsp::complex_t));
            if (rawStream.swap(count)) { sndPushed += (uint64_t)count; }
        };
        // Before onSamples: the packet's first sample is the next input.
        kiwiSdrClient.onPacket = [this](const KiwiSDRClient::SndPacket& packet) {
            const uint64_t first = sndInputBase + sndPushed;
            seqClock.record(packet.seq, chain.playoutTime(first));
            commitIQWindow(packet, first);
        };
        wfClient.onFrame = [this](const KiwiWaterfallClient::Frame& frame) { onWaterfallFrame(frame); };
        provider.module = this;

        // Yeah no server-ception, sorry...
        // Initialize lists
        handler.ctx = this;
        handler.selectHandler = menuSelected;
        handler.deselectHandler = menuDeselected;
        handler.menuHandler = menuHandler;
        handler.startHandler = start;
        handler.stopHandler = stop;
        handler.tuneHandler = tune;
        handler.uiTickHandler = uiTick;
        handler.stream = &stream;

        kiwiSdrClient.onConnected = [&]() {
            connected = true;
            tune(lastTuneFrequency, this);
        };

        kiwiSdrClient.onDisconnected = [&]() {
            connected = false;
            running = false;
            gui::mainWindow.setPlayState(false);
        };



        // Load config

        sigpath::sourceManager.registerSource("KiwiSDR", &handler);

        remote::RemoteHandler remoteHandler;
        remoteHandler.kind = remote::RemoteKind::KiwiSDR;
        remoteHandler.sourceName = "KiwiSDR";
        remoteHandler.supportsTls = false; // the SND client speaks plain http
        remoteHandler.probe = probeKiwiSDR;
        remoteHandler.connect = [this](const remote::RemoteEntry& entry) { connectRemote(entry); };
        sigpath::remoteRegistry.registerHandler(remoteHandler);
    }

    ~KiwiSDRSourceModule() {
        // Server mode: the constructor returned before registering.
        if (core::args["server"].b()) { return; }
        sigpath::remoteRegistry.unregisterHandler(remote::RemoteKind::KiwiSDR);
        stop(this);
        wfClient.stop();
        sigpath::sourceManager.unregisterSource("KiwiSDR");
    }

    void postInit() {
    }

    void enable() {
        enabled = true;
    }

    void disable() {
        enabled = false;
    }

    bool isEnabled() {
        return enabled;
    }

    // Constrain the main-window frequency selector to the server's served
    // range: the known band if we have one, otherwise the plain-KiwiSDR
    // default. Also trims the number of visible digits in the display.
    void applyFreqLimits() {
        uint64_t lo, hi;
        if (selectedBand) {
            // The directory 'bands' field often reports a start of 0; a plain
            // KiwiSDR isn't usefully tunable below 10 kHz, so keep that floor.
            lo = std::max<uint64_t>(selectedBand->startHz, DEFAULT_MIN_FREQ);
            hi = selectedBand->endHz;
        }
        else {
            lo = DEFAULT_MIN_FREQ;
            hi = DEFAULT_MAX_FREQ;
        }
        // Pass the native range; the manager shifts it by any tuning offset
        // (converter-equipped Kiwi) into the display domain.
        sigpath::sourceManager.setTuningLimits((double)lo, (double)hi);
        // This estimate now owns the widget, so the live-refine cache no longer
        // reflects it. Invalidate it so the next served-range poll re-applies
        // even if the new server reports the same [lo, hi] as the previous one.
        appliedServedRange.reset();
    }

    static bool sameHostPort(const RecentServer& r, const std::string& host, int port) {
        if (r.port != port || r.host.size() != host.size()) { return false; }
        return std::equal(r.host.begin(), r.host.end(), host.begin(), [](char a, char b) {
            return std::tolower((unsigned char)a) == std::tolower((unsigned char)b);
        });
    }

    const RecentServer* findRecent(const std::string& host, int port) const {
        for (const auto& r : recentServers) {
            if (sameHostPort(r, host, port)) { return &r; }
        }
        return nullptr;
    }

    // Move (or insert) the entry to the front of the MRU list. Does not
    // persist; callers fold recentToJson() into their own config write.
    void touchRecent(RecentServer entry) {
        recentServers.erase(std::remove_if(recentServers.begin(), recentServers.end(),
                                           [&](const RecentServer& r) { return sameHostPort(r, entry.host, entry.port); }),
                            recentServers.end());
        recentServers.insert(recentServers.begin(), std::move(entry));
        if (recentServers.size() > RECENT_MAX) { recentServers.resize(RECENT_MAX); }
    }

    json recentToJson() const {
        json arr = json::array();
        for (const auto& r : recentServers) {
            json j;
            j["host"] = r.host;
            j["port"] = r.port;
            j["loc"] = r.loc;
            if (r.band) {
                j["band_start"] = r.band->startHz;
                j["band_end"] = r.band->endHz;
            }
            arr.push_back(j);
        }
        return arr;
    }

    // Defensive parse of the persisted MRU list: a malformed entry is
    // skipped, never fatal (the config file is hand-editable).
    void loadRecent(const json& arr) {
        recentServers.clear();
        if (!arr.is_array()) { return; }
        for (const auto& j : arr) {
            if (!j.is_object() || !j.contains("host") || !j["host"].is_string()) { continue; }
            RecentServer r;
            r.host = j["host"];
            r.port = j.value("port", 8073);
            r.loc = j.value("loc", std::string());
            if (j.contains("band_start") && j.contains("band_end")) {
                FrequencyBand b;
                b.startHz = j["band_start"];
                b.endHz = j["band_end"];
                if (b.startHz <= b.endHz) { r.band = b; }
            }
            if (r.host.empty() || findRecent(r.host, r.port)) { continue; }
            recentServers.push_back(std::move(r));
            if (recentServers.size() >= RECENT_MAX) { break; }
        }
    }

    // Single commit path for every way of choosing a server (map pick,
    // typed host/port, recent-list pick): update the widgets' state, touch
    // the MRU list, persist everything in one config write, re-derive the
    // tuning limits and re-point the client.
    void commitServer(const std::string& host, int port, const std::string& loc,
                      const std::optional<FrequencyBand>& band) {
        std::strncpy(kiwisdrHost, host.c_str(), sizeof(kiwisdrHost) - 1);
        kiwisdrHost[sizeof(kiwisdrHost) - 1] = '\0';
        kiwisdrPort = port;
        kiwisdrLoc = loc;
        selectedBand = band;
        touchRecent(RecentServer{ host, port, loc, band });
        {
            auto configAccess = config.edit();
            configAccess.set("kiwisdr_host", host);
            configAccess.set("kiwisdr_port", port);
            configAccess.set("kiwisdr_loc", loc);
            if (band) {
                configAccess.set("kiwisdr_band_start", band->startHz);
                configAccess.set("kiwisdr_band_end", band->endHz);
            }
            else {
                configAccess.erase("kiwisdr_band_start");
                configAccess.erase("kiwisdr_band_end");
            }
            configAccess.set("kiwisdr_recent", recentToJson());
        }
        applyFreqLimits();
        kiwiSdrClient.init(kiwisdrHostPort());
    }

    // The remote browser's Connect: the browser has selected this source.
    void connectRemote(const remote::RemoteEntry& entry) {
        std::optional<FrequencyBand> band;
        if (!entry.ranges.empty()) { band = FrequencyBand{ entry.ranges.front().first, entry.ranges.front().second }; }
        commitServer(entry.target.host, entry.target.port, entry.location, band);
        gui::mainWindow.setPlayState(true);
    }

    // Apply a hand-typed host/port (from the text boxes on commit). A typed
    // server has no directory entry, but if it matches a remembered recent
    // server, re-adopt that entry's location/band; otherwise fall back to
    // the default tuning range.
    void applyTypedServer() {
        std::string loc;
        std::optional<FrequencyBand> band;
        if (const RecentServer* r = findRecent(kiwisdrHost, kiwisdrPort)) {
            loc = r->loc;
            band = r->band;
        }
        commitServer(kiwisdrHost, kiwisdrPort, loc, band);
        remote::RemoteTarget target{ "http", kiwisdrHost, kiwisdrPort, "" };
        auto entry = remote::manualEntry(remote::RemoteKind::KiwiSDR, target, loc);
        if (band) { entry.ranges.emplace_back(band->startHz, band->endHz); }
        entry.location = loc;
        remote::recordRecent(entry);
    }

    void applyRecent(int idx) {
        if (idx < 0 || idx >= (int)recentServers.size()) { return; }
        RecentServer r = recentServers[idx]; // copy: commitServer reorders the list
        commitServer(r.host, r.port, r.loc, r.band);
    }

    void removeRecent(int idx) {
        if (idx < 0 || idx >= (int)recentServers.size()) { return; }
        recentServers.erase(recentServers.begin() + idx);
        config.edit().set("kiwisdr_recent", recentToJson());
    }

    static void menuSelected(void* ctx) {
        KiwiSDRSourceModule* _this = (KiwiSDRSourceModule*)ctx;
        // The server's audio_rate (12000, 20250 on rx3.wf3), the default
        // until one has reported it; uiTick follows later changes.
        _this->appliedRate = _this->kiwiSdrClient.audioRate();
        core::setInputSampleRate(_this->appliedRate);
        _this->applyFreqLimits();
        flog::info("KiwiSDRSourceModule '{0}': Menu Select!", _this->name);
    }

    static void menuDeselected(void* ctx) {
        KiwiSDRSourceModule* _this = (KiwiSDRSourceModule*)ctx;
        _this->unregisterProvider();
        // Release the range constraint so a stale KiwiSDR band doesn't leak
        // into whatever source is selected next.
        sigpath::sourceManager.clearTuningLimits();
        _this->appliedServedRange.reset();
        flog::info("KiwiSDRSourceModule '{0}': Menu Deselect!", _this->name);
    }

    // UI thread, once per frame while selected: the network threads only
    // store what the server reported; the DSP rate, the waterfall and the
    // server waterfall's view change here.
    static void uiTick(void* ctx) {
        KiwiSDRSourceModule* _this = (KiwiSDRSourceModule*)ctx;
        const int rate = _this->kiwiSdrClient.audioRate();
        if (rate != _this->appliedRate) {
            flog::info("KiwiSDRSourceModule '{0}': server audio rate {1}", _this->name, rate);
            _this->appliedRate = rate;
            core::setInputSampleRate(rate);
            _this->applyBufferMode(true);
            if (_this->providerRegistered) { sigpath::vfoManager.setIQWindow(_this->intendedW.load(), _this->iqWindowWidth()); }
        }
        _this->serverWaterfallTick();
        if (_this->providerRegistered) { _this->placeIQWindow(); }
        if (_this->running.load() && _this->connected.load()) { _this->sendIQTune(true); }
    }

    // Server waterfall (W/F) -------------------------------------------------

    // The SpectrumProvider core sees; all calls on the UI thread.
    struct Provider : public SpectrumProvider {
        KiwiSDRSourceModule* module = nullptr;
        std::string name() const override { return "KiwiSDR"; }
        int binCount() const override { return KiwiWaterfallClient::kBins; }
        std::vector<double> zoomSpans() const override { return module->wfSpans; }
        void requestSpan(double spanHz) override { module->requestSpan(spanHz); }
        bool available() const override {
            return module->running.load() && module->wfClient.available();
        }
        std::string unavailableReason() const override { return module->wfClient.unavailableReason(); }
        std::pair<float, float> defaultLevels() const override { return { -120.0f, -50.0f }; }
    };

    // Playout time of each recent SND packet by its seq: a W/F line carries
    // the seq of the audio leaving when it was computed.
    class SeqClock {
    public:
        void record(uint32_t seq, std::chrono::steady_clock::time_point t) {
            std::lock_guard lck(m_mtx);
            m_slots[seq % kSlots] = Slot{ seq, t, true };
        }
        std::optional<std::chrono::steady_clock::time_point> find(uint32_t seq) {
            std::lock_guard lck(m_mtx);
            const Slot& s = m_slots[seq % kSlots];
            if (!s.valid || s.seq != seq) { return std::nullopt; }
            return s.t;
        }
        void clear() {
            std::lock_guard lck(m_mtx);
            m_slots.fill(Slot{});
        }
    private:
        static constexpr size_t kSlots = 256;
        struct Slot {
            uint32_t seq = 0;
            std::chrono::steady_clock::time_point t{};
            bool valid = false;
        };
        std::mutex m_mtx;
        std::array<Slot, kSlots> m_slots{};
    };

    // IQ window -----------------------------------------------------------
    //
    // With the server waterfall, the SND stream is a window at C + W inside
    // the displayed span; the VFOs keep their place and the window follows
    // them (iq_window_policy.h). W is display-domain Hz from C.

    // Usable window: the passband +-(rate/2 - 100) less 300 Hz of margin
    // and the filter's roll-off on each side.
    double iqWindowWidth() const { return (double)appliedRate - 600.0; }

    // W in hardware Hz: Invert IQ mirrors the display.
    double hardwareOffset(double w) const { return uiInvertIQ.load() ? -w : w; }

    // UI thread, while the provider is registered: place the window over
    // the VFOs, inside the receiver range.
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
        // Hardware limits: the whole passband inside 0 ... bandwidth.
        const double half = (double)appliedRate / 2.0;
        const double centreBb = lastTuneFrequency - (double)kiwiSdrClient.frequencyOffsetHz();
        const double lo = half - centreBb;
        const double hi = wfClient.setup().bandwidthHz - half - centreBb;
        in.minOffset = uiInvertIQ.load() ? -hi : lo;
        in.maxOffset = uiInvertIQ.load() ? -lo : hi;
        const double w = chooseIQWindow(in);
        if (std::fabs(w - intendedW.load()) >= 1.0) {
            intendedW = w;
            sigpath::vfoManager.setIQWindow(w, in.width);
        }
    }

    // Send the SND tune for C + W. From tune() at once, from the UI tick
    // when the window moved (rate-limited to 8 per second). A send that
    // moves the window arms the commit (commitIQWindow).
    void sendIQTune(bool rateLimited) {
        std::lock_guard lck(iqTuneMtx);
        const double w = providerRegistered ? intendedW.load() : 0.0;
        const double hw = hardwareOffset(w);
        const double freq = lastTuneFrequency + hw;
        const auto now = std::chrono::steady_clock::now();
        if (sentIqValid && std::fabs(freq - sentIqFreq) < 0.5) { return; }
        if (rateLimited && now - lastIqSend < std::chrono::milliseconds(125)) { return; }
        if (!sentIqValid || std::fabs(hw - sentHwW) >= 0.5) {
            pendingW = w;
            pendingSinceTicks = now.time_since_epoch().count();
            pendingActive = true;
        }
        kiwiSdrClient.tune(freq, KiwiSDRClient::TUNE_IQ);
        sentIqFreq = freq;
        sentHwW = hw;
        sentIqValid = true;
        lastIqSend = now;
    }

    // SND thread, every packet before its samples: once the server applies a
    // window move, commit the VFOs' DSP offsets at the sample where the IQ
    // switched, muting around it. KiwiSDR flags the first packet after a
    // retune (NEW_FREQ); the switch falls about 24 ms into it (research
    // 8.2, 8.6). Without the flag (Kiwi-compatible servers, or none within
    // 400 ms), commit at the first packet 150 ms after the command.
    void commitIQWindow(const KiwiSDRClient::SndPacket& packet, uint64_t firstSample) {
        if (!pendingActive.load()) { return; }
        const double sr = kiwiSdrClient.sampleRate();
        const auto since = std::chrono::steady_clock::now()
                           - std::chrono::steady_clock::time_point(std::chrono::steady_clock::duration(pendingSinceTicks.load()));
        const bool compatible = kiwiSdrClient.flavour() == KiwiSDRClient::Flavour::KiwiCompatible;
        if (packet.newFreq && !compatible) {
            pendingActive = false;
            scheduleCommit(firstSample + (uint64_t)std::llround(0.024 * sr), pendingW.load(), 0.006, packet.count);
            return;
        }
        const bool noFlag = compatible || since > std::chrono::milliseconds(400);
        if (noFlag && since >= std::chrono::milliseconds(150)) {
            pendingActive = false;
            scheduleCommit(firstSample, pendingW.load(), 0.020, packet.count);
        }
    }

    // Mute every VFO from halfMute before index to halfMute after it, and
    // commit W at index. A marker fires before the whole output block that
    // holds its sample, so the mute also covers one block.
    void scheduleCommit(uint64_t index, double w, double halfMute, int packetSamples) {
        const double sr = kiwiSdrClient.sampleRate();
        const uint64_t half = (uint64_t)std::llround(halfMute * sr);
        const int block = std::max(packetSamples, (int)std::lround(0.010 * sr));
        const int muteSamples = (int)(2 * half) + block;
        chain.scheduleMarker(index > half ? index - half : 0, [muteSamples] { sigpath::vfoManager.muteAllFor(muteSamples); });
        chain.scheduleMarker(index, [w] { sigpath::vfoManager.commitIQWindowOffset(w); });
    }

    // UI thread: the deepest zoom whose span still covers spanHz.
    void requestSpan(double spanHz) {
        int zoom = 0;
        for (int z = 0; z < (int)wfSpans.size(); z++) {
            if (wfSpans[(size_t)z] >= spanHz * (1.0 - 1e-9)) { zoom = z; }
        }
        wantZoom = zoom;
    }

    void serverWaterfallTick() {
        // What the W/F thread needs to place a frame in the display domain.
        uiTuningOffset = sigpath::sourceManager.getTuningOffset();
        uiCentre = gui::waterfall.getCenterFrequency();
        uiInvertIQ = sigpath::iqFrontEnd.getInvertIQ();

        // Core takes the provider away when it reports unavailable.
        if (providerRegistered && sigpath::iqFrontEnd.spectrumProvider() != &provider) {
            providerRegistered = false;
        }
        // Stopped or switched off: this source withdraws the provider. A
        // waterfall that fails mid-session is left to core, which falls back
        // to the local FFT with a notice; it is not registered again until
        // the next session.
        if (!(serverWaterfall && running.load() && connected.load())) {
            unregisterProvider();
            return;
        }

        const KiwiWaterfallClient::Setup setup = wfClient.setup();
        if (!providerRegistered) {
            if (!wfClient.available() || sigpath::iqFrontEnd.spectrumProvider() != nullptr) { return; }
            wfSpans.clear();
            for (int z = 0; z <= setup.zoomCap; z++) { wfSpans.push_back(KiwiWaterfallClient::zoomSpanHz(setup, z)); }
            sigpath::iqFrontEnd.setSpectrumProvider(&provider);
            providerRegistered = true;
            wantZoom = 0;
            sentZoom = -1;
            appliedZoom = -1;
            intendedW = 0.0;
            sigpath::vfoManager.setIQWindow(0.0, iqWindowWidth());
        }

        // The span the frames say the server computes.
        int zoom = frameZoom.load();
        if (zoom < 0) { zoom = wantZoom; }
        if (zoom != appliedZoom && zoom >= 0 && zoom < (int)wfSpans.size()) {
            appliedZoom = zoom;
            core::setDisplaySpan(wfSpans[(size_t)zoom]);
        }

        // View: zoom and centre (baseband kHz), at most 8 sends per second;
        // a held-back change goes out once the interval has passed.
        const double cfKhz = (lastTuneFrequency - (double)kiwiSdrClient.frequencyOffsetHz()) / 1e3;
        const auto now = std::chrono::steady_clock::now();
        const bool changed = (wantZoom != sentZoom) || std::fabs(cfKhz - sentCfKhz) >= 0.001;
        if (changed && now - lastViewSend >= std::chrono::milliseconds(125)) {
            wfClient.setView(wantZoom, cfKhz);
            sentZoom = wantZoom;
            sentCfKhz = cfKhz;
            lastViewSend = now;
        }
    }

    // UI thread.
    void unregisterProvider() {
        if (!providerRegistered) { return; }
        providerRegistered = false;
        if (sigpath::iqFrontEnd.spectrumProvider() == &provider) {
            sigpath::iqFrontEnd.setSpectrumProvider(nullptr);
        }
        core::clearDisplaySpan();
        // W = 0 at once; the retune to C follows from the UI tick.
        sigpath::vfoManager.clearIQWindow();
        intendedW = 0.0;
        appliedZoom = -1;
    }

    // W/F thread: one line to core, in the display domain.
    void onWaterfallFrame(const KiwiWaterfallClient::Frame& frame) {
        if (!connected.load() || frame.count <= 0) { return; }
        const KiwiWaterfallClient::Setup setup = wfClient.setup();
        wfFrame.bins.resize((size_t)frame.count);
        for (int i = 0; i < frame.count; i++) {
            wfFrame.bins[(size_t)i] = KiwiWaterfallClient::binDbm(setup, frame.bins[i]);
        }
        const double span = KiwiWaterfallClient::zoomSpanHz(setup, frame.zoom) * frame.count / KiwiWaterfallClient::kBins;
        double start = KiwiWaterfallClient::frameStartHz(setup, frame.xBin)
                       + (double)kiwiSdrClient.frequencyOffsetHz() + uiTuningOffset.load();
        if (uiInvertIQ.load()) {
            std::reverse(wfFrame.bins.begin(), wfFrame.bins.end());
            start = 2.0 * uiCentre.load() - (start + span);
        }
        wfFrame.startHz = start;
        wfFrame.spanHz = span;
        wfFrame.presentAt = presentAt(frame.seq);
        frameZoom.store(frame.zoom);
        // The queue swaps in an older buffer, which the next frame reuses.
        sigpath::iqFrontEnd.pushSpectrum(std::move(wfFrame));
    }

    // When the line should appear: with the audio of its seq on KiwiSDR
    // firmware, else (no seq match, or a Kiwi-compatible server's unrelated
    // seq) after the audio now in the prebuffer.
    std::chrono::steady_clock::time_point presentAt(uint32_t seq) {
        if (kiwiSdrClient.flavour() == KiwiSDRClient::Flavour::KiwiSDR) {
            if (auto t = seqClock.find(seq)) { return *t; }
        }
        const double delay = (double)chain.bufferedSamples() / kiwiSdrClient.sampleRate();
        return std::chrono::steady_clock::now()
               + std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(delay));
    }

    static void start(void* ctx) {
        KiwiSDRSourceModule* _this = (KiwiSDRSourceModule*)ctx;
        if (_this->running.load()) { return; }
        if (_this->running.exchange(true)) { return; }
        {
            std::lock_guard lck(_this->bufferModeMtx);
            _this->rawStream.clearWriteStop();
            _this->applyBufferModeLocked(true);
            _this->chain.start();
        }
        _this->seqClock.clear();
        _this->frameZoom = -1;
        // The link has consumed everything pushed so far; this session's
        // samples continue its input count.
        _this->sndInputBase = _this->chain.samplesIn();
        _this->sndPushed = 0;
        {
            std::lock_guard lck(_this->iqTuneMtx);
            _this->sentIqValid = false;
            _this->pendingActive = false;
        }
        _this->kiwiSdrClient.waterfall = _this->serverWaterfall ? &_this->wfClient : nullptr;
        _this->kiwiSdrClient.waterfallSpeed = _this->wfSpeed;
        _this->kiwiSdrClient.waterfallCfKhz = _this->lastTuneFrequency / 1e3;
        _this->kiwiSdrClient.start();
        flog::info("KiwiSDRSourceModule '{0}': Start!", _this->name);
    }

    static void stop(void* ctx) {
        KiwiSDRSourceModule* _this = (KiwiSDRSourceModule*)ctx;
        bool wasRunning = _this->running.exchange(false);
        if (!wasRunning && !_this->kiwiSdrClient.running.load()) { return; }
        // Unblock a writer stuck in rawStream.swap() before stopping the
        // client (whose network thread is that writer), and outside the mode
        // mutex so a concurrent applyBufferMode can finish its switch.
        _this->rawStream.stopWriter();
        _this->kiwiSdrClient.stop();
        {
            std::lock_guard lck(_this->bufferModeMtx);
            _this->chain.stop();
            _this->rawStream.clearWriteStop();
        }
        flog::info("KiwiSDRSourceModule '{0}': Stop!", _this->name);
    }

    // Serialized against stop(): the slider applies the mode on the GUI
    // thread, while stop() can arrive on the client's network thread
    // (onDisconnected -> setPlayState(false) -> SourceManager::stop). An
    // unsynchronized interleaving could restart the prebuffer after stop()
    // already tore it down.
    void applyBufferMode(bool resetBuffer) {
        std::lock_guard lck(bufferModeMtx);
        applyBufferModeLocked(resetBuffer);
    }

    void applyBufferModeLocked(bool resetBuffer) {
        chain.setSampleRate(appliedRate);
        // A live mode switch re-plumbs the link while the KiwiSDR network
        // thread may sit blocked in rawStream.swap(); release it for the
        // duration of the switch (it tolerates the dropped block).
        const bool live = running.load();
        const bool replumb = live && (bufferMs.load() > 0) != chain.prebuffering();
        if (replumb) { rawStream.stopWriter(); }
        chain.setPrebufferMsec(bufferMs.load(), resetBuffer, live);
        if (replumb) { rawStream.clearWriteStop(); }
    }


    double lastTuneFrequency = 14.100;

    static void tune(double freq, void* ctx) {
        KiwiSDRSourceModule* _this = (KiwiSDRSourceModule*)ctx;
        _this->lastTuneFrequency = freq;
        if (_this->running.load() && _this->connected.load()) {
            _this->sendIQTune(false);
        }
        flog::info("KiwiSDRSourceModule '{0}': Tune: {1}!", _this->name, freq);
    }


    static void menuHandler(void* ctx) {

        KiwiSDRSourceModule* _this = (KiwiSDRSourceModule*)ctx;

        // While connected, refine the tuning range to the exact span the
        // server reports (freq_offset + bandwidth). This supersedes the
        // directory-band / default estimate set on select, and corrects for
        // converter-equipped Kiwis and 32 MHz wideband units. Runs on the GUI
        // thread, so it's the safe place to touch gui::freqSelect.
        if (_this->connected.load()) {
            if (auto range = _this->kiwiSdrClient.getServedRange()) {
                const uint64_t lo = std::max<int64_t>(range->minHz, (int64_t)DEFAULT_MIN_FREQ);
                const uint64_t hi = std::max<int64_t>(range->maxHz, range->minHz);
                const auto served = std::make_pair(lo, hi);
                if (_this->appliedServedRange != served) {
                    _this->appliedServedRange = served;
                    // Native range; the manager applies any tuning offset.
                    sigpath::sourceManager.setTuningLimits((double)lo, (double)hi);
                }
            }
        }

        // This source is never registered on a headless server (see the
        // constructor), so this menu only ever renders locally.
        const bool playing = gui::mainWindow.isPlaying();

        // The shared remote browser, KiwiSDR only. Its Connect stops the
        // radio first, so it stays enabled while playing.
        if (ImGui::Button("Browse...##_kiwisdr_browse")) {
            remote::openBrowser(remote::RemoteKind::KiwiSDR);
        }

        ImGui::BeginDisabled(playing);
        if (SmGui::InputText(("##_kiwisdr_host_" + _this->name).c_str(),
                             _this->kiwisdrHost, sizeof(_this->kiwisdrHost))) {
            config.edit().set("kiwisdr_host", std::string(_this->kiwisdrHost));
        }
        bool hostCommitted = ImGui::IsItemDeactivatedAfterEdit();
        SmGui::SameLine();
        SmGui::FillWidth();
        if (SmGui::InputInt(("##_kiwisdr_port_" + _this->name).c_str(), &_this->kiwisdrPort, 0, 0)) {
            config.edit().set("kiwisdr_port", _this->kiwisdrPort);
        }
        bool portCommitted = ImGui::IsItemDeactivatedAfterEdit();
        if (hostCommitted || portCommitted) {
            _this->applyTypedServer();
        }
        ImGui::EndDisabled();

        // Recent servers, MRU first. Plain ImGui is fine here: this source
        // is never registered on a headless server (see constructor).
        if (!_this->recentServers.empty()) {
            ImGui::BeginDisabled(playing);
            ImGui::SetNextItemWidth(-1);
            if (ImGui::BeginCombo(("##_kiwisdr_recent_" + _this->name).c_str(), "Recent...")) {
                int pick = -1, remove = -1;
                const float btn = ImGui::GetFrameHeight();
                const float spacing = ImGui::GetStyle().ItemInnerSpacing.x;
                // Center the label vertically in the button-height row.
                ImGui::PushStyleVar(ImGuiStyleVar_SelectableTextAlign, ImVec2(0.0f, 0.5f));
                for (int i = 0; i < (int)_this->recentServers.size(); i++) {
                    const auto& r = _this->recentServers[i];
                    ImGui::PushID(i);
                    const std::string label = r.loc.empty()
                        ? r.host + ":" + std::to_string(r.port) : r.loc;
                    // Row = pick target sized to leave a square for the
                    // delete button; the two never overlap, so a tap can't
                    // mis-fire.
                    const float pickWidth = ImGui::GetContentRegionAvail().x - btn - spacing;
                    if (ImGui::Selectable(label.c_str(), false, 0, ImVec2(pickWidth, btn))) {
                        pick = i;
                    }
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip("%s:%d%s%s", r.host.c_str(), r.port,
                                          r.loc.empty() ? "" : "\n", r.loc.c_str());
                    }
                    ImGui::SameLine(0.0f, spacing);
                    // Button, not Selectable: buttons don't close the combo
                    // popup, so several entries can be pruned in one visit.
                    if (ImGui::Button("x", ImVec2(btn, btn))) { remove = i; }
                    ImGui::PopID();
                }
                ImGui::PopStyleVar();
                ImGui::EndCombo();
                // Defer both actions out of the loop; they mutate recentServers.
                if (pick >= 0)        { _this->applyRecent(pick); }
                else if (remove >= 0) { _this->removeRecent(remove); }
            }
            ImGui::EndDisabled();
        }

        if (!_this->kiwisdrLoc.empty())
            SmGui::TextF("Loc: %s", _this->kiwisdrLoc.c_str());
        SmGui::TextF("Status: %s", _this->kiwiSdrClient.getConnectionStatus().c_str());
        if (_this->connected.load()) {
            const KiwiSDRClient& c = _this->kiwiSdrClient;
            const char* server = c.flavour() == KiwiSDRClient::Flavour::KiwiSDR          ? "KiwiSDR"
                                 : c.flavour() == KiwiSDRClient::Flavour::KiwiCompatible ? "Kiwi-compatible"
                                                                                         : "?";
            SmGui::TextF("%s, %.3f S/s, RSSI %.0f dBm", server, c.sampleRate(), c.rssiDbm());
            if (c.seqGaps() > 0) { SmGui::TextF("Lost packets: %llu", (unsigned long long)c.seqGaps()); }
            if (c.adcOverloadRecent()) { SmGui::TextF("ADC overload"); }
        }

        std::time_t t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
        auto tmm = safeLocalTime(t);
        char streamTime[64];
        strftime(streamTime, sizeof(streamTime), "%Y-%m-%d %H:%M:%S", &tmm);
        SmGui::TextF("Stream pos: %s", streamTime);

        // Network prebuffer: trades latency for immunity against connection
        // jitter (mobile links). Takes effect live, including while playing.
        int bufMs = _this->bufferMs.load();
        SmGui::LeftLabel("Buffer (ms)");
        SmGui::FillWidth();
        if (SmGui::SliderInt(("##_kiwisdr_buffer_ms_" + _this->name).c_str(), &bufMs, BUFFER_MS_MIN, BUFFER_MS_MAX)) {
            _this->bufferMs.store(bufMs);
            _this->applyBufferMode(true);
            config.edit().set("kiwisdr_buffer_ms", bufMs);
        }

        // Server waterfall: opened with the session, so the switch applies
        // on the next start; the speed applies live.
        ImGui::BeginDisabled(playing);
        if (ImGui::Checkbox(("Server waterfall##_kiwisdr_wf_" + _this->name).c_str(), &_this->serverWaterfall)) {
            config.edit().set("kiwisdr_server_waterfall", _this->serverWaterfall);
        }
        ImGui::EndDisabled();
        if (_this->serverWaterfall) {
            static const char* kSpeeds = "1 fps\0" "5 fps\0" "13 fps\0" "23 fps\0";
            int speedId = _this->wfSpeed - KiwiWaterfallClient::kMinSpeed;
            ImGui::LeftLabel("Waterfall speed");
            ImGui::FillWidth();
            if (ImGui::Combo(("##_kiwisdr_wf_speed_" + _this->name).c_str(), &speedId, kSpeeds)) {
                _this->wfSpeed = speedId + KiwiWaterfallClient::kMinSpeed;
                _this->wfClient.setSpeed(_this->wfSpeed);
                config.edit().set("kiwisdr_wf_speed", _this->wfSpeed);
            }
            if (playing && _this->connected.load()) {
                if (_this->providerRegistered) {
                    const auto setup = _this->wfClient.setup();
                    SmGui::TextF("Waterfall: server, zoom %d of %d", std::max(_this->appliedZoom, 0), setup.zoomCap);
                }
                else {
                    const std::string reason = _this->wfClient.unavailableReason();
                    SmGui::TextF("Waterfall: local (%s)", reason.empty() ? "server waterfall unavailable" : reason.c_str());
                }
            }
        }

        KiwiSDRClient::AgcSettings agc = _this->kiwiSdrClient.getAgc();
        bool agcChanged = false;
        const std::string agcId = "AGC##_kiwisdr_agc_" + _this->name;
        agcChanged |= SmGui::Checkbox(agcId.c_str(), &agc.enabled);
        SmGui::SameLine();
        const std::string hangId = "Hang##_kiwisdr_agc_hang_" + _this->name;
        agcChanged |= SmGui::Checkbox(hangId.c_str(), &agc.hang);

        auto slider = [&](const char* label, const char* id, int* value, int min, int max, SmGui::FormatString format) {
            SmGui::LeftLabel(label);
            SmGui::FillWidth();
            const std::string sliderId = std::string(id) + _this->name;
            return SmGui::SliderInt(sliderId.c_str(), value, min, max, format);
        };

        if (agc.enabled) {
            agcChanged |= slider("Threshold", "##_kiwisdr_agc_threshold_", &agc.thresholdDb, -130, 0, SmGui::FMT_STR_INT_DB);
            agcChanged |= slider("Slope", "##_kiwisdr_agc_slope_", &agc.slopeDb, 0, 10, SmGui::FMT_STR_INT_DB);
            agcChanged |= slider("Decay", "##_kiwisdr_agc_decay_", &agc.decayMs, 20, 5000, SmGui::FMT_STR_INT_DEFAULT);
        }
        else {
            agcChanged |= slider("Gain", "##_kiwisdr_agc_gain_", &agc.manualGainDb, 0, 120, SmGui::FMT_STR_INT_DB);
        }
        if (agcChanged) {
            _this->kiwiSdrClient.setAgc(agc);
        }
    }


    std::string name;
    bool enabled = true;
    std::atomic<bool> running{false};
    std::atomic<bool> connected{false};
    // Shared prebuffer target in milliseconds; applied live while playing.
    std::atomic<int> bufferMs{250};

    double freq;
    bool serverBusy = false;

    dsp::stream<dsp::complex_t> rawStream;
    dsp::stream<dsp::complex_t> stream;
    dsp::buffer::PrebufferedLink<dsp::complex_t> chain;
    // Guards the prebuffer/bypass mode switch (the chain's routing and
    // start/stop) between the GUI thread and a stop() arriving on the
    // client's network thread.
    std::mutex bufferModeMtx;
    SourceManager::SourceHandler handler;
    // The rate the DSP and the prebuffer run at; GUI thread.
    int appliedRate = KiwiSDRClient::kDefaultAudioRate;

    // Server waterfall settings (config), UI thread.
    bool serverWaterfall = true;
    int wfSpeed = KiwiWaterfallClient::kMaxSpeed;
    // UI thread.
    Provider provider;
    bool providerRegistered = false;
    std::vector<double> wfSpans;   // span per zoom, 0 ... zoom_cap
    int wantZoom = 0;
    int sentZoom = -1;
    double sentCfKhz = 0.0;
    int appliedZoom = -1;          // zoom whose span core shows
    std::chrono::steady_clock::time_point lastViewSend{};
    // Written on the UI thread, read on the W/F thread.
    std::atomic<double> uiTuningOffset{0.0};
    std::atomic<double> uiCentre{0.0};
    std::atomic<bool> uiInvertIQ{false};
    // W/F thread.
    SpectrumFrame wfFrame;
    std::atomic<int> frameZoom{-1};  // zoom of the latest frame
    SeqClock seqClock;

    // IQ window. intendedW: UI thread writes, the tune path reads.
    std::atomic<double> intendedW{0.0};
    // Last SND tune sent; guarded by iqTuneMtx (tune() may come from any
    // thread).
    std::mutex iqTuneMtx;
    bool sentIqValid = false;
    double sentIqFreq = 0.0;
    double sentHwW = 0.0;
    std::chrono::steady_clock::time_point lastIqSend{};
    // A window move the server has not applied yet: set by sendIQTune(),
    // consumed on the SND thread.
    std::atomic<bool> pendingActive{false};
    std::atomic<double> pendingW{0.0};
    std::atomic<int64_t> pendingSinceTicks{0};
    // SND thread: the link's input index of the next pushed sample is
    // sndInputBase + sndPushed.
    uint64_t sndInputBase = 0;
    uint64_t sndPushed = 0;

    std::shared_ptr<KiwiSDRClient> client;
};

MOD_EXPORT void _INIT_() {
    json def = json({});
    config.setPath(core::args["root"].s() + "/kiwisdr_source_config.json");
    config.load(def);
    config.enableAutoSave();
}

MOD_EXPORT ModuleManager::Instance* _CREATE_INSTANCE_(std::string name) {
    auto root = core::args["root"].s();
    return new KiwiSDRSourceModule(name, root);
}

MOD_EXPORT void _DELETE_INSTANCE_(ModuleManager::Instance* instance) {
    delete (KiwiSDRSourceModule*)instance;
}

MOD_EXPORT void _END_() {
    config.shutdown();
}
