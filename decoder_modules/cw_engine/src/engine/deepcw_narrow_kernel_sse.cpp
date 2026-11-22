/*
 * deepcw_narrow_kernel_sse.cpp — deepcw_narrow_kernel.inc on SSE2 (two
 * __m128 per 8 lanes), for x86 CPUs without AVX2 + FMA: Intel before
 * Haswell (Core 2 to Ivy Bridge), the Atom-class Celeron and Pentium J/N up
 * to Jasper Lake (no AVX at all), AMD before Excavator. SSE2 is x86-64's
 * baseline: no flags, no CPU check. Elsewhere it compiles to a stub.
 *
 * x86-64 has 16 vector registers and a vector here takes two, so the GEMM
 * and attention blocks are 2 rows (DCWN_ROWS) and the depthwise conv takes
 * 32 channels a pass. No FMA: a · b + c is rounded twice, as in MLAS's SSE
 * SGEMM on these CPUs, so the front end's convs see ONNX Runtime's values
 * there too.
 *
 * vrcp: rcpps and one Newton step. vexp: Cephes expf, as the AVX2 file's,
 * the rounding to nearest by cvtps2dq (SSE2 has no roundps).
 *
 * Part of SDRIAK. GPL-3.0-or-later.
 */
#include "deepcw_narrow_kernel.h"

#if defined(__SSE2__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2)

#include <emmintrin.h>

namespace dcwn {
namespace {

struct V8 {
    __m128 a, b;
};

inline V8 vzero() { return V8{ _mm_setzero_ps(), _mm_setzero_ps() }; }
inline V8 vload(const float *p) { return V8{ _mm_loadu_ps(p), _mm_loadu_ps(p + 4) }; }
inline void vstore(float *p, V8 x) {
    _mm_storeu_ps(p, x.a);
    _mm_storeu_ps(p + 4, x.b);
}
inline V8 vset1(float x) { return V8{ _mm_set1_ps(x), _mm_set1_ps(x) }; }
inline V8 vbcast(const float *p) {
    const __m128 d = _mm_load1_ps(p);
    return V8{ d, d };
}
inline V8 vadd(V8 x, V8 y) { return V8{ _mm_add_ps(x.a, y.a), _mm_add_ps(x.b, y.b) }; }
inline V8 vsub(V8 x, V8 y) { return V8{ _mm_sub_ps(x.a, y.a), _mm_sub_ps(x.b, y.b) }; }
inline V8 vmul(V8 x, V8 y) { return V8{ _mm_mul_ps(x.a, y.a), _mm_mul_ps(x.b, y.b) }; }
inline V8 vmax(V8 x, V8 y) { return V8{ _mm_max_ps(x.a, y.a), _mm_max_ps(x.b, y.b) }; }
// x · y + c, rounded twice
inline V8 vfma(V8 x, V8 y, V8 c) {
    return V8{ _mm_add_ps(_mm_mul_ps(x.a, y.a), c.a), _mm_add_ps(_mm_mul_ps(x.b, y.b), c.b) };
}

inline __m128 rcp4(__m128 x) {
    const __m128 r = _mm_rcp_ps(x);
    return _mm_mul_ps(r, _mm_sub_ps(_mm_set1_ps(2.0f), _mm_mul_ps(x, r)));
}
inline V8 vrcp(V8 x) { return V8{ rcp4(x.a), rcp4(x.b) }; }

inline float hsum4(__m128 s) {
    s = _mm_add_ps(s, _mm_movehl_ps(s, s));
    s = _mm_add_ss(s, _mm_shuffle_ps(s, s, 1));
    return _mm_cvtss_f32(s);
}
inline float hmax4(__m128 s) {
    s = _mm_max_ps(s, _mm_movehl_ps(s, s));
    s = _mm_max_ss(s, _mm_shuffle_ps(s, s, 1));
    return _mm_cvtss_f32(s);
}
inline float vhsum(V8 x) { return hsum4(_mm_add_ps(x.a, x.b)); }
inline float vhmax(V8 x) { return hmax4(_mm_max_ps(x.a, x.b)); }
inline float ssqrt(float x) { return _mm_cvtss_f32(_mm_sqrt_ss(_mm_set_ss(x))); }

inline __m128 exp4(__m128 x) {
    // n = round(x / ln 2) (cvtps2dq: to nearest, the default MXCSR mode),
    // r = x − n ln 2 (in two parts), e^r by Cephes' polynomial, 2^n added
    // into the exponent bits; ~1e-38 below −87.3
    x = _mm_min_ps(x, _mm_set1_ps(88.0f));
    x = _mm_max_ps(x, _mm_set1_ps(-87.33654f));
    const __m128i ni = _mm_cvtps_epi32(_mm_mul_ps(x, _mm_set1_ps(1.44269504088896341f)));
    const __m128 n = _mm_cvtepi32_ps(ni);
    x = _mm_sub_ps(x, _mm_mul_ps(n, _mm_set1_ps(0.693359375f)));
    x = _mm_sub_ps(x, _mm_mul_ps(n, _mm_set1_ps(-2.12194440e-4f)));
    __m128 y = _mm_set1_ps(1.9875691500E-4f);
    y = _mm_add_ps(_mm_mul_ps(y, x), _mm_set1_ps(1.3981999507E-3f));
    y = _mm_add_ps(_mm_mul_ps(y, x), _mm_set1_ps(8.3334519073E-3f));
    y = _mm_add_ps(_mm_mul_ps(y, x), _mm_set1_ps(4.1665795894E-2f));
    y = _mm_add_ps(_mm_mul_ps(y, x), _mm_set1_ps(1.6666665459E-1f));
    y = _mm_add_ps(_mm_mul_ps(y, x), _mm_set1_ps(5.0000001201E-1f));
    y = _mm_add_ps(_mm_mul_ps(y, _mm_mul_ps(x, x)), _mm_add_ps(x, _mm_set1_ps(1.0f)));
    const __m128i e = _mm_slli_epi32(ni, 23);
    return _mm_castsi128_ps(_mm_add_epi32(_mm_castps_si128(y), e));
}
inline V8 vexp(V8 x) { return V8{ exp4(x.a), exp4(x.b) }; }

} // namespace
} // namespace dcwn

#define DCWN_ENCODE encode_sse
#define DCWN_MATMUL matmul_sse
#define DCWN_ROWS 2
#include "deepcw_narrow_kernel.inc"

bool dcwn::sse_compiled() { return true; }

#else

void dcwn::encode_sse(const Net &, const float *, const float *, unsigned, float *, float *) {}
void dcwn::matmul_sse(const float *, unsigned, unsigned, const float *, unsigned, const float *, float *) {}
bool dcwn::sse_compiled() { return false; }

#endif
