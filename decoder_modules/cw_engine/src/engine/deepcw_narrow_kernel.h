/*
 * deepcw_narrow_kernel.h — the encoder of the DeepCW v2 narrow model as
 * hand-written loops: the shapes, the weight pointers, and one entry per
 * instruction set (deepcw_narrow_kernel.inc, compiled once per ISA by
 * deepcw_narrow_kernel_{generic,sse,avx2,neon}.cpp). deepcw_narrow_fused.h is the
 * class the backends use; this header is its private half.
 *
 * Plain data and declarations only: the AVX2 translation unit is compiled
 * with -mavx2 -mfma, and an inline function shared with the rest of the
 * program could be emitted there with AVX2 code (an ODR hazard on CPUs
 * without it).
 *
 * Compiled only with OPT_BUILD_CW_DEEPCW.
 *
 * Part of SDRIAK. GPL-3.0-or-later.
 */
#pragma once

#include <cstddef>

namespace dcwn {

constexpr unsigned kD          = 64;   // model width
constexpr unsigned kFF         = 128;  // FFN and the conv module's pointwise_in
constexpr unsigned kHeads      = 4;
constexpr unsigned kHeadDim    = 16;
constexpr unsigned kLayers     = 3;
constexpr unsigned kDwKernel   = 15;
constexpr unsigned kFeat       = 24;   // front-end channels into the projection
constexpr unsigned kClasses    = 42;
constexpr unsigned kClassesPad = 48;   // the classifier padded to whole 16-lane blocks

// Every matrix is [in][out], the output channels contiguous.
struct Layer {
    const float *ffn1_ng, *ffn1_nb, *ffn1_w1, *ffn1_b1, *ffn1_w2, *ffn1_b2;
    const float *attn_ng, *attn_nb, *qkv_w, *qkv_b, *out_w, *out_b;
    const float *conv_ng, *conv_nb, *pwin_w, *pwin_b, *dw_w, *dw_b, *pwout_w, *pwout_b;
    const float *ffn2_ng, *ffn2_nb, *ffn2_w1, *ffn2_b1, *ffn2_w2, *ffn2_b2;
    const float *final_ng, *final_nb;
};

struct Net {
    const float *proj_w, *proj_b, *proj_ng, *proj_nb;
    Layer        layer[kLayers];
    const float *out_ng, *out_nb;
    const float *cls_w, *cls_b;          // [64][48], [48]: columns 42.. zero
};

// Scratch floats encode_*() needs for T1 encoder frames.
size_t work_floats(unsigned T1);

// feat [T1][24] (the front end's output) and pe [T1][64] (the position
// code) -> logits [T1][48] (before the log-softmax; columns 42.. unused).
// work: work_floats(T1) floats.
void encode_generic(const Net &net, const float *feat, const float *pe, unsigned T1, float *logits, float *work);
void encode_avx2(const Net &net, const float *feat, const float *pe, unsigned T1, float *logits, float *work);
void encode_neon(const Net &net, const float *feat, const float *pe, unsigned T1, float *logits, float *work);
void encode_sse(const Net &net, const float *feat, const float *pe, unsigned T1, float *logits, float *work);

// Y [M][N] = X [M][K] W [K][N] + b (N a multiple of 16): the front end's
// second conv as an im2col product.
void matmul_generic(const float *X, unsigned M, unsigned K, const float *W, unsigned N, const float *b, float *Y);
void matmul_avx2(const float *X, unsigned M, unsigned K, const float *W, unsigned N, const float *b, float *Y);
void matmul_neon(const float *X, unsigned M, unsigned K, const float *W, unsigned N, const float *b, float *Y);
void matmul_sse(const float *X, unsigned M, unsigned K, const float *W, unsigned N, const float *b, float *Y);

// encode_avx2 is compiled in (an x86 build) — the CPU check is the caller's.
bool avx2_compiled();
// encode_neon is compiled in (AArch64, where NEON is the baseline, or 32-bit
// ARM built with NEON, as the NDK builds armeabi-v7a).
bool neon_compiled();
// encode_sse is compiled in (an x86 build with SSE2, x86-64's baseline).
bool sse_compiled();

} // namespace dcwn
