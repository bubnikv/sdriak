#pragma once
#include "quadrature.h"
#include "../taps/low_pass.h"
#include "../taps/band_pass.h"
#include "../filter/fir.h"
#include "../loop/pll.h"
#include "../convert/l_r_to_stereo.h"
#include "../convert/real_to_complex.h"
#include "../convert/complex_to_real.h"
#include "../math/conjugate.h"
#include "../math/delay.h"
#include "../math/multiply.h"
#include "../math/add.h"
#include "../math/subtract.h"
#include "../multirate/rational_resampler.h"

namespace dsp::demod {
    class BroadcastFM : public Processor<complex_t, stereo_t> {
        using base_type = Processor<complex_t, stereo_t>;
    public:
        BroadcastFM() {}

        BroadcastFM(stream<complex_t>* in, double deviation, double samplerate, bool stereo = true, bool lowPass = true, bool rdsOut = false) { init(in, deviation, samplerate, stereo, lowPass); }

        ~BroadcastFM() {
            if (!base_type::_block_init) { return; }
            base_type::stop();
        }

        // The RDS branch runs at RDS_SAMPLERATE, not at `samplerate`, so
        // everything downstream of rdsResamp is sized for that -- see
        // getRDSMaxBlock(). At the usual 250 kHz IF that is a 50:1 reduction.
        static constexpr double RDS_SAMPLERATE = 5000.0;

        // Valid after init(). This is the selected resampler bound, including
        // capped ratio approximation and retained phase. Hand it to whatever
        // consumes rdsOut; the same capacity must propagate across the module
        // boundary into RDSDemod.
        int getRDSMaxBlock() const { return _rdsMaxBlock; }

        virtual void init(stream<complex_t>* in, double deviation, double samplerate, bool stereo = true, bool lowPass = true, bool rdsOut = false, int maxBlock = STREAM_BUFFER_SIZE) {
            _maxBlock = maxBlock;
            _deviation = deviation;
            _samplerate = samplerate;
            _stereo = stereo;
            _lowPass = lowPass;
            _rdsOut = rdsOut;
            
            // Every sub-block below is *fused*: it is never start()ed and never
            // swap()ed, process() calls it inline. So none of them needs a
            // dsp::stream -- two buffers plus two mutexes and two condition
            // variables for handing data to a thread that does not exist. They
            // get a plain scratch buffer and an explicit block bound instead.
            demod.init(_deviation, _samplerate);
            pilotFir.init(taps::bandPass<complex_t>(18750.0, 19250.0, 3000.0, _samplerate, true), _maxBlock);
            rtoc.init();
            pilotPLL.init(25000.0 / _samplerate, 0.0, math::hzToRads(19000.0, _samplerate), math::hzToRads(18750.0, _samplerate), math::hzToRads(19250.0, _samplerate));
            lprDelay.init(((pilotFir.tapCount() - 1) / 2) + 1, _maxBlock);
            lmrDelay.init(((pilotFir.tapCount() - 1) / 2) + 1, _maxBlock);
            // Both channels use one tap set: alFir owns it, arFir borrows it.
            alFir.init(taps::lowPass(15000.0, 4000.0, _samplerate), _maxBlock);
            arFir.init(alFir.ownedTaps(), _maxBlock);
            xlator.init(-57000.0, samplerate);
            rdsResamp.init(samplerate, RDS_SAMPLERATE, _maxBlock);

            // [RATE-SIZED] rdsOut publishes ~1/50 of a block at the usual
            // 250 kHz IF, but its backing storage is also the resampler's
            // workspace. The first stage of the 32:1 power decimator writes a
            // larger intermediate block there. Query both bounds from the
            // producer rather than recomputing the requested rate ratio:
            // MAX_INTERP approximation can round upward or select passthrough.
            // Nothing is started yet, so no reader can hold the old buffer.
            // see doc/design/buffer-size-requirements.md 2.6
            _rdsMaxBlock = rdsResamp.maxOutputBlock();
            _rdsWorkMaxBlock = rdsResamp.maxOutputBuffer();
            // this-> because init()'s `bool rdsOut` parameter shadows the
            // stream member of the same name.
            this->rdsOut.setBufferSize(_rdsWorkMaxBlock);

            // Scratch for the fused stages. Every stage that can runs in place
            // on a buffer owned by an earlier one; see process() for who holds
            // what when.
            demodOut.allocate(_maxBlock);
            rtocOut.allocate(_maxBlock);
            pilotOut.allocate(_maxBlock);
            lmr.allocate(_maxBlock);
            l.allocate(_maxBlock);

            base_type::init(in);
        }

        void setDeviation(double deviation) {
            assert(base_type::_block_init);
            std::lock_guard<std::recursive_mutex> lck(base_type::ctrlMtx);
            _deviation = deviation;
            demod.setDeviation(_deviation, _samplerate);
        }

        void setSamplerate(double samplerate) {
            assert(base_type::_block_init);
            std::lock_guard<std::recursive_mutex> lck(base_type::ctrlMtx);
            dsp::block::TempStop guard(this);

            // Preview the producer's selected ratio and grow its destination
            // before reconfiguring it. If allocation throws, the old producer
            // and capacity still agree; if a later setter throws, the excess
            // capacity is harmless.
            //
            // PRECONDITION: the caller must have stopped whatever consumes
            // rdsOut. TempStop above stops this block, not the reader, and
            // setBufferSize() frees the buffer that reader may still hold.
            // Grow-only, so a rate that needs less keeps the larger buffer
            // until the next init() -- memory, never correctness. (No caller
            // today: WFM's IF rate is the constant 250 kHz.)
            // [RATE-SIZED] see doc/design/buffer-size-requirements.md 2.6
            const int wantedOutput = multirate::RationalResampler<complex_t>::maxOutputBlockForRates(
                samplerate, RDS_SAMPLERATE, _maxBlock);
            const int wantedWork = multirate::RationalResampler<complex_t>::maxOutputBufferForRates(
                samplerate, RDS_SAMPLERATE, _maxBlock);
            if (wantedWork > _rdsWorkMaxBlock) {
                rdsOut.setBufferSize(wantedWork);
                _rdsWorkMaxBlock = wantedWork;
            }

            _samplerate = samplerate;

            demod.setDeviation(_deviation, _samplerate);
            pilotFir.setTaps(taps::bandPass<complex_t>(18750.0, 19250.0, 3000.0, samplerate, true));
            
            pilotPLL.setFrequencyLimits(math::hzToRads(18750.0, _samplerate), math::hzToRads(19250.0, _samplerate));
            pilotPLL.setInitialFreq(math::hzToRads(19000.0, _samplerate));
            lprDelay.setDelay(((pilotFir.tapCount() - 1) / 2) + 1);
            lmrDelay.setDelay(((pilotFir.tapCount() - 1) / 2) + 1);

            // arFir points at the set alFir has just freed until it is
            // re-pointed on the next line; the worker is stopped meanwhile.
            alFir.setTaps(taps::lowPass(15000.0, 4000.0, _samplerate));
            arFir.setTaps(alFir.ownedTaps());

            xlator.setOffset(-57000.0, samplerate);
            rdsResamp.setInSamplerate(samplerate);
            _rdsMaxBlock = wantedOutput;
            assert(rdsResamp.maxOutputBlock() == _rdsMaxBlock);
            assert(rdsResamp.maxOutputBuffer() <= _rdsWorkMaxBlock);

            reset();
        }

        void setStereo(bool stereo) {
            assert(base_type::_block_init);
            std::lock_guard<std::recursive_mutex> lck(base_type::ctrlMtx);
            dsp::block::TempStop guard(this);
            _stereo = stereo;
            reset();
        }

        void setLowPass(bool lowPass) {
            assert(base_type::_block_init);
            std::lock_guard<std::recursive_mutex> lck(base_type::ctrlMtx);
            dsp::block::TempStop guard(this);
            _lowPass = lowPass;
            reset();
        }

        void setRDSOut(bool rdsOut) {
            assert(base_type::_block_init);
            std::lock_guard<std::recursive_mutex> lck(base_type::ctrlMtx);
            dsp::block::TempStop guard(this);
            _rdsOut = rdsOut;
            reset();
        }

        void reset() {
            assert(base_type::_block_init);
            std::lock_guard<std::recursive_mutex> lck(base_type::ctrlMtx);
            dsp::block::TempStop guard(this);
            demod.reset();
            pilotFir.reset();
            pilotPLL.reset();
            lprDelay.reset();
            lmrDelay.reset();
            alFir.reset();
            arFir.reset();
        }

        inline int process(int count, complex_t* in, stereo_t* out, int& rdsOutCount, complex_t* rdsout = NULL) {
            // Demodulate
            demod.process(count, in, demodOut);
            if (_stereo) {
                // Two complex buffers carry the whole stereo path:
                //   rtocOut  -- the MPX as complex, later delayed and mixed
                //               down in place into L-R
                //   pilotOut -- first the RDS branch's translated copy, then
                //               the filtered pilot and the PLL reference
                // The RDS branch runs first so that rtocOut is still the
                // undelayed MPX and pilotOut is still free. The stages keep
                // their own state, so the order within a block changes nothing.

                // Convert to complex
                rtoc.process(count, demodOut, rtocOut);

                // Do RDS demod
                if (_rdsOut) {
                    // Translate to 0Hz
                    xlator.process(count, rtocOut, pilotOut);

                    // Resample to the output samplerate
                    rdsOutCount = rdsResamp.process(count, pilotOut, rdsout);
                }

                // Filter out pilot and run through PLL
                pilotFir.process(count, rtocOut, pilotOut);
                pilotPLL.process(count, pilotOut, pilotOut);

                // Delay
                lprDelay.process(count, demodOut, demodOut);
                lmrDelay.process(count, rtocOut, rtocOut);

                // conjugate PLL output to down convert twice the L-R signal
                math::Conjugate::process(count, pilotOut, pilotOut);
                math::Multiply<dsp::complex_t>::process(count, rtocOut, pilotOut, rtocOut);
                math::Multiply<dsp::complex_t>::process(count, rtocOut, pilotOut, rtocOut);

                // Convert output back to real for further processing
                convert::ComplexToReal::process(count, rtocOut, lmr);

                // Amplify by 2x
                volk_32f_s32f_multiply_32f(lmr, lmr, 2.0f, count);

                // Do L = (L+R) + (L-R), R = (L+R) - (L-R). R overwrites L-R,
                // which is no longer needed once L has been formed.
                math::Add<float>::process(count, demodOut, lmr, l);
                math::Subtract<float>::process(count, demodOut, lmr, lmr);
                float* r = lmr;

                // Filter if needed
                if (_lowPass) {
                    alFir.process(count, l, l);
                    arFir.process(count, r, r);
                }

                // Interleave into stereo
                convert::LRToStereo::process(count, l, r, out);
            }
            else {
                // Process RDS if needed. Note: find a way to not have to copy half the code from the stereo demod
                if (_rdsOut) {
                    // Convert to complex
                    rtoc.process(count, demodOut, rtocOut);

                    // Translate to 0Hz
                    xlator.process(count, rtocOut, rtocOut);

                    // Resample to the output samplerate
                    rdsOutCount = rdsResamp.process(count, rtocOut, rdsout);
                }

                // Filter if needed
                if (_lowPass) {
                    alFir.process(count, demodOut, demodOut);
                }

                // Interleave raw MPX to stereo
                convert::LRToStereo::process(count, demodOut, demodOut, out);
            }

            return count;
        }

        int run() {
            int count = base_type::_in->read();
            if (count < 0) { return -1; }

            int rdsOutCount = 0;
            process(count, base_type::_in->readBuf, base_type::out.writeBuf, rdsOutCount, rdsOut.writeBuf);

            base_type::_in->flush();
            if (!base_type::out.swap(count)) { return -1; }
            if (rdsOutCount && _rdsOut) {
                if (!rdsOut.swap(rdsOutCount)) { return -1; }
            }
            return count;
        }

        stream<complex_t> rdsOut;

    protected:
        int _maxBlock = STREAM_BUFFER_SIZE;
        int _rdsMaxBlock = STREAM_BUFFER_SIZE;
        int _rdsWorkMaxBlock = STREAM_BUFFER_SIZE;
        double _deviation;
        double _samplerate;
        bool _stereo;
        bool _lowPass;
        bool _rdsOut;

        Quadrature demod;
        filter::FIR<complex_t, complex_t> pilotFir;
        convert::RealToComplex rtoc;
        channel::FrequencyXlator xlator;
        loop::PLL pilotPLL;
        math::Delay<float> lprDelay;
        math::Delay<complex_t> lmrDelay;
        // alFir owns the audio taps and arFir borrows them, so alFir is
        // declared first and outlives it.
        filter::FIR<float, float> alFir;
        filter::FIR<float, float> arFir;
        multirate::RationalResampler<dsp::complex_t> rdsResamp;

        // Scratch for the fused stages, in place of their (unused) out streams.
        buffer::Buffer<float> demodOut;
        buffer::Buffer<complex_t> rtocOut;   // MPX as complex, then L-R in place
        buffer::Buffer<complex_t> pilotOut;  // RDS translate, then pilot / PLL reference

        buffer::Buffer<float> lmr;           // L-R, then R in place
        buffer::Buffer<float> l;
        
    };
}
