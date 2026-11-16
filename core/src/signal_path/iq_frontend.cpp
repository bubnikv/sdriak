#include "iq_frontend.h"
#include "../dsp/window/rectangular.h"
#include "../dsp/window/hamming.h"
#include "../dsp/window/hann.h"
#include "../dsp/window/blackman.h"
#include "../dsp/window/nuttall.h"
#include "../dsp/window/blackman_harris.h"
#include "../dsp/window/blackman_harris7.h"
#include <utils/flog.h>
#include <gui/gui.h>
#include <core.h>
#include <memory>
#include <vector>

namespace {
    // dsp::block::TempStop for a set of blocks at once. TempStop is neither
    // copyable nor movable, so it cannot live in a container; this keeps the
    // same guarantee for a run-time-sized group.
    //
    // Restart is in reverse order, and only for the blocks actually stopped --
    // tempStop() joins a worker thread and can in principle throw, and half a
    // group left stopped is the failure mode this exists to prevent.
    class TempStopGroup {
    public:
        explicit TempStopGroup(std::vector<dsp::block*> blocks) : _blocks(std::move(blocks)) {
            try {
                for (; _stopped < _blocks.size(); ++_stopped) { _blocks[_stopped]->tempStop(); }
            }
            catch (...) {
                // No destructor runs for an object whose constructor threw, so
                // the blocks stopped so far have to be restarted here -- this
                // is the half-stopped group the class exists to prevent.
                restartStopped();
                throw;
            }
        }

        ~TempStopGroup() { restartStopped(); }

        TempStopGroup(const TempStopGroup&) = delete;
        TempStopGroup& operator=(const TempStopGroup&) = delete;

    private:
        void restartStopped() noexcept {
            while (_stopped > 0) {
                --_stopped;
                // Must not throw: this also runs from a destructor, and from
                // the constructor's unwind path. See dsp::block::TempStop.
                try { _blocks[_stopped]->tempStart(); }
                catch (const std::exception& e) {
                    flog::error("Failed to restart a DSP block after reconfiguration: {0}", e.what());
                }
                catch (...) {
                    flog::error("Failed to restart a DSP block after reconfiguration");
                }
            }
        }

        std::vector<dsp::block*> _blocks;
        size_t _stopped = 0;
    };
}

IQFrontEnd::~IQFrontEnd() {
    // Everything else is released by its owner, including after an init() that
    // threw part-way -- member destructors run regardless of _init.
    if (_init) { stop(); }
}

void IQFrontEnd::init(dsp::stream<dsp::complex_t>* in, double sampleRate, bool buffering, int decimRatio, bool dcBlocking, int fftSize, double fftRate, FFTWindow fftWindow, float* (*acquireFFTBuffer)(void* ctx), void (*releaseFFTBuffer)(void* ctx), void* fftCtx) {
    _sampleRate = sampleRate;
    _decimRatio = decimRatio;
    _fftSize = fftSize;
    _fftRate = fftRate;
    _fftWindow = fftWindow;
    _acquireFFTBuffer = acquireFFTBuffer;
    _releaseFFTBuffer = releaseFFTBuffer;
    _fftCtx = fftCtx;

    effectiveSr = _sampleRate / _decimRatio;

    inBuf.init(in);
    inBuf.bypass = !buffering;

    decim.init(NULL, _decimRatio);
    dcBlock.init(NULL, genDCBlockRate(effectiveSr));
    conjugate.init(NULL);

    preproc.init(&inBuf.out);
    preproc.addBlock(&decim, _decimRatio > 1);
    preproc.addBlock(&dcBlock, dcBlocking);
    preproc.addBlock(&conjugate, false); // TODO: Replace by parameter

    split.init(preproc.out);

    // TODO: Do something to avoid basically repeating this code twice
    genFrameParams(effectiveSr, _fftSize, _fftRate, _fftSkip, _nzFFTSize);
    _fftFill = 0;
    _fftSkipLeft = 0;
    fftSink.init(&fftIn, this);

    fftWindowBuf.allocate(_nzFFTSize);
    genFFTWindow(fftWindowBuf, _nzFFTSize);

    fftInBuf.allocate(_fftSize);
    fftOutBuf.allocate(_fftSize);
    fftwPlan.planDFT1D(_fftSize, fftInBuf, fftOutBuf, FFTW_FORWARD);

    // Clear the rest of the FFT input buffer
    dsp::buffer::clear<fftwf_complex>(fftInBuf, _fftSize - _nzFFTSize, _nzFFTSize);

    split.bindStream(&fftIn);

    _init = true;
}

void IQFrontEnd::setInput(dsp::stream<dsp::complex_t>* in) {
    inBuf.setInput(in);
}

void IQFrontEnd::setSampleRate(double sampleRate) {
    // Temp stop the necessary blocks. Everything below this point allocates --
    // the VFOs rebuild resampler banks and filter delay lines, updateFFTPath()
    // rebuilds the window, the FFT buffers and the plan -- so the restart has
    // to be a scope guard. Bare tempStart() calls at the end of the function
    // are skipped by a throw, which leaves the DC blocker and every VFO stopped
    // for the life of the process.
    std::vector<dsp::block*> blocks;
    blocks.reserve(vfos.size() + 1);
    blocks.push_back(&dcBlock);
    for (auto& [name, vfo] : vfos) { blocks.push_back(vfo); }
    TempStopGroup guard(std::move(blocks));

    // Update the samplerate
    _sampleRate = sampleRate;
    effectiveSr = _sampleRate / _decimRatio;
    dcBlock.setRate(genDCBlockRate(effectiveSr));
    for (auto& [name, vfo] : vfos) {
        vfo->setInSamplerate(effectiveSr);
    }

    // Reconfigure the FFT
    updateFFTPath();
}

void IQFrontEnd::setBuffering(bool enabled) {
    inBuf.bypass = !enabled;
}

void IQFrontEnd::setDecimation(int ratio) {
    {
        // Scoped rather than function-scoped so the decimator is running again
        // before the chain is re-linked below, as it was before. setRatio()
        // rebuilds the decimation stages and can throw.
        dsp::block::TempStop guard(&decim);

        // Update the decimation ratio
        _decimRatio = ratio;
        if (_decimRatio > 1) { decim.setRatio(_decimRatio); }
        setSampleRate(_sampleRate);
    }

    // Enable or disable in the chain
    preproc.setBlockEnabled(&decim, _decimRatio > 1, [=](dsp::stream<dsp::complex_t>* out){ split.setInput(out); });

    // Update the DSP sample rate (TODO: Find a way to get rid of this)
    core::setInputSampleRate(_sampleRate);
}

void IQFrontEnd::setDCBlocking(bool enabled) {
    preproc.setBlockEnabled(&dcBlock, enabled, [=](dsp::stream<dsp::complex_t>* out){ split.setInput(out); });
}

void IQFrontEnd::setInvertIQ(bool enabled) {
    preproc.setBlockEnabled(&conjugate, enabled, [=](dsp::stream<dsp::complex_t>* out){ split.setInput(out); });
}

void IQFrontEnd::addPreprocessor(dsp::Processor<dsp::complex_t, dsp::complex_t>* processor, bool enabled) {
    preproc.addBlock(processor, enabled);
    split.setInput(preproc.out);
}

void IQFrontEnd::removePreprocessor(dsp::Processor<dsp::complex_t, dsp::complex_t>* processor) {
    preproc.removeBlock(processor, [=](dsp::stream<dsp::complex_t>* out) { split.setInput(out); });
}

void IQFrontEnd::togglePreprocessor(dsp::Processor<dsp::complex_t, dsp::complex_t>* processor, bool enabled) {
    if (enabled) {
        preproc.enableBlock(processor, [=](dsp::stream<dsp::complex_t>* out) { split.setInput(out); });
    }
    else {
        preproc.disableBlock(processor, [=](dsp::stream<dsp::complex_t>* out) { split.setInput(out); });
    }
}

void IQFrontEnd::bindIQStream(dsp::stream<dsp::complex_t>* stream) {
    split.bindStream(stream);
}

void IQFrontEnd::unbindIQStream(dsp::stream<dsp::complex_t>* stream) {
    split.unbindStream(stream);
}

dsp::channel::RxVFO* IQFrontEnd::addVFO(std::string name, double sampleRate, double bandwidth, double offset) {
    // Make sure no other VFO with that name already exists
    if (vfos.find(name) != vfos.end()) {
        flog::error("[IQFrontEnd] Tried to add VFO with existing name.");
        return NULL;
    }

    // Create VFO and its input stream. Both are held by unique_ptr until every
    // step that can throw has succeeded: the stream constructor allocates, the
    // RxVFO constructor allocates a resampler bank and two delay lines, and the
    // map insertions allocate nodes. Releasing early leaks whatever was built
    // before the throw, and leaves the maps referring to objects nobody owns.
    auto vfoIn = std::make_unique<dsp::stream<dsp::complex_t>>();
    auto vfo = std::make_unique<dsp::channel::RxVFO>(vfoIn.get(), effectiveSr, sampleRate, bandwidth, offset);

    // Reserve both map slots before either is filled, so the second insertion
    // cannot throw after the first has committed.
    vfoStreams[name] = NULL;
    try {
        vfos[name] = NULL;
    }
    catch (...) {
        vfoStreams.erase(name);
        throw;
    }

    // Register them. Assigning to a key that already exists cannot throw.
    dsp::channel::RxVFO* raw = vfo.get();
    dsp::stream<dsp::complex_t>* rawIn = vfoIn.get();
    vfoStreams[name] = rawIn;
    vfos[name] = raw;

    // The maps hold raw pointers, but ownership stays with the unique_ptrs
    // until start() has succeeded. bindIQStream() and start() both allocate,
    // and the caller that catches their throw never learns the name got
    // registered -- nobody would ever call removeVFO() for it, leaving an
    // unbound VFO that IQFrontEnd::start() still spawns a worker for and that
    // no destructor deletes.
    bool bound = false;
    try {
        bindIQStream(rawIn);
        bound = true;

        // Start VFO
        raw->start();
    }
    catch (...) {
        if (bound) {
            try { unbindIQStream(rawIn); }
            catch (...) { flog::error("[IQFrontEnd] Failed to unbind the VFO stream while unwinding addVFO()"); }
        }
        // Erasing a key that is present cannot throw. The unique_ptrs still own
        // both objects, so they are released as this propagates.
        vfos.erase(name);
        vfoStreams.erase(name);
        throw;
    }

    vfoIn.release();
    vfo.release();
    return raw;
}

void IQFrontEnd::removeVFO(std::string name) {
    // Make sure that a VFO with that name exists
    if (vfos.find(name) == vfos.end()) {
        flog::error("[IQFrontEnd] Tried to remove a VFO that doesn't exist.");
        return;
    }

    // Remove the VFO and stream from registry
    dsp::stream<dsp::complex_t>* vfoIn = vfoStreams[name];
    dsp::channel::RxVFO* vfo = vfos[name];

    // Stop the VFO
    vfo->stop();

    unbindIQStream(vfoIn);
    vfoStreams.erase(name);
    vfos.erase(name);

    // Delete the VFO and its input stream
    delete vfo;
    delete vfoIn;
}

void IQFrontEnd::setFFTSize(int size) {
    // Handed down rather than committed here: _fftSize sizes the FFT buffers,
    // the plan and the waterfall's raw FFT, none of which exist yet.
    updateFFTPath(true, size);
}

void IQFrontEnd::setFFTRate(double rate) {
    _fftRate = rate;
    updateFFTPath();
}

void IQFrontEnd::setFFTWindow(FFTWindow fftWindow) {
    _fftWindow = fftWindow;
    updateFFTPath();
}

void IQFrontEnd::flushInputBuffer() {
    inBuf.flush();
}

void IQFrontEnd::start() {
    // Start input buffer
    inBuf.start();

    // Start pre-proc chain (automatically start all bound blocks)
    preproc.start();

    // Start IQ splitter
    split.start();

    // Start all VFOs
    for (auto& [name, vfo] : vfos) {
        vfo->start();
    }

    // Start FFT chain
    fftSink.start();
}

void IQFrontEnd::stop() {
    // Stop input buffer
    inBuf.stop();

    // Stop pre-proc chain (automatically start all bound blocks)
    preproc.stop();

    // Stop IQ splitter
    split.stop();

    // Stop all VFOs
    for (auto& [name, vfo] : vfos) {
        vfo->stop();
    }

    // Stop FFT chain
    fftSink.stop();
}

double IQFrontEnd::getEffectiveSamplerate() {
    return effectiveSr;
}

int IQFrontEnd::FFTSink::run() {
    int count = _in->read();
    if (count < 0) { return -1; }
    _owner->processFFTInput(_in->readBuf, count);
    _in->flush();
    return count;
}

void IQFrontEnd::processFFTInput(const dsp::complex_t* data, int count) {
    // An FFT rate above the sample rate rounds the frame interval to zero;
    // there is no frame to fill, and the loop below would never advance.
    if (_nzFFTSize <= 0) { return; }

    while (count > 0) {
        // Drop the samples between frames. No copy: at a high sample rate and
        // the default 20 fps this is nearly all of the input.
        if (_fftSkipLeft > 0) {
            int n = std::min<int>(count, _fftSkipLeft);
            _fftSkipLeft -= n;
            data += n;
            count -= n;
            continue;
        }

        // Window straight into the FFT input -- the only copy of the samples
        // on this path. A frame may span any number of input blocks.
        int n = std::min<int>(count, _nzFFTSize - _fftFill);
        volk_32fc_32f_multiply_32fc((lv_32fc_t*)(fftInBuf.get() + _fftFill), (const lv_32fc_t*)data, fftWindowBuf.get() + _fftFill, n);
        _fftFill += n;
        data += n;
        count -= n;

        if (_fftFill == _nzFFTSize) {
            runFFT();
            _fftFill = 0;
            _fftSkipLeft = _fftSkip;
        }
    }
}

void IQFrontEnd::runFFT() {
    // Execute FFT. fftInBuf past _nzFFTSize stays zero (see updateFFTPath()).
    fftwf_execute(fftwPlan);

    // Aquire buffer
    float* fftBuf = _acquireFFTBuffer(_fftCtx);

    // Convert the complex output of the FFT to dB amplitude. The window is
    // normalized to unity coherent gain, so no 1/N normalization here.
    if (fftBuf) {
        volk_32fc_s32f_power_spectrum_32f(fftBuf, (lv_32fc_t*)fftOutBuf.get(), 1.0f, _fftSize);
    }

    // Release buffer
    _releaseFFTBuffer(_fftCtx);
}

void IQFrontEnd::genFFTWindow(float* buf, int size) {
    double sum = 0.0;
    for (int i = 0; i < size; i++) {
        double w = 1.0;
        switch (_fftWindow) {
            case FFTWindow::RECTANGULAR:      w = dsp::window::rectangular(i, size); break;
            case FFTWindow::HAMMING:          w = dsp::window::hamming(i, size); break;
            case FFTWindow::HANN:             w = dsp::window::hann(i, size); break;
            case FFTWindow::BLACKMAN:         w = dsp::window::blackman(i, size); break;
            case FFTWindow::NUTTALL:          w = dsp::window::nuttall(i, size); break;
            case FFTWindow::BLACKMAN_HARRIS4: w = dsp::window::blackmanHarris(i, size); break;
            case FFTWindow::BLACKMAN_HARRIS7: w = dsp::window::blackmanHarris7(i, size); break;
        }
        buf[i] = w;
        sum += w;
    }

    // Normalize to unity coherent gain (window sum = 1): a coherent sinusoid
    // then reads its true dBFS amplitude with any window and any FFT zero
    // padding. The (-1)^n factor rotating DC to the center of the FFT output
    // is folded into the window.
    float scale = (sum > 0.0) ? (float)(1.0 / sum) : 1.0f;
    for (int i = 0; i < size; i++) {
        buf[i] *= (i % 2) ? -scale : scale;
    }
}

void IQFrontEnd::updateFFTPath(bool updateWaterfall, int newFFTSize) {
    // Temp stop branch. Scope guards, not bare tempStop()/tempStart(): the
    // reallocations below throw on OOM, and a throw past a bare tempStart()
    // leaves the FFT path stopped for good -- a frozen waterfall on top of
    // whatever the caller does with the exception. fftSink is the whole branch,
    // so stopping it covers everything below.
    dsp::block::TempStop fftSinkGuard(&fftSink);

    // The new geometry goes into locals. Everything the FFT thread reads --
    // _fftSize, _nzFFTSize, _fftSkip, the window, the FFT buffers and the plan
    // -- describes one consistent configuration, so none of it may be
    // published until all of it can be.
    const int fftSize = (newFFTSize > 0) ? newFFTSize : _fftSize;
    int skip, nzFFTSize;
    genFrameParams(effectiveSr, fftSize, _fftRate, skip, nzFFTSize);

    // Update window. Built as a local and committed below with the FFT buffers:
    // allocate() alone would leave the *old*, possibly smaller window in place
    // against a new _nzFFTSize, and processFFTInput() reads up to _nzFFTSize
    // floats out of it.
    dsp::buffer::Buffer<float> newWindow(nzFFTSize);
    genFFTWindow(newWindow, nzFFTSize);

    // Update FFT plan. Re-planning is mandatory, not housekeeping: a plan is
    // bound to the alignment of the arrays it was built against, so it cannot
    // outlive a reallocation of them.
    //
    // Both buffers and the plan are built as locals and committed together.
    // Replacing the members one at a time releases the storage the *existing*
    // plan points at, so a throw from the second allocation or from the planner
    // would leave a live plan referencing freed memory -- and the guards above
    // then restart the FFT worker straight into fftwf_execute() on it.
    //
    // Moving preserves the pointers, so the plan stays bound to the arrays it
    // was created against. Commit the plan first: it releases the old plan
    // before the arrays it referenced go away.
    dsp::buffer::FFTWBuffer<fftwf_complex> newInBuf(fftSize);
    dsp::buffer::FFTWBuffer<fftwf_complex> newOutBuf(fftSize);
    dsp::buffer::FFTWPlan newPlan;
    newPlan.planDFT1D(fftSize, newInBuf, newOutBuf, FFTW_FORWARD);

    // Commit point: nothing above this line has been published, and nothing
    // below it can throw -- plain assignments and four noexcept moves.
    _fftSize = fftSize;
    _nzFFTSize = nzFFTSize;
    _fftSkip = skip;
    fftWindowBuf = std::move(newWindow);
    fftwPlan = std::move(newPlan);
    fftInBuf = std::move(newInBuf);
    fftOutBuf = std::move(newOutBuf);

    // The frame in progress was windowed into the buffer just replaced, for
    // the old geometry: start the next one from scratch.
    _fftFill = 0;
    _fftSkipLeft = 0;

    // Clear the rest of the FFT input buffer
    dsp::buffer::clear<fftwf_complex>(fftInBuf, _fftSize - _nzFFTSize, _nzFFTSize);

    // Update waterfall (TODO: This is annoying, it makes this module non testable and will constantly clear the waterfall for any reason)
    if (updateWaterfall) { gui::waterfall.setRawFFTSize(_fftSize); }

    // The branch is restarted by the guards above, on the way out.
}