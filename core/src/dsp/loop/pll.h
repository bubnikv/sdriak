#pragma once
#include <assert.h>
#include "../types.h"
#include "../math/normalize_phase.h"
#include "../math/phasor.h"
#include "phase_control_loop.h"

namespace dsp::loop {
    // Computation kernel, not a dsp::block. See dsp::math::Delay for why.
    // The caller must have stopped the containing block's worker before calling
    // any setter or reset().
    class PLL {
    public:
        PLL() {}

        PLL(double bandwidth, double initPhase = 0.0, double initFreq = 0.0, double minFreq = -FL_M_PI, double maxFreq = FL_M_PI) { init(bandwidth, initPhase, initFreq, minFreq, maxFreq); }

        void init(double bandwidth, double initPhase = 0.0, double initFreq = 0.0, double minFreq = -FL_M_PI, double maxFreq = FL_M_PI) {
            _initPhase = initPhase;
            _initFreq = initFreq;

            // Init phase control loop
            float alpha, beta;
            PhaseControlLoop<float>::criticallyDamped(bandwidth, alpha, beta);
            pcl.init(alpha, beta, initPhase, -FL_M_PI, FL_M_PI, initFreq, minFreq, maxFreq);

            _init = true;
        }

        void setBandwidth(double bandwidth) {
            assert(_init);
            float alpha, beta;
            PhaseControlLoop<float>::criticallyDamped(bandwidth, alpha, beta);
            pcl.setCoefficients(alpha, beta);
        }

        void setInitialPhase(double initPhase) {
            assert(_init);
            _initPhase = initPhase;
        }

        void setInitialFreq(double initFreq) {
            assert(_init);
            _initFreq = initFreq;
        }

        void setFrequencyLimits(double minFreq, double maxFreq) {
            assert(_init);
            pcl.setFreqLimits(minFreq, maxFreq);
        }

        void reset() {
            assert(_init);
            pcl.phase = _initPhase;
            pcl.freq = _initFreq;
        }

        // IN-PLACE: safe (in == out).
        virtual inline int process(int count, complex_t* in, complex_t* out) {
            for (int i = 0; i < count; i++) {
                float inPhase = in[i].phase();
                out[i] = math::phasor(pcl.phase);
                pcl.advance(math::normalizePhase(inPhase - pcl.phase));
            }
            return count;
        }


    protected:
        bool _init = false;
        PhaseControlLoop<float> pcl;
        float _initPhase;
        float _initFreq;
        complex_t lastVCO = { 1.0f, 0.0f };

    };
}