#pragma once
// Manual/auto AGC switching adapted from the SDRPP fork by qrp73 (https://github.com/qrp73/SDRPP).
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
            agc.init(1.0, agcAttack, agcDecay, 10e6, 10.0, INFINITY);

            // The float path needs a complex scratch for the translator; the
            // stereo path translates into the caller's output instead (see
            // process()) and only needs the mono AGC scratch.
            if constexpr (std::is_same_v<T, float>) {
                xlatorOut.allocate(STREAM_BUFFER_SIZE);
            }
            else if constexpr (std::is_same_v<T, stereo_t>) {
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
            if constexpr (std::is_same_v<T, float>) {
                xlator.process(count, in, xlatorOut);
                dsp::convert::ComplexToReal::process(count, xlatorOut, out);
                agc.process(count, out, out);
            }
            else if constexpr (std::is_same_v<T, stereo_t>) {
                // `out` holds `count` stereo samples, which is exactly the room
                // `count` complex samples need, and nothing else writes it until
                // MonoToStereo at the end. So the translator borrows it as scratch.
                // No sample is read or written through complex_t here: the
                // translator, ComplexToReal and MonoToStereo all touch this
                // memory only inside VOLK, as lv_32fc_t, the same way
                // MonoToStereo has always written the stereo output. Keep it
                // that way: a plain C++ loop over `xlated` would break strict
                // aliasing.
                static_assert(sizeof(stereo_t) == sizeof(complex_t) && alignof(stereo_t) == alignof(complex_t),
                              "CW borrows the stereo output as complex scratch");
                complex_t* xlated = reinterpret_cast<complex_t*>(out);

                xlator.process(count, in, xlated);
                dsp::convert::ComplexToReal::process(count, xlated, agcOut);
                agc.process(count, agcOut, agcOut);
                convert::MonoToStereo::process(count, agcOut, out);
            }
            else {
                static_assert(dependent_false_v<T>, "Unsupported sample type");
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
        buffer::Buffer<complex_t> xlatorOut;   // scratch, float path only
        dsp::loop::AGC<float> agc;
        buffer::Buffer<float> agcOut;   // scratch, stereo path only

    };
}