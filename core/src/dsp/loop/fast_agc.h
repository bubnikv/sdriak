#pragma once
#include <assert.h>
#include <cmath>
#include "../types.h"
#include "../processor.h"

namespace dsp::loop {
    // Computation kernel, not a dsp::block. Three of its four uses are fused
    // inside a composite; the one real chain node (atv_decoder) uses
    // FastAGCBlock below. See dsp::math::Delay for why.
    template <class T>
    class FastAGC {
    public:
        FastAGC() {}

        FastAGC(double setPoint, double maxGain, double rate, double initGain = 1.0) { init(setPoint, maxGain, rate, initGain); }

        void init(double setPoint, double maxGain, double rate, double initGain = 1.0) {
            _setPoint = setPoint;
            _maxGain = maxGain;
            _rate = rate;
            _initGain = initGain;

            _gain = _initGain;

            _init = true;
        }

        void setSetPoint(double setPoint) {
            assert(_init);
            _setPoint = setPoint;
        }

        void setMaxGain(double maxGain) {
            assert(_init);
            _maxGain = maxGain;
        }

        void setRate(double rate) {
            assert(_init);
            _rate = rate;
        }

        void setInitGain(double initGain) {
            assert(_init);
            _initGain = initGain;
        }

        void setGain(double gain) {
            assert(_init);
            _gain = gain;
        }

        void reset() {
            assert(_init);
            _gain = _initGain;
        }

        inline int process(int count, const T* in, T* out) {
            for (int i = 0; i < count; i++) {
                // Output scaled input
                out[i] = in[i] * _gain;

                // Calculate output amplitude
                float amp;
                if constexpr (std::is_same_v<T, float>) {
                    amp = fabsf(out[i]);
                }
                if constexpr (std::is_same_v<T, complex_t>) {
                    amp = out[i].amplitude();
                }

                // Update and clamp gain
                _gain += (_setPoint - amp) * _rate;
                if (_gain > _maxGain) { _gain = _maxGain; }
            }
            return count;
        }


    protected:
        bool _init = false;
        float _gain;
        float _setPoint;
        float _rate;
        float _maxGain;
        float _initGain;

    };

    // Threaded adapter around the kernel, for the one FastAGC that really is a
    // chain node with its own worker and output stream.
    template <class T>
    class FastAGCBlock : public Processor<T, T> {
        using base_type = Processor<T, T>;
    public:
        FastAGCBlock() {}

        FastAGCBlock(stream<T>* in, double setPoint, double maxGain, double rate, double initGain = 1.0) { init(in, setPoint, maxGain, rate, initGain); }

        ~FastAGCBlock() {
            // The owning members below are destroyed before ~block() runs, so
            // the worker must be joined here -- otherwise it keeps reading
            // buffers that have already been freed.
            if (!base_type::_block_init) { return; }
            base_type::stop();
        }

        void init(stream<T>* in, double setPoint, double maxGain, double rate, double initGain = 1.0) {
            kernel.init(setPoint, maxGain, rate, initGain);
            base_type::init(in);
        }

        void reset() {
            assert(base_type::_block_init);
            std::lock_guard<std::recursive_mutex> lck(base_type::ctrlMtx);
            dsp::block::TempStop guard(this);
            kernel.reset();
        }

        inline int process(int count, const T* in, T* out) { return kernel.process(count, in, out); }

        int run() {
            int count = base_type::_in->read();
            if (count < 0) { return -1; }

            kernel.process(count, base_type::_in->readBuf, base_type::out.writeBuf);

            base_type::_in->flush();
            if (!base_type::out.swap(count)) { return -1; }
            return count;
        }

        FastAGC<T> kernel;
    };
}
