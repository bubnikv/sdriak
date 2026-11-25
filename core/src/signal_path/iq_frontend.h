#pragma once
#include "../dsp/buffer/frame_buffer.h"
#include "../dsp/multirate/power_decimator.h"
#include "../dsp/correction/dc_blocker.h"
#include "../dsp/chain.h"
#include "../dsp/routing/splitter.h"
#include "../dsp/channel/rx_vfo.h"
#include "../dsp/sink.h"
#include "../dsp/math/conjugate.h"
#include "../dsp/buffer/fftw.h"
#include "spectrum_provider.h"
#include "spectrum_frame.h"
#include <utils/event.h>

class IQFrontEnd {
public:
    ~IQFrontEnd();

    enum FFTWindow {
        RECTANGULAR,
        HAMMING,
        HANN,
        BLACKMAN,
        NUTTALL,
        BLACKMAN_HARRIS4,
        BLACKMAN_HARRIS7
    };

    void init(dsp::stream<dsp::complex_t>* in, double sampleRate, bool buffering, int decimRatio, bool dcBlocking, int fftSize, double fftRate, FFTWindow fftWindow, float* (*acquireFFTBuffer)(void* ctx), void (*releaseFFTBuffer)(void* ctx), void* fftCtx);

    void setInput(dsp::stream<dsp::complex_t>* in);
    void setSampleRate(double sampleRate);
    inline double getSampleRate() { return _sampleRate / _decimRatio; }

    void setBuffering(bool enabled);
    void setDecimation(int ratio);
    void setInvertIQ(bool enabled);
    bool getInvertIQ() const { return _invertIQ; }
    void setDCBlocking(bool enabled);

    // UI thread. A provider supplies the waterfall instead of the local FFT:
    // fftIn is unbound from the splitter, so the FFT branch gets no samples
    // and costs nothing. FFT size, rate and window changes are stored and
    // applied when the local FFT returns. nullptr rebinds fftIn and
    // restarts the local FFT with a cleared waterfall. Either way the frame
    // queue is emptied. GUI only; see spectrum_provider.h.
    void setSpectrumProvider(SpectrumProvider* provider);
    SpectrumProvider* spectrumProvider() const { return _spectrumProvider; }

    // Provider frames. push: any thread, never blocks (SpectrumQueue).
    // pop: UI thread, only frames that are due.
    bool pushSpectrum(SpectrumFrame&& frame) { return _spectrumQueue.push(std::move(frame)); }
    bool popSpectrum(SpectrumFrame& out) { return _spectrumQueue.pop(out); }
    uint64_t droppedSpectrumFrames() const { return _spectrumQueue.dropped(); }

    // Display span set by the source (core::setDisplaySpan), or 0 when the
    // waterfall shows the effective sample rate.
    void setDisplaySpanOverride(double spanHz) { _displaySpan = spanHz; }
    double displaySpanOverride() const { return _displaySpan; }

    // The IQ carries only a narrow window of the display (offset from the
    // display centre and width, Hz), set by VFOManager::setIQWindow and
    // cleared by clearIQWindow. Consumers that want the whole band warn
    // (ImGui::NarrowIQNotice). UI thread.
    void setNarrowIQ(bool narrow, double offset = 0.0, double width = 0.0);
    bool isIQNarrow() const { return _narrowIQ; }
    double narrowIQOffset() const { return _narrowIQOffset; }
    double narrowIQWidth() const { return _narrowIQWidth; }
    Event<bool> onNarrowIQChanged;

    // Runtime insertion of external blocks (e.g. wideband noise reduction)
    // into the pre-processing chain, ahead of the FFT and all VFOs.
    void addPreprocessor(dsp::Processor<dsp::complex_t, dsp::complex_t>* processor, bool enabled);
    void removePreprocessor(dsp::Processor<dsp::complex_t, dsp::complex_t>* processor);
    void togglePreprocessor(dsp::Processor<dsp::complex_t, dsp::complex_t>* processor, bool enabled);

    void bindIQStream(dsp::stream<dsp::complex_t>* stream);
    void unbindIQStream(dsp::stream<dsp::complex_t>* stream);

    dsp::channel::RxVFO* addVFO(std::string name, double sampleRate, double bandwidth, double offset);
    void removeVFO(std::string name);
    // UI thread: feed VFO `name` from `input` at `sampleRate` (a source's
    // stream of its own for that VFO) instead of the splitter; nullptr
    // restores the splitter feed. The caller keeps `input` written until it
    // routes the VFO back. False when there is no such VFO.
    bool routeVFOInput(const std::string& name, dsp::stream<dsp::complex_t>* input, double sampleRate);
    bool isVFORouted(const std::string& name) const { return routedVFOs.count(name) != 0; }

    void setFFTSize(int size);
    void setFFTRate(double rate);
    void setFFTWindow(FFTWindow fftWindow);

    void flushInputBuffer();

    void start();
    void stop();

    double getEffectiveSamplerate();

protected:
    // The whole FFT branch on one thread. It reads fftIn straight off the
    // splitter, cuts it into frames (processFFTInput()) and runs the FFT
    // (runFFT()), with no intermediate stream.
    class FFTSink : public dsp::Sink<dsp::complex_t> {
    public:
        void init(dsp::stream<dsp::complex_t>* in, IQFrontEnd* owner) {
            _owner = owner;
            dsp::Sink<dsp::complex_t>::init(in);
        }

        int run() override;

    private:
        IQFrontEnd* _owner = nullptr;
    };

    void processFFTInput(const dsp::complex_t* data, int count);
    void runFFT();
    // newFFTSize > 0 replaces _fftSize, which is committed at the same point as
    // the buffers and the plan built for it -- see the definition.
    void updateFFTPath(bool updateWaterfall = false, int newFFTSize = -1);
    void genFFTWindow(float* buf, int size);

    static inline double genDCBlockRate(double sampleRate) {
        return 50.0 / sampleRate;
    }

    // One frame every round(sampleRate / rate) samples: the first nzSampCount
    // of them (at most the FFT size; the rest of the FFT is zero-padded) are
    // transformed and the next `skip` dropped. Frames never overlap.
    static inline void genFrameParams(double sampleRate, int size, double rate, int& skip, int& nzSampCount) {
        int fftInterval = round(sampleRate / rate);
        nzSampCount = std::min<int>(fftInterval, size);
        skip = fftInterval - nzSampCount;
    }

    // Input buffer
    dsp::buffer::SampleFrameBuffer<dsp::complex_t> inBuf;

    // Pre-processing chain
    dsp::multirate::PowerDecimatorBlock<dsp::complex_t> decim;
    dsp::math::Conjugate conjugate;
    dsp::correction::DCBlocker<dsp::complex_t> dcBlock;
    dsp::chain<dsp::complex_t> preproc;

    // Splitting
    dsp::routing::Splitter<dsp::complex_t> split;

    // FFT
    dsp::stream<dsp::complex_t> fftIn;
    FFTSink fftSink;

    // VFOs
    std::map<std::string, dsp::stream<dsp::complex_t>*> vfoStreams;
    std::map<std::string, dsp::channel::RxVFO*> vfos;
    // VFOs fed by routeVFOInput(), with their input rate; their own input
    // stream is unbound from the splitter meanwhile.
    std::map<std::string, double> routedVFOs;

    // Parameters
    double _sampleRate;
    double _decimRatio;
    int _fftSize;
    double _fftRate;
    FFTWindow _fftWindow;
    float* (*_acquireFFTBuffer)(void* ctx);
    void (*_releaseFFTBuffer)(void* ctx);
    void* _fftCtx;

    // Processing data. Framing state, touched only by fftSink's thread and by
    // updateFFTPath() with that thread stopped: samples of the current frame
    // already windowed into fftInBuf, and samples still to drop before the
    // next frame starts.
    int _fftSkip = 0;
    int _fftFill = 0;
    int _fftSkipLeft = 0;
    int _nzFFTSize;
    dsp::buffer::Buffer<float> fftWindowBuf;
    dsp::buffer::FFTWBuffer<fftwf_complex> fftInBuf, fftOutBuf;
    dsp::buffer::FFTWPlan fftwPlan;
    float* fftDbOut = NULL;

    double effectiveSr;

    bool _invertIQ = false;
    SpectrumProvider* _spectrumProvider = nullptr;
    SpectrumQueue _spectrumQueue;
    // FFT size requested while a provider is active, or -1.
    int _pendingFFTSize = -1;
    double _displaySpan = 0.0;
    bool _narrowIQ = false;
    double _narrowIQOffset = 0.0;
    double _narrowIQWidth = 0.0;

    bool _init = false;

};