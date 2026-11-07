#pragma once
#include "../processor.h"
#include "../channel/frequency_xlator.h"
#include "../convert/complex_to_real.h"
#include "../loop/agc.h"
#include "../convert/mono_to_stereo.h"

namespace dsp::demod {
    template <class T>
    class CW : public Processor<complex_t, T> {
        using base_type = Processor<complex_t, T>;
    public:
        CW() {}
        
        CW(stream<complex_t>* in, double tone, double agcAttack, double agcDecay, double samplerate) { init(in, tone, agcAttack, agcDecay, samplerate); }

        ~CW() {
            // Not redundant despite the RAII buffers: ~block() joins the
            // worker, but it runs *after* this class's members are destroyed.
            // The thread has to be stopped here, before that happens.
            if (!base_type::_block_init) { return; }
            base_type::stop();
        }

        void init(stream<complex_t>* in, double tone, double agcAttack, double agcDecay, double samplerate) {
            _tone = tone;
            _samplerate = samplerate;
            
            xlator.init(tone, samplerate);
            xlatorOut.allocate(STREAM_BUFFER_SIZE);
            agc.init(1.0, agcAttack, agcDecay, 10e6, 10.0, INFINITY);

            // Only the stereo path needs an AGC scratch buffer; the float
            // path works directly in the caller's output.
            if constexpr (std::is_same_v<T, stereo_t>) {
                agcOut.allocate(STREAM_BUFFER_SIZE);
            }

            base_type::init(in);
        }

        void setTone(double tone) {
            assert(base_type::_block_init);
            std::lock_guard<std::recursive_mutex> lck(base_type::ctrlMtx);
            _tone = tone;
            xlator.setOffset(_tone, _samplerate);
        }

        void setAGCEnabled(bool enabled) {
            assert(base_type::_block_init);
            std::lock_guard<std::recursive_mutex> lck(base_type::ctrlMtx);
            agc.setEnabled(enabled);
        }

        void setAGCGain(float gain) {
            assert(base_type::_block_init);
            std::lock_guard<std::recursive_mutex> lck(base_type::ctrlMtx);
            agc.setGain(gain);
        }

        float getAGCGain() {
            assert(base_type::_block_init);
            return agc.getGain();
        }

        void setAGCAttack(double attack) {
            assert(base_type::_block_init);
            std::lock_guard<std::recursive_mutex> lck(base_type::ctrlMtx);
            agc.setAttack(attack);
        }

        void setAGCDecay(double decay) {
            assert(base_type::_block_init);
            std::lock_guard<std::recursive_mutex> lck(base_type::ctrlMtx);
            agc.setDecay(decay);
        }

        void setSamplerate(double samplerate) {
            assert(base_type::_block_init);
            std::lock_guard<std::recursive_mutex> lck(base_type::ctrlMtx);
            _samplerate = samplerate;
            xlator.setOffset(_tone, _samplerate);
        }

        inline int process(int count, const complex_t* in, T* out) {
            xlator.process(count, in, xlatorOut);
            if constexpr (std::is_same_v<T, float>) {
                dsp::convert::ComplexToReal::process(count, xlatorOut, out);
                agc.process(count, out, out);
            }
            if constexpr (std::is_same_v<T, stereo_t>) {
                dsp::convert::ComplexToReal::process(count, xlatorOut, agcOut);
                agc.process(count, agcOut, agcOut);
                convert::MonoToStereo::process(count, agcOut, out);
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
        double _tone;
        double _samplerate;

        dsp::channel::FrequencyXlator xlator;
        buffer::Buffer<complex_t> xlatorOut;   // scratch, in place of xlator's old out stream
        dsp::loop::AGC<float> agc;
        buffer::Buffer<float> agcOut;   // scratch, stereo path only

    };
}