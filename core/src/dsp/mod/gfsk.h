#pragma once
#include "../multirate/rrc_interpolator.h"
#include "quadrature.h"

namespace dsp::mod {
    class GFSK : public Processor<float, complex_t> {
        using base_type = Processor<float, complex_t>;
    public:
        GFSK() {}

        GFSK(stream<float>* in, double symbolrate, double samplerate, double rrcBeta, int rrcTapCount, double deviation) {
            init(in, symbolrate, samplerate, rrcBeta, rrcTapCount, deviation);
        }

        ~GFSK() {
            // Not redundant despite the RAII buffers: ~block() joins the
            // worker, but it runs *after* this class's members are destroyed.
            // The thread has to be stopped here, before that happens.
            if (!base_type::_block_init) { return; }
            base_type::stop();
        }

        void init(stream<float>* in, double symbolrate, double samplerate, double rrcBeta, int rrcTapCount, double deviation) {
            _samplerate = samplerate;
            _deviation = deviation;
            
            interp.init(symbolrate, _samplerate, rrcBeta, rrcTapCount);
            interpOut.allocate(STREAM_BUFFER_SIZE);
            mod.init(NULL, _deviation, _samplerate);

            base_type::init(in);
        }

        void setRates(double symbolrate, double samplerate) {
            assert(base_type::_block_init);
            std::lock_guard<std::recursive_mutex> lck(base_type::ctrlMtx);
            dsp::block::TempStop guard(this);
            _samplerate = samplerate;
            interp.setRates(symbolrate, _samplerate);
            mod.setDeviation(_deviation, _samplerate);
        }

        void setRRCParams(double rrcBeta, int rrcTapCount) {
            assert(base_type::_block_init);
            std::lock_guard<std::recursive_mutex> lck(base_type::ctrlMtx);
            dsp::block::TempStop guard(this);
            interp.setRRCParam(rrcBeta, rrcTapCount);
        }

        void setDeviation(double deviation) {
            assert(base_type::_block_init);
            std::lock_guard<std::recursive_mutex> lck(base_type::ctrlMtx);
            _deviation = deviation;
            mod.setDeviation(_deviation, _samplerate);
        }

        void reset() {
            assert(base_type::_block_init);
            std::lock_guard<std::recursive_mutex> lck(base_type::ctrlMtx);
            dsp::block::TempStop guard(this);
            interp.reset();
            mod.reset();
        }

        inline int process(int count, const float* in, complex_t* out) {
            count = interp.process(count, in, interpOut);
            mod.process(count, interpOut, out);
            return count;
        }

        int run() {
            int count = base_type::_in->read();
            if (count < 0) { return -1; }

            int outCount = process(count, base_type::_in->readBuf, base_type::out.writeBuf);

            // Swap if some data was generated
            base_type::_in->flush();
            if (outCount) {
                if (!base_type::out.swap(outCount)) { return -1; }
            }
            return outCount;
        }

    private:
        double _samplerate;
        double _deviation;

        multirate::RRCInterpolator<float> interp;
        buffer::Buffer<float> interpOut;   // scratch, in place of its old out stream
        Quadrature mod;
    };
}