/*
 * deepcw_narrow_fused.cpp — see deepcw_narrow_fused.h.
 *
 * The blob reader, the QDQ front end (0.6 of the window's 61 MMAC, plain
 * loops over the output channels) and the log-softmax head; the encoder is
 * deepcw_narrow_kernel.inc.
 *
 * Part of SDRIAK. GPL-3.0-or-later.
 */
#include "deepcw_narrow_fused.h"

#include <filesystem>
#include <fstream>
#include <map>

#include "skim_sha256.h"

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
#include <xmmintrin.h>
#define DCWN_X86 1
#if defined(_MSC_VER)
#include <intrin.h>
#endif
#endif

namespace {

constexpr char kMagic[8] = { 'D', 'C', 'W', 'N', 'A', 'R', 'W', '1' };
constexpr uint32_t kVersion = 1;
constexpr size_t kNameLen = 48;
constexpr size_t kHeaderBytes = 8 + 8 + 64;
constexpr size_t kEntryBytes = kNameLen + 4 * 2 + 4 * 4 + 4 * 2;
constexpr const char *kModelSha256 = "fc80c4baec72d756af1dbce9b70357a731b0b1a6b8306815bcfc61b6403c46e5";

uint32_t le32(const uint8_t *p) {
    return static_cast<uint32_t>(p[0]) | static_cast<uint32_t>(p[1]) << 8 | static_cast<uint32_t>(p[2]) << 16 |
           static_cast<uint32_t>(p[3]) << 24;
}

struct Entry {
    uint32_t dtype = 0, ndim = 0, dims[4] = { 1, 1, 1, 1 }, offset = 0, bytes = 0;
};

bool cpu_has_avx2_fma() {
#if defined(DCWN_X86)
#if defined(_MSC_VER)
    int r[4];
    __cpuid(r, 0);
    if (r[0] < 7)
        return false;
    __cpuid(r, 1);
    const bool fma = (r[2] >> 12) & 1, osxsave = (r[2] >> 27) & 1, avx = (r[2] >> 28) & 1;
    if (!(fma && osxsave && avx))
        return false;
    if ((_xgetbv(0) & 6) != 6)
        return false;
    __cpuidex(r, 7, 0);
    return (r[1] >> 5) & 1;
#else
    __builtin_cpu_init();
    return __builtin_cpu_supports("avx2") && __builtin_cpu_supports("fma");
#endif
#else
    return false;
#endif
}

// The QDQ pair of a uint8 tensor with zero point 0: round half to even,
// clamp to 0..255, back to float (ONNX QuantizeLinear, DequantizeLinear).
inline float qdq_u8(float x, float scale) {
    float q = std::nearbyint(x / scale);
    q = q < 0.0f ? 0.0f : (q > 255.0f ? 255.0f : q);
    return q * scale;
}

// Eigen's vectorised sum (redux_vec_linear) as ONNX Runtime's x64 build
// runs it, on SSE packets of 4: the packets from the first 16-byte aligned
// element (start) in two interleaved accumulators, their sum reduced as
// (p0 + p2) + (p1 + p3), then the unaligned head, then the tail.
float eigen_sum4(const float *v, size_t n, size_t start) {
    constexpr size_t P = 4;
    const size_t s0 = std::min(start, n);
    const size_t aligned = (n - s0) / P * P, aligned2 = (n - s0) / (2 * P) * (2 * P);
    const size_t end = s0 + aligned, end2 = s0 + aligned2;
    if (aligned == 0) {
        float r = v[0];
        for (size_t i = 1; i < n; i++)
            r = r + v[i];
        return r;
    }
    float p0[P], p1[P];
    for (size_t l = 0; l < P; l++)
        p0[l] = v[s0 + l];
    if (aligned > P) {
        for (size_t l = 0; l < P; l++)
            p1[l] = v[s0 + P + l];
        for (size_t i = s0 + 2 * P; i < end2; i += 2 * P)
            for (size_t l = 0; l < P; l++) {
                p0[l] = p0[l] + v[i + l];
                p1[l] = p1[l] + v[i + P + l];
            }
        for (size_t l = 0; l < P; l++)
            p0[l] = p0[l] + p1[l];
        if (end > end2)
            for (size_t l = 0; l < P; l++)
                p0[l] = p0[l] + v[end2 + l];
    }
    float r = (p0[0] + p0[2]) + (p0[1] + p0[3]);
    for (size_t i = 0; i < s0; i++)
        r = r + v[i];
    for (size_t i = end; i < n; i++)
        r = r + v[i];
    return r;
}

// GroupNorm as the model computes it, to the bit of ONNX Runtime's CPU
// InstanceNormalization (the first one feeds a uint8 quantiser: a last-bit
// difference there moves a value a whole 0.04 step): x planar [C][W] (the
// model's own layout, each group's channels contiguous, W the frames ×
// frequencies of one channel), groups of C / G channels. Per group: the
// mean as Eigen's sum over the group (its head aligned as an aligned buffer
// puts it) / n; the squared deviations' sum (an expression: no aligned
// head); inv = 1 / sqrt(sq / n + ε); y = x · (inv · scale) + (bias − mean ·
// inv · scale); then the per-channel gamma and beta, and the ReLU.
void group_norm_relu(float *x, size_t W, unsigned C, unsigned G, const float *gs, const float *gb, const float *gamma,
                     const float *beta, std::vector<float> &sq) {
    const size_t n = W * (C / G);
    sq.resize(n);
    for (unsigned g = 0; g < G; g++) {
        float *v = x + g * n;
        const float mean = eigen_sum4(v, n, (4 - (g * n) % 4) % 4) / static_cast<float>(n);
        for (size_t i = 0; i < n; i++) {
            const float d = v[i] - mean;
            sq[i] = d * d;
        }
        const float inv = 1.0f / std::sqrt(eigen_sum4(sq.data(), n, 0) / static_cast<float>(n) + 1e-5f);
        const float cs = inv * gs[g];
        const float sh = gb[g] - mean * cs;
        for (unsigned c = g * (C / G); c < (g + 1) * (C / G); c++) {
            float *row = x + static_cast<size_t>(c) * W;
            for (size_t i = 0; i < W; i++) {
                float y = row[i] * cs + sh;
                y = y * gamma[c] + beta[c];
                row[i] = y > 0.0f ? y : 0.0f;
            }
        }
    }
}

} // namespace

const char *DeepcwNarrowFused::isa_name(Isa isa) {
    switch (isa) {
    case Isa::kAuto: return "auto";
    case Isa::kSse: return "sse";
    case Isa::kAvx2: return "avx2";
    case Isa::kNeon: return "neon";
    default: return "generic";
    }
}

bool DeepcwNarrowFused::isa_from_name(const std::string &name, Isa *isa) {
    for (Isa i : { Isa::kAuto, Isa::kGeneric, Isa::kSse, Isa::kAvx2, Isa::kNeon }) {
        if (name == isa_name(i)) {
            *isa = i;
            return true;
        }
    }
    return false;
}

std::vector<DeepcwNarrowFused::Isa> DeepcwNarrowFused::available_isas() {
    std::vector<Isa> v;
    if (dcwn::avx2_compiled() && cpu_has_avx2_fma())
        v.push_back(Isa::kAvx2);
    if (dcwn::sse_compiled())
        v.push_back(Isa::kSse);
    if (dcwn::neon_compiled())
        v.push_back(Isa::kNeon);
    v.push_back(Isa::kGeneric);
    return v;
}

std::unique_ptr<DeepcwNarrowFused> DeepcwNarrowFused::load(const std::string &path, bool verify, std::string *error,
                                                           Isa isa) {
    auto fail = [&](const std::string &why) -> std::unique_ptr<DeepcwNarrowFused> {
        if (error)
            *error = why;
        return nullptr;
    };
    const std::vector<Isa> have = available_isas();
    if (isa != Isa::kAuto && std::find(have.begin(), have.end(), isa) == have.end())
        return fail(std::string("the fused kernel's ") + isa_name(isa) + " encoder is not available on this CPU or build");
    std::ifstream f(std::filesystem::u8path(path), std::ios::binary);
    if (!f)
        return fail("fused weights not found: " + path);
    std::vector<uint8_t> blob((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (verify) {
        const std::string sha = SkimSha256::of(std::string_view(reinterpret_cast<const char *>(blob.data()), blob.size()));
        if (sha != kSha256)
            return fail(path + " is not the narrow model's fused weights (sha256 " + sha.substr(0, 8) + "…, expected " +
                        std::string(kSha256).substr(0, 8) + "…): write it with tools/deepcw_narrow_extract.py");
    }
    std::unique_ptr<DeepcwNarrowFused> m(new DeepcwNarrowFused());
    if (!m->bind(path, blob, error))
        return nullptr;
    m->m_isa = isa == Isa::kAuto ? have.front() : isa;
    return m;
}

bool DeepcwNarrowFused::bind(const std::string &path, const std::vector<uint8_t> &blob, std::string *error) {
    auto fail = [&](const std::string &why) {
        if (error)
            *error = path + ": " + why;
        return false;
    };
    if (blob.size() < kHeaderBytes || std::memcmp(blob.data(), kMagic, 8) != 0)
        return fail("not a DeepCW narrow fused-weights file");
    if (le32(blob.data() + 8) != kVersion)
        return fail("fused-weights version " + std::to_string(le32(blob.data() + 8)) + ", expected 1");
    const uint32_t count = le32(blob.data() + 12);
    if (std::string(reinterpret_cast<const char *>(blob.data()) + 16, 64) != kModelSha256)
        return fail("written from another model file than model_narrow.onnx fc80c4ba…");
    if (blob.size() < kHeaderBytes + static_cast<size_t>(count) * kEntryBytes)
        return fail("truncated");
    std::map<std::string, Entry> entries;
    for (uint32_t i = 0; i < count; i++) {
        const uint8_t *p = blob.data() + kHeaderBytes + static_cast<size_t>(i) * kEntryBytes;
        const char *nm = reinterpret_cast<const char *>(p);
        Entry e;
        e.dtype = le32(p + kNameLen);
        e.ndim = le32(p + kNameLen + 4);
        for (int d = 0; d < 4; d++)
            e.dims[d] = le32(p + kNameLen + 8 + 4 * d);
        e.offset = le32(p + kNameLen + 24);
        e.bytes = le32(p + kNameLen + 28);
        if (static_cast<size_t>(e.offset) + e.bytes > blob.size())
            return fail("tensor out of range");
        entries[std::string(nm, strnlen(nm, kNameLen))] = e;
    }

    m_arena.clear();
    // every tensor, its alignment, the dequantised convs and the padded classifier
    m_arena.reserve(blob.size() / 4 + 16 * (static_cast<size_t>(count) + 8) + kK0 * kN0 + kK1 * kN1 + kN1 +
                    (dcwn::kD + 1) * dcwn::kClassesPad);
    auto alloc = [&](size_t n) {
        const size_t at = (m_arena.size() + 15) & ~static_cast<size_t>(15);
        m_arena.resize(at + n, 0.0f);   // within the reserve: earlier slices stay put
        return m_arena.data() + at;
    };
    std::string bad;
    auto find = [&](const std::string &name, uint32_t dtype, std::initializer_list<uint32_t> dims) -> const Entry * {
        const auto it = entries.find(name);
        if (it == entries.end()) {
            bad = "no tensor " + name;
            return nullptr;
        }
        const Entry &e = it->second;
        size_t n = 1, d = 0;
        bool ok = e.dtype == dtype && e.ndim == dims.size();
        for (uint32_t v : dims) {
            ok = ok && e.dims[d++] == v;
            n *= v;
        }
        ok = ok && e.bytes == n * (dtype == 0 ? 4 : 1);
        if (!ok) {
            bad = "tensor " + name + " has another type or shape";
            return nullptr;
        }
        return &e;
    };
    auto f32 = [&](const std::string &name, std::initializer_list<uint32_t> dims) -> const float * {
        const Entry *e = find(name, 0, dims);
        if (!e)
            return nullptr;
        float *dst = alloc(e->bytes / 4);
        for (size_t i = 0; i < e->bytes / 4; i++) {
            const uint32_t u = le32(blob.data() + e->offset + 4 * i);
            std::memcpy(dst + i, &u, 4);
        }
        return dst;
    };
    auto i8 = [&](const std::string &name, std::initializer_list<uint32_t> dims) -> const int8_t * {
        const Entry *e = find(name, 1, dims);
        return e ? reinterpret_cast<const int8_t *>(blob.data() + e->offset) : nullptr;
    };

    // ---- front end: dequantise the int8 weights as DequantizeLinear does
    const unsigned co[2] = { kC0, kC1 }, ci[2] = { 1, kC0 }, groups[2] = { kG0, kG1 };
    for (unsigned c = 0; c < 2; c++) {
        const std::string p = "fe" + std::to_string(c) + ".";
        const float *sc = f32(p + "in_scale", { 1 });
        const int8_t *w = i8(p + "w", { co[c], ci[c], 3, 3 });
        const float *ws = f32(p + "w_scale", { co[c] });
        const int8_t *wz = i8(p + "w_zp", { co[c] });
        m_gn_scale[c] = f32(p + "gn_scale", { groups[c] });
        m_gn_bias[c] = f32(p + "gn_bias", { groups[c] });
        m_gamma[c] = f32(p + "gamma", { co[c] });
        m_beta[c] = f32(p + "beta", { co[c] });
        if (!sc || !w || !ws || !wz || !m_gn_scale[c] || !m_gn_bias[c] || !m_gamma[c] || !m_beta[c])
            return fail(bad);
        m_in_scale[c] = sc[0];
        // im2col weights [in][kt][kf][out], the outputs padded to a whole
        // 16-lane block. conv 0: [9][16]. conv 1: [12][3][kf 1..2][32] — its
        // frequency tap 0 always reads the padding (its one output frequency
        // is centred on input 0 of 2), and a tap on zero adds nothing.
        const bool c1 = c == 1;
        float *dq = alloc(c1 ? static_cast<size_t>(kK1) * kN1 : static_cast<size_t>(kK0) * kN0);
        for (unsigned o = 0; o < co[c]; o++)
            for (unsigned i = 0; i < ci[c]; i++)
                for (unsigned k = 0; k < 9; k++) {
                    const int q = w[(static_cast<size_t>(o) * ci[c] + i) * 9 + k];
                    const float v = static_cast<float>(q - static_cast<int>(wz[o])) * ws[o];
                    if (!c1)
                        dq[static_cast<size_t>(k) * kN0 + o] = v;
                    else if (k % 3 != 0)
                        dq[((static_cast<size_t>(i) * 3 + k / 3) * 2 + (k % 3 - 1)) * kN1 + o] = v;
                }
        (c == 0 ? m_w0 : m_w1) = dq;
    }

    m_zero = alloc(kN1);

    // ---- the encoder
    using dcwn::kD;
    using dcwn::kFF;
    m_net.proj_w = f32("proj.w", { dcwn::kFeat, kD });
    m_net.proj_b = f32("proj.b", { kD });
    m_net.proj_ng = f32("proj_norm.g", { kD });
    m_net.proj_nb = f32("proj_norm.b", { kD });
    m_freq = f32("pe.freq", { kD / 2 });
    bool ok = m_net.proj_w && m_net.proj_b && m_net.proj_ng && m_net.proj_nb && m_freq;
    for (unsigned l = 0; l < dcwn::kLayers && ok; l++) {
        dcwn::Layer &L = m_net.layer[l];
        const std::string p = "l" + std::to_string(l) + ".";
        const float **ffn[2][6] = { { &L.ffn1_ng, &L.ffn1_nb, &L.ffn1_w1, &L.ffn1_b1, &L.ffn1_w2, &L.ffn1_b2 },
                                    { &L.ffn2_ng, &L.ffn2_nb, &L.ffn2_w1, &L.ffn2_b1, &L.ffn2_w2, &L.ffn2_b2 } };
        for (int k = 0; k < 2; k++) {
            const std::string q = p + (k == 0 ? "ffn1." : "ffn2.");
            *ffn[k][0] = f32(q + "norm.g", { kD });
            *ffn[k][1] = f32(q + "norm.b", { kD });
            *ffn[k][2] = f32(q + "w1", { kD, kFF });
            *ffn[k][3] = f32(q + "b1", { kFF });
            *ffn[k][4] = f32(q + "w2", { kFF, kD });
            *ffn[k][5] = f32(q + "b2", { kD });
        }
        L.attn_ng = f32(p + "attn_norm.g", { kD });
        L.attn_nb = f32(p + "attn_norm.b", { kD });
        L.qkv_w = f32(p + "qkv.w", { kD, 3 * kD });
        L.qkv_b = f32(p + "qkv.b", { 3 * kD });
        L.out_w = f32(p + "out.w", { kD, kD });
        L.out_b = f32(p + "out.b", { kD });
        L.conv_ng = f32(p + "conv.norm.g", { kD });
        L.conv_nb = f32(p + "conv.norm.b", { kD });
        L.pwin_w = f32(p + "pw_in.w", { kD, kFF });
        L.pwin_b = f32(p + "pw_in.b", { kFF });
        L.dw_w = f32(p + "dw.w", { dcwn::kDwKernel, kD });
        L.dw_b = f32(p + "dw.b", { kD });
        L.pwout_w = f32(p + "pw_out.w", { kD, kD });
        L.pwout_b = f32(p + "pw_out.b", { kD });
        L.final_ng = f32(p + "final_norm.g", { kD });
        L.final_nb = f32(p + "final_norm.b", { kD });
        const float *all[] = { L.ffn1_ng, L.ffn1_nb, L.ffn1_w1, L.ffn1_b1, L.ffn1_w2, L.ffn1_b2, L.attn_ng, L.attn_nb,
                               L.qkv_w, L.qkv_b, L.out_w, L.out_b, L.conv_ng, L.conv_nb, L.pwin_w, L.pwin_b, L.dw_w,
                               L.dw_b, L.pwout_w, L.pwout_b, L.ffn2_ng, L.ffn2_nb, L.ffn2_w1, L.ffn2_b1, L.ffn2_w2,
                               L.ffn2_b2, L.final_ng, L.final_nb };
        for (const float *a : all)
            ok = ok && a;
    }
    m_net.out_ng = f32("out_norm.g", { kD });
    m_net.out_nb = f32("out_norm.b", { kD });
    const float *cw = f32("cls.w", { kD, dcwn::kClasses });
    const float *cb = f32("cls.b", { dcwn::kClasses });
    if (!ok || !m_net.out_ng || !m_net.out_nb || !cw || !cb)
        return fail(bad);
    // the classifier padded to 48 columns (zeros)
    float *pw = alloc(static_cast<size_t>(kD) * dcwn::kClassesPad);
    float *pb = alloc(dcwn::kClassesPad);
    for (unsigned k = 0; k < kD; k++)
        std::copy_n(cw + static_cast<size_t>(k) * dcwn::kClasses, dcwn::kClasses, pw + static_cast<size_t>(k) * dcwn::kClassesPad);
    std::copy_n(cb, dcwn::kClasses, pb);
    m_net.cls_w = pw;
    m_net.cls_b = pb;

    m_pe.assign(static_cast<size_t>(kPeRows) * kD, 0.0f);
    fill_position_code(0, kPeRows, m_pe.data());
    return true;
}

// pe[t][2i] = sin(t · f_i), pe[t][2i + 1] = cos(t · f_i), the angle in
// float as the model's Mul computes it.
void DeepcwNarrowFused::fill_position_code(unsigned t0, unsigned t1, float *pe) const {
    for (unsigned t = t0; t < t1; t++) {
        float *row = pe + static_cast<size_t>(t - t0) * dcwn::kD;
        for (unsigned i = 0; i < dcwn::kD / 2; i++) {
            const float a = static_cast<float>(t) * m_freq[i];
            row[2 * i] = std::sin(a);
            row[2 * i + 1] = std::cos(a);
        }
    }
}

const float *DeepcwNarrowFused::position_code(unsigned T1, std::vector<float> &tmp) const {
    if (T1 <= kPeRows)
        return m_pe.data();
    tmp.resize(static_cast<size_t>(T1) * dcwn::kD);
    fill_position_code(0, T1, tmp.data());
    return tmp.data();
}

struct DeepcwNarrowFused::Scratch {
    std::vector<float> xq, col, mm, planar, sq, c0, feat, pe, work, logits;
};

// tile [T][3] -> feat [T1][24]: QDQ, conv 3×3 stride (2, 2), GroupNorm 6,
// ReLU, QDQ, conv 3×3 stride (1, 2), GroupNorm 8, ReLU — to the bit of
// ONNX Runtime on the same ISA: each conv an im2col product (its taps in
// (input channel, time, frequency) order, one FMA chain from zero as MLAS's
// SGEMM; mul + add on the generic path, as MLAS without FMA), the
// GroupNorms in the model's planar layout.
void DeepcwNarrowFused::front_end(const float *tile, unsigned T, float *feat, Scratch &s, Isa isa) const {
    const auto matmul = isa == Isa::kAvx2 ? dcwn::matmul_avx2
                      : isa == Isa::kSse  ? dcwn::matmul_sse
                      : isa == Isa::kNeon ? dcwn::matmul_neon
                                          : dcwn::matmul_generic;
    const unsigned T1 = output_frames(T);
    const size_t R0 = static_cast<size_t>(T1) * 2;          // conv 0's outputs: frames × 2 frequencies
    s.xq.resize(static_cast<size_t>(T) * kBins);
    for (size_t i = 0; i < s.xq.size(); i++)
        s.xq[i] = qdq_u8(tile[i], m_in_scale[0]);
    // conv 0: col [T1 · 2][9] x w0 [9][16]
    s.col.assign(R0 * kK0, 0.0f);
    for (unsigned t = 0; t < T1; t++)
        for (unsigned fo = 0; fo < 2; fo++) {
            float *row = s.col.data() + (static_cast<size_t>(t) * 2 + fo) * kK0;
            for (unsigned dt = 0; dt < 3; dt++) {
                const int ti = static_cast<int>(2 * t + dt) - 1;
                for (unsigned df = 0; df < 3; df++) {
                    const int fi = static_cast<int>(2 * fo + df) - 1;
                    if (ti >= 0 && ti < static_cast<int>(T) && fi >= 0 && fi < static_cast<int>(kBins))
                        row[dt * 3 + df] = s.xq[static_cast<size_t>(ti) * kBins + fi];
                }
            }
        }
    s.mm.resize(R0 * kN0);
    matmul(s.col.data(), static_cast<unsigned>(R0), kK0, m_w0, kN0, m_zero, s.mm.data());
    // planar [12][T1][2], GroupNorm, ReLU, QDQ, back to c0 [T1][2][12]
    s.planar.resize(R0 * kC0);
    for (size_t r = 0; r < R0; r++)
        for (unsigned c = 0; c < kC0; c++)
            s.planar[c * R0 + r] = s.mm[r * kN0 + c];
    group_norm_relu(s.planar.data(), R0, kC0, kG0, m_gn_scale[0], m_gn_bias[0], m_gamma[0], m_beta[0], s.sq);
    s.c0.resize(R0 * kC0);
    for (size_t r = 0; r < R0; r++)
        for (unsigned c = 0; c < kC0; c++)
            s.c0[r * kC0 + c] = qdq_u8(s.planar[c * R0 + r], m_in_scale[1]);
    // conv 1: col [T1][12 · 3 · 2] (time taps outside the window are 0) x
    // w1 [72][32]
    s.col.assign(static_cast<size_t>(T1) * kK1, 0.0f);
    for (unsigned t = 0; t < T1; t++) {
        float *row = s.col.data() + static_cast<size_t>(t) * kK1;
        for (unsigned dt = 0; dt < 3; dt++) {
            const int ti = static_cast<int>(t + dt) - 1;
            if (ti < 0 || ti >= static_cast<int>(T1))
                continue;
            const float *src = s.c0.data() + static_cast<size_t>(ti) * 2 * kC0;
            for (unsigned c = 0; c < kC0; c++) {
                row[(c * 3 + dt) * 2] = src[c];
                row[(c * 3 + dt) * 2 + 1] = src[kC0 + c];
            }
        }
    }
    s.mm.resize(static_cast<size_t>(T1) * kN1);
    matmul(s.col.data(), T1, kK1, m_w1, kN1, m_zero, s.mm.data());
    s.planar.resize(static_cast<size_t>(T1) * kC1);
    for (unsigned t = 0; t < T1; t++)
        for (unsigned c = 0; c < kC1; c++)
            s.planar[static_cast<size_t>(c) * T1 + t] = s.mm[static_cast<size_t>(t) * kN1 + c];
    group_norm_relu(s.planar.data(), T1, kC1, kG1, m_gn_scale[1], m_gn_bias[1], m_gamma[1], m_beta[1], s.sq);
    for (unsigned t = 0; t < T1; t++)
        for (unsigned c = 0; c < kC1; c++)
            feat[static_cast<size_t>(t) * kC1 + c] = s.planar[static_cast<size_t>(c) * T1 + t];
}

void DeepcwNarrowFused::run(const float *tile, unsigned T, float *logp, Isa force) const {
    if (T == 0)
        return;
#if defined(DCWN_X86)
    // denormals flushed for the run (FTZ, DAZ), the caller's mode restored
    const unsigned csr = _mm_getcsr();
    _mm_setcsr(csr | 0x8040u);
#elif defined(__aarch64__) && (defined(__GNUC__) || defined(__clang__))
    // AArch64: FPCR.FZ for the run, the caller's mode restored
    uint64_t fpcr;
    __asm__ __volatile__("mrs %0, fpcr" : "=r"(fpcr));
    __asm__ __volatile__("msr fpcr, %0" : : "r"(fpcr | (uint64_t(1) << 24)));
#endif
    thread_local Scratch s;
    // another encoder only where it runs here (the CPU check is done once)
    static const std::vector<Isa> have = available_isas();
    const Isa isa = force != Isa::kAuto && std::find(have.begin(), have.end(), force) != have.end() ? force : m_isa;
    const unsigned T1 = output_frames(T);
    s.feat.resize(static_cast<size_t>(T1) * kC1);
    front_end(tile, T, s.feat.data(), s, isa);
    const float *pe = position_code(T1, s.pe);
    s.work.resize(dcwn::work_floats(T1));
    s.logits.resize(static_cast<size_t>(T1) * dcwn::kClassesPad);
    const auto encode = isa == Isa::kAvx2 ? dcwn::encode_avx2
                      : isa == Isa::kSse  ? dcwn::encode_sse
                      : isa == Isa::kNeon ? dcwn::encode_neon
                                          : dcwn::encode_generic;
    encode(m_net, s.feat.data(), pe, T1, s.logits.data(), s.work.data());
    // LogSoftmax over the 42 classes
    for (unsigned t = 0; t < T1; t++) {
        const float *z = s.logits.data() + static_cast<size_t>(t) * dcwn::kClassesPad;
        float *o = logp + static_cast<size_t>(t) * dcwn::kClasses;
        float mx = z[0];
        for (unsigned k = 1; k < dcwn::kClasses; k++)
            mx = std::max(mx, z[k]);
        float sum = 0.0f;
        for (unsigned k = 0; k < dcwn::kClasses; k++)
            sum += std::exp(z[k] - mx);
        const float ls = std::log(sum);
        for (unsigned k = 0; k < dcwn::kClasses; k++)
            o[k] = z[k] - mx - ls;
    }
#if defined(DCWN_X86)
    _mm_setcsr(csr);
#elif defined(__aarch64__) && (defined(__GNUC__) || defined(__clang__))
    __asm__ __volatile__("msr fpcr, %0" : : "r"(fpcr));
#endif
}
