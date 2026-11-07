#include "arrays.h"
#include <dsp/buffer/fftw.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <utils/flog.h>

#if SDRPP_NR_RECYCLE_ARRAYS
#include <mutex>
#include <new>
#include <unordered_map>
#endif

namespace dsp {

    namespace math {

        // Exponential integral E1(x) for x > 0, used to build the lookup table
        // below. Rational approximations from Abramowitz & Stegun 5.1.53 / 5.1.56,
        // absolute error < 2e-7, well below the float precision of the table.
        // Not std::expint: libc++ (Apple clang) does not ship the C++17 special
        // math functions.
        static double expintE1(double x) {
            if (x <= 1.0) {
                double p = 0.00107857;
                p = p * x - 0.00976004;
                p = p * x + 0.05519968;
                p = p * x - 0.24991055;
                p = p * x + 0.99999193;
                p = p * x - 0.57721566;
                return p - std::log(x);
            }
            double num = (((x + 8.5733287401) * x + 18.0590169730) * x + 8.6347608925) * x + 0.2677737343;
            double den = (((x + 9.5733223454) * x + 25.6329561486) * x + 21.0996530827) * x + 3.9584969228;
            return num / den * std::exp(-x) / x;
        }

        // E1 lookup table on an x^(1/4)-spaced grid: entry i holds E1((i / EXPN_GRID)^4),
        // covering x in [0, 81]. Built once at load time.
        static constexpr int EXPN_GRID = 1000;
        static constexpr int EXPN_SIZE = 3001;
        static const std::array<float, EXPN_SIZE> expnTable = [] {
            std::array<float, EXPN_SIZE> t;
            // E1 diverges at x = 0; clamp the first entry to E1(1e-16).
            t[0] = (float)expintE1(1e-16);
            for (int i = 1; i < EXPN_SIZE; i++) {
                double r = (double)i / EXPN_GRID;
                t[i] = (float)expintE1(r * r * r * r);
            }
            return t;
        }();

        float expn(float q) {
            // Clamp instead of extrapolating: E1 saturates near 0 and underflows
            // to 0 past x = 81, so the edge entries are the correct limits.
            if (!(q > 0.0f)) return expnTable[0]; // also catches NaN
            double index = std::sqrt(std::sqrt((double)q)) * EXPN_GRID;
            if (index >= EXPN_SIZE - 1) return expnTable[EXPN_SIZE - 1];
            int i = (int)index;
            float f = (float)(index - i);
            return expnTable[i] + (expnTable[i + 1] - expnTable[i]) * f;
        }

        bool linearInterpolateHoles(float *arr, int narr) {
            int firstV = -1;
            int lastV = -1;
            for(int q=0; q < narr; q++) {
                float val = arr[q];
                if(firstV < 0 && val != 0) {
                    firstV = q;
                }
                if (val != 0) {
                    if (lastV != -1) {
                        if (q - lastV > 1) {
                            // fill the gap
                            auto d = (val - arr[lastV]) / (q - lastV);
                            auto running = arr[lastV];
                            for(int w=lastV+1; w<q; w++) {
                                running += d;
                                arr[w] = running;
                            }
                        }
                    }
                    lastV = q;
                }
            }
            if (firstV < 0 || lastV < 0) {
                return false;
            } else {
                auto v = arr[firstV];
                for (int q = firstV - 1; q >= 0; q--) {
                    arr[q] = v;
                }
                v = arr[lastV];
                for (int q = lastV + 1; q < narr; q++) {
                    arr[q] = v;
                }
                return true;
            }
        }
    }

    namespace arrays {

#if SDRPP_NR_RECYCLE_ARRAYS
        namespace detail {

            // Small allocations (vector growth ramp-up, MSVC debug container
            // proxies) are not worth recycling and would pollute the buckets.
            static constexpr size_t POOL_MIN_BYTES = 4096;
            // Peak concurrently-live temporaries per logmmse frame is ~10-12
            // per size class; a slightly larger cap keeps the hit rate at
            // ~100% while bounding retention to a few hundred KB per class.
            static constexpr size_t MAX_PER_BUCKET = 16;

            // Round requests to the next power of two so variable-size
            // allocations (logmmse_all output, worker1c growth) collapse into
            // a bounded set of buckets, at the cost of at most 2x internal
            // fragmentation. alloc and free round identically, so a block
            // always returns to the bucket it came from.
            static size_t sizeClass(size_t bytes) {
                size_t c = POOL_MIN_BYTES;
                while (c < bytes) c *= 2;
                return c;
            }

            class BytePool {
                std::mutex mtx;
                std::unordered_map<size_t, std::vector<void*>> buckets;

            public:
                void* alloc(size_t bytes) {
                    {
                        std::lock_guard<std::mutex> lck(mtx);
                        auto it = buckets.find(bytes);
                        if (it != buckets.end() && !it->second.empty()) {
                            void* p = it->second.back();
                            it->second.pop_back();
                            return p;
                        }
                    }
                    // volk alignment lets volk dispatch its aligned kernels.
                    void* p = volk_malloc(bytes, volk_get_alignment());
                    if (!p) { throw std::bad_alloc(); }
                    return p;
                }

                void free(void* p, size_t bytes) noexcept {
                    {
                        std::lock_guard<std::mutex> lck(mtx);
                        auto& bucket = buckets[bytes];
                        if (bucket.size() < MAX_PER_BUCKET) {
                            bucket.push_back(p);
                            return;
                        }
                    }
                    volk_free(p);
                }
            };

            // Leaked: arrays with static storage duration may deallocate
            // during static destruction, so the pool must never be destroyed.
            static BytePool& pool() {
                static BytePool* p = new BytePool();
                return *p;
            }

            void* poolAlloc(size_t bytes) {
                if (bytes < POOL_MIN_BYTES) { return ::operator new(bytes); }
                return pool().alloc(sizeClass(bytes));
            }

            void poolFree(void* p, size_t bytes) noexcept {
                if (bytes < POOL_MIN_BYTES) {
                    ::operator delete(p);
                    return;
                }
                pool().free(p, sizeClass(bytes));
            }
        }
#endif

        FloatArray npzeros(int size) {
            FloatArray r(size);
            memset(r.data(), 0, size * sizeof(float));
            return r;
        }

        ComplexArray npzeros_c(int size) {
            ComplexArray r(size);
            memset(r.data(), 0, size * sizeof(dsp::complex_t));
            return r;
        }

        // hanning window
        FloatArray nphanning(int len) {
            FloatArray retval(len);
            for (int i = 0; i < len; i++) {
                retval[i] = (0.5 - 0.5 * cos(2.0 * M_PI * i / (len - 1)));
            }
            return retval;
        }

        // add all items together
        float npsum(const FloatArray& v) {
            float f = 0;
            for (auto d : v) {
                f += d;
            }
            return f;
        }

        bool npall(const FloatArray& v) {
            for (auto d : v) {
                if (d == 0) {
                    return false;
                }
            }
            return true;
        }

        // multiply by scalar
        FloatArray mul(const FloatArray& v, float e) {
            FloatArray retval(v.size());
            volk_32f_s32f_multiply_32f(retval.data(), v.data(), e, v.size());
            return retval;
        }

        FloatArray div(const FloatArray& v, float e) {
            FloatArray retval(v.size());
            volk_32f_s32f_multiply_32f(retval.data(), v.data(), 1.0 / e, v.size());
            return retval;
        }

        // add scalar to all items
        FloatArray add(const FloatArray& v, float e) {
            FloatArray retval(v.size());
            int limit = (int)v.size();
            auto* src = v.data();
            auto* dst = retval.data();
            for (int i=0; i<limit; i++) {
                dst[i] = src[i] + e;
            }
            return retval;
        }

        // add two arrays
        FloatArray addeach(const FloatArray& v, const FloatArray& w) {
            FloatArray retval(v.size());
            volk_32f_x2_add_32f(retval.data(), v.data(), w.data(), v.size());
            return retval;
        }

        // subtract two arrays
        FloatArray subeach(const FloatArray& v, const FloatArray& w) {
            FloatArray retval(v.size());
            volk_32f_x2_subtract_32f(retval.data(), v.data(), w.data(), v.size());
            return retval;
        }

        // add two arrays
        ComplexArray addeach(const ComplexArray& v, const ComplexArray& w) {
            ComplexArray retval(v.size());
            volk_32fc_x2_add_32fc((lv_32fc_t*)retval.data(), (const lv_32fc_t*)v.data(), (const lv_32fc_t*)w.data(), v.size());
            return retval;
        }

        // multiply two arrays
        FloatArray muleach(const FloatArray& v, const FloatArray& w) {
            FloatArray retval(v.size());
            volk_32f_x2_multiply_32f(retval.data(), v.data(), w.data(), v.size());
            return retval;
        }

        // multiply complex array by real array
        ComplexArray muleach(const FloatArray& v, const ComplexArray& w) {
            ComplexArray retval(v.size());
            volk_32fc_32f_multiply_32fc((lv_32fc_t*)retval.data(), (const lv_32fc_t*)w.data(), v.data(), v.size());
            return retval;
        }

        FloatArray diveach(const FloatArray& v, const FloatArray& w) {
            FloatArray retval(v.size());
            volk_32f_x2_divide_32f(retval.data(), v.data(), w.data(), v.size());
            return retval;
        }

        FloatArray npminimum(FloatArray v, float lim) {
            for (auto& d : v) {
                if (d > lim) {
                    d = lim;
                }
            }
            return v;
        }

        FloatArray npmaximum(FloatArray v, float lim) {
            for (auto& d : v) {
                if (d < lim) {
                    d = lim;
                }
            }
            return v;
        }

        // array range
        ComplexArray nparange(const ComplexArray& v, int begin, int end) {
            return ComplexArray(v.begin() + begin, v.begin() + end);
        }

        // update array in-place
        void nparangeset(ComplexArray& v, int begin, const ComplexArray& part) {
            std::copy(part.begin(), part.end(), v.begin() + begin);
        }

        FloatArray npexp(const FloatArray& v) {
            FloatArray retval(v.size());
            volk_32f_expfast_32f(retval.data(), v.data(), v.size());
            return retval;
        }

        FloatArray npabsolute(const ComplexArray& in) {
            FloatArray retval(in.size());
            volk_32fc_magnitude_32f(retval.data(), (const lv_32fc_t*)in.data(), in.size());
            return retval;
        }

        FloatArray scipyspecialexpn(const FloatArray& in) {
            FloatArray retval(in.size());
            auto rvD = retval.data();
            auto inD = in.data();
            for (size_t q = 0; q < in.size(); q++) {
                rvD[q] = dsp::math::expn(inD[q]);
            }
            return retval;
        }

        // only even window sizes
        FloatArray npmavg(const FloatArray& v, int windowSize) {
            FloatArray retval(v.size());
            auto rvD = retval.data();
            size_t written = 0;
            float sum = 0;
            float count = 0;
            auto ws2 = windowSize / 2;
            for (int ix = 0; ix < v.size() + ws2; ix++) {
                if (ix < v.size()) {
                    sum += v[ix];
                    count++;
                }
                if (ix > windowSize) {
                    count--;
                    sum -= v[ix - count];
                }
                if (ix >= ws2) {
                    rvD[written++] = sum / count;
                }
            }
            if (written != v.size()) {
                flog::info("FloatArray npmavg: written != v.size() {} {}\n", (int)written, (int)v.size());
                abort();
            }
            return retval;
        }

        struct fftwPlanImplFFTW : public FFTPlan {
            int nbuckets;
            bool reverse;
            // Fixed-size working buffers the FFTW plan is bound to; they must
            // never reallocate.
            ComplexArray input;
            ComplexArray output;
            // Declared last so it is destroyed first, before the arrays it was
            // planned against -- the order the hand-written destructor had.
            dsp::buffer::FFTWPlan p;

            fftwPlanImplFFTW(bool backward, int buckets) : FFTPlan() {
                nbuckets = buckets;
                reverse = backward;
                // assign, not resize: the default-init allocator leaves
                // resized elements uninitialized, and output is readable
                // via getOutput() before the first execute.
                input.assign(buckets, dsp::complex_t{ 0, 0 });
                output.assign(buckets, dsp::complex_t{ 0, 0 });
                // FFTWPlan serialises this against every other planner call
                // in the process: this one runs on a DSP worker while the UI
                // and the IQ front end plan on their own threads, and only
                // fftwf_execute() is thread-safe.
                p.planDFT1D(buckets, (fftwf_complex*)input.data(), (fftwf_complex*)output.data(), backward ? FFTW_BACKWARD : FFTW_FORWARD);
            }

            const ComplexArray& getOutput() override {
                return output;
            }

            const ComplexArray& npfftfft(const ComplexArray& in) override {
                // Zero-pad / truncate in into the plan-bound input buffer.
                auto limit = std::min(in.size(), (size_t)nbuckets);
                std::copy(in.begin(), in.begin() + limit, input.begin());
                std::fill(input.begin() + limit, input.end(), dsp::complex_t{ 0, 0 });
                fftwf_execute(p);
                if (reverse) {
                    volk_32fc_s32fc_multiply_32fc((lv_32fc_t*)output.data(), (lv_32fc_t*)output.data(), lv_32fc_t(1.0f / nbuckets, 0.0f), output.size());
                }
                return output;
            }

            virtual ~fftwPlanImplFFTW() {}
        };

        std::shared_ptr<FFTPlan> allocateFFTWPlan(bool backward, int buckets) {
            return std::make_shared<fftwPlanImplFFTW>(backward, buckets);
        }

        void npfftfft(const ComplexArray& in, const std::shared_ptr<FFTPlan>& plan) {
            plan->npfftfft(in);
        }
    }
}
