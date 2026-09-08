#include <signal_path/signal_path.h>
#include <gui/gui.h>
#include <gui/tuner.h>
#include <string>

// For synchronization with hardware radio, where the VFO frequency is locked at a fixed offset from IF center frequency.
bool g_vfoLockedToCenter = false;
double g_vfoToCenterOffsetHz = 0.0;

namespace tuner {

    void panadapterTuning(std::string vfoName, ImGui::WaterfallVFO* vfo, double freq, double offset) {
        assert(vfo != nullptr);

        double BW = gui::waterfall.getBandwidth();
        double viewBW = gui::waterfall.getViewBandwidth();
        sigpath::vfoManager.setOffset(vfoName, offset);
        //FIXME why? Some sanitization before setCenterFrequency?
        gui::waterfall.setViewOffset((BW / 2.0) - (viewBW / 2.0));
        double centerFreq = freq - offset;
        gui::waterfall.setCenterFrequency(centerFreq);
        // What logic to use here to be natural for the user?
        // Variant A: Keep the VFO reference line in the center of the view.
        // Variant B: Keep the VFO center frequency in the center of the view.
        // Variant C: Keep the VFO center frequency in the same relative position in the view.
        gui::waterfall.setViewOffset(vfo->centerOffset);
        gui::freqSelect.setFrequency(freq);
        sigpath::sourceManager.tune(centerFreq);
    }

    void centerTuning(std::string vfoName, double freq) {
        if (vfoName != "") {
            auto itVfo = gui::waterfall.vfos.find(vfoName);
            if (itVfo == gui::waterfall.vfos.end())
                return;
            if (g_vfoLockedToCenter) {
                panadapterTuning(vfoName, itVfo->second, freq, g_vfoToCenterOffsetHz);
                return;
            }
            sigpath::vfoManager.setOffset(vfoName, 0);
        }
        double BW = gui::waterfall.getBandwidth();
        double viewBW = gui::waterfall.getViewBandwidth();
        gui::waterfall.setViewOffset((BW / 2.0) - (viewBW / 2.0));
        gui::waterfall.setCenterFrequency(freq);
        gui::waterfall.setViewOffset(0);
        gui::freqSelect.setFrequency(freq);
        sigpath::sourceManager.tune(freq);
    }

    void normalTuningFree(std::string vfoName, ImGui::WaterfallVFO* vfo, double freq) {
        double viewBW = gui::waterfall.getViewBandwidth();
        double BW = gui::waterfall.getBandwidth();

        double currentOff = vfo->centerOffset;
        double currentTune = gui::waterfall.getCenterFrequency() + vfo->generalOffset;
        double delta = freq - currentTune;

        double newVFO = currentOff + delta;
        double vfoBW = vfo->bandwidth;
        double vfoBottom = newVFO - (vfoBW / 2.0);
        double vfoTop = newVFO + (vfoBW / 2.0);

        double view = gui::waterfall.getViewOffset();
        double viewBottom = view - (viewBW / 2.0);
        double viewTop = view + (viewBW / 2.0);

        // VFO still fints in the view
        if (vfoBottom > viewBottom && vfoTop < viewTop) {
            sigpath::vfoManager.setCenterOffset(vfoName, newVFO);
            return;
        }

        double bottom = -(BW / 2.0);
        double top = (BW / 2.0);

        // VFO too low for current SDR tuning
        if (vfoBottom < bottom) {
            gui::waterfall.setViewOffset((BW / 2.0) - (viewBW / 2.0));
            double newVFOOffset = (BW / 2.0) - (vfoBW / 2.0) - (viewBW / 10.0);
            sigpath::vfoManager.setOffset(vfoName, newVFOOffset);
            gui::waterfall.setCenterFrequency(freq - newVFOOffset);
            sigpath::sourceManager.tune(freq - newVFOOffset);
            return;
        }

        // VFO too high for current SDR tuning
        if (vfoTop > top) {
            gui::waterfall.setViewOffset((viewBW / 2.0) - (BW / 2.0));
            double newVFOOffset = (vfoBW / 2.0) - (BW / 2.0) + (viewBW / 10.0);
            sigpath::vfoManager.setOffset(vfoName, newVFOOffset);
            gui::waterfall.setCenterFrequency(freq - newVFOOffset);
            sigpath::sourceManager.tune(freq - newVFOOffset);
            return;
        }

        // VFO is still without the SDR's bandwidth
        if (delta < 0) {
            double newViewOff = vfoTop - (viewBW / 2.0) + (viewBW / 10.0);
            double newViewBottom = newViewOff - (viewBW / 2.0);

            if (newViewBottom > bottom) {
                gui::waterfall.setViewOffset(newViewOff);
                sigpath::vfoManager.setCenterOffset(vfoName, newVFO);
                return;
            }

            gui::waterfall.setViewOffset((BW / 2.0) - (viewBW / 2.0));
            double newVFOOffset = (BW / 2.0) - (vfoBW / 2.0) - (viewBW / 10.0);
            sigpath::vfoManager.setCenterOffset(vfoName, newVFOOffset);
            gui::waterfall.setCenterFrequency(freq - newVFOOffset);
            sigpath::sourceManager.tune(freq - newVFOOffset);
        }
        else {
            double newViewOff = vfoBottom + (viewBW / 2.0) - (viewBW / 10.0);
            double newViewTop = newViewOff + (viewBW / 2.0);

            if (newViewTop < top) {
                gui::waterfall.setViewOffset(newViewOff);
                sigpath::vfoManager.setCenterOffset(vfoName, newVFO);
                return;
            }

            gui::waterfall.setViewOffset((viewBW / 2.0) - (BW / 2.0));
            double newVFOOffset = (vfoBW / 2.0) - (BW / 2.0) + (viewBW / 10.0);
            sigpath::vfoManager.setCenterOffset(vfoName, newVFOOffset);
            gui::waterfall.setCenterFrequency(freq - newVFOOffset);
            sigpath::sourceManager.tune(freq - newVFOOffset);
        }
    }

    void normalTuning(std::string vfoName, double freq) {
        if (vfoName.empty()) {
            centerTuning(vfoName, freq);
        } else if (auto it = gui::waterfall.vfos.find(vfoName); it != gui::waterfall.vfos.end()) {
            ImGui::WaterfallVFO* vfo = it->second;
            if (g_vfoLockedToCenter) {
                panadapterTuning(vfoName, vfo, freq, g_vfoToCenterOffsetHz);
            } else {
                normalTuningFree(vfoName, vfo, freq);
            }
        }
    }

    void iqTuning(double freq) {
        gui::waterfall.setCenterFrequency(freq);
        gui::waterfall.centerFreqMoved = true;
        sigpath::sourceManager.tune(freq);
    }

    void tune(int mode, std::string vfoName, double freq) {
        switch (mode) {
        case TUNER_MODE_CENTER:
            centerTuning(vfoName, freq);
            break;
        case TUNER_MODE_NORMAL:
            normalTuning(vfoName, freq);
            break;
        case TUNER_MODE_LOWER_HALF:
            normalTuning(vfoName, freq);
            break;
        case TUNER_MODE_UPPER_HALF:
            normalTuning(vfoName, freq);
            break;
        case TUNER_MODE_IQ_ONLY:
            iqTuning(freq);
            break;
        }
    }

    bool lockVFOtoCenter(double offsetHz)
    {
        if (g_vfoLockedToCenter && g_vfoToCenterOffsetHz == offsetHz)
            // not updated
            return false;
        g_vfoLockedToCenter = true;
        g_vfoToCenterOffsetHz = offsetHz;
        return true;
    }

    void unlockVFO()
    {
        g_vfoLockedToCenter = false;
        g_vfoToCenterOffsetHz = 0.0;
    }

    bool vfoLockedToCenter()
    {
        return g_vfoLockedToCenter;
    }
}
