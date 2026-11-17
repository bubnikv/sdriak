/*
 * decode_deepcw2.cpp — DeepCW v2 backend (see decode_deepcw2.h).
 *
 * Three origins, marked section by section:
 *  - deepcw2_normalize() and DeepcwStitcher: ported from e04/deepcw-engine
 *    examples/python/decode_morse.py at 79104fb (normalize_spectrogram,
 *    to_dilated_log_probs, decode_ctc_greedy, to_lane_text,
 *    PosteriorStitcher), under the MIT licence below;
 *  - the channel's front end, gate, level/offset, pane draft and output
 *    cadence: the v1 backend's (decode_deepcw.cpp — OK1BR/OK1HRA
 *    skimmer-for-linux, GPL-3.0-or-later), and its SNR from OK1BR 4db0483
 *    (gh#27);
 *  - the rest (the channel ↔ stitcher glue, the gate hangover, the TX-hold
 *    final window, restart): SDRIAK's.
 *
 * ---- licence of the ported stitcher -------------------------------------
 * MIT License
 *
 * Copyright (c) 2026 e04
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to permit
 * persons to whom the Software is furnished to do so, subject to the
 * following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN
 * NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
 * DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
 * OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
 * USE OR OTHER DEALINGS IN THE SOFTWARE.
 * -------------------------------------------------------------------------
 *
 * Part of SDRIAK. GPL-3.0-or-later (the ported parts: MIT, above).
 */
#include "decode_deepcw2.h"

#include <atomic>
#include <cinttypes>
#include <deque>
#include <limits>
#include <random>

#include "deepcw_runtime.h"

namespace {

constexpr int kClasses = DeepcwModel::kClasses;
constexpr int kBlank   = DeepcwModel::kBlank;
constexpr int kSpace   = DeepcwModel::kSpace;
constexpr double kInf  = std::numeric_limits<double>::infinity();

double env_d(const char *name, double dflt) {
    const char *v = std::getenv(name);
    return v && v[0] ? std::strtod(v, nullptr) : dflt;
}

} // namespace

DeepcwV2Config DeepcwV2Config::from_env() { return from_env(DeepcwV2Config()); }

DeepcwV2Config DeepcwV2Config::from_env(const DeepcwV2Config &base) {
    DeepcwV2Config c = base;
    if (const char *v = std::getenv("SKIM_DEEPCW2_MODEL"); v && v[0])
        c.model_path = v;
    if (const char *v = std::getenv("SKIM_ORT_LIB"); v && v[0])
        c.ort_library = v;
    if (const char *v = std::getenv("SKIM_DEEPCW2_DEVICE"); v && v[0])
        c.device = v;
    c.narrow  = env_d("SKIM_DEEPCW2_NARROW", c.narrow ? 1 : 0) != 0;
    c.verify  = env_d("SKIM_DEEPCW2_VERIFY", c.verify ? 1 : 0) != 0;
    c.threads = static_cast<int>(env_d("SKIM_DEEPCW2_THREADS", c.threads));
    c.workers = std::clamp(static_cast<int>(env_d("SKIM_DEEPCW2_WORKERS", c.workers)), 1, 8);
    c.batch   = std::clamp(static_cast<int>(env_d("SKIM_DEEPCW2_BATCH", c.batch)), 1, 256);
    c.sync    = env_d("SKIM_DEEPCW2_SYNC", c.sync ? 1 : 0) != 0;
    c.debug   = static_cast<int>(env_d("SKIM_DEEPCW2_DEBUG", c.debug));
    c.gate    = env_d("SKIM_DEEPCW2_GATE", c.gate ? 1 : 0) != 0;
    c.gate_db = env_d("SKIM_DEEPCW2_GATE_DB", c.gate_db);
    c.draft   = env_d("SKIM_DEEPCW2_DRAFT", c.draft ? 1 : 0) != 0;
    if (const char *v = std::getenv("SKIM_DEEPCW2_NOISE"); v && v[0])
        c.noise_mode = v;
    c.model_v1 = env_d("SKIM_DEEPCW2_V1", c.model_v1 ? 1 : 0) != 0;
    c.stitch.step_s        = env_d("SKIM_DEEPCW2_STEP", c.stitch.step_s);
    c.stitch.max_segment_s = env_d("SKIM_DEEPCW2_WIN", c.stitch.max_segment_s);
    c.win_ring_s = std::max(c.win_ring_s, c.stitch.max_segment_s + 2.0);
    return c;
}

// ==== BEGIN port: e04/deepcw-engine examples/python/decode_morse.py @79104fb (MIT) ====

// normalize_spectrogram(): "noise_q90_log1p". The 90th percentile of the
// magnitudes is the noise's when most cells are noise; over the Rayleigh
// factor sqrt(−2 ln(1 − q)) it is the noise's Rayleigh σ.
void deepcw2_normalize(float *x, size_t n, const float *src, size_t m) {
    if (m == 0)
        return;
    constexpr double kQuantile = 0.9;                    // noise_level_quantile
    const double rayleigh = std::sqrt(-2.0 * std::log(1.0 - kQuantile));
    std::vector<float> s(src, src + m);
    const size_t qi = static_cast<size_t>(std::floor(kQuantile * static_cast<double>(m - 1)));
    std::nth_element(s.begin(), s.begin() + static_cast<ptrdiff_t>(qi), s.end());
    const double q = s[qi];
    const double peak = *std::max_element(s.begin(), s.end());
    const double noise = std::max({ q / rayleigh, peak * 1e-3,    // NOISE_LEVEL_MIN_RELATIVE_TO_PEAK
                                    1e-5 });                      // NOISE_LEVEL_MIN
    const float inv = static_cast<float>(1.0 / noise);
    for (size_t i = 0; i < n; i++)
        x[i] = std::log1p(x[i] * inv);
}

namespace {

void log_softmax_row(float *v) {
    float peak = v[0];
    for (int c = 1; c < kClasses; c++)
        peak = std::max(peak, v[c]);
    double sum = 0.0;
    for (int c = 0; c < kClasses; c++)
        sum += std::exp(static_cast<double>(v[c] - peak));
    const float lse = peak + static_cast<float>(std::log(sum));
    for (int c = 0; c < kClasses; c++)
        v[c] -= lse;
}

double smoothstep(double v) {
    const double t = std::clamp(v, 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}

} // namespace

DeepcwStitcher::DeepcwStitcher(const DeepcwStitchConfig &cfg, double frames_per_second)
    : m_cfg(cfg), m_fps(frames_per_second) {
    m_left_context_frames  = std::llround(cfg.left_context_s * m_fps);
    m_freeze_guard_frames  = std::llround(cfg.freeze_guard_s * m_fps);
    m_max_segment_frames   = std::llround(cfg.max_segment_s * m_fps);
    // SDRIAK: upstream counts ceil(134 hops / stride) of ITS 15 ms hop; the
    // seconds are kept here, on this backend's frame rate.
    m_word_boundary_frames = static_cast<int64_t>(std::ceil(cfg.word_boundary_s * m_fps));
}

int64_t DeepcwStitcher::context_start_frame() const {
    return std::max(m_origin, m_frozen_end - m_left_context_frames);
}

int64_t DeepcwStitcher::window_start_frame(int64_t end_frame) const {
    return std::max(context_start_frame(), end_frame - m_max_segment_frames);
}

// to_dilated_log_probs(): every class but blank takes its best value within
// ±dilation frames, so a spike placed a frame apart by two windows still
// adds up.
std::vector<float> DeepcwStitcher::dilated_log_probs(const float *logits, unsigned count) const {
    std::vector<float> lp(logits, logits + static_cast<size_t>(count) * kClasses);
    for (unsigned f = 0; f < count; f++)
        log_softmax_row(lp.data() + static_cast<size_t>(f) * kClasses);
    std::vector<float> dil(lp.size());
    const int r = m_cfg.dilation;
    for (unsigned f = 0; f < count; f++) {
        const unsigned first = f >= static_cast<unsigned>(r) ? f - r : 0;
        const unsigned last = std::min(count - 1, f + static_cast<unsigned>(r));
        float *d = dil.data() + static_cast<size_t>(f) * kClasses;
        for (int c = 0; c < kClasses; c++) {
            float v = lp[static_cast<size_t>(f) * kClasses + c];
            if (c != kBlank) {
                for (unsigned o = first; o <= last; o++)
                    v = std::max(v, lp[static_cast<size_t>(o) * kClasses + c]);
            }
            d[c] = v;
        }
        log_softmax_row(d);
    }
    return dil;
}

void DeepcwStitcher::ensure_rows(int64_t end_frame) {
    const int64_t rows = static_cast<int64_t>(m_weights.size());
    const int64_t missing = end_frame - m_row_base - rows;
    if (missing <= 0)
        return;
    m_sums.resize(m_sums.size() + static_cast<size_t>(missing) * kClasses, 0.0f);
    m_weights.resize(m_weights.size() + static_cast<size_t>(missing), 0.0f);
}

void DeepcwStitcher::drop_rows_before(int64_t frame) {
    const int64_t drop = std::min(frame - m_row_base, static_cast<int64_t>(m_weights.size()));
    if (drop <= 0)
        return;
    m_sums.erase(m_sums.begin(), m_sums.begin() + static_cast<ptrdiff_t>(drop * kClasses));
    m_weights.erase(m_weights.begin(), m_weights.begin() + static_cast<ptrdiff_t>(drop));
    m_row_base += drop;
}

// add_window(): the window's frames beyond the frozen point are averaged
// in, weighted down near its edges (no left ramp at the stream's start, no
// right ramp on the final window).
void DeepcwStitcher::add_window(int64_t start, const float *logits, unsigned count, bool is_final) {
    if (count == 0) {
        if (is_final)
            m_frozen_end = m_covered_end;
        return;
    }
    const int64_t end = start + count;
    ensure_rows(end);
    const std::vector<float> lp = dilated_log_probs(logits, count);
    for (int64_t f = std::max<int64_t>(0, m_frozen_end - start); f < static_cast<int64_t>(count); f++) {
        const double left = start == m_origin ? kInf : static_cast<double>(f) / m_fps;
        const double right = is_final ? kInf : static_cast<double>(count - 1 - f) / m_fps;
        const float w = static_cast<float>(std::max(
            m_cfg.min_view_weight, smoothstep(left / m_cfg.left_ramp_s) * smoothstep(right / m_cfg.right_ramp_s)));
        const int64_t row = start + f - m_row_base;
        if (row < 0)
            continue;   // SDRIAK guard: a frame already dropped
        float *s = m_sums.data() + static_cast<size_t>(row) * kClasses;
        const float *l = lp.data() + static_cast<size_t>(f) * kClasses;
        for (int c = 0; c < kClasses; c++)
            s[c] += w * l[c];
        m_weights[static_cast<size_t>(row)] += w;
    }
    m_covered_end = std::max(m_covered_end, end);
    m_frozen_end = is_final ? m_covered_end : std::max(m_frozen_end, end - m_freeze_guard_frames);
}

// _read_scores(): the averaged log-probs; an uncovered frame reads blank.
std::vector<float> DeepcwStitcher::read_scores(int64_t start, int64_t end) const {
    std::vector<float> sc(static_cast<size_t>(end - start) * kClasses);
    for (int64_t f = start; f < end; f++) {
        float *o = sc.data() + static_cast<size_t>(f - start) * kClasses;
        const int64_t row = f - m_row_base;
        const float w = row >= 0 && row < static_cast<int64_t>(m_weights.size()) ? m_weights[static_cast<size_t>(row)] : 0.0f;
        if (w > 0) {
            const float *s = m_sums.data() + static_cast<size_t>(row) * kClasses;
            for (int c = 0; c < kClasses; c++)
                o[c] = s[c] / w;
        } else {
            std::fill_n(o, kClasses, static_cast<float>(m_cfg.uncovered_log_prob));
            o[kBlank] = 0.0f;
        }
    }
    return sc;
}

// decode_ctc_greedy(): best path; a blank run of word_boundary frames or
// more between two characters becomes a word gap (counting the blanks
// carried over from the last take at the start); then the spans.
std::vector<DeepcwStitcher::Span> DeepcwStitcher::decode_greedy(const std::vector<float> &scores, int64_t n,
                                                                std::vector<int> &argmax) const {
    argmax.assign(static_cast<size_t>(n), 0);
    for (int64_t f = 0; f < n; f++) {
        const float *row = scores.data() + static_cast<size_t>(f) * kClasses;
        int best = 0;
        for (int c = 1; c < kClasses; c++) {
            if (row[c] > row[best])
                best = c;
        }
        argmax[static_cast<size_t>(f)] = best;
    }
    std::vector<int> idx = argmax;
    int previous = m_previous == Symbol::Char ? -2 : m_previous == Symbol::Space ? kSpace : -1;
    for (int64_t f = 0; f < n;) {
        if (idx[static_cast<size_t>(f)] != kBlank) {
            previous = idx[static_cast<size_t>(f)];
            f++;
            continue;
        }
        const int64_t run_start = f;
        while (f < n && idx[static_cast<size_t>(f)] == kBlank)
            f++;
        const int following = f < n ? idx[static_cast<size_t>(f)] : -1;
        const int64_t run = f - run_start + (run_start == 0 ? m_blank_frames : 0);
        if (run >= m_word_boundary_frames && previous != -1 && previous != kSpace && following >= 0 &&
            following != kSpace) {
            std::fill(idx.begin() + run_start, idx.begin() + f, kSpace);
        }
    }
    std::vector<Span> spans;
    int prev = -1;
    ptrdiff_t active = -1;
    for (int64_t f = 0; f < n; f++) {
        const int c = idx[static_cast<size_t>(f)];
        if (c == kBlank) {
            prev = -1;
            active = -1;
            continue;
        }
        if (c == prev) {
            if (active >= 0)
                spans[static_cast<size_t>(active)].end = f;
            continue;
        }
        prev = c;
        spans.push_back({ static_cast<uint8_t>(c), f, f });
        active = static_cast<ptrdiff_t>(spans.size()) - 1;
    }
    return spans;
}

// to_lane_text(): spaces only between characters; a leading one is a flag.
DeepcwStitcher::LaneText DeepcwStitcher::to_lane_text(const std::vector<Span> &spans, size_t from, size_t to,
                                                      bool prepend) {
    LaneText t;
    t.prepend_space = prepend;
    bool pending_space = false;
    for (size_t i = from; i < to; i++) {
        if (spans[i].cls == kSpace) {
            if (t.text.empty())
                t.prepend_space = true;
            else
                pending_space = true;
            continue;
        }
        if (pending_space) {
            t.text.push_back(' ');
            pending_space = false;
        }
        t.text.push_back(DeepcwModel::kChars[spans[i].cls]);
    }
    return t;
}

// take(): commit every character that ends before the frozen point (all on
// the final take), release the frames up to the next character, and read
// the rest as pending.
DeepcwStitcher::Take DeepcwStitcher::take(bool final) {
    Take t;
    const int64_t start = m_committed_end;
    const int64_t n = m_covered_end - start;
    if (n <= 0)
        return t;
    const bool starts_word = !m_has_committed || m_previous == Symbol::Space;
    const std::vector<float> scores = read_scores(start, m_covered_end);
    std::vector<int> argmax;
    std::vector<Span> spans = decode_greedy(scores, n, argmax);
    for (Span &s : spans) {
        s.start += start;
        s.end += start;
    }
    const int64_t limit = final ? std::numeric_limits<int64_t>::max() : m_frozen_end;
    size_t cc = 0;
    for (size_t i = 0; i < spans.size(); i++) {
        if (spans[i].end + 1 >= limit)
            break;
        if (spans[i].cls != kSpace)
            cc = i + 1;
    }
    if (cc > 0) {
        t.committed = to_lane_text(spans, 0, cc, starts_word);
        // SDRIAK: confidence and extent of the committed characters.
        double ps = 0.0;
        for (size_t i = 0; i < cc; i++) {
            if (spans[i].cls == kSpace)
                continue;
            ps += std::exp(static_cast<double>(scores[static_cast<size_t>(spans[i].start - start) * kClasses + spans[i].cls]));
            t.letters++;
            if (t.first < 0)
                t.first = spans[i].start;
            t.last = spans[i].end;
        }
        t.confidence = t.letters ? ps / t.letters : 0.0;
        m_has_committed = true;
        m_committed_end = spans[cc - 1].end + 1;
        m_previous = Symbol::Char;
        m_blank_frames = 0;
    }
    int64_t next_char = std::numeric_limits<int64_t>::max();
    for (size_t i = cc; i < spans.size(); i++) {
        if (spans[i].cls != kSpace) {
            next_char = spans[i].start;
            break;
        }
    }
    const int64_t release_end = std::min(final ? m_covered_end : m_frozen_end, next_char) - 1;
    Symbol ps = m_previous;
    int64_t bf = m_blank_frames;
    for (int64_t f = m_committed_end; f < release_end; f++) {
        if (argmax[static_cast<size_t>(f - start)] == kSpace) {
            ps = Symbol::Space;
            bf = 0;
        } else {
            bf++;
        }
    }
    if (release_end > m_committed_end) {
        m_previous = ps;
        m_blank_frames = bf;
        m_committed_end = release_end;
    }
    drop_rows_before(m_committed_end);
    std::vector<Span> pending;
    for (size_t i = cc; i < spans.size(); i++) {
        if (spans[i].end >= m_committed_end)
            pending.push_back(spans[i]);
    }
    t.pending = to_lane_text(pending, 0, pending.size(), !m_has_committed || m_previous == Symbol::Space);
    return t;
}

// ==== END port: e04/deepcw-engine decode_morse.py (MIT) ====

// SDRIAK: a restart after a gap the channel did not see.
void DeepcwStitcher::restart(int64_t frame) {
    m_origin = m_row_base = m_covered_end = m_frozen_end = m_committed_end = frame;
    m_sums.clear();
    m_weights.clear();
    m_previous = m_has_committed ? Symbol::Space : Symbol::None;
    m_blank_frames = 0;
}

// ---- the backend's shared state ---------------------------------------------

namespace {

const DeepcwModelSpec &spec_of(const DeepcwV2Config &c) {
    if (c.model_v1)
        return kDeepcwModelV1;
    return c.narrow ? kDeepcwModelV2Narrow : kDeepcwModelV2;
}

DeepcwRuntimeConfig runtime_config(const DeepcwV2Config &c) {
    DeepcwRuntimeConfig r;
    r.model_path  = c.model_path;
    r.ort_library = c.ort_library;
    r.device      = c.device;
    r.threads     = c.threads;
    r.workers     = c.workers;
    r.batch       = c.batch;
    r.verify      = c.verify;
    return r;
}

} // namespace

class DeepcwV2Context {
public:
    explicit DeepcwV2Context(const DeepcwV2Config &cfg) : m_cfg(cfg), m_runtime(spec_of(m_cfg), runtime_config(m_cfg)) {}
    const DeepcwV2Config &cfg() const { return m_cfg; }
    DeepcwRuntime &runtime() { return m_runtime; }
    unsigned next_registration() { return m_reg++; }
    bool first_rate_warning() { return !m_rate_warned.exchange(true); }
    bool first_model_warning() { return !m_model_warned.exchange(true); }

private:
    DeepcwV2Config    m_cfg;
    DeepcwRuntime     m_runtime;
    unsigned          m_reg = 0;
    std::atomic<bool> m_rate_warned{ false };
    std::atomic<bool> m_model_warned{ false };
};

// ---- per-channel state -------------------------------------------------------

namespace {

class DeepcwV2Channel final : public SkimDecodeChannel {
public:
    DeepcwV2Channel(std::shared_ptr<DeepcwV2Context> ctx, double rate);
    ~DeepcwV2Channel() override { m_box->dying.store(true); }

    bool process(const float *iq, unsigned nframes, SkimDecode *out) override;
    double level() const override { return m_lvl_ema; }
    double tone_offset_hz() const override { return m_off_ema; }
    void set_freq(double freq_hz) override { m_freq_hz = freq_hz; }
    bool take_pane_op(SkimPaneOp &op) override;
    void resync() override;
    void hold_begin() override;
    double noise() const override { return m_nmean; }
    void set_band_noise(double noise, double enbw_hz) override {
        if (noise > 0 && enbw_hz > 0)
            m_band_n0 = noise / (1.5 * enbw_hz);   // OK1BR 4db0483 (gh#27)
    }

    SkimDeepcw2Debug debug() const;

private:
    static constexpr int kKeep = 11;    // channel bins −5..+5 (v1's)
    static constexpr int kHalf = 5;

    float *ring_row(uint64_t f) { return m_ring.data() + static_cast<size_t>(f % m_ring_frames) * kKeep; }
    void frame();
    void tick();
    void window(bool final);
    void window_stats(uint64_t w0, uint64_t w1);
    void tile(uint64_t w0, uint64_t w1);
    void submit(unsigned T, int64_t s, bool final);
    void apply(const float *logp, unsigned To, int64_t s, bool final);
    void flush_try();
    void flush_done();
    void restart();
    void pane_compose();

    std::shared_ptr<DeepcwV2Context> m_ctx;
    const DeepcwV2Config &m_cfg;
    const int m_bins;                   // model input bins: 65 or 3
    const unsigned m_stride;            // input frames per output frame: 2 (v1 experiment: 1)
    DeepcwStitcher m_stitcher;

    double   m_rate = 0.0;
    unsigned m_N = 0, m_hop = 0;
    std::vector<float> m_win, m_tw_re, m_tw_im, m_sring, m_ring, m_tile;
    unsigned m_spos = 0, m_sfill = 0, m_since = 0;
    unsigned m_ring_frames = 0;
    uint64_t m_frames_abs = 0;
    unsigned m_tick_frames = 1, m_tick_phase = 0, m_min_pending_f = 0, m_hangover_f = 0;
    bool     m_was_open = false;
    uint64_t m_last_open = 0;           // input frame of the last open gate
    double   m_lvl_ema = 0.0, m_off_ema = 0.0, m_env_lo = 0.0;
    double   m_nacc = 0.0, m_nmean = 0.0, m_band_n0 = 0.0;
    unsigned m_nacc_n = 0;
    double   m_snr_db = 0.0, m_wpm = 0.0, m_ratio_db = 0.0, m_duty = 0.0;
    bool     m_gate = false;
    int      m_line_bin = -1;
    unsigned m_ticks = 0;
    std::string m_out;
    double   m_out_conf = 0.0;
    bool     m_has_text = false;        // anything committed on this channel
    double   m_freq_hz = 0.0;
    bool     m_dead = false, m_nomodel = false;
    std::string m_draft, m_pane_last;
    size_t   m_pane_last_fin = 0;
    bool     m_pane_open = false, m_pane_dirty = false;
    std::deque<SkimPaneOp> m_ops;
    bool     m_inflight = false;
    std::shared_ptr<DeepcwMailbox> m_box = std::make_shared<DeepcwMailbox>();
    bool     m_flush_want = false, m_flush_inflight = false, m_resync_due = false;
    std::mt19937 m_fill_rng{ 12345 };      // noise_mode "fill"
};

DeepcwV2Channel::DeepcwV2Channel(std::shared_ptr<DeepcwV2Context> ctx, double rate)
    : m_ctx(std::move(ctx)), m_cfg(m_ctx->cfg()), m_bins(m_ctx->runtime().spec().bins),
      m_stride(static_cast<unsigned>(m_ctx->runtime().spec().time_stride)),
      m_stitcher(m_cfg.stitch, rate / std::max(std::llround(rate * 0.016), 1LL) / m_stride) {
    // ---- the v1 backend's front end (decode_deepcw.cpp, OK1BR/OK1HRA) ----
    m_rate = rate;
    const double nf = rate / DeepcwModel::kBinHz;
    const unsigned N = static_cast<unsigned>(std::llround(nf));
    if (std::fabs(nf - N) > 1e-6 || N < 8) {
        if (m_ctx->first_rate_warning()) {
            std::fprintf(stderr, "deepcw2: channel rate %.1f Hz is no multiple of 12.5 Hz "
                                 "(the CW bank runs 250 Hz) — channel decodes nothing\n", rate);
        }
        m_dead = true;
        return;
    }
    std::string err;
    m_nomodel = !m_ctx->runtime().ensure_session(&err);
    if (m_nomodel && m_ctx->first_model_warning())
        std::fprintf(stderr, "deepcw2: %s — channels track but decode nothing\n", err.c_str());
    m_N   = N;
    m_hop = std::max(static_cast<unsigned>(std::llround(rate * 0.016)), 1u);   // 16 ms: 4 at 250 Hz
    m_win.resize(N);
    m_tw_re.resize(static_cast<size_t>(kKeep) * N);
    m_tw_im.resize(static_cast<size_t>(kKeep) * N);
    for (unsigned n = 0; n < N; n++) {
        m_win[n] = 0.5f - 0.5f * std::cos(2.0f * static_cast<float>(SKIM_PI) * static_cast<float>(n) / static_cast<float>(N));
        for (int k = -kHalf; k <= kHalf; k++) {
            const double a = 2.0 * SKIM_PI * static_cast<double>(k) * static_cast<double>(n) / static_cast<double>(N);
            m_tw_re[(k + kHalf) * N + n] = static_cast<float>(std::cos(a));
            m_tw_im[(k + kHalf) * N + n] = static_cast<float>(-std::sin(a));
        }
    }
    m_sring.assign(2 * N, 0.0f);
    const double fps = rate / m_hop;                             // input frames/s
    m_ring_frames = std::max(static_cast<unsigned>(std::llround(m_cfg.win_ring_s * fps)), 64u);
    m_ring.assign(static_cast<size_t>(m_ring_frames) * kKeep, 0.0f);
    m_tile.assign(static_cast<size_t>(m_ring_frames) * DeepcwModel::kBins, 0.0f);
    // ---- SDRIAK: the stitcher's cadence and the gate hangover ----
    m_tick_frames   = std::max(static_cast<unsigned>(std::llround(m_cfg.stitch.step_s * fps)), 1u);
    m_tick_phase    = (m_ctx->next_registration() * 7u) % m_tick_frames;
    m_min_pending_f = static_cast<unsigned>(std::llround(m_cfg.stitch.min_pending_s * fps));
    // After the gate closes the windows keep coming until the freeze guard
    // has passed the over's end, so its tail commits.
    m_hangover_f    = static_cast<unsigned>(std::llround(
        (m_cfg.stitch.freeze_guard_s + m_cfg.stitch.right_ramp_s + 2.0 * m_cfg.stitch.step_s) * fps));
}

// ---- the v1 backend's front end, gate and pane draft (decode_deepcw.cpp) ----

void DeepcwV2Channel::frame() {
    float *row = ring_row(m_frames_abs);
    float mags[kKeep];
    for (int b = 0; b < kKeep; b++) {
        const float *tr = m_tw_re.data() + static_cast<size_t>(b) * m_N;
        const float *ti = m_tw_im.data() + static_cast<size_t>(b) * m_N;
        float re = 0, im = 0;
        unsigned idx = m_spos;
        for (unsigned n = 0; n < m_N; n++) {
            const float w = m_win[n];
            const float x = m_sring[2 * idx] * w, y = m_sring[2 * idx + 1] * w;
            re += x * tr[n] - y * ti[n];
            im += x * ti[n] + y * tr[n];
            idx = (idx + 1 == m_N) ? 0 : idx + 1;
        }
        mags[b] = std::sqrt(re * re + im * im);
        row[b] = mags[b];
    }
    int p = 0;
    for (int b = 1; b < kKeep; b++) {
        if (mags[b] > mags[p])
            p = b;
    }
    float sum = 0;
    unsigned nfl = 0;
    for (int b = kHalf - 3; b <= kHalf + 3; b++) {
        if (b - p >= 2 || p - b >= 2) {
            sum += mags[b];
            nfl++;
        }
    }
    const float fl = nfl ? sum / nfl : mags[p];
    const double decay = std::exp(-static_cast<double>(m_hop) / (2.0 * m_rate));
    m_lvl_ema = std::max(static_cast<double>(mags[p]), m_lvl_ema * decay);
    if (m_env_lo <= 0.0)
        m_env_lo = fl;
    else
        m_env_lo += 0.01 * (fl - m_env_lo);
    {   // OK1BR 4db0483 (gh#27): the band-noise statistic
        double tot = 0.0;
        for (int b = 0; b < kKeep; b++)
            tot += static_cast<double>(mags[b]) * mags[b];
        m_nacc += tot;
        if (++m_nacc_n >= std::max(static_cast<unsigned>(m_rate / m_hop), 1u)) {
            m_nmean = m_nacc / m_nacc_n;
            m_nacc = 0.0;
            m_nacc_n = 0;
        }
    }
    if (mags[p] > 4.0f * fl && mags[p] > 0.5 * m_lvl_ema && p > 0 && p < kKeep - 1) {
        const double l = std::log(static_cast<double>(std::max(mags[p - 1], 1e-9f)));
        const double c = std::log(static_cast<double>(std::max(mags[p], 1e-9f)));
        const double r = std::log(static_cast<double>(std::max(mags[p + 1], 1e-9f)));
        const double den = l - 2.0 * c + r;
        const double delta = den != 0.0 ? std::clamp(0.5 * (l - r) / den, -0.5, 0.5) : 0.0;
        const double off = (static_cast<double>(p) - kHalf + delta) * DeepcwModel::kBinHz;
        m_off_ema += 0.05 * (off - m_off_ema);
    }
    m_frames_abs++;
    if ((m_frames_abs + m_tick_phase) % m_tick_frames == 0)
        tick();
}

// v1's gate (line over the floor, keyed duty) and OK1BR's SNR in 500 Hz,
// over input frames [w0, w1); the line bin for the narrow tile.
void DeepcwV2Channel::window_stats(uint64_t w0, uint64_t w1) {
    const unsigned T = static_cast<unsigned>(w1 - w0);
    double prof[kKeep] = {};
    for (uint64_t f = w0; f < w1; f++) {
        const float *r = ring_row(f);
        for (int b = 0; b < kKeep; b++)
            prof[b] += r[b];
    }
    int p = 0;
    for (int b = 1; b < kKeep; b++) {
        if (prof[b] > prof[p])
            p = b;
    }
    double floor_sum = 0;
    unsigned floor_n = 0;
    for (int b = 0; b < kKeep; b++) {
        if (b - p >= 3 || p - b >= 3) {
            floor_sum += prof[b];
            floor_n++;
        }
    }
    const double line = prof[p] / T;
    const double floorv = floor_n ? floor_sum / floor_n / T : 1e-12;
    m_ratio_db = 20.0 * std::log10(std::max(line, 1e-12) / std::max(floorv, 1e-12));
    double mx = 0;
    for (uint64_t f = w0; f < w1; f++)
        mx = std::max(mx, static_cast<double>(ring_row(f)[p]));
    const double thr = 0.5 * (mx + floorv);
    unsigned on = 0;
    for (uint64_t f = w0; f < w1; f++)
        on += ring_row(f)[p] > thr;
    m_duty = static_cast<double>(on) / T;
    m_gate = m_ratio_db >= m_cfg.gate_db && m_duty >= 0.03 && m_duty <= 0.97;
    m_line_bin = std::clamp(p, 1, kKeep - 2);
    // OK1BR 4db0483 (gh#27): SNR in 500 Hz (see decode_deepcw.cpp snr_500hz)
    double nb;
    if (m_band_n0 > 0.0) {
        nb = m_band_n0 * (1.5 * DeepcwModel::kBinHz);
    } else {
        const double flm = std::max(m_env_lo, 1e-12);
        nb = (4.0 / SKIM_PI) * flm * flm;
    }
    const int b0 = p > 0 ? p - 1 : p;
    const int b1 = p + 1 < kKeep ? p + 1 : p;
    double p3 = 0.0;
    unsigned non = 0;
    for (uint64_t f = w0; f < w1; f++) {
        const float *r = ring_row(f);
        if (r[p] <= 0.7 * mx)
            continue;
        for (int b = b0; b <= b1; b++)
            p3 += static_cast<double>(r[b]) * r[b];
        non++;
    }
    if (non) {
        const unsigned nbins = static_cast<unsigned>(b1 - b0 + 1);
        const double sig = (p3 / non - nbins * nb) / (nbins == 3 ? 1.5 : 1.25);
        m_snr_db = 10.0 * std::log10(std::max(sig, 1e-3 * nb) / nb) +
                   10.0 * std::log10(1.5 * DeepcwModel::kBinHz / 500.0);
    }
}

void DeepcwV2Channel::pane_compose() {
    if (!m_cfg.draft || !m_pane_dirty)
        return;
    m_pane_dirty = false;
    const size_t fin = m_out.size();
    std::string reg = m_out + m_draft;
    auto queue = [&](SkimPaneOpKind kind, std::string text, unsigned final_len) {
        SkimPaneOp op;
        op.kind = kind;
        op.final_len = final_len;
        op.text = std::move(text);
        m_ops.push_back(std::move(op));
    };
    if (reg.empty()) {
        if (m_pane_open) {
            queue(SkimPaneOpKind::Close, std::string(), 0);
            m_pane_open = false;
            m_pane_last.clear();
            m_pane_last_fin = 0;
        }
        return;
    }
    if (m_pane_open && fin == m_pane_last_fin && reg == m_pane_last)
        return;
    queue(m_pane_open ? SkimPaneOpKind::Set : SkimPaneOpKind::Open, reg, static_cast<unsigned>(fin));
    m_pane_open = true;
    m_pane_last = std::move(reg);
    m_pane_last_fin = fin;
}

bool DeepcwV2Channel::take_pane_op(SkimPaneOp &op) {
    if (m_ops.empty())
        return false;
    op = std::move(m_ops.front());
    m_ops.pop_front();
    return true;
}

// ---- SDRIAK: the channel ↔ stitcher glue -------------------------------------

void DeepcwV2Channel::tick() {
    if (m_inflight || m_frames_abs < m_min_pending_f)
        return;
    window(false);
}

// One window ending at the current input frame (rounded down to an output
// frame), starting where the stitcher wants it, on an output-frame boundary.
void DeepcwV2Channel::window(bool final) {
    const uint64_t E = m_frames_abs - m_frames_abs % m_stride;
    const int64_t end_out = static_cast<int64_t>(E / m_stride);
    int64_t s = m_stitcher.window_start_frame(end_out);
    const int64_t ring_lo = E > m_ring_frames ? static_cast<int64_t>((E - m_ring_frames + 1) / m_stride) : 0;
    s = std::max(s, ring_lo);
    if (s >= end_out) {
        if (final)
            flush_done();
        return;
    }
    const uint64_t w0 = static_cast<uint64_t>(s) * m_stride;
    window_stats(w0, E);
    if (m_nomodel) {
        if (final)
            flush_done();
        return;
    }
    if (!final) {
        if (m_gate) {
            m_was_open = true;
            m_last_open = E;
        }
        const bool hang = m_was_open && E - m_last_open < m_hangover_f;
        if (m_cfg.gate && !m_gate && !hang) {
            if (!m_draft.empty()) {         // the over ended: no draft may linger
                m_draft.clear();
                m_pane_dirty = true;
            }
            return;
        }
    }
    tile(w0, E);
    submit(static_cast<unsigned>(E - w0), s, final);
}

// The tile (see decode_deepcw2.h), normalised noise_q90_log1p.
void DeepcwV2Channel::tile(uint64_t w0, uint64_t w1) {
    const unsigned T = static_cast<unsigned>(w1 - w0);
    if (m_bins == 3) {
        const int c = m_line_bin;
        for (unsigned t = 0; t < T; t++) {
            const float *r = ring_row(w0 + t);
            for (int k = 0; k < 3; k++)
                m_tile[static_cast<size_t>(t) * 3 + k] = r[c - 1 + k];
        }
        deepcw2_normalize(m_tile.data(), static_cast<size_t>(T) * 3, m_tile.data(), static_cast<size_t>(T) * 3);
        return;
    }
    const size_t n = static_cast<size_t>(T) * DeepcwModel::kBins;
    std::fill_n(m_tile.begin(), n, 0.0f);
    std::vector<float> populated;
    populated.reserve(static_cast<size_t>(T) * kKeep);
    for (unsigned t = 0; t < T; t++) {
        const float *r = ring_row(w0 + t);
        float *row = m_tile.data() + static_cast<size_t>(t) * DeepcwModel::kBins + (DeepcwModel::kCentre - kHalf);
        for (int b = 0; b < kKeep; b++) {
            row[b] = r[b];
            populated.push_back(r[b]);
        }
    }
    if (m_cfg.model_v1) {
        // The v1 backend's tile (decode_deepcw.cpp tile(), OK1BR): the
        // window's peak to 30, log1p.
        const float mx = *std::max_element(populated.begin(), populated.end());
        const float g = mx > 0 ? 30.0f / mx : 1.0f;
        for (size_t i = 0; i < n; i++)
            m_tile[i] = std::log1p(m_tile[i] * g);
        return;
    }
    if (m_cfg.noise_mode == "q90") {
        deepcw2_normalize(m_tile.data(), n, populated.data(), populated.size());
        return;
    }
    // SDRIAK experiment (DeepcwV2Config::noise_mode): a noise level that a
    // strong line cannot lift — the Rayleigh median of the populated cells.
    std::vector<float> s = populated;
    std::nth_element(s.begin(), s.begin() + static_cast<ptrdiff_t>(s.size() / 2), s.end());
    const double sigma = std::max(static_cast<double>(s[s.size() / 2]) / 1.1774, 1e-5);
    if (m_cfg.noise_mode == "fill") {
        // The empty bins as the band would be: Rayleigh(σ) magnitudes, then
        // the reference's normalisation over the whole tile.
        std::uniform_real_distribution<double> u(1e-12, 1.0);
        for (unsigned t = 0; t < T; t++) {
            float *row = m_tile.data() + static_cast<size_t>(t) * DeepcwModel::kBins;
            for (int b = 0; b < DeepcwModel::kBins; b++) {
                if (b >= DeepcwModel::kCentre - kHalf && b <= DeepcwModel::kCentre + kHalf)
                    continue;
                row[b] = static_cast<float>(sigma * std::sqrt(-2.0 * std::log(u(m_fill_rng))));
            }
        }
        deepcw2_normalize(m_tile.data(), n, m_tile.data(), n);
        return;
    }
    const float inv = static_cast<float>(1.0 / std::max(sigma, static_cast<double>(*std::max_element(s.begin(), s.end())) * 1e-3));
    for (size_t i = 0; i < n; i++)
        m_tile[i] = std::log1p(m_tile[i] * inv);
}

void DeepcwV2Channel::submit(unsigned T, int64_t s, bool final) {
    if (m_cfg.sync) {
        std::vector<float> logp;
        unsigned To = 0;
        m_ctx->runtime().infer(m_tile.data(), T, logp, To);
        apply(logp.data(), To, s, final);
        return;
    }
    auto job = std::make_unique<DeepcwJob>();
    job->box = m_box;
    job->T = T;
    job->w0 = static_cast<uint64_t>(s);
    job->tile.assign(m_tile.begin(), m_tile.begin() + static_cast<ptrdiff_t>(static_cast<size_t>(T) * m_bins));
    if (!m_ctx->runtime().submit(std::move(job), final))
        return;
    m_flush_inflight = final;
    m_inflight = true;
}

void DeepcwV2Channel::apply(const float *logp, unsigned To, int64_t s, bool final) {
    if (To) {
        m_ticks++;
        m_stitcher.add_window(s, logp, To, final);
    } else if (final) {
        m_stitcher.add_window(s, nullptr, 0, true);
    }
    const DeepcwStitcher::Take t = m_stitcher.take(final);
    if (t.committed && !t.committed->text.empty()) {
        // decode_morse.py Decoder.run_window: the space before the text.
        const std::string add = (t.committed->prepend_space && m_has_text ? " " : "") + t.committed->text;
        const size_t before = m_out.size();
        m_out += add;
        m_has_text = true;
        const size_t tot = m_out.size();
        m_out_conf = tot ? (m_out_conf * before + t.confidence * add.size()) / tot : t.confidence;
        // v1's WPM rule on the committed span: PARIS, 12 × chars/s.
        const double secs = static_cast<double>(t.last - t.first + 1) * m_stride * m_hop / m_rate;
        if (t.letters >= 6 && secs >= 2.0) {
            const double wpm = 12.0 * t.letters / secs;
            if (wpm >= 5.0 && wpm <= 60.0)
                m_wpm = wpm;
        }
    }
    m_draft.clear();
    if (m_cfg.draft && !final && !t.pending.text.empty())
        m_draft = (t.pending.prepend_space && (m_has_text || !m_out.empty()) ? " " : "") + t.pending.text;
    m_pane_dirty = true;
    if (m_cfg.debug) {
        std::fprintf(stderr, "deepcw2: %.2f kHz s=%" PRId64 " To=%u%s ratio %.1f dB commit |%s| pending |%s| "
                             "covered %" PRId64 " frozen %" PRId64 " committed %" PRId64 "\n",
                     m_freq_hz / 1000.0, s, To, final ? " FINAL" : "", m_ratio_db,
                     t.committed ? t.committed->text.c_str() : "", t.pending.text.c_str(),
                     m_stitcher.covered_end(), m_stitcher.frozen_end(), m_stitcher.committed_end());
    }
    if (final)
        flush_done();
}

// TX hold: the stitcher's final window (decode_morse.py Decoder.finalize:
// no right ramp, everything committed). Upstream's audio simply ends; here
// the hold begins, so there is no live-end trim and no whole-word test (v1
// has both).
void DeepcwV2Channel::hold_begin() {
    if (m_dead)
        return;
    m_flush_want = true;
    flush_try();
}

void DeepcwV2Channel::flush_try() {
    if (!m_flush_want || m_inflight)
        return;
    m_flush_want = false;
    if (m_nomodel || m_stitcher.covered_end() <= m_stitcher.committed_end()) {
        flush_done();
        return;
    }
    window(true);
}

void DeepcwV2Channel::flush_done() {
    m_flush_inflight = false;
    if (!m_draft.empty()) {
        m_draft.clear();
        m_pane_dirty = true;
    }
    if (m_resync_due) {
        m_resync_due = false;
        restart();
    }
}

void DeepcwV2Channel::restart() {
    m_stitcher.restart(static_cast<int64_t>((m_frames_abs + m_stride - 1) / m_stride));
    if (!m_draft.empty()) {
        m_draft.clear();
        m_pane_dirty = true;
    }
}

void DeepcwV2Channel::resync() {
    if (m_dead)
        return;
    m_sfill = 0;
    m_spos = 0;
    m_since = 0;
    if (m_flush_want) {
        flush_try();
        m_flush_want = false;
    }
    if (m_inflight && m_flush_inflight)
        m_resync_due = true;
    else
        restart();
    pane_compose();
}

bool DeepcwV2Channel::process(const float *iq, unsigned nframes, SkimDecode *out) {
    *out = SkimDecode{};
    if (m_dead)
        return false;
    if (m_inflight) {
        std::vector<float> logp;
        unsigned To = 0;
        uint64_t w0 = 0;
        bool got = false;
        {
            std::lock_guard<std::mutex> lk(m_box->lock);
            if (m_box->ready) {
                logp = std::move(m_box->logp);
                To = m_box->T;
                w0 = m_box->w0;
                m_box->ready = false;
                got = true;
            }
        }
        if (got) {
            m_inflight = false;
            apply(logp.data(), To, static_cast<int64_t>(w0), m_flush_inflight);
        }
    }
    flush_try();
    for (unsigned i = 0; i < nframes; i++) {
        const unsigned w = (m_spos + m_sfill) % m_N;
        m_sring[2 * w] = iq[2 * i];
        m_sring[2 * w + 1] = iq[2 * i + 1];
        if (m_sfill < m_N)
            m_sfill++;
        else
            m_spos = (m_spos + 1) % m_N;
        if (m_sfill == m_N && ++m_since >= m_hop) {
            m_since = 0;
            frame();
        }
    }
    if (m_out.empty()) {
        pane_compose();
        return false;
    }
    // v1's cadence: one WORD per call.
    size_t n = m_out.size();
    if (const size_t sp = m_out.find(' ', 1); sp != std::string::npos)
        n = sp + 1;
    n = std::min(n, static_cast<size_t>(SkimDecode::kTextMax) - 1);
    std::memcpy(out->text, m_out.data(), n);
    out->text[n] = '\0';
    m_out.erase(0, n);
    m_pane_dirty = true;
    pane_compose();
    out->confidence     = std::clamp(m_out_conf, 0.0, 1.0);
    out->freq_offset_hz = m_off_ema;
    out->speed          = m_wpm;
    out->snr_db         = m_snr_db;
    out->pane_own       = false;
    return true;
}

SkimDeepcw2Debug DeepcwV2Channel::debug() const {
    SkimDeepcw2Debug d;
    d.gate = m_gate;
    d.ratio_db = m_ratio_db;
    d.duty = m_duty;
    d.wpm = m_wpm;
    d.frames_abs = m_frames_abs;
    d.covered_end = m_stitcher.covered_end();
    d.frozen_end = m_stitcher.frozen_end();
    d.committed_end = m_stitcher.committed_end();
    d.ticks = m_ticks;
    d.dead = m_dead;
    d.inflight = m_inflight;
    d.pane_open = m_pane_open;
    d.draft_len = static_cast<unsigned>(m_draft.size());
    d.line_bin = m_line_bin;
    return d;
}

} // namespace

// ---- the backend ---------------------------------------------------------------

SkimDeepcw2Backend::SkimDeepcw2Backend(const DeepcwV2Config &cfg) : m_ctx(std::make_shared<DeepcwV2Context>(cfg)) {}

SkimDeepcw2Backend::~SkimDeepcw2Backend() = default;

const char *SkimDeepcw2Backend::name() const {
    if (m_ctx->cfg().model_v1)
        return "deepcw2v1";
    return m_ctx->cfg().narrow ? "deepcw2n" : "deepcw2";
}

std::unique_ptr<SkimDecodeChannel> SkimDeepcw2Backend::channel_new(double sample_rate) const {
    return std::make_unique<DeepcwV2Channel>(m_ctx, sample_rate);
}

bool SkimDeepcw2Backend::implements(SkimDecodeHook hook) const {
    return hook == SkimDecodeHook::Level || hook == SkimDecodeHook::ToneOffsetHz ||
           hook == SkimDecodeHook::SetFreq || hook == SkimDecodeHook::TakePaneOp ||
           hook == SkimDecodeHook::Resync || hook == SkimDecodeHook::HoldBegin ||
           hook == SkimDecodeHook::Noise || hook == SkimDecodeHook::SetBandNoise;
}

bool SkimDeepcw2Backend::available(std::string *error) const { return m_ctx->runtime().ensure_session(error); }

const std::string &SkimDeepcw2Backend::model_path() const { return m_ctx->cfg().model_path; }

std::string SkimDeepcw2Backend::runtime_info() const { return m_ctx->runtime().runtime_info(); }

void SkimDeepcw2Backend::set_device(const std::string &device) { m_ctx->runtime().set_device(device); }

void SkimDeepcw2Backend::reset() { m_ctx->runtime().reset(); }

std::optional<SkimDeepcw2Debug> SkimDeepcw2Backend::debug(const SkimDecodeChannel &channel) {
    if (const auto *c = dynamic_cast<const DeepcwV2Channel *>(&channel))
        return c->debug();
    return std::nullopt;
}
