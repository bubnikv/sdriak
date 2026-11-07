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
#include "utils/url.h"
#include "gui/smgui.h"
#include <algorithm>
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
#include <gui/brown/kiwisdr_map.h>


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

    char kiwisdrHost[1024] = "";
    int kiwisdrPort = 8073;
    std::string kiwisdrLoc = "";
    // Served frequency range of the selected server, if known (from the
    // directory 'bands' field). Used to constrain the tuning UI.
    std::optional<ServerEntry::FrequencyBand> selectedBand;
    // Last native [lo, hi] pushed to sourceManager from the live-refine path,
    // so menuHandler only re-applies the limit when the served range changes.
    std::optional<std::pair<uint64_t, uint64_t>> appliedServedRange;
    KiwiSDRClient kiwiSdrClient;
    std::string root;
    KiwiSDRMapSelector selector;

    static constexpr size_t RECENT_MAX = 8;

    // One remembered server in the most-recently-used list shown by the
    // "Recent..." combo. loc/band come from the directory entry when the
    // server was picked on the map; both may be empty for typed hosts.
    struct RecentServer {
        std::string host;
        int port = 8073;
        std::string loc;
        std::optional<ServerEntry::FrequencyBand> band;
    };
    std::vector<RecentServer> recentServers; // front = most recent

    std::string kiwisdrHostPort() const {
        return std::string(kiwisdrHost) + ":" + std::to_string(kiwisdrPort);
    }

    KiwiSDRSourceModule(std::string name, const std::string &root) : kiwiSdrClient(), selector(root, &config, "KiwiSDR Source") {
        this->name = name;
        this->root = root;

        // Not served headless: re-streaming an already networked source adds
        // a pointless hop (a client should connect to the KiwiSDR directly),
        // and the disconnect handling relies on gui::mainWindow, which is
        // never driven on a server. Skip registration so the source doesn't
        // appear in the server's list. Also the map selector is a custom canvas widget
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
            ServerEntry::FrequencyBand b;
            if (configAccess.tryGet("kiwisdr_band_start", b.startHz) &&
                configAccess.tryGet("kiwisdr_band_end", b.endHz) &&
                b.startHz <= b.endHz) {
                selectedBand = b;
            }
            int storedBufferMs = 0;
            if (configAccess.tryGet("kiwisdr_buffer_ms", storedBufferMs)) {
                bufferMs = std::clamp<int>(storedBufferMs, BUFFER_MS_MIN, BUFFER_MS_MAX);
            }
            recent = configAccess.value("kiwisdr_recent", json());
        }
        // Parsed outside the access: loadRecent() walks the whole list and
        // builds a string per entry, none of which needs the config mutex.
        if (!recent.is_null()) { loadRecent(recent); }

        kiwiSdrClient.init(kiwisdrHostPort());
        chain.init(&rawStream, &stream);
        chain.setSampleRate(kiwiSdrClient.IQDATA_FREQUENCY);
        chain.setPrebufferMsec(bufferMs.load(), false, false);
        kiwiSdrClient.onSamples = [this](const dsp::complex_t* samples, int count) {
            memcpy(rawStream.writeBuf, samples, count * sizeof(dsp::complex_t));
            rawStream.swap(count);
        };

        // Yeah no server-ception, sorry...
        // Initialize lists
        handler.ctx = this;
        handler.selectHandler = menuSelected;
        handler.deselectHandler = menuDeselected;
        handler.menuHandler = menuHandler;
        handler.startHandler = start;
        handler.stopHandler = stop;
        handler.tuneHandler = tune;
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
    }

    ~KiwiSDRSourceModule() {
        // Server mode: the constructor returned before registering.
        if (core::args["server"].b()) { return; }
        stop(this);
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
                ServerEntry::FrequencyBand b;
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
                      const std::optional<ServerEntry::FrequencyBand>& band) {
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

    // Apply a hand-typed host/port (from the text boxes on commit). A typed
    // server has no directory entry, but if it matches a remembered recent
    // server, re-adopt that entry's location/band; otherwise fall back to
    // the default tuning range.
    void applyTypedServer() {
        std::string loc;
        std::optional<ServerEntry::FrequencyBand> band;
        if (const RecentServer* r = findRecent(kiwisdrHost, kiwisdrPort)) {
            loc = r->loc;
            band = r->band;
        }
        commitServer(kiwisdrHost, kiwisdrPort, loc, band);
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
        core::setInputSampleRate(12000); // fixed for kiwisdr
        _this->applyFreqLimits();
        flog::info("KiwiSDRSourceModule '{0}': Menu Select!", _this->name);
    }

    static void menuDeselected(void* ctx) {
        KiwiSDRSourceModule* _this = (KiwiSDRSourceModule*)ctx;
        // Release the range constraint so a stale KiwiSDR band doesn't leak
        // into whatever source is selected next.
        sigpath::sourceManager.clearTuningLimits();
        _this->appliedServedRange.reset();
        flog::info("KiwiSDRSourceModule '{0}': Menu Deselect!", _this->name);
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
        chain.setSampleRate(kiwiSdrClient.IQDATA_FREQUENCY);
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
            _this->kiwiSdrClient.tune(freq, KiwiSDRClient::TUNE_IQ);
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

        ImGui::BeginDisabled(playing);
        if (ImGui::Button("Choose on map...")) {
            _this->selector.openPopup();
        }
        ImGui::EndDisabled();

        _this->selector.drawPopup([=](const std::string &hostPort, const std::string &loc, const std::optional<ServerEntry::FrequencyBand> &band) {
            auto parsed = url::splitHostPort(hostPort);
            std::string host = parsed ? parsed->host : hostPort;
            int port = parsed ? parsed->port : 8073;
            // The KiwiSDR source is the active source while this popup is
            // open, so commitServer's applyFreqLimits() retunes the digit
            // limits to the newly chosen server.
            _this->commitServer(host, port, loc, band);
        });

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
