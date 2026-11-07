#pragma once
#include <mutex>
#include <stdexcept>
#include <string>
#include <fftw3.h>
#include "../../utils/flog.h"

// RAII owners for the two FFTW resources this tree allocates: plan-bound
// working buffers and the plans themselves. Deliberately separate from
// dsp::buffer::Buffer, for two reasons that are easy to lose sight of:
//
//  - The storage must come from fftwf_malloc(), not volk_malloc(). A plan
//    records the *alignment* of the arrays it was built against and picks its
//    SIMD codelets accordingly, so the buffers and the plan are a matched pair.
//    Reallocating a buffer therefore invalidates the plan: re-plan, never just
//    re-point. FFTWPlan::planDFT1D() exists so that reads as one step.
//
//  - Everything in FFTW except fftwf_execute() touches shared planner state and
//    must be serialised. This tree plans from at least three threads -- the UI
//    (FMIF::setBins, SubWaterfall), a source thread (IQFrontEnd::updateFFTPath
//    via setSampleRate) and a DSP worker (the logmmse plan cache) -- so the
//    lock lives here, held across every planner call, and no caller has to
//    remember it.
//
// This header includes <fftw3.h>, so it is included only by translation units
// that already use FFTW. It is not pulled in by buffer.h.

namespace dsp::buffer {
    // Guards every FFTW entry point except fftwf_execute(). Function-local
    // static so the ordering is defined however the owners are constructed.
    inline std::mutex& fftwPlannerMutex() {
        static std::mutex mtx;
        return mtx;
    }

    [[noreturn]] inline void throwFFTWAllocFailure(size_t bytes) {
        flog::error("fftwf_malloc failed to allocate {0} bytes", (uint64_t)bytes);
        throw std::runtime_error("dsp::buffer::FFTWBuffer: out of memory allocating " + std::to_string(bytes) + " bytes");
    }

    // RAII owner for an fftwf_malloc'd array. Same shape as dsp::buffer::Buffer:
    // implicitly converts to T* so it reads like the raw pointer it replaces,
    // with get() for the places a C-style cast is applied (a cast cannot see
    // through a user conversion).
    template<class T>
    class FFTWBuffer {
    public:
        FFTWBuffer() {}
        explicit FFTWBuffer(int count) { allocate(count); }

        ~FFTWBuffer() { release(); }

        FFTWBuffer(const FFTWBuffer&) = delete;
        FFTWBuffer& operator=(const FFTWBuffer&) = delete;

        FFTWBuffer(FFTWBuffer&& other) noexcept : _ptr(other._ptr), _size(other._size) {
            other._ptr = NULL;
            other._size = 0;
        }

        FFTWBuffer& operator=(FFTWBuffer&& other) noexcept {
            if (this != &other) {
                release();
                _ptr = other._ptr;
                _size = other._size;
                other._ptr = NULL;
                other._size = 0;
            }
            return *this;
        }

        // Strong guarantee: acquire first, commit second, so a failure leaves
        // the owner on its existing storage rather than empty.
        //
        // Any plan built against the old storage is dead once this returns --
        // re-plan before the next execute.
        void allocate(int count) {
            if (count <= 0) {
                // Not an out-of-memory condition, and worth its own message:
                // reporting "failed to allocate 0 bytes" sends the reader
                // hunting for a memory problem that isn't there.
                throw std::runtime_error("dsp::buffer::FFTWBuffer: invalid element count " + std::to_string(count));
            }
            const size_t bytes = (size_t)count * sizeof(T);
            T* fresh = (T*)fftwf_malloc(bytes);
            if (!fresh) { throwFFTWAllocFailure(bytes); }
            release();
            _ptr = fresh;
            _size = count;
        }

        void release() {
            if (!_ptr) { return; }
            fftwf_free(_ptr);
            _ptr = NULL;
            _size = 0;
        }

        T* get() const { return _ptr; }
        int size() const { return _size; }

        operator T*() const { return _ptr; }

    private:
        T* _ptr = NULL;
        int _size = 0;
    };

    // RAII owner for an fftwf_plan. Converts implicitly to fftwf_plan so
    // fftwf_execute(plan) keeps working unchanged.
    //
    // There is deliberately no way to adopt a plan built elsewhere: creation has
    // to happen under the planner lock, and an adopt() would quietly hand that
    // responsibility back to callers. Add a planXxx() member for any other
    // transform this tree starts using.
    class FFTWPlan {
    public:
        FFTWPlan() {}

        ~FFTWPlan() { destroy(); }

        FFTWPlan(const FFTWPlan&) = delete;
        FFTWPlan& operator=(const FFTWPlan&) = delete;

        FFTWPlan(FFTWPlan&& other) noexcept : _plan(other._plan) { other._plan = NULL; }

        FFTWPlan& operator=(FFTWPlan&& other) noexcept {
            if (this != &other) {
                destroy();
                _plan = other._plan;
                other._plan = NULL;
            }
            return *this;
        }

        // Replaces whatever plan is held. Builds the new one before releasing
        // the old, so a planner failure leaves the existing plan usable --
        // which is safe with FFTW_ESTIMATE, the only flag used in this tree,
        // because it does not write to the arrays while planning. Passing
        // FFTW_MEASURE here would clobber `in` and `out`.
        void planDFT1D(int n, fftwf_complex* in, fftwf_complex* out, int sign, unsigned int flags = FFTW_ESTIMATE) {
            std::lock_guard<std::mutex> lck(fftwPlannerMutex());
            fftwf_plan fresh = fftwf_plan_dft_1d(n, in, out, sign, flags);
            if (!fresh) {
                throw std::runtime_error("dsp::buffer::FFTWPlan: fftwf_plan_dft_1d failed for size " + std::to_string(n));
            }
            if (_plan) { fftwf_destroy_plan(_plan); }
            _plan = fresh;
        }

        void destroy() {
            if (!_plan) { return; }
            std::lock_guard<std::mutex> lck(fftwPlannerMutex());
            fftwf_destroy_plan(_plan);
            _plan = NULL;
        }

        fftwf_plan get() const { return _plan; }

        operator fftwf_plan() const { return _plan; }

    private:
        fftwf_plan _plan = NULL;
    };
}
