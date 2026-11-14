#pragma once
#include <assert.h>
#include "../types.h"
#include "../buffer/history_buffer.h"
#include "../stream.h"
#include "../loop/phase_control_loop.h"
#include "../taps/windowed_sinc.h"
#include "../multirate/polyphase_bank.h"
#include "../math/step.h"

namespace dsp::clock_recovery {
    // Computation kernel, not a dsp::block. See dsp::math::Delay for why.
    // The caller must have stopped the containing block's worker before calling
    // any setter or reset().
    template<class T>
    class MM {
    public:
        MM() {}

        MM(double omega, double omegaGain, double muGain, double omegaRelLimit, int interpPhaseCount = 128, int interpTapCount = 8, int maxBlock = STREAM_BUFFER_SIZE) { init(omega, omegaGain, muGain, omegaRelLimit, interpPhaseCount, interpTapCount, maxBlock); }

        void init(double omega, double omegaGain, double muGain, double omegaRelLimit, int interpPhaseCount = 128, int interpTapCount = 8, int maxBlock = STREAM_BUFFER_SIZE) {
            _maxBlock = maxBlock;
            _omega = omega;
            _omegaGain = omegaGain;
            _muGain = muGain;
            _omegaRelLimit = omegaRelLimit;
            _interpPhaseCount = interpPhaseCount;
            _interpTapCount = interpTapCount;

            pcl.init(_muGain, _omegaGain, 0.0, 0.0, 1.0, _omega, _omega * (1.0 - omegaRelLimit), _omega * (1.0 + omegaRelLimit));
            interpBank = buildInterpBank(_interpPhaseCount, _interpTapCount);
            // [RATE-SIZED] RDSDemod and POCSAGDSP pass a rate-derived maxBlock
            // here rather than STREAM_BUFFER_SIZE. see doc/design/buffer-size-requirements.md 2.6
            // Layout: [ history: taps-1 | input: count ]. volk_malloc does not
            // zero, and the first block convolves over the history.
            hist.init(_maxBlock, _interpTapCount - 1);

            _init = true;
        }

        void setOmega(double omega) {
            assert(_init);
            _omega = omega;
            offset = 0;
            pcl.phase = 0.0f;
            pcl.freq = _omega;
            pcl.setFreqLimits(_omega * (1.0 - _omegaRelLimit), _omega * (1.0 + _omegaRelLimit));
        }

        void setOmegaGain(double omegaGain) {
            assert(_init);
            _omegaGain = omegaGain;
            pcl.setCoefficients(_muGain, _omegaGain);
        }

        void setMuGain(double muGain) {
            assert(_init);
            _muGain = muGain;
            pcl.setCoefficients(_muGain, _omegaGain);
        }

        void setOmegaRelLimit(double omegaRelLimit) {
            assert(_init);
            _omegaRelLimit = omegaRelLimit;
            pcl.setFreqLimits(_omega * (1.0 - _omegaRelLimit), _omega * (1.0 + _omegaRelLimit));
        }

        void setInterpParams(int interpPhaseCount, int interpTapCount) {
            assert(_init);

            // Acquire both the bank and the buffer before committing anything,
            // so a throw leaves the block on its existing configuration.
            dsp::multirate::PolyphaseBank<float> freshBank =
                buildInterpBank(interpPhaseCount, interpTapCount);
            // Strong, and discards the history: it belonged to the old taps.
            hist.resize(_maxBlock, interpTapCount - 1);

            _interpPhaseCount = interpPhaseCount;
            _interpTapCount = interpTapCount;
            interpBank = std::move(freshBank);
        }

        void reset() {
            assert(_init);
            offset = 0;
            pcl.phase = 0.0f;
            pcl.freq = _omega;
            lastOut = 0.0f;
            _p_0T = { 0.0f, 0.0f }; _p_1T = { 0.0f, 0.0f }; _p_2T = { 0.0f, 0.0f };
            _c_0T = { 0.0f, 0.0f }; _c_1T = { 0.0f, 0.0f }; _c_2T = { 0.0f, 0.0f };
        }

        // IN-PLACE: safe (in == out).
        inline int process(int count, const T* in, T* out) {
            const T* window = hist.push(in, count);

            // Process all samples
            int outCount = 0;
            while (offset < count) {
                float error;
                T outVal;

                // Calculate new output value
                int phase = std::clamp<int>(floorf(pcl.phase * (float)_interpPhaseCount), 0, _interpPhaseCount - 1);
                if constexpr (std::is_same_v<T, float>) {
                    volk_32f_x2_dot_prod_32f(&outVal, &window[offset], interpBank.phase(phase), _interpTapCount);
                }
                else if constexpr (std::is_same_v<T, complex_t>) {
                    volk_32fc_32f_dot_prod_32fc((lv_32fc_t*)&outVal, (const lv_32fc_t*)&window[offset], interpBank.phase(phase), _interpTapCount);
                }
                else {
                    static_assert(dependent_false_v<T>, "Unsupported sample type");
                }
                out[outCount++] = outVal;

                // Calculate symbol phase error
                if constexpr (std::is_same_v<T, float>) {
                    error = (math::step(lastOut) * outVal) - (lastOut * math::step(outVal));
                    lastOut = outVal;
                }
                else if constexpr (std::is_same_v<T, complex_t>) {
                    // Propagate delay
                    _p_2T = _p_1T;
                    _p_1T = _p_0T;
                    _c_2T = _c_1T;
                    _c_1T = _c_0T;

                    // Update the T0 values
                    _p_0T = outVal;
                    _c_0T = math::step(outVal);

                    // Error
                    error = (((_p_0T - _p_2T) * _c_1T.conj()) - ((_c_0T - _c_2T) * _p_1T.conj())).re;
                }
                else {
                    static_assert(dependent_false_v<T>, "Unsupported sample type");
                }

                // Clamp symbol phase error
                if (error > 1.0f) { error = 1.0f; }
                if (error < -1.0f) { error = -1.0f; }

                // Advance symbol offset and phase
                pcl.advance(error);
                float delta = floorf(pcl.phase);
                offset += delta;
                pcl.phase -= delta;
            }
            offset -= count;

            hist.advance(count);

            return outCount;
        }


    protected:
        bool _init = false;
        int _maxBlock = STREAM_BUFFER_SIZE;
        // Returns the bank instead of assigning it, so callers can build the
        // replacement before committing anything. lp owns its taps: if the bank
        // allocation throws, the windowed sinc is released on the way out.
        dsp::multirate::PolyphaseBank<float> buildInterpBank(int phaseCount, int tapCount) {
            double bw = 0.5 / (double)phaseCount;
            dsp::tap<float> lp = dsp::taps::windowedSinc<float>(phaseCount * tapCount, dsp::math::hzToRads(bw, 1.0), dsp::window::nuttall, phaseCount);
            return dsp::multirate::buildPolyphaseBank<float>(phaseCount, lp);
        }

        dsp::multirate::PolyphaseBank<float> interpBank;
        loop::PhaseControlLoop<float, false> pcl;

        double _omega;
        double _omegaGain;
        double _muGain;
        double _omegaRelLimit;
        int _interpPhaseCount;
        int _interpTapCount;

        // Previous output storage
        float lastOut = 0.0f;
        complex_t _p_0T = { 0.0f, 0.0f }, _p_1T = { 0.0f, 0.0f }, _p_2T = { 0.0f, 0.0f };
        complex_t _c_0T = { 0.0f, 0.0f }, _c_1T = { 0.0f, 0.0f }, _c_2T = { 0.0f, 0.0f };

        int offset = 0;
        // [ history: interpTapCount-1 | block: <= maxBlock ]
        buffer::HistoryBuffer<T> hist;
    };
}