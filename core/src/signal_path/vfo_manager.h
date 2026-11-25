#pragma once
#include "../dsp/channel/rx_vfo.h"
#include <gui/widgets/waterfall.h>
#include <utils/event.h>
#include <atomic>
#include <mutex>

class VFOManager {
public:
    VFOManager();

    class VFO {
    public:
        VFO(std::string name, int reference, double offset, double bandwidth, double sampleRate, double minBandwidth, double maxBandwidth, bool bandwidthLocked);
        ~VFO();

        void setOffset(double offset);
        double getOffset();
        void setCenterOffset(double offset);
        void setBandwidth(double bandwidth, bool updateWaterfall = true);
        void setSampleRate(double sampleRate, double bandwidth);
        void setReference(int ref);
        void setSnapInterval(double interval);
        void setBandwidthLimits(double minBandwidth, double maxBandwidth, bool bandwidthLocked);
        bool getBandwidthChanged(bool erase = true);
        double getBandwidth();
        int getReference();
        void setColor(ImU32 color);
        std::string getName();

        dsp::stream<dsp::complex_t>* output;

        friend class VFOManager;

        dsp::channel::RxVFO* dspVFO;
        ImGui::WaterfallVFO* wtfVFO;

    private:
        std::string name;
        double _bandwidth;
        VFOManager* m_manager = nullptr;
        // wtfVFO->centerOffset as last applied to the DSP, readable off the
        // UI thread by commitIQWindowOffset().
        std::atomic<double> m_dspCentre{0.0};
        // Fed by a stream of its own (VFOManager::routeVFOInput): the window
        // offset that stream carries, committed and intended, and its width.
        std::atomic<bool> m_routed{false};
        std::atomic<double> m_routedOffset{0.0};
        double m_ownWindowOffset = 0.0;  // UI thread
        double m_ownWindowWidth = 0.0;   // UI thread

    };

    VFOManager::VFO* createVFO(std::string name, int reference, double offset, double bandwidth, double sampleRate, double minBandwidth, double maxBandwidth, bool bandwidthLocked);
    void deleteVFO(VFOManager::VFO* vfo);

    void setOffset(std::string name, double offset);
    double getOffset(std::string name);
    void setCenterOffset(std::string name, double offset);
    void setBandwidth(std::string name, double bandwidth, bool updateWaterfall = true);
    void setSampleRate(std::string name, double sampleRate, double bandwidth);
    void setReference(std::string name, int ref);
    void setBandwidthLimits(std::string name, double minBandwidth, double maxBandwidth, bool bandwidthLocked);
    bool getBandwidthChanged(std::string name, bool erase = true);
    double getBandwidth(std::string name);
    void setColor(std::string name, ImU32 color);
    std::string getName();
    int getReference(std::string name);
    bool vfoExists(std::string name);

    void updateFromWaterfall(ImGui::WaterFall* wtf);

    // IQ window: the slice of the display a source's IQ stream carries,
    // centred at C + offset (display domain). Every VFO's DSP offset is
    // its centre offset minus the committed window offset. With no window
    // set the offset is 0 and the width unlimited.
    //
    // UI thread: the window the source intends. Marks and mutes the VFOs
    // whose passband is not inside it.
    void setIQWindow(double offset, double width);
    void clearIQWindow();
    // Any thread (a stream-position marker): the offset the IQ samples
    // carry from now on. Retunes every VFO's DSP.
    void commitIQWindowOffset(double offset);
    double committedIQWindowOffset() const { return m_committedOffset.load(std::memory_order_acquire); }
    // Any thread (a stream-position marker): silence every VFO for the next
    // inputSamples IQ samples, to hide a window commit's transient.
    void muteAllFor(int inputSamples);
    bool isOutOfWindow(const std::string& name) const;

    // Per-VFO input: a source feeds VFO `name` from a stream of its own
    // (IQFrontEnd::routeVFOInput), which carries a window of `width` at
    // C + offset. The VFO's DSP offset is then its centre offset minus that
    // stream's committed offset, its out-of-window mark is against that
    // window, and the primary window's commits and mutes pass it by.
    // UI thread; input nullptr restores the IQ front-end feed.
    bool routeVFOInput(const std::string& name, dsp::stream<dsp::complex_t>* input, double sampleRate,
                       double offset = 0.0, double width = 0.0);
    bool isVFORouted(const std::string& name) const;
    // UI thread: the window a routed VFO's stream is being moved to.
    void setVFOWindow(const std::string& name, double offset, double width);
    // Any thread (a marker on that stream): the offset it carries from now
    // on, and a mute for its next inputSamples samples.
    void commitVFOWindowOffset(const std::string& name, double offset);
    void muteVFOFor(const std::string& name, int inputSamples);

    Event<VFOManager::VFO*> onVfoCreated;
    Event<VFOManager::VFO*> onVfoDelete;
    Event<std::string> onVfoDeleted;

private:
    // Sets vfo's DSP offset from its waterfall centre and the committed
    // window offset; the only writer of a VFO's DSP offset.
    void applyDspOffset(VFO* vfo);
    // UI thread: recompute vfo's out-of-window mark and mute.
    void updateOutOfWindow(VFO* vfo);

    // Written by the UI thread only; m_vfoMtx guards it against the
    // off-thread commit, which is its only other reader.
    std::map<std::string, VFO*> vfos;
    // Map guard, and serialises DSP offset writes so a centre change and a
    // window commit cannot interleave into a stale sum.
    mutable std::mutex m_vfoMtx;

    std::atomic<double> m_committedOffset{0.0};
    // Intended window, UI thread only.
    bool m_windowActive = false;
    double m_windowOffset = 0.0;
    double m_windowWidth = 0.0;
};