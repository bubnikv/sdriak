#pragma once
#include <assert.h>
#include "../types.h"

namespace dsp::convert {
    // Computation kernel, not a dsp::block. See dsp::math::Delay for why.
    //
    // The imaginary part is written as zeros directly, one store per sample.
    // Interleaving against a zero-filled buffer instead would cost a
    // maxBlock-float allocation (4 MB at the default block size) and a load
    // per sample.
    class RealToComplex {
    public:
        RealToComplex() {}

        void init() { _init = true; }

        // `in` and `out` must not alias: this writes two floats for every one
        // it reads, so an in-place call would overwrite its own input.
        inline int process(int count, const float* in, complex_t* out) {
            for (int i = 0; i < count; i++) {
                out[i] = { in[i], 0.0f };
            }
            return count;
        }

    private:
        bool _init = false;
    };
}
