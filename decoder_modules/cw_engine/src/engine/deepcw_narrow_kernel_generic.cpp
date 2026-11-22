/*
 * deepcw_narrow_kernel_generic.cpp — deepcw_narrow_kernel.inc on plain
 * 8-float arrays, for CPUs without AVX2 and non-x86 builds (the compiler
 * vectorises the lane loops as it can). No FMA: a · b + c is rounded twice.
 *
 * Part of SDRIAK. GPL-3.0-or-later.
 */
#include "deepcw_narrow_kernel.h"

#include <cmath>

namespace dcwn {
namespace {

struct V8 {
    float v[8];
};

inline V8 vzero() { return V8{ { 0, 0, 0, 0, 0, 0, 0, 0 } }; }
inline V8 vload(const float *p) {
    V8 r;
    for (int i = 0; i < 8; i++) r.v[i] = p[i];
    return r;
}
inline void vstore(float *p, V8 a) {
    for (int i = 0; i < 8; i++) p[i] = a.v[i];
}
inline V8 vset1(float x) { return V8{ { x, x, x, x, x, x, x, x } }; }
inline V8 vbcast(const float *p) { return vset1(*p); }
#define DCWN_LANES(expr)                                                                                               \
    V8 r;                                                                                                              \
    for (int i = 0; i < 8; i++) r.v[i] = (expr);                                                                       \
    return r
inline V8 vadd(V8 a, V8 b) { DCWN_LANES(a.v[i] + b.v[i]); }
inline V8 vsub(V8 a, V8 b) { DCWN_LANES(a.v[i] - b.v[i]); }
inline V8 vmul(V8 a, V8 b) { DCWN_LANES(a.v[i] * b.v[i]); }
inline V8 vrcp(V8 a) { DCWN_LANES(1.0f / a.v[i]); }
inline V8 vmax(V8 a, V8 b) { DCWN_LANES(a.v[i] > b.v[i] ? a.v[i] : b.v[i]); }
inline V8 vfma(V8 a, V8 b, V8 c) { DCWN_LANES(a.v[i] * b.v[i] + c.v[i]); }
inline V8 vexp(V8 a) { DCWN_LANES(std::exp(a.v[i])); }
#undef DCWN_LANES
inline float vhsum(V8 a) {
    return ((a.v[0] + a.v[4]) + (a.v[2] + a.v[6])) + ((a.v[1] + a.v[5]) + (a.v[3] + a.v[7]));
}
inline float vhmax(V8 a) {
    float m = a.v[0];
    for (int i = 1; i < 8; i++) m = a.v[i] > m ? a.v[i] : m;
    return m;
}
inline float ssqrt(float x) { return std::sqrt(x); }

} // namespace
} // namespace dcwn

#define DCWN_ENCODE encode_generic
#define DCWN_MATMUL matmul_generic
#include "deepcw_narrow_kernel.inc"

size_t dcwn::work_floats(unsigned T1) {
    const size_t T1p = (T1 + 15u) & ~15u;
    return static_cast<size_t>(T1) * (3 * kD + 3 * kD) + T1p * (2 * kHeadDim + 6);
}
