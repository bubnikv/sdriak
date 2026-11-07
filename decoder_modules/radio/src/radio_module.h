#pragma once
#include <imgui.h>
#include <module.h>
#include <gui/gui.h>
#include <gui/style.h>
#include <gui/widgets/toggle_button_grid.h>
#include <signal_path/signal_path.h>
#include <config.h>
#include <dsp/chain.h>
#include <dsp/noise_reduction/noise_blanker.h>
#include <dsp/noise_reduction/fm_if.h>
#include <dsp/noise_reduction/power_squelch.h>
#include <dsp/noise_reduction/ctcss_squelch.h>
#include <dsp/multirate/rational_resampler.h>
#include <dsp/filter/deephasis.h>
#include <core.h>
#include <stdint.h>
#include <array>
#include <atomic>
#include <algorithm>
#include <memory>
#include <utils/optionlist.h>
#include <radio_interface.h>
#include "demod.h"

ConfigManager config;

#define CONCAT(a, b) ((std::string(a) + b).c_str())

std::map<DeemphasisMode, double> deempTaus = {
    { DEEMP_MODE_22US, 22e-6 },
    { DEEMP_MODE_50US, 50e-6 },
    { DEEMP_MODE_75US, 75e-6 }
};

std::map<IFNRPreset, double> ifnrTaps = {
    { IFNR_PRESET_NOAA_APT, 9},
    { IFNR_PRESET_VOICE, 15 },
    { IFNR_PRESET_NARROW_BAND, 31 },
    { IFNR_PRESET_BROADCAST, 32 }
};

class RadioModule : public ModuleManager::Instance {
public:
    RadioModule(std::string name) {
        this->name = name;

        // Initialize option lists
        deempModes.define("None", DEEMP_MODE_NONE);
        deempModes.define("22us", DEEMP_MODE_22US);
        deempModes.define("50us", DEEMP_MODE_50US);
        deempModes.define("75us", DEEMP_MODE_75US);

        ifnrPresets.define("NOAA APT", IFNR_PRESET_NOAA_APT);
        ifnrPresets.define("Voice", IFNR_PRESET_VOICE);
        ifnrPresets.define("Narrow Band", IFNR_PRESET_NARROW_BAND);

        squelchModes.define("off", "Off", SQUELCH_MODE_OFF);
        squelchModes.define("power", "Power", SQUELCH_MODE_POWER);
        //squelchModes.define("snr", "SNR", SQUELCH_MODE_SNR);
        squelchModes.define("ctcss_mute", "CTCSS (Mute)", SQUELCH_MODE_CTCSS_MUTE);
        squelchModes.define("ctcss_decode", "CTCSS (Decode Only)", SQUELCH_MODE_CTCSS_DECODE);
        //squelchModes.define("dcs_mute", "DCS (Mute)", SQUELCH_MODE_DCS_MUTE);
        //squelchModes.define("dcs_decode", "DCS (Decode Only)", SQUELCH_MODE_DCS_DECODE);

        for (int i = 0; i < dsp::noise_reduction::_CTCSS_TONE_COUNT; i++) {
            float tone = dsp::noise_reduction::CTCSS_TONES[i];
            char buf[64];
            sprintf(buf, "%.1fHz", tone);
            ctcssTones.define((int)round(tone) * 10, buf, (dsp::noise_reduction::CTCSSTone)i);
        }
        ctcssTones.define(-1, "Any", dsp::noise_reduction::CTCSS_TONE_ANY);

        // Initialize the config if it doesn't exist
        {
            auto configAccess = config.edit();
            ConfigManager::EditSection inst = configAccess.section(name);
            std::string selectedDemod;
            if (inst.tryGet("selectedDemod", selectedDemod)) {
                const int mode = radioModeFromName(selectedDemod.c_str());
                if (radioModeValid(mode)) {
                    selectedDemodID = mode;
                }
                else {
                    flog::warn("Unknown selectedDemod '{0}' for radio '{1}'; using WFM",
                               selectedDemod, name);
                }
            }
            inst.set("selectedDemod", radioModeName(selectedDemodID));
        }

        // Initialize the VFO
        vfo = sigpath::vfoManager.createVFO(name, ImGui::WaterfallVFO::REF_CENTER, 0, 200000, 200000, 50000, 200000, false);
        onUserChangedBandwidthHandler.handler = vfoUserChangedBandwidthHandler;
        onUserChangedBandwidthHandler.ctx = this;
        vfo->wtfVFO->onUserChangedBandwidth.bindHandler(&onUserChangedBandwidthHandler);

        // Initialize IF DSP chain
        ifChainOutputChanged.ctx = this;
        ifChainOutputChanged.handler = ifChainOutputChangeHandler;
        ifChain.init(vfo->output);

        nb.init(NULL, 500.0 / 24000.0, 10.0);
        fmnr.init(NULL, 32);
        powerSquelch.init(NULL, MIN_SQUELCH);

        ifChain.addBlock(&nb, false);
        ifChain.addBlock(&powerSquelch, false);
        ifChain.addBlock(&fmnr, false);

        // Initialize audio DSP chain
        afChain.init(&dummyAudioStream);

        ctcss.init(NULL, 50000.0);
        resamp.init(NULL, 250000.0, 48000.0);
        hpTaps = dsp::taps::highPass(300.0, 100.0, 48000.0);
        hpf.init(NULL, hpTaps);
        deemp.init(NULL, 50e-6, 48000.0);

        afChain.addBlock(&ctcss, false);
        afChain.addBlock(&resamp, true);
        afChain.addBlock(&hpf, false);
        afChain.addBlock(&deemp, false);

        // Initialize the sink
        srChangeHandler.ctx = this;
        srChangeHandler.handler = sampleRateChangeHandler;
        stream.init(afChain.out, &srChangeHandler, audioSampleRate);
        sigpath::sinkManager.registerStream(name, &stream);

        // Select the demodulator
        selectDemodByID((DemodID)selectedDemodID);

        // Start IF chain
        ifChain.start();

        // Start AF chain
        afChain.start();

        // Start stream, the rest was started when selecting the demodulator
        stream.start();

        // Register the menu
        gui::menu.registerEntry(name, menuHandler, this, this);

        // Register the module interface
        core::modComManager.registerInterface("radio", name, moduleInterfaceHandler, this);
    }

    ~RadioModule() {
        core::modComManager.unregisterInterface(name);
        gui::menu.removeEntry(name);
        stream.stop();
        if (enabled) {
            disable();
        }
        sigpath::sinkManager.unregisterStream(name);
    }

    void postInit() {}

    void enable() {
        enabled = true;
        if (!vfo) {
            vfo = sigpath::vfoManager.createVFO(name, ImGui::WaterfallVFO::REF_CENTER, 0, 200000, 200000, 50000, 200000, false);
            vfo->wtfVFO->onUserChangedBandwidth.bindHandler(&onUserChangedBandwidthHandler);
        }
        ifChain.setInput(vfo->output, [=](dsp::stream<dsp::complex_t>* out){ ifChainOutputChangeHandler(out, this); });
        ifChain.start();
        selectDemodByID((DemodID)selectedDemodID);
        afChain.start();
    }

    void disable() {
        enabled = false;
        ifChain.stop();
        if (selectedDemod) { selectedDemod->stop(); }
        afChain.stop();
        if (vfo) { sigpath::vfoManager.deleteVFO(vfo); }
        vfo = NULL;
    }

    bool isEnabled() {
        return enabled;
    }

    std::string name;

    enum DemodID {
        RADIO_DEMOD_NFM,
        RADIO_DEMOD_WFM,
        RADIO_DEMOD_AM,
        RADIO_DEMOD_DSB,
        RADIO_DEMOD_USB,
        RADIO_DEMOD_CW,
        RADIO_DEMOD_LSB,
        RADIO_DEMOD_RAW,
        RADIO_DEMOD_CWR,
        _RADIO_DEMOD_COUNT,
    };

    static_assert(RADIO_DEMOD_NFM == RADIO_IFACE_MODE_NFM, "NFM mode IDs diverged");
    static_assert(RADIO_DEMOD_WFM == RADIO_IFACE_MODE_WFM, "WFM mode IDs diverged");
    static_assert(RADIO_DEMOD_AM  == RADIO_IFACE_MODE_AM,  "AM mode IDs diverged");
    static_assert(RADIO_DEMOD_DSB == RADIO_IFACE_MODE_DSB, "DSB mode IDs diverged");
    static_assert(RADIO_DEMOD_USB == RADIO_IFACE_MODE_USB, "USB mode IDs diverged");
    static_assert(RADIO_DEMOD_CW  == RADIO_IFACE_MODE_CW,  "CW mode IDs diverged");
    static_assert(RADIO_DEMOD_LSB == RADIO_IFACE_MODE_LSB, "LSB mode IDs diverged");
    static_assert(RADIO_DEMOD_RAW == RADIO_IFACE_MODE_RAW, "RAW mode IDs diverged");
    static_assert(RADIO_DEMOD_CWR == RADIO_IFACE_MODE_CWR, "CWR mode IDs diverged");
    static_assert(_RADIO_DEMOD_COUNT == _RADIO_IFACE_MODE_COUNT, "radio mode counts diverged");

private:
    static void menuHandler(void* ctx) {
        RadioModule* _this = (RadioModule*)ctx;

        if (!_this->enabled) { style::beginDisabled(); }

        float menuWidth = ImGui::GetContentRegionAvail().x;
        ImGui::BeginGroup();

        // Display order differs from DemodID order: related modes grouped so
        // the default 5+4 wrap yields NFM WFM AM DSB RAW / LSB USB CW CWR.
        // Only the order is local -- the labels themselves come from the shared
        // table, so a button can never disagree with the name used elsewhere.
        static const std::array<DemodID, _RADIO_DEMOD_COUNT> modeIDs = {
            RADIO_DEMOD_NFM, RADIO_DEMOD_WFM, RADIO_DEMOD_AM, RADIO_DEMOD_DSB, RADIO_DEMOD_RAW,
            RADIO_DEMOD_LSB, RADIO_DEMOD_USB, RADIO_DEMOD_CW, RADIO_DEMOD_CWR
        };
        static const std::vector<std::string> modeLabels = [] {
            std::vector<std::string> l;
            for (DemodID id : modeIDs) { l.push_back(radioModeName(id)); }
            return l;
        }();
        auto modeIt = std::find(modeIDs.begin(), modeIDs.end(), (DemodID)_this->selectedDemodID);
        int modeIdx = (modeIt != modeIDs.end()) ? (int)std::distance(modeIDs.begin(), modeIt) : 0;
        if (doToggleButtonGrid("##_radio_mode_" + _this->name, modeIdx, modeLabels)) {
            _this->selectDemodByID(modeIDs[modeIdx]);
        }

        ImGui::EndGroup();

        if (!_this->bandwidthLocked) {
            ImGui::LeftLabel("Bandwidth");
            bool changed = false;
            if (_this->selectedDemodID == RADIO_DEMOD_CW || _this->selectedDemodID == RADIO_DEMOD_CWR) {
                static constexpr std::array<int, 5> cwBandwidthPresets = { 100, 200, 250, 400, 500 };
                changed = demod::showHzPresetInput(("_radio_bw_" + _this->name).c_str(), _this->bandwidth, _this->minBandwidth, _this->maxBandwidth, cwBandwidthPresets, 1.0f, 100.0f);
            }
            else {
                ImGui::SetNextItemWidth(menuWidth - ImGui::GetCursorPosX());
                if (ImGui::InputFloat(("##_radio_bw_" + _this->name).c_str(), &_this->bandwidth, 1, 100, "%.0f")) {
                    _this->bandwidth = std::clamp<float>(_this->bandwidth, _this->minBandwidth, _this->maxBandwidth);
                    changed = true;
                }
            }
            if (changed) {
                _this->setBandwidth(_this->bandwidth);
            }
        }

        // VFO snap interval
        ImGui::LeftLabel("Snap Interval");
        ImGui::SetNextItemWidth(menuWidth - ImGui::GetCursorPosX());
        if (ImGui::InputInt(("##_radio_snap_" + _this->name).c_str(), &_this->snapInterval, 1, 100)) {
            if (_this->snapInterval < 1) { _this->snapInterval = 1; }
            _this->vfo->setSnapInterval(_this->snapInterval);
            config.edit().section(_this->name, _this->selectedDemod->getName()).set("snapInterval", _this->snapInterval);
        }

        // Deemphasis mode
        if (_this->deempAllowed) {
            ImGui::LeftLabel("De-emphasis");
            ImGui::SetNextItemWidth(menuWidth - ImGui::GetCursorPosX());
            if (ImGui::Combo(("##_radio_wfm_deemp_" + _this->name).c_str(), &_this->deempId, _this->deempModes.txt)) {
                _this->setDeemphasisMode(_this->deempModes[_this->deempId]);
            }
        }

        // Squelch
        if (_this->squelchAllowed) {
            ImGui::LeftLabel("Squelch Mode");
            ImGui::FillWidth();
            if (ImGui::Combo(("##_radio_sqelch_mode_" + _this->name).c_str(), &_this->squelchModeId, _this->squelchModes.txt)) {
                _this->setSquelchMode(_this->squelchModes[_this->squelchModeId]);
            }
            switch (_this->squelchModes[_this->squelchModeId]) {
            case SQUELCH_MODE_POWER:
                ImGui::LeftLabel("Squelch Level");
                ImGui::FillWidth();
                if (ImGui::SliderFloat(("##_radio_sqelch_lvl_" + _this->name).c_str(), &_this->squelchLevel, _this->MIN_SQUELCH, _this->MAX_SQUELCH, "%.3fdB")) {
                    _this->setSquelchLevel(_this->squelchLevel);
                }
                break;

            case SQUELCH_MODE_CTCSS_MUTE:
                if (_this->squelchModes[_this->squelchModeId] == SQUELCH_MODE_CTCSS_MUTE) {
                    ImGui::LeftLabel("CTCSS Tone");
                    ImGui::FillWidth();
                    if (ImGui::Combo(("##_radio_ctcss_tone_" + _this->name).c_str(), &_this->ctcssToneId, _this->ctcssTones.txt)) {
                        _this->setCTCSSTone(_this->ctcssTones[_this->ctcssToneId]);
                    }
                }
            }
        }

        // Noise blanker
        if (_this->nbAllowed) {
            // Snapshot once: selectDemod() may rewrite nbEnabled from the rigctl
            // network thread mid-frame, and reading the guard twice would leave
            // the beginDisabled/endDisabled pair unbalanced (issue #1437).
            bool nbEnabled = _this->nbEnabled.load(std::memory_order_relaxed);
            if (ImGui::Checkbox(("Noise blanker (W.I.P.)##_radio_nb_ena_" + _this->name).c_str(), &nbEnabled)) {
                _this->setNBEnabled(nbEnabled);
            }
            bool nbDisabled = !nbEnabled && _this->enabled;
            if (nbDisabled) { style::beginDisabled(); }
            ImGui::SameLine();
            ImGui::SetNextItemWidth(menuWidth - ImGui::GetCursorPosX());
            if (ImGui::SliderFloat(("##_radio_nb_lvl_" + _this->name).c_str(), &_this->nbLevel, _this->MIN_NB, _this->MAX_NB, "%.3fdB")) {
                _this->setNBLevel(_this->nbLevel);
            }
            if (nbDisabled) { style::endDisabled(); }
        }

        // FM IF Noise Reduction
        if (_this->FMIFNRAllowed) {
            // Snapshot once (see nbEnabled above): rigctl set_mode can flip this
            // mid-frame, which would unbalance the disabled block (issue #1437).
            bool fmIFNREnabled = _this->FMIFNREnabled.load(std::memory_order_relaxed);
            if (ImGui::Checkbox(("IF Noise Reduction##_radio_fmifnr_ena_" + _this->name).c_str(), &fmIFNREnabled)) {
                _this->setFMIFNREnabled(fmIFNREnabled);
            }
            if (_this->selectedDemodID == RADIO_DEMOD_NFM) {
                bool fmIFNRDisabled = !fmIFNREnabled && _this->enabled;
                if (fmIFNRDisabled) { style::beginDisabled(); }
                ImGui::SameLine();
                ImGui::SetNextItemWidth(menuWidth - ImGui::GetCursorPosX());
                if (ImGui::Combo(("##_radio_fmifnr_ena_" + _this->name).c_str(), &_this->fmIFPresetId, _this->ifnrPresets.txt)) {
                    _this->setIFNRPreset(_this->ifnrPresets[_this->fmIFPresetId]);
                }
                if (fmIFNRDisabled) { style::endDisabled(); }
            }
        }

        // High pass
        if (_this->highPassAllowed) {
            if (ImGui::Checkbox(("High Pass##_radio_hpf_" + _this->name).c_str(), &_this->highPass)) {
                _this->setHighPass(_this->highPass);
            }
        }

        // Demodulator specific menu
        _this->selectedDemod->showMenu();

        // Display the squelch diagnostics
        switch (_this->squelchModes[_this->squelchModeId]) {
        case SQUELCH_MODE_CTCSS_MUTE:
            ImGui::TextUnformatted("Received Tone:");
            ImGui::SameLine();
            {
                auto ctone = _this->ctcss.getCurrentTone();
                auto dtone = _this->ctcssTones[_this->ctcssToneId];
                if (ctone != dsp::noise_reduction::CTCSS_TONE_NONE) {
                    if (dtone == dsp::noise_reduction::CTCSS_TONE_ANY || ctone == dtone) {
                        ImGui::TextColored(ImVec4(0, 1, 0, 1), "%.1fHz", dsp::noise_reduction::CTCSS_TONES[_this->ctcss.getCurrentTone()]);
                    }
                    else {
                        ImGui::TextColored(ImVec4(1, 0, 0, 1), "%.1fHz", dsp::noise_reduction::CTCSS_TONES[_this->ctcss.getCurrentTone()]);
                    }
                }
                else {
                    ImGui::TextUnformatted("None");
                }
            }
            break;

        case SQUELCH_MODE_CTCSS_DECODE:
            ImGui::TextUnformatted("Received Tone:");
            ImGui::SameLine();
            {
                auto ctone = _this->ctcss.getCurrentTone();
                if (ctone != dsp::noise_reduction::CTCSS_TONE_NONE) {
                    ImGui::TextColored(ImVec4(0, 1, 0, 1), "%.1fHz", dsp::noise_reduction::CTCSS_TONES[_this->ctcss.getCurrentTone()]);
                }
                else {
                    ImGui::TextUnformatted("None");
                }
            }
            break;
        }

        if (!_this->enabled) { style::endDisabled(); }
    }

    demod::Demodulator* instantiateDemod(DemodID id) {
        // Owned until the last step, so an exception out of init() below frees
        // the demodulator instead of leaking it. Ownership is released to the
        // caller only on success.
        std::unique_ptr<demod::Demodulator> demod;
        switch (id) {
            case DemodID::RADIO_DEMOD_NFM:  demod.reset(new demod::NFM()); break;
            case DemodID::RADIO_DEMOD_WFM:  demod.reset(new demod::WFM()); break;
            case DemodID::RADIO_DEMOD_AM:   demod.reset(new demod::AM());  break;
            case DemodID::RADIO_DEMOD_DSB:  demod.reset(new demod::DSB()); break;
            case DemodID::RADIO_DEMOD_USB:  demod.reset(new demod::USB()); break;
            case DemodID::RADIO_DEMOD_CW:   demod.reset(new demod::CW());  break;
            case DemodID::RADIO_DEMOD_LSB:  demod.reset(new demod::LSB()); break;
            case DemodID::RADIO_DEMOD_RAW:  demod.reset(new demod::RAW()); break;
            case DemodID::RADIO_DEMOD_CWR:  demod.reset(new demod::CWR()); break;
            default:                        break;
        }
        if (!demod) { return NULL; }

        // Default config
        double bw = demod->getDefaultBandwidth();
        {
            auto configAccess = config.edit();
            ConfigManager::EditSection dcfg = configAccess.section(name, demod->getName());
            dcfg.ensure("bandwidth", bw);
            dcfg.ensure("snapInterval", demod->getDefaultSnapInterval());
            dcfg.ensure("squelchLevel", MIN_SQUELCH);
            dcfg.ensure("squelchEnabled", false);
        }
        bw = std::clamp<double>(bw, demod->getMinBandwidth(), demod->getMaxBandwidth());

        // Initialize
        demod->init(name, &config, ifChain.out, bw, stream.getSampleRate());

        return demod.release();
    }

    void selectDemodByID(DemodID id) {
        auto startTime = std::chrono::high_resolution_clock::now();
        demod::Demodulator* demod = instantiateDemod(id);
        if (!demod) {
            flog::error("Demodulator {0} not implemented", (int)id);
            return;
        }
        selectedDemodID = id;
        selectDemod(demod);

        // Save config
        config.edit().section(name).set("selectedDemod", radioModeName((int)id));
        auto endTime = std::chrono::high_resolution_clock::now();
        flog::warn("Demod switch took {0} us", (int64_t)((std::chrono::duration_cast<std::chrono::microseconds>(endTime - startTime)).count()));
    }

    void selectDemod(demod::Demodulator* demod) {
        // Stopcurrently selected demodulator and select new
        afChain.setInput(&dummyAudioStream, [=](dsp::stream<dsp::stereo_t>* out){ stream.setInput(out); });
        if (selectedDemod) {
            selectedDemod->stop();
            delete selectedDemod;
        }
        selectedDemod = demod;

        // Give the demodulator the most recent audio SR
        selectedDemod->AFSampRateChanged(audioSampleRate);

        // Set the demodulator's input
        selectedDemod->setInput(ifChain.out);

        // Set AF chain's input
        afChain.setInput(selectedDemod->getOutput(), [=](dsp::stream<dsp::stereo_t>* out){ stream.setInput(out); });

        // Load config
        bandwidth = selectedDemod->getDefaultBandwidth();
        minBandwidth = selectedDemod->getMinBandwidth();
        maxBandwidth = selectedDemod->getMaxBandwidth();
        bandwidthLocked = selectedDemod->getBandwidthLocked();
        snapInterval = selectedDemod->getDefaultSnapInterval();
        deempAllowed = selectedDemod->getDeempAllowed();
        deempId = deempModes.valueId((DeemphasisMode)selectedDemod->getDefaultDeemphasisMode());
        squelchModeId = squelchModes.valueId(SQUELCH_MODE_OFF);
        squelchLevel = MIN_SQUELCH;
        ctcssToneId = ctcssTones.valueId(dsp::noise_reduction::CTCSS_TONE_67Hz);
        highPass = false;

        postProcEnabled = selectedDemod->getPostProcEnabled();
        FMIFNRAllowed = selectedDemod->getFMIFNRAllowed();
        FMIFNREnabled = false;
        fmIFPresetId = ifnrPresets.valueId(IFNR_PRESET_VOICE);
        nbAllowed = selectedDemod->getNBAllowed();
        squelchAllowed = selectedDemod->getSquelchAllowed();
        highPassAllowed = selectedDemod->getHighPassAllowed();
        nbEnabled = false;
        nbLevel = 0.0f;
        double ifSamplerate = selectedDemod->getIFSampleRate();
        {
            auto configAccess = config.edit();
            ConfigManager::EditSection dcfg = configAccess.section(name, selectedDemod->getName());

            if (dcfg.tryGet("bandwidth", bandwidth)) {
                bandwidth = std::clamp<double>(bandwidth, minBandwidth, maxBandwidth);
            }
            dcfg.tryGet("snapInterval", snapInterval);

            std::string squelchModeStr;
            if (dcfg.tryGet("squelchMode", squelchModeStr) && squelchModes.keyExists(squelchModeStr)) {
                squelchModeId = squelchModes.keyId(squelchModeStr);
            }
            dcfg.tryGet("squelchLevel", squelchLevel);
            int ctcssToneX10 = 0;
            if (dcfg.tryGet("ctcssTone", ctcssToneX10) && ctcssTones.keyExists(ctcssToneX10)) {
                ctcssToneId = ctcssTones.keyId(ctcssToneX10);
            }
            dcfg.tryGet("highPass", highPass);

            // A pre-rename config stored the de-emphasis mode as an index; repair
            // it with the current name rather than trying to map it back.
            if (dcfg.contains("deempMode")) {
                std::string deempOpt;
                if (dcfg.tryGet("deempMode", deempOpt) && deempModes.keyExists(deempOpt)) {
                    deempId = deempModes.keyId(deempOpt);
                }
                else {
                    dcfg.set("deempMode", deempModes.key(deempId));
                }
            }

            bool storedFMIFNREnabled = FMIFNREnabled.load();
            if (dcfg.tryGet("FMIFNREnabled", storedFMIFNREnabled)) {
                FMIFNREnabled.store(storedFMIFNREnabled);
            }
            std::string presetOpt;
            if (dcfg.tryGet("fmifnrPreset", presetOpt) && ifnrPresets.keyExists(presetOpt)) {
                fmIFPresetId = ifnrPresets.keyId(presetOpt);
            }
            bool storedNBEnabled = nbEnabled.load();
            if (dcfg.tryGet("noiseBlankerEnabled", storedNBEnabled)) {
                nbEnabled.store(storedNBEnabled);
            }
            dcfg.tryGet("noiseBlankerLevel", nbLevel);
        }

        // Configure VFO
        if (vfo) {
            vfo->setBandwidthLimits(minBandwidth, maxBandwidth, selectedDemod->getBandwidthLocked());
            vfo->setReference(selectedDemod->getVFOReference());
            vfo->setSnapInterval(snapInterval);
            vfo->setSampleRate(ifSamplerate, bandwidth);
        }

        // Configure bandwidth
        setBandwidth(bandwidth);

        // Configure noise blanker
        nb.setRate(500.0 / ifSamplerate);
        setNBLevel(nbLevel);
        setNBEnabled(nbAllowed && nbEnabled.load());

        // Configure FM IF Noise Reduction
        setIFNRPreset((selectedDemodID == RADIO_DEMOD_NFM) ? ifnrPresets[fmIFPresetId] : IFNR_PRESET_BROADCAST);
        setFMIFNREnabled(FMIFNRAllowed ? FMIFNREnabled.load() : false);

        // Configure squelch
        powerSquelch.setSamplerate(ifSamplerate);
        setSquelchMode(squelchAllowed ? squelchModes[squelchModeId] : SQUELCH_MODE_OFF);
        setSquelchLevel(squelchLevel);
        setCTCSSTone(ctcssTones[ctcssToneId]);

        // Configure AF chain
        if (postProcEnabled) {
            // Configure resampler
            afChain.stop();
            double afsr = selectedDemod->getAFSampleRate();
            ctcss.setSamplerate(afsr);
            resamp.setInSamplerate(afsr);
            afChain.enableBlock(&resamp, [=](dsp::stream<dsp::stereo_t>* out){ stream.setInput(out); });
            setAudioSampleRate(audioSampleRate);

            // Configure the HPF
            setHighPass(highPass && highPassAllowed);

            // Configure deemphasis
            setDeemphasisMode(deempModes[deempId]);
        }
        else {
            // Disable everything if post processing is disabled
            afChain.disableAllBlocks([=](dsp::stream<dsp::stereo_t>* out){ stream.setInput(out); });
        }

        // Start new demodulator
        selectedDemod->start();
    }


    void setBandwidth(double bw) {
        bw = std::clamp<double>(bw, minBandwidth, maxBandwidth);
        bandwidth = bw;
        if (!selectedDemod) { return; }
        vfo->setBandwidth(bandwidth);
        selectedDemod->setBandwidth(bandwidth);

        config.edit().section(name, selectedDemod->getName()).set("bandwidth", bandwidth);
    }

    void setAudioSampleRate(double sr) {
        audioSampleRate = sr;
        if (!selectedDemod) { return; }
        selectedDemod->AFSampRateChanged(audioSampleRate);
        if (!postProcEnabled && vfo) {
            // If postproc is disabled, IF SR = AF SR
            minBandwidth = selectedDemod->getMinBandwidth();
            maxBandwidth = selectedDemod->getMaxBandwidth();
            bandwidth = selectedDemod->getIFSampleRate();
            vfo->setBandwidthLimits(minBandwidth, maxBandwidth, selectedDemod->getBandwidthLocked());
            vfo->setSampleRate(selectedDemod->getIFSampleRate(), bandwidth);
            return;
        }

        afChain.stop();

        // Configure resampler
        resamp.setOutSamplerate(audioSampleRate);

        // Configure the HPF sample rate
        hpTaps = dsp::taps::highPass(300.0, 100.0, audioSampleRate);
        hpf.setTaps(hpTaps);

        // Configure deemphasis sample rate
        deemp.setSamplerate(audioSampleRate);

        afChain.start();
    }

    void setHighPass(bool enabled) {
        // Update the state
        highPass = enabled;

        // Check if post-processing is enabled and that a demodulator is selected
        if (!postProcEnabled || !selectedDemod) { return; }

        // Set the state of the HPF in the AF chain
        afChain.setBlockEnabled(&hpf, enabled, [=](dsp::stream<dsp::stereo_t>* out){ stream.setInput(out); });

        // Save config
        config.edit().section(name, selectedDemod->getName()).set("highPass", enabled);
    }

    void setDeemphasisMode(DeemphasisMode mode) {
        deempId = deempModes.valueId(mode);
        if (!postProcEnabled || !selectedDemod) { return; }
        bool deempEnabled = (mode != DEEMP_MODE_NONE);
        if (deempEnabled) { deemp.setTau(deempTaus[mode]); }
        afChain.setBlockEnabled(&deemp, deempEnabled, [=](dsp::stream<dsp::stereo_t>* out){ stream.setInput(out); });

        // Save config
        config.edit().section(name, selectedDemod->getName()).set("deempMode", deempModes.key(deempId));
    }

    void setNBEnabled(bool enable) {
        nbEnabled = enable;
        if (!selectedDemod) { return; }
        ifChain.setBlockEnabled(&nb, enable, [=](dsp::stream<dsp::complex_t>* out){ selectedDemod->setInput(out); });

        // Save config
        config.edit().section(name, selectedDemod->getName()).set("noiseBlankerEnabled", enable);
    }

    void setNBLevel(float level) {
        nbLevel = std::clamp<float>(level, MIN_NB, MAX_NB);
        nb.setLevel(nbLevel);
        if (!selectedDemod) { return; }

        // Save config
        config.edit().section(name, selectedDemod->getName()).set("noiseBlankerLevel", nbLevel);
    }

    void setSquelchMode(SquelchMode mode) {
        squelchModeId = squelchModes.valueId(mode);
        if (!selectedDemod) { return; }

        // Disable all squelch blocks
        ifChain.disableBlock(&powerSquelch, [=](dsp::stream<dsp::complex_t>* out){ selectedDemod->setInput(out); });
        afChain.disableBlock(&ctcss, [=](dsp::stream<dsp::stereo_t>* out){ stream.setInput(out); });

        // Enable the block depending on the mode
        switch (mode) {
        case SQUELCH_MODE_OFF:
            break;

        case SQUELCH_MODE_POWER:
            // Enable the power squelch block
            ifChain.enableBlock(&powerSquelch, [=](dsp::stream<dsp::complex_t>* out){ selectedDemod->setInput(out); });
            break;

        case SQUELCH_MODE_SNR:
            // TODO
            break;

        case SQUELCH_MODE_CTCSS_MUTE:
            // Set the required tone and enable the CTCSS squelch block
            ctcss.setRequiredTone(ctcssTones[ctcssToneId]);
            afChain.enableBlock(&ctcss, [=](dsp::stream<dsp::stereo_t>* out){ stream.setInput(out); });
            break;

        case SQUELCH_MODE_CTCSS_DECODE:
            // Set the required tone to none and enable the CTCSS squelch block
            ctcss.setRequiredTone(dsp::noise_reduction::CTCSS_TONE_NONE);
            afChain.enableBlock(&ctcss, [=](dsp::stream<dsp::stereo_t>* out){ stream.setInput(out); });
            break;

        case SQUELCH_MODE_DCS_MUTE:
            // TODO
            break;

        case SQUELCH_MODE_DCS_DECODE:
            // TODO
            break;
        }

        // Save config
        config.edit().section(name, selectedDemod->getName()).set("squelchMode", squelchModes.key(squelchModeId));
    }

    void setSquelchLevel(float level) {
        squelchLevel = std::clamp<float>(level, MIN_SQUELCH, MAX_SQUELCH);
        powerSquelch.setLevel(squelchLevel);
        if (!selectedDemod) { return; }

        // Save config
        config.edit().section(name, selectedDemod->getName()).set("squelchLevel", squelchLevel);
    }

    void setCTCSSTone(dsp::noise_reduction::CTCSSTone tone) {
        // Check for an invalid value
        if (tone == dsp::noise_reduction::CTCSS_TONE_NONE) { return; }

        // If not in CTCSS mute mode, do nothing
        if (squelchModes[squelchModeId] != SQUELCH_MODE_CTCSS_MUTE) { return; }

        // Set the tone
        ctcssToneId = ctcssTones.valueId(tone);
        ctcss.setRequiredTone(tone);
        if (!selectedDemod) { return; }

        // Save config
        config.edit().section(name, selectedDemod->getName()).set("ctcssTone", ctcssTones.key(ctcssToneId));
    }

    void setFMIFNREnabled(bool enabled) {
        FMIFNREnabled = enabled;
        if (!selectedDemod) { return; }
        ifChain.setBlockEnabled(&fmnr, enabled, [=](dsp::stream<dsp::complex_t>* out){ selectedDemod->setInput(out); });

        // Save config
        config.edit().section(name, selectedDemod->getName()).set("FMIFNREnabled", enabled);
    }

    void setIFNRPreset(IFNRPreset preset) {
        // Don't save if in broadcast mode
        if (preset == IFNR_PRESET_BROADCAST) {
            if (!selectedDemod) { return; }
            fmnr.setBins(ifnrTaps[preset]);
            return;
        }

        fmIFPresetId = ifnrPresets.valueId(preset);
        if (!selectedDemod) { return; }
        fmnr.setBins(ifnrTaps[preset]);

        // Save config
        config.edit().section(name, selectedDemod->getName()).set("fmifnrPreset", ifnrPresets.key(fmIFPresetId));
    }

    static void vfoUserChangedBandwidthHandler(double newBw, void* ctx) {
        RadioModule* _this = (RadioModule*)ctx;
        _this->setBandwidth(newBw);
    }

    static void sampleRateChangeHandler(float sampleRate, void* ctx) {
        RadioModule* _this = (RadioModule*)ctx;
        _this->setAudioSampleRate(sampleRate);
    }

    static void ifChainOutputChangeHandler(dsp::stream<dsp::complex_t>* output, void* ctx) {
        RadioModule* _this = (RadioModule*)ctx;
        if (!_this->selectedDemod) { return; }
        _this->selectedDemod->setInput(output);
    }

    static void moduleInterfaceHandler(int code, void* in, void* out, void* ctx) {
        RadioModule* _this = (RadioModule*)ctx;

        // AF chain injection commands don't require a selected demodulator.
        // Introduced in SDRPPBrown for baseband noise suppression.
        if (in) {
            switch (code) {
            case RADIO_IFACE_CMD_ADD_TO_AFCHAIN:
                _this->afChain.addBlock((dsp::Processor<dsp::stereo_t, dsp::stereo_t>*)in, false);
                return;
            case RADIO_IFACE_CMD_REMOVE_FROM_AFCHAIN:
                _this->afChain.removeBlock((dsp::Processor<dsp::stereo_t, dsp::stereo_t>*)in, [=](dsp::stream<dsp::stereo_t>* out) { _this->stream.setInput(out); });
                return;
            case RADIO_IFACE_CMD_ENABLE_IN_AFCHAIN:
                _this->afChain.setBlockEnabled((dsp::Processor<dsp::stereo_t, dsp::stereo_t>*)in, true, [=](dsp::stream<dsp::stereo_t>* out) { _this->stream.setInput(out); });
                return;
            case RADIO_IFACE_CMD_DISABLE_IN_AFCHAIN:
                _this->afChain.setBlockEnabled((dsp::Processor<dsp::stereo_t, dsp::stereo_t>*)in, false, [=](dsp::stream<dsp::stereo_t>* out) { _this->stream.setInput(out); });
                return;
            }
        }

        // If no demod is selected, reject the command
        if (!_this->selectedDemod) { return; }

        // Execute commands
        if (code == RADIO_IFACE_CMD_GET_MODE && out) {
            int* _out = (int*)out;
            *_out = _this->selectedDemodID;
        }
        else if (code == RADIO_IFACE_CMD_SET_MODE && in && _this->enabled) {
            int* _in = (int*)in;
            _this->selectDemodByID((DemodID)*_in);
        }
        else if (code == RADIO_IFACE_CMD_GET_BANDWIDTH && out) {
            float* _out = (float*)out;
            *_out = _this->bandwidth;
        }
        else if (code == RADIO_IFACE_CMD_SET_BANDWIDTH && in && _this->enabled) {
            float* _in = (float*)in;
            if (_this->bandwidthLocked) { return; }
            _this->setBandwidth(*_in);
        }
        else if (code == RADIO_IFACE_CMD_GET_SQUELCH_MODE && out) {
            SquelchMode* _out = (SquelchMode*)out;
            *_out = _this->squelchModes[_this->squelchModeId];
        }
        else if (code == RADIO_IFACE_CMD_SET_SQUELCH_MODE && in && _this->enabled) {
            SquelchMode* _in = (SquelchMode*)in;
            _this->setSquelchMode(*_in);
        }
        else if (code == RADIO_IFACE_CMD_GET_SQUELCH_LEVEL && out) {
            float* _out = (float*)out;
            *_out = _this->squelchLevel;
        }
        else if (code == RADIO_IFACE_CMD_SET_SQUELCH_LEVEL && in && _this->enabled) {
            float* _in = (float*)in;
            _this->setSquelchLevel(*_in);
        }
        else if (code == RADIO_IFACE_CMD_GET_CTCSS_TONE && out) {
            dsp::noise_reduction::CTCSSTone* _out = (dsp::noise_reduction::CTCSSTone*)out;
            *_out = _this->ctcssTones[_this->ctcssToneId];
        }
        else if (code == RADIO_IFACE_CMD_SET_CTCSS_TONE && in && _this->enabled) {
            dsp::noise_reduction::CTCSSTone* _in = (dsp::noise_reduction::CTCSSTone*)in;
            _this->setCTCSSTone(*_in);
        }
        else if (code == RADIO_IFACE_CMD_GET_HIGHPASS && out) {
            bool* _out = (bool*)out;
            *_out = _this->highPass;
        }
        else if (code == RADIO_IFACE_CMD_SET_HIGHPASS && in && _this->enabled) {
            bool* _in = (bool*)in;
            _this->setHighPass(*_in);
        }
        else {
            return;
        }

        // Success
        return;
    }

    // Handlers
    EventHandler<double> onUserChangedBandwidthHandler;
    EventHandler<float> srChangeHandler;
    EventHandler<dsp::stream<dsp::complex_t>*> ifChainOutputChanged;
    EventHandler<dsp::stream<dsp::stereo_t>*> afChainOutputChanged;

    VFOManager::VFO* vfo = NULL;

    // IF chain
    dsp::chain<dsp::complex_t> ifChain;
    dsp::noise_reduction::NoiseBlanker nb;
    dsp::noise_reduction::FMIF fmnr;
    dsp::noise_reduction::PowerSquelch powerSquelch;

    // Audio chain
    dsp::stream<dsp::stereo_t> dummyAudioStream;
    dsp::chain<dsp::stereo_t> afChain;
    dsp::noise_reduction::CTCSSSquelch ctcss;
    dsp::multirate::RationalResamplerBlock<dsp::stereo_t> resamp;
    dsp::OwnedTap<float> hpTaps;
    dsp::filter::FIRBlock<dsp::stereo_t, float> hpf;
    dsp::filter::Deemphasis<dsp::stereo_t> deemp;

    SinkManager::Stream stream;

    demod::Demodulator* selectedDemod = NULL;

    OptionList<std::string, DeemphasisMode> deempModes;
    OptionList<std::string, IFNRPreset> ifnrPresets;
    OptionList<std::string, SquelchMode> squelchModes;
    OptionList<int, dsp::noise_reduction::CTCSSTone> ctcssTones;

    double audioSampleRate = 48000.0;
    float minBandwidth;
    float maxBandwidth;
    float bandwidth;
    bool bandwidthLocked;
    int snapInterval;
    int selectedDemodID = 1;
    bool postProcEnabled;

    int squelchModeId = 0;
    float squelchLevel;
    int ctcssToneId = 0;
    bool squelchAllowed = false;

    bool highPass = false;
    bool highPassAllowed = false;

    int deempId = 0;
    bool deempAllowed;

    bool FMIFNRAllowed;
    // Atomic: rewritten by selectDemod() (which rigctl set_mode can drive from
    // the network thread) while menuHandler() reads it on the GUI thread. See
    // the snapshot in menuHandler() for issue #1437.
    std::atomic<bool> FMIFNREnabled{ false };
    int fmIFPresetId;

    bool notchEnabled = false;
    float notchPos = 0;
    float notchWidth = 500;

    bool nbAllowed;
    // Atomic, same reason as FMIFNREnabled (issue #1437).
    std::atomic<bool> nbEnabled{ false };
    float nbLevel = 10.0f;

    const double MIN_NB = 1.0;
    const double MAX_NB = 10.0;
    const double MIN_SQUELCH = -100.0;
    const double MAX_SQUELCH = 0.0;

    bool enabled = true;
};
