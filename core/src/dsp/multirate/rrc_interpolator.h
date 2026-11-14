#pragma once
#include "../multirate/polyphase_resampler.h"
#include "../taps/root_raised_cosine.h"
#include <numeric>

namespace dsp::multirate {
    // Computation kernel, not a dsp::block. See dsp::math::Delay for why.
    template <class T>
    class RRCInterpolator {
    public:
        RRCInterpolator() {}

        RRCInterpolator(double symbolrate, double samplerate, double rrcBeta, int rrcTapCount) { init(symbolrate, samplerate, rrcBeta, rrcTapCount); }

        void init(double symbolrate, double samplerate, double rrcBeta, int rrcTapCount) {
            _symbolrate = symbolrate;
            _samplerate = samplerate;
            _rrcTapCount = rrcTapCount;

            // The resampler builds its phase bank from the taps, so they are
            // not kept.
            resamp.init(1, 1, taps::rootRaisedCosine<float>(_rrcTapCount, rrcBeta, _symbolrate, _samplerate));
            _rrcBeta = rrcBeta;
            genTaps();

            _init = true;
        }

        void setRates(double symbolrate, double samplerate) {
            assert(_init);
            _symbolrate = symbolrate;
            _samplerate = samplerate;
            genTaps();
        }

        void setRRCParam(double rrcBeta, int rrcTapCount) {
            assert(_init);
            _rrcBeta = rrcBeta;
            _rrcTapCount = rrcTapCount;
            genTaps();
        }

        void reset() {
            assert(_init);
            resamp.reset();
        }

        // IN-PLACE: safe (in == out). Delegates to PolyphaseResampler, which is.
        inline int process(int count, const T* in, T* out) {
            return resamp.process(count, in, out);
        }


    private:
        void genTaps() {
            // Free current taps if they exist

            // Calculate the rational samplerate ratio
            int InSR = round(_symbolrate);
            int OutSR = round(_samplerate);
            int gcd = std::gcd(InSR, OutSR);
            int interp = OutSR / gcd;
            int decim = InSR / gcd;

            // Configure resampler
            double tapSamplerate = _symbolrate * (double)interp;
            resamp.setRatio(interp, decim, taps::rootRaisedCosine<float>(_rrcTapCount * interp, _rrcBeta, _symbolrate, tapSamplerate));
        }

        double _symbolrate;
        double _samplerate;
        double _rrcBeta;
        int _rrcTapCount;

        multirate::PolyphaseResampler<T> resamp;

    };
}