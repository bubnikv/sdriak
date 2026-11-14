#pragma once
#include "fir.h"

namespace dsp::filter {
    template <class D, class T>
    class DecimatingFIR : public FIR<D, T> {
        using base_type = FIR<D, T>;
    public:
        DecimatingFIR() {}

        // Same ownership rules as FIR: an rvalue is adopted, an lvalue borrowed.
        DecimatingFIR(tap<T>&& taps, int decimation, int maxBlock = STREAM_BUFFER_SIZE) { init(std::move(taps), decimation, maxBlock); }
        DecimatingFIR(const tap<T>& taps, int decimation, int maxBlock = STREAM_BUFFER_SIZE) { init(taps, decimation, maxBlock); }

        void init(tap<T>&& taps, int decimation, int maxBlock = STREAM_BUFFER_SIZE) {
            _decimation = decimation;
            offset = 0;
            base_type::init(std::move(taps), maxBlock);
        }

        void init(const tap<T>& taps, int decimation, int maxBlock = STREAM_BUFFER_SIZE) {
            _decimation = decimation;
            offset = 0;
            base_type::init(taps, maxBlock);
        }

        void setDecimation(int decimation) {
            _decimation = decimation;
            offset = 0;
        }

        void reset() override {
            offset = 0;
            base_type::reset();
        }

        // Same delay line as FIR::process(), but only every _decimation-th
        // output is computed; offset carries the phase over.
        // IN-PLACE: safe (in == out).
        inline int process(int count, const D* in, D* out) {
            const TapView<T>& taps = base_type::m_taps;
            const D* window = base_type::m_hist.push(in, count);

            // Do convolution
            int outCount = 0;
            for (; offset < count; offset += _decimation) {
                if constexpr (std::is_same_v<D, float> && std::is_same_v<T, float>)
                    volk_32f_x2_dot_prod_32f(&out[outCount++], &window[offset], taps.taps(), taps.size());
                else if constexpr ((std::is_same_v<D, complex_t> || std::is_same_v<D, stereo_t>) && std::is_same_v<T, float>)
                    volk_32fc_32f_dot_prod_32fc((lv_32fc_t*)&out[outCount++], (const lv_32fc_t*)&window[offset], taps.taps(), taps.size());
                else if constexpr ((std::is_same_v<D, complex_t> || std::is_same_v<D, stereo_t>) && std::is_same_v<T, complex_t>)
                    volk_32fc_x2_dot_prod_32fc((lv_32fc_t*)&out[outCount++], (const lv_32fc_t*)&window[offset], (const lv_32fc_t*)taps.taps(), taps.size());
                else {
                    static_assert(dependent_false_v<D, T>, "Unsupported DecimatingFIR input/output/tap type combination");
                }
            }
            offset -= count;

            base_type::m_hist.advance(count);
            return outCount;
        }

    protected:
        void applyTaps(TapView<T> taps, int maxBlock) override {
            offset = 0;
            base_type::applyTaps(taps, maxBlock);
        }

        int _decimation;
        int offset = 0;
    };
}