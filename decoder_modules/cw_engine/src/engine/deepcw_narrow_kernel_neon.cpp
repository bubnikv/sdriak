/*
 * deepcw_narrow_kernel_neon.cpp — deepcw_narrow_kernel.inc on NEON, two
 * float32x4_t per 8 lanes:
 *  - AArch64 (arm64-v8a): FMA, vrndnq, vsqrtq and the across-vector
 *    reductions are the baseline, so no CPU check is needed;
 *  - 32-bit ARM with NEON (armeabi-v7a: the NDK builds it with NEON): no
 *    FMA guaranteed (VFPv4 is optional on ARMv7), so vmlaq (a · b + c
 *    rounded twice, as MLAS's ARMv7 SGEMM); rounding to nearest by the
 *    1.5 · 2^23 trick (NEON always rounds to nearest), pairwise reductions,
 *    a scalar square root; 16 vector registers, so 2-row blocks as on SSE.
 *    NEON flushes denormals itself in 32-bit mode.
 * Elsewhere (and on 32-bit ARM built without NEON, e.g. armhf's default
 * VFPv3-D16) it compiles to a stub.
 *
 * Written for phones such as the Samsung Galaxy A24 (MediaTek Helio G99:
 * 2 × Cortex-A76 + 6 × Cortex-A55, ARMv8.2-A). AArch64 has 32 vector
 * registers, so the 6 × 16 GEMM block keeps its 24 accumulators in
 * registers. The front end's convs run on this file's matmul: one FMA chain
 * from zero, as MLAS's AArch64 SGEMM, so the GroupNorms see ONNX Runtime's
 * values (deepcw_narrow_fused.cpp).
 *
 * vrcp: vrecpe and two Newton steps (vrecps), about 1 ulp. vexp: Cephes
 * expf, as the AVX2 file's.
 *
 * Part of SDRIAK. GPL-3.0-or-later.
 */
#include "deepcw_narrow_kernel.h"

#if defined(__aarch64__) || defined(_M_ARM64) || (defined(__ARM_NEON) && defined(__arm__))

#include <arm_neon.h>

#if defined(__aarch64__) || defined(_M_ARM64)
#define DCWN_A64 1
#else
#include <cmath>
#endif

namespace dcwn {
namespace {

struct V8 {
    float32x4_t a, b;
};

inline V8 vzero() { return V8{ vdupq_n_f32(0.0f), vdupq_n_f32(0.0f) }; }
inline V8 vload(const float *p) { return V8{ vld1q_f32(p), vld1q_f32(p + 4) }; }
inline void vstore(float *p, V8 x) {
    vst1q_f32(p, x.a);
    vst1q_f32(p + 4, x.b);
}
inline V8 vset1(float x) { return V8{ vdupq_n_f32(x), vdupq_n_f32(x) }; }
inline V8 vbcast(const float *p) {
    const float32x4_t d = vld1q_dup_f32(p);
    return V8{ d, d };
}
inline V8 vadd(V8 x, V8 y) { return V8{ vaddq_f32(x.a, y.a), vaddq_f32(x.b, y.b) }; }
inline V8 vsub(V8 x, V8 y) { return V8{ vsubq_f32(x.a, y.a), vsubq_f32(x.b, y.b) }; }
inline V8 vmul(V8 x, V8 y) { return V8{ vmulq_f32(x.a, y.a), vmulq_f32(x.b, y.b) }; }
inline V8 vmax(V8 x, V8 y) { return V8{ vmaxq_f32(x.a, y.a), vmaxq_f32(x.b, y.b) }; }
#if defined(DCWN_A64)
// x · y + c, one rounding
inline float32x4_t fma4(float32x4_t c, float32x4_t x, float32x4_t y) { return vfmaq_f32(c, x, y); }
// c − x · y, one rounding
inline float32x4_t fms4(float32x4_t c, float32x4_t x, float32x4_t y) { return vfmsq_f32(c, x, y); }
#else
// x · y + c and c − x · y, rounded twice
inline float32x4_t fma4(float32x4_t c, float32x4_t x, float32x4_t y) { return vmlaq_f32(c, x, y); }
inline float32x4_t fms4(float32x4_t c, float32x4_t x, float32x4_t y) { return vmlsq_f32(c, x, y); }
#endif
inline V8 vfma(V8 x, V8 y, V8 c) { return V8{ fma4(c.a, x.a, y.a), fma4(c.b, x.b, y.b) }; }

inline float32x4_t rcp4(float32x4_t x) {
    float32x4_t r = vrecpeq_f32(x);
    r = vmulq_f32(vrecpsq_f32(x, r), r);
    return vmulq_f32(vrecpsq_f32(x, r), r);
}
inline V8 vrcp(V8 x) { return V8{ rcp4(x.a), rcp4(x.b) }; }

#if defined(DCWN_A64)
inline float vhsum(V8 x) { return vaddvq_f32(vaddq_f32(x.a, x.b)); }
inline float vhmax(V8 x) { return vmaxvq_f32(vmaxq_f32(x.a, x.b)); }
inline float ssqrt(float x) { return vgetq_lane_f32(vsqrtq_f32(vdupq_n_f32(x)), 0); }
#else
inline float vhsum(V8 x) {
    const float32x4_t s = vaddq_f32(x.a, x.b);
    const float32x2_t p = vadd_f32(vget_low_f32(s), vget_high_f32(s));
    return vget_lane_f32(vpadd_f32(p, p), 0);
}
inline float vhmax(V8 x) {
    const float32x4_t s = vmaxq_f32(x.a, x.b);
    const float32x2_t p = vmax_f32(vget_low_f32(s), vget_high_f32(s));
    return vget_lane_f32(vpmax_f32(p, p), 0);
}
inline float ssqrt(float x) { return std::sqrt(x); }
#endif

inline float32x4_t exp4(float32x4_t x) {
    // n = round(x / ln 2), r = x − n ln 2 (in two parts), e^r by Cephes'
    // polynomial, 2^n added into the exponent bits; ~1e-38 below −87.3
    x = vminq_f32(x, vdupq_n_f32(88.0f));
    x = vmaxq_f32(x, vdupq_n_f32(-87.33654f));
#if defined(DCWN_A64)
    const float32x4_t n = vrndnq_f32(vmulq_f32(x, vdupq_n_f32(1.44269504088896341f)));
#else
    // |x / ln 2| < 2^22: adding and taking away 1.5 · 2^23 rounds to nearest
    const float32x4_t magic = vdupq_n_f32(12582912.0f);
    const float32x4_t n = vsubq_f32(vaddq_f32(vmulq_f32(x, vdupq_n_f32(1.44269504088896341f)), magic), magic);
#endif
    x = fms4(x, n, vdupq_n_f32(0.693359375f));
    x = fms4(x, n, vdupq_n_f32(-2.12194440e-4f));
    float32x4_t y = vdupq_n_f32(1.9875691500E-4f);
    y = fma4(vdupq_n_f32(1.3981999507E-3f), y, x);
    y = fma4(vdupq_n_f32(8.3334519073E-3f), y, x);
    y = fma4(vdupq_n_f32(4.1665795894E-2f), y, x);
    y = fma4(vdupq_n_f32(1.6666665459E-1f), y, x);
    y = fma4(vdupq_n_f32(5.0000001201E-1f), y, x);
    y = fma4(vaddq_f32(x, vdupq_n_f32(1.0f)), y, vmulq_f32(x, x));
    const int32x4_t e = vshlq_n_s32(vcvtq_s32_f32(n), 23);
    return vreinterpretq_f32_s32(vaddq_s32(vreinterpretq_s32_f32(y), e));
}
inline V8 vexp(V8 x) { return V8{ exp4(x.a), exp4(x.b) }; }

} // namespace
} // namespace dcwn

#define DCWN_ENCODE encode_neon
#define DCWN_MATMUL matmul_neon
#if !defined(DCWN_A64)
#define DCWN_ROWS 2   // 16 vector registers in 32-bit mode
#endif
#include "deepcw_narrow_kernel.inc"

bool dcwn::neon_compiled() { return true; }

#else

void dcwn::encode_neon(const Net &, const float *, const float *, unsigned, float *, float *) {}
void dcwn::matmul_neon(const float *, unsigned, unsigned, const float *, unsigned, const float *, float *) {}
bool dcwn::neon_compiled() { return false; }

#endif
