#pragma once
#include <cmath>
#include <algorithm>
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

        // Largest block the RDS output can carry, for a given input bound.
        // Deliberately not clamped to maxBlock: if someone configures an input
        // rate below RDS_SAMPLERATE the branch interpolates, and under-sizing
        // it would overflow rather than merely waste.
        // [RATE-SIZED] ASSUMED ratio: RDS_SAMPLERATE / samplerate. This bound
        // leaves the object -- getRDSMaxBlock() hands it to RDSDemod, which
        // sizes its own streams and buffers from it -- so an error here
        // propagates across a module boundary. see doc/design/buffer-size-requirements.md 2.6
        static int rdsBlockFor(int maxBlock, double samplerate) {
            if (!(samplerate > 0.0) || maxBlock <= 0) { return maxBlock; }
            // + 64 for the resampler's retained phase state, which can emit one
            // extra sample beyond the flat ratio on any given block.
            double n = std::ceil((double)maxBlock * RDS_SAMPLERATE / samplerate) + 64.0;
            return (int)(std::max)(1.0, n);
        }

        // Valid after init(). Hand this to whatever consumes rdsOut.
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
            pilotFirTaps = taps::bandPass<complex_t>(18750.0, 19250.0, 3000.0, _samplerate, true);
            pilotFir.init(pilotFirTaps, _maxBlock);
            rtoc.init();
            pilotPLL.init(25000.0 / _samplerate, 0.0, math::hzToRads(19000.0, _samplerate), math::hzToRads(18750.0, _samplerate), math::hzToRads(19250.0, _samplerate));
            lprDelay.init(((pilotFirTaps.size - 1) / 2) + 1, _maxBlock);
            lmrDelay.init(((pilotFirTaps.size - 1) / 2) + 1, _maxBlock);
            audioFirTaps = taps::lowPass(15000.0, 4000.0, _samplerate);
            alFir.init(audioFirTaps, _maxBlock);
            arFir.init(audioFirTaps, _maxBlock);
            xlator.init(-57000.0, samplerate);
            rdsResamp.init(samplerate, RDS_SAMPLERATE, _maxBlock);

            // [RATE-SIZED] rdsOut is no longer STREAM_BUFFER_SIZE; it holds
            // ~1/50 of a block at the usual 250 kHz IF. Nothing is started yet,
            // so no reader can hold the old buffer. see doc/design/buffer-size-requirements.md 2.6
            _rdsMaxBlock = rdsBlockFor(_maxBlock, samplerate);
            // this-> because init()'s `bool rdsOut` parameter shadows the
            // stream member of the same name.
            this->rdsOut.setBufferSize(_rdsMaxBlock);

            // Scratch for the fused stages. lprDelay, alFir, arFir, xlator and
            // rdsResamp need none: they write into a buffer owned by an earlier
            // stage, or into the caller's RDS output.
            demodOut.allocate(_maxBlock);
            rtocOut.allocate(_maxBlock);
            pilotFirOut.allocate(_maxBlock);
            pilotPLLOut.allocate(_maxBlock);
            lmrDelayOut.allocate(_maxBlock);
            lmr.allocate(_maxBlock);
            l.allocate(_maxBlock);
            r.allocate(_maxBlock);

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
            _samplerate = samplerate;

            demod.setDeviation(_deviation, _samplerate);
            pilotFirTaps = taps::bandPass<complex_t>(18750.0, 19250.0, 3000.0, samplerate, true);
            pilotFir.setTaps(pilotFirTaps);
            
            pilotPLL.setFrequencyLimits(math::hzToRads(18750.0, _samplerate), math::hzToRads(19250.0, _samplerate));
            pilotPLL.setInitialFreq(math::hzToRads(19000.0, _samplerate));
            lprDelay.setDelay(((pilotFirTaps.size - 1) / 2) + 1);
            lmrDelay.setDelay(((pilotFirTaps.size - 1) / 2) + 1);

            audioFirTaps = taps::lowPass(15000.0, 4000.0, _samplerate);
            alFir.setTaps(audioFirTaps);
            arFir.setTaps(audioFirTaps);

            xlator.setOffset(-57000.0, samplerate);
            rdsResamp.setInSamplerate(samplerate);

            // Keep the RDS branch sized for the new rate; a lower IF rate
            // means *more* RDS samples per block.
            //
            // PRECONDITION: the caller must have stopped whatever consumes
            // rdsOut. TempStop above stops this block, not the reader, and
            // setBufferSize() frees the buffer that reader may still hold.
            // Grow-only, so a rate that needs less keeps the larger buffer
            // until the next init() -- memory, never correctness. (No caller
            // today: WFM's IF rate is the constant 250 kHz.)
            // [RATE-SIZED] see doc/design/buffer-size-requirements.md 2.6
            int wanted = rdsBlockFor(_maxBlock, samplerate);
            if (wanted > _rdsMaxBlock) {
                _rdsMaxBlock = wanted;
                rdsOut.setBufferSize(_rdsMaxBlock);
            }

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
                // Convert to complex
                rtoc.process(count, demodOut, rtocOut);

                // Filter out pilot and run through PLL
                pilotFir.process(count, rtocOut, pilotFirOut);
                pilotPLL.process(count, pilotFirOut, pilotPLLOut);

                // Delay
                lprDelay.process(count, demodOut, demodOut);
                lmrDelay.process(count, rtocOut, lmrDelayOut);

                // conjugate PLL output to down convert twice the L-R signal
                math::Conjugate::process(count, pilotPLLOut, pilotPLLOut);
                math::Multiply<dsp::complex_t>::process(count, lmrDelayOut, pilotPLLOut, lmrDelayOut);
                math::Multiply<dsp::complex_t>::process(count, lmrDelayOut, pilotPLLOut, lmrDelayOut);

                // Do RDS demod
                if (_rdsOut) {
                    // Translate to 0Hz
                    xlator.process(count, rtocOut, rtocOut);

                    // Resample to the output samplerate
                    rdsOutCount = rdsResamp.process(count, rtocOut, rdsout);
                }

                // Convert output back to real for further processing
                convert::ComplexToReal::process(count, lmrDelayOut, lmr);

                // Amplify by 2x
                volk_32f_s32f_multiply_32f(lmr, lmr, 2.0f, count);

                // Do L = (L+R) + (L-R), R = (L+R) - (L-R)
                math::Add<float>::process(count, demodOut, lmr, l);
                math::Subtract<float>::process(count, demodOut, lmr, r);

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
        double _deviation;
        double _samplerate;
        bool _stereo;
        bool _lowPass;
        bool _rdsOut;

        Quadrature demod;
        OwnedTap<complex_t> pilotFirTaps;
        filter::FIR<complex_t, complex_t> pilotFir;
        convert::RealToComplex rtoc;
        channel::FrequencyXlator xlator;
        loop::PLL pilotPLL;
        math::Delay<float> lprDelay;
        math::Delay<complex_t> lmrDelay;
        OwnedTap<float> audioFirTaps;
        filter::FIR<float, float> arFir;
        filter::FIR<float, float> alFir;
        multirate::RationalResampler<dsp::complex_t> rdsResamp;

        // Scratch for the fused stages, in place of their (unused) out streams.
        buffer::Buffer<float> demodOut;
        buffer::Buffer<complex_t> rtocOut;
        buffer::Buffer<complex_t> pilotFirOut;
        buffer::Buffer<complex_t> pilotPLLOut;
        buffer::Buffer<complex_t> lmrDelayOut;

        buffer::Buffer<float> lmr;
        buffer::Buffer<float> l;
        buffer::Buffer<float> r;
        
    };
}