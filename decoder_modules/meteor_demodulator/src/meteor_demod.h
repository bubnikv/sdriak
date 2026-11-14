#pragma once
#include <dsp/taps/root_raised_cosine.h>
#include <dsp/filter/fir.h>
#include <dsp/loop/fast_agc.h>
#include "meteor_costas.h"
#include <dsp/clock_recovery/mm.h>

namespace dsp::demod {
    class Meteor : public Processor<complex_t, complex_t> {
        using base_type = Processor<complex_t, complex_t>;
    public:
        Meteor() {}

        Meteor(stream<complex_t>* in, double symbolrate, double samplerate, int rrcTapCount, double rrcBeta, double agcRate, double costasBandwidth, bool brokenModulation, bool oqpsk, double omegaGain, double muGain, double omegaRelLimit = 0.01) {
            init(in, symbolrate, samplerate, rrcTapCount, rrcBeta, agcRate, costasBandwidth, brokenModulation, oqpsk, omegaGain, muGain);
        }

        ~Meteor() {
            if (!base_type::_block_init) { return; }
            base_type::stop();
        }

        void init(stream<complex_t>* in, double symbolrate, double samplerate, int rrcTapCount, double rrcBeta, double agcRate, double costasBandwidth, bool brokenModulation, bool oqpsk, double omegaGain, double muGain, double omegaRelLimit = 0.01) {
            _symbolrate = symbolrate;
            _samplerate = samplerate;
            _rrcTapCount = rrcTapCount;
            _rrcBeta = rrcBeta;
            _oqpsk = oqpsk;
            
            rrc.init(taps::rootRaisedCosine<float>(_rrcTapCount, _rrcBeta, _symbolrate, _samplerate));
            agc.init(1.0, 10e6, agcRate);
            costas.init(costasBandwidth, brokenModulation);
            recov.init(_samplerate / _symbolrate,  omegaGain, muGain, omegaRelLimit);


            base_type::init(in);
        }

        void setSymbolrate(double symbolrate) {
            assert(base_type::_block_init);
            std::lock_guard<std::recursive_mutex> lck(base_type::ctrlMtx);
            dsp::block::TempStop guard(this);
            _symbolrate = symbolrate;
            rrc.setTaps(taps::rootRaisedCosine<float>(_rrcTapCount, _rrcBeta, _symbolrate, _samplerate));
            recov.setOmega(_samplerate / _symbolrate);
        }

        void setSamplerate(double samplerate) {
            assert(base_type::_block_init);
            std::lock_guard<std::recursive_mutex> lck(base_type::ctrlMtx);
            dsp::block::TempStop guard(this);
            _samplerate = samplerate;
            rrc.setTaps(taps::rootRaisedCosine<float>(_rrcTapCount, _rrcBeta, _symbolrate, _samplerate));
            recov.setOmega(_samplerate / _symbolrate);
        }

        void setRRCParams(int rrcTapCount, double rrcBeta) {
            assert(base_type::_block_init);
            std::lock_guard<std::recursive_mutex> lck(base_type::ctrlMtx);
            dsp::block::TempStop guard(this);
            _rrcTapCount = rrcTapCount;
            _rrcBeta = rrcBeta;
            rrc.setTaps(taps::rootRaisedCosine<float>(_rrcTapCount, _rrcBeta, _symbolrate, _samplerate));
        }

        void setRRCTapCount(int rrcTapCount) {
            setRRCParams(rrcTapCount, _rrcBeta);
        }

        void setRRCBeta(int rrcBeta) {
            setRRCParams(_rrcTapCount, rrcBeta);
        }

        void setAGCRate(double agcRate) {
            assert(base_type::_block_init);
            std::lock_guard<std::recursive_mutex> lck(base_type::ctrlMtx);
            agc.setRate(agcRate);
        }

        void setCostasBandwidth(double bandwidth) {
            assert(base_type::_block_init);
            std::lock_guard<std::recursive_mutex> lck(base_type::ctrlMtx);
            costas.setBandwidth(bandwidth);
        }

        void setMMParams(double omegaGain, double muGain, double omegaRelLimit = 0.01) {
            assert(base_type::_block_init);
            std::lock_guard<std::recursive_mutex> lck(base_type::ctrlMtx);
            recov.setOmegaGain(omegaGain);
            recov.setMuGain(muGain);
            recov.setOmegaRelLimit(omegaRelLimit);
        }

        void setOmegaGain(double omegaGain) {
            assert(base_type::_block_init);
            std::lock_guard<std::recursive_mutex> lck(base_type::ctrlMtx);
            recov.setOmegaGain(omegaGain);
        }

        void setMuGain(double muGain) {
            assert(base_type::_block_init);
            std::lock_guard<std::recursive_mutex> lck(base_type::ctrlMtx);
            recov.setMuGain(muGain);
        }

        void setOmegaRelLimit(double omegaRelLimit) {
            assert(base_type::_block_init);
            std::lock_guard<std::recursive_mutex> lck(base_type::ctrlMtx);
            recov.setOmegaRelLimit(omegaRelLimit);
        }

        void setBrokenModulation(bool enabled) {
            assert(base_type::_block_init);
            std::lock_guard<std::recursive_mutex> lck(base_type::ctrlMtx);
            costas.setBrokenModulation(enabled);
        }

        void setOQPSK(bool enabled) {
            assert(base_type::_block_init);
            std::lock_guard<std::recursive_mutex> lck(base_type::ctrlMtx);
            _oqpsk = enabled;
        }

        void reset() {
            assert(base_type::_block_init);
            std::lock_guard<std::recursive_mutex> lck(base_type::ctrlMtx);
            dsp::block::TempStop guard(this);
            rrc.reset();
            agc.reset();
            costas.reset();
            recov.reset();
        }

        // IN-PLACE: safe (in == out).
        inline int process(int count, const complex_t* in, complex_t* out) {
            rrc.process(count, in, out);
            agc.process(count, out, out);
            costas.process(count, out, out);

            if (_oqpsk) {
                // Single sample delay + deinterleave
                for (int i = 0; i < count; i++) {
                    float tmp = out[i].im;
                    out[i].im = lastI;
                    lastI = tmp;
                }

                // TODO: Additional 1/24th sample delay
            }

            return recov.process(count, out, out);
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

    protected:
        double _symbolrate;
        double _samplerate;
        int _rrcTapCount;
        double _rrcBeta;
        float lastI = 0.0f;
        bool _oqpsk = false;

        filter::FIR<complex_t, float> rrc;
        loop::FastAGC<complex_t> agc;
        loop::MeteorCostas costas;
        clock_recovery::MM<complex_t> recov;
    };
}