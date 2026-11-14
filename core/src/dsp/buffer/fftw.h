#pragma once
#include <mutex>
#include <stdexcept>
#include <string>
#include <fftw3.h>
#include "../../utils/flog.h"

// RAII owners for FFTW plans and buffers.

// Same rule as SDRPP_CPP_EXPORT in module.h, repeated here so this DSP header
// does not have to pull in module.h (and with it Windows.h and json.hpp).
#ifdef _WIN32
#ifdef SDRPP_IS_CORE
#define SDRPP_FFTW_EXPORT __declspec(dllexport)
#else
#define SDRPP_FFTW_EXPORT __declspec(dllimport)
#endif
#else
#define SDRPP_FFTW_EXPORT
#endif

namespace dsp::buffer {
    // Guards every FFTW entry point except fftwf_execute(); the FFTW planner
    // is not thread-safe. Defined once, in core (fftw_mutex.cpp), and exported:
    // as an inline function each module DLL would get its own copy of the
    // static, and planning in dab_decoder or logmmse would not be serialized
    // against planning in core.
    SDRPP_FFTW_EXPORT std::mutex& fftwMutex();

    [[noreturn]] inline void throwFFTWAllocFailure(size_t bytes) {
        flog::error("fftwf_malloc failed to allocate {0} bytes", (uint64_t)bytes);
        throw std::runtime_error("dsp::buffer::FFTWBuffer: out of memory allocating " + std::to_string(bytes) + " bytes");
    }

    // RAII owner for an fftwf_malloc'd array, for SIMD buffer alignment.
    template<class T>
    class FFTWBuffer {
    public:
        FFTWBuffer() = default;
        explicit FFTWBuffer(int count) { allocate(count); }

        ~FFTWBuffer() { release(); }

        FFTWBuffer(const FFTWBuffer&) = delete;
        FFTWBuffer& operator=(const FFTWBuffer&) = delete;

        FFTWBuffer(FFTWBuffer&& other) noexcept : m_ptr(other.m_ptr), m_size(other.m_size) {
            other.m_ptr = nullptr;
            other.m_size = 0;
        }

        FFTWBuffer& operator=(FFTWBuffer&& other) noexcept {
            if (this != &other) {
                release();
                m_ptr = other.m_ptr;
                m_size = other.m_size;
                other.m_ptr = nullptr;
                other.m_size = 0;
            }
            return *this;
        }

        // Strong guarantee: acquire first, commit second, so a failure leaves
        // the owner on its existing storage rather than empty.
        //
        // Any plan built against the old storage is dead once this returns --
        // re-plan before the next execute.
        void allocate(int count) {
            if (count <= 0)
                throw std::runtime_error("dsp::buffer::FFTWBuffer: invalid element count " + std::to_string(count));
            const size_t bytes = (size_t)count * sizeof(T);
            T* fresh = (T*)fftwf_malloc(bytes);
            if (!fresh)
                throwFFTWAllocFailure(bytes);
            release();
            m_ptr = fresh;
            m_size = count;
        }

        void release() {
            if (!m_ptr)
                return;
            fftwf_free(m_ptr);
            m_ptr = nullptr;
            m_size = 0;
        }

        T* get() const { return m_ptr; }
        int size() const { return m_size; }
        operator T*() const { return m_ptr; }

    private:
        T*  m_ptr = nullptr;
        int m_size = 0;
    };

    // RAII owner for an fftwf_plan. Converts implicitly to fftwf_plan so
    // fftwf_execute(plan) keeps working unchanged.
    class FFTWPlan {
    public:
        FFTWPlan() = default;
        ~FFTWPlan() { destroy(); }

        FFTWPlan(const FFTWPlan&) = delete;
        FFTWPlan& operator=(const FFTWPlan&) = delete;

        FFTWPlan(FFTWPlan&& other) noexcept : m_plan(other.m_plan) { other.m_plan = nullptr; }

        FFTWPlan& operator=(FFTWPlan&& other) noexcept {
            if (this != &other) {
                destroy();
                m_plan = other.m_plan;
                other.m_plan = nullptr;
            }
            return *this;
        }

        // Replaces whatever plan is held. Builds the new one before releasing
        // the old, so a planner failure leaves the existing plan usable --
        // which is safe with FFTW_ESTIMATE, the only flag used in this tree,
        // because it does not write to the arrays while planning. Passing
        // FFTW_MEASURE here would clobber `in` and `out`.
        void planDFT1D(int n, fftwf_complex* in, fftwf_complex* out, int sign, unsigned int flags = FFTW_ESTIMATE) {
            std::lock_guard<std::mutex> lck(fftwMutex());
            fftwf_plan fresh = fftwf_plan_dft_1d(n, in, out, sign, flags);
            if (!fresh)
                throw std::runtime_error("dsp::buffer::FFTWPlan: fftwf_plan_dft_1d failed for size " + std::to_string(n));
            if (m_plan)
                fftwf_destroy_plan(m_plan);
            m_plan = fresh;
        }

        void destroy() {
            if (!m_plan)
                return;
            std::lock_guard<std::mutex> lck(fftwMutex());
            fftwf_destroy_plan(m_plan);
            m_plan = nullptr;
        }

        fftwf_plan get() const { return m_plan; }

        operator fftwf_plan() const { return m_plan; }

    private:
        fftwf_plan m_plan = nullptr;
    };
}
