#pragma once
#include <assert.h>
#include "../types.h"

namespace dsp::convert {
    // Computation kernel, not a dsp::block. See dsp::math::Delay for why.
    //
    // This used to keep a zero-filled scratch buffer of maxBlock floats and
    // hand it to volk_32f_x2_interleave_32fc() as the imaginary part -- 4 MB of
    // zeros per instance at the default block size, read in full on every call.
    // Writing the zeros directly costs one store per sample instead of a load
    // and a store, and removes the allocation entirely.
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
