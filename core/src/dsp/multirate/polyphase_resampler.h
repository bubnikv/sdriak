#pragma once
#include <assert.h>
#include <string.h>
#include "../types.h"
#include "../buffer/buffer.h"
#include "../buffer/history_buffer.h"
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

        PolyphaseResampler(int interp, int decim, const tap<float>& taps, int maxBlock = STREAM_BUFFER_SIZE) { init(interp, decim, taps, maxBlock); }

        void init(int interp, int decim, const tap<float>& taps, int maxBlock = STREAM_BUFFER_SIZE) {
            _interp = interp;
            _decim = decim;

            // Build filter bank. The bank is a copy, so the taps are not kept.
            phases = buildPolyphaseBank(_interp, taps);

            // Delay line: the last tapsPerPhase-1 input samples.
            hist.init(maxBlock, phases.tapsPerPhase() - 1);

            _init = true;
        }

        // maxBlock defaults to -1, meaning "keep the current delay line". Pass
        // a value when the caller's own block bound has changed -- a resampler
        // that sits behind a decimator sees a fraction of the block the owner
        // was sized for, and the delay line is the expensive part.
        void setRatio(int interp, int decim, const tap<float>& taps, int maxBlock = -1) {
            assert(_init);

            // Build first, commit second. Committing _interp/_decim up front and
            // then throwing would leave the new phase arithmetic running against
            // the old bank, eventually indexing past it.
            // Held in a local first: the buffer allocation below can throw, and
            // the old bank must survive that.
            PolyphaseBank<float> fresh = buildPolyphaseBank(interp, taps);

            // Both acquisitions are strong, and both happen before anything is
            // committed: a throw here leaves the resampler on its existing bank
            // and delay line, and `fresh` is released on the way out.
            // [RATE-SIZED] maxBlock is what the stage in front of this one
            // emits, not a constant. see doc/design/buffer-size-requirements.md 2.6
            // The history is discarded (the phase arithmetic restarts), and
            // resize() reallocates only if the bound or the room changed.
            hist.resize((maxBlock > 0) ? maxBlock : hist.maxBlock(), fresh.tapsPerPhase() - 1);

            _interp = interp;
            _decim = decim;
            phases = std::move(fresh);

            reset();
        }

        void reset() {
            assert(_init);
            hist.clear();
            phase = 0;
            offset = 0;
        }

        // IN-PLACE: safe (in == out).
        inline int process(int count, const T* in, T* out) {
            int outCount = 0;

            const T* window = hist.push(in, count);

            while (offset < count) {
                // Do convolution
                if constexpr (std::is_same_v<T, float>) {
                    volk_32f_x2_dot_prod_32f(&out[outCount++], &window[offset], phases.phase(phase), phases.tapsPerPhase());
                }
                else if constexpr (std::is_same_v<T, complex_t> || std::is_same_v<T, stereo_t>) {
                    volk_32fc_32f_dot_prod_32fc((lv_32fc_t*)&out[outCount++], (const lv_32fc_t*)&window[offset], phases.phase(phase), phases.tapsPerPhase());
                }
                else {
                    static_assert(dependent_false_v<T>, "Unsupported sample type");
                }

                // Increment phase
                phase += _decim;

                // Branchless phase advance if phase wrap arround occurs
                offset += phase / _interp;

                // Wrap around if needed
                phase = phase % _interp;
            }
            offset -= count;

            hist.advance(count);

            return outCount;
        }


    protected:
        bool _init = false;
        int _interp;
        int _decim;
        PolyphaseBank<float> phases;
        int phase = 0;
        int offset = 0;
        // [ history: tapsPerPhase-1 | block: <= maxBlock ]
        buffer::HistoryBuffer<T> hist;

    };
}