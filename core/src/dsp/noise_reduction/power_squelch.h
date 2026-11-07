#pragma once
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
            buffer::free(normBuffer);
        }

        void init(stream<complex_t>* in, double level) {
            _level = level;
            updateHoldSamples();

            normBuffer = buffer::alloc<float>(STREAM_BUFFER_SIZE);

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

        inline int process(int count, const complex_t* in, complex_t* out) {
            // Compute the amplitude of each sample
            volk_32fc_magnitude_32f(normBuffer, (lv_32fc_t*)in, count);

            // Compute the mean amplitude
            float sum = 0.0f;
            volk_32f_accumulator_s32f(&sum, normBuffer, count);
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
                memcpy(out, in, count * sizeof(complex_t));
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

        float* normBuffer = NULL;
        float _level = -50.0f;
        double _samplerate = 48000.0;
        int _holdSamples = 4800;
        int _aboveCount = 0;
        bool _muted = false;

    };
}