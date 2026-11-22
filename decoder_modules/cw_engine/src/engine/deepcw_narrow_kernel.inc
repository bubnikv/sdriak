/*
 * deepcw_narrow_kernel.inc — the DeepCW v2 narrow encoder, written once
 * against an 8-lane vector API that the including file defines (V8, vzero,
 * vload, vstore, vset1, vbcast, vadd, vsub, vmul, vrcp, vmax, vfma,
 * vexp, vhsum, vhmax, ssqrt) together with DCWN_ENCODE and DCWN_MATMUL, the
 * entries' names, and optionally DCWN_ROWS: the rows of a GEMM block and the
 * queries of an attention block (6; an ISA whose 8 lanes take two of its 16
 * registers sets fewer, so that 4 · DCWN_ROWS accumulators fit).
 * vrcp is 1 / x to about 1 ulp (a division costs as much as the GEMM it
 * follows).
 *
 * Activations are [time][channel] with the channels as the SIMD lanes. The
 * operations follow e04's model_narrow.onnx node for node and in its order
 * of rounding (bias after the product, the FFN's 0.5 after the bias, the
 * residual add last; q and k each scaled by 0.5, here q by 0.25, which is
 * exact), so the logits match ONNX Runtime's to float rounding
 * (deepcw2_test, SKIM_DEEPCW2_FUSED_CHECK).
 *
 * No standard-library templates or inline functions in here: see
 * deepcw_narrow_kernel.h.
 *
 * Part of SDRIAK. GPL-3.0-or-later.
 */

#ifndef DCWN_ROWS
#define DCWN_ROWS 6
#endif

namespace dcwn {
namespace {

constexpr int kRows = DCWN_ROWS;
static_assert(kRows >= 1 && kRows <= 6, "the scores buffer holds 6 query rows (work_floats)");
// the depthwise conv's channels per pass, in 8-lane vectors: all 64 where
// 8 accumulators fit the registers, else two passes of 32
constexpr unsigned kDwVecs = kRows >= 6 ? kD / 8 : kD / 16;

enum Epilogue { kStore, kSilu, kResidual };

inline unsigned round16(unsigned n) { return (n + 15u) & ~15u; }

inline V8 sigmoid8(V8 x) { return vrcp(vadd(vset1(1.0f), vexp(vsub(vzero(), x)))); }

// v · sigmoid(v) over n floats (n a multiple of 8).
inline void silu_inplace(float *p, size_t n) {
    for (size_t i = 0; i < n; i += 8) {
        const V8 v = vload(p + i);
        vstore(p + i, vmul(v, sigmoid8(v)));
    }
}

// R rows of Y = X W + b (W [K][N], N a multiple of 16), then the epilogue:
// kStore Y = v; kSilu Y = v * sigmoid(v) (over the finished rows, outside
// the register-blocked loop); kResidual Y += v * scale.
template <int R, int E>
inline void gemm_rows(const float *X, unsigned ldx, unsigned K, const float *W, unsigned N, const float *bias,
                      float *Y, unsigned ldy, float scale) {
    for (unsigned n = 0; n < N; n += 16) {
        V8 a0[R], a1[R];
        for (int r = 0; r < R; r++) {
            a0[r] = vzero();
            a1[r] = vzero();
        }
        const float *w = W + n;
        for (unsigned k = 0; k < K; k++, w += N) {
            const V8 w0 = vload(w), w1 = vload(w + 8);
            for (int r = 0; r < R; r++) {
                const V8 x = vbcast(X + static_cast<size_t>(r) * ldx + k);
                a0[r] = vfma(x, w0, a0[r]);
                a1[r] = vfma(x, w1, a1[r]);
            }
        }
        const V8 b0 = vload(bias + n), b1 = vload(bias + n + 8);
        for (int r = 0; r < R; r++) {
            float *y = Y + static_cast<size_t>(r) * ldy + n;
            V8 v0 = vadd(a0[r], b0), v1 = vadd(a1[r], b1);
            if (E == kResidual) {
                const V8 s = vset1(scale);
                v0 = vadd(vload(y), vmul(v0, s));
                v1 = vadd(vload(y + 8), vmul(v1, s));
            }
            vstore(y, v0);
            vstore(y + 8, v1);
        }
    }
    if (E == kSilu) {
        for (int r = 0; r < R; r++)
            silu_inplace(Y + static_cast<size_t>(r) * ldy, N);
    }
}

// The last left (< kRows) rows of a GEMM.
template <int E, int R>
inline void gemm_tail(unsigned left, const float *x, unsigned ldx, unsigned K, const float *W, unsigned N,
                      const float *bias, float *y, unsigned ldy, float scale) {
    if constexpr (R > 0) {
        if (left == static_cast<unsigned>(R))
            gemm_rows<R, E>(x, ldx, K, W, N, bias, y, ldy, scale);
        else
            gemm_tail<E, R - 1>(left, x, ldx, K, W, N, bias, y, ldy, scale);
    }
}

template <int E>
void gemm(const float *X, unsigned ldx, unsigned M, unsigned K, const float *W, unsigned N, const float *bias,
          float *Y, unsigned ldy, float scale = 1.0f) {
    unsigned m = 0;
    for (; m + kRows <= M; m += kRows)
        gemm_rows<kRows, E>(X + static_cast<size_t>(m) * ldx, ldx, K, W, N, bias, Y + static_cast<size_t>(m) * ldy, ldy,
                            scale);
    gemm_tail<E, kRows - 1>(M - m, X + static_cast<size_t>(m) * ldx, ldx, K, W, N, bias,
                            Y + static_cast<size_t>(m) * ldy, ldy, scale);
}

// LayerNormalization over the 64 channels of each row: the mean, the
// variance from the deviations (two passes: E[x²] − E[x]² cancels where the
// residual stream's mean is large), then (x − mean) · (1 / sd) · g + b.
// y may be x.
void layer_norm(const float *x, unsigned M, const float *g, const float *b, float *y) {
    for (unsigned m = 0; m < M; m++) {
        const float *p = x + static_cast<size_t>(m) * kD;
        float *q = y + static_cast<size_t>(m) * kD;
        V8 s = vzero();
        for (unsigned c = 0; c < kD; c += 8)
            s = vadd(s, vload(p + c));
        const float mean = vhsum(s) / static_cast<float>(kD);
        const V8 vm = vset1(mean);
        V8 s2 = vzero();
        for (unsigned c = 0; c < kD; c += 8) {
            const V8 d = vsub(vload(p + c), vm);
            s2 = vfma(d, d, s2);
        }
        const float var = vhsum(s2) / static_cast<float>(kD);
        const V8 inv = vset1(1.0f / ssqrt(var + 1e-5f));
        for (unsigned c = 0; c < kD; c += 8)
            vstore(q + c, vfma(vmul(vsub(vload(p + c), vm), inv), vload(g + c), vload(b + c)));
    }
}

// R queries of one head: scores over every key, softmax, the weighted sum
// of the values. kt [16][T1p] (the head's keys, transposed, zero padded to
// T1p = T1 rounded up to 16), vh [T1][16] (its values), sc [R][T1p].
// The softmax's max comes out of the score loop (whole blocks of real keys
// only), the exponentials and their sum in one pass, and the 1 / sum
// scales the 16 outputs rather than the T1 weights.
template <int R>
void attn_rows(const float *qkv, unsigned i, unsigned T1, unsigned h, const float *kt, const float *vh, float *sc,
               float *out) {
    const unsigned T1p = round16(T1);
    const unsigned full = T1 & ~15u;
    float q[R][kHeadDim];
    for (int r = 0; r < R; r++) {
        for (unsigned d = 0; d < kHeadDim; d++)
            q[r][d] = qkv[static_cast<size_t>(i + r) * 3 * kD + h * kHeadDim + d] * 0.25f;
    }
    V8 vmx[R];
    for (int r = 0; r < R; r++)
        vmx[r] = vset1(-3.0e38f);
    // 16 keys a block: 2R independent FMA chains
    for (unsigned j = 0; j < T1p; j += 16) {
        V8 a0[R], a1[R];
        for (int r = 0; r < R; r++) {
            a0[r] = vzero();
            a1[r] = vzero();
        }
        for (unsigned d = 0; d < kHeadDim; d++) {
            const float *k = kt + static_cast<size_t>(d) * T1p + j;
            const V8 k0 = vload(k), k1 = vload(k + 8);
            for (int r = 0; r < R; r++) {
                const V8 qd = vbcast(&q[r][d]);
                a0[r] = vfma(qd, k0, a0[r]);
                a1[r] = vfma(qd, k1, a1[r]);
            }
        }
        for (int r = 0; r < R; r++) {
            vstore(sc + static_cast<size_t>(r) * T1p + j, a0[r]);
            vstore(sc + static_cast<size_t>(r) * T1p + j + 8, a1[r]);
            if (j < full)
                vmx[r] = vmax(vmx[r], vmax(a0[r], a1[r]));
        }
    }
    float inv[R];
    for (int r = 0; r < R; r++) {
        float *s = sc + static_cast<size_t>(r) * T1p;
        float mx = vhmax(vmx[r]);
        for (unsigned j = full; j < T1; j++)
            mx = s[j] > mx ? s[j] : mx;
        const V8 m = vset1(mx);
        V8 vs = vzero();
        for (unsigned j = 0; j < full; j += 8) {
            const V8 e = vexp(vsub(vload(s + j), m));
            vstore(s + j, e);
            vs = vadd(vs, e);
        }
        float tail = 0.0f;
        if (full < T1) {
            for (unsigned j = full; j < T1p; j += 8)
                vstore(s + j, vexp(vsub(vload(s + j), m)));
            for (unsigned j = full; j < T1; j++)
                tail += s[j];
        }
        inv[r] = 1.0f / (vhsum(vs) + tail);
    }
    V8 o0[R], o1[R];
    for (int r = 0; r < R; r++) {
        o0[r] = vzero();
        o1[r] = vzero();
    }
    const float *v = vh;
    for (unsigned j = 0; j < T1; j++, v += kHeadDim) {
        const V8 v0 = vload(v), v1 = vload(v + 8);
        for (int r = 0; r < R; r++) {
            const V8 p = vbcast(sc + static_cast<size_t>(r) * T1p + j);
            o0[r] = vfma(p, v0, o0[r]);
            o1[r] = vfma(p, v1, o1[r]);
        }
    }
    for (int r = 0; r < R; r++) {
        float *o = out + static_cast<size_t>(i + r) * kD + h * kHeadDim;
        const V8 s = vset1(inv[r]);
        vstore(o, vmul(o0[r], s));
        vstore(o + 8, vmul(o1[r], s));
    }
}

template <int R>
inline void attn_tail(unsigned left, const float *qkv, unsigned i, unsigned T1, unsigned h, const float *kt,
                      const float *vh, float *sc, float *out) {
    if constexpr (R > 0) {
        if (left == static_cast<unsigned>(R))
            attn_rows<R>(qkv, i, T1, h, kt, vh, sc, out);
        else
            attn_tail<R - 1>(left, qkv, i, T1, h, kt, vh, sc, out);
    }
}

// Multi-head self-attention without a mask: qkv [T1][192] -> out [T1][64].
// kt [16][T1p], vh [T1][16], sc [6][T1p]: one head's keys, values, scores.
void attention(const float *qkv, unsigned T1, float *out, float *kt, float *vh, float *sc) {
    const unsigned T1p = round16(T1);
    for (unsigned h = 0; h < kHeads; h++) {
        for (unsigned d = 0; d < kHeadDim; d++) {
            float *k = kt + static_cast<size_t>(d) * T1p;
            const float *src = qkv + kD + h * kHeadDim + d;
            for (unsigned j = 0; j < T1; j++)
                k[j] = src[static_cast<size_t>(j) * 3 * kD];
            for (unsigned j = T1; j < T1p; j++)
                k[j] = 0.0f;
        }
        for (unsigned j = 0; j < T1; j++) {
            const float *src = qkv + static_cast<size_t>(j) * 3 * kD + 2 * kD + h * kHeadDim;
            vstore(vh + static_cast<size_t>(j) * kHeadDim, vload(src));
            vstore(vh + static_cast<size_t>(j) * kHeadDim + 8, vload(src + 8));
        }
        unsigned i = 0;
        for (; i + kRows <= T1; i += kRows)
            attn_rows<kRows>(qkv, i, T1, h, kt, vh, sc, out);
        attn_tail<kRows - 1>(T1 - i, qkv, i, T1, h, kt, vh, sc, out);
    }
}

// GLU over pointwise_in's 128 channels: x [T1][128] -> y [T1][64].
void glu(const float *x, unsigned T1, float *y) {
    for (unsigned t = 0; t < T1; t++) {
        const float *p = x + static_cast<size_t>(t) * kFF;
        float *q = y + static_cast<size_t>(t) * kD;
        for (unsigned c = 0; c < kD; c += 8)
            vstore(q + c, vmul(vload(p + c), sigmoid8(vload(p + kD + c))));
    }
}

// The depthwise conv (k 15, zero pad 7) and its SiLU: x [T1][64] -> y.
void dwconv_silu(const float *x, unsigned T1, const float *w, const float *b, float *y) {
    constexpr unsigned kHalf = kDwKernel / 2;
    for (unsigned t = 0; t < T1; t++) {
        const unsigned j0 = t < kHalf ? kHalf - t : 0;
        const unsigned j1 = t + kHalf >= T1 ? kHalf + T1 - t : kDwKernel;
        for (unsigned c0 = 0; c0 < kD; c0 += 8 * kDwVecs) {
            V8 acc[kDwVecs];
            for (unsigned c = 0; c < kDwVecs; c++)
                acc[c] = vzero();
            for (unsigned j = j0; j < j1; j++) {
                const float *p = x + static_cast<size_t>(t + j - kHalf) * kD + c0;
                const float *k = w + static_cast<size_t>(j) * kD + c0;
                for (unsigned c = 0; c < kDwVecs; c++)
                    acc[c] = vfma(vload(p + 8 * c), vload(k + 8 * c), acc[c]);
            }
            float *q = y + static_cast<size_t>(t) * kD + c0;
            for (unsigned c = 0; c < kDwVecs; c++) {
                const V8 v = vadd(acc[c], vload(b + c0 + 8 * c));
                vstore(q + 8 * c, vmul(v, sigmoid8(v)));
            }
        }
    }
}

// The macaron FFN's half step: h += 0.5 · W2 SiLU(W1 LN(h)).
void ffn(float *h, unsigned T1, const float *ng, const float *nb, const float *w1, const float *b1, const float *w2,
         const float *b2, float *y, float *big) {
    layer_norm(h, T1, ng, nb, y);
    gemm<kSilu>(y, kD, T1, kD, w1, kFF, b1, big, kFF);
    gemm<kResidual>(big, kFF, T1, kFF, w2, kD, b2, h, kD, 0.5f);
}

} // namespace

void DCWN_MATMUL(const float *X, unsigned M, unsigned K, const float *W, unsigned N, const float *b, float *Y) {
    gemm<kStore>(X, K, M, K, W, N, b, Y, N);
}

void DCWN_ENCODE(const Net &net, const float *feat, const float *pe, unsigned T1, float *logits, float *work) {
    const unsigned T1p = round16(T1);
    float *h   = work;                                   // [T1][64] the residual stream
    float *y   = h + static_cast<size_t>(T1) * kD;       // [T1][64]
    float *z   = y + static_cast<size_t>(T1) * kD;       // [T1][64]
    float *big = z + static_cast<size_t>(T1) * kD;       // [T1][192]
    float *kt  = big + static_cast<size_t>(T1) * 3 * kD; // [16][T1p]
    float *vh  = kt + static_cast<size_t>(kHeadDim) * T1p; // [T1p][16]
    float *sc  = vh + static_cast<size_t>(kHeadDim) * T1p; // [6][T1p]

    gemm<kStore>(feat, kFeat, T1, kFeat, net.proj_w, kD, net.proj_b, h, kD);
    layer_norm(h, T1, net.proj_ng, net.proj_nb, h);
    for (size_t i = 0; i < static_cast<size_t>(T1) * kD; i += 8)
        vstore(h + i, vadd(vload(h + i), vload(pe + i)));

    for (unsigned l = 0; l < kLayers; l++) {
        const Layer &L = net.layer[l];
        ffn(h, T1, L.ffn1_ng, L.ffn1_nb, L.ffn1_w1, L.ffn1_b1, L.ffn1_w2, L.ffn1_b2, y, big);

        layer_norm(h, T1, L.attn_ng, L.attn_nb, y);
        gemm<kStore>(y, kD, T1, kD, L.qkv_w, 3 * kD, L.qkv_b, big, 3 * kD);
        attention(big, T1, z, kt, vh, sc);
        gemm<kResidual>(z, kD, T1, kD, L.out_w, kD, L.out_b, h, kD, 1.0f);

        layer_norm(h, T1, L.conv_ng, L.conv_nb, y);
        gemm<kStore>(y, kD, T1, kD, L.pwin_w, kFF, L.pwin_b, big, kFF);
        glu(big, T1, z);
        dwconv_silu(z, T1, L.dw_w, L.dw_b, y);
        gemm<kResidual>(y, kD, T1, kD, L.pwout_w, kD, L.pwout_b, h, kD, 1.0f);

        ffn(h, T1, L.ffn2_ng, L.ffn2_nb, L.ffn2_w1, L.ffn2_b1, L.ffn2_w2, L.ffn2_b2, y, big);
        layer_norm(h, T1, L.final_ng, L.final_nb, h);
    }
    layer_norm(h, T1, net.out_ng, net.out_nb, h);
    gemm<kStore>(h, kD, T1, kD, net.cls_w, kClassesPad, net.cls_b, logits, kClassesPad);
}

} // namespace dcwn
