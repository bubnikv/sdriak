#pragma once
// Hysteresis, unmute hold and dB level math adapted from the SDRPP fork by qrp73 (https://github.com/qrp73/SDRPP).
#include <algorithm>
#include "../processor.h"

// TODO: Rewrite better!!!!!
namespace dsp::noise_reduction {
    class PowerSquelch : public Processor<complex_t, complex_t> {
        using base_type = Processor<complex_t, complex_t>;
    public:
        PowerSquelch() {}

        PowerSquelch(stream<complex_t>* in, double level) {}

        ~PowerSquelch() {
            if (!base_type::_block_init) { return; }
            base_type::stop();
        }

        void init(stream<complex_t>* in, double level) {
            _level = level;
            updateHoldSamples();

            base_type::init(in);
        }

        void setLevel(double level) {
            assert(base_type::_block_init);
            std::lock_guard<std::recursive_mutex> lck(base_type::ctrlMtx);
            _level = level;
        }

        void setSamplerate(double samplerate) {
            assert(base_type::_block_init);
            std::lock_guard<std::recursive_mutex> lck(base_type::ctrlMtx);
            _samplerate = samplerate;
            updateHoldSamples();
        }

        // IN-PLACE: safe (in == out).
        inline int process(int count, const complex_t* in, complex_t* out) {
            // Compute the mean amplitude. The magnitudes are only summed, so
            // they go through a small stack chunk rather than a block-sized
            // buffer; `out` cannot serve as scratch, since it may be `in`.
            alignas(64) float norm[NORM_CHUNK];
            float sum = 0.0f;
            for (int i = 0; i < count; i += NORM_CHUNK) {
                int n = (std::min)(NORM_CHUNK, count - i);
                volk_32fc_magnitude_32f(norm, (const lv_32fc_t*)&in[i], n);
                float part = 0.0f;
                volk_32f_accumulator_s32f(&part, norm, n);
                sum += part;
            }
            sum /= (float)count;

            float level = 20.0f * log10f(sum);
            if (_muted) {
                // Unmute only after the signal has stayed above the threshold for the hold time
                if (level >= _level) {
                    _aboveCount += count;
                    if (_aboveCount >= _holdSamples) { _muted = false; }
                }
                else {
                    _aboveCount = 0;
                }
            }
            else if (level < _level - HYSTERESIS) {
                _muted = true;
                _aboveCount = 0;
            }

            if (!_muted) {
                if (out != in) { memcpy(out, in, count * sizeof(complex_t)); }
            }
            else {
                memset(out, 0, count * sizeof(complex_t));
            }

            return count;
        }

        int run() {
            int count = base_type::_in->read();
            if (count < 0) { return -1; }
            process(count, base_type::_in->readBuf, base_type::out.writeBuf);
            base_type::_in->flush();
            if (!base_type::out.swap(count)) { return -1; }
            return count;
        }

    private:
        void updateHoldSamples() {
            _holdSamples = (int)(_samplerate * HOLD_TIME);
        }

        static constexpr float HYSTERESIS = 1.0f;   // dB
        static constexpr double HOLD_TIME = 0.1;    // seconds above threshold before unmuting

        static constexpr int NORM_CHUNK = 1024;     // 4 KB of stack in process()

        float _level = -50.0f;
        double _samplerate = 48000.0;
        int _holdSamples = 4800;
        int _aboveCount = 0;
        bool _muted = false;

    };
}