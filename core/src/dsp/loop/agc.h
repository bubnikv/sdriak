#pragma once
#include <assert.h>
#include <algorithm>
#include <cmath>
#include "../types.h"

namespace dsp::loop {
    // Computation kernel, not a dsp::block. See dsp::math::Delay for why.
    // The caller must have stopped the containing block's worker before calling
    // any setter or reset().
    template <class T>
    class AGC {
    public:
        AGC() {}

        AGC(double setPoint, double attack, double decay, double maxGain, double maxOutputAmp, double initGain = 1.0) { init(setPoint, attack, decay, maxGain, maxOutputAmp, initGain); }

        void init(double setPoint, double attack, double decay, double maxGain, double maxOutputAmp, double initGain = 1.0) {
            _setPoint = setPoint;
            _attack = attack;
            _invAttack = 1.0f - _attack;
            _decay = decay;
            _invDecay = 1.0f - _decay;
            _maxGain = maxGain;
            _maxOutputAmp = maxOutputAmp;
            _initGain = initGain;
            amp = _setPoint / _initGain;
            _gain = std::min<float>(_initGain, _maxGain);
            _enabled = true;
            _init = true;
        }

        float getGain() {
            return _gain;
        }

        void setGain(float gain) {
            assert(_init);
            _gain = gain;
            // Seed the envelope so that when enabled, the AGC continues from this gain
            // instead of recomputing it from a stale envelope on the next sample
            if (gain > 0.0f) { amp = _setPoint / gain; }
        }

        void setEnabled(bool enabled) {
            assert(_init);
            _enabled = enabled;
        }

        void setSetPoint(double setPoint) {
            assert(_init);
            _setPoint = setPoint;
        }

        void setAttack(double attack) {
            assert(_init);
            _attack = attack;
            _invAttack = 1.0f - _attack;
        }

        void setDecay(double decay) {
            assert(_init);
            _decay = decay;
            _invDecay = 1.0f - _decay;
        }

        void setMaxGain(double maxGain) {
            assert(_init);
            _maxGain = maxGain;
        }

        void setMaxOutputAmp(double maxOutputAmp) {
            assert(_init);
            _maxOutputAmp = maxOutputAmp;
        }

        void setInitialGain(double initGain) {
            assert(_init);
            _initGain = initGain;
        }

        void reset() {
            assert(_init);
            amp = _setPoint / _initGain;
            _gain = std::min<float>(_initGain, _maxGain);
        }

        inline int process(int count, const T* in, T* out) {
            for (int i = 0; i < count; i++) {
                // Get signal amplitude
                float inAmp;
                if constexpr (std::is_same_v<T, complex_t>) {
                    inAmp = in[i].amplitude();
                }
                if constexpr (std::is_same_v<T, float>) {
                    inAmp = fabsf(in[i]);
                }

                if (_enabled) {
                    // Update average amplitude
                    if (inAmp != 0.0f) {
                        amp = (inAmp > amp) ? ((amp * _invAttack) + (inAmp * _attack)) : ((amp * _invDecay) + (inAmp * _decay));
                        _gain = std::min<float>(_setPoint / amp, _maxGain);
                    }
                    else {
                        _gain = 1.0f;
                    }

                    // If clipping is detected look ahead and correct
                    if (inAmp*_gain > _maxOutputAmp) {
                        float maxAmp = 0;
                        for (int j = i; j < count; j++) {
                            float lookAmp;
                            if constexpr (std::is_same_v<T, complex_t>) {
                                lookAmp = in[j].amplitude();
                            }
                            if constexpr (std::is_same_v<T, float>) {
                                lookAmp = fabsf(in[j]);
                            }
                            if (lookAmp > maxAmp) { maxAmp = lookAmp; }
                        }
                        amp = maxAmp;
                        _gain = std::min<float>(_setPoint / amp, _maxGain);
                    }

                    // Scale output by gain
                    out[i] = in[i] * _gain;
                }
                else {
                    // Manual mode: fixed gain, clipped to the max output amplitude
                    out[i] = in[i] * ((inAmp * _gain > _maxOutputAmp) ? (_maxOutputAmp / inAmp) : _gain);
                }
            }
            return count;
        }


    protected:
        bool _init = false;
        float _setPoint;
        float _attack;
        float _invAttack;
        float _decay;
        float _invDecay;
        float _maxGain;
        float _maxOutputAmp;
        float _initGain;
        float _gain = 1.0f;
        bool _enabled = true;

        float amp = 1.0;

    };
}