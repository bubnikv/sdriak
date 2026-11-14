#pragma once
// Manual/auto AGC switching adapted from the SDRPP fork by qrp73 (https://github.com/qrp73/SDRPP).
#include "../processor.h"
#include "../loop/agc.h"
#include "../correction/dc_blocker.h"
#include "../convert/mono_to_stereo.h"
#include "../filter/fir.h"
#include "../taps/low_pass.h"

namespace dsp::demod {
    template <class T>
    class AM : public Processor<dsp::complex_t, T> {
        using base_type = Processor<dsp::complex_t, T>;
    public:
        enum AGCMode {
            OFF,
            CARRIER,
            AUDIO,
        };

        AM() {}

        AM(stream<complex_t>* in, AGCMode agcMode, double bandwidth, double agcAttack, double agcDecay, double dcBlockRate, double samplerate) { init(in, agcMode, bandwidth, agcAttack, agcDecay, dcBlockRate, samplerate); }

        ~AM() {
            if (!base_type::_block_init) { return; }
            base_type::stop();
        }

        void init(stream<complex_t>* in, AGCMode agcMode, double bandwidth, double agcAttack, double agcDecay, double dcBlockRate, double samplerate) {
            _agcMode = agcMode;
            _bandwidth = bandwidth;
            _samplerate = samplerate;

            carrierAgc.init(1.0, agcAttack, agcDecay, 10e6, 10.0, INFINITY);
            audioAgc.init(1.0, agcAttack, agcDecay, 10e6, 10.0, INFINITY);
            dcBlock.init(NULL, dcBlockRate);
            lpf.init(taps::lowPass(bandwidth / 2.0, (bandwidth / 2.0) * 0.1, samplerate));

            audioAgc.setEnabled(_agcMode == AGCMode::AUDIO);

            // Only the stereo path needs an audio scratch buffer; the float
            // path works directly in the caller's output.
            if constexpr (std::is_same_v<T, stereo_t>) {
                audioAgcOut.allocate(STREAM_BUFFER_SIZE);
            }
            dcBlock.out.free();
            
            base_type::init(in);
        }

        void setAGCMode(AGCMode agcMode) {
            assert(base_type::_block_init);
            std::lock_guard<std::recursive_mutex> lck(base_type::ctrlMtx);
            dsp::block::TempStop guard(this);
            // Carry the current gain to the newly active AGC so the audio level doesn't jump
            // on a mode switch (setGain also seeds the AGC envelope)
            float gain = (_agcMode == AGCMode::CARRIER) ? carrierAgc.getGain() : audioAgc.getGain();
            _agcMode = agcMode;
            audioAgc.setEnabled(agcMode == AGCMode::AUDIO);
            if (agcMode == AGCMode::CARRIER) {
                carrierAgc.setGain(gain);
            }
            else {
                audioAgc.setGain(gain);
            }
        }

        void setAGCGain(float gain) {
            assert(base_type::_block_init);
            std::lock_guard<std::recursive_mutex> lck(base_type::ctrlMtx);
            audioAgc.setGain(gain);
        }

        float getAGCGain() {
            assert(base_type::_block_init);
            return (_agcMode == AGCMode::CARRIER) ? carrierAgc.getGain() : audioAgc.getGain();
        }

        void setBandwidth(double bandwidth) {
            assert(base_type::_block_init);
            std::lock_guard<std::recursive_mutex> lck(base_type::ctrlMtx);
            if (bandwidth == _bandwidth) { return; }
            _bandwidth = bandwidth;
            std::lock_guard<std::mutex> lck2(lpfMtx);
            lpf.setTaps(taps::lowPass(_bandwidth / 2.0, (_bandwidth / 2.0) * 0.1, _samplerate));
        }

        void setAGCAttack(double attack) {
            assert(base_type::_block_init);
            std::lock_guard<std::recursive_mutex> lck(base_type::ctrlMtx);
            carrierAgc.setAttack(attack);
            audioAgc.setAttack(attack);
        }

        void setAGCDecay(double decay) {
            assert(base_type::_block_init);
            std::lock_guard<std::recursive_mutex> lck(base_type::ctrlMtx);
            carrierAgc.setDecay(decay);
            audioAgc.setDecay(decay);
        }

        void setDCBlockRate(double rate) {
            assert(base_type::_block_init);
            std::lock_guard<std::recursive_mutex> lck(base_type::ctrlMtx);
            dcBlock.setRate(rate);
        }

        // TODO: Implement setSamplerate

        void reset() {
            assert(base_type::_block_init);
            std::lock_guard<std::recursive_mutex> lck(base_type::ctrlMtx);
            dsp::block::TempStop guard(this);
            carrierAgc.reset();
            audioAgc.reset();
            dcBlock.reset();
        }

        // The carrier AGC runs on the envelope rather than on the complex IQ.
        // Its gain is computed from |in[i]| either way, and |in[i] * g| equals
        // |in[i]| * g, so scaling the magnitudes gives the same samples (up to
        // float rounding) without a complex-sized scratch buffer. The
        // equivalence holds for finite, normalized IQ. If |in[i]| overflows
        // to inf, the old path output 0 and the new one outputs NaN, but
        // both leave the AGC permanently broken until reset().
        int process(int count, complex_t* in, T* out) {
            if constexpr (std::is_same_v<T, float>) {
                volk_32fc_magnitude_32f(out, (lv_32fc_t*)in, count);
                if (_agcMode == AGCMode::CARRIER) {
                    carrierAgc.process(count, out, out);
                }
                dcBlock.process(count, out, out);
                if (_agcMode != AGCMode::CARRIER) {
                    audioAgc.process(count, out, out);
                }
                {
                    std::lock_guard<std::mutex> lck(lpfMtx);
                    lpf.process(count, out, out);
                }
            }
            else if constexpr (std::is_same_v<T, stereo_t>) {
                volk_32fc_magnitude_32f(audioAgcOut, (lv_32fc_t*)in, count);
                if (_agcMode == AGCMode::CARRIER) {
                    carrierAgc.process(count, audioAgcOut, audioAgcOut);
                }
                dcBlock.process(count, audioAgcOut, audioAgcOut);
                if (_agcMode != AGCMode::CARRIER) {
                    audioAgc.process(count, audioAgcOut, audioAgcOut);
                }
                {
                    std::lock_guard<std::mutex> lck(lpfMtx);
                    lpf.process(count, audioAgcOut, audioAgcOut);
                }
                convert::MonoToStereo::process(count, audioAgcOut, out);
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

    protected:
        AGCMode _agcMode;

        double _samplerate;
        double _bandwidth;

        loop::AGC<float> carrierAgc;               // on the envelope, before the DC block
        loop::AGC<float> audioAgc;
        buffer::Buffer<float> audioAgcOut;         // scratch, stereo path only
        correction::DCBlocker<float> dcBlock;
        filter::FIR<float, float> lpf;
        std::mutex lpfMtx;

    };
}