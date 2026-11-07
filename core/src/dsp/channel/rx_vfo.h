#pragma once
#include <assert.h>
#include <cmath>
#include <algorithm>
#include <mutex>
#include <utility>
#include "frequency_xlator.h"
#include "../multirate/rational_resampler.h"
#include "../filter/fir.h"
#include "../taps/low_pass.h"

namespace dsp::channel {
    // Computation kernel: xlator -> resampler -> filter, all three of which are
    // kernels too, so the whole chain runs in place on the caller's buffer with
    // no output stream, worker or mutex anywhere inside it. Used directly by the
    // composites that fuse a VFO (CTCSSSquelch, the VOR receiver).
    //
    // The threaded VFO that IQFrontEnd hands out is RxVFO below. Note the naming
    // is inverted relative to FIRBlock/FastAGCBlock: RxVFO* is public API
    // (vfo_manager.h, iq_frontend.h) and has to keep its name.
    //
    // The caller must serialise all setters against each other, and must stop
    // the containing block's worker before calling any of them -- except
    // setOffset(), which only publishes an atomic, and setBandwidth(), which
    // takes filterMtx because the UI calls it while the worker is running.
    class RxVFOKernel {
    public:
        RxVFOKernel() {}

        RxVFOKernel(double inSamplerate, double outSamplerate, double bandwidth, double offset, int maxBlock = STREAM_BUFFER_SIZE) {
            init(inSamplerate, outSamplerate, bandwidth, offset, maxBlock);
        }

        void init(double inSamplerate, double outSamplerate, double bandwidth, double offset, int maxBlock = STREAM_BUFFER_SIZE) {
            _maxBlock = maxBlock;
            _inSamplerate = inSamplerate;
            _outSamplerate = outSamplerate;
            _bandwidth = bandwidth;
            _offset = offset;
            filterNeeded = (_bandwidth != _outSamplerate);

            xlator.init(-_offset, _inSamplerate);
            // The xlator and the resampler see a full block; the filter runs
            // *after* the resampler and only ever sees what it emits.
            resamp.init(_inSamplerate, _outSamplerate, _maxBlock);
            OwnedTap<float> newTaps = generateTaps();
            // [RATE-SIZED] see doc/design/buffer-size-requirements.md 2.6
            filter.init(newTaps, filterMaxBlock());
            ftaps = std::move(newTaps);

            _init = true;
        }

        void setInSamplerate(double inSamplerate) {
            assert(_init);
            _inSamplerate = inSamplerate;
            xlator.setOffset(-_offset, _inSamplerate);
            resamp.setInSamplerate(_inSamplerate);
            // [RATE-SIZED] The decimation ratio moved, so the filter's block
            // bound moved with it. Safe to resize here: the caller has stopped
            // the worker, and unlike a stream this delay line has no external
            // reader. see doc/design/buffer-size-requirements.md 2.6
            if (filterNeeded) { filter.setTaps(ftaps, filterMaxBlock()); }
        }

        void setOutSamplerate(double outSamplerate, double bandwidth) {
            assert(_init);
            _outSamplerate = outSamplerate;
            _bandwidth = bandwidth;
            filterNeeded = (_bandwidth != _outSamplerate);
            resamp.setOutSamplerate(_outSamplerate);
            if (filterNeeded) {
                // Local until the FIR owns it -- see generateTaps().
                OwnedTap<float> newTaps = generateTaps();
                // [RATE-SIZED] see doc/design/buffer-size-requirements.md 2.6
                filter.setTaps(newTaps, filterMaxBlock());
                ftaps = std::move(newTaps);
            }
        }

        void setBandwidth(double bandwidth) {
            assert(_init);
            std::lock_guard<std::mutex> lck(filterMtx);
            _bandwidth = bandwidth;
            filterNeeded = (_bandwidth != _outSamplerate);
            if (filterNeeded) {
                // Local until the FIR owns it -- see generateTaps(). This path
                // never resizes, so setTaps() cannot throw, but keeping the
                // shape uniform costs nothing.
                OwnedTap<float> newTaps = generateTaps();
                filter.setTaps(newTaps);
                ftaps = std::move(newTaps);
            }
        }

        void setOffset(double offset) {
            assert(_init);
            _offset = offset;
            xlator.setOffset(-_offset, _inSamplerate);
        }

        void reset() {
            assert(_init);
            xlator.reset();
            resamp.reset();
            filter.reset();
        }

        inline int process(int count, const complex_t* in, complex_t* out) {
            xlator.process(count, in, out);
            count = resamp.process(count, out, out);
            // filterNeeded is written by setBandwidth() from the UI while this
            // worker runs, so it has to be read under the same mutex that
            // guards the taps it selects.
            {
                std::lock_guard<std::mutex> lck(filterMtx);
                if (filterNeeded) { filter.process(count, out, out); }
            }
            return count;
        }

    protected:
        // Returned rather than assigned straight into ftaps: the assignment
        // frees the array filter._taps still points at, and setTaps() allocates
        // before it publishes the replacement. Callers keep the new set local
        // until the FIR has adopted it, then move it in.
        OwnedTap<float> generateTaps() const {
            double filterWidth = _bandwidth / 2.0;
            return taps::lowPass(filterWidth, filterWidth * 0.1, _outSamplerate);
        }

        // Largest block the filter can be handed: what the resampler emits for
        // a full input block. Not clamped down when the VFO interpolates
        // (outSamplerate > inSamplerate) -- under-sizing would overflow, and
        // nothing rejects that configuration today. + 64 for the resampler's
        // retained phase state.
        // [RATE-SIZED] ASSUMED ratio: _outSamplerate / _inSamplerate, both of
        // which the owner can change at runtime. The filter's delay line is
        // sized from this, and process() writes count samples into it with no
        // bound check -- a wrong assumption here is a heap overflow, not a
        // glitch. see doc/design/buffer-size-requirements.md 2.6
        int filterMaxBlock() const {
            if (!(_inSamplerate > 0.0) || _maxBlock <= 0) { return _maxBlock; }
            double n = std::ceil((double)_maxBlock * _outSamplerate / _inSamplerate) + 64.0;
            return (int)(std::max)(1.0, n);
        }

        bool _init = false;
        int _maxBlock = STREAM_BUFFER_SIZE;

        FrequencyXlator xlator;
        multirate::RationalResampler<complex_t> resamp;
        filter::FIR<complex_t, float> filter;
        OwnedTap<float> ftaps;
        // Guarded by filterMtx: written by setBandwidth() from the UI thread,
        // read by process() on the worker.
        bool filterNeeded = false;

        double _inSamplerate = 0.0;
        double _outSamplerate = 0.0;
        double _bandwidth = 0.0;
        double _offset = 0.0;

        std::mutex filterMtx;
    };

    // The threaded VFO handed out by IQFrontEnd. Owns the output stream and the
    // worker; all the signal processing lives in the kernel.
    class RxVFO : public Processor<complex_t, complex_t> {
        using base_type = Processor<complex_t, complex_t>;
    public:
        RxVFO() {}

        RxVFO(stream<complex_t>* in, double inSamplerate, double outSamplerate, double bandwidth, double offset, int maxBlock = STREAM_BUFFER_SIZE) {
            init(in, inSamplerate, outSamplerate, bandwidth, offset, maxBlock);
        }

        ~RxVFO() {
            // The owning members below are destroyed before ~block() runs, so
            // the worker must be joined here -- otherwise it keeps reading
            // buffers that have already been freed.
            if (!base_type::_block_init) { return; }
            base_type::stop();
        }

        void init(stream<complex_t>* in, double inSamplerate, double outSamplerate, double bandwidth, double offset, int maxBlock = STREAM_BUFFER_SIZE) {
            kernel.init(inSamplerate, outSamplerate, bandwidth, offset, maxBlock);
            base_type::init(in);
        }

        void setInSamplerate(double inSamplerate) { reconf([&]{ kernel.setInSamplerate(inSamplerate); }); }
        void setOutSamplerate(double outSamplerate, double bandwidth) { reconf([&]{ kernel.setOutSamplerate(outSamplerate, bandwidth); }); }
        void reset() { reconf([&]{ kernel.reset(); }); }

        // Both of these run while the worker runs. They are on the tuning path
        // (waterfall drag, rigctl, source retune) and stopping the worker there
        // stalls the whole IQ chain at mouse-event rate: the upstream splitter
        // blocks in swap() until the VFO restarts, and a worker that exits
        // between read() and flush() reprocesses the same block afterwards.
        // setOffset() is a lock-free atomic publish into the xlator;
        // setBandwidth() takes filterMtx in the kernel. ctrlMtx still serialises
        // both against the reconf() setters, which touch the same offset and
        // taps without filterMtx.
        void setOffset(double offset) {
            assert(base_type::_block_init);
            std::lock_guard<std::recursive_mutex> lck(base_type::ctrlMtx);
            kernel.setOffset(offset);
        }

        void setBandwidth(double bandwidth) {
            assert(base_type::_block_init);
            std::lock_guard<std::recursive_mutex> lck(base_type::ctrlMtx);
            kernel.setBandwidth(bandwidth);
        }

        inline int process(int count, const complex_t* in, complex_t* out) {
            return kernel.process(count, in, out);
        }

        int run() {
            int count = base_type::_in->read();
            if (count < 0) { return -1; }

            int outCount = kernel.process(count, base_type::_in->readBuf, out.writeBuf);

            // Swap if some data was generated
            base_type::_in->flush();
            if (outCount) {
                if (!out.swap(outCount)) { return -1; }
            }
            return outCount;
        }

        RxVFOKernel kernel;

    private:
        template<class Func>
        void reconf(Func f) {
            assert(base_type::_block_init);
            std::lock_guard<std::recursive_mutex> lck(base_type::ctrlMtx);
            dsp::block::TempStop guard(this);
            f();
        }
    };
}
