#pragma once
#include "pll.h"

namespace dsp::loop {
    class CarrierTrackingPLL : public PLL {
        using base_type = PLL;
    public:
        CarrierTrackingPLL() {}

        CarrierTrackingPLL(double bandwidth, double initPhase = 0.0, double initFreq = 0.0, double minFreq = -FL_M_PI, double maxFreq = FL_M_PI) {
            base_type::init(bandwidth, initPhase, initFreq, minFreq, maxFreq);
        }

        // IN-PLACE: safe (in == out).
        inline int process(int count, complex_t* in, complex_t* out) {
            for (int i = 0; i < count; i++) {
                complex_t s = in[i];
                out[i] = s * math::phasor(-pcl.phase);
                pcl.advance(math::normalizePhase(s.phase() - pcl.phase));
            }
            return count;
        }
    };
}