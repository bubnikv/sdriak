#include <cmath>
#include <algorithm>
#include <dsp/sink.h>
#include <dsp/types.h>
#include <dsp/demod/quadrature.h>
#include <dsp/convert/real_to_complex.h>
#include <dsp/channel/frequency_xlator.h>
#include <dsp/filter/fir.h>
#include <dsp/math/delay.h>
#include <dsp/math/conjugate.h>
#include <dsp/channel/rx_vfo.h>
#include "vor_fm_filter.h"
#include <utils/wav.h>

#define VOR_IN_SR   25e3
#define VOR_OUT_SR  1000.0

namespace vor {
    class Receiver : public dsp::Processor<dsp::complex_t, float> {
        using base_type = dsp::Processor<dsp::complex_t, float>;
    public:
        Receiver() {}

        Receiver(dsp::stream<dsp::complex_t>* in) { init(in); }

        ~Receiver() {
            if (!base_type::_block_init) { return; }
            base_type::stop();
        }

        void init(dsp::stream<dsp::complex_t>* in, int maxBlock = STREAM_BUFFER_SIZE) {
            _maxBlock = maxBlock;

            amr2c.init();
            fmr2c.init();
            fmx.init(-9960, VOR_IN_SR);
            fmfTaps = dsp::taps::fromArray(FM_TAPS_COUNT, fm_taps);
            fmf.init(fmfTaps, _maxBlock);
            fmd.init(600, VOR_IN_SR);
            amde.init(FM_TAPS_COUNT / 2, _maxBlock);

            // Full-rate scratch, at VOR_IN_SR.
            //
            // magOut replaces what used to be a complete dsp::demod::AM<float>
            // -- an output stream, a carrier-AGC buffer and an LPF delay line,
            // about 20 MB -- that was never start()ed and never process()ed.
            // It existed so that one volk_32fc_magnitude_32f() call had
            // somewhere to write. It needed a buffer, not a demodulator.
            magOut.allocate(_maxBlock);
            amr2cOut.allocate(_maxBlock);
            fmxOut.allocate(_maxBlock);
            fmdOut.allocate(_maxBlock);
            fmr2cOut.allocate(_maxBlock);

            // The two 30 Hz detectors decimate 25:1, and everything from their
            // output onward -- including this block's own output stream -- runs
            // at VOR_OUT_SR. Sized at init(), before anything is started, so no
            // reader can be holding the old buffer; VOR_IN_SR is a constant, so
            // there is no reconfiguration path that would have to resize it.
            amv.init(VOR_IN_SR, VOR_OUT_SR, 30, 30, _maxBlock);
            fmv.init(VOR_IN_SR, VOR_OUT_SR, 30, 30, _maxBlock);
            // [RATE-SIZED] None of these three is STREAM_BUFFER_SIZE any
            // more; they hold 1/25 of a block. see doc/design/buffer-size-requirements.md 2.6
            _outMaxBlock = outBlockFor(_maxBlock);
            amvOut.allocate(_outMaxBlock);
            fmvOut.allocate(_outMaxBlock);
            base_type::out.setBufferSize(_outMaxBlock);

            base_type::init(in);
        }

        // [RATE-SIZED] ASSUMED ratio: VOR_OUT_SR / VOR_IN_SR. Both are
        // compile-time constants here, which is why the output stream can be
        // sized once at init() and never resized. + 64 for the resamplers'
        // retained phase state. see doc/design/buffer-size-requirements.md 2.6
        static int outBlockFor(int maxBlock) {
            if (maxBlock <= 0) { return maxBlock; }
            double n = std::ceil((double)maxBlock * VOR_OUT_SR / VOR_IN_SR) + 64.0;
            return (int)(std::max)(1.0, n);
        }
        
        int process(dsp::complex_t* in, float* out, int count) {
            // Demodulate the AM outer modulation
            volk_32fc_magnitude_32f(magOut, (lv_32fc_t*)in, count);
            amr2c.process(count, magOut, amr2cOut);

            // Isolate the FM subcarrier
            fmx.process(count, amr2cOut, fmxOut);
            fmf.process(count, fmxOut, fmxOut);

            // Demodulate the FM subcarrier
            fmd.process(count, fmxOut, fmdOut);
            fmr2c.process(count, fmdOut, fmr2cOut);

            // Delay the AM signal by the same amount as the FM one
            amde.process(count, amr2cOut, amr2cOut);

            // Isolate the 30Hz component on both the AM and FM channels
            int rcount = amv.process(count, amr2cOut, amvOut);
            fmv.process(count, fmr2cOut, fmvOut);

            // If no data was returned, we're done for this round
            if (!rcount) { return 0; }

            // Conjugate FM reference
            volk_32fc_conjugate_32fc((lv_32fc_t*)fmvOut.get(), (lv_32fc_t*)fmvOut.get(), rcount);

            // Multiply both together
            volk_32fc_x2_multiply_32fc((lv_32fc_t*)amvOut.get(), (lv_32fc_t*)amvOut.get(), (lv_32fc_t*)fmvOut.get(), rcount);
            
            // Compute angle
            volk_32fc_s32f_atan2_32f(out, (lv_32fc_t*)amvOut.get(), 1.0f, rcount);

            return rcount;
        }

        int run() {
            int count = base_type::_in->read();
            if (count < 0) { return -1; }

            int outCount = process(base_type::_in->readBuf, base_type::out.writeBuf, count);

            // Swap if some data was generated
            base_type::_in->flush();
            if (outCount) {
                if (!base_type::out.swap(outCount)) { return -1; }
            }
            return outCount;
        }

    private:
        int _maxBlock = STREAM_BUFFER_SIZE;
        int _outMaxBlock = STREAM_BUFFER_SIZE;
        dsp::buffer::Buffer<float> magOut;   // AM envelope, in place of a whole AM demod
        dsp::convert::RealToComplex amr2c;
        dsp::convert::RealToComplex fmr2c;
        dsp::channel::FrequencyXlator fmx;
        dsp::OwnedTap<float> fmfTaps;
        dsp::filter::FIR<dsp::complex_t, float> fmf;
        dsp::demod::Quadrature fmd;
        dsp::math::Delay<dsp::complex_t> amde;
        dsp::channel::RxVFOKernel amv;
        dsp::channel::RxVFOKernel fmv;
        dsp::buffer::Buffer<dsp::complex_t> amvOut;
        dsp::buffer::Buffer<dsp::complex_t> fmvOut;

        // Scratch for the fused kernels
        dsp::buffer::Buffer<dsp::complex_t> amr2cOut;
        dsp::buffer::Buffer<dsp::complex_t> fmxOut;
        dsp::buffer::Buffer<float> fmdOut;
        dsp::buffer::Buffer<dsp::complex_t> fmr2cOut;
    };
}