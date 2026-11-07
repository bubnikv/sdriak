#pragma once
// Manual/auto AGC controls adapted from the SDRPP fork by qrp73 (https://github.com/qrp73/SDRPP).
#include "../demod.h"
#include <dsp/demod/cw.h>

namespace demod {
    class CW : public Demodulator {
    public:
        CW() {}

        CW(std::string name, ConfigManager* config, dsp::stream<dsp::complex_t>* input, double bandwidth, double audioSR) {
            init(name, config, input, bandwidth, audioSR);
        }

        ~CW() {
            stop();
        }

        void init(std::string name, ConfigManager* config, dsp::stream<dsp::complex_t>* input, double bandwidth, double audioSR) {
            this->name = name;
            this->_config = config;
            this->afbwChangeHandler = afbwChangeHandler;

            // Load config
            nlohmann::json cfg = loadSection();
            agc.load(cfg);
            loadConf(cfg, "tone", tone);

            // Define structure
            demod.init(input, tone, agc.attack / getIFSampleRate(), agc.decay / getIFSampleRate(), getIFSampleRate());
            agc.apply(demod);
        }

        void start() { demod.start(); }

        void stop() { demod.stop(); }

        void showMenu() {
            agc.showMenu(demod, this, "cw", getIFSampleRate(), ImGui::GetContentRegionAvail().x);
            ImGui::LeftLabel("Tone Frequency");
            static constexpr std::array<int, 6> tonePresets = { 500, 600, 700, 750, 800, 900 };
            if (showHzPresetInput(("_radio_cw_tone_" + name).c_str(), tone, 250, 1250, tonePresets, 10, 100)) {
                demod.setTone(tone);
                saveConf("tone", tone);
            }
        }

        void setBandwidth(double bandwidth) {}

        void setInput(dsp::stream<dsp::complex_t>* input) { demod.setInput(input); }

        void AFSampRateChanged(double newSR) {}

        // ============= INFO =============

        const char* getName() { return radioModeName(RADIO_IFACE_MODE_CW); }
        double getIFSampleRate() { return 3000.0; }
        double getAFSampleRate() { return getIFSampleRate(); }
        double getDefaultBandwidth() { return 500.0; }
        double getMinBandwidth() { return 50.0; }
        double getMaxBandwidth() { return 500.0; }
        bool getBandwidthLocked() { return false; }
        double getDefaultSnapInterval() { return 10.0; }
        int getVFOReference() { return ImGui::WaterfallVFO::REF_CENTER; }
        bool getDeempAllowed() { return false; }
        bool getPostProcEnabled() { return true; }
        int getDefaultDeemphasisMode() { return DEEMP_MODE_NONE; }
        bool getFMIFNRAllowed() { return false; }
        bool getNBAllowed() { return false; }
        bool getHighPassAllowed() { return false; }
        bool getSquelchAllowed() { return false; }
        dsp::stream<dsp::stereo_t>* getOutput() { return &demod.out; }

    private:
        dsp::demod::CW<dsp::stereo_t> demod;

        AGCControls agc = { true, 0.0f, 100.0f, 5.0f };
        int tone = 700;

        EventHandler<float> afbwChangeHandler;
    };
}
