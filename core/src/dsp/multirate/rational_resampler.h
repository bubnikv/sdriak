#pragma once
#include <vector>
#include <numeric>
#include <cstdint>
#include <cmath>
#include <limits>
#include <stdexcept>
#include "../../utils/flog.h"
#include "../processor.h"
#include "../filter/decimating_fir.h"
#include "../taps/from_array.h"
#include "polyphase_resampler.h"
#include "power_decimator.h"
#include "../taps/low_pass.h"
#include "../window/nuttall.h"

namespace dsp::multirate {
    // Computation kernel, not a dsp::block. Fused inside RxVFO, BroadcastFM and
    // the RyFi transmitter; the real chain nodes (radio afChain, M17) use
    // RationalResamplerBlock below. See dsp::math::Delay for why.
    template<class T>
    class RationalResampler {
    public:
        RationalResampler() {}

        RationalResampler(double inSamplerate, double outSamplerate, int maxBlock = STREAM_BUFFER_SIZE) {
            init(inSamplerate, outSamplerate, maxBlock);
        }

        void init(double inSamplerate, double outSamplerate, int maxBlock = STREAM_BUFFER_SIZE) {
            _inSamplerate = inSamplerate;
            _outSamplerate = outSamplerate;
            _maxBlock = maxBlock;
            
            // Dummy initialization since only used for processing
            decim.init(2, _maxBlock);
            resamp.init(1, 1, taps::lowPass(0.25, 0.1, 1.0), _maxBlock);

            // Proper configuration
            reconfigure();

            _init = true;
        }

        void reset() {
            assert(_init);
            decim.reset();
            resamp.reset();
        }

        // Largest block process() can emit for the maxBlock supplied to init().
        // This is derived from the ratios the resampler actually selected, not
        // from the requested rates: MAX_INTERP approximation can round upward
        // or even select passthrough for a near-unity ratio.
        int maxOutputBlock() const {
            assert(_init);
            return _maxOutputBlock;
        }

        void setInSamplerate(double inSamplerate) {
            assert(_init);
            _inSamplerate = inSamplerate;
            reconfigure();
        }

        void setOutSamplerate(double outSamplerate) {
            assert(_init);
            _outSamplerate = outSamplerate;
            reconfigure();
        }

        void setRates(double inSamplerate, double outSamplerate) {
            assert(_init);
            _inSamplerate = inSamplerate;
            _outSamplerate = outSamplerate;
            reconfigure();
        }

        // IN-PLACE: safe (in == out). DecimatingFIR and PolyphaseResampler are
        // both in-place safe, and the pass-through skips the copy when out == in.
        inline int process(int count, const T* in, T* out) {
            switch(mode) {
                case Mode::BOTH:
                    count = decim.process(count, in, out);
                    return resamp.process(count, out, out);
                case Mode::DECIM_ONLY:
                    return decim.process(count, in, out);
                case Mode::RESAMP_ONLY:
                    return resamp.process(count, in, out);
                case Mode::NONE:
                    // Fused callers (RxVFOKernel) pass the same pointer for in
                    // and out; memcpy forbids overlapping ranges, identical
                    // ones included.
                    if (out != in) { memcpy(out, in, count * sizeof(T)); }
                    return count;
            }
            return count;
        }


    protected:
        bool _init = false;

        enum Mode {
            BOTH,
            DECIM_ONLY,
            RESAMP_ONLY,
            NONE
        };

        // Largest polyphase bank we are willing to build. The bank costs
        // interp * tapsPerPhase coefficients (one allocation, rows padded to the SIMD alignment),
        // while tapsPerPhase -- and therefore the cost and the quality of each
        // output sample -- does not depend on interp at all. A large interp
        // therefore buys nothing, and is worth trading for a tiny rate error.
        static constexpr int MAX_INTERP = 512;

        // Best rational approximation p/q to num/den with p <= maxNum, using the
        // convergents of the continued fraction expansion plus the best
        // semiconvergent. q is deliberately left unconstrained: it is only the
        // stride of the phase accumulator and costs no memory, and bounding it
        // would spoil exact ratios such as 384/625.
        static void approxRatio(int64_t num, int64_t den, int maxNum, int& outNum, int& outDen) {
            if (num <= (int64_t)maxNum) {
                outNum = (int)num;
                outDen = (int)den;
                return;
            }

            const double target = (double)num / (double)den;
            int64_t p0 = 0, q0 = 1, p1 = 1, q1 = 0;
            int64_t a = num, b = den;
            while (b != 0) {
                const int64_t t = a / b;
                const int64_t p2 = (t * p1) + p0;
                const int64_t q2 = (t * q1) + q0;
                if (p2 > (int64_t)maxNum) {
                    // The next convergent overflows the cap. The best remaining
                    // approximation is the largest semiconvergent that fits.
                    if (p1 > 0 && q1 > 0) {
                        const int64_t tmax = ((int64_t)maxNum - p0) / p1;
                        if (tmax > 0) {
                            const int64_t cp = (tmax * p1) + p0;
                            const int64_t cq = (tmax * q1) + q0;
                            if (cq > 0 && fabs(((double)cp / (double)cq) - target) < fabs(((double)p1 / (double)q1) - target)) {
                                outNum = (int)cp;
                                outDen = (int)cq;
                                return;
                            }
                        }
                    }
                    break;
                }
                p0 = p1; q0 = q1; p1 = p2; q1 = q2;
                const int64_t r = a - (t * b);
                a = b;
                b = r;
            }

            if (p1 > 0 && q1 > 0) {
                outNum = (int)p1;
                outDen = (int)q1;
                return;
            }

            // q1 == 0 means the loop broke on the very first convergent: the
            // integer part of num/den already exceeds maxNum, so no convergent
            // and no semiconvergent fits and neither branch above ran. The best
            // available approximation is then maxNum/1, not 1/1 -- returning
            // 1/1 here turned a 513:1 interpolation into passthrough.
            if (num > 0 && den > 0) {
                outNum = (int)((std::min)((int64_t)maxNum, num));
                outDen = 1;
            }
            else {
                outNum = 1;
                outDen = 1;
            }
        }

        static int boundedOutput(int input, int interp, int decim, int extra = 0) {
            if (input <= 0) { return input; }
            const int64_t output = (((int64_t)input * interp) + decim - 1) / decim + extra;
            if (output > (int64_t)(std::numeric_limits<int>::max)()) {
                throw std::runtime_error("RationalResampler output block size exceeds INT_MAX");
            }
            return (int)output;
        }

        void reconfigure() {
            // A non-positive rate yields interp == 0, which reaches
            // buildPolyphaseBank() as a zero phase count: a division by zero
            // computing tapsPerPhase, and a zero-sized allocation. Stay in
            // passthrough instead.
            if (!(_inSamplerate > 0.0) || !(_outSamplerate > 0.0)) {
                flog::warn("Resampler configured with a non-positive rate ({0} -> {1}), passing through",
                           _inSamplerate, _outSamplerate);
                mode = Mode::NONE;
                _maxOutputBlock = _maxBlock;
                return;
            }

            // Calculate highest power-of-two decimation for the power decimator.
            // Only meaningful when decimating: when upsampling, log2() is negative
            // and shifting by a negative count is undefined behaviour. Cap the
            // power, not the ratio -- shifting by getMaxRatio() would itself be
            // undefined behaviour.
            const int maxPredecPower = (int)log2((double)PowerDecimator<T>::getMaxRatio());
            int predecPower = 0;
            if (_inSamplerate > _outSamplerate) {
                predecPower = std::min<int>((int)floor(log2(_inSamplerate / _outSamplerate)), maxPredecPower);
            }
            int predecRatio = std::min<int>(1 << predecPower, PowerDecimator<T>::getMaxRatio());
            double intSamplerate = _inSamplerate;

            // Configure the DDC.
            //
            // From here on the sub-blocks are mutated one at a time and any of
            // them can throw on OOM, so there is no way to keep decim, resamp
            // and mode consistent through a failure. Drop to passthrough first:
            // a wrong output rate is recoverable and audible, whereas a stale
            // mode running new phase arithmetic against an old bank indexes out
            // of bounds. mode is committed again at each exit below.
            mode = Mode::NONE;
            _maxOutputBlock = _maxBlock;

            bool useDecim = (_inSamplerate > _outSamplerate && predecPower > 0);
            if (useDecim) {
                intSamplerate = _inSamplerate / (double)predecRatio;
                decim.setRatio(predecRatio);
            }

            // Calculate interpolation and decimation for the polyphase resampler.
            //
            // The ratio is derived exactly, as _outSamplerate * predecRatio over
            // _inSamplerate. Rounding intSamplerate to an integer first and taking
            // the gcd of that -- which is what this used to do -- is a trap: the
            // predecimated rate is usually fractional (2.5 MS/s / 64 = 39062.5),
            // and rounding destroys the common factor. gcd(39063, 24000) is 3
            // rather than 125, which turns interp into 8000 where 384 does the
            // job: a 21x larger bank, in 8000 allocations instead of 384, for the
            // same tapsPerPhase and a *worse* rate error.
            //
            // Note the names: `decim` is the PowerDecimator member, so the
            // polyphase side is `resampInterp`/`resampDecim`. The old code
            // declared a local `decim` here that shadowed the member, and only
            // worked because decim.setRatio() happens to sit above it.
            const int64_t ratioNum = (int64_t)llround(_outSamplerate) * (int64_t)predecRatio;
            const int64_t ratioDen = (int64_t)llround(_inSamplerate);
            const int64_t g = std::gcd(ratioNum, ratioDen);
            int resampInterp = 1;
            int resampDecim = 1;
            approxRatio(g ? (ratioNum / g) : ratioNum, g ? (ratioDen / g) : ratioDen, MAX_INTERP, resampInterp, resampDecim);

            // Check for excessive error
            double actualOutSR = intSamplerate * (double)resampInterp / (double)resampDecim;
            double error = fabs((actualOutSR - _outSamplerate) / _outSamplerate) * 100.0;
            if (error > 0.01) {
                flog::warn("Resampling error is over 0.01%: {0}% ({1} -> {2}, interp {3}, decim {4})",
                           error, _inSamplerate, _outSamplerate, resampInterp, resampDecim);
            }

            // If the power decimator already did all the work, don't use the resampler
            if (resampInterp == resampDecim) {
                _maxOutputBlock = useDecim ? boundedOutput(_maxBlock, 1, predecRatio) : _maxBlock;
                mode = useDecim ? Mode::DECIM_ONLY : Mode::NONE;
                return;
            }

            // Configure the polyphase resampler
            double tapSamplerate = intSamplerate * (double)resampInterp;
            double tapBandwidth = std::min<double>(_inSamplerate, _outSamplerate) / 2.0;
            double tapTransWidth = tapBandwidth * 0.1;
            // Local: the resampler builds its phase bank from it and keeps no reference.
            tap<float> rtaps = taps::lowPass(tapBandwidth, tapTransWidth, tapSamplerate);
            // Interpolating by N spreads the energy over N phases; restore the gain.
            rtaps.scale((float)resampInterp);
            // The polyphase stage only ever sees what the power decimator in
            // front of it emits, so its delay line is sized for that and not
            // for the block the owner was configured with.
            // [RATE-SIZED] Exact: predecRatio is a power of two applied by
            // the decimator above, so this is its true output bound. see doc/design/buffer-size-requirements.md 2.6
            const int resampMax = useDecim ? boundedOutput(_maxBlock, 1, predecRatio) : _maxBlock;
            // At an arbitrary retained phase a block can produce one sample
            // beyond the flat ceil(input * interp / decim) ratio.
            const int outputMax = boundedOutput(resampMax, resampInterp, resampDecim, 1);
            resamp.setRatio(resampInterp, resampDecim, rtaps, resampMax);

            flog::debug("[Resamp] predec: {0}, interp: {1}, decim: {2}, inacc: {3}%, taps: {4}",
                        predecRatio, resampInterp, resampDecim, error, rtaps.size());

            _maxOutputBlock = outputMax;
            mode = useDecim ? Mode::BOTH : Mode::RESAMP_ONLY;
        }
        
        PowerDecimator<T> decim;
        PolyphaseResampler<T> resamp;
        int _maxBlock = STREAM_BUFFER_SIZE;
        int _maxOutputBlock = STREAM_BUFFER_SIZE;
        double _inSamplerate;
        double _outSamplerate;
        Mode mode = Mode::NONE;
    };

    // Threaded adapter around the kernel, for the resamplers that really are
    // chain nodes with their own worker and output stream.
    template<class T>
    class RationalResamplerBlock : public Processor<T, T> {
        using base_type = Processor<T, T>;
    public:
        RationalResamplerBlock() {}

        RationalResamplerBlock(stream<T>* in, double inSamplerate, double outSamplerate, int maxBlock = STREAM_BUFFER_SIZE) {
            init(in, inSamplerate, outSamplerate, maxBlock);
        }

        ~RationalResamplerBlock() {
            // The owning members below are destroyed before ~block() runs, so
            // the worker must be joined here -- otherwise it keeps reading
            // buffers that have already been freed.
            if (!base_type::_block_init) { return; }
            base_type::stop();
        }

        void init(stream<T>* in, double inSamplerate, double outSamplerate, int maxBlock = STREAM_BUFFER_SIZE) {
            kernel.init(inSamplerate, outSamplerate, maxBlock);
            base_type::init(in);
        }

        void setInSamplerate(double inSamplerate) { reconf([&]{ kernel.setInSamplerate(inSamplerate); }); }
        void setOutSamplerate(double outSamplerate) { reconf([&]{ kernel.setOutSamplerate(outSamplerate); }); }
        void setRates(double inSamplerate, double outSamplerate) { reconf([&]{ kernel.setRates(inSamplerate, outSamplerate); }); }
        void reset() { reconf([&]{ kernel.reset(); }); }

        // IN-PLACE: safe (in == out). Delegates to the kernel above.
        inline int process(int count, const T* in, T* out) { return kernel.process(count, in, out); }

        int run() {
            int count = base_type::_in->read();
            if (count < 0) { return -1; }

            int outCount = kernel.process(count, base_type::_in->readBuf, base_type::out.writeBuf);

            base_type::_in->flush();
            if (outCount) {
                if (!base_type::out.swap(outCount)) { return -1; }
            }
            return outCount;
        }

        RationalResampler<T> kernel;

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
