#pragma once
#include <cmath>
#include <algorithm>
#include <dsp/stream.h>
#include <dsp/buffer/reshaper.h>
#include <dsp/multirate/rational_resampler.h>
#include <dsp/sink/handler_sink.h>
#include <dsp/demod/quadrature.h>
#include <dsp/clock_recovery/mm.h>
#include <dsp/taps/root_raised_cosine.h>
#include <dsp/correction/dc_blocker.h>
#include <dsp/loop/fast_agc.h>
#include <dsp/digital/binary_slicer.h>
#include <dsp/routing/doubler.h>

class POCSAGDSP : public dsp::Processor<dsp::complex_t, uint8_t> {
    using base_type = dsp::Processor<dsp::complex_t, uint8_t>;
public:
    POCSAGDSP() {}
    POCSAGDSP(dsp::stream<dsp::complex_t>* in, double samplerate, double baudrate) { init(in, samplerate, baudrate); }

    ~POCSAGDSP() {
        // Owning members are destroyed before ~block() runs, so the worker must
        // be joined here or it keeps reading freed buffers.
        if (!base_type::_block_init) { return; }
        base_type::stop();
    }

    // Largest block the symbol-rate side can carry: the clock recovery emits
    // one sample per symbol, so 1/10 of the input at the configured 24 kHz for
    // 2400 baud. + 64 for the recovery loop's retained phase.
    // [RATE-SIZED] ASSUMED ratio: baudrate / samplerate. see doc/design/buffer-size-requirements.md 2.6
    static int symbolBlockFor(int maxBlock, double samplerate, double baudrate) {
        if (!(samplerate > 0.0) || maxBlock <= 0) { return maxBlock; }
        double n = std::ceil((double)maxBlock * baudrate / samplerate) + 64.0;
        return (int)(std::max)(1.0, n);
    }

    void init(dsp::stream<dsp::complex_t>* in, double samplerate, double baudrate, int maxBlock = STREAM_BUFFER_SIZE) {
        // Save settings
        _samplerate = samplerate;
        _maxBlock = maxBlock;

        // Configure blocks. demod, fir and recov all run at the input rate.
        demod.init(-4500.0, samplerate);
        demodOut.allocate(_maxBlock);
        float taps[] = { 0.1f, 0.1f, 0.1f, 0.1f, 0.1f, 0.1f, 0.1f, 0.1f, 0.1f, 0.1f };
        fir.init(dsp::taps::fromArray<float>(10, taps), _maxBlock);
        recov.init(samplerate/baudrate, 1e-4, 1.0, 0.05, 128, 8, _maxBlock);

        // [RATE-SIZED] Both output streams carry symbols, not samples, and
        // neither is STREAM_BUFFER_SIZE any more. Sized before anything is
        // started, so no reader can be holding the old buffer. see doc/design/buffer-size-requirements.md 2.6
        _symbolMaxBlock = symbolBlockFor(_maxBlock, samplerate, baudrate);
        soft.setBufferSize(_symbolMaxBlock);
        base_type::out.setBufferSize(_symbolMaxBlock);

        // Init base
        base_type::init(in);
    }

    int process(int count, dsp::complex_t* in, float* softOut, uint8_t* out) {
        count = demod.process(count, in, demodOut);
        count = fir.process(count, demodOut, demodOut);
        count = recov.process(count, demodOut, softOut);
        dsp::digital::BinarySlicer::process(count, softOut, out);
        return count;
    }

    void setBaudrate(double baudrate) {
        assert(base_type::_block_init);
        std::lock_guard<std::recursive_mutex> lck(base_type::ctrlMtx);
        dsp::block::TempStop guard(this);
        // Still a stub. Whoever implements it has to move _symbolMaxBlock
        // with the baud rate -- and resizing `soft` or `out` frees a buffer
        // their readers may hold, which TempStop above does not stop.
    }

    int run() {
        int count = base_type::_in->read();
        if (count < 0) { return -1; }

        count = process(count, base_type::_in->readBuf, soft.writeBuf, base_type::out.writeBuf);

        base_type::_in->flush();
        if (!base_type::out.swap(count)) { return -1; }
        if (count) { if (!soft.swap(count)) { return -1; } }
        return count;
    }

    dsp::stream<float> soft;

private:
    dsp::demod::Quadrature demod;
    dsp::buffer::Buffer<float> demodOut;   // scratch, in place of demod's old out stream
    dsp::filter::FIR<float, float> fir;
    dsp::clock_recovery::MM<float> recov;

    double _samplerate;
    int _maxBlock = STREAM_BUFFER_SIZE;
    int _symbolMaxBlock = STREAM_BUFFER_SIZE;
};