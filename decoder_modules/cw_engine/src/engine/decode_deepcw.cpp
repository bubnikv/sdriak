/*
 * decode_deepcw.cpp — DeepCW neural CW backend (see decode_deepcw.h).
 *
 * Per channel: complex baseband (250 Hz for the CW bank) → 20-point Hann
 * DFT every 4 samples (80 ms window, 16 ms hop — the model was trained on
 * 80 ms / 15 ms; the 6.7 % time stretch sits well inside its speed range,
 * offline-verified 15–35 WPM) → the ±5 bins (±62.5 Hz, the channel) kept
 * per frame in a ring → on the channel's tick: gate (keyed line over the
 * floor) → 65-bin tile, channel bins at 27..37, peak-normalised, log1p →
 * one model run → greedy CTC → commit rule → text out through process().
 *
 * SDRIAK: upstream's decode_deepcw.c on the standard library. DcwState is
 * DeepcwChannel (upstream's dcw_* functions on it are its methods, `st->`
 * dropped, fields m_ + upstream's name); the process-wide session, worker
 * pool and tail dump are DeepcwContext, one per backend (the session and the
 * workers in deepcw_runtime.h).
 *
 * Part of skimmer-for-linux. GPL-3.0-or-later.
 */
#include "decode_deepcw.h"

#include <atomic>
#include <cinttypes>
#include <deque>

#include "deepcw_runtime.h"

namespace {

double env_d(const char *name, double dflt) {
    const char *v = std::getenv(name);
    return v && v[0] ? std::strtod(v, nullptr) : dflt;
}

} // namespace

DeepcwConfig DeepcwConfig::from_env() { return from_env(DeepcwConfig()); }

DeepcwConfig DeepcwConfig::from_env(const DeepcwConfig &base) {
    DeepcwConfig c = base;
    if (const char *v = std::getenv("SKIM_DEEPCW_MODEL"); v && v[0])
        c.model_path = v;
    if (const char *v = std::getenv("SKIM_ORT_LIB"); v && v[0])
        c.ort_library = v;
    if (const char *v = std::getenv("SKIM_DEEPCW_DEVICE"); v && v[0])
        c.device = v;
    c.verify      = env_d("SKIM_DEEPCW_VERIFY", c.verify ? 1 : 0) != 0;
    c.win_s       = env_d("SKIM_DEEPCW_WIN", c.win_s);
    c.tick_s      = env_d("SKIM_DEEPCW_TICK", c.tick_s);
    c.tail_s      = env_d("SKIM_DEEPCW_TAIL", c.tail_s);
    c.minconf_s   = env_d("SKIM_DEEPCW_MINCONF", c.minconf_s);
    c.gate_db     = env_d("SKIM_DEEPCW_GATE_DB", c.gate_db);
    c.threads     = static_cast<int>(env_d("SKIM_DEEPCW_THREADS", c.threads));
    c.space_p     = env_d("SKIM_DEEPCW_SPACE_P", c.space_p);
    c.sync        = env_d("SKIM_DEEPCW_SYNC", c.sync ? 1 : 0) != 0;
    c.workers     = std::clamp(static_cast<int>(env_d("SKIM_DEEPCW_WORKERS", c.workers)), 1, 8);
    c.batch       = std::clamp(static_cast<int>(env_d("SKIM_DEEPCW_BATCH", c.batch)), 1, 256);
    c.debug       = static_cast<int>(env_d("SKIM_DEEPCW_DEBUG", c.debug));
    c.draft       = env_d("SKIM_DEEPCW_DRAFT", c.draft ? 1 : 0) != 0;
    c.draft_min_s = env_d("SKIM_DEEPCW_DRAFT_MIN", c.draft_min_s);
    c.draft_p     = env_d("SKIM_DEEPCW_DRAFT_P", c.draft_p);
    c.flush       = env_d("SKIM_DEEPCW_FLUSH", c.flush ? 1 : 0) != 0;
    c.flush_pad_s = env_d("SKIM_DEEPCW_FLUSH_PAD", c.flush_pad_s);
    c.flush_quiet = env_d("SKIM_DEEPCW_FLUSH_QUIET", c.flush_quiet);
    c.flush_k     = env_d("SKIM_DEEPCW_FLUSH_K", c.flush_k);
    c.flush_p     = env_d("SKIM_DEEPCW_FLUSH_P", c.flush_p);
    {
        const char *only = std::getenv("SKIM_DEEPCW_ONLY");
        const char *dash = only ? std::strchr(only, '-') : nullptr;
        if (dash) {
            c.only_lo = std::strtod(only, nullptr) * 1000.0;
            c.only_hi = std::strtod(dash + 1, nullptr) * 1000.0;
        }
    }
    if (const char *gw = std::getenv("SKIM_DEEPCW_GAP_WORDS"); gw && gw[0]) {
        // g_strsplit(gw, " ", -1) upstream: every single space separates.
        c.gap_words.clear();
        std::string_view s(gw);
        for (size_t at = 0;;) {
            const size_t sp = s.find(' ', at);
            c.gap_words.emplace_back(s.substr(at, sp == std::string_view::npos ? sp : sp - at));
            if (sp == std::string_view::npos)
                break;
            at = sp + 1;
        }
    }
    if (const char *td = std::getenv("SKIM_DEEPCW_TAILDUMP"); td && td[0])
        c.taildump_path = td;
    return c;
}

// ---- the commit rule (pure) ----------------------------------------------

// Greedy CTC collapse of logp[T][42]: one Spike per emitted character at
// the frame where its run starts, p = posterior there.
std::vector<DeepcwCommitRule::Spike> DeepcwCommitRule::ctc_collapse(const float *logp, unsigned T,
                                                                    uint64_t w0) const {
    std::vector<Spike> a;
    int prev = -1;
    for (unsigned t = 0; t < T; t++) {
        const float *row = logp + static_cast<size_t>(t) * DeepcwModel::kClasses;
        int best = 0;
        for (int c = 1; c < DeepcwModel::kClasses; c++) {
            if (row[c] > row[best])
                best = c;
        }
        if (best == DeepcwModel::kBlank) {
            prev = -1;
            continue;
        }
        if (best != prev) {
            const Spike s = { static_cast<uint8_t>(best), w0 + t, std::exp(row[best]) };
            if (m_debug > 2 && best == DeepcwModel::kSpace)
                std::fprintf(stderr, "deepcw: space p=%.2f at %" PRIu64 "\n", s.p, s.t);
            a.push_back(s);
        }
        prev = best;
    }
    return a;
}

// A word gap the model is unsure about (posterior < space_p) is more often
// a torn fist than a gap — but only where it would tear a SHORT TAIL off
// a token: "OK2B TK" → OK2BTK, "OK1C Z" → OK1CZ (80 m fixture: both
// halves validated as calls and the panadapter would have shown OK2B),
// while "TEST TA5ARU" with a weak gap stays two words — dropping every
// weak gap fused weak stations' text and lost TA5ARU on 20 m. The left
// piece must be a token body (≥ 3 chars): a short LEFT piece is a word —
// DE, CQ, TU, K, R, 73 — and gluing it ate a third of the real word gaps
// on the 80 m fixture ("DEOK1FHI", "CQOL", 1488 gaps dropped per replay;
// Richard 2026-09-13: "nedělá mezery mezi slovy"). Such a space is
// neither a split point nor emitted.
void DeepcwCommitRule::drop_weak_tears(std::vector<Spike> &sp, double dump_hz) const {
    const double bar = m_space_p;
    if (bar <= 0.0)
        return;
    const char *chars = DeepcwModel::kChars;
    for (size_t i = 0; i < sp.size(); i++) {
        const Spike *s = &sp[i];
        if (s->cls != DeepcwModel::kSpace || s->p >= bar)
            continue;
        unsigned left = 0, right = 0;
        for (ptrdiff_t j = static_cast<ptrdiff_t>(i) - 1; j >= 0 && sp[j].cls != DeepcwModel::kSpace; j--)
            left++;
        for (size_t j = i + 1; j < sp.size() && sp[j].cls != DeepcwModel::kSpace; j++)
            right++;
        if (left >= 3 && right > 0 && right <= 2) {
            // the short right piece is a WORD (K, R, CQ, DE, TU, 73 …): the gap
            // before it is real — "OK1MDK CQ", "5NN K", "TEST DE" stay two words
            const char rt[3] = { chars[sp[i + 1].cls], right == 2 ? chars[sp[i + 2].cls] : '\0', '\0' };
            const bool word = std::any_of(m_gap_words.begin(), m_gap_words.end(),
                                          [&](const std::string &w) { return w == rt; });
            if (word)
                continue;
            if (m_dump) {
                char lt[4] = "", rt3[4] = "";
                for (unsigned k = 0; k < std::min(left, 3u); k++)
                    lt[k] = chars[sp[i - left + k].cls];
                for (unsigned k = 0; k < std::min(right, 3u); k++)
                    rt3[k] = chars[sp[i + 1 + k].cls];
                std::fprintf(m_dump, "X %.0f %" PRIu64 " %.3f %u %u %s %s\n",
                             dump_hz, s->t, s->p, left, right, lt, rt3);
            }
            sp.erase(sp.begin() + static_cast<ptrdiff_t>(i));
            i--;
        }
    }
}

unsigned DeepcwCommitRule::commit(const float *logp, unsigned T, uint64_t w0, uint64_t &cursor,
                                  unsigned tail, unsigned margin, bool &last_space, std::string &out,
                                  double *conf) const {
    uint64_t space_t = 0;
    return commit_ex(logp, T, w0, cursor, tail, margin, last_space, space_t, out, conf, nullptr, 0, 0.0);
}

unsigned DeepcwCommitRule::commit_ex(const float *logp, unsigned T, uint64_t w0, uint64_t &cursor,
                                     unsigned tail, unsigned margin, bool &last_space,
                                     uint64_t &space_t, std::string &out, double *conf,
                                     std::string *draft, unsigned draft_min, double draft_p,
                                     double dump_hz) const {
    const uint64_t w1 = w0 + T;
    const char *chars = DeepcwModel::kChars;
    std::vector<Spike> sp = ctc_collapse(logp, T, w0);
    drop_weak_tears(sp, dump_hz);
    unsigned n = 0;
    double psum = 0.0;
    // The space squeeze runs on through the tail: a draft that starts with
    // a gap right after a final gap shows no double space. It is written
    // back only when a character is FINAL — the draft never moves state.
    bool ls = last_space;
    // Sliding window: the whole ring is re-read every tick, so the model
    // always has its full left context (dropping committed audio from the
    // window tore words and lost stations on the 80 m fixture: 34 → 27).
    // A character is final once it sits ≥ tail before the window end (its
    // right context is in); the cursor is the frame of the last committed
    // spike and a re-placed copy of it lands within ±2 frames — a margin of
    // a few frames rejects it, while the next real character is a
    // character gap (≥ 9 frames at 25 WPM) further on. Spaces are placed
    // loosely by the model, so a space passes on frame order alone and
    // consecutive spaces are squeezed.
    for (const Spike &spike : sp) {
        const Spike *sk = &spike;
        const bool in_tail = w1 < tail || sk->t > w1 - tail;
        if (in_tail && !draft)
            break;
        // Same cursor/margin rule for the tail — a re-placed copy of the last
        // final character must not reappear as draft either.
        if (sk->cls == DeepcwModel::kSpace) {
            if (ls)
                continue;                               // double gap: squeezed
            if (sk->t <= cursor) {
                // The model places a gap loosely — often ON the frame of the
                // character before it, or a frame or two earlier — so a real gap
                // used to read as a re-placed copy of something committed and
                // vanished (80 m fixture: ~400 gaps per replay). A gap within
                // `slack` before the cursor passes when no committed gap sits
                // near it; a re-read of a committed gap sits within the same
                // slack of space_t and is squeezed. The cursor never moves back.
                const unsigned slack = 2 * margin;
                const bool old_read = sk->t + slack <= cursor;
                const bool committed_gap =
                    space_t && (sk->t + slack >= space_t) && (space_t + slack >= sk->t);
                if (old_read || committed_gap) {
                    if (m_dump && !in_tail) {
                        std::fprintf(m_dump, "S %.0f %" PRIu64 " %.3f %" PRIu64 "\n",
                                     dump_hz, sk->t, sk->p, cursor);
                    }
                    continue;
                }
            }
            ls = true;
        } else {
            if (sk->t <= cursor + margin)
                continue;
            ls = false;
        }
        if (in_tail) {
            // DRAFT: the model's current reading of the tail guard — display
            // only, re-read (and possibly changed) at the next tick. Only its
            // RELIABLE PREFIX shows: the reading is cut at the first spike that
            // sits closer than draft_min frames to the window end (no right
            // context yet) or below the draft_p posterior — measured on the 80 m
            // fixture, the raw tail was mostly wrong and flickered at 2 Hz
            // (Richard: "není nic čitelné").
            if (w1 - sk->t < draft_min || sk->p < draft_p)
                break;
            draft->push_back(chars[sk->cls]);
            if (m_dump) {
                std::fprintf(m_dump, "T %.0f %" PRIu64 " %" PRIu64 " %u %c %.3f\n",
                             dump_hz, w1, sk->t, static_cast<unsigned>(w1 - sk->t), chars[sk->cls], sk->p);
            }
            continue;
        }
        if (m_dump) {
            std::fprintf(m_dump, "F %.0f %" PRIu64 " %" PRIu64 " %c %.3f\n",
                         dump_hz, w1, sk->t, chars[sk->cls], sk->p);
        }
        out.push_back(chars[sk->cls]);
        psum += sk->p;
        n++;
        if (sk->cls == DeepcwModel::kSpace)
            space_t = sk->t;
        cursor = std::max(cursor, sk->t);
        last_space = ls;
    }
    if (conf)
        *conf = n ? psum / n : 0.0;
    return n;
}

// ---- runtime + model, workers: one per backend ---------------------------

// The backend's shared state: the tunables, the commit rule, the tail dump
// and the model runtime (deepcw_runtime.h: the session, its sha256 pin, the
// equal-length batching workers). SDRIAK: upstream keeps all of it as
// process-wide statics.
class DeepcwContext {
public:
    explicit DeepcwContext(const DeepcwConfig &cfg);
    DeepcwContext(const DeepcwContext &) = delete;
    DeepcwContext &operator=(const DeepcwContext &) = delete;

    const DeepcwConfig &cfg() const { return m_cfg; }
    const DeepcwCommitRule &rule() const { return m_rule; }
    FILE *dump() const { return m_dump.get(); }
    DeepcwRuntime &runtime() { return m_runtime; }

    // Engine thread only (one pipeline per backend).
    unsigned next_registration() { return m_reg++; }

    // true the first time only: one warning per backend for each of a
    // channel's conditions (upstream: function-local static flags).
    bool first_rate_warning() { return !m_rate_warned.exchange(true); }
    bool first_model_warning() { return !m_model_warned.exchange(true); }

private:
    DeepcwConfig          m_cfg;
    std::shared_ptr<FILE> m_dump;   // SKIM_DEEPCW_TAILDUMP
    DeepcwCommitRule      m_rule;
    DeepcwRuntime         m_runtime;
    unsigned              m_reg = 0;   // channel registration counter
    std::atomic<bool>     m_rate_warned{ false };
    std::atomic<bool>     m_model_warned{ false };
    // (OK1BR 4db0483, gh#27: no process-wide band floor any more; the band's
    // noise comes in through set_band_noise().)
};

namespace {

std::shared_ptr<FILE> open_dump(const std::string &path) {
    if (path.empty())
        return nullptr;
    FILE *f = std::fopen(path.c_str(), "w");
    if (!f)
        return nullptr;
    return std::shared_ptr<FILE>(f, [](FILE *p) { std::fclose(p); });
}

DeepcwRuntimeConfig runtime_config(const DeepcwConfig &c) {
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

DeepcwContext::DeepcwContext(const DeepcwConfig &cfg)
    : m_cfg(cfg), m_dump(open_dump(m_cfg.taildump_path)), m_rule(m_cfg, m_dump.get()),
      m_runtime(kDeepcwModelV1, runtime_config(m_cfg)) {}

// ---- per-channel state -----------------------------------------------------

namespace {

class DeepcwChannel final : public SkimDecodeChannel {
public:
    DeepcwChannel(std::shared_ptr<DeepcwContext> ctx, double rate);
    ~DeepcwChannel() override;

    bool process(const float *iq, unsigned nframes, SkimDecode *out) override;
    double level() const override { return m_lvl_ema; }
    double tone_offset_hz() const override { return m_off_ema; }
    void set_freq(double freq_hz) override { m_freq_hz = freq_hz; }
    bool take_pane_op(SkimPaneOp &op) override;
    void resync() override;
    void hold_begin() override;
    double noise() const override { return m_nmean; }           // OK1BR 4db0483 (gh#27)
    void set_band_noise(double noise, double enbw_hz) override; // OK1BR 4db0483 (gh#27)

    SkimDeepcwDebug debug() const;

private:
    static constexpr int   kKeep    = 11;       // channel bins −5..+5
    static constexpr int   kHalf    = 5;
    static constexpr float kPeakMag = 30.0f;    // peak → log1p ≈ 3.4 (training range)

    float *ring_row(uint64_t abs_frame) {
        return m_ring.data() + static_cast<size_t>(abs_frame % m_ring_frames) * kKeep;
    }
    void pane_queue(SkimPaneOpKind kind, std::string text, unsigned final_len);
    void pane_compose();
    void window_stats(uint64_t w0, uint64_t w1);
    double snr_500hz(uint64_t w0, uint64_t w1, int p, double mx);   // OK1BR 4db0483 (gh#27)
    void noise_accumulate(const float *mags);                        // OK1BR 4db0483 (gh#27)
    void dump_window(char kind, uint64_t w0, unsigned To) const;
    void apply(const float *logp, unsigned To, uint64_t w0);
    bool outside() const;
    void close_word();
    void flush_done(bool whole);
    bool flush_word_complete(const float *logp, unsigned To, uint64_t w0);
    void apply_flush(const float *logp, unsigned To, uint64_t w0);
    void tile(uint64_t w0, uint64_t w1, unsigned pad);
    void submit(unsigned T, uint64_t w0, bool flush);
    void tick();
    void flush_try();
    void frame();

    std::shared_ptr<DeepcwContext> m_ctx;
    const DeepcwConfig &m_cfg;          // the context's

    double   m_rate = 0.0;
    unsigned m_N = 0, m_hop = 0;
    std::vector<float> m_win;               // Hann N
    std::vector<float> m_tw_re, m_tw_im;    // [kKeep][N]
    std::vector<float> m_sring;             // complex sample ring, N frames
    unsigned m_spos = 0, m_sfill = 0, m_since = 0;
    std::vector<float> m_ring;              // [ring_frames][kKeep] magnitudes
    unsigned m_ring_frames = 0;
    uint64_t m_frames_abs = 0;
    uint64_t m_committed = 0;
    uint64_t m_space_t = 0;                 // frame of the last committed word gap
    unsigned m_tick_frames = 0, m_tick_phase = 0, m_tail_f = 0, m_minconf_f = 0,
             m_margin_f = 0, m_draft_min_f = 0;
    bool     m_last_space = true;           // the last committed spike was a space
                                            // (no gap before the first character)
    double   m_lvl_ema = 0.0, m_off_ema = 0.0;
    double   m_env_lo = 0.0;                // channel noise floor (inner bins, EMA)
    double   m_win_peak = 0.0;              // strongest line magnitude in the window
    double   m_snr_db = 0.0, m_wpm = 0.0;
    // OK1BR 4db0483 (gh#27): the band-noise statistic and the band's density.
    double   m_nacc = 0.0;                  // Σ over the running second of the
                                            // frame's total power in the 11 bins
    unsigned m_nacc_n = 0;
    double   m_nmean = 0.0;                 // that total, mean of the last whole
                                            // second (noise(); 0 = none)
    double   m_band_n0 = 0.0;               // the band's noise density in bin units,
                                            // N0·(Σw)² per Hz (set_band_noise); 0 = untold
    bool     m_gate = false;
    double   m_ratio_db = 0.0, m_duty = 0.0;
    unsigned m_ticks = 0;
    std::string m_out;
    double   m_out_conf = 0.0;
    double   m_freq_hz = 0.0;
    bool     m_dead = false;
    bool     m_nomodel = false;
    // Pane draft (display only, decode.h pane ops): the model's reading of
    // the tail guard shows dim in the pane right after the tick and firms
    // (or changes) ~tail_s later when it commits. The over region = the
    // committed words still waiting in m_out (plain) + m_draft (dim), so
    // the pipeline's one-word-per-call cadence never leaves a hole between
    // the appended text and the draft. Ops carry no `fresh` — the decode
    // log, extractor, station table and spot path never see a draft.
    std::string m_draft;                    // tail-guard reading from the last tick
    std::string m_pane_last;                // last region text sent (dedup)
    size_t   m_pane_last_fin = 0;
    bool     m_pane_open = false;           // an over region is open in the pane
    bool     m_pane_dirty = false;          // out/draft changed since the last compose
    std::deque<SkimPaneOp> m_ops;           // pending (take_pane_op)
    std::vector<float> m_tile;              // scratch [ring_frames][kBins]
    // async: one job in flight per channel; the result waits in the mailbox
    // for the engine thread, which runs the commit rule (deterministic order,
    // no locking around the text path).
    bool     m_inflight = false;
    std::shared_ptr<DeepcwMailbox> m_box = std::make_shared<DeepcwMailbox>();
    // TX-hold flush (decode.h hold_begin, gh#18): one more model run over the
    // window as it stands when the hold begins, dead air appended, and its
    // tail guard committed under the flush rule — see flush_try().
    unsigned m_flush_pad_f = 0, m_fade_f = 0;
    bool     m_flush_want = false;          // hold began; goes out once no job flies
    uint64_t m_flush_w1 = 0;                // window end when the hold began
    bool     m_flush_inflight = false;      // the job in flight IS the flush
    uint64_t m_flush_livef = 0;             // live end of the flush window (abs frame)
    unsigned m_flush_live = 0;              // frames of the flush window (no pad)
    bool     m_resync_due = false;          // resync came while the flush was out
    uint64_t m_resync_at = 0;
    uint64_t m_samples_abs = 0;             // channel samples seen (dump: frame ↔ time)
};

DeepcwChannel::DeepcwChannel(std::shared_ptr<DeepcwContext> ctx, double rate)
    : m_ctx(std::move(ctx)), m_cfg(m_ctx->cfg()) {
    m_rate = rate;
    const double nf = rate / DeepcwModel::kBinHz;
    const unsigned N = static_cast<unsigned>(std::llround(nf));
    if (std::fabs(nf - N) > 1e-6 || N < 8) {
        if (m_ctx->first_rate_warning()) {
            std::fprintf(stderr, "deepcw: channel rate %.1f Hz is no multiple of 12.5 Hz "
                                 "(the CW bank runs 250 Hz) — channel decodes nothing\n", rate);
        }
        m_dead = true;
        return;
    }
    // No runtime/model: the DSP half still runs (level, offset, gate — the
    // gate can prove it without a model); only inference is skipped. The
    // pipeline never picks this backend in that case (it falls back to v2),
    // so this is the test harness's path.
    std::string err;
    m_nomodel = !m_ctx->runtime().ensure_session(&err);
    if (m_nomodel && m_ctx->first_model_warning())
        std::fprintf(stderr, "deepcw: %s — channels track but decode nothing\n", err.c_str());
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
    const double fps = rate / m_hop;                             // 62.5 frames/s
    m_ring_frames = std::max(static_cast<unsigned>(std::llround(m_cfg.win_s * fps)), 16u);
    m_ring.assign(static_cast<size_t>(m_ring_frames) * kKeep, 0.0f);
    m_tile.assign(static_cast<size_t>(m_ring_frames) * DeepcwModel::kBins, 0.0f);
    // Tick: 0.5 s on a GPU session, 1.0 s on the CPU (the whole ring is
    // re-read every tick — 20 audio-seconds per channel-second at 0.5 s on
    // a 10 s ring); SKIM_DEEPCW_TICK overrides.
    double tick_s = m_cfg.tick_s;
    if (tick_s <= 0.0)
        tick_s = m_ctx->runtime().runtime_info().find("CUDA:") != std::string::npos ? 0.5 : 1.0;
    m_tick_frames = std::max(static_cast<unsigned>(std::llround(tick_s * fps)), 1u);
    m_margin_f    = 4;                                           // 64 ms at 62.5 fps
    m_tick_phase  = (m_ctx->next_registration() * 7u) % m_tick_frames;
    m_tail_f      = static_cast<unsigned>(std::llround(m_cfg.tail_s * fps));
    m_minconf_f   = static_cast<unsigned>(std::llround(m_cfg.minconf_s * fps));
    m_draft_min_f = static_cast<unsigned>(std::llround(m_cfg.draft_min_s * fps));
    m_flush_pad_f = std::min(static_cast<unsigned>(std::llround(m_cfg.flush_pad_s * fps)), m_ring_frames / 4);
    // A mute on the wire reaches the frames through the channelizer prototype
    // (8 taps per branch at 2× oversampling = 16 channel samples) and the DFT
    // window (N): that many frames fade before they read exact zero.
    m_fade_f      = (16 + N + m_hop - 1) / m_hop;
}

DeepcwChannel::~DeepcwChannel() {
    m_box->dying.store(true);       // a job in flight drops its result
}

// ---- pane draft (display only) ---------------------------------------------
void DeepcwChannel::pane_queue(SkimPaneOpKind kind, std::string text, unsigned final_len) {
    SkimPaneOp op;
    op.kind      = kind;
    op.erase     = 0;
    op.final_len = final_len;
    op.text      = std::move(text);
    m_ops.push_back(std::move(op));
}

// Compose the over region from the pending final words + the draft and
// queue ONE op when it differs from what the pane shows: OPEN when a region
// appears, SET on change, CLOSE("") when it empties. Called at the end of
// process() — after the one-word extraction — because the pipeline appends
// d.text BEFORE it drains the ops and the app inserts appended text ahead
// of an open region: per call the pane reads "word appended, region =
// remainder + draft". Composing earlier would double the word at the seam.
void DeepcwChannel::pane_compose() {
    if (!m_cfg.draft || !m_pane_dirty)
        return;
    m_pane_dirty = false;
    const size_t fin = m_out.size();
    std::string reg = m_out + m_draft;
    if (reg.empty()) {
        if (m_pane_open) {
            pane_queue(SkimPaneOpKind::Close, std::string(), 0);
            m_pane_open = false;
            m_pane_last.clear();
            m_pane_last_fin = 0;
        }
        return;
    }
    if (m_pane_open && fin == m_pane_last_fin && reg == m_pane_last)
        return;
    pane_queue(m_pane_open ? SkimPaneOpKind::Set : SkimPaneOpKind::Open, reg, static_cast<unsigned>(fin));
    m_pane_open = true;
    m_pane_last = std::move(reg);
    m_pane_last_fin = fin;
}

// Gate + WPM + SNR over abs frames [w0, w1).
void DeepcwChannel::window_stats(uint64_t w0, uint64_t w1) {
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
    // keyed duty on the line bin: threshold halfway between its max and the
    // floor; on/off run lengths give a dit estimate (shortest cluster).
    double mx = 0;
    for (uint64_t f = w0; f < w1; f++)
        mx = std::max(mx, static_cast<double>(ring_row(f)[p]));
    const double thr = 0.5 * (mx + floorv);
    m_win_peak = mx;
    unsigned on = 0, run = 0, runs[64], nr = 0;
    for (uint64_t f = w0; f < w1; f++) {
        const bool k = ring_row(f)[p] > thr;
        if (k) {
            on++;
            run++;
        } else if (run) {
            if (nr < 64)
                runs[nr++] = run;
            run = 0;
        }
    }
    m_duty = static_cast<double>(on) / T;
    // WPM only from a window that is really keyed (a quiet tail with noise
    // runs reads as 50+ WPM); otherwise the last estimate stands.
    const bool keyed_enough = m_duty >= 0.15 && m_duty <= 0.85 && nr >= 5;
    if (m_cfg.debug > 1) {
        std::fprintf(stderr, "deepcw: stats T=%u p=%d line %.4f floor %.4f mx %.4f thr %.4f runs %u:",
                     T, p, line, floorv, mx, thr, nr);
        for (unsigned i = 0; i < std::min(nr, 12u); i++)
            std::fprintf(stderr, " %u", runs[i]);
        std::fprintf(stderr, "\n");
    }
    if (keyed_enough) {
        // Dit = the lower mode of the on-run histogram, 1-frame glitches
        // excluded (they read as 50+ WPM); refine with its ±1 neighbours.
        unsigned hist[16] = {};
        for (unsigned i = 0; i < nr; i++) {
            if (runs[i] >= 2 && runs[i] < 16)
                hist[runs[i]]++;
        }
        // Dah-heavy fists (OK1XC: O K X C) put the histogram's peak on the
        // DAH (7–9 frames at 25–30 WPM) and read 10 WPM — the dit is the
        // FIRST run length that is well populated, not the most populated.
        unsigned mode = 0;
        for (unsigned L = 2; L < 9; L++) {
            if (hist[L] > hist[mode])
                mode = L;
        }
        unsigned dit = 0;
        for (unsigned L = 2; L < 9; L++) {
            if (hist[L] >= 3 && hist[L] * 5 >= hist[mode] * 2) {
                dit = L;
                break;
            }
        }
        if (dit) {
            double s = 0;
            unsigned c = 0;
            for (unsigned L = dit - 1; L <= dit + 1 && L < 16; L++) {
                s += static_cast<double>(L) * hist[L];
                c += hist[L];
            }
            const double dit_s = (s / c) * m_hop / m_rate;
            const double wpm = 1.2 / dit_s;
            if (wpm <= 60.0 && wpm >= 5.0)
                m_wpm = wpm;
            if (m_cfg.debug > 1)
                std::fprintf(stderr, "deepcw: dit %u (mode %u) frames, refined %.2f → %.1f wpm\n", dit, mode, s / c, m_wpm);
        }
    }
    m_gate = m_ratio_db >= m_cfg.gate_db && m_duty >= 0.03 && m_duty <= 0.97;
    m_snr_db = snr_500hz(w0, w1, p, mx);
}

// ---- BEGIN port: OK1BR/skimmer-for-linux 4db0483 (2026-11-07, gh#27) --------
// "Spots: the dB is SNR in 500 Hz, measured against stations of known SNR",
// decode_deepcw.c window_stats(). Not in OK1HRA's fork (it branched from
// OK1BR on 2026-09-20). Replaces upstream's "window peak over the inner-bin
// floor minus 10 dB", which read ~8 dB high, and the process-global band
// floor EMA behind it.
//
// SNR in 500 Hz (decode.h snr_db) — CW Skimmer's own recipe (VE3NEA via
// N4ZR, RBN blog 2014-03-05): the key-on samples give the signal, the noise
// density comes from the flat spectrum of the whole band, times 500 Hz.
// Here: the key-down power of the line — the frames within 3 dB of the
// window's peak (a dit at 25 WPM is shorter than the 80 ms DFT window, so
// only the solid frames inside dahs and long dits show the carrier whole) —
// summed over the line bin and its two neighbours: for a Hann window that
// sum is 1.5× the tone's bin-centre power wherever the tone sits between
// bins (1.47–1.50), so no scalloping term is needed. The noise power per bin
// comes from the band's noise density (the pipeline's median over the bank,
// set_band_noise) times a Hann bin's 1.5 bins of noise bandwidth, and is
// subtracted bin for bin; that same 18.75 Hz refers the ratio to 500 Hz.
// Measured in OK1BR's skimmer-snr-test against stations of known SNR:
// within 0.7 dB from 3 to 30 dB.
double DeepcwChannel::snr_500hz(uint64_t w0, uint64_t w1, int p, double mx) {
    // Noise power per bin: the band's density × a Hann bin's 1.5 bins of
    // noise bandwidth; before the pipeline has told it, 4/π × the squared
    // Rayleigh mean of this channel's own inner-bin floor.
    double nb;
    if (m_band_n0 > 0.0) {
        nb = m_band_n0 * (1.5 * DeepcwModel::kBinHz);
    } else {
        const double flm = std::max(m_env_lo, 1e-12);
        nb = (4.0 / SKIM_PI) * flm * flm;
    }
    const int b0 = p > 0 ? p - 1 : p;
    const int b1 = p + 1 < kKeep ? p + 1 : p;
    const double solid = 0.7 * mx;                       // within 3 dB
    double p3 = 0.0;
    unsigned non = 0;
    for (uint64_t f = w0; f < w1; f++) {
        const float *r = ring_row(f);
        if (r[p] <= solid)
            continue;
        for (int b = b0; b <= b1; b++)
            p3 += static_cast<double>(r[b]) * r[b];
        non++;
    }
    if (!non)
        return m_snr_db;                                 // upstream: keeps the last value
    const unsigned nbins = static_cast<unsigned>(b1 - b0 + 1);
    const double hsum = nbins == 3 ? 1.5 : 1.25;         // Hann 3-bin / 2-bin
    const double sig = (p3 / non - nbins * nb) / hsum;
    return 10.0 * std::log10(std::max(sig, 1e-3 * nb) / nb) +
           10.0 * std::log10(1.5 * DeepcwModel::kBinHz / 500.0);
}

// The band-noise statistic (decode.h noise()): the frame's total power in
// the 11 bins, meaned over the running second. Hann bins at their own
// spacing cover the band 1.5× over, so on a quiet channel the mean is
// 1.5·N0·(Σw)²·ENBW — the pipeline's median over the bank comes back
// through set_band_noise as the band's noise density.
void DeepcwChannel::noise_accumulate(const float *mags) {
    double tot = 0.0;
    for (int b = 0; b < kKeep; b++)
        tot += static_cast<double>(mags[b]) * mags[b];
    m_nacc += tot;
    if (++m_nacc_n >= std::max(static_cast<unsigned>(m_rate / m_hop), 1u)) {
        m_nmean  = m_nacc / m_nacc_n;
        m_nacc   = 0.0;
        m_nacc_n = 0;
    }
}

void DeepcwChannel::set_band_noise(double noise, double enbw_hz) {
    if (noise > 0 && enbw_hz > 0)
        m_band_n0 = noise / (1.5 * enbw_hz);
}
// ---- END port: OK1BR 4db0483 (gh#27) -----------------------------------------

// Tail dump: one line per applied window, so a frame index maps to channel
// samples (frame F ended at sample s_last − (frames_abs − 1 − F)·hop; holds
// swallow audio, so frame numbers alone do not give stream time).
void DeepcwChannel::dump_window(char kind, uint64_t w0, unsigned To) const {
    FILE *d = m_ctx->dump();
    if (!d)
        return;
    std::fprintf(d, "%c %.0f %" PRIu64 " %u %" PRIu64 " %" PRIu64 "\n", kind, m_freq_hz, w0, To,
                 m_frames_abs, m_samples_abs - m_since);
}

// Commit a finished inference (engine thread only): the rule, then the
// confidence and WPM bookkeeping of the newly pending text.
void DeepcwChannel::apply(const float *logp, unsigned To, uint64_t w0) {
    double conf = 0;
    const size_t before = m_out.size();
    const uint64_t cursor0 = std::max(m_committed, w0);
    m_draft.clear();
    dump_window('W', w0, To);
    const unsigned n = m_ctx->rule().commit_ex(logp, To, w0, m_committed, m_tail_f, m_margin_f,
                                               m_last_space, m_space_t, m_out, &conf,
                                               m_cfg.draft ? &m_draft : nullptr,
                                               m_draft_min_f, m_cfg.draft_p, m_freq_hz);
    m_pane_dirty = true;             // out and/or the draft moved
    if (n) {
        // Confidence of the pending emission: length-weighted merge.
        const size_t tot = m_out.size();
        m_out_conf = tot ? (m_out_conf * before + conf * n) / tot : conf;
        // WPM from the character rate of the committed span (PARIS: 10 dit
        // units per character incl. its gap → WPM ≈ 12 × chars/s) — the
        // on-run histogram read dah-heavy fists (OK1XC) at a third of their
        // speed; the model's own segmentation is fist-shape independent.
        const uint64_t span = m_committed > cursor0 ? m_committed - cursor0 : 0;
        unsigned letters = 0;
        for (size_t i = before; i < tot; i++) {
            if (m_out[i] != ' ')
                letters++;
        }
        if (letters >= 6 && span >= 2 * m_rate / m_hop) {
            const double wpm = 12.0 * letters / (static_cast<double>(span) * m_hop / m_rate);
            if (wpm >= 5.0 && wpm <= 60.0)
                m_wpm = wpm;
        }
    }
    if (m_cfg.debug) {
        std::fprintf(stderr, "deepcw: %.2f kHz T=%u ratio %.1f dB duty %.2f wpm %.0f "
                             "commit %u |%s| draft |%s| cursor %" PRIu64 "\n",
                     m_freq_hz / 1000.0, To, m_ratio_db, m_duty, m_wpm, n,
                     m_out.c_str() + before, m_draft.c_str(), m_committed);
    }
}

bool DeepcwChannel::outside() const {
    return m_cfg.only_hi > 0.0 && (m_freq_hz < m_cfg.only_lo || m_freq_hz > m_cfg.only_hi);
}

// A TX hold is an over boundary for us whatever the band did meanwhile: the
// text after it starts a new word. The model emits no gap there (it never
// saw the pause), so "TU OH2BBM" + the answer "OK1BR 5NN" read OH2BBMOK1BR
// (live 2026-09-19). Only text still inside the window counts — a channel
// that fell silent long ago would just log a lone gap at every hold.
void DeepcwChannel::close_word() {
    if (!m_last_space && m_committed + m_ring_frames > m_frames_abs) {
        m_out.push_back(' ');
        m_pane_dirty = true;
    }
    m_last_space = true;
}

// The flush is over (applied, or there was nothing to run it on). The word
// closes only when it is WHOLE: the head of a word the hold cut, closed,
// would be a token of its own (OH2X); left open it fuses with whatever
// fragment the channel resumes on and validates as nothing — as it always
// did before gh#18.
void DeepcwChannel::flush_done(bool whole) {
    m_flush_inflight = false;
    if (!m_draft.empty()) {
        m_draft.clear();
        m_pane_dirty = true;
    }
    if (whole)
        close_word();
    m_last_space = true;
    if (m_resync_due) {              // the hold ended while the flush flew
        m_resync_due = false;
        m_committed = std::max(m_committed, m_resync_at);
        m_space_t = 0;
    }
}

// Noise beside the line in ONE frame: the inner bins (inside the channelizer
// passband) at least 2 off the line — frame()'s floor estimator.
float frame_floor(const float *r, int p, int half) {
    float sum = 0;
    unsigned n = 0;
    for (int b = half - 3; b <= half + 3; b++) {
        if (b - p >= 2 || p - b >= 2) {
            sum += r[b];
            n++;
        }
    }
    return n ? sum / n : r[p];
}

// The quietest inner bin of a frame: noise whatever a CW line does beside
// it, and lifted with every other bin by a broadband click.
float frame_quietest(const float *r, int half) {
    float m = r[half - 3];
    for (int b = half - 2; b <= half + 3; b++)
        m = std::min(m, r[b]);
    return m;
}

// Is the last word the flush read WHOLE? Every other channel is mid-over
// when the operator keys, and the head of a call cut there validates as a
// call of its own (OH2X where OH2XX was being sent). The word is whole when
// ITS line — the strongest bin over the word's own frames, not the window's
// (a louder station earlier in the window hid a weak caller's keying) — has
// been silent for flush_quiet dits at the live end: more than a character
// gap, so nothing of the word is still to come.
bool DeepcwChannel::flush_word_complete(const float *logp, unsigned To, uint64_t w0) {
    const uint64_t live = m_flush_livef;
    const double fps = m_rate / m_hop;
    const std::vector<DeepcwCommitRule::Spike> sp = m_ctx->rule().ctc_collapse(logp, To, w0);
    ptrdiff_t last = -1;
    for (size_t i = 0; i < sp.size(); i++) {
        const auto *k = &sp[i];
        if (k->cls != DeepcwModel::kSpace && k->t > m_committed + m_margin_f && k->t <= live)
            last = static_cast<ptrdiff_t>(i);
    }
    // The span the line is looked for in: the last pending word (its first
    // spike, half a second of lead — a spike sits at its character's END), or
    // with nothing pending the last second: a long character still being keyed
    // has no spike yet, and the word before it is not whole either.
    uint64_t t0 = live > static_cast<uint64_t>(fps) ? live - static_cast<uint64_t>(fps / 2) : w0;
    bool weak = false;
    if (last >= 0) {
        ptrdiff_t first = last;
        while (first > 0 && sp[first - 1].cls != DeepcwModel::kSpace &&
               sp[first].t - sp[first - 1].t < static_cast<uint64_t>(fps)) {
            first--;
        }
        t0 = sp[first].t;
        // The model must be sure of what it read without right context: one
        // spike of the last word under flush_p inside the tail guard and the
        // word stays out whole (SAC fixture, 37 synthetic overs: 11 of 16 wrong
        // last words fall under 0.5, against 5 of 77 right ones).
        for (ptrdiff_t i = first; i <= last; i++) {
            const auto *k = &sp[i];
            if (k->t + m_tail_f > live && k->p < m_cfg.flush_p)
                weak = true;
        }
    }
    const uint64_t ta = std::max(w0, t0 > static_cast<uint64_t>(fps / 2) ? t0 - static_cast<uint64_t>(fps / 2) : uint64_t(0));
    if (live <= ta)
        return false;
    double prof[kKeep] = {};
    for (uint64_t f = ta; f < live; f++) {
        const float *r = ring_row(f);
        for (int b = 0; b < kKeep; b++)
            prof[b] += r[b];
    }
    int p = 1;
    for (int b = 1; b < kKeep - 1; b++) {
        if (prof[b] > prof[p])
            p = b;
    }
    // Keyed = the line (±1 bin) over flush_k × the NOISE beside it: the lower
    // quartile of the per-frame floor over the word — the frames between the
    // marks; during a mark the keying sidebands lift it. A bar relative to the
    // word's peak called a station in a QSB dip silent mid-character (KC1XX
    // fading 12 dB inside one call: "KC1" went out as a whole word).
    float pk = 0.0f;
    const unsigned nfl = static_cast<unsigned>(live - ta);
    std::vector<float> fls(nfl);
    for (uint64_t f = ta; f < live; f++) {
        const float *r = ring_row(f);
        pk = std::max(pk, r[p]);
        fls[f - ta] = frame_floor(r, p, kHalf);
    }
    std::sort(fls.begin(), fls.end());
    const double fl = fls[nfl / 4];
    const double thr = m_cfg.flush_k * fl;
    unsigned quiet = 0;
    for (uint64_t f = live; f > ta; f--) {
        const float *r = ring_row(f - 1);
        if (std::max(r[p], std::max(r[p - 1], r[p + 1])) > thr)
            break;
        quiet++;
    }
    const double wpm = m_wpm > 0 ? std::clamp(m_wpm, 15.0, 45.0) : 25.0;
    const unsigned need = static_cast<unsigned>(std::ceil(m_cfg.flush_quiet * 1.2 / wpm * fps));
    if (FILE *d = m_ctx->dump()) {
        std::fprintf(d, "Q %.0f %" PRIu64 " %" PRIu64 " %d %u %u %.0f %d\n", m_freq_hz, m_flush_w1,
                     live, p, quiet, need, m_wpm, static_cast<int>(weak));
        if (m_cfg.debug > 1) {            // the line against thr, oldest first
            std::fprintf(d, "V %.0f ta %" PRIu64 " thr %.5f pk %.5f fl %.5f:", m_freq_hz, ta, thr, pk, fl);
            for (uint64_t f = live > ta + 80 ? live - 80 : ta; f < m_flush_w1; f++) {
                const float *r = ring_row(f);
                std::fprintf(d, " %.0f", 100.0 * std::max(r[p], std::max(r[p - 1], r[p + 1])) / thr);
            }
            std::fprintf(d, "\n");
        }
    }
    return quiet >= need && !weak;
}

// The flush result (engine thread): commit through the live end of the
// window — the pad is no audio — and no draft. A last word the hold cut
// short stays out: its head may validate as a call of its own.
void DeepcwChannel::apply_flush(const float *logp, unsigned To, uint64_t w0) {
    double conf = 0;
    const size_t before = m_out.size();
    const bool ls0 = m_last_space;
    const unsigned dead = To > m_flush_live ? To - m_flush_live : 0;   // the pad
    dump_window('H', w0, To);
    const bool complete = flush_word_complete(logp, To, w0);
    unsigned n = m_ctx->rule().commit_ex(logp, To, w0, m_committed, dead, m_margin_f,
                                         m_last_space, m_space_t, m_out, &conf, nullptr, 0, 0.0,
                                         m_freq_hz);
    unsigned dropped = 0;
    if (n && !complete) {
        size_t cut = before;
        for (size_t i = m_out.size(); i > before; i--) {
            if (m_out[i - 1] == ' ') {
                cut = i;
                break;
            }
        }
        dropped = static_cast<unsigned>(m_out.size() - cut);
        m_out.resize(cut);
        m_last_space = cut > before ? true : ls0;
        n -= dropped;
    }
    if (n) {
        const size_t tot = m_out.size();
        m_out_conf = tot ? (m_out_conf * before + conf * n) / tot : conf;
        m_pane_dirty = true;
    }
    if (FILE *d = m_ctx->dump())
        std::fprintf(d, "E %.0f %u %u |%s|\n", m_freq_hz, static_cast<unsigned>(complete), dropped, m_out.c_str() + before);
    if (m_cfg.debug) {
        std::fprintf(stderr, "deepcw: %.2f kHz T=%u FLUSH live %u %s commit %u dropped %u |%s|\n",
                     m_freq_hz / 1000.0, To, m_flush_live, complete ? "complete" : "cut", n, dropped,
                     m_out.c_str() + before);
    }
    flush_done(complete);
}

// Tile of abs frames [w0, w1) + `pad` frames of dead air: channel bins at
// 27..37, peak-normalised, log1p; rest zero.
void DeepcwChannel::tile(uint64_t w0, uint64_t w1, unsigned pad) {
    const unsigned T = static_cast<unsigned>(w1 - w0);
    float mx = 0.0f;
    for (uint64_t f = w0; f < w1; f++) {
        const float *r = ring_row(f);
        for (int b = 0; b < kKeep; b++)
            mx = std::max(mx, r[b]);
    }
    const float g = mx > 0 ? kPeakMag / mx : 1.0f;
    std::fill_n(m_tile.begin(), static_cast<size_t>(T + pad) * DeepcwModel::kBins, 0.0f);
    for (unsigned t = 0; t < T; t++) {
        const float *r = ring_row(w0 + t);
        float *row = m_tile.data() + static_cast<size_t>(t) * DeepcwModel::kBins + (DeepcwModel::kCentre - kHalf);
        for (int b = 0; b < kKeep; b++)
            row[b] = std::log1p(r[b] * g);
    }
}

// m_tile[T] through the model: inline (replays) or to a worker.
void DeepcwChannel::submit(unsigned T, uint64_t w0, bool flush) {
    if (m_cfg.sync) {
        std::vector<float> logp;
        unsigned To = 0;
        m_ctx->runtime().infer(m_tile.data(), T, logp, To);
        if (To) {
            if (flush)
                apply_flush(logp.data(), To, w0);
            else
                apply(logp.data(), To, w0);
        } else if (flush) {
            flush_done(true);
        }
        return;
    }
    // Async: hand the window to a worker; the mailbox comes back through
    // process() on the engine thread. One job per channel in flight; a
    // queue that keeps growing means the workers cannot keep up — skip the
    // tick (the channel retries next tick) and say so once. A flush is never
    // retried, so it always goes in.
    auto job = std::make_unique<DeepcwJob>();
    job->box = m_box;
    job->T   = T;
    job->w0  = w0;
    job->tile.assign(m_tile.begin(), m_tile.begin() + static_cast<ptrdiff_t>(static_cast<size_t>(T) * DeepcwModel::kBins));
    if (!m_ctx->runtime().submit(std::move(job), flush))
        return;
    m_flush_inflight = flush;
    m_inflight = true;
}

void DeepcwChannel::tick() {
    if (m_inflight)
        return;                      // async: one window at a time
    if (outside())
        return;
    const uint64_t w1 = m_frames_abs;
    const uint64_t w0 = w1 > m_ring_frames ? w1 - m_ring_frames : 0;
    if (w1 <= w0)
        return;
    const unsigned T = static_cast<unsigned>(w1 - w0);
    if (T < m_minconf_f)
        return;
    window_stats(w0, w1);
    m_ticks++;
    if (!m_gate || m_nomodel) {
        if (m_cfg.debug) {
            std::fprintf(stderr, "deepcw: %.2f kHz T=%u closed ratio %.1f dB duty %.2f\n",
                         m_freq_hz / 1000.0, T, m_ratio_db, m_duty);
        }
        // the over ended (or never was): no draft may linger in the pane
        if (!m_draft.empty()) {
            m_draft.clear();
            m_pane_dirty = true;
        }
        return;
    }
    tile(w0, w1, 0);
    submit(T, w0, false);
}

// TX-hold flush (decode.h hold_begin, gh#18). The tail guard exists because
// the model reads the last second of a window badly: the audio there is
// usually cut mid-character and has no right context. When the operator
// starts to transmit, that context never comes — the pipeline swallows the
// band for seconds and resync then abandons the tail, which in search and
// pounce is the call of the very station being answered. So the window is
// read once more as it stands, with dead air appended (true: the band is
// dead for us from here on), and the tail is committed.
//
// What the flush may NOT do is mint a truncated call: every other channel
// is mid-over when the operator keys, and "OH2X" validates where OH2XX was
// being sent. The last word goes out only when the line has been silent for
// flush_quiet dits at the LIVE end of the window (more than a character
// gap: the word is whole); otherwise the flush stops at the last word gap.
// The live end is not the window end: sdr-for-linux mutes the wire at
// key-down and reports trx 0.04–0.43 s later (gh#17), so the window ends in
// exact-zero frames — silence that says nothing about the station —
// preceded by the mute's own transient: a band cut to zero from one sample
// to the next is a CLICK in every channel, fade_f frames wide (channelizer
// prototype + DFT window), and the model reads it as a dit with full
// confidence ("E", p 1.00, on channel after channel — SAC fixture). The
// model gets the window up to the live end and nothing behind it.
void DeepcwChannel::flush_try() {
    if (!m_flush_want || m_inflight)
        return;
    m_flush_want = false;
    if (m_nomodel || outside()) {
        flush_done(true);
        return;
    }
    const uint64_t wend = m_flush_w1;
    const uint64_t wmin = wend > m_ring_frames ? wend - m_ring_frames : 0;
    if (wend - wmin < m_minconf_f) {
        flush_done(true);
        return;
    }

    // The live end. Normal = a frame whose noise (the quietest inner bin — a
    // CW line is narrow, the mute's click is not) sits within ×/÷ 3 of the
    // reference below; everything abnormal at the very end is the mute (or a
    // strong station still keying, whose last word is cut either way).
    const double fps = m_rate / m_hop;
    const unsigned look = std::min(static_cast<unsigned>(wend - wmin), static_cast<unsigned>(std::llround(2.0 * fps)));
    std::vector<float> q(look);
    for (unsigned i = 0; i < look; i++)
        q[i] = frame_quietest(ring_row(wend - look + i), kHalf);
    // reference = the lower quartile of the frames that carry anything at
    // all: the gaps between marks (a strong station's own keying lifts even
    // its quietest bin), the muted frames left out
    std::vector<float> qs;
    qs.reserve(look);
    for (unsigned i = 0; i < look; i++) {
        if (q[i] > 0.0f)
            qs.push_back(q[i]);
    }
    if (qs.size() < 16) {
        flush_done(true);
        return;
    }
    std::sort(qs.begin(), qs.end());
    const double ref = qs[qs.size() / 4];
    uint64_t live = wend;
    const uint64_t lo = wend - std::min(look, static_cast<unsigned>(std::llround(0.7 * fps)) + m_fade_f);
    while (live > lo) {
        const double v = q[look - static_cast<unsigned>(wend - live) - 1];
        if (v >= ref / 3.0 && v <= ref * 3.0)
            break;
        live--;
    }
    if (live < wend)
        live = live > wmin + 2 ? live - 2 : wmin;   // its first frames look normal

    const unsigned pad = m_flush_pad_f;
    const uint64_t w0 = (live - wmin) + pad > m_ring_frames ? live + pad - m_ring_frames : wmin;
    if (live <= w0 || static_cast<unsigned>(live - w0) < m_minconf_f) {
        flush_done(true);
        return;
    }
    window_stats(w0, live);
    if (!m_gate) {
        flush_done(true);
        return;
    }
    m_flush_livef = live;
    m_flush_live = static_cast<unsigned>(live - w0);
    tile(w0, live, pad);
    submit(static_cast<unsigned>(live - w0) + pad, w0, true);
}

void DeepcwChannel::frame() {
    // DFT of the last N samples (oldest first) × Hann → bins −5..+5.
    float *row = ring_row(m_frames_abs);
    float mags[kKeep];
    for (int b = 0; b < kKeep; b++) {
        const float *tr = m_tw_re.data() + static_cast<size_t>(b) * m_N;
        const float *ti = m_tw_im.data() + static_cast<size_t>(b) * m_N;
        float re = 0, im = 0;
        unsigned idx = m_spos;                         // oldest sample
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
    if (m_cfg.debug > 1 && m_frames_abs % 50 == 30) {
        std::fprintf(stderr, "deepcw: frame %" PRIu64 " mags", m_frames_abs);
        for (int b = 0; b < kKeep; b++)
            std::fprintf(stderr, " %.4f", mags[b]);
        std::fprintf(stderr, "\n");
    }
    // Level (raw units, EMA on the peak bin) and tone offset (centroid on
    // mark frames only — v2's rule: a noisy phase drags the pin).
    int p = 0;
    for (int b = 1; b < kKeep; b++) {
        if (mags[b] > mags[p])
            p = b;
    }
    // Noise floor of this frame = the INNER bins (|b| ≤ 3 = ±37.5 Hz,
    // inside the channelizer passband) at least 2 bins off the line. The
    // outer bins sit on the filter roll-off and read tens of dB low, which
    // inflated every SNR and let a −12 dB spur outrank its station.
    const float fl = frame_floor(mags, p, kHalf);
    // Level = peak hold with a ~2 s decay (a word gap must not drop it to
    // the noise: the ghost arbitration compares channels at every block).
    // Offset updates only on frames near that peak — noise frames between
    // marks would drag the estimate to the centre (gate-caught: 30 → 14 Hz).
    const double decay = std::exp(-static_cast<double>(m_hop) / (2.0 * m_rate));
    m_lvl_ema = std::max(static_cast<double>(mags[p]), m_lvl_ema * decay);
    // Noise floor: what the line bin shows between marks — falls at once,
    // rises slowly (v2's env_lo idea). SNR = peak hold over it, so a 35 dB
    // station and its −20 dB image no longer read the same (the tile's
    // line/floor ratio saturates on the signal's own keying sidebands).
    // The line bin's own minima are useless for a strong fast station: an
    // 80 ms window never reaches the noise inside a 48 ms element gap, so a
    // 37 dB station read 29 dB and its −12 dB spur 35 dB (80 m fixture) —
    // the table then parked the station on the spur. The floor is the
    // quietest the OTHER bins have been lately instead: fast down, ~8 s up.
    if (m_env_lo <= 0.0)
        m_env_lo = fl;
    else
        m_env_lo += 0.01 * (fl - m_env_lo);
    noise_accumulate(mags);          // OK1BR 4db0483 (gh#27), in place of the band floor EMA
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

bool DeepcwChannel::process(const float *iq, unsigned nframes, SkimDecode *out) {
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
            if (m_flush_inflight) {
                if (To)
                    apply_flush(logp.data(), To, w0);
                else
                    flush_done(true);
            } else if (To) {
                apply(logp.data(), To, w0);
            }
        }
    }
    flush_try();                     // a hold began while a tick was out
    for (unsigned i = 0; i < nframes; i++) {
        m_samples_abs++;
        // write position = oldest slot once the ring is full
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
    // One WORD per call (the drain runs every ~256 ms per channel): the
    // extractor and the station table then see the same cadence of hits
    // they were tuned on with v2's per-character stream, instead of one
    // whole-over hit per commit.
    size_t n = m_out.size();
    {
        const size_t sp = m_out.find(' ');
        if (sp != std::string::npos)
            n = sp + 1;
    }
    n = std::min(n, static_cast<size_t>(SkimDecode::kTextMax) - 1);
    std::memcpy(out->text, m_out.data(), n);
    out->text[n] = '\0';
    m_out.erase(0, n);
    m_pane_dirty = true;             // the word leaves the region
    pane_compose();
    out->confidence     = std::clamp(m_out_conf, 0.0, 1.0);
    out->freq_offset_hz = m_off_ema;
    out->speed          = m_wpm;
    out->snr_db         = m_snr_db;
    out->pane_own       = false;
    return true;
}

// TX hold begins: read the tail to its end now (flush_try).
void DeepcwChannel::hold_begin() {
    if (m_dead || !m_cfg.flush)
        return;
    m_flush_want = true;
    m_flush_w1 = m_frames_abs;
    flush_try();
}

// TX hold ends: the channel missed audio — start the window afresh, keep
// the level/offset tracking and any text already committed. Nothing before
// this frame commits any more (the audio after the gap is no right context
// for the audio before it); a flush still in flight takes the cursor jump
// with it (flush_done).
void DeepcwChannel::resync() {
    if (m_dead)
        return;
    m_sfill = 0;
    m_spos = 0;
    m_since = 0;
    if (m_flush_want) {              // a tick was out through the whole hold
        flush_try();
        m_flush_want = false;
    }
    if (m_inflight && m_flush_inflight) {
        m_resync_due = true;
        m_resync_at = m_frames_abs;
    } else {
        if (m_cfg.flush)
            close_word();            // before the cursor moves
        m_committed = m_frames_abs;
        m_last_space = true;
        m_space_t = 0;
        if (!m_draft.empty()) {
            m_draft.clear();
            m_pane_dirty = true;
        }
    }
    pane_compose();
}

bool DeepcwChannel::take_pane_op(SkimPaneOp &op) {
    if (m_ops.empty())
        return false;
    op = std::move(m_ops.front());
    m_ops.pop_front();
    return true;
}

SkimDeepcwDebug DeepcwChannel::debug() const {
    SkimDeepcwDebug dbg;
    dbg.gate       = m_gate;
    dbg.ratio_db   = m_ratio_db;
    dbg.duty       = m_duty;
    dbg.wpm        = m_wpm;
    dbg.frames_abs = m_frames_abs;
    dbg.committed  = m_committed;
    dbg.ticks      = m_ticks;
    dbg.dead       = m_dead;
    dbg.inflight   = m_inflight;
    dbg.draft_len  = static_cast<unsigned>(m_draft.size());
    dbg.pane_open  = m_pane_open;
    return dbg;
}

} // namespace

// ---- the backend --------------------------------------------------------------

SkimDeepcwBackend::SkimDeepcwBackend(const DeepcwConfig &cfg)
    : m_ctx(std::make_shared<DeepcwContext>(cfg)) {}

SkimDeepcwBackend::~SkimDeepcwBackend() = default;

std::unique_ptr<SkimDecodeChannel> SkimDeepcwBackend::channel_new(double sample_rate) const {
    return std::make_unique<DeepcwChannel>(m_ctx, sample_rate);
}

bool SkimDeepcwBackend::implements(SkimDecodeHook hook) const {
    return hook == SkimDecodeHook::Level || hook == SkimDecodeHook::ToneOffsetHz ||
           hook == SkimDecodeHook::SetFreq || hook == SkimDecodeHook::TakePaneOp ||
           hook == SkimDecodeHook::Resync || hook == SkimDecodeHook::HoldBegin ||
           hook == SkimDecodeHook::Noise || hook == SkimDecodeHook::SetBandNoise;   // gh#27
}

bool SkimDeepcwBackend::available(std::string *error) const { return m_ctx->runtime().ensure_session(error); }

const std::string &SkimDeepcwBackend::model_path() const { return m_ctx->cfg().model_path; }

std::string SkimDeepcwBackend::runtime_info() const { return m_ctx->runtime().runtime_info(); }

void SkimDeepcwBackend::set_device(const std::string &device) { m_ctx->runtime().set_device(device); }

void SkimDeepcwBackend::reset() { m_ctx->runtime().reset(); }

std::optional<SkimDeepcwDebug> SkimDeepcwBackend::debug(const SkimDecodeChannel &channel) {
    if (const auto *c = dynamic_cast<const DeepcwChannel *>(&channel))
        return c->debug();
    return std::nullopt;
}
