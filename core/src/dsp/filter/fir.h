#pragma once
#include <assert.h>
#include <string.h>
#include <volk/volk.h>
#include "../types.h"
#include "../buffer/buffer.h"
#include "../taps/tap.h"
#include "../processor.h"

namespace dsp::filter {
    // Computation kernel, not a dsp::block. Twelve of the fourteen FIR instances
    // in this tree are fused inside a composite that calls process() inline, so
    // the output stream, worker thread and mutexes were dead weight in all of
    // them -- 8-16 MB each. The two that really are chain nodes use FIRBlock
    // below. The containing block owns the thread and must stop it before
    // calling setTaps()/reset().
    template <class D, class T>
    class FIR {
    public:
        FIR() {}

        FIR(tap<T>& taps, int maxBlock = STREAM_BUFFER_SIZE) { init(taps, maxBlock); }


        virtual void init(tap<T>& taps, int maxBlock = STREAM_BUFFER_SIZE) {
            _taps = taps;
            _maxBlock = maxBlock;

            // [RATE-SIZED] Callers may pass a maxBlock derived from a rate
            // ratio rather than STREAM_BUFFER_SIZE; process() then relies on
            // count <= maxBlock. Overruns here are silent heap corruption.
            // see doc/design/buffer-size-requirements.md 2.6
            buffer.allocate(_maxBlock + 64000);
            bufStart = &buffer[_taps.size - 1];
            buffer::clear<D>(buffer, _taps.size - 1);

            _init = true;
        }

        // maxBlock defaults to -1, meaning "keep the current delay line".
        // Pass a value when the caller's block bound has changed -- a filter
        // that runs after a resampler sees a fraction of the block its owner
        // was sized for, and the delay line is the expensive part. Resizing
        // discards the history, so only do it while reconfiguring a rate.
        virtual void setTaps(tap<T>& taps, int maxBlock = -1) {
            assert(_init);

            if (maxBlock > 0 && maxBlock != _maxBlock) {
                // [RATE-SIZED] Same contract as init(): the new bound comes
                // from the caller's rate arithmetic. see doc/design/buffer-size-requirements.md 2.6
                //
                // Nothing is committed before the allocation. Publishing _taps
                // first and then throwing would restart the worker with the new
                // tap count, the old capacity and a bufStart computed from the
                // old count -- a read past the end of the retained buffer on any
                // tap-count increase.
                buffer.allocate(maxBlock + 64000);
                _maxBlock = maxBlock;
                _taps = taps;
                bufStart = &buffer[_taps.size - 1];
                buffer::clear<D>(buffer, _taps.size - 1);
                return;
            }

            // From here on nothing can throw, so the order does not matter.
            int oldTC = _taps.size;
            _taps = taps;

            // Update start of buffer
            bufStart = &buffer[_taps.size - 1];

            // Move existing data to make transition seemless
            if (_taps.size < oldTC) {
                memmove(buffer, &buffer[oldTC - _taps.size], (_taps.size - 1) * sizeof(D));
            }
            else if (_taps.size > oldTC) {
                memmove(&buffer[_taps.size - oldTC], buffer, (oldTC - 1) * sizeof(D));
                buffer::clear<D>(buffer, _taps.size - oldTC);
            }
        }

        virtual void reset() {
            assert(_init);
            buffer::clear<D>(buffer, _taps.size - 1);
        }

        inline int process(int count, const D* in, D* out) {
            // Copy data to work buffer
            memcpy(bufStart, in, count * sizeof(D));

            // Do convolution
            for (int i = 0; i < count; i++) {
                if constexpr (std::is_same_v<D, float> && std::is_same_v<T, float>) {
                    volk_32f_x2_dot_prod_32f(&out[i], &buffer[i], _taps.taps, _taps.size);
                }
                if constexpr ((std::is_same_v<D, complex_t> || std::is_same_v<D, stereo_t>) && std::is_same_v<T, float>) {
                    volk_32fc_32f_dot_prod_32fc((lv_32fc_t*)&out[i], (lv_32fc_t*)&buffer[i], _taps.taps, _taps.size);
                }
                if constexpr ((std::is_same_v<D, complex_t> || std::is_same_v<D, stereo_t>) && std::is_same_v<T, complex_t>) {
                    volk_32fc_x2_dot_prod_32fc((lv_32fc_t*)&out[i], (lv_32fc_t*)&buffer[i], (lv_32fc_t*)_taps.taps, _taps.size);
                }
            }

            // Move unused data
            memmove(buffer, &buffer[count], (_taps.size - 1) * sizeof(D));

            return count;
        }

    protected:
        bool _init = false;
        int _maxBlock = 0;
        // Not owned: init() is handed a tap set the caller generated and keeps.
        tap<T> _taps;
        buffer::Buffer<D> buffer;
        D* bufStart = NULL;
    };

    // Threaded adapter around the kernel, for the FIRs that really are chain
    // nodes with their own worker and output stream.
    template <class D, class T>
    class FIRBlock : public Processor<D, D> {
        using base_type = Processor<D, D>;
    public:
        FIRBlock() {}

        FIRBlock(stream<D>* in, tap<T>& taps) { init(in, taps); }

        ~FIRBlock() {
            // The owning members below are destroyed before ~block() runs, so
            // the worker must be joined here -- otherwise it keeps reading
            // buffers that have already been freed.
            if (!base_type::_block_init) { return; }
            base_type::stop();
        }

        void init(stream<D>* in, tap<T>& taps) {
            kernel.init(taps);
            base_type::init(in);
        }

        void setTaps(tap<T>& taps, int maxBlock = -1) {
            assert(base_type::_block_init);
            std::lock_guard<std::recursive_mutex> lck(base_type::ctrlMtx);
            dsp::block::TempStop guard(this);
            kernel.setTaps(taps, maxBlock);
        }

        void reset() {
            assert(base_type::_block_init);
            std::lock_guard<std::recursive_mutex> lck(base_type::ctrlMtx);
            dsp::block::TempStop guard(this);
            kernel.reset();
        }

        inline int process(int count, const D* in, D* out) {
            return kernel.process(count, in, out);
        }

        int run() {
            int count = base_type::_in->read();
            if (count < 0) { return -1; }

            kernel.process(count, base_type::_in->readBuf, base_type::out.writeBuf);

            base_type::_in->flush();
            if (!base_type::out.swap(count)) { return -1; }
            return count;
        }

        FIR<D, T> kernel;
    };
}
