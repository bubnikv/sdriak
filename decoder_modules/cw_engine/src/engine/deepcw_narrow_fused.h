/*
 * deepcw_narrow_fused.h — the DeepCW v2 narrow model (e04/deepcw-engine
 * model_narrow.onnx) as one hand-written forward pass, without ONNX
 * Runtime.
 *
 * The weights come from a blob that tools/deepcw_narrow_extract.py writes
 * from the pinned model file; the blob is pinned too (kSha256) and records
 * the model's sha256. The forward pass reproduces the model node for node:
 *  - the front end's QDQ: the input and the first ReLU quantised to uint8
 *    (zero point 0) and back, the conv weights int8 with per-channel scales,
 *    dequantised as ONNX DequantizeLinear does;
 *  - the two convs, GroupNorm over time (as InstanceNormalization), ReLU;
 *  - the projection, LayerNorm, the sinusoidal position code;
 *  - 3 macaron Conformer blocks at d 64 and the log-softmax head.
 * Its log-probs match ONNX Runtime's to about 1e-5, with the same greedy
 * text (deepcw2_test; SKIM_DEEPCW2_FUSED_CHECK=1 compares every window a
 * run reads). The encoder is deepcw_narrow_kernel.inc: on AVX2 + FMA when
 * the CPU has them, else on SSE2 (x86), on NEON in an AArch64 build
 * (phones), else portable.
 *
 * The weights are e04's (MIT, Copyright (c) 2026 e04); the blob carries the
 * notice. Compiled only with OPT_BUILD_CW_DEEPCW.
 *
 * Part of SDRIAK. GPL-3.0-or-later.
 */
#pragma once

#include "skim_common.h"

#include "deepcw_narrow_kernel.h"

class DeepcwNarrowFused {
public:
    // tools/deepcw_narrow_extract.py's output for model_narrow.onnx
    // fc80c4ba… (deepcw-engine 79104fb).
    static constexpr const char *kSha256 = "253ffb73161a09f3ee7d2c23d03106e4d64ecd8b846ac6a7ec7f48fe81820dc3";
    static constexpr unsigned kBins = 3;

    // The instruction set the encoder runs on. kAuto: the best this build and
    // CPU have (avx2, then sse on x86; neon on AArch64; else generic).
    enum class Isa { kAuto, kGeneric, kSse, kAvx2, kNeon };
    static const char *isa_name(Isa isa);
    // "auto", "generic", "sse", "avx2", "neon"; false for another name
    static bool isa_from_name(const std::string &name, Isa *isa);
    // What this build and CPU can run, the best first.
    static std::vector<Isa> available_isas();

    // The blob at path (verify: against kSha256); nullptr + *error when it
    // cannot be read, is not this model's, or isa is not available here.
    static std::unique_ptr<DeepcwNarrowFused> load(const std::string &path, bool verify, std::string *error,
                                                   Isa isa = Isa::kAuto);

    // Output frames for T input frames (stride 2).
    static unsigned output_frames(unsigned T) { return (T + 1) / 2; }

    // tile [T][3] -> logp [output_frames(T)][42]. Thread-safe; each calling
    // thread keeps its own scratch. isa: another available encoder than
    // load()'s (the gate compares them all); one not available runs load()'s.
    void run(const float *tile, unsigned T, float *logp, Isa isa = Isa::kAuto) const;

    // The encoder run() takes: "avx2", "sse", "neon" or "generic".
    const char *isa() const { return isa_name(m_isa); }

private:
    DeepcwNarrowFused() = default;
    bool bind(const std::string &path, const std::vector<uint8_t> &blob, std::string *error);
    struct Scratch;
    void front_end(const float *tile, unsigned T, float *feat, Scratch &s, Isa isa) const;
    const float *position_code(unsigned T1, std::vector<float> &tmp) const;
    void fill_position_code(unsigned t0, unsigned t1, float *pe) const;

    static constexpr unsigned kC0 = 12, kC1 = 24, kG0 = 6, kG1 = 8;
    // the convs as im2col products: their taps, their outputs padded to 16 lanes
    static constexpr unsigned kK0 = 9, kN0 = 16, kK1 = kC0 * 3 * 2, kN1 = 32;
    static constexpr unsigned kPeRows = 2048;        // the position code kept for windows up to 65 s

    std::vector<float> m_arena;                      // every float weight, 64-byte aligned slices
    dcwn::Net          m_net{};
    float              m_in_scale[2] = { 0, 0 };     // the QDQ scales of conv 0's and conv 1's inputs
    const float       *m_w0 = nullptr;               // [9][16]: [kt][kf][out], dequantised
    const float       *m_w1 = nullptr;               // [72][32]: [cin][kt][kf 1..2][out]
    const float       *m_zero = nullptr;             // [32] zeros: the convs' bias
    const float       *m_gn_scale[2] = {}, *m_gn_bias[2] = {}, *m_gamma[2] = {}, *m_beta[2] = {};
    const float       *m_freq = nullptr;             // [32]
    std::vector<float> m_pe;                         // [kPeRows][64]
    Isa                m_isa = Isa::kGeneric;
};
