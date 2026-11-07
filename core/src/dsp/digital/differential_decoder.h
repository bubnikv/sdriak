#pragma once
#include <assert.h>
#include <stdint.h>

namespace dsp::digital {
    // Computation kernel, not a dsp::block. See dsp::math::Delay for why.
    class DifferentialDecoder {
    public:
        DifferentialDecoder() {}

        DifferentialDecoder(uint8_t modulus, uint8_t initSym = 0) { init(modulus, initSym); }

        void init(uint8_t modulus, uint8_t initSym = 0) {
            _modulus = modulus;
            _initSym = initSym;
            last = _initSym;
            _init = true;
        }

        void setModulus(uint8_t modulus) {
            assert(_init);
            _modulus = modulus;
        }

        void setInitSym(uint8_t initSym) {
            assert(_init);
            _initSym = initSym;
        }

        void reset() {
            assert(_init);
            last = _initSym;
        }

        inline int process(int count, const uint8_t* in, uint8_t* out) {
            for (int i = 0; i < count; i++) {
                out[i] = (in[i] - last + _modulus) % _modulus;
                last = in[i];
            }
            return count;
        }

    protected:
        bool _init = false;
        uint8_t last = 0;
        uint8_t _initSym = 0;
        uint8_t _modulus = 2;
    };
}
