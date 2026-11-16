#pragma once
#include <dsp/processor.h>
#include <dsp/loop/fast_agc.h>
#include <dsp/loop/costas.h>
#include <dsp/taps/band_pass.h>
#include <dsp/filter/fir.h>
#include <dsp/convert/complex_to_real.h>
#include <dsp/clock_recovery/mm.h>
#include <dsp/digital/binary_slicer.h>
#include <dsp/digital/differential_decoder.h>
#include <dsp/buffer/frame_tap.h>

class RDSDemod : public dsp::Processor<dsp::complex_t, uint8_t> {
    using base_type = dsp::Processor<dsp::complex_t, uint8_t>;
public:
    RDSDemod() {}
    RDSDemod(dsp::stream<dsp::complex_t>* in, bool enableSoft, int maxBlock = STREAM_BUFFER_SIZE) { init(in, enableSoft, maxBlock); }
    ~RDSDemod() {
        // Owning members are destroyed before ~block() runs, so the worker must
        // be joined here or it keeps reading freed buffers.
        if (!base_type::_block_init) { return; }
        base_type::stop();
    }

    // maxBlock is the largest block this demodulator can be handed, which is
    // whatever BroadcastFM's RDS branch emits -- 1/50 of the full rate at the
    // usual 250 kHz IF. Everything below runs at 5 kHz, so sizing it for a
    // full-rate block wastes ~50x on every buffer and on the output stream.
    void init(dsp::stream<dsp::complex_t>* in, bool enableSoft, int maxBlock = STREAM_BUFFER_SIZE) {
        // Save config
        this->enableSoft = enableSoft;

        // Initialize the DSP
        agc.init(1.0, 1e6, 0.1);
        costas.init(0.005f);
        fir.init(dsp::taps::bandPass<dsp::complex_t>(0, 2375, 100, 5000), maxBlock);   // [RATE-SIZED]
        double baudfreq = dsp::math::hzToRads(2375.0/2.0, 5000);
        costas2.init(0.01, 0.0, baudfreq, baudfreq - (baudfreq*0.1), baudfreq + (baudfreq*0.1));
        recov.init(5000.0 / (2375.0 / 2.0), 1e-6, 0.01, 0.01, 128, 8, maxBlock);
        diff.init(2);

        // [RATE-SIZED] Every buffer and the output stream below are sized from
        // `maxBlock`, which the caller derived from BroadcastFM's RDS
        // decimation ratio, not from STREAM_BUFFER_SIZE, and process()
        // bound-checks none of them. see doc/design/buffer-size-requirements.md 2.6
        // One complex scratch: the AGC copies out of the input stream, and every
        // later complex stage (costas, fir, costas2) runs in place on it.
        scratch.allocate(maxBlock);

        // The soft symbols, which the clock recovery writes in place over the
        // real part -- one per input sample at most. They are not published
        // as a stream; they only go to softTap.
        softBuf.allocate(maxBlock);

        // `out` carries at most one symbol per ~4.2 input samples. Sized before
        // anything is started, so no reader can be holding the old buffer.
        base_type::out.setBufferSize(maxBlock);

        // Init the rest
        base_type::init(in);
    }

    void setSoftEnabled(bool enable) {
        assert(base_type::_block_init);
        std::lock_guard<std::recursive_mutex> lck(base_type::ctrlMtx);
        dsp::block::TempStop guard(this);
        enableSoft = enable;
    }

    void reset() {
        assert(base_type::_block_init);
        std::lock_guard<std::recursive_mutex> lck(base_type::ctrlMtx);
        dsp::block::TempStop guard(this);
        agc.reset();
        costas.reset();
        fir.reset();
        costas2.reset();
        recov.reset();
        diff.reset();
    }

    inline int process(int count, dsp::complex_t* in, float* softOut, uint8_t* hardOut) {
        count = agc.process(count, in, scratch);
        count = costas.process(count, scratch, scratch);
        count = fir.process(count, scratch, scratch);
        count = costas2.process(count, scratch, scratch);
        count = dsp::convert::ComplexToReal::process(count, scratch, softOut);
        count = recov.process(count, softOut, softOut);
        count = dsp::digital::BinarySlicer::process(count, softOut, hardOut);
        count = diff.process(count, hardOut, hardOut);
        return count;
    }

    int run() {
        int count = base_type::_in->read();
        if (count < 0) { return -1; }

        count = process(count, base_type::_in->readBuf, softBuf, base_type::out.writeBuf);

        base_type::_in->flush();
        // Inline and non-blocking, so the diagnostic display cannot hold up
        // the demodulator.
        if (enableSoft) { softTap.push(softBuf, count); }
        if (!base_type::out.swap(count)) { return -1; }
        return count;
    }

    // Soft symbols for the diagnostic display. Initialise it before start();
    // it is fed only while soft output is enabled.
    dsp::buffer::FrameTap<float> softTap;

private:
    bool enableSoft = false;
    
    dsp::buffer::Buffer<dsp::complex_t> scratch;
    dsp::buffer::Buffer<float> softBuf;

    dsp::loop::FastAGC<dsp::complex_t> agc;
    dsp::loop::Costas<2> costas;
    dsp::filter::FIR<dsp::complex_t, dsp::complex_t> fir;
    dsp::loop::Costas<2> costas2;
    dsp::clock_recovery::MM<float> recov;
    dsp::digital::DifferentialDecoder diff;
};