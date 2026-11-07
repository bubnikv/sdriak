#pragma once
// Manual/auto AGC controls adapted from the SDRPP fork by qrp73 (https://github.com/qrp73/SDRPP).
#include "../demod.h"
#include <dsp/demod/ssb.h>

namespace demod {
    class DSB : public Demodulator {
    public:
        DSB() {}

        DSB(std::string name, ConfigManager* config, dsp::stream<dsp::complex_t>* input, double bandwidth, double audioSR) {
            init(name, config, input, bandwidth, audioSR);
        }

        ~DSB() {
            stop();
        }

        void init(std::string name, ConfigManager* config, dsp::stream<dsp::complex_t>* input, double bandwidth, double audioSR) {
            this->name = name;
            _config = config;

            // Load config
            agc.load(loadSection());

            // Define structure
            demod.init(input, dsp::demod::SSB<dsp::stereo_t>::Mode::DSB, bandwidth, getIFSampleRate(), agc.attack / getIFSampleRate(), agc.decay / getIFSampleRate());
            agc.apply(demod);
        }

        void start() { demod.start(); }

        void stop() { demod.stop(); }

        void showMenu() {
            agc.showMenu(demod, this, "dsb", getIFSampleRate(), ImGui::GetContentRegionAvail().x);
        }

        void setBandwidth(double bandwidth) { demod.setBandwidth(bandwidth); }

        void setInput(dsp::stream<dsp::complex_t>* input) { demod.setInput(input); }

        void AFSampRateChanged(double newSR) {}

        // ============= INFO =============

        const char* getName() { return radioModeName(RADIO_IFACE_MODE_DSB); }
        double getIFSampleRate() { return 24000.0; }
        double getAFSampleRate() { return getIFSampleRate(); }
        double getDefaultBandwidth() { return 4600.0; }
        double getMinBandwidth() { return 1000.0; }
        double getMaxBandwidth() { return getIFSampleRate() / 2.0; }
        bool getBandwidthLocked() { return false; }
        double getDefaultSnapInterval() { return 100.0; }
        int getVFOReference() { return ImGui::WaterfallVFO::REF_CENTER; }
        bool getDeempAllowed() { return false; }
        bool getPostProcEnabled() { return true; }
        int getDefaultDeemphasisMode() { return DEEMP_MODE_NONE; }
        bool getFMIFNRAllowed() { return false; }
        bool getNBAllowed() { return true; }
        bool getHighPassAllowed() { return true; }
        bool getSquelchAllowed() { return true; }
        dsp::stream<dsp::stereo_t>* getOutput() { return &demod.out; }

    private:
        dsp::demod::SSB<dsp::stereo_t> demod;
        AGCControls agc;
    };
}