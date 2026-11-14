#pragma once
#include <assert.h>
#include "../types.h"
#include "../math/fast_atan2.h"
#include "../math/hz_to_rads.h"
#include "../math/normalize_phase.h"

namespace dsp::demod {
    // Computation kernel, not a dsp::block. See dsp::math::Delay for why.
    // The caller must have stopped the containing block's worker before calling
    // setDeviation() or reset().
    class Quadrature {
    public:
        Quadrature() {}

        Quadrature(double deviation) { init(deviation); }

        Quadrature(double deviation, double samplerate) { init(deviation, samplerate); }

        void init(double deviation) {
            _invDeviation = 1.0 / deviation;
            _init = true;
        }

        void init(double deviation, double samplerate) {
            init(math::hzToRads(deviation, samplerate));
        }

        void setDeviation(double deviation) {
            assert(_init);
            _invDeviation = 1.0 / deviation;
        }

        void setDeviation(double deviation, double samplerate) {
            assert(_init);
            _invDeviation = 1.0 / math::hzToRads(deviation, samplerate);
        }

        inline int process(int count, const complex_t* in, float* out) {
            for (int i = 0; i < count; i++) {
                float cphase = in[i].phase();
                out[i] = math::normalizePhase(cphase - phase) * _invDeviation;
                phase = cphase;
            }
            return count;
        }

        void reset() {
            assert(_init);
            phase = 0.0f;
        }

    protected:
        bool _init = false;
        float _invDeviation = 1.0f;
        float phase = 0.0f;
    };
}
