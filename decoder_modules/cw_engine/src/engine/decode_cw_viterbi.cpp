/*
 * decode_cw_viterbi.cpp — CW decode backends v2 and v3, one implementation.
 *
 * SDRIAK: upstream keeps v2 (decode_cw_v2.c) and v3 (decode_cw_v3.c) as two
 * near-copies — v3 began as a verbatim copy of v2 and every v3 change sits
 * behind its weak view or front end, but for three. This is upstream's v3
 * file with those three behind CwDecoderConfig::v3.robust_clock, so the v2
 * preset (front, kws and robust_clock off: decode_cw_config.h) runs exactly
 * v2's code paths and the v3 preset exactly v3's:
 *   - no lattice without a clock (lattice_start),
 *   - the stall watchdog (process),
 *   - forgetting the fist clears the carrier (process).
 * The pre-roll replay's 512-sample cap for v3's long ring applies with the
 * front end only (v2's ring is 512 samples at 250 Hz anyway). Bookkeeping
 * only v3 reads (the call reader's text history, the stall watchdog's
 * clock) runs in both.
 *
 * decode_cw_v3.c — CW decode backend v3: v2's semi-Markov Viterbi behind a
 * new observation front end (docs/CW-V3.md).
 *
 * Phase 1 (scaffolding): a verbatim copy of v2 under its own symbols and
 * switches (SKIM_V3_*, SKIM_CW3_*), so the engine plumbing, the tests and
 * the golden-v3 reference exist before the decoder changes. The phases
 * that follow replace the envelope front end (carrier detector + AFC,
 * coherent half-dit matched filter), the LLR (calibrated, Rayleigh
 * fading, band noise floor), the gate (CFAR, 6 s pre-roll) and add the
 * audio-confirmed callsign reader. v2's own notes follow.
 *
 * decode_cw_v2.c — CW decode backend v2: soft-decision semi-Markov Viterbi.
 *
 * v1's plumbing survives verbatim — |IQ| envelope (3-tap MA), dual-rate
 * level trackers, hysteresis squelch, keying-duty test, phase-slope tone
 * offset, pause/reset semantics. What v2 replaces is the decision layer:
 * where v1 runs a Schmitt trigger and classifies hard run lengths, v2 keeps
 * every sample SOFT and lets a Viterbi search pick the element sequence.
 *
 *   sample → mark/space log-likelihood ratio      (fast µ_mark/µ_space
 *            discriminator, ~0.4 s time constants  — it FOLLOWS QSB)
 *   LLR    → semi-Markov Viterbi over segments {DIT, DAH} × {ESP, CSP, WSP}
 *            with log-normal duration priors tied to the adaptive dit
 *   path   → lag-committed traceback → Morse code assembly (v1's table,
 *            lone-E/T rule, break separator)
 *
 * Why: v1's binary threshold LOSES elements in deep QSB — a faded trailing
 * dit never crosses the Schmitt threshold and "7" (--...) mutates into "G"
 * (--.), a dropout splits a dash (live-caught 9A170NT, 2026-07-15). In v2 a
 * faded dit still scores *less space-like* than its surroundings, and the
 * duration priors carry it: the element is weak evidence, not no evidence.
 *
 * The dit clock bootstraps exactly like v1 (Schmitt run clustering) — the
 * lattice needs a timing anchor before its duration priors mean anything.
 * A ~2 s pre-roll ring replays the head of an over the gate delay and the
 * bootstrap consumed (preroll_replay — the squelch attack fix).
 *
 * Part of skimmer-for-linux. GPL-3.0-or-later.
 */
#include "decode_cw.h"
#include "cw_envelope.h"
#include "cw_morse.h"
#include "cw_preroll.h"
#include "cw_reread.h"
#include "cw_rundump.h"
#include "cw_align.h"
#include "cw_callreader.h"
#include "cw_clock.h"
#include "cw_debug.h"
#include "cw_discriminator.h"
#include "cw_fist.h"
#include "cw_frontend.h"
#include "cw_lattice.h"
#include "cw_levels.h"
#include "cw_live.h"
#include "cw_schmitt.h"
#include "cw_tone.h"

#include "callsign.h"
#include "oracle.h"

#include <cmath>
#include <cstdio>
#include <cstring>

// --- debug trace (SKIM_CW3_DEBUG): cw_debug.h (SDRIAK: upstream keeps it here)

// --- Morse table: cw_morse.h (SDRIAK: upstream keeps it here) ----------------

// --- lattice geometry, segment types and duration priors: cw_lattice.h
// (SDRIAK: upstream keeps them here) -----------------------------------------

// Observation model: cw_discriminator.h (SDRIAK: upstream keeps it here).
// Fist model: cw_fist.h (SDRIAK: upstream keeps it here).

// --- the call reader: cw_callreader.h (SDRIAK: upstream keeps it here) -------

// SDRIAK: the text one process() call emits, a cursor on out->text
// (upstream threads out and the write position through every call).
class CwText {
public:
    explicit CwText(SkimDecode *out) : m_out(out) {}

    // Characters written so far.
    unsigned size() const { return m_pos; }

    // n more characters (and the terminator) fit.
    bool fits(size_t n) const { return m_pos + n < SkimDecode::kTextMax; }

    // One character; false when the buffer is full.
    bool put(char c) {
        if (m_pos + 1 >= SkimDecode::kTextMax)
            return false;
        m_out->text[m_pos++] = c;
        m_out->text[m_pos] = '\0';
        return true;
    }

    // A string, whole or not at all (upstream: emit_str).
    void put(const char *s) {
        size_t n = strlen(s);
        if (m_pos + n < SkimDecode::kTextMax) {
            memcpy(m_out->text + m_pos, s, n + 1);
            m_pos += static_cast<unsigned>(n);
        }
    }

private:
    SkimDecode *m_out;
    unsigned    m_pos = 0;
};

// SDRIAK: the per-channel state is the channel object (upstream: an opaque
// struct the backend's table functions took). Its methods are defined
// below, where upstream has those functions, bodies unchanged.
class CwState final : public SkimDecodeChannel {
public:
    CwState(const CwDecoderConfig &cfg, std::shared_ptr<const SkimCallsignDictSource> dict,
            double sample_rate);
    CwState(const CwState &) = delete;             // m_trace points into it
    CwState &operator=(const CwState &) = delete;

    bool   process(const float *iq, unsigned nframes, SkimDecode *out) override;
    double level() const override { return m_levels.env_hi(); }
    double tone_offset_hz() const override { return m_tone.offset_hz(); }
    void   set_freq(double hz) override { m_freq_hz = hz; }
    void   set_floor(double noise_pw) override { m_front.set_floor(noise_pw); }

private:
    // Commit lag, dits: boundaries older than this are stable. The full lag
    // matters in a fade — evidence that rescues a drowned element trickles in
    // late. On a healthy signal the lattice never revises that far back, so a
    // short lag cuts the text latency roughly in half (Richard, 2026-07-15:
    // "the decode trails the audio").
    static constexpr auto kCommitLagDits = 8.0;
    static constexpr auto kFastCommitLagDits = 4.0;
    static constexpr auto kOverBreakDits = 16.0;  // this much space ends the over
    static constexpr auto kOverBreakMark = "\xC2\xB7 ";  // "· " over separator (v1)
    static constexpr auto kStallSeconds = 10.0;   // a clock that commits nothing
                                                  // this long while the channel
                                                  // lives is lost (stall watchdog)

    double m_rate = 0;

    // --- v1 plumbing: envelope, trackers, squelch ----------------------------
    CwLevels m_levels;                      // SDRIAK: cw_levels.h

    // --- bootstrap Schmitt (until the dit clock is learned) ------------------
    CwBootstrap m_bootstrap;                // SDRIAK: cw_schmitt.h

    // --- element clock --------------------------------------------------------
    CwClock m_clock;                        // SDRIAK: cw_clock.h
    unsigned  m_marks_committed = 0;             // marks committed on this clock
    uint64_t m_last_mark_t = 0;                  // m_t of the last committed mark
                                                 // or of the clock's (re)start

    // --- soft discriminator ----------------------------------------------------
    CwDiscriminator m_disc;                 // SDRIAK: cw_discriminator.h
    bool m_carrier = false;                     // long solid mark — not keying

    // --- pre-roll ring (squelch attack fix, 2026-07-19) -------------------------
    // The last ~2 s of post-MA envelope samples, written on EVERY sample —
    // dead air included. When the channel wakes with a usable dit clock
    // (reopen from pause, or the moment the bootstrap learns the clock) the
    // ring is replayed through the soft path, so the head the gate delay and
    // the bootstrap consumed still decodes. pre_from fences the replayable
    // window: audio older than the last pause entry (or a watchdog reboot)
    // was already decoded or squelched — replaying it would duplicate text.
    CwPreroll m_preroll;                    // SDRIAK: cw_preroll.h

    // --- Viterbi lattice: cw_lattice.h -----------------------------------------
    uint64_t  m_t = 0;                           // absolute sample index
    CwLattice m_lattice;

    // --- character assembly -----------------------------------------------------
    CwAssembler m_chars;                   // SDRIAK: the code in flight (cw_morse.h)
    bool m_break_sent = false;                  // the over separator went out
    bool m_paused = false;
    double   m_quiet = 0;
    CwLive m_live;                          // SDRIAK: live emission, cw_live.h

    // --- fist model: cw_fist.h -------------------------------------------------
    CwFist m_fist;

    // --- SKIM_V3_NARROW + the 3-tap envelope: cw_envelope.h --------------------
    CwEnvelope m_env;                       // SDRIAK: upstream's ma/nb_* fields

    // --- the word in flight (SKIM_V3_RESCORE) ------------------------------------
    CwWordReread m_word;                    // SDRIAK: cw_reread.h
    CwKwsProbe m_probe;                     // SDRIAK: cw_callreader.h

    // --- v3 front end: cw_frontend.h -------------------------------------------
    CwFrontEnd m_front;

    // --- the call reader: cw_callreader.h --------------------------------------
    CwCallReader m_reader;

    // --- estimates --------------------------------------------------------------
    CwTone m_tone;                          // SDRIAK: cw_tone.h
    double m_snr_latch = 0;                     // SNR at the last mark commit — the
                                                // peak tracker has decayed by the
                                                // time a trailing break emits

    double   m_freq_hz = 0;                     // absolute label from the pipeline
    CwRunDump m_dump;                       // SKIM_CW_DUMP_RUNS: cw_rundump.h

    // SDRIAK: the tunables (upstream: environment variables read on first
    // use, decode_cw_config.h) and what this channel derives from them.
    CwDecoderConfig m_cfg;
    std::shared_ptr<const SkimCallsignDictSource> m_dict_source;   // the dictionary
    std::shared_ptr<const SkimCallsignDict> m_dict;   // … as of this process()
    CwTrace m_trace;                      // the debug trace (points into this)
    double m_edge_weak_ratio[2] = {};           // edge-weak on below / off above

    // The weak view: everything v3 changes acts on it alone. A station above
    // SKIM_V3_WEAK_DB keeps v2's gate, bootstrap, pre-roll, fist memory, AFC
    // and likelihoods verbatim — v2 was tuned on strong live air and stays the
    // reference there.
    bool weak_view() const { return m_front.on() && m_front.weak(); }

    // SDRIAK: upstream's static functions on the state, as methods (defined
    // below, where upstream has the functions).
    void emit(CwText &text, char c, uint64_t t_end);
    void emit_code(CwText &text, uint64_t t_end);
    void lattice_start();
    bool edge_use() const;
    CwSegmentPriors segment_priors() const;
    CwReadView read_view();
    void word_close(CwText &text, uint64_t B);
    void on_segment(CwSeg ty, double d, uint64_t start, uint64_t end, CwText &text);
    void lattice_commit(double lag_dits, CwText &text);
    void lattice_flush(CwText &text);
    bool carrier_watch();
    void live_emit(CwText &text);
    void soft_step(float soft_env, float matched_amp, CwText &text);
    bool preroll_replay(CwText &text);
    void clock_watchdog(CwText &text);
    float front_end_step(float i, float q);
    void gate_step(float env, bool o_gate, bool o_gsoft);
    struct DeadAir {
        bool dead;      // dead air by v2's own judgment
        bool solid;     // the clock is solid (the pause predicate's input)
    };
    DeadAir dead_air_now() const;
    void dead_air(bool solid, CwText &text);
    void tone_step(float i, float q, float env, float mag, bool o_freq);
    void bootstrap_step(float env, float matched_amp, CwText &text);
};

// SDRIAK: the backend is a class (upstream: a table of function pointers).
// Its channel_new() makes a CwState, whose constructor is upstream's
// channel_new() body (defined below, where upstream has the function); the
// per-channel functions are CwState's methods.
class CwBackend final : public SkimDecodeBackend {
public:
    CwBackend(const char *name, const CwDecoderConfig &cfg,
              std::shared_ptr<const SkimCallsignDictSource> dict)
        : m_name(name), m_cfg(cfg), m_dict(std::move(dict)) {}
    const char *name() const override { return m_name; }
    std::unique_ptr<SkimDecodeChannel> channel_new(double sample_rate) const override {
        return std::make_unique<CwState>(m_cfg, m_dict, sample_rate);
    }
    bool implements(SkimDecodeHook hook) const override {
        return hook == SkimDecodeHook::Level || hook == SkimDecodeHook::ToneOffsetHz ||
               hook == SkimDecodeHook::SetFreq ||
               (hook == SkimDecodeHook::SetFloor && m_cfg.v3.front.on);   // v3's front end
    }

private:
    const char     *m_name;               // "cw2", "cw3"
    CwDecoderConfig m_cfg;                // each channel takes a copy
    std::shared_ptr<const SkimCallsignDictSource> m_dict;   // … and the dictionary
};

// SDRIAK: upstream's channel_new() body, as the constructor.
CwState::CwState(const CwDecoderConfig &cfg, std::shared_ptr<const SkimCallsignDictSource> dict,
                 double sample_rate)
    : m_cfg(cfg), m_dict_source(std::move(dict)) {
    m_trace = CwTrace{&m_cfg.diag, &m_freq_hz};
    m_rate     = sample_rate > 0 ? sample_rate : 250.0;
    m_levels.init(m_rate);
    m_disc.init(m_rate);
    m_dump.init(m_cfg.diag);
    // SDRIAK: the envelope (with the narrow biquad) and the edge-weak ratios,
    // from the config at this channel's rate (upstream: statics set at first
    // use).
    m_env.init(m_rate, m_cfg.envelope);
    {
        const double db = m_cfg.lattice.edge_weak_db;
        m_edge_weak_ratio[0] = db > 0 ? pow(10.0, db / 20.0) : 0.0;
        m_edge_weak_ratio[1] = db > 0 ? pow(10.0, (db + 3.0) / 20.0) : 0.0;
    }
    // Pre-roll ring ~2 s: long enough for the bootstrap span of a slow fist
    // (4 marks of a 15 WPM "Y" ≈ 1.2 s) plus its quiet lead-in.
    // v3: CwFrontEnd::kRingSeconds of raw I/Q and of the matched statistic —
    // a candidate the detector took a second to be sure of still decodes from
    // its first mark.
    const double pre_s = m_cfg.v3.front.on ? CwFrontEnd::kRingSeconds : 2.0;
    unsigned n = 1;
    while (n < static_cast<unsigned>(pre_s * m_rate)) { n <<= 1; }
    m_preroll.init(std::min(n, 4096u), m_cfg.v3.front.on);
    if (m_cfg.v3.front.on) {
        m_front.init(m_rate, m_cfg.v3.front, m_preroll.size());
    }
}

// --- run dump: cw_rundump.h (SDRIAK: upstream keeps it here) -------------------

// --- assembly helpers (v1 semantics) ------------------------------------------

// Emit one decoded char (SDRIAK: the neural reader's pane draft is gone).
void CwState::emit(CwText &text, char c, uint64_t t_end) {
    if (!text.put(c))
        return;
    m_reader.push_char(c, c == ' ' ? m_t : t_end);
    CW_DBG(m_trace, "[emit '%c' at %.2f (t %.2f)]\n", c, static_cast<double>(t_end) / m_rate,
            static_cast<double>(m_t) / m_rate);
    if (c != ' ') { m_word.push_char(c, t_end); }
    if (m_dump.on()) {
        m_dump.emit_char(m_freq_hz, t_end, m_rate, c, m_clock.timing_error(),
                       m_clock.dit() > 0 && m_marks_committed >= 8 && m_clock.timing_error() < 0.35);
    }
}

void CwState::emit_code(CwText &text, uint64_t t_end) {
    const char c = m_chars.take(m_cfg.assembly, m_marks_committed, m_clock.timing_error());
    if (c) { emit(text, c, t_end); }
}

// --- lattice -------------------------------------------------------------------

void CwState::lattice_start() {
    m_last_mark_t = m_t;                   // the stall watchdog's zero
    // No clock, no lattice: the over break restarts the lattice for the next
    // over, and with the fist just forgotten (dit 0) that restart re-armed
    // it with every block — "break" each 8 samples, the bootstrap never
    // reached again, the channel deaf for hours (rec 2, 40 m, after a
    // runaway weak clock had tripped the carrier rule).
    if (m_cfg.v3.robust_clock && m_clock.dit() <= 0) {
        m_lattice.stop();
        m_carrier = false;
        return;
    }
    m_lattice.start(m_t);
    m_word.start(m_t);
    m_disc.seed(m_levels.env_hi(), m_levels.env_lo());
    m_live.restart();
}

bool CwState::edge_use() const {
    return m_cfg.lattice.edge || m_disc.edge_now();
}

// The clock, the fist and the edge switch as the lattice's priors read
// them (cw_lattice.h).
CwSegmentPriors CwState::segment_priors() const {
    return { m_clock.dit(), m_clock.element_space(), m_clock.timing_error(), m_fist.char_space(), m_fist.word_space(),
               m_cfg.lattice.dah_max, edge_use() };
}

// fist_learn(): CwFist::learn (SDRIAK: upstream keeps it here).

// seg_ideal(), dur_pen(), dur_pen_memo(), pen_sync(), lattice_step() and
// lattice_head(): CwSegmentPriors / CwLattice (SDRIAK: upstream keeps them here).

// What the word re-read and the call reader read of the decoder (cw_align.h).
CwReadView CwState::read_view() {
    return { &m_lattice, segment_priors(), m_t, m_rate, m_freq_hz, weak_view(), m_dict.get(), m_cfg.diag.oracle.get(), &m_trace };
}

// --- SKIM_V3_RESCORE, the word re-read: cw_reread.cpp (SDRIAK: upstream keeps
// it here) ---------------------------------------------------------------------








// SKIM_ORACLE_MODE=kws: CwKwsProbe, cw_callreader.cpp (SDRIAK: upstream keeps
// it here).

// The word in flight is done (a word gap or the over break follows; its
// evidence ends at B): re-read it, write the correction for the extractor,
// start the next word.
void CwState::word_close(CwText &text, uint64_t B) {
    SkimCall alt;
    if (m_word.close(read_view(), m_cfg.rescore, B, alt) && text.fits(alt.size() + 3)) {
        const char open[2] = { SkimCallsign::kFixOpen, '\0' };
        const char close[2] = { SkimCallsign::kFixClose, '\0' };
        text.put(open);
        text.put(alt.c_str());
        text.put(close);
    }
}

// --- clock re-lock: cw_clock.h (SDRIAK: upstream keeps it here) ---------------

// One committed segment of the best path, type ty, d samples over
// [start, end): the clock, the fist and the character assembly.
void CwState::on_segment(CwSeg ty, double d, uint64_t start, uint64_t end, CwText &text) {
    // Did the live rule already emit for the space starting here?
    const bool lived = m_live.lived(start);
    CW_DBG(m_trace, "[commit %s d=%.0f dit=%.1f err=%.2f t=%.2f csp=%.2f wsp=%.2f]\n",
            CwSegmentPriors::kSegment[seg_index(ty)].name, d, m_clock.dit(),
            m_clock.timing_error(), static_cast<double>(end) / m_rate, m_fist.char_space(), m_fist.word_space());
    switch (ty) {
    case CwSeg::Dit:
    case CwSeg::Dah: {
        m_clock.mark(ty == CwSeg::Dah, d, m_rate, m_cfg.clock,
                       20.0 * log10(std::max(m_disc.mark_level(), 1e-12) / std::max(m_disc.space_level(), 1e-12)),
                       m_trace);
        m_last_mark_t = m_t;
    }
        m_marks_committed++;
        if (m_word.empty() && m_chars.empty()) { m_word.set_start(start); }
        m_chars.push(ty == CwSeg::Dah);
        m_break_sent = false;
        m_live.clear();     // a new space will come
        m_snr_latch = m_front.on() ? m_front.snr500()
                                  : 20.0 * log10(m_levels.env_hi() / std::max(m_levels.env_lo(), 1e-9));
        break;
    case CwSeg::ElementSpace:
        m_clock.learn_element_space(d);
        m_disc.learn_smear(m_clock.dit(), d);
        break;
    case CwSeg::CharSpace:
        m_fist.learn(d / m_clock.dit(), m_trace);
        m_clock.learn_space(d);
        if (!lived) { emit_code(text, start); }
        m_live.clear();
        break;
    case CwSeg::WordSpace:
        m_fist.learn(d / m_clock.dit(), m_trace);
        if (!lived) {
            emit_code(text, start);
            word_close(text, end);
            emit(text, ' ', end);
        } else if (!m_live.word_live() && m_live.had_char()) {
            word_close(text, end);
            emit(text, ' ', end);            // live char, space grew
        }
        m_live.clear();
        break;
    }
}

// Commit the stable prefix of the best path (segments ending ≤ t − lag) and
// feed it to the character assembly. With lag 0 everything commits (flush).
// SDRIAK: the traceback is CwLattice::commit, the per-segment body
// on_segment().
void CwState::lattice_commit(double lag_dits, CwText &text) {
    m_lattice.commit(m_t, m_clock.dit(), lag_dits,
                       [&](CwSeg ty, double d, uint64_t start, uint64_t end) {
                           on_segment(ty, d, start, end, text);
                       });
}

// Flush everything decodable and stop the lattice (signal gone / carrier).
void CwState::lattice_flush(CwText &text) {
    if (!m_lattice.active())
        return;
    lattice_commit(0.0, text);
    emit_code(text, m_t);              // the char in flight
    m_lattice.stop();
}

// --- the call reader: CwCallReader, cw_callreader.cpp (SDRIAK: upstream keeps
// it here) -------------------------------------------------------------------

// --- v3 front end ---------------------------------------------------------------

// fe_fft(), fe_select(), fe_noise_refresh(), fe_track(), fe_sample(),
// fe_backfill(), fe_hop(), fe_llr(), fe_snr500(): CwFrontEnd (SDRIAK:
// upstream keeps them here).

// --- soft path: discriminator → llr ring → Viterbi ------------------------------

// A solid mark far beyond a dash is a carrier, not keying (v1 rule):
// drop the assembly and idle until the key finally opens. True: the
// sample went to the carrier (st->t moved on).
bool CwState::carrier_watch() {
    if (m_carrier) {
        if (m_disc.mark_run() == 0) {
            m_carrier = false;
            lattice_start();
        }
        m_t++;
        return true;
    }
    if (m_disc.mark_run() > static_cast<unsigned>(std::max(8.0 * m_clock.dit(), 0.8 * m_rate))) {
        // 8 dits alone is NOT carrier-proof against a stale clock: an
        // operator dropping from ~35 to ~12 WPM makes every dah ~8× the old
        // dit — the whole slow transmission read as "carrier" and, worse,
        // dahs never committed so the clock had nothing to relearn from
        // (live-caught 2026-07-16, 7028.9). A real carrier holds for
        // seconds; even a 5 WPM dah is 720 ms, so the 0.8 s floor keeps
        // SLOW keying committing — and one committed slow dah snaps the
        // timing_error-boosted clock onto the new speed.
        m_carrier = true;
        m_lattice.stop();                       // drop the code in flight
        m_chars.clear();
        m_t++;
        return true;
    }
    return false;
}

// Live emission (v1's rule, model-driven): once the best path says
// "a space has been running since commit_t" — everything before it
// is committed — the char in flight is DONE: emit it without waiting
// for a closing mark (the last char of an over never gets one).
// The word space and the over break follow the same clock.
void CwState::live_emit(CwText &text) {
    uint64_t tau;
    bool space_running;
    m_lattice.best_head(m_t, m_clock.dit(), &tau, &space_running);
    if (space_running && tau == m_lattice.committed_until()) {
        const double sp = static_cast<double>(m_t - tau);
        // The live clocks ride the fist centres (cw_live.h).
        if (m_live.char_due(sp, m_fist, m_clock.dit())) {
            m_live.fire_char(tau, !m_chars.empty());
            emit_code(text, tau);
        }
        if (m_live.word_due(sp, m_fist, m_clock.dit())) {
            m_live.fire_word();
            word_close(text, m_t);
            emit(text, ' ', tau);
        }
        if (sp > kOverBreakDits * m_clock.dit()) {         // the over is done
            CW_DBG(m_trace, "[break at t=%lu, sp=%.0f]\n", (unsigned long)m_t, sp);
            lattice_flush(text);
            word_close(text, m_t);
            if (!m_break_sent) {
                m_break_sent = true;
                text.put(kOverBreakMark);
            }
            lattice_start();
            // The next over may be the OTHER op of a QSO — the re-lock rings
            // must weigh HIS marks alone (a solid channel skips the pause on
            // a short turnaround gap, so the pause-side clear never fires).
            m_clock.clear_rings();
        }
    }
}

// One sample through the soft path: µ trackers → carrier watch → LLR →
// lattice step → lagged commit + live emission. Factored out of the live
// loop VERBATIM so the pre-roll replay pushes ring samples through the
// exact same float ops a live sample takes.
void CwState::soft_step(float soft_env, float matched_amp, CwText &text) {
    const double mid = m_disc.update(soft_env);
    if (carrier_watch())
        return;
    const double llr = weak_view() ? m_front.llr(matched_amp) : m_disc.llr(soft_env, mid, m_cfg.llr);
    const bool trace = m_cfg.diag.trace;
    if (trace) {
        fprintf(stderr, "T %lu %.4f %.4f %.4f %.2f\n",
                static_cast<unsigned long>(m_t), soft_env, m_disc.mark_level(), m_disc.space_level(), llr);
    }
    m_t++;
    m_lattice.push_llr(m_t, llr);
    m_lattice.step(m_t, segment_priors());

    if ((m_t & 7) == 0) {                      // lagged commit, every 8
        m_disc.update_edge_model(m_edge_weak_ratio);
        // Healthy signal (discriminator riding near the envelope peak, clock
        // trusted) → short lag; a fade or a re-lock keeps the full window.
        const double lag =
            (m_disc.mark_level() > 0.6 * m_levels.env_hi() && m_clock.timing_error() < 0.20)
                ? kFastCommitLagDits
                : kCommitLagDits;
        lattice_commit(lag, text);
        live_emit(text);
    }
}

// --- pre-roll replay (squelch attack fix, 2026-07-19) ---------------------------
// The dead-air gate and the dit bootstrap both consume signal from the HEAD
// of an over: the keying-duty EMA needs ~0.3 s to concede a cold channel is
// keying, and after the 8 s fist-forget the first 4-8 marks are spent
// LEARNING the clock, not decoding — "YOTA" logged as "OTA" (Y is exactly
// the 4 marks the bootstrap eats; 32 % of the R3OM reference channel's
// overs, live 2026-07-19). Every sample lands in the pre ring regardless of
// gate state, so when the channel wakes with a usable clock the ring still
// holds the head: find the over's onset (first ≥0.55-dit mark run preceded
// by ≥2 dits of quiet, inside the pre_from fence) and push the span through
// the same soft path live samples take. No onset, or quiet ≥ 14 dits
// embedded in the span (that is QRM + a real break, not one over's head) →
// no replay — exactly the old behaviour, never worse.
bool CwState::preroll_replay(CwText &text) {
    if (m_preroll.empty() || m_clock.dit() <= 0 || !m_lattice.active())
        return false;
    // v3: the head is found on the matched amplitude (its bars below)
    const float thr = weak_view()
        ? static_cast<float>(m_front.matched_lo() + 0.45 * (m_front.matched_hi() - m_front.matched_lo()))
        : static_cast<float>(m_levels.env_lo() + 0.45 * (m_levels.env_hi() - m_levels.env_lo()));
    // the strong view replays v2's window (2 s), the weak one the whole ring
    const unsigned cap = m_front.on() && !weak_view() ? 512u : UINT_MAX;   // SDRIAK: fe_on
    unsigned have, start;
    if (!m_preroll.find_head(weak_view(), cap, thr, weak_view() ? m_cfg.v3.weak.preroll_x : 5.6,
                           m_clock.dit(), m_rate, m_t, m_trace, &have, &start))
        return false;
    for (unsigned k = start; k < have; k++) {
        const unsigned at = m_preroll.head() - have + k;
        soft_step(m_preroll.env(at), m_front.on() ? m_preroll.matched(at) : 0.0f, text);
    }
    return true;
}

// --- the backend ---------------------------------------------------------------

// --- process(): the per-sample steps -------------------------------------------

void CwState::clock_watchdog(CwText &text) {
    // Clock-lost watchdog: a run of committed dahs with not one dit means the
    // dit clock sits a class too fast and the labeling went SELF-CONSISTENT —
    // slow dits read as dahs at ratio ~3, timing_error stays low, the EMA never
    // moves (a ~3× speed drop; the carrier floor alone let the marks commit
    // but not the clock recover, gate-caught 2026-07-16). No real text is
    // dah-only for 16 marks ("0 0 0" is 15), so forget the clock and let the
    // bootstrap relearn it from the very keying that is arriving.
    // The mirror trap: a clock stuck a class too SLOW reads everything as
    // dits (a wrong up-jump lands here — live-replay-caught on UB7M: 24 WPM
    // jumped to 9 and sat in "IISSESS" for 20 s). Real dit-only text runs
    // longer than real dah-only text ("5 IS HIS" happens), so the bar sits
    // higher than the dah watchdog's.
    // v3 stall watchdog: a clock that commits no mark for kStallSeconds while the
    // channel is live (not paused in dead air) reads nothing at all — the
    // one-class streaks above never fire because nothing commits. Seen on
    // replays: a clock run to its floor on a weak over sat on its channel for
    // hours, strong stations included. Relearn it from what is arriving.
    if (m_clock.dit() > 0 && m_paused) { m_last_mark_t = m_t; }
    const bool stalled = m_cfg.v3.robust_clock &&
        m_clock.dit() > 0 && m_t > m_last_mark_t + static_cast<uint64_t>(kStallSeconds * m_rate);
    // v3, weak view: noise between the marks read as key-down glues them —
    // marks grow, the intra-character spaces shrink to nothing, the clock
    // follows the marks to its floor (rec 2: dit 25 -> 75 samples in 13 s,
    // spaces 18). Real keying spaces about a dit; under 0.35 the clock is
    // reading glue, not a fist.
    const bool collapsed = m_clock.dit() > 0 && weak_view() && m_marks_committed >= 8 &&
                               m_clock.element_space() > 0 && m_clock.element_space() < 0.35 * m_clock.dit();
    if (collapsed) { m_clock.drop_element_space(); }
    if ((m_clock.dah_streak() >= 16 || m_clock.dit_streak() >= 24 || stalled || collapsed) &&
        m_clock.dit() > 0) {
        CW_DBG(m_trace, "[clock lost at t=%lu — %u dahs/%u dits one-class%s%s; rebootstrap]\n",
                (unsigned long)m_t, m_clock.dah_streak(), m_clock.dit_streak(),
                stalled ? ", stalled" : "", collapsed ? ", spaces collapsed" : "");
        lattice_flush(text);
        m_clock.forget_dit();
        m_bootstrap.forget();
        m_marks_committed = 0;
        m_clock.reset_streaks();
        m_clock.clear_rings();
        m_fist.reset();
        m_disc.reset_smear();
        m_carrier = false;
        m_paused = false;
        // The mis-clocked span WAS emitted (badly) — replaying it after the
        // rebootstrap would duplicate text. Only post-watchdog audio may.
        m_preroll.fence();
    }
}

// v3's front end on one raw frame: the I/Q ring and the detector hop, the
// matched amplitude where it is read (returned; 0 elsewhere), the weak /
// strong view between overs, the matched pre-roll ring.
float CwState::front_end_step(float i, float q) {
    float matched_amp = 0.0f;
    if (m_front.push_iq(i, q)) {
        m_front.hop(m_t, m_clock, m_preroll, m_trace);
    }
    // the matched path costs: only on a candidate, or on a weak over
    // the lattice still holds — a strong channel reads v2's view
    const bool locked = m_lattice.active() && !m_paused;
    if ((m_front.candidate() && !(locked && !weak_view())) || (weak_view() && locked)) {
        matched_amp = m_front.sample(i, q, m_clock);
        m_front.track(matched_amp);
        // the view changes between overs only: mid-over the lattice would
        // read two likelihood scales in one word
        if (!m_lattice.active() || m_paused) { m_front.update_view(m_t, m_trace); }
    }
    m_preroll.put_matched(matched_amp);
    return matched_amp;
}

// The level trackers, squelch and keying duty on one envelope sample, and
// the oracle's gate (measurement only).
void CwState::gate_step(float env, bool o_gate, bool o_gsoft) {
    // Level trackers + squelch + keying-duty — v1 verbatim.
    m_levels.update(env);
    if (o_gate) { m_levels.force_open(); }
    if (m_front.on() && (o_gate || o_gsoft) && !m_front.candidate()) {
        m_front.force_candidate(m_clock, m_preroll);
    } else if (o_gsoft) {
        m_levels.soft_open();
    }
}

// Dead air by v2's own judgment (the pause predicate below, hoisted):
// envelope gate shut with no solid discriminator to ride a dip, or no
// keying duty on a channel that never proved itself.
CwState::DeadAir CwState::dead_air_now() const {
    const bool solid =
        m_clock.dit() > 0 && m_marks_committed >= 8 && m_clock.timing_error() < 0.35;
    const bool mu_alive =
        m_lattice.active() && m_disc.mark_level() > 3.0 * m_disc.space_level();
    // v3: the detector is the gate — a candidate, or a solid lattice
    // riding out a dip; no envelope squelch, no keying-duty bar.
    // A channel that locked on a station keeps its over through word gaps
    // and fades: dead air there takes v2's over break — kOverBreakDits of no
    // evidence — not one dip of the presence log-odds (it tore every weak
    // word in two on live-air replays).
    const unsigned quiet_bar = solid ? static_cast<unsigned>(std::max(kOverBreakDits * m_clock.dit(), 1.0 * m_rate))
                                  : static_cast<unsigned>(m_cfg.v3.weak.dead_s * m_rate);
    const bool fe_quiet =
        !m_front.candidate() || m_front.absent_run() > quiet_bar ||
        (m_front.noise() > 0 && m_front.snr500() < m_cfg.v3.weak.min_db);
    const bool dead = weak_view()
        ? (fe_quiet && !(solid && mu_alive))
        : ((!m_levels.gate_open() && !(solid && mu_alive)) || (!m_levels.keying() && !solid));
    return { dead, solid };
}

// One sample of dead air: the pause (flush, over break) and, after the
// hold, the fist forgotten.
void CwState::dead_air(bool solid, CwText &text) {
    if (solid || m_bootstrap.marks()) {
        if (!m_paused) {
            CW_DBG(m_trace, "[pause t=%lu gate=%d keying=%d solid=%d marks=%u err=%.2f "
                    "hi=%.3f lo=%.3f]\n",
                    (unsigned long)m_t, m_levels.gate_open(), m_levels.keying(), solid,
                    m_marks_committed, m_clock.timing_error(), m_levels.env_hi(), m_levels.env_lo());
            lattice_flush(text);
            if (m_clock.dit() > 0 && !m_break_sent) {
                m_break_sent = true;
                text.put(kOverBreakMark);
            }
            m_chars.clear();
            m_clock.clear_rings();
            m_paused = true;
            m_quiet  = 0.0;
            // Everything before this pause was decoded or squelched — the
            // pre-roll replay may only reach back to here.
            m_preroll.fence();
        }
        m_quiet += 1.0;
        if (m_quiet > (weak_view() ? m_cfg.v3.weak.fist_hold_s : 8.0) * m_rate) { // forget the fist
            m_clock.forget_dit();
            if (m_cfg.v3.robust_clock) { m_carrier = false; }  // v3: no lattice without a clock
            m_bootstrap.forget();
            m_marks_committed = 0;
            m_clock.clear_rings();
            m_paused = false;
            m_fist.reset();                    // spacing style goes with it
            m_disc.reset_smear();
        }
    }
    m_bootstrap.idle();
    m_t++;
}

// Tone offset from the phase slope, SOLID mark samples only — the 0.55
// bar matches v1's Schmitt ON level. A softer bar (the discriminator
// midpoint) let borderline samples in, and their noisy phase slope
// dragged the estimate toward the channel centre — the reported
// frequency kept sliding with signal strength (live 2026-07-15).
void CwState::tone_step(float i, float q, float env, float mag, bool o_freq) {
    const double mid_now =
        m_lattice.active() || m_carrier
            ? m_disc.space_level() + 0.55 * (m_disc.mark_level() - m_disc.space_level())
            : m_levels.env_lo() + 0.55 * (m_levels.env_hi() - m_levels.env_lo());
    m_tone.update(i, q, !o_freq && !weak_view() && env > mid_now && mag > m_levels.env_lo() * 3.0,
                    m_rate);
    if (weak_view() && !o_freq) { m_tone.set_offset_hz(m_front.carrier_hz()); }   // the detector's AFC
}

// Bootstrap: v1's Schmitt + first-marks clustering learns the dit.
void CwState::bootstrap_step(float env, float matched_amp, CwText &text) {
    const double b_lo = weak_view() ? m_front.matched_lo() : m_levels.env_lo();
    const double b_hi = weak_view() ? m_front.matched_hi() : m_levels.env_hi();
    CwSchmitt::Run run;
    if (m_bootstrap.step(weak_view() ? matched_amp : env, b_lo, b_hi, run)) {
        if (CwBootstrap::mark_ready(run)) {
            m_clock.set_dit(m_bootstrap.learn(run.len, m_rate, m_clock.dit()));
            if (m_clock.dit() > 0) {
                CW_DBG(m_trace, "[boot dit %.1f from %u marks]\n", m_clock.dit(), m_bootstrap.marks());
                lattice_start();                 // hand over to the Viterbi
                // The bootstrap consumed the over's head to LEARN the clock —
                // the ring still holds that audio; decode it now.
                if (preroll_replay(text)) {
                    m_bootstrap.settle();
                    return;                          // replay ran through NOW
                }
            }
        }
        m_bootstrap.settle();
    }
    m_t++;
}

bool CwState::process(const float *iq, unsigned nframes, SkimDecode *out) {
    CwText text(out);
    m_dict = m_dict_source ? m_dict_source->snapshot() : nullptr;
    out->text[0]      = '\0';
    out->pane_own     = false;

    // Measurement only (oracle.h, SKIM_ORACLE): where R heard a station on
    // this channel, hold the gate open / take R's clock / take the measured
    // carrier. Off (no oracle) nothing below changes.
    const SkimOracle *orc = m_cfg.diag.oracle.get();
    const unsigned om = orc ? orc->modes() : 0;
    const std::optional<SkimOracleHit> oh =
        om ? orc->at(m_freq_hz, 0.25 * m_rate, orc->time(m_preroll.head() / m_rate)) : std::nullopt;
    const bool ohit = oh.has_value();
    const bool o_gate  = ohit && (om & SkimOracle::kGate);
    const bool o_gsoft = ohit && (om & SkimOracle::kSoftGate);
    const bool o_clock = ohit && (om & SkimOracle::kClock);
    const bool o_freq  = ohit && (om & SkimOracle::kFreq);
    const double   o_dit   = o_clock ? 1.2 * m_rate / oh->wpm : 0.0;
    if (o_freq) { m_tone.set_offset_hz(oh->carrier_hz - m_freq_hz); }
    if (o_clock && m_clock.dit() > 0) {
        const double ub = m_clock.unbiased_dit();
        if (fabs(ub / o_dit - 1.0) > 0.25) { m_clock.impose(o_dit); }
    }

    clock_watchdog(text);

    // SKIM_V3_NARROW=<fc Hz> (default 45 since v0.6.0, "0" = off: two
    // recordings of live air, recall +1.3 and +2.1, busts never up; below
    // 10 dB +5 points): the channel is 125 Hz wide whatever the speed;
    // a 25-35 WPM signal needs about a third of that. Derotate by the tracked
    // tone offset and low-pass (2nd-order Butterworth, fc) before the
    // envelope — less noise, less of the neighbours' splatter. The offset
    // estimate below keeps reading the raw samples.
    // SDRIAK: the envelope and its narrow path are the CwEnvelope stage.

    for (unsigned n = 0; n < nframes; n++) {
        const float i = iq[2 * n], q = iq[2 * n + 1];
        const CwEnvelope::Sample es = m_env.push(i, q, m_tone.offset_hz());
        const float mag = es.mag;
        float env = es.raw;
        // The soft path's envelope: v3's matched amplitude on a candidate (or
        // a lattice riding out a dip), v2's narrow one with SKIM_V3_FRONT=v2.
        // Squelch, keying duty and v2's bootstrap keep reading the raw channel.
        const float soft_env = es.soft;
        float matched_amp = 0.0f;                  // v3's matched amplitude
        if (m_front.on()) { matched_amp = front_end_step(i, q); }
        m_preroll.push(soft_env);
        if ((om & SkimOracle::kKws)) { m_probe.push(read_view(), m_preroll.head(), soft_env, m_cfg.diag.kws_dump); }

        gate_step(env, o_gate, o_gsoft);

        const auto [dead, solid] = dead_air_now();

        // Run keyer: sees EVERY sample (before gate/pause early-exits), so the
        // corpus/reader keeps what the decode path may throw away.
        if (m_dump.on()) {                   // SDRIAK: no neural reader
            m_dump.step(env, m_levels.env_lo(), m_levels.env_hi(), m_freq_hz, m_t, m_rate);
        }

        // Deep QSB pulls faded marks under the envelope midpoint, feeds them
        // to env_lo and slams the envelope gate shut mid-callsign (v1 tears
        // "9A170NT" apart exactly this way). The µ discriminator knows better:
        // in a trough µ_m/µ_s stays ≫ the 1.5 floor it collapses to on noise —
        // a SOLID channel with a live discriminator rides the dip out.
        if (dead) {
            dead_air(solid, text);
            continue;
        }
        if (m_paused) {
            m_paused = false;                      // signal is back
            m_carrier = false;
            if (m_clock.dit() > 0) {
                lattice_start();
                // The wake lagged the true onset (in a fade re-entry by a lot) —
                // replay it from the ring; the current sample rides along.
                if (preroll_replay(text)) { continue; }
            }
        }

        tone_step(i, q, env, mag, o_freq);

        if (o_clock && !m_lattice.active() && !m_carrier) {
            m_clock.impose(o_dit);                         // R's clock, no bootstrap
            lattice_start();
            if (preroll_replay(text)) { continue; }
        }
        if (!m_lattice.active() && !m_carrier) {
            bootstrap_step(env, matched_amp, text);
            continue;
        }

        soft_step(soft_env, matched_amp, text);
        if (m_front.on() && m_lattice.active() && !m_paused && m_clock.dit() > 0 && m_cfg.v3.reader.on &&
            m_reader.due(m_t, m_rate)) {
            m_reader.read(read_view(), m_cfg.v3.reader);
        }
        if (m_reader.fix()[0] && m_word.empty() && text.fits(strlen(m_reader.fix()) + 1)) {
            text.put(m_reader.fix());
            m_reader.fix_sent();
        }
    }

    if (text.size() == 0)
        return false;

    out->confidence     = std::clamp(1.0 - 1.5 * m_clock.timing_error(), 0.0, 1.0);
    out->freq_offset_hz = m_tone.offset_hz();
    const double dit_ub = m_clock.unbiased_dit();
    out->speed          = m_clock.dit() > 0 ? 1.2 * m_rate / dit_ub : 0.0;
    out->snr_db         = m_snr_latch != 0.0
                              ? m_snr_latch
                              : 20.0 * log10(m_levels.env_hi() / std::max(m_levels.env_lo(), 1e-9));
    return true;
}

// SDRIAK: a backend on a configuration and a dictionary of one's own
// (upstream: process-wide singletons on its SKIM_* environment switches and
// the global dictionary — cw_config_from_env() gives the former).
std::unique_ptr<SkimDecodeBackend> skim_decode_cw_v2_new(const CwDecoderConfig &cfg,
                                                         std::shared_ptr<const SkimCallsignDictSource> dict) {
    return std::make_unique<CwBackend>("cw2", cfg, std::move(dict));
}

std::unique_ptr<SkimDecodeBackend> skim_decode_cw_v3_new(const CwDecoderConfig &cfg,
                                                         std::shared_ptr<const SkimCallsignDictSource> dict) {
    return std::make_unique<CwBackend>("cw3", cfg, std::move(dict));
}
