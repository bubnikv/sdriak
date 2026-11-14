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
    int skip;
    genReshapeParams(effectiveSr, _fftSize, _fftRate, skip, _nzFFTSize);
    // `out` carries exactly one reshaped block, which updateFFTPath() then
    // narrows to _nzFFTSize -- a fraction of the default stream in every
    // configuration.
    reshape.init(&fftIn, fftSize, skip, fftSize);
    fftSink.init(&reshape.out, handler, this);

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
    reshape.start();
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
    reshape.stop();
    fftSink.stop();
}

double IQFrontEnd::getEffectiveSamplerate() {
    return effectiveSr;
}

void IQFrontEnd::handler(dsp::complex_t* data, int count, void* ctx) {
    IQFrontEnd* _this = (IQFrontEnd*)ctx;

    // Apply window
    volk_32fc_32f_multiply_32fc((lv_32fc_t*)_this->fftInBuf.get(), (lv_32fc_t*)data, _this->fftWindowBuf, _this->_nzFFTSize);

    // Execute FFT
    fftwf_execute(_this->fftwPlan);

    // Aquire buffer
    float* fftBuf = _this->_acquireFFTBuffer(_this->_fftCtx);

    // Convert the complex output of the FFT to dB amplitude. The window is
    // normalized to unity coherent gain, so no 1/N normalization here.
    if (fftBuf) {
        volk_32fc_s32f_power_spectrum_32f(fftBuf, (lv_32fc_t*)_this->fftOutBuf.get(), 1.0f, _this->_fftSize);
    }

    // Release buffer
    _this->_releaseFFTBuffer(_this->_fftCtx);
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
    // whatever the caller does with the exception. The guards unwind in reverse,
    // restarting the sink before the reshaper that feeds it.
    dsp::block::TempStop reshapeGuard(&reshape);
    dsp::block::TempStop fftSinkGuard(&fftSink);

    // The new geometry goes into locals. Everything handler() reads -- _fftSize,
    // _nzFFTSize, the reshaper's keep and skip, the window, the FFT buffers and
    // the plan -- describes one consistent configuration, so none of it may be
    // published until all of it can be.
    const int fftSize = (newFFTSize > 0) ? newFFTSize : _fftSize;
    int skip, nzFFTSize;
    genReshapeParams(effectiveSr, fftSize, _fftRate, skip, nzFFTSize);

    // The exception: `out` has to be grown before setKeep() may ask for the
    // larger block, and growing it allocates. Only the *bound* moves here --
    // the reshaper still emits the old keep -- so a throw below leaves nothing
    // behind but a roomier output stream. The opposite direction is the mirror
    // image and waits until after the commit, below. Both guards above are
    // held, so the FFT sink that reads reshape.out is joined, which is the
    // other precondition setKeepBound() states.
    if (nzFFTSize > reshape.getKeepBound()) { reshape.setKeepBound(nzFFTSize); }

    // Update window. Built as a local and committed below with the FFT buffers:
    // allocate() alone would leave the *old*, possibly smaller window in place
    // against a new _nzFFTSize, and handler() reads _nzFFTSize floats out of it.
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
    // below it can throw. setKeep()/setSkip() only assign -- the TempStop each
    // takes is a no-op under the guards already held here -- and the four moves
    // are noexcept.
    _fftSize = fftSize;
    _nzFFTSize = nzFFTSize;
    reshape.setKeep(_nzFFTSize);
    reshape.setSkip(skip);
    fftWindowBuf = std::move(newWindow);
    fftwPlan = std::move(newPlan);
    fftInBuf = std::move(newInBuf);
    fftOutBuf = std::move(newOutBuf);

    // Now that _keep has fallen, `out` can give the excess back -- a zero-padded
    // FFT keeps a small fraction of each block, so this is most of the stream.
    // It allocates, hence its place below the commit point: the configuration
    // above is already coherent, and a failure here only leaves a buffer larger
    // than it needs to be, which is why it is logged rather than propagated.
    if (_nzFFTSize < reshape.getKeepBound()) {
        try { reshape.setKeepBound(_nzFFTSize); }
        catch (const std::exception& e) {
            flog::warn("[IQFrontEnd] Could not shrink the reshaper output to {0}: {1}", _nzFFTSize, e.what());
        }
    }

    // Clear the rest of the FFT input buffer
    dsp::buffer::clear<fftwf_complex>(fftInBuf, _fftSize - _nzFFTSize, _nzFFTSize);

    // Update waterfall (TODO: This is annoying, it makes this module non testable and will constantly clear the waterfall for any reason)
    if (updateWaterfall) { gui::waterfall.setRawFFTSize(_fftSize); }

    // The branch is restarted by the guards above, on the way out.
}