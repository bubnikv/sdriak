#pragma once
#include <assert.h>
#include <volk/volk.h>
#include "../types.h"
#include "../math/hz_to_rads.h"
#include <atomic>
#include <cstdint>
#include <cstring>

namespace dsp::channel {
    // Computation kernel, not a dsp::block. See dsp::math::Delay for why.
    // The lock-free phaseDelta publish is kept: setOffset() is still called from
    // control threads while the *containing* block's worker runs process().
    class FrequencyXlator {
    public:
        FrequencyXlator() {}

        FrequencyXlator(double offset) { init(offset); }

        FrequencyXlator(double offset, double samplerate) { init(offset, samplerate); }

        void init(double offset) {
            phase = lv_cmake(1.0f, 0.0f);
            phaseDeltaBits.store(packDelta(lv_cmake((float)cos(offset), (float)sin(offset))), std::memory_order_relaxed);
            _init = true;
        }

        void init(double offset, double samplerate) {
            init(math::hzToRads(offset, samplerate));
        }

        void setOffset(double offset) {
            assert(_init);
            // Lock-free publish to the worker of the containing block: the store
            // is self-synchronizing and setOffset touches no other shared state.
            phaseDeltaBits.store(packDelta(lv_cmake((float)cos(offset), (float)sin(offset))), std::memory_order_relaxed);
        }

        void setOffset(double offset, double samplerate) {
            setOffset(math::hzToRads(offset, samplerate));
        }

        // The caller must have stopped the containing block's worker first.
        void reset() {
            assert(_init);
            phase = lv_cmake(1.0f, 0.0f);
        }

        // IN-PLACE: safe in practice (in == out). The VOLK rotator is
        // element-wise (it loads and stores the same index), and GNU Radio
        // relies on that, but VOLK does not document it as a guarantee.
        inline int process(int count, const complex_t* in, complex_t* out) {
            // Snapshot the atomically-published phaseDelta once per buffer. The
            // rotator only reads the increment (it advances 'phase'), so a local
            // copy is safe and the read can't tear against a concurrent setOffset.
            lv_32fc_t pd = unpackDelta(phaseDeltaBits.load(std::memory_order_relaxed));
#if VOLK_VERSION >= 030100
            volk_32fc_s32fc_x2_rotator2_32fc((lv_32fc_t*)out, (lv_32fc_t*)in, &pd, &phase, count);
#else
            volk_32fc_s32fc_x2_rotator_32fc((lv_32fc_t*)out, (lv_32fc_t*)in, pd, &phase, count);
#endif
            return count;
        }


    protected:
        bool _init = false;

        // Pack/unpack the 8-byte lv_32fc_t phase increment into a uint64_t so it
        // can be published to the lock-free worker via a single atomic store/load.
        static_assert(sizeof(lv_32fc_t) == sizeof(uint64_t), "lv_32fc_t must be two contiguous 32-bit floats to pack into a uint64_t");
        static inline uint64_t packDelta(lv_32fc_t c) {
            uint64_t v;
            std::memcpy(&v, &c, sizeof(v));
            return v;
        }
        static inline lv_32fc_t unpackDelta(uint64_t v) {
            lv_32fc_t c;
            std::memcpy(&c, &v, sizeof(c));
            return c;
        }

        // Touched only by the worker (and by reset(), which stops-and-joins it first).
        lv_32fc_t phase;
        // Published by setOffset() (control threads), consumed by process() (worker).
        // Must be lock-free or it silently becomes a mutex on the DSP hot path.
        std::atomic<uint64_t> phaseDeltaBits{ 0 };
        static_assert(std::atomic<uint64_t>::is_always_lock_free,
                      "FrequencyXlator publishes phaseDelta via a lock-free 64-bit atomic; "
                      "this target lacks one, which would add a lock on the DSP hot path");
    };
}