#pragma once
#include "../processor.h"
#include "../window/nuttall.h"
#include "../buffer/fftw.h"
#include "../buffer/history_buffer.h"

namespace dsp::noise_reduction {
    class FMIF : public Processor<complex_t, complex_t> {
        using base_type = Processor<complex_t, complex_t>;
    public:
        FMIF() {}

        FMIF(stream<complex_t>* in, int bins) { init(in, bins); }

        ~FMIF() {
            // The owners below are destroyed before ~block() runs, so the
            // worker must be joined here; they then release everything.
            if (!base_type::_block_init) { return; }
            base_type::stop();
        }

        void init(stream<complex_t>* in, int bins) {
            _bins = bins;
            initBuffers();
            base_type::init(in);
        }

        void setBins(int bins) {
            assert(base_type::_block_init);
            std::lock_guard<std::recursive_mutex> lck(base_type::ctrlMtx);
            dsp::block::TempStop guard(this);
            _bins = bins;
            initBuffers();   // leaves everything empty if it throws
        }

        void reset() {
            assert(base_type::_block_init);
            std::lock_guard<std::recursive_mutex> lck(base_type::ctrlMtx);
            dsp::block::TempStop guard(this);
            if (!configured) { return; }
            hist.clear();
            buffer::clear<complex_t>(backFFTIn, _bins);
        }

        // IN-PLACE: safe (in == out).
        int process(int count, const complex_t* in, complex_t* out) {
            // Pass through if the buffers are gone. setBins() releases them and
            // rethrows when an allocation fails, and ~TempStop restarts the
            // worker on its way out -- so this runs with everything NULL while
            // the OOM exception is still unwinding. Faulting here would bury
            // the diagnostic that exception carries.
            if (!configured) {
                if (out != in) { memcpy(out, in, count * sizeof(complex_t)); }
                return count;
            }

            // Sliding window of the last bins-1 samples plus this block
            const complex_t* window = hist.push(in, count);
            
            // Iterate the FFT
            for (int i = 0; i < count; i++) {
                // Apply windows
                volk_32fc_32f_multiply_32fc((lv_32fc_t*)forwFFTIn.get(), (const lv_32fc_t*)&window[i], fftWin, _bins);

                // Do forward FFT
                fftwf_execute(forwardPlan);

                // Process bins here
                uint32_t idx;
                volk_32fc_magnitude_32f(ampBuf, (lv_32fc_t*)forwFFTOut.get(), _bins);
                volk_32f_index_max_32u(&idx, ampBuf, _bins);

                // Keep only the bin of highest amplitude
                backFFTIn[idx] = forwFFTOut[idx];

                // Do reverse FFT and get first element
                fftwf_execute(backwardPlan);
                out[i] = backFFTOut[_bins / 2];

                // Reset the input buffer
                backFFTIn[idx] = { 0, 0 };
            }

            hist.advance(count);

            return count;
        }

        int run() {
            int count = base_type::_in->read();
            if (count < 0) { return -1; }

            process(count, base_type::_in->readBuf, base_type::out.writeBuf);

            // Swap if some data was generated
            base_type::_in->flush();
            if (!base_type::out.swap(count)) { return -1; }
            return count;
        }

    protected:
        void initBuffers() {
            // Any allocation below can throw, and _bins has already been
            // updated by then. Release everything on the way out rather than
            // leave a mix of new- and old-sized buffers, which the new _bins
            // would index out of bounds; process() passes through while they
            // are empty. release() is a no-op on an owner that is already
            // empty, so this is safe on partially built state too.
            try {
                initBuffersUnsafe();
            }
            catch (...) {
                releaseBuffers();
                throw;
            }
        }

        void initBuffersUnsafe() {
            // The plans are bound to the alignment of the arrays they were
            // built against and cannot survive their reallocation, so they go
            // first -- before the storage under them moves, not after.
            forwardPlan.destroy();
            backwardPlan.destroy();

            // Allocate FFT buffers
            forwFFTIn.allocate(_bins);
            forwFFTOut.allocate(_bins);
            backFFTIn.allocate(_bins);
            backFFTOut.allocate(_bins);

            // Delay line: the last bins-1 samples. The input is a stream block,
            // so STREAM_BUFFER_SIZE bounds it.
            hist.init(STREAM_BUFFER_SIZE, _bins - 1);

            // Clear backward FFT input since only one value is changed and reset at a time
            buffer::clear<complex_t>(backFFTIn, _bins);

            // Allocate amplitude buffer
            ampBuf.allocate(_bins);

            // Allocate and generate Window
            fftWin.allocate(_bins);
            for (int i = 0; i < _bins; i++) { fftWin[i] = window::nuttall(i, _bins - 1); }

            // Plan FFTs
            forwardPlan.planDFT1D(_bins, (fftwf_complex*)forwFFTIn.get(), (fftwf_complex*)forwFFTOut.get(), FFTW_FORWARD);
            backwardPlan.planDFT1D(_bins, (fftwf_complex*)backFFTIn.get(), (fftwf_complex*)backFFTOut.get(), FFTW_BACKWARD);

            // Committed last: process() passes through while not configured,
            // so this must not be set until everything above has succeeded.
            configured = true;
        }

        void releaseBuffers() {
            forwardPlan.destroy();
            backwardPlan.destroy();
            forwFFTIn.release();
            forwFFTOut.release();
            backFFTIn.release();
            backFFTOut.release();
            hist.release();
            ampBuf.release();
            fftWin.release();
            configured = false;
        }

        buffer::FFTWBuffer<complex_t> forwFFTIn;
        buffer::FFTWBuffer<complex_t> forwFFTOut;
        buffer::FFTWBuffer<complex_t> backFFTIn;
        buffer::FFTWBuffer<complex_t> backFFTOut;

        buffer::FFTWPlan forwardPlan;
        buffer::FFTWPlan backwardPlan;

        // [ history: bins-1 | block: <= STREAM_BUFFER_SIZE ]
        buffer::HistoryBuffer<complex_t> hist;
        bool configured = false;

        buffer::Buffer<float> fftWin;

        buffer::Buffer<float> ampBuf;

        int _bins;

    }; 
}