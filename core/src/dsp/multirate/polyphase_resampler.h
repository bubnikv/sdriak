#pragma once
#include <assert.h>
#include <string.h>
#include "../types.h"
#include "../buffer/buffer.h"
#include "../stream.h"
#include "../taps/tap.h"
#include "polyphase_bank.h"

namespace dsp::multirate {
    // Computation kernel, not a dsp::block. See dsp::math::Delay for why.
    // Only ever used inside RationalResampler and RRCInterpolator, both of which
    // call process() inline. The caller must stop the containing block's worker
    // before calling setRatio()/reset().
    template<class T>
    class PolyphaseResampler {
    public:
        PolyphaseResampler() {}

        PolyphaseResampler(int interp, int decim, tap<float> taps, int maxBlock = STREAM_BUFFER_SIZE) { init(interp, decim, taps, maxBlock); }

        void init(int interp, int decim, tap<float> taps, int maxBlock = STREAM_BUFFER_SIZE) {
            _interp = interp;
            _decim = decim;
            _taps = taps;

            // Build filter bank
            phases = buildPolyphaseBank(_interp, _taps);

            // Allocate delay buffer
            _maxBlock = maxBlock;
            buffer.allocate(_maxBlock + 64000);
            bufStart = &buffer[phases.tapsPerPhase - 1];
            buffer::clear<T>(buffer, phases.tapsPerPhase - 1);

            _init = true;
        }

        // maxBlock defaults to -1, meaning "keep the current delay line". Pass
        // a value when the caller's own block bound has changed -- a resampler
        // that sits behind a decimator sees a fraction of the block the owner
        // was sized for, and the delay line is the expensive part.
        void setRatio(int interp, int decim, tap<float>& taps, int maxBlock = -1) {
            assert(_init);

            // Build first, commit second. Committing _interp/_decim up front and
            // then throwing would leave the new phase arithmetic running against
            // the old bank, eventually indexing past it.
            // Owned, not a bare PolyphaseBank: the buffer allocation below can
            // throw, and a bare one would leak every phase allocation in it.
            OwnedPolyphaseBank<float> fresh = buildPolyphaseBank(interp, taps);

            // Both acquisitions are strong, and both happen before anything is
            // committed: a throw here leaves the resampler on its existing bank
            // and delay line, and `fresh` is released on the way out.
            // [RATE-SIZED] maxBlock is what the stage in front of this one
            // emits, not a constant. see doc/design/buffer-size-requirements.md 2.6
            if (maxBlock > 0 && maxBlock != _maxBlock) {
                buffer.allocate(maxBlock + 64000);
                _maxBlock = maxBlock;
            }

            _interp = interp;
            _decim = decim;
            _taps = taps;
            phases = std::move(fresh);

            // Reset buffer
            bufStart = &buffer[phases.tapsPerPhase - 1];
            reset();

        }

        void reset() {
            assert(_init);
            buffer::clear<T>(buffer, phases.tapsPerPhase - 1);
            phase = 0;
            offset = 0;
        }

        inline int process(int count, const T* in, T* out) {
            int outCount = 0;

            // Copy input to buffer
            memcpy(bufStart, in, count * sizeof(T));

            while (offset < count) {
                // Do convolution
                if constexpr (std::is_same_v<T, float>) {
                    volk_32f_x2_dot_prod_32f(&out[outCount++], &buffer[offset], phases.phases[phase], phases.tapsPerPhase);
                }
                if constexpr (std::is_same_v<T, complex_t> || std::is_same_v<T, stereo_t>) {
                    volk_32fc_32f_dot_prod_32fc((lv_32fc_t*)&out[outCount++], (lv_32fc_t*)&buffer[offset], phases.phases[phase], phases.tapsPerPhase);
                }

                // Increment phase
                phase += _decim;

                // Branchless phase advance if phase wrap arround occurs
                offset += phase / _interp;

                // Wrap around if needed
                phase = phase % _interp;
            }
            offset -= count;

            // Move delay
            memmove(buffer, &buffer[count], (phases.tapsPerPhase - 1) * sizeof(T));

            return outCount;
        }


    protected:
        bool _init = false;
        int _maxBlock = 0;
        int _interp;
        int _decim;
        tap<float> _taps;
        OwnedPolyphaseBank<float> phases;
        int phase = 0;
        int offset = 0;
        buffer::Buffer<T> buffer;
        T* bufStart = NULL;

    };
}