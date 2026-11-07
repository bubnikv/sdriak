#pragma once
#include "fir.h"

namespace dsp::filter {
    template <class D, class T>
    class DecimatingFIR : public FIR<D, T> {
        using base_type = FIR<D, T>;
    public:
        DecimatingFIR() {}

        DecimatingFIR(tap<T>& taps, int decimation, int maxBlock = STREAM_BUFFER_SIZE) { init(taps, decimation, maxBlock); }

        void init(tap<T>& taps, int decimation, int maxBlock = STREAM_BUFFER_SIZE) {
            _decimation = decimation;
            base_type::init(taps, maxBlock);
        }

        void setTaps(tap<T>& taps, int maxBlock = -1) override {
            offset = 0;
            base_type::setTaps(taps, maxBlock);
        }

        void setDecimation(int decimation) {
            _decimation = decimation;
            offset = 0;
        }

        void reset() override {
            offset = 0;
            base_type::reset();
        }

        inline int process(int count, const D* in, D* out) {
            // Copy data to work buffer
            memcpy(base_type::bufStart, in, count * sizeof(D));

            // Do convolution
            int outCount = 0;
            for (; offset < count; offset += _decimation) {
                if constexpr (std::is_same_v<D, float> && std::is_same_v<T, float>) {
                    volk_32f_x2_dot_prod_32f(&out[outCount++], &base_type::buffer[offset], base_type::_taps.taps, base_type::_taps.size);
                }
                if constexpr ((std::is_same_v<D, complex_t> || std::is_same_v<D, stereo_t>) && std::is_same_v<T, float>) {
                    volk_32fc_32f_dot_prod_32fc((lv_32fc_t*)&out[outCount++], (lv_32fc_t*)&base_type::buffer[offset], base_type::_taps.taps, base_type::_taps.size);
                }
                if constexpr ((std::is_same_v<D, complex_t> || std::is_same_v<D, stereo_t>) && std::is_same_v<T, complex_t>) {
                    volk_32fc_x2_dot_prod_32fc((lv_32fc_t*)&out[outCount++], (lv_32fc_t*)&base_type::buffer[offset], (lv_32fc_t*)base_type::_taps.taps, base_type::_taps.size);
                }
            }
            offset -= count;

            // Move unused data
            memmove(base_type::buffer, &base_type::buffer[count], (base_type::_taps.size - 1) * sizeof(D));

            return outCount;
        }


    protected:
        int _decimation;
        int offset = 0;
    };
}