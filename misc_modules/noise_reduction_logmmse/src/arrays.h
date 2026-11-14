#pragma once
#include <memory>
#include <vector>
#include <math.h>
#include <dsp/buffer/fftw.h>
#include <dsp/block.h>

namespace dsp {

    namespace math {

        // Exponential integral E1(x), table-driven (see arrays.cpp).
        float expn(float q);
        // Fill zero-valued gaps in arr by linear interpolation between the
        // surrounding non-zero samples; returns false if arr is all zeros.
        bool linearInterpolateHoles(float *arr, int narr);
    }

    /**
     * NumPy-lookalike vector ops for the LogMMSE port (logmmse.h). The
     * vocabulary deliberately mirrors the Python reference implementation so
     * the port stays line-for-line auditable against it — keep additions
     * limited to what that algorithm needs, and write new in-place/streaming
     * code with raw VOLK instead (see add_noise_history in logmmse.h).
     *
     * Arrays are std::vector values: functions take const references and
     * return fresh vectors; clamp-style ops (npminimum, npmaximum) take
     * their argument by value and reuse its storage, so pass a temporary or
     * std::move an lvalue you no longer need.
     */
    namespace arrays {

        // Storage backend for FloatArray/ComplexArray:
        //   0 (default) - each array is a plain heap allocation
        //   1           - allocations are recycled through a size-class pool
        //                 (see arrays.cpp); trades a few MB of retained RSS
        //                 for less allocator churn on slow allocators
        //                 (Android scudo). Pooled memory is invisible to
        //                 ASan/leak tools; keep 0 for debugging.
        // Must be set identically for every TU of this module (via
        // target_compile_definitions in CMakeLists.txt, not per-file).
#ifndef SDRPP_NR_RECYCLE_ARRAYS
#define SDRPP_NR_RECYCLE_ARRAYS 0
#endif

#if SDRPP_NR_RECYCLE_ARRAYS
        namespace detail {
            void* poolAlloc(size_t bytes);
            void poolFree(void* p, size_t bytes) noexcept;
        }
#endif

        // Allocator for the array types. Regardless of the storage backend
        // it default-initializes instead of value-initializing, so
        // FloatArray(n) / resize(n) skip the zero-fill (a no-op for float /
        // complex_t) and return UNSPECIFIED contents. Every arrays.cpp op
        // overwrites its result in full before returning; npzeros /
        // npzeros_c zero explicitly. Copy/fill construction is unaffected
        // (construct(p, args...) falls back to plain placement new).
        template <class T>
        struct dsp_allocator {
            using value_type = T;
            template <class U> struct rebind { using other = dsp_allocator<U>; };
            dsp_allocator() = default;
            template <class U> dsp_allocator(const dsp_allocator<U>&) noexcept {}

            T* allocate(size_t n) {
#if SDRPP_NR_RECYCLE_ARRAYS
                return (T*)detail::poolAlloc(n * sizeof(T));
#else
                return (T*)::operator new(n * sizeof(T));
#endif
            }

            void deallocate(T* p, size_t n) noexcept {
#if SDRPP_NR_RECYCLE_ARRAYS
                detail::poolFree(p, n * sizeof(T));
#else
                (void)n;
                ::operator delete(p);
#endif
            }

            template <class U> void construct(U* p) { ::new ((void*)p) U; }

            template <class U> bool operator==(const dsp_allocator<U>&) const noexcept { return true; }
            template <class U> bool operator!=(const dsp_allocator<U>&) const noexcept { return false; }
        };

        typedef std::vector<float, dsp_allocator<float>> FloatArray;
        typedef std::vector<dsp::complex_t, dsp_allocator<dsp::complex_t>> ComplexArray;

        FloatArray npzeros(int size);
        ComplexArray npzeros_c(int size);

        // hanning window
        FloatArray nphanning(int len);

        // add all items together
        float npsum(const FloatArray& v);
        // true if no element is zero (numpy.all on the != 0 mask)
        bool npall(const FloatArray& v);

        // multiply by scalar
        FloatArray mul(const FloatArray& v, float e);
        // divide by scalar
        FloatArray div(const FloatArray& v, float e);
        // add scalar to all items
        FloatArray add(const FloatArray& v, float e);

        // elementwise binary ops
        FloatArray addeach(const FloatArray& v, const FloatArray& w);
        FloatArray subeach(const FloatArray& v, const FloatArray& w);
        FloatArray muleach(const FloatArray& v, const FloatArray& w);
        FloatArray diveach(const FloatArray& v, const FloatArray& w);
        ComplexArray addeach(const ComplexArray& v, const ComplexArray& w);
        ComplexArray muleach(const FloatArray& v, const ComplexArray& w);

        // clamp to at most / at least lim; reuses v's storage
        FloatArray npminimum(FloatArray v, float lim);
        FloatArray npmaximum(FloatArray v, float lim);

        // copy of v[begin..end)
        ComplexArray nparange(const ComplexArray& v, int begin, int end);
        // copy part into v starting at begin
        void nparangeset(ComplexArray& v, int begin, const ComplexArray& part);

        // elementwise exp (VOLK expfast: ~1e-4 relative error)
        FloatArray npexp(const FloatArray& v);
        // elementwise magnitude of complex array
        FloatArray npabsolute(const ComplexArray& in);
        // elementwise E1 (scipy.special.expn(1, x))
        FloatArray scipyspecialexpn(const FloatArray& in);

        // centered moving average, only even window sizes
        FloatArray npmavg(const FloatArray& v, int windowSize);

        class ZeroPaddedFFTW {
        public:
            ZeroPaddedFFTW(bool backward, int buckets) : nbuckets(buckets), reverse(backward) {
                m_input.assign(buckets, dsp::complex_t{ 0, 0 });
                m_output.assign(buckets, dsp::complex_t{ 0, 0 });
                m_plan.planDFT1D(buckets, (fftwf_complex*)m_input.data(), (fftwf_complex*)m_output.data(), backward ? FFTW_BACKWARD : FFTW_FORWARD);
            }
            const ComplexArray& getOutput() { return m_output; }

            const ComplexArray& npfftfft(const ComplexArray& in) {
                // Zero-pad / truncate in into the plan-bound m_input buffer.
                auto limit = std::min(in.size(), (size_t)nbuckets);
                std::copy(in.begin(), in.begin() + limit, m_input.begin());
                std::fill(m_input.begin() + limit, m_input.end(), dsp::complex_t{ 0, 0 });
                fftwf_execute(m_plan);
                if (reverse)
                    volk_32fc_s32fc_multiply_32fc((lv_32fc_t*)m_output.data(), (lv_32fc_t*)m_output.data(), lv_32fc_t(1.0f / nbuckets, 0.0f), m_output.size());
                return m_output;
            }

            private:
                // Fixed-size working buffers the FFTW plan is bound to; they must never reallocate.
                ComplexArray m_input;
                ComplexArray m_output;
                dsp::buffer::FFTWPlan m_plan;

                int nbuckets;
                bool reverse;
        };
   } // namespace arrays
} // namespace dsp
